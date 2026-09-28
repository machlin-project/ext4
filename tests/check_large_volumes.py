#!/usr/bin/env python3
"""Mutate and independently recover sparse volumes without reading their holes."""

import argparse
import json
from pathlib import Path
import re
import subprocess
import time

from check_large_files import mapped_blocks, span_blocks
from check_namespace import inode_fields
from check_orphans import accounting
from check_rename import entries
from generate_fixtures import resolve_tools
from sparse_image import sparse_copy, sparse_digest

SECTOR_BYTES = 512
SECONDS = 1700000500


def expected_spans(fixture, seed):
    cluster = fixture["block_size"] * fixture["cluster_blocks"]
    return [(2 * index * cluster + 7, bytes([(ord("A") if seed else ord("K")) + index]) * 32)
            for index in range(0 if seed else 1, 5)]


def file_state(image, name, fixture, state, tools, run, output):
    path = f"/upper/{name}"
    raw = run([tools["debugfs"], "-R", f"stat {path}", image])
    inode = inode_fields(raw)
    block, ratio = fixture["block_size"], fixture["cluster_blocks"]
    cluster = block * ratio
    spans = expected_spans(fixture, name == "seed")
    size = 8 * cluster + 39
    reserved = set()
    value = bytes(0x90 + index % 23 for index in range(300)) if name == "seed" else b"y" * 300
    if name == "created":
        if state == "committed":
            spans, size, value = [], 0, b"x" * 300
        else:
            spans.append((2 * cluster + block + 13, b"Z" * 17))
            reserved = {12 * ratio + 1}
        if (inode["mode"], inode["uid"], inode["gid"], inode["generation"]) != (0o640, 70000, 80000, 1):
            raise RuntimeError("High-address creation changed admitted attributes")
        if any(inode[key] != (SECONDS, 0) for key in ("atime", "mtime", "ctime")):
            raise RuntimeError("High-address file timestamp differs")
    if inode["type"] != "regular" or inode["size"] != size or inode["links"] != 1:
        raise RuntimeError("High-address regular file identity or size differs")
    data, metadata = mapped_blocks(image, path, inode, raw, tools, run)
    wanted = span_blocks(spans, block) | reserved
    if set(data) != wanted or {logical for logical, (_, unwritten) in data.items() if unwritten} != reserved:
        raise RuntimeError("High-address initialized/unwritten maps differ")
    acl = re.search(r"File ACL:\s+(\d+)", raw)
    if acl is None or int(acl[1]) < fixture["first_high_block"]:
        raise RuntimeError("External attribute lost its high physical address")
    data_clusters = {physical // ratio for physical, _ in data.values()}
    nodes = {physical // ratio for physical in metadata}
    attr_cluster = int(acl[1]) // ratio
    if data_clusters & nodes or attr_cluster in data_clusters | nodes or len(nodes) != len(metadata):
        raise RuntimeError("High-address data/metadata ownership aliases")
    if inode["blocks"] != (len(data_clusters) + len(nodes) + 1) * cluster // SECTOR_BYTES:
        raise RuntimeError("High-address inode sector charge differs")
    if any(physical < fixture["first_high_block"] for physical, _ in data.values()) or any(
            physical < fixture["first_high_block"] for physical in metadata):
        raise RuntimeError("An operation escaped to a lower physical block group")
    with image.open("rb") as stream:
        for logical, (physical, unwritten) in data.items():
            if unwritten:
                continue
            expected = bytearray(block)
            for offset, content in spans:
                first, end = max(offset, logical * block), min(offset + len(content), (logical + 1) * block)
                if end > first:
                    expected[first - logical * block:end - logical * block] = content[first - offset:end - offset]
            stream.seek(physical * block)
            if stream.read(block) != expected:
                raise RuntimeError("High-address block data or zero padding differs")
    attr = output / f"{name}.xattr"
    run([tools["debugfs"], "-R", f'ea_get -r -f "{attr}" {path} user.large', image])
    if attr.read_bytes() != value:
        raise RuntimeError("High-address external attribute bytes differ")
    location = run([tools["debugfs"], "-R", f"imap {path}", image])
    inode_block = re.search(r"located at block (\d+), offset", location)
    if inode_block is None or int(inode_block[1]) < fixture["first_high_block"]:
        raise RuntimeError("Independent inode-table address is not in the last group")
    return dict(inode=inode, data=data, metadata=sorted(metadata), attribute=int(acl[1]),
                value=value.hex(), inode_block=int(inode_block[1]))


def snapshot(image, fixture, state, tools, run, output):
    output.mkdir()
    before = sparse_digest(image)
    run([tools["e2fsck"], "-fn", image])
    header = run([tools["dumpe2fs"], "-h", image])
    counts = accounting(header)
    if counts["Block count"] != fixture["blocks"]:
        raise RuntimeError("Independent large-volume block count differs")
    result = dict(accounting=counts, files={}, root=None, directory=None, names=None)
    for key, path in (("root", "/"), ("directory", "/upper")):
        result[key] = inode_fields(run([tools["debugfs"], "-R", f"stat {path}", image]))
    names = entries(run([tools["debugfs"], "-R", "ls -p /upper", image]))
    expected = {".", "..", "seed"} | ({"created"} if state in ("mutated", "committed") else set())
    if set(names) != expected or names["."] != fixture["directory_inode"] or names["seed"] != fixture["seed_inode"]:
        raise RuntimeError("High-address directory names or inode identities differ")
    result["names"] = names
    for name in sorted(expected - {".", ".."}):
        result["files"][name] = file_state(image, name, fixture, state, tools, run, output)
        if result["files"][name]["inode"]["inode"] != names[name]:
            raise RuntimeError("High-address directory references a different inode")
    if sparse_digest(image) != before:
        raise RuntimeError("Read-only large-volume verification changed image contents")
    (output / "state.json").write_text(json.dumps(result, indent=2) + "\n")
    return result


def compare_accounting(baseline, result):
    if baseline["files"]["seed"] != result["files"]["seed"] or baseline["root"] != result["root"]:
        raise RuntimeError("Large-volume mutation changed unrelated seed or root metadata")
    expected = dict(baseline["accounting"])
    old = sum(row["inode"]["blocks"] for row in baseline["files"].values()) + baseline["directory"]["blocks"]
    new = sum(row["inode"]["blocks"] for row in result["files"].values()) + result["directory"]["blocks"]
    charged = (new - old) * SECTOR_BYTES
    if charged % expected["Block size"]:
        raise RuntimeError("Large-volume sector charge is not block-aligned")
    expected["Free blocks"] -= charged // expected["Block size"]
    expected["Free inodes"] -= len(result["files"]) - len(baseline["files"])
    if result["accounting"] != expected:
        raise RuntimeError(f"Large-volume allocation differs: actual={result['accounting']}, expected={expected}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--fixtures", required=True, type=Path)
    parser.add_argument("--tools-root", required=True, type=Path)
    parser.add_argument("--test", required=True, type=Path)
    parser.add_argument("--recover", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--profile", action="append")
    args = parser.parse_args()
    fixtures = json.loads(args.fixtures.read_text())
    if len(fixtures) != 3 or not all(row.get("passed") for row in fixtures):
        raise RuntimeError("Expected the three verified sparse volume profiles")
    if args.profile:
        if set(args.profile) - {row["profile"] for row in fixtures}:
            raise RuntimeError("Unknown required volume profile")
        fixtures = [row for row in fixtures if row["profile"] in args.profile]
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    tools = resolve_tools(args.tools_root)
    reports = []
    for fixture in fixtures:
        source = Path(fixture["image"])
        if sparse_digest(source) != fixture["sparse_sha256"]:
            raise RuntimeError("Protected sparse fixture changed")
        row = dict(profile=fixture["profile"], source=str(source), commands=[], states=[])
        reports.append(row)
        directory = output / fixture["profile"]
        directory.mkdir()

        def save():
            (output / "report.json").write_text(json.dumps(reports, indent=2) + "\n")

        def run(command):
            command = [str(x) for x in command]
            started = time.monotonic()
            done = subprocess.run(command, capture_output=True, text=True, timeout=300)
            row["commands"].append(dict(command=command, status=done.returncode,
                                         seconds=time.monotonic() - started, stdout=done.stdout, stderr=done.stderr))
            save()
            if done.returncode:
                raise RuntimeError(f"Large-volume operation failed: {command}: {done.stderr}")
            return done.stdout

        def inspect(image, phase, label, baseline):
            state = snapshot(image, fixture, phase, tools, run, directory / label)
            if baseline is not None:
                compare_accounting(baseline, state)
            row["states"].append(dict(label=label, image=str(image), sparse_sha256=sparse_digest(image), state=state))
            save()
            return state

        try:
            run([args.test.resolve(), "--read", source])
            baseline = inspect(source, "original", "original", None)
            mutated = directory / "mutated.img"
            sparse_copy(source, mutated)
            run([args.test.resolve(), "--mutate", mutated])
            inspect(mutated, "mutated", "mutated", baseline)
            reclaimed = directory / "reclaimed.img"
            sparse_copy(mutated, reclaimed)
            run([args.test.resolve(), "--reclaim", reclaimed])
            inspect(reclaimed, "reclaimed", "reclaimed", baseline)
            for cut in ("before", "after"):
                pending = directory / f"{cut}-pending.img"
                sparse_copy(source, pending)
                run([args.test.resolve(), f"--{cut}-commit", pending])
                pending_hash = sparse_digest(pending)
                states = []
                for replay in ("core", "oracle"):
                    image = directory / f"{cut}-{replay}.img"
                    sparse_copy(pending, image)
                    if replay == "core":
                        text = run([args.recover.resolve(), "--write", image])
                        transactions = re.search(r"transactions=(\d+)", text)
                        if transactions is None or int(transactions[1]) != int(cut == "after"):
                            raise RuntimeError("Unexpected high-address replay transaction count")
                    else:
                        run([tools["debugfs"], "-w", "-R", "journal_run", image])
                    state = inspect(image, "committed" if cut == "after" else "original", f"{cut}-{replay}", baseline)
                    states.append(state)
                    if replay == "core":
                        clean = sparse_digest(image)
                        run([args.recover.resolve(), "--write", image])
                        if sparse_digest(image) != clean:
                            raise RuntimeError("Repeated high-address recovery changed a clean image")
                if states[0] != states[1] or sparse_digest(pending) != pending_hash:
                    raise RuntimeError("High-address core and independent journal replay differ")
            if sparse_digest(source) != fixture["sparse_sha256"]:
                raise RuntimeError("Large-volume run changed its original input")
            row["passed"] = True
            print(f"PASS {fixture['profile']}: high addresses, allocation, reclamation and independent replay", flush=True)
        except Exception as error:
            row.update(passed=False, error=str(error))
            raise
        finally:
            save()


if __name__ == "__main__":
    main()

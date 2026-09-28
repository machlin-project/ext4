#!/usr/bin/env python3
"""Verify every allocated sparse-file block without materializing logical holes."""

import argparse
import json
from pathlib import Path
import re
import subprocess

from check_namespace import INODE_EXTENTS, inode_fields
from check_orphans import accounting, digest
from generate_fixtures import resolve_tools

STATES = ("written", "ranged", "regrown", "reclaimed")
SECONDS = 1700000400
MAPPING_LIMIT = 128
SECTOR_BYTES = 512


def mapped_blocks(image, path, inode, raw, tools, run):
    data = {}
    metadata = set()

    def add(first, last, physical, unwritten=False):
        if last < first or last - first + 1 > MAPPING_LIMIT or len(data) + last - first + 1 > MAPPING_LIMIT:
            raise RuntimeError("Unexpected unbounded independent sparse mapping")
        for logical in range(first, last + 1):
            if logical in data:
                raise RuntimeError("Duplicate independent logical mapping")
            data[logical] = (physical + logical - first, unwritten)

    if inode["flags"] & INODE_EXTENTS:
        listing = run([tools["debugfs"], "-R", f"dump_extents {path}", image])
        pattern = (r"\s*(\d+)/\s*(\d+)\s+\d+/\s*\d+\s+(\d+)\s+-\s+(\d+)\s+"
                   r"(\d+)(?:\s+-\s+(\d+))?\s+(\d+)\s*(Uninit)?\s*")
        for line in listing.splitlines():
            if not line.strip() or line.startswith("Level Entries"):
                continue
            match = re.fullmatch(pattern, line)
            if not match:
                raise RuntimeError(f"Unrecognized independent extent record: {line!r}")
            level, depth, first, last, physical = map(int, match.groups()[:5])
            if level == depth:
                if match[6] is None or int(match[6]) - physical != last - first or int(match[7]) != last - first + 1:
                    raise RuntimeError("Inconsistent independent extent geometry")
                add(first, last, physical, bool(match[8]))
            elif level < depth and match[6] is None and not match[8]:
                if physical in metadata:
                    raise RuntimeError("Duplicate independent mapping-node reference")
                metadata.add(physical)
            else:
                raise RuntimeError("Invalid independent mapping depth")
    else:
        listing = raw.split("BLOCKS:\n", 1)[1].split("TOTAL:", 1)[0].strip()
        for token in listing.split(",") if listing else ():
            match = re.fullmatch(r"\s*\((\d+)(?:-(\d+))?\):(\d+)(?:-(\d+))?\s*", token)
            node = re.fullmatch(r"\s*\((?:IND|DIND|TIND)\):(\d+)\s*", token)
            if match:
                first, last = int(match[1]), int(match[2] or match[1])
                physical, end = int(match[3]), int(match[4] or match[3])
                if end - physical != last - first:
                    raise RuntimeError("Inconsistent independent indirect geometry")
                add(first, last, physical)
            elif node and int(node[1]) not in metadata:
                metadata.add(int(node[1]))
            else:
                raise RuntimeError(f"Unrecognized independent indirect record: {token!r}")
    return data, metadata


def verify_file(image, path, fixture, spans, size, wanted_blocks, tools, run):
    raw = run([tools["debugfs"], "-R", f"stat {path}", image])
    inode = inode_fields(raw)
    acl = re.search(r"File ACL:\s+(\d+)", raw)
    if inode["type"] != "regular" or inode["size"] != size or inode["links"] != 1 or acl is None or int(acl[1]) != 0:
        raise RuntimeError(f"Incorrect independent sparse-file metadata: {path}: {inode}")
    data, metadata = mapped_blocks(image, path, inode, raw, tools, run)
    if set(data) != wanted_blocks:
        raise RuntimeError(f"Incorrect {path} mapped blocks: actual={sorted(data)}, expected={sorted(wanted_blocks)}")
    block, ratio = fixture["block_size"], fixture["cluster_blocks"]
    clusters = {physical // ratio for physical, _ in data.values()}
    nodes = {physical // ratio for physical in metadata}
    if clusters & nodes or len(nodes) != len(metadata):
        raise RuntimeError("Independent data/metadata cluster alias")
    if inode["blocks"] != (len(clusters) + len(nodes)) * ratio * block // SECTOR_BYTES:
        raise RuntimeError("Independent inode allocation charge differs from its mapping")
    with image.open("rb") as stream:
        for logical, (physical, unwritten) in data.items():
            expected = bytearray(block)
            for offset, value in spans:
                start, end = max(offset, logical * block), min(offset + len(value), (logical + 1) * block, size)
                if end > start:
                    expected[start - logical * block:end - logical * block] = value[start - offset:end - offset]
            if unwritten:
                if any(expected):
                    raise RuntimeError("Written bytes remain independently marked unwritten")
                continue
            stream.seek(physical * block)
            if stream.read(block) != expected:
                raise RuntimeError(f"Independent sparse data/tail mismatch: {path} logical block {logical}")
    return inode


def span_blocks(spans, block):
    return {logical for offset, value in spans
            for logical in range(offset // block, (offset + len(value) - 1) // block + 1)}


def expected_created(fixture, state):
    block, limit = fixture["block_size"], fixture["limit"]
    spans = [(row["offset"], bytes.fromhex(row["data"])) for row in fixture["segments"]]
    spans[0] = (0, b"\x71" * 61)
    allocated = span_blocks(spans, block)
    if state != "written":
        spans[1] = (spans[1][0], bytes(len(spans[1][1])))
        if "indirect" not in fixture["profile"]:
            spans.append((limit - 3 * block + 9, b"R"))
            allocated.update(range(limit // block - 4, limit // block - 2))
    if state in ("regrown", "reclaimed"):
        truncated = (1 << 32) + 1
        spans = [(offset, value[:truncated - offset]) for offset, value in spans if offset < truncated]
        allocated = {logical for logical in allocated if logical * block < truncated}
    if state == "reclaimed":
        return [], 0, set()
    return spans, limit, allocated


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--fixtures", required=True, type=Path)
    parser.add_argument("--exports", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--tools-root", type=Path)
    args = parser.parse_args()
    fixtures = json.loads(args.fixtures.read_text())
    if len(fixtures) != 9 or not all(row.get("passed") for row in fixtures):
        raise RuntimeError("Missing verified nine-profile large-file fixtures")
    tools = resolve_tools(args.tools_root)
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    reports = []
    for fixture in fixtures:
        source = Path(fixture["image"])
        if digest(source) != fixture["image_sha256"]:
            raise RuntimeError("Large-file fixture changed")
        seed_spans = [(row["offset"], bytes.fromhex(row["data"])) for row in fixture["segments"]]
        seed_blocks = span_blocks(seed_spans, fixture["block_size"])
        baseline = None
        for state in STATES:
            image = args.exports.resolve() / f"large-{state}-{source.name}"
            row = dict(image=str(image), source=str(source), profile=fixture["profile"], state=state, commands=[])
            reports.append(row)

            def run(command):
                done = subprocess.run([str(x) for x in command], capture_output=True, text=True,
                                      errors="backslashreplace", timeout=120)
                row["commands"].append(dict(command=[str(x) for x in command], status=done.returncode,
                                             stdout=done.stdout, stderr=done.stderr))
                if done.returncode != 0:
                    raise RuntimeError(f"Independent sparse-file command failed: {command}: {done.stderr}")
                return done.stdout

            def summary(candidate):
                run([tools["e2fsck"], "-fn", candidate])
                return accounting(run([tools["dumpe2fs"], "-h", candidate]))

            try:
                before_hash = digest(image)
                if baseline is None:
                    baseline = summary(source)
                    original = verify_file(source, "/seed", fixture, seed_spans, fixture["limit"], seed_blocks, tools, run)
                    original_root = inode_fields(run([tools["debugfs"], "-R", "stat /", source]))
                counts = summary(image)
                unchanged = verify_file(image, "/seed", fixture, seed_spans, fixture["limit"], seed_blocks, tools, run)
                if unchanged != original:
                    raise RuntimeError("Independent seed inode changed during another file's mutation")
                spans, size, allocated = expected_created(fixture, state)
                inode = verify_file(image, "/created", fixture, spans, size, allocated, tools, run)
                if (inode["uid"], inode["gid"], inode["mode"], inode["generation"]) != (70000, 80000, 0o640, 1):
                    raise RuntimeError("Created sparse-file ownership/identity differs")
                if any(inode[field] != (SECONDS, 0) for field in ("atime", "mtime", "ctime")):
                    raise RuntimeError("Created sparse-file timestamps differ")
                root = inode_fields(run([tools["debugfs"], "-R", "stat /", image]))
                charged = (inode["blocks"] + root["blocks"] - original_root["blocks"]) * SECTOR_BYTES
                expected = dict(baseline)
                expected["Free blocks"] -= charged // fixture["block_size"]
                expected["Free inodes"] -= 1
                if charged % fixture["block_size"] or counts != expected:
                    raise RuntimeError(f"Large-file accounting differs: expected={expected}, actual={counts}")
                if digest(source) != fixture["image_sha256"] or digest(image) != before_hash:
                    raise RuntimeError("Independent read-only verification changed media")
                row.update(passed=True, source_sha256=fixture["image_sha256"], image_sha256=before_hash,
                           allocation=counts, logical_size=size, mapped_blocks=len(allocated))
                print(f"PASS {image.name}: all mapped bytes, exact sparse ranges, accounting and strict fsck", flush=True)
            except Exception as error:
                row.update(passed=False, error=str(error))
                raise
            finally:
                (output / "report.json").write_text(json.dumps(reports, indent=2) + "\n")


if __name__ == "__main__":
    main()

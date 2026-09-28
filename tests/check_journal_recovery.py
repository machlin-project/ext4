#!/usr/bin/env python3
"""Check debugfs-generated transactions with our replayer and independent e2fsck."""

import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil
import struct
import subprocess

from generate_fixtures import resolve_tools

JBD2_MAGIC = 0xC03B3998
JBD2_COMMIT = 2
JBD2_COMMIT_PREFIX = struct.Struct(">IIIBB2x")


def digest(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    inputs = parser.add_mutually_exclusive_group(required=True)
    inputs.add_argument("--fixtures", type=Path)
    inputs.add_argument("--writers", type=Path, help="pending transactions exported by ext4-journal-test")
    parser.add_argument("--recover", required=True, type=Path)
    parser.add_argument("--e2fsck", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--discard-commit", action="store_true",
                        help="damage the sole async writer commit and require both replayers to discard it")
    parser.add_argument("--tools-root", type=Path, help="e2fsprogs build used to locate the commit")
    args = parser.parse_args()
    if args.discard_commit and not args.writers:
        parser.error("--discard-commit requires --writers")
    tools = resolve_tools(args.tools_root) if args.discard_commit else None
    output = args.output.resolve()
    if output.exists():
        parser.error("output must be a new directory")
    output.mkdir(parents=True)
    fixtures = (args.fixtures or args.writers).resolve()
    if args.fixtures:
        manifest = json.loads((fixtures / "manifest.json").read_text())
        cases = manifest["cases"]
        for case in cases:
            case["block_size"] = manifest["block_size"]
            case["expected_bytes"] = [(fixtures / path).read_bytes() for path in case["expected"]]
    else:
        cases = []
        for path in sorted(fixtures.glob("writer-*.json")):
            case = json.loads(path.read_text())
            block_size = case["block_size"]
            expected = [bytearray([0x53]) * block_size, bytes([0xA7]) * block_size]
            expected[0][:4] = bytes.fromhex("c03b3998")
            case.update(name=path.stem, image=path.with_suffix(".img").name,
                        image_sha256=digest(path.with_suffix(".img")),
                        transactions=1, expected_bytes=expected)
            cases.append(case)
    if not cases:
        raise RuntimeError("no journal cases")
    results = []
    for case in cases:
        source = fixtures / case["image"]
        if digest(source) != case["image_sha256"]:
            raise RuntimeError(f"fixture changed: {source}")
        image = output / source.name
        shutil.copyfile(source, image)
        result = {"name": case["name"], "input_sha256": case["image_sha256"], "commands": []}
        results.append(result)

        def run(command):
            done = subprocess.run([str(x) for x in command], capture_output=True, text=True)
            result["commands"].append({"command": [str(x) for x in command],
                                       "status": done.returncode, "stdout": done.stdout,
                                       "stderr": done.stderr})
            (output / "report.json").write_text(json.dumps(results, indent=2) + "\n")
            done.check_returncode()
            return done.stdout

        if args.discard_commit:
            if not case.get("async_commit"):
                raise RuntimeError("discard-commit requires an async writer profile")
            header = run([tools["dumpe2fs"], "-h", image])
            features = re.search(r"^Journal features:\s+(.+)$", header, re.M)
            journal = re.search(r"^Journal inode:\s+(\d+)$", header, re.M)
            if not journal or not features or "journal_async_commit" not in features[1].split():
                raise RuntimeError("independent header did not identify an internal async journal")
            log = run([tools["debugfs"], "-R", "logdump -a", image])
            commits = re.findall(rf"^Found expected sequence (\d+), type {JBD2_COMMIT} \(commit block\) at block (\d+)$", log, re.M)
            if len(commits) != 1:
                raise RuntimeError("commit damage requires one independently decoded transaction")
            mapped = run([tools["debugfs"], "-R", f"bmap <{journal[1]}> {commits[0][1]}", image]).strip()
            if not mapped.isdecimal():
                raise RuntimeError("independent journal block lookup failed")
            offset = int(mapped) * case["block_size"]
            if offset + case["block_size"] > image.stat().st_size:
                raise RuntimeError("journal commit is out of bounds")
            with image.open("r+b") as file:
                file.seek(offset)
                magic, kind, sequence, _, _ = JBD2_COMMIT_PREFIX.unpack(file.read(JBD2_COMMIT_PREFIX.size))
                if (magic, kind, sequence) != (JBD2_MAGIC, JBD2_COMMIT, int(commits[0][0])):
                    raise RuntimeError("independent commit location does not match the record")
                checksum_byte = file.read(1)
                file.seek(offset + JBD2_COMMIT_PREFIX.size)
                file.write(bytes([checksum_byte[0] ^ 1]))
                case["expected_bytes"] = []
                for block in case["targets"]:
                    file.seek(block * case["block_size"])
                    case["expected_bytes"].append(file.read(case["block_size"]))
            case["transactions"] = 0
            result.update(discarded_commit=True, damaged_checksum_offset=offset + JBD2_COMMIT_PREFIX.size)
        oracle = output / f"oracle-{source.name}"
        shutil.copyfile(image, oracle)
        result["replay_input_sha256"] = digest(image)
        log = run([args.recover.resolve(), "--write", image])
        match = re.search(r"transactions=(\d+)", log)
        if not match or int(match[1]) != case["transactions"]:
            raise RuntimeError(f"incorrect recovered transaction count: {case['name']}")
        def check_bytes(candidate):
            with candidate.open("rb") as file:
                for block, expected in zip(case["targets"], case["expected_bytes"], strict=True):
                    file.seek(block * case["block_size"])
                    if file.read(case["block_size"]) != expected:
                        raise RuntimeError(f"incorrect recovered bytes: {candidate.name}, block {block}")

        check_bytes(image)
        before = digest(image)
        run([args.recover.resolve(), "--write", image])
        if digest(image) != before:
            raise RuntimeError("repeated recovery changed an already clean image")
        run([args.e2fsck.resolve(), "-fn", image])
        run([args.e2fsck.resolve(), "-fy", "-E", "journal_only", oracle])
        check_bytes(oracle)
        run([args.e2fsck.resolve(), "-fn", oracle])
        if digest(source) != case["image_sha256"]:
            raise RuntimeError("journal verification modified its source")
        result["recovered_sha256"] = before
        result["oracle_sha256"] = digest(oracle)
        result["passed"] = True
        (output / "report.json").write_text(json.dumps(results, indent=2) + "\n")
        print(f"PASS {case['name']}: core/oracle replay, exact contents, repeat recovery, e2fsck", flush=True)


if __name__ == "__main__":
    main()

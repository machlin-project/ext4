#!/usr/bin/env python3
"""Check allocation exports independently: sparse bytes, inode accounting and fsck."""

import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess

from generate_fixtures import resolve_tools


def digest(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def expected_contents(block_size):
    data = bytearray(block_size * 724)
    patch = bytes((index * 29 + 7) & 255 for index in range(block_size + 23))
    data[block_size - 7:block_size * 2 + 16] = patch
    size = block_size * 2 + 16
    for index in range(350):
        logical = 4 + ((index * 73) % 359) * 2
        offset = logical * block_size + index % 31
        data[offset:offset + 17] = bytes([index % 251 + 1]) * 17
        size = max(size, offset + 17)
    data[block_size * 3 - 7:block_size * 4 + 16] = patch
    return data[:size]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--exports", type=Path, required=True)
    parser.add_argument("--tools-root", type=Path)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    tools = resolve_tools(args.tools_root)
    images = sorted(args.exports.resolve().glob("*.img"))
    if not images:
        raise RuntimeError("no allocation exports")
    records = []
    for image in images:
        record = {"image": str(image), "input_sha256": digest(image), "commands": []}
        records.append(record)

        def save():
            (output / "report.json").write_text(json.dumps(records, indent=2) + "\n")

        def run(command):
            done = subprocess.run([str(part) for part in command], capture_output=True, text=True)
            record["commands"].append({"command": [str(part) for part in command],
                                       "status": done.returncode, "stdout": done.stdout,
                                       "stderr": done.stderr})
            save()
            done.check_returncode()
            return done.stdout

        header = run([tools["dumpe2fs"], "-h", image])
        block_size = int(re.search(r"^Block size:\s+(\d+)$", header, re.M)[1])
        record.update(block_size=block_size)
        if "needs_recovery" in header or not re.search(r"^Filesystem state:\s+clean$", header, re.M):
            raise RuntimeError("export is not cleanly finished")
        contents = output / f"{image.stem}.empty"
        run([tools["debugfs"], "-R", f"dump /empty {contents}", image])
        expected = expected_contents(block_size)
        if contents.read_bytes() != expected:
            raise RuntimeError(f"incorrect allocation bytes or exposed unwritten contents: {image.name}")
        payload = output / f"{image.stem}.payload"
        run([tools["debugfs"], "-R", f"dump /payload.bin {payload}", image])
        if payload.read_bytes() != bytes((index * 17 + 23) & 255 for index in range(200000)):
            raise RuntimeError("allocation damaged another inode's data")
        record["stat"] = run([tools["debugfs"], "-R", "stat /empty", image])
        run([tools["e2fsck"], "-fn", image])
        if digest(image) != record["input_sha256"]:
            raise RuntimeError("independent inspection changed source")
        record.update(size=len(expected), passed=True)
        save()
        print(f"PASS {image.name}: sparse contents, extent/indirect mapping, accounting, e2fsck", flush=True)


if __name__ == "__main__":
    main()

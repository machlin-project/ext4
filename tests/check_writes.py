#!/usr/bin/env python3
"""Independently check exported file mutations with e2fsprogs, without repairs."""

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


def encoded_time(seconds, nanoseconds):
    low = seconds & 0xffffffff
    signed = low if low <= 0x7fffffff else low - (1 << 32)
    return low, (nanoseconds << 2) | ((seconds - signed) >> 32)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--exports", type=Path, required=True)
    parser.add_argument("--tools-root", type=Path, help="e2fsprogs build; defaults to tools on PATH")
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    tools = resolve_tools(args.tools_root)
    images = sorted(args.exports.resolve().glob("*.img"))
    if not images:
        raise RuntimeError("no exported images")
    records = []
    for image in images:
        record = {"image": str(image), "input_sha256": digest(image), "commands": []}
        records.append(record)

        def save():
            (output / "report.json").write_text(json.dumps(records, indent=2) + "\n")

        def run(command):
            done = subprocess.run([str(x) for x in command], capture_output=True, text=True)
            record["commands"].append({"command": [str(x) for x in command],
                                       "status": done.returncode, "stdout": done.stdout,
                                       "stderr": done.stderr})
            save()
            done.check_returncode()
            return done.stdout

        header = run([tools["dumpe2fs"], "-h", image])
        block_size = int(re.search(r"^Block size:\s+(\d+)$", header, re.M)[1])
        inode_size = int(re.search(r"^Inode size:\s+(\d+)$", header, re.M)[1])
        record.update(block_size=block_size, inode_size=inode_size)
        if "needs_recovery" in header or not re.search(r"^Filesystem state:\s+clean$", header, re.M):
            raise RuntimeError("mutation export is not cleanly finished")
        payload = bytearray((index * 17 + 23) & 255 for index in range(200000))
        patch = bytes((index * 29 + 7) & 255 for index in range(block_size + 23))
        payload[block_size - 7:block_size * 2 + 16] = patch
        for name, expected in (("payload.bin", payload), ("hello.txt", b"Xachlin ext4\n"),
                               ("hello-hardlink", b"Xachlin ext4\n"), ("metadata.txt", b"metadata\n")):
            destination = output / f"{image.stem}.{name}"
            run([tools["debugfs"], "-R", f"dump /{name} {destination}", image])
            if destination.read_bytes() != expected:
                raise RuntimeError(f"incorrect independent file bytes: {image.name}: {name}")
        status = run([tools["debugfs"], "-R", "stat /metadata.txt", image])
        owners = re.search(r"User:\s+(-?\d+)\s+Group:\s+(-?\d+)", status)
        # debugfs formats these 32-bit fields with signed decimal on some builds.
        if not owners or tuple(int(value) & 0xffffffff for value in owners.groups()) != (4294967294, 2166572391) or not re.search(r"Mode:\s+0610", status):
            raise RuntimeError("incorrect independent inode ownership/mode")
        times = {"atime": (0, 0),
                 "mtime": (1700000001, 0) if inode_size == 128 else (-2147483648, 123456789),
                 "ctime": (1700000002, 0) if inode_size == 128 else (15032385535, 999999999)}
        if inode_size != 128:
            times["crtime"] = (4294967296, 987654321)
        for name, (seconds, nanos) in times.items():
            match = re.search(rf"\b{name}:\s+0x([0-9a-f]+)(?::([0-9a-f]+))?", status)
            low, extra = encoded_time(seconds, nanos)
            if not match or int(match[1], 16) != low or int(match[2] or "0", 16) != extra:
                raise RuntimeError(f"incorrect independent {name} wire encoding")
        hello = run([tools["debugfs"], "-R", "stat /hello-hardlink", image])
        if not re.search(r"User:\s+1111\s+Group:\s+2222", hello) or not re.search(r"Links:\s+2", hello):
            raise RuntimeError("incorrect hardlink metadata")
        run([tools["e2fsck"], "-fn", image])
        if digest(image) != record["input_sha256"]:
            raise RuntimeError("independent read-only checks changed the source")
        record["passed"] = True
        save()
        print(f"PASS {image.name}: contents, links, owners, modes, timestamps, e2fsck", flush=True)


if __name__ == "__main__":
    main()

#!/usr/bin/env python3
"""Author directory link-count and large-block growth boundaries with e2fsprogs."""

import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess

from check_namespace import inode_fields
from generate_fixtures import EXPECTED_FEATURES, UUID, resolve_tools

LINK_MAX = 65000
BLOCK_SIZE = 1024
IMAGE_BLOCKS = 128 * 1024
INODE_COUNT = 70016
INODE_INDEX = 0x1000
LARGE_BLOCK_SIZE = 65536
LARGE_BLOCK_INODES = 1024


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--tools-root", type=Path)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--verify-only", action="store_true",
                        help="Inspect the existing image without repeating its creation")
    parser.add_argument("--large-block-only", action="store_true",
                        help="Create a 64 KiB-block image with an internal journal")
    args = parser.parse_args()
    tools = resolve_tools(args.tools_root)
    output = args.output.resolve()
    if not args.verify_only:
        output.mkdir(parents=True, exist_ok=False)
    image = output / ("directory-64k.img" if args.large_block_only else "directory-links.img")
    block_size = LARGE_BLOCK_SIZE if args.large_block_only else BLOCK_SIZE
    inode_count = LARGE_BLOCK_INODES if args.large_block_only else INODE_COUNT
    payload = output / "hello.txt"
    script = output / "mkdir.debugfs"
    commands = [f"write {payload} /hello.txt"]
    if not args.large_block_only:
        commands += ["mkdir /indexed", "mkdir /peer", "mkdir /peer/child", "mkdir /indexed/child"]
        commands += [f"mkdir /indexed/d{index:05}" for index in range(LINK_MAX - 3)]
    record = (json.loads((output / "report.json").read_text())[0] if args.verify_only
              else dict(image=str(image), commands=[]))

    def run(command, allowed=(0,)):
        done = subprocess.run([str(part) for part in command], capture_output=True,
                              text=True, timeout=900)
        event = dict(command=[str(part) for part in command], status=done.returncode,
                     stdout=done.stdout, stderr=done.stderr)
        record["commands"].append(event)
        (output / "report.json").write_text(json.dumps([record], indent=2) + "\n")
        if done.returncode not in allowed:
            raise RuntimeError(f"Fixture command failed: {command}")
        return done.stdout

    features = EXPECTED_FEATURES - {"dir_nlink", "resize_inode"}
    if not args.verify_only:
        payload.write_bytes(b"Machlin ext4\n")
        script.write_text("\n".join(commands) + "\n")
        run([tools["mke2fs"], "-F", "-t", "ext4", "-b", block_size, "-N", inode_count,
             "-O", "none," + ",".join(sorted(features)), "-U", UUID,
             "-E", "lazy_itable_init=0,lazy_journal_init=0,nodiscard", image,
             IMAGE_BLOCKS * BLOCK_SIZE // block_size])
        run([tools["debugfs"], "-w", "-f", script, image])
        if not args.large_block_only:
            run([tools["e2fsck"], "-fyD", image], allowed=(0, 1))
    with image.open("rb") as stream:
        before = hashlib.file_digest(stream, "sha256").hexdigest()
    run([tools["e2fsck"], "-fn", image])
    header = run([tools["dumpe2fs"], "-h", image])
    actual = re.search(r"^Filesystem features:\s+(.+)$", header, re.M)
    if actual is None or set(actual[1].split()) != features:
        raise RuntimeError("Unexpected directory boundary feature set")
    if args.large_block_only:
        geometry = re.search(r"^Block size:\s+(\d+)$", header, re.M)
        contents = run([tools["debugfs"], "-R", "cat /hello.txt", image])
        if geometry is None or int(geometry[1]) != block_size or contents != "Machlin ext4\n":
            raise RuntimeError("Invalid large-block geometry or fixture contents")
        with image.open("rb") as stream:
            sha256 = hashlib.file_digest(stream, "sha256").hexdigest()
        if sha256 != before:
            raise RuntimeError("Independent inspection changed the image")
        record.update(passed=True, input_sha256=sha256, block_size=block_size, journal=True)
        (output / "report.json").write_text(json.dumps([record], indent=2) + "\n")
        print("PASS 64 KiB block directory fixture with internal journal", flush=True)
        return
    inode = inode_fields(run([tools["debugfs"], "-R", "stat /indexed", image]))
    listing = run([tools["debugfs"], "-R", "ls -p /indexed", image])
    rows = [line.split("/") for line in listing.splitlines() if line.strip()]
    if any(len(row) != 8 for row in rows):
        raise RuntimeError("Malformed independent directory listing")
    allocated_rows = [row for row in rows if int(row[1]) != 0]
    names = {row[5]: int(row[1]) for row in allocated_rows}
    expected = {".", "..", "child"} | {f"d{index:05}" for index in range(LINK_MAX - 3)}
    if (inode is None or inode["links"] != LINK_MAX or not inode["flags"] & INODE_INDEX or
            set(names) != expected or len(allocated_rows) != len(names) or
            len(set(names.values())) != len(names)):
        raise RuntimeError("Directory does not contain the exact independently authored boundary")
    with image.open("rb") as stream:
        sha256 = hashlib.file_digest(stream, "sha256").hexdigest()
    if sha256 != before:
        raise RuntimeError("Independent inspection changed the image")
    record.update(passed=True, input_sha256=sha256, links=inode["links"],
                  child_directories=LINK_MAX - 2, inode=inode, block_size=BLOCK_SIZE)
    (output / "report.json").write_text(json.dumps([record], indent=2) + "\n")
    print(f"PASS directory link boundary: {LINK_MAX - 2} child directories", flush=True)


if __name__ == "__main__":
    main()

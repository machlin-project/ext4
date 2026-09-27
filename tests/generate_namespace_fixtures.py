#!/usr/bin/env python3
"""Create small multi-group filesystems for inode exhaustion and lazy initialization."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess

from generate_fixtures import EXPECTED_FEATURES, UUID, resolve_tools

BLOCK_SIZE = 1024
BLOCKS_PER_GROUP = 1024
IMAGE_BLOCKS = 8192
INODES = 128


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--tools-root", type=Path)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    tools = resolve_tools(args.tools_root)
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    tree = output / "tree"
    tree.mkdir()
    (tree / "hello.txt").write_bytes(b"Machlin ext4\n")
    os.symlink("hello.txt", tree / "hello-link")
    profiles = [
        ("namespace-small", set(), 256, False),
        ("namespace-lazy", set(), 256, True),
        ("namespace-indirect", {"extent", "64bit", "flex_bg"}, 256, False),
        ("namespace-no-checksum", {"metadata_csum"}, 256, False),
        ("namespace-no-filetype", {"filetype"}, 256, False),
        ("namespace-inode128", {"extra_isize"}, 128, False),
    ]
    records = []
    for name, excluded, inode_size, lazy in profiles:
        features = EXPECTED_FEATURES - excluded - {"resize_inode"}
        image = output / f"{name}.img"
        record = {"image": str(image), "commands": []}
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

        run([tools["mke2fs"], "-F", "-t", "ext4", "-b", BLOCK_SIZE, "-g", BLOCKS_PER_GROUP,
             "-N", INODES, "-I", inode_size, "-O", "none," + ",".join(sorted(features)),
             "-U", UUID, "-E", f"lazy_itable_init={int(lazy)},lazy_journal_init=0,nodiscard",
             "-d", tree, image, IMAGE_BLOCKS])
        run([tools["e2fsck"], "-fn", image])
        header = run([tools["dumpe2fs"], "-h", image])
        groups = run([tools["dumpe2fs"], image])
        match = re.search(r"^Filesystem features:\s+(.+)$", header, re.M)
        if not match or set(match[1].split()) != features:
            raise RuntimeError("Unexpected namespace fixture features")
        inodes = int(re.search(r"^Inode count:\s+(\d+)$", header, re.M)[1])
        per_group = int(re.search(r"^Inodes per group:\s+(\d+)$", header, re.M)[1])
        free = int(re.search(r"^Free inodes:\s+(\d+)$", header, re.M)[1])
        if inodes != INODES or per_group * (IMAGE_BLOCKS // BLOCKS_PER_GROUP) != inodes:
            raise RuntimeError("Unexpected inode geometry")
        if "metadata_csum" in features and "INODE_UNINIT" not in groups:
            raise RuntimeError("Fixture does not exercise lazy inode bitmaps")
        lazy_groups = [line for line in groups.splitlines()
                       if re.match(r"^Group [1-9].*INODE_UNINIT", line) and "ITABLE_ZEROED" not in line]
        if lazy and not lazy_groups:
            raise RuntimeError("Missing inode group with an uninitialized table")
        with image.open("rb") as stream:
            sha256 = hashlib.file_digest(stream, "sha256").hexdigest()
        record.update(block_size=BLOCK_SIZE, inodes=inodes, inodes_per_group=per_group,
                      free_inodes=free, lazy_table_groups=len(lazy_groups), input_sha256=sha256, passed=True)
        save()
        print(f"PASS {image.name}: {inodes} inodes in {inodes // per_group} groups", flush=True)


if __name__ == "__main__":
    main()

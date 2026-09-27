#!/usr/bin/env python3
"""Create full block bitmaps with independent tools for namespace capacity tests."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess

from check_namespace import inode_fields
from check_orphans import accounting
from generate_fixtures import EXPECTED_FEATURES, UUID, resolve_tools

IMAGE_BYTES = 8 * 1024 * 1024
NAME_MAX = 255
DIRECTORY_HEADER_BYTES = 8
DIRECTORY_ALIGNMENT = 4
DIRECTORY_DOT_BYTES = 24
DIRECTORY_TAIL_BYTES = 12
RESERVE_BLOCKS = 8
INITIAL_INDEX_NAMES = 96
INODE_INDEX = 0x1000


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--tools-root", type=Path)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    tools = resolve_tools(args.tools_root)
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    payload = output / "filler.data"
    payload.write_bytes(b"m" * IMAGE_BYTES)
    profiles = [dict(name="space-1k"), dict(name="space-4k", block_size=4096),
                dict(name="space-indirect", exclude={"extent", "64bit", "flex_bg"}),
                dict(name="space-no-checksum", exclude={"metadata_csum"}),
                dict(name="space-no-filetype", exclude={"filetype"}),
                dict(name="space-inode128", exclude={"extra_isize"}, inode_size=128),
                dict(name="space-checksum-seed", include={"metadata_csum_seed"}),
                dict(name="space-orphan-file", include={"orphan_file"})]
    records = []

    def save():
        (output / "report.json").write_text(json.dumps(records, indent=2) + "\n")

    for profile in profiles:
        block_size = profile.get("block_size", 1024)
        features = (EXPECTED_FEATURES - {"resize_inode"} - profile.get("exclude", set()) |
                    profile.get("include", set()))
        tree = output / f"{profile['name']}-tree"
        tree.mkdir()
        (tree / "target").write_bytes(b"T" * block_size)
        (tree / "reserve").write_bytes(b"S" * (RESERVE_BLOCKS * block_size))
        (tree / "empty").write_bytes(b"")
        (tree / "room").mkdir()
        for parent, prefix, count in (("linear", "linear", None),
                                      ("indexed", "initial", INITIAL_INDEX_NAMES)):
            directory = tree / parent
            directory.mkdir()
            if count is None:
                tail = DIRECTORY_TAIL_BYTES if "metadata_csum" in features else 0
                record_bytes = ((DIRECTORY_HEADER_BYTES + NAME_MAX + DIRECTORY_ALIGNMENT - 1) &
                                ~(DIRECTORY_ALIGNMENT - 1))
                count = (block_size - DIRECTORY_DOT_BYTES - tail) // record_bytes
            for index in range(count):
                name = f"{prefix}-{index:06d}-".ljust(NAME_MAX, "n")
                os.link(tree / "target", directory / name)
        image = output / f"{profile['name']}.img"
        record = dict(image=str(image), block_size=block_size, reserve_blocks=RESERVE_BLOCKS,
                      commands=[])
        records.append(record)

        def run(command, allowed=(0,)):
            done = subprocess.run([str(part) for part in command], capture_output=True,
                                  text=True, errors="backslashreplace", timeout=120)
            record["commands"].append(dict(command=[str(part) for part in command],
                                            status=done.returncode, stdout=done.stdout, stderr=done.stderr))
            save()
            if done.returncode not in allowed:
                raise RuntimeError(f"Full-space fixture command failed: {command}")
            return done.stdout

        extended = "lazy_itable_init=0,lazy_journal_init=0,nodiscard"
        if "orphan_file" in features:
            extended += ",orphan_file_size=4"
        run([tools["mke2fs"], "-F", "-t", "ext4", "-b", block_size, "-g", 1024,
             "-N", 256, "-I", profile.get("inode_size", 256), "-m", 0,
             "-O", "none," + ",".join(sorted(features)), "-U", UUID, "-E", extended,
             "-d", tree, image, IMAGE_BYTES // block_size])
        run([tools["e2fsck"], "-fyD", image], allowed=(0, 1))
        # debugfs reports ENOSPC in stderr but returns zero. The independent
        # bitmap/counter and nonrepairing checks below decide whether filling
        # succeeded. The unallocated suffix of this oversized file is a hole.
        run([tools["debugfs"], "-w", "-R", f'write "{payload}" /filler', image])
        run([tools["e2fsck"], "-fn", image])
        header = run([tools["dumpe2fs"], "-h", image])
        counts = accounting(header)
        found = re.search(r"^Filesystem features:\s+(.+)$", header, re.M)
        if found is None or set(found[1].split()) != features or counts["Free blocks"] != 0:
            raise RuntimeError("Full-space fixture has unexpected features or free blocks")
        groups = run([tools["dumpe2fs"], image])
        free_counts = re.findall(r"^\s+(\d+) free blocks,", groups, re.M)
        if not free_counts or any(int(value) != 0 for value in free_counts) or "BLOCK_UNINIT" in groups:
            raise RuntimeError("Full-space fixture does not have completely allocated block groups")
        objects = {}
        for name in ("linear", "indexed", "reserve", "filler"):
            objects[name] = inode_fields(run([tools["debugfs"], "-R", f"stat /{name}", image]))
            if objects[name] is None:
                raise RuntimeError(f"Missing full-space object {name}")
        if (objects["linear"]["flags"] & INODE_INDEX or objects["linear"]["size"] != block_size or
                not objects["indexed"]["flags"] & INODE_INDEX or
                objects["reserve"]["blocks"] != RESERVE_BLOCKS * block_size // 512 or
                objects["reserve"]["size"] != RESERVE_BLOCKS * block_size):
            raise RuntimeError("Full-space fixture has incorrect directory or reserve geometry")
        with image.open("rb") as stream:
            digest = hashlib.file_digest(stream, "sha256").hexdigest()
        record.update(passed=True, input_sha256=digest, accounting=counts, objects=objects,
                      full_block_groups=len(free_counts))
        save()
        print(f"PASS {image.name}: {len(free_counts)} completely allocated groups; indexed and linear parents", flush=True)


if __name__ == "__main__":
    main()

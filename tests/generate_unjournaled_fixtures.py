#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
"""Author empty volumes without a journal with e2fsprogs.

The base ext4 feature set without has_journal at 4 KiB and 1 KiB blocks, and an ext2
volume with block maps, exercise EXT4_WRITE_UNJOURNALED; small copies keep one
exported image per power cut compact and have a second group, whose backup superblock
lets e2fsck repair a torn primary one. Strict fsck must accept each image before the
core changes it."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess

from generate_fixtures import EXPECTED_FEATURES, UUID, resolve_tools

IMAGE_BYTES = 64 * 1024 * 1024
SMALL_IMAGE_BYTES = 16 * 1024 * 1024
# Blocks per group of the small 4 KiB volume, which would otherwise have one group.
SMALL_GROUP_BLOCKS = 1024
EXT2_FEATURES = {"ext_attr", "resize_inode", "dir_index", "filetype", "sparse_super",
                 "large_file"}
# name, block size, features, bytes, further mke2fs arguments
VARIANTS = (
    ("unjournaled-4k", 4096, EXPECTED_FEATURES - {"has_journal"}, IMAGE_BYTES, ()),
    ("unjournaled-1k", 1024, EXPECTED_FEATURES - {"has_journal"}, IMAGE_BYTES, ()),
    ("ext2-1k", 1024, EXT2_FEATURES, IMAGE_BYTES, ()),
    ("unjournaled-small-4k", 4096, EXPECTED_FEATURES - {"has_journal"}, SMALL_IMAGE_BYTES,
     ("-g", SMALL_GROUP_BLOCKS)),
    ("unjournaled-small-1k", 1024, EXPECTED_FEATURES - {"has_journal"}, SMALL_IMAGE_BYTES, ()),
    ("ext2-small-1k", 1024, EXT2_FEATURES, SMALL_IMAGE_BYTES, ()),
)


def digest(path):
    with Path(path).open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--tools-root", type=Path)
    args = parser.parse_args()
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    tools = resolve_tools(args.tools_root)
    reports = []
    for name, block, features, size, extra in VARIANTS:
        image = output / f"{name}.img"
        row = dict(variant=name, image=str(image), block_size=block, commands=[],
                   passed=False)
        reports.append(row)

        def run(command, row=row):
            result = subprocess.run([str(x) for x in command], capture_output=True, text=True,
                                    errors="backslashreplace", timeout=300)
            row["commands"].append(dict(command=[str(x) for x in command],
                                        status=result.returncode, stdout=result.stdout[-4000:],
                                        stderr=result.stderr[-4000:]))
            (output / "report.json").write_text(json.dumps(reports, indent=2) + "\n")
            result.check_returncode()
            return result.stdout

        run([tools["mke2fs"], "-F", "-t", "ext4", "-b", block, "-m", 0,
             "-O", "none," + ",".join(sorted(features)), "-U", UUID, *extra, image,
             size // block])
        run([tools["e2fsck"], "-fn", image])
        header = run([tools["dumpe2fs"], "-h", image])
        listed = set(next(line for line in header.splitlines()
                          if line.startswith("Filesystem features:")).split(":")[1].split())
        if "has_journal" in listed or not features <= listed:
            raise RuntimeError(f"{name}: unexpected features {sorted(listed)}")
        row.update(passed=True, features=sorted(listed), sha256=digest(image))
        (output / "report.json").write_text(json.dumps(reports, indent=2) + "\n")
        print(f"PASS unjournaled {name}: {' '.join(sorted(listed))}", flush=True)


if __name__ == "__main__":
    main()

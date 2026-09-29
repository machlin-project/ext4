#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
"""Author one empty volume per optional format feature and geometry with e2fsprogs.

Each variant changes the base feature set of generate_fixtures.py or its geometry
in one respect that the other writable fixtures do not already cover. The sustained
suite runs mixed operations with power cuts on every variant; strict fsck must
accept each image before the core changes it."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess

from generate_fixtures import EXPECTED_FEATURES, UUID, resolve_tools

IMAGE_BYTES = 32 * 1024 * 1024
# mke2fs gives a 64 KiB-block volume a journal only with 1,024 journal blocks.
LARGE_BLOCK_IMAGE_BYTES = 128 * 1024 * 1024
# name, block size, features added, features removed, further mke2fs arguments
VARIANTS = (
    ("block-64k", 65536, set(), set(), ()),
    ("meta-bg", 4096, {"meta_bg"}, {"resize_inode"}, ()),
    ("meta-bg-1k", 1024, {"meta_bg"}, {"resize_inode"}, ()),
    ("sparse-super2", 4096, {"sparse_super2"}, set(), ()),
    ("sparse-super2-no-backups", 4096, {"sparse_super2"}, set(),
     ("-E", "num_backup_sb=0")),
    ("no-sparse-super", 4096, set(), {"sparse_super", "resize_inode"}, ()),
    ("no-flex-bg", 4096, set(), {"flex_bg"}, ()),
    ("no-64bit", 4096, set(), {"64bit"}, ()),
    ("no-huge-file", 4096, set(), {"huge_file"}, ()),
    ("no-dir-nlink", 4096, set(), {"dir_nlink"}, ()),
    ("no-extra-isize", 4096, set(), {"extra_isize"}, ()),
    ("no-filetype", 4096, set(), {"filetype"}, ()),
    ("no-dir-index", 4096, set(), {"dir_index"}, ()),
    ("no-ext-attr", 4096, set(), {"ext_attr"}, ()),
    ("group-checksum", 4096, {"uninit_bg"}, {"metadata_csum"}, ()),
    ("inode-512", 4096, set(), set(), ("-I", "512")),
    ("inode-1024", 4096, set(), set(), ("-I", "1024")),
    ("few-inodes", 4096, set(), set(), ("-N", "64")),
    ("small-groups", 1024, set(), set(), ("-g", "1024")),
    ("large-dir", 4096, {"large_dir"}, set(), ()),
    ("stable-inodes", 4096, {"stable_inodes"}, set(), ()),
    ("orphan-file", 4096, {"orphan_file"}, set(), ()),
    ("fast-commit", 4096, {"fast_commit"}, set(), ()),
    ("inline-data", 4096, {"inline_data"}, set(), ()),
    ("ea-inode", 4096, {"ea_inode"}, set(), ()),
    ("bigalloc-64k", 4096, {"bigalloc"}, set(), ("-C", "65536")),
    ("encrypt", 4096, {"encrypt"}, set(), ()),
    ("casefold", 4096, {"casefold"}, set(), ()),
    ("verity", 4096, {"verity"}, set(), ()),
)
NAMES = tuple(name for name, *_ in VARIANTS)


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
    for name, block, added, removed, extra in VARIANTS:
        image = output / f"format-{name}.img"
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

        features = (EXPECTED_FEATURES | added) - removed
        size = LARGE_BLOCK_IMAGE_BYTES if block == 65536 else IMAGE_BYTES
        run([tools["mke2fs"], "-F", "-t", "ext4", "-b", block, "-m", 0,
             "-O", "none," + ",".join(sorted(features)), "-U", UUID, *extra,
             image, size // block])
        run([tools["e2fsck"], "-fn", image])
        header = run([tools["dumpe2fs"], "-h", image])
        listed = set(next(line for line in header.splitlines()
                          if line.startswith("Filesystem features:")).split(":")[1].split())
        if not features <= listed | {"none"}:
            raise RuntimeError(f"{name}: mke2fs dropped features {sorted(features - listed)}")
        row.update(passed=True, features=sorted(listed), sha256=digest(image))
        (output / "report.json").write_text(json.dumps(reports, indent=2) + "\n")
        print(f"PASS format {name}: {' '.join(sorted(listed))}", flush=True)


if __name__ == "__main__":
    main()

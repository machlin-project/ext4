#!/usr/bin/env python3
"""Create CRC16 group descriptors, including lazy block and inode groups."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import re

from generate_fixtures import (
    EXPECTED_FEATURES, UUID, create_image, make_tree, record_features, resolve_tools,
    run_logged, tool_version,
)


FEATURES = EXPECTED_FEATURES - {"metadata_csum"} | {"uninit_bg"}
SMALL_BLOCK_SIZE = 1024
SMALL_BLOCKS_PER_GROUP = 1024
SMALL_IMAGE_BLOCKS = 8192
SMALL_INODES = 128


def field(text, name):
    match = re.search(rf"^{re.escape(name)}:\s+(\d+)$", text, re.M)
    if not match:
        raise RuntimeError(f"Missing numeric field: {name}")
    return int(match[1])


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--tools-root", type=Path)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    tools = resolve_tools(args.tools_root)
    versions = {name: tool_version(path) for name, path in tools.items()}
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    root = output / "root"
    make_tree(root)
    small_root = output / "small-root"
    small_root.mkdir()
    (small_root / "hello.txt").write_bytes(b"Machlin ext4\n")
    os.symlink("hello.txt", small_root / "hello-link")
    records = []
    profiles = [
        ("gdt-1k32", 1024, 256, FEATURES - {"64bit"}, False),
        ("gdt-4k64", 4096, 256, FEATURES, False),
        ("gdt-indirect", 1024, 256, FEATURES - {"extent", "64bit"}, False),
        ("gdt-inode128", 1024, 128, FEATURES - {"extra_isize"}, False),
        ("gdt-small64", 1024, 256, FEATURES - {"resize_inode"}, True),
        ("gdt-small-indirect", 1024, 256,
         FEATURES - {"resize_inode", "extent", "64bit", "flex_bg"}, True),
    ]
    for name, block_size, inode_size, features, small in profiles:
        image = output / f"{name}.img"
        if small:
            run_logged(output, f"mke2fs-{name}", [
                str(tools["mke2fs"]), "-F", "-t", "ext4", "-b", str(block_size),
                "-g", str(SMALL_BLOCKS_PER_GROUP), "-N", str(SMALL_INODES),
                "-I", str(inode_size), "-O", "none," + ",".join(sorted(features)),
                "-U", UUID, "-E", "lazy_itable_init=1,lazy_journal_init=0,nodiscard",
                "-d", str(small_root), str(image), str(SMALL_IMAGE_BLOCKS),
            ], versions)
            run_logged(output, f"e2fsck-{name}",
                       [str(tools["e2fsck"]), "-fn", str(image)], versions)
        else:
            create_image(output, root, image, block_size, tools, versions,
                         features, inode_size)
        actual, _ = record_features(output, image, tools, versions)
        if actual != features:
            raise RuntimeError(f"Unexpected features for {name}: {actual ^ features}")
        groups = run_logged(output, f"groups-{name}",
                            [str(tools["dumpe2fs"]), str(image)], versions)
        if field(groups, "Block size") != block_size or field(groups, "Inode size") != inode_size:
            raise RuntimeError(f"Unexpected geometry: {name}")
        group_lines = [line for line in groups.splitlines() if re.match(r"^Group \d+:", line)]
        lazy_inodes = sum("INODE_UNINIT" in line for line in group_lines)
        lazy_blocks = sum("BLOCK_UNINIT" in line for line in group_lines)
        lazy_tables = sum("INODE_UNINIT" in line and "ITABLE_ZEROED" not in line
                          for line in group_lines)
        if small:
            expected_groups = SMALL_IMAGE_BLOCKS // SMALL_BLOCKS_PER_GROUP
            if (field(groups, "Inode count") != SMALL_INODES
                    or field(groups, "Inodes per group") * expected_groups != SMALL_INODES
                    or len(group_lines) != expected_groups
                    or not lazy_inodes or not lazy_blocks or not lazy_tables):
                raise RuntimeError(f"Missing small lazy group coverage: {name}")
        if "64bit" in features and field(groups, "Group descriptor size") != 64:
            raise RuntimeError(f"Unexpected wide descriptor: {name}")
        with image.open("rb") as stream:
            digest = hashlib.file_digest(stream, "sha256").hexdigest()
        records.append({
            "image": str(image), "input_sha256": digest, "features": sorted(actual),
            "block_size": block_size, "inode_size": inode_size,
            "descriptor_size": 64 if "64bit" in features else 32,
            "groups": len(group_lines), "lazy_inode_groups": lazy_inodes,
            "lazy_block_groups": lazy_blocks, "lazy_table_groups": lazy_tables,
            "passed": True,
        })
        (output / "report.json").write_text(json.dumps({
            "tools": {name: {"path": str(tools[name]), "version": version}
                      for name, version in versions.items()},
            "profiles": records,
        }, indent=2) + "\n")
        print(f"PASS {image.name}: CRC16, {len(group_lines)} groups, "
              f"lazy inodes/blocks/tables={lazy_inodes}/{lazy_blocks}/{lazy_tables}", flush=True)


if __name__ == "__main__":
    main()

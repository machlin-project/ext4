#!/usr/bin/env python3
"""Create distributed descriptors and sparse-superblock layouts with e2fsprogs."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import re

from generate_fixtures import (
    EXPECTED_FEATURES, UUID, resolve_tools, run_logged, tool_version,
)
from generate_group_checksum_fixtures import field


BLOCKS_PER_GROUP = 256
INODES_PER_GROUP = 16
BASE = EXPECTED_FEATURES - {"resize_inode"}
META = BASE | {"meta_bg"}
PROFILES = [
    ("meta-1k", 1024, 51, META, 0, None),
    ("meta-32", 1024, 67, META - {"64bit", "flex_bg"}, 0, None),
    ("meta-4k", 4096, 67, META, 0, None),
    ("meta-gdt", 1024, 35, META - {"metadata_csum"} | {"uninit_bg"}, 0, None),
    ("meta-hybrid", 1024, 51, META, 1, None),
    ("meta-sparse2", 1024, 51, META | {"sparse_super2"}, 0, 2),
    ("sparse2-0", 1024, 19, BASE | {"sparse_super2"}, 0, 0),
    ("sparse2-1", 1024, 19, BASE | {"sparse_super2"}, 0, 1),
    ("sparse2-2", 1024, 19, BASE | {"sparse_super2"}, 0, 2),
]


def group_layout(text):
    records = []
    for line in text.splitlines():
        if not re.match(r"^\d+:", line):
            continue
        group, first, superblock, descriptors, blockmap, inodemap, table = line.split(":")
        span = re.fullmatch(r"(\d+)(?:-(\d+))?", descriptors)
        records.append(dict(
            group=int(group), first=int(first), superblock=int(superblock),
            descriptors_first=int(span[1]) if span else -1,
            descriptors_last=int(span[2] or span[1]) if span else -1,
            block_bitmap=int(blockmap), inode_bitmap=int(inodemap), inode_table=int(table),
        ))
        if span is None and descriptors != "-1":
            raise RuntimeError(f"Unexpected descriptor location: {line}")
    if not records or [item["group"] for item in records] != list(range(len(records))):
        raise RuntimeError("Missing or unordered independent group layout")
    return records


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
    root.mkdir()
    (root / "hello.txt").write_bytes(b"Machlin ext4\n")
    (root / "empty").touch()
    records = []
    for name, block_size, groups, features, first_meta, backups in PROFILES:
        image = output / f"{name}.img"
        blocks = groups * BLOCKS_PER_GROUP + int(block_size == 1024)
        extended = "lazy_itable_init=1,lazy_journal_init=0,nodiscard"
        if backups is not None:
            extended += f",num_backup_sb={backups}"
        # mke2fs exposes this test input for independently creating the hybrid
        # layout used after growth from contiguous to distributed descriptors.
        saved = os.environ.get("MKE2FS_FIRST_META_BG")
        os.environ["MKE2FS_FIRST_META_BG"] = str(first_meta)
        try:
            run_logged(output, f"mke2fs-{name}", [
                str(tools["mke2fs"]), "-F", "-t", "ext4", "-b", str(block_size),
                "-g", str(BLOCKS_PER_GROUP), "-N", str(groups * INODES_PER_GROUP),
                "-I", "256", "-m", "0", "-O", "none," + ",".join(sorted(features)),
                "-U", UUID, "-E", extended, "-d", str(root), str(image), str(blocks),
            ], versions)
        finally:
            if saved is None:
                os.environ.pop("MKE2FS_FIRST_META_BG", None)
            else:
                os.environ["MKE2FS_FIRST_META_BG"] = saved
        run_logged(output, f"e2fsck-{name}", [str(tools["e2fsck"]), "-fn", str(image)], versions)
        header = run_logged(output, f"header-{name}",
                            [str(tools["dumpe2fs"]), "-h", str(image)], versions)
        actual = set(next(line for line in header.splitlines()
                          if line.startswith("Filesystem features:")).split(":", 1)[1].split())
        if actual != features or field(header, "Inodes per group") != INODES_PER_GROUP:
            raise RuntimeError(f"Unexpected format or inode geometry: {name}")
        if first_meta and field(header, "First meta block group") != first_meta:
            raise RuntimeError(f"Missing hybrid descriptor layout: {name}")
        layout = group_layout(run_logged(output, f"groups-{name}",
                                         [str(tools["dumpe2fs"]), "-g", str(image)], versions))
        if len(layout) != groups:
            raise RuntimeError(f"Unexpected group count: {name}")
        expected = image.with_suffix(".geometry")
        expected.write_text("".join(" ".join(str(value) for value in item.values()) + "\n"
                                    for item in layout))
        with image.open("rb") as stream:
            digest = hashlib.file_digest(stream, "sha256").hexdigest()
        records.append(dict(image=str(image), input_sha256=digest, features=sorted(actual),
                            block_size=block_size, groups=groups, layout=layout,
                            first_meta_group=first_meta, backup_count=backups,
                            create_environment={"MKE2FS_FIRST_META_BG": str(first_meta)},
                            expected=str(expected), passed=True))
        (output / "report.json").write_text(json.dumps(dict(profiles=records, tools={
            name: dict(path=str(tools[name]), version=version) for name, version in versions.items()
        }), indent=2) + "\n")
        print(f"PASS {name}: {groups} independently described groups", flush=True)


if __name__ == "__main__":
    main()

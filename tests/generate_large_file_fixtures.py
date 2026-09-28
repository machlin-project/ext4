#!/usr/bin/env python3
"""Import sparse files at logical-address limits into bounded ext4 images."""

import argparse
import json
import os
from pathlib import Path
import shlex
import subprocess

from check_namespace import inode_fields
from check_orphans import digest
from generate_fixtures import EXPECTED_FEATURES, UUID, resolve_tools

LOGICAL_BLOCKS = (1 << 32) - 1
SECTOR_BYTES = 512
DIRECT_BLOCKS = 12
POINTER_BYTES = 4
PROFILES = (
    dict(name="1k", block=1024),
    dict(name="4k", block=4096),
    dict(name="64k", block=65536),
    dict(name="indirect", block=1024, exclude={"extent", "64bit"}),
    dict(name="inline", block=1024, include={"inline_data"}),
    dict(name="cluster", block=1024, ratio=4, include={"bigalloc"}),
    dict(name="cluster-inline", block=1024, ratio=4, include={"bigalloc", "inline_data"}),
    dict(name="no-huge", block=1024, exclude={"huge_file"}),
    dict(name="indirect-no-huge", block=4096, exclude={"extent", "64bit", "huge_file"}),
)


def indirect_metadata(blocks, pointers):
    remaining = max(blocks - DIRECT_BLOCKS, 0)
    result = 0
    for depth in range(1, 4):
        covered = min(remaining, pointers ** depth)
        result += sum((covered + pointers ** level - 1) // pointers ** level
                      for level in range(1, depth + 1))
        remaining -= covered
    return result


def file_limit(block, extents=True, huge=True):
    pointers = block // POINTER_BYTES
    blocks = LOGICAL_BLOCKS if extents else min(LOGICAL_BLOCKS, DIRECT_BLOCKS + pointers + pointers ** 2 + pointers ** 3)
    if not huge:
        ceiling = LOGICAL_BLOCKS // (block // SECTOR_BYTES)
        blocks = min(blocks, ceiling)
        if not extents and blocks + indirect_metadata(blocks, pointers) > ceiling:
            blocks = ceiling - indirect_metadata(ceiling, pointers)
    return blocks * block


def segments(block, limit):
    positions = [0, (1 << 31) - 3, (1 << 32) - 3]
    signed_logical = (1 << 31) * block - 3
    if signed_logical + 7 < limit:
        positions.append(signed_logical)
    return [(offset, bytes([0x41 + index]) * 7) for index, offset in enumerate(positions)] + [(limit - 1, b"Z")]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--tools-root", type=Path)
    args = parser.parse_args()
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    tools = resolve_tools(args.tools_root)
    # mke2fs's sparse importer passes offsets through a signed 32-bit argument
    # in e2fsprogs 1.47.3. Use the public 64-bit API for the independent seed.
    build = args.tools_root.resolve() if args.tools_root else tools["debugfs"].parent.parent
    helper = output / "large-file-fixture"
    command = shlex.split(os.environ.get("CC", "cc")) + ["-std=c11", "-O2", "-Wall", "-Wextra", "-Werror",
               "-Wdeclaration-after-statement", f"-I{build / 'lib'}", f"-I{build.parent / 'lib'}",
               str(Path(__file__).with_name("large_file_fixture.c")),
               str(build / "lib/libext2fs.a"), str(build / "lib/libcom_err.a"),
               "-lpthread", "-o", str(helper)]
    compiled = subprocess.run(command, capture_output=True, text=True)
    (output / "helper-build.json").write_text(json.dumps(dict(
        command=command, status=compiled.returncode, stdout=compiled.stdout, stderr=compiled.stderr), indent=2) + "\n")
    compiled.check_returncode()
    reports = []
    for profile in PROFILES:
        block = profile["block"]
        features = (EXPECTED_FEATURES - {"resize_inode"} - profile.get("exclude", set())) | profile.get("include", set())
        limit = file_limit(block, "extent" in features, "huge_file" in features)
        tree = output / (profile["name"] + "-tree")
        tree.mkdir()
        seed = tree / "seed"
        spans = segments(block, limit)
        seed.write_bytes(b"")
        image = output / ("large-file-" + profile["name"] + ".img")
        row = dict(image=str(image), profile=profile["name"], block_size=block,
                   cluster_blocks=profile.get("ratio", 1), limit=limit,
                   segments=[dict(offset=off, data=value.hex()) for off, value in spans], commands=[])
        reports.append(row)

        def run(command):
            done = subprocess.run([str(x) for x in command], capture_output=True, text=True,
                                  errors="backslashreplace", timeout=120)
            row["commands"].append(dict(command=[str(x) for x in command], status=done.returncode,
                                         stdout=done.stdout, stderr=done.stderr))
            (output / "report.json").write_text(json.dumps(reports, indent=2) + "\n")
            if done.returncode != 0:
                raise RuntimeError(f"Independent command failed: {command}: {done.stderr}")
            return done.stdout

        geometry = ["-C", block * profile["ratio"]] if "ratio" in profile else []
        image_bytes = (128 if block == 65536 else 32) * 1024 * 1024
        run([tools["mke2fs"], "-F", "-t", "ext4", "-b", block, *geometry,
             "-N", 256, "-I", 256, "-m", 0, "-O", "none," + ",".join(sorted(features)),
             "-U", UUID, "-E", "lazy_itable_init=0,lazy_journal_init=0,nodiscard",
             "-d", tree, image, image_bytes // block])
        run([helper, image, "/seed", limit,
             *(argument for off, value in spans for argument in (off, value.hex()))])
        run([tools["e2fsck"], "-fn", image])
        inode = inode_fields(run([tools["debugfs"], "-R", "stat /seed", image]))
        if inode["size"] != limit:
            raise RuntimeError("Independent imported file limit differs")
        row.update(passed=True, inode=inode, image_sha256=digest(image))
        (output / "report.json").write_text(json.dumps(reports, indent=2) + "\n")
        seed.unlink()
        print(f"PASS {profile['name']}: logical size {limit}, bounded image {image_bytes}", flush=True)


if __name__ == "__main__":
    main()

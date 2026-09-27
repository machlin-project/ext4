#!/usr/bin/env python3
"""Add an independently allocated ext4 orphan file to clean image copies."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import struct
import subprocess

from generate_fixtures import resolve_tools


DEFAULT_ORPHAN_BLOCKS = 4
MAX_ORPHAN_BLOCKS = 512
ORPHAN_TAIL = struct.Struct("<II")
ORPHAN_MAGIC = 0x0B10CA04
PROFILES = (
    "ext4-1k.img", "ext4-2k.img", "ext4-4k.img", "ext4-8k.img",
    "ext4-16k.img", "ext4-32k.img", "ext4-indirect-1k.img",
    "ext4-no-checksum.img", "ext4-checksum-seed.img", "ext4-inode128.img",
)


def digest(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def field(header, name):
    match = re.search(rf"^{re.escape(name)}:\s+(.+)$", header, re.M)
    if not match:
        raise RuntimeError(f"Missing {name} in dumpe2fs output")
    return match[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--fixtures", type=Path, required=True)
    parser.add_argument("--tools-root", type=Path)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--blocks", type=int, default=DEFAULT_ORPHAN_BLOCKS)
    parser.add_argument("--case", action="append", choices=PROFILES)
    args = parser.parse_args()
    if not 2 <= args.blocks <= MAX_ORPHAN_BLOCKS:
        parser.error("--blocks must be between 2 and 512")
    tools = resolve_tools(args.tools_root)
    tune2fs = (args.tools_root.resolve() / "misc/tune2fs" if args.tools_root
               else Path(shutil.which("tune2fs") or "/missing/tune2fs"))
    if not tune2fs.is_file() or not os.access(tune2fs, os.X_OK):
        raise FileNotFoundError(f"Missing executable tune2fs: {tune2fs}")
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    records = []
    for name in args.case or PROFILES:
        source = (args.fixtures / name).resolve()
        image = output / f"orphan-file-{name}"
        record = {"source": str(source), "source_sha256": digest(source),
                  "image": str(image), "commands": []}
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

        shutil.copyfile(source, image)
        header = run([tools["dumpe2fs"], "-h", image])
        features = set(field(header, "Filesystem features").split())
        if "orphan_file" in features or "orphan_present" in features or "needs_recovery" in features:
            raise RuntimeError("Expected a clean source without an orphan file")
        block_size = int(field(header, "Block size"))
        run([tools["e2fsck"], "-fn", image])
        run([tune2fs, "-O", "orphan_file", "-E",
             f"orphan_file_size={args.blocks * block_size // 1024}K", image])
        after = run([tools["dumpe2fs"], "-h", image])
        if set(field(after, "Filesystem features").split()) != features | {"orphan_file"}:
            raise RuntimeError("Unexpected feature changes while adding orphan_file")
        number = int(field(after, "Orphan file inode"))
        stat = run([tools["debugfs"], "-R", f"stat <{number}>", image])
        size = re.search(r"\bSize:\s+(\d+)", stat)
        generation = re.search(r"\bGeneration:\s+(\d+)", stat)
        if not size or int(size[1]) != args.blocks * block_size or not generation:
            raise RuntimeError(f"Unexpected orphan file inode: {stat!r}")
        blocks = []
        for logical in range(args.blocks):
            mapping = run([tools["debugfs"], "-R", f"bmap <{number}> {logical}", image])
            match = re.fullmatch(r"\s*(\d+)\s*", mapping)
            if not match:
                raise RuntimeError(f"Missing initialized orphan block: {mapping!r}")
            blocks.append(int(match[1]))
        if len(set(blocks)) != args.blocks or any(
                block <= 0 or (block + 1) * block_size > image.stat().st_size for block in blocks):
            raise RuntimeError("Invalid or duplicated orphan file blocks")
        with image.open("rb") as stream:
            for block in blocks:
                stream.seek(block * block_size)
                data = stream.read(block_size)
                magic, _checksum = ORPHAN_TAIL.unpack(data[-ORPHAN_TAIL.size:])
                if magic != ORPHAN_MAGIC or any(data[:-ORPHAN_TAIL.size]):
                    raise RuntimeError("Expected valid empty orphan file blocks")
        run([tools["e2fsck"], "-fn", image])
        if digest(source) != record["source_sha256"]:
            raise RuntimeError("Fixture source changed")
        record.update(block_size=block_size, orphan_inode=number,
                      orphan_generation=int(generation[1]), orphan_blocks=blocks,
                      input_sha256=digest(image), passed=True)
        save()
        print(f"PASS {image.name}: orphan inode {number}, {args.blocks} blocks", flush=True)


if __name__ == "__main__":
    main()

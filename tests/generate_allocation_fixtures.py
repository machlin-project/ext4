#!/usr/bin/env python3
"""Create independently allocated unwritten extents with nonzero backing bytes."""

import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil
import subprocess

from generate_fixtures import resolve_tools


def digest(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--fixtures", type=Path, required=True)
    parser.add_argument("--tools-root", type=Path)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    tools = resolve_tools(args.tools_root)
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    records = []
    for name in ("ext4-1k.img", "ext4-4k.img", "ext4-no-checksum.img", "ext4-checksum-seed.img"):
        source = (args.fixtures / name).resolve()
        image = output / f"unwritten-{name}"
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
        block_size = int(re.search(r"^Block size:\s+(\d+)$", header, re.M)[1])
        count = 128
        run([tools["debugfs"], "-w", "-R", f"fallocate /empty 0 {count - 1}", image])
        run([tools["debugfs"], "-w", "-R", f"set_inode_field /empty size {count * block_size}", image])
        blocks = []
        for logical in range(count):
            mapping = run([tools["debugfs"], "-R", f"bmap /empty {logical}", image])
            match = re.fullmatch(r"\s*(\d+) \(uninit\)\s*", mapping)
            if not match:
                raise RuntimeError(f"missing unwritten mapping: {mapping!r}")
            blocks.append(int(match[1]))
        if len(set(blocks)) != count or any(block <= 0 or (block + 1) * block_size > image.stat().st_size for block in blocks):
            raise RuntimeError("invalid or duplicate independently allocated block")
        with image.open("r+b") as stream:
            for logical, block in enumerate(blocks):
                stream.seek(block * block_size)
                stream.write(bytes([(logical * 11 + 165) % 255 + 1]) * block_size)
        run([tools["debugfs"], "-R", "stat /empty", image])
        run([tools["e2fsck"], "-fn", image])
        if digest(source) != record["source_sha256"]:
            raise RuntimeError("fixture source changed")
        record.update(block_size=block_size, unwritten_blocks=blocks,
                      sha256=digest(image), passed=True)
        save()
        print(f"PASS {image.name}: {count} unwritten blocks with nonzero backing bytes", flush=True)


if __name__ == "__main__":
    main()

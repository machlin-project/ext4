#!/usr/bin/env python3
"""Inspect bounded preallocation zeroing with independent filesystem utilities."""

import argparse
import json
from pathlib import Path
import re
import subprocess

from check_index_write import entries
from check_namespace import inode_fields
from check_orphans import accounting, digest
from check_writes import encoded_time
from generate_fixtures import resolve_tools

DATA_BLOCKS = 269
OLD_SIZE_TAIL = 17
WRITE_OFFSET = 17
PREALLOC_TAIL = 73
HOLE_FIRST = 131
HOLE_BLOCKS = 3
SECONDS = 1700000120
SECTOR_BYTES = 512
WRITTEN = b"newly exposed data!"
ATTRIBUTE_NAME = "user.growth-new"
ATTRIBUTE_VALUE = bytes((7, 0, 128, 255, 1, 9)) + bytes(294)
STATES = ("write", "truncate")


def data_byte(index):
    return (11 + index * 13 + index // 257) & 255


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--fixtures", type=Path, required=True)
    parser.add_argument("--exports", type=Path, required=True)
    parser.add_argument("--tools-root", type=Path)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    tools = resolve_tools(args.tools_root)
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    exports = args.exports.resolve()
    sources = [args.fixtures.resolve() / image.name.removeprefix("growth-write-")
               for image in sorted(exports.glob("growth-write-*.img"))]
    wanted_exports = {f"growth-{state}-{source.name}" for source in sources for state in STATES}
    if not sources or {image.name for image in exports.glob("*.img")} != wanted_exports:
        raise RuntimeError("Expected exactly two growth states per fixture")
    records = []

    def save():
        (output / "report.json").write_text(json.dumps(records, indent=2) + "\n")

    for source in sources:
        images = {state: exports / f"growth-{state}-{source.name}" for state in STATES}
        protected = {path: digest(path) for path in (source, *images.values())}
        record = dict(source=str(source), input_sha256={str(k): v for k, v in protected.items()},
                      commands=[], states={})
        records.append(record)

        def run(command, raw=False):
            done = subprocess.run([str(part) for part in command], capture_output=True, timeout=60)
            record["commands"].append(dict(command=[str(part) for part in command],
                                           status=done.returncode,
                                           stdout=done.stdout.decode("utf-8", "backslashreplace"),
                                           stderr=done.stderr.decode("utf-8", "backslashreplace")))
            save()
            done.check_returncode()
            return done.stdout if raw else done.stdout.decode("utf-8")

        counts = accounting(run([tools["dumpe2fs"], "-h", source]))
        block_size = counts["Block size"]
        sectors = block_size // SECTOR_BYTES
        original = inode_fields(run([tools["debugfs"], "-R", "stat /empty", source]))
        root = inode_fields(run([tools["debugfs"], "-R", "stat /", source]))
        names = entries(run([tools["debugfs"], "-R", "ls -p /", source], raw=True))
        if original is None or original["size"] != 0 or original["blocks"] != 0:
            raise RuntimeError("Expected an original empty file")
        offset = DATA_BLOCKS * block_size + WRITE_OFFSET
        length = offset + len(WRITTEN)
        expected = bytearray(length)
        expected[:block_size + OLD_SIZE_TAIL] = bytes(data_byte(i)
                                                    for i in range(block_size + OLD_SIZE_TAIL))

        for state, image in images.items():
            after = accounting(run([tools["dumpe2fs"], "-h", image]))
            inode = inode_fields(run([tools["debugfs"], "-R", "stat /empty", image]))
            after_root = inode_fields(run([tools["debugfs"], "-R", "stat /", image]))
            after_names = entries(run([tools["debugfs"], "-R", "ls -p /", image], raw=True))
            if inode is None or inode["blocks"] % sectors:
                raise RuntimeError("Missing inode or fractional block accounting")
            wanted_inode = dict(original, size=length, blocks=inode["blocks"], mode=0o640,
                                mtime=encoded_time(SECONDS + 10, 0),
                                ctime=encoded_time(SECONDS + 11, 0))
            wanted_counts = dict(counts)
            wanted_counts["Free blocks"] -= inode["blocks"] // sectors
            if inode != wanted_inode or after != wanted_counts:
                raise RuntimeError("Unexpected metadata or allocation accounting")
            if after_names != names or after_root != root:
                raise RuntimeError("Growth changed the namespace")
            extracted = output / f"{image.stem}.empty"
            run([tools["debugfs"], "-R", f"dump /empty {json.dumps(str(extracted))}", image])
            contents = bytearray(expected)
            if state == "write":
                contents[offset:] = WRITTEN
            if extracted.read_bytes() != contents:
                raise RuntimeError("Incorrect zeroed gap or damaged visible prefix")
            listing = run([tools["debugfs"], "-R", "ea_list /empty", image])
            attrs = re.findall(r"^  (.+?) \((\d+)\)(?: =.*)?$", listing, re.M)
            if attrs != [(ATTRIBUTE_NAME, str(len(ATTRIBUTE_VALUE)))]:
                raise RuntimeError("Incorrect CREATE/REMOVE transition")
            attribute = output / f"{image.stem}.attribute"
            run([tools["debugfs"], "-R",
                 f"ea_get -r -f {json.dumps(str(attribute))} /empty {ATTRIBUTE_NAME}", image])
            if attribute.read_bytes() != ATTRIBUTE_VALUE:
                raise RuntimeError("Attribute value changed")
            for logical in range(HOLE_FIRST, HOLE_FIRST + HOLE_BLOCKS):
                if int(run([tools["debugfs"], "-R", f"bmap /empty {logical}", image]).strip()) != 0:
                    raise RuntimeError("Zeroing allocated a sparse hole")
            physical = int(run([tools["debugfs"], "-R", f"bmap /empty {DATA_BLOCKS}", image]).strip())
            if physical <= 0:
                raise RuntimeError("Missing retained final data block")
            with image.open("rb") as stream:
                within = length % block_size
                stream.seek(physical * block_size + within)
                tail = stream.read(PREALLOC_TAIL - within)
            if tail != bytes(data_byte(DATA_BLOCKS * block_size + i)
                             for i in range(within, PREALLOC_TAIL)):
                raise RuntimeError("Changed hidden bytes after the requested end")
            payload = output / f"{image.stem}.payload"
            run([tools["debugfs"], "-R", f"dump /payload.bin {json.dumps(str(payload))}", image])
            if payload.read_bytes() != bytes((i * 17 + 23) & 255 for i in range(200000)):
                raise RuntimeError("Growth damaged another file")
            run([tools["e2fsck"], "-fn", image])
            record["states"][state] = dict(image=str(image), passed=True,
                                            inode=inode, accounting=after)
            save()
        if any(digest(path) != before for path, before in protected.items()):
            raise RuntimeError("Independent inspection changed an input")
        record["passed"] = True
        save()
        print(f"PASS {source.name}: write/truncate zeros, sparse gap, attrs, tail and e2fsck",
              flush=True)


if __name__ == "__main__":
    main()

#!/usr/bin/env python3
"""Independently inspect large writes, one-time attributes and final inode release."""

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
DATA_TAIL = 43
DATA_OFFSET = 17
SECONDS = 1700000090
SECTOR_BYTES = 512
ATTRIBUTE_NAME = "user.partial-write"
ATTRIBUTE_VALUE = bytes((0, 255, 1, 128, 2, 3))
STATES = ("large", "overwrite", "released")


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
    sources = [args.fixtures.resolve() / image.name.removeprefix("large-")
               for image in sorted(exports.glob("large-*.img"))]
    expected_exports = {f"{state}-{source.name}" for source in sources for state in STATES}
    if not sources or {image.name for image in exports.glob("*.img")} != expected_exports:
        raise RuntimeError("Expected exactly three export states per fixture")
    records = []

    def save():
        (output / "report.json").write_text(json.dumps(records, indent=2) + "\n")

    for source in sources:
        images = {state: exports / f"{state}-{source.name}" for state in STATES}
        protected = {path: digest(path) for path in (source, *images.values())}
        record = dict(source=str(source), input_sha256={str(k): v for k, v in protected.items()},
                      commands=[], states={})
        records.append(record)
        save()

        def run(command, raw=False):
            done = subprocess.run([str(part) for part in command], capture_output=True,
                                  timeout=60)
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
            raise RuntimeError("Expected the original empty file")
        payload = bytes((index * 17 + 23) & 255 for index in range(200000))
        data_length = DATA_BLOCKS * block_size + DATA_TAIL
        data = bytearray(DATA_OFFSET) + bytearray((7 + index * 29 + index // 251) & 255
                                                 for index in range(data_length))

        for state, image in images.items():
            after = accounting(run([tools["dumpe2fs"], "-h", image]))
            inode = inode_fields(run([tools["debugfs"], "-R", "stat /empty", image]))
            after_root = inode_fields(run([tools["debugfs"], "-R", "stat /", image]))
            after_names = entries(run([tools["debugfs"], "-R", "ls -p /", image], raw=True))
            wanted_names = dict(names)
            wanted_root = root
            wanted_counts = dict(counts)
            if state == "released":
                del wanted_names[b"empty"]
                wanted_root = dict(root, mtime=encoded_time(SECONDS + 1, 0),
                                   ctime=encoded_time(SECONDS + 1, 0))
                wanted_counts["Free inodes"] += 1
                if inode is not None:
                    raise RuntimeError("Released inode is still reachable")
            else:
                if inode is None or inode["blocks"] % sectors != 0:
                    raise RuntimeError("Missing file or fractional block accounting")
                wanted_inode = dict(original, size=len(data), blocks=inode["blocks"], mode=0o640,
                                    mtime=encoded_time(SECONDS, 0),
                                    ctime=encoded_time(SECONDS + 1, 0))
                if inode != wanted_inode:
                    raise RuntimeError(f"Changed unrelated inode metadata in {image.name}")
                wanted_counts["Free blocks"] -= inode["blocks"] // sectors
                extracted = output / f"{image.stem}.empty"
                run([tools["debugfs"], "-R", f"dump /empty {json.dumps(str(extracted))}", image])
                expected = bytearray(data)
                if state == "overwrite":
                    expected[DATA_OFFSET] ^= 255
                if extracted.read_bytes() != expected:
                    raise RuntimeError(f"Incorrect large-write contents in {image.name}")
                listing = run([tools["debugfs"], "-R", "ea_list /empty", image])
                attributes = re.findall(r"^  (.+?) \((\d+)\)(?: =.*)?$", listing, re.M)
                wanted = [(ATTRIBUTE_NAME, str(len(ATTRIBUTE_VALUE)))] if state == "large" else []
                if attributes != wanted:
                    raise RuntimeError(f"Incorrect one-time attribute transition in {image.name}")
                if state == "large":
                    value = output / f"{image.stem}.attribute"
                    run([tools["debugfs"], "-R",
                         f"ea_get -r -f {json.dumps(str(value))} /empty {ATTRIBUTE_NAME}", image])
                    if value.read_bytes() != ATTRIBUTE_VALUE:
                        raise RuntimeError("Attribute value changed across data batches")
            if after_names != wanted_names or after_root != wanted_root:
                raise RuntimeError(f"Unexpected namespace change in {image.name}")
            if after != wanted_counts:
                raise RuntimeError(f"Independent allocation accounting differs: {after} != {wanted_counts}")
            extracted = output / f"{image.stem}.payload"
            run([tools["debugfs"], "-R", f"dump /payload.bin {json.dumps(str(extracted))}", image])
            if extracted.read_bytes() != payload:
                raise RuntimeError("Large write damaged another file")
            run([tools["e2fsck"], "-fn", image])
            record["states"][state] = dict(image=str(image), passed=True, inode=inode, accounting=after)
            save()
        if any(digest(path) != before for path, before in protected.items()):
            raise RuntimeError("Independent inspection changed an input")
        record["passed"] = True
        save()
        print(f"PASS {source.name}: large/overwrite/released contents, xattrs, accounting and e2fsck",
              flush=True)


if __name__ == "__main__":
    main()

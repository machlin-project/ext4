#!/usr/bin/env python3
"""Independently check writes into preallocation without free metadata space."""

import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess

from check_index_write import entries
from check_namespace import inode_fields
from check_orphans import accounting, digest
from generate_fixtures import resolve_tools

EXTENTS = 4
FULL_EXTENT_BLOCKS = 2
LARGE_EXTENT_BLOCKS = 69
SECONDS = 1700000160
SECTOR_BYTES = 512
EXTENT_HEADER_BYTES = 12
EXTENT_RECORD_BYTES = 12


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--fixtures", type=Path, required=True)
    parser.add_argument("--exports", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--tools-root", type=Path)
    parser.add_argument("--full", action="store_true")
    parser.add_argument("--external", action="store_true", help="a full external extent leaf, with --full")
    args = parser.parse_args()
    if args.external and not args.full:
        parser.error("--external requires --full")
    tools = resolve_tools(args.tools_root)
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    records = []
    images = sorted(args.exports.resolve().glob("range-capacity-written-*.img"))
    if not images:
        raise RuntimeError("No capacity exports")
    for written in images:
        name = written.name.removeprefix("range-capacity-written-")
        source = args.fixtures.resolve() / name
        reserved = written.with_name("range-capacity-reserved-" + name)
        protected = {path: digest(path) for path in (source, reserved, written)}
        commands = []

        def run(command, raw=False):
            done = subprocess.run([str(part) for part in command], capture_output=True, timeout=90)
            commands.append(dict(command=[str(part) for part in command], status=done.returncode,
                                 stdout=done.stdout.decode("utf-8", "backslashreplace"),
                                 stderr=done.stderr.decode("utf-8", "backslashreplace")))
            if done.returncode:
                raise RuntimeError(f"Independent capacity command failed: {command}")
            return done.stdout if raw else done.stdout.decode("utf-8")

        def stat(image, path):
            result = inode_fields(run([tools["debugfs"], "-R", f"stat {path}", image]))
            if result is None:
                raise RuntimeError(f"Missing capacity object {path}")
            return result

        def contents(image, path):
            dest = output / f"{image.stem}-{path.removeprefix('/')}.data"
            run([tools["debugfs"], "-R", f"dump {path} {json.dumps(str(dest))}", image])
            return dest.read_bytes()

        def neighbors(image):
            return {path: dict(inode=stat(image, path), sha256=hashlib.sha256(contents(image, path)).hexdigest())
                    for path in ("/filler", "/target")}

        try:
            old_inode = stat(source, "/empty")
            old_root = stat(source, "/")
            old_names = entries(run([tools["debugfs"], "-R", "ls -p /", source], raw=True))
            old_neighbors = neighbors(source)
            old_counts = accounting(run([tools["dumpe2fs"], "-h", source]))
            block_size = old_counts["Block size"]
            blocks = FULL_EXTENT_BLOCKS if args.full else LARGE_EXTENT_BLOCKS
            # One two-block reservation crosses the fixture's physical gap.
            runs = (block_size - EXTENT_HEADER_BYTES) // EXTENT_RECORD_BYTES - 1 if args.external else EXTENTS
            owned_blocks = runs * blocks + int(args.external)
            sectors = block_size // SECTOR_BYTES
            length = (runs * (blocks + 1) - 1) * block_size
            expected_counts = dict(old_counts)
            if old_counts["Free blocks"] != 0:
                raise RuntimeError("Expected a physically full source")
            if not args.full or args.external:
                released = owned_blocks - (8 if args.external else 0)
                filler = bytearray(contents(source, "/filler"))
                filler[:released * block_size] = bytes(released * block_size)
                old_neighbors["/filler"]["sha256"] = hashlib.sha256(filler).hexdigest()
                old_filler = old_neighbors["/filler"]["inode"]
                old_filler.update(blocks=old_filler["blocks"] - released * sectors,
                                  mode=0o640, mtime=(SECONDS, 0), ctime=(SECONDS + 1, 0))
            expected_inode = dict(old_inode, size=length, blocks=owned_blocks * sectors,
                                  mode=0o640, mtime=(SECONDS, 0), ctime=(SECONDS + 1, 0))
            reserved_map = None
            for state, image in (("reserved", reserved), ("written", written)):
                record = dict(image=str(image), input_sha256=protected[image], source=str(source),
                              block_size=block_size, capacity_state=state, commands=commands)
                records.append(record)
                expected = bytearray(length)
                if state == "written":
                    for index in range(runs):
                        expected[(index * (blocks + 1) + 1) * block_size + 7] = (0xd1 + index) & 255
                observed = contents(image, "/empty")
                inode = stat(image, "/empty")
                header = run([tools["dumpe2fs"], "-h", image])
                counts = accounting(header)
                reserved_count = re.search(r"^Reserved block count:\s+(\d+)$", header, re.M)
                if reserved_count is None or int(reserved_count[1]) != 0:
                    raise RuntimeError("Capacity write changed the reserved-space policy")
                if observed != expected or inode != expected_inode or counts != expected_counts:
                    raise RuntimeError("Capacity bytes, metadata or allocation counts disagree")
                if (stat(image, "/") != old_root or neighbors(image) != old_neighbors or
                        entries(run([tools["debugfs"], "-R", "ls -p /", image], raw=True)) != old_names):
                    raise RuntimeError("Capacity write changed unrelated objects")
                if args.full:
                    reserve = stat(image, "/reserve")
                    wanted = dict(stat(source, "/reserve"), size=0, blocks=0, mode=0o640,
                                  mtime=(SECONDS, 0), ctime=(SECONDS + 1, 0))
                    if reserve != wanted or contents(image, "/reserve"):
                        raise RuntimeError("Capacity setup freed an incorrect reserve")
                elif stat(image, "/reserve") != stat(source, "/reserve") or contents(image, "/reserve") != contents(source, "/reserve"):
                    raise RuntimeError("Large capacity setup changed the retained reserve")
                physical_blocks = []
                for logical in range(length // block_size + 1):
                    mapping = run([tools["debugfs"], "-R", f"bmap /empty {logical}", image]).strip()
                    matched = re.fullmatch(r"(\d+)( \(uninit\))?", mapping)
                    hole = logical == length // block_size or logical % (blocks + 1) == blocks
                    # Only the second block of a physically split two-block
                    # reservation is written. Its untouched one-block neighbor
                    # stays unwritten because no metadata allocation is needed.
                    untouched_fragment = (state == "written" and args.external and not hole and
                                          logical % (blocks + 1) == 0 and
                                          reserved_map[logical + 1] != reserved_map[logical] + 1)
                    unwritten = state == "reserved" or untouched_fragment
                    if (matched is None or (int(matched[1]) == 0) != hole or
                            not hole and bool(matched[2]) != unwritten):
                        raise RuntimeError("Unwritten conversion changed the preallocated block map")
                    physical_blocks.append(int(matched[1]))
                if state == "reserved":
                    reserved_map = physical_blocks
                elif physical_blocks != reserved_map:
                    raise RuntimeError("Writing into preallocation relocated its backing blocks")
                run([tools["e2fsck"], "-fn", image])
                record.update(passed=True, accounting=counts,
                              verified_range=dict(inode=inode, attributes=[], data_size=len(observed),
                                                  data_sha256=hashlib.sha256(observed).hexdigest()))
                record["commands"] = list(commands)
                commands.clear()
                print(f"PASS {image.name}: exact bytes/maps, stable allocation and independent fsck", flush=True)
            if any(digest(path) != value for path, value in protected.items()):
                raise RuntimeError("Capacity checking modified an input")
        finally:
            (output / "report.json").write_text(json.dumps(records, indent=2) + "\n")


if __name__ == "__main__":
    main()

#!/usr/bin/env python3
"""Independently check writes into preallocation without free metadata space."""

import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil
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
    parser.add_argument("--keep-size", action="store_true", help="reservation starts with zero EOF")
    parser.add_argument("--faults", action="store_true", help="one large KEEP_SIZE growth and its pending journal")
    parser.add_argument("--recover", type=Path)
    args = parser.parse_args()
    if args.external and not args.full:
        parser.error("--external requires --full")
    if args.faults and (not args.keep_size or args.full or args.recover is None):
        parser.error("--faults requires --keep-size and --recover, without --full")
    tools = resolve_tools(args.tools_root)
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    records = []
    prefix = "range-growth-" if args.keep_size else "range-capacity-"
    result_prefix = prefix + ("after-" if args.faults else "written-")
    images = sorted(args.exports.resolve().glob(result_prefix + "*.img"))
    if not images:
        raise RuntimeError("No capacity exports")
    for written in images:
        name = written.name.removeprefix(result_prefix)
        source = args.fixtures.resolve() / name
        reserved = written.with_name(prefix + ("before-" if args.faults else "reserved-") + name)
        protected = {path: digest(path) for path in (source, reserved, written)}
        if args.faults:
            for state in ("pending", "uncommitted"):
                candidate = written.with_name(prefix + state + "-" + name)
                protected[candidate] = digest(candidate)
            protected[args.recover.resolve()] = digest(args.recover)
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
            states = [("reserved", reserved, None), ("written", written, None)]
            if args.faults:
                for state in ("pending", "uncommitted"):
                    candidate = written.with_name(prefix + state + "-" + name)
                    for engine in ("core", "oracle"):
                        copy = output / f"{engine}-{candidate.name}"
                        shutil.copyfile(candidate, copy)
                        states.append(("written" if state == "pending" else "reserved", copy, engine))
            for state, image, engine in states:
                if engine == "core":
                    replay = run([args.recover.resolve(), "--write", image])
                    transactions = re.search(r"transactions=(\d+)", replay)
                    if transactions is None or (state == "written" and int(transactions[1]) == 0):
                        raise RuntimeError("Growth did not replay its committed data/EOF transaction")
                elif engine == "oracle":
                    run([tools["e2fsck"], "-y", "-E", "journal_only", image])
                record = dict(image=str(image), input_sha256=digest(image), source=str(source),
                              block_size=block_size, capacity_state=state, commands=commands)
                records.append(record)
                data_length = length
                if args.keep_size:
                    data_length = 0 if state == "reserved" else (
                        (blocks - 1) * block_size + 8 if args.faults else length - block_size + 8)
                expected = bytearray(data_length)
                wanted_inode = dict(expected_inode, size=data_length)
                if state == "written":
                    for index in range(1 if args.faults else runs):
                        block = index * (blocks + 1) + (blocks - 1 if args.keep_size else 1)
                        expected[block * block_size + 7] = 0xe3 if args.faults else (0xd1 + index) & 255
                    if args.faults:
                        wanted_inode.update(mtime=(SECONDS + 9, 0), ctime=(SECONDS + 10, 0))
                observed = contents(image, "/empty")
                inode = stat(image, "/empty")
                header = run([tools["dumpe2fs"], "-h", image])
                counts = accounting(header)
                reserved_count = re.search(r"^Reserved block count:\s+(\d+)$", header, re.M)
                if reserved_count is None or int(reserved_count[1]) != 0:
                    raise RuntimeError("Capacity write changed the reserved-space policy")
                if observed != expected or inode != wanted_inode or counts != expected_counts:
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
                    unwritten = state == "reserved" or untouched_fragment or (
                        args.faults and logical // (blocks + 1) != 0)
                    if (matched is None or (int(matched[1]) == 0) != hole or
                            not hole and bool(matched[2]) != unwritten):
                        raise RuntimeError("Unwritten conversion changed the preallocated block map")
                    physical_blocks.append(int(matched[1]))
                if reserved_map is None:
                    reserved_map = physical_blocks
                elif physical_blocks != reserved_map:
                    raise RuntimeError("Writing into preallocation relocated its backing blocks")
                run([tools["e2fsck"], "-fn", image])
                record.update(passed=True, accounting=counts,
                              verified_range=dict(inode=inode, attributes=[], data_size=len(observed),
                                                  data_sha256=hashlib.sha256(observed).hexdigest()))
                if args.keep_size:
                    record["keep_size_growth"] = True
                if args.faults and image == written:
                    pending = written.with_name(prefix + "pending-" + name)
                    record.update(pending=str(pending), pending_sha256=protected[pending],
                                  recovered_outcome="new")
                if engine == "core":
                    stable = digest(image)
                    run([args.recover.resolve(), "--write", image])
                    if digest(image) != stable:
                        raise RuntimeError("Repeated growth recovery changes the image")
                if engine is not None:
                    record["recovery_engine"] = engine
                record["commands"] = list(commands)
                commands.clear()
                print(f"PASS {image.name}: exact bytes/maps, stable allocation and independent fsck", flush=True)
            if any(digest(path) != value for path, value in protected.items()):
                raise RuntimeError("Capacity checking modified an input")
        finally:
            (output / "report.json").write_text(json.dumps(records, indent=2) + "\n")


if __name__ == "__main__":
    main()

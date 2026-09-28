#!/usr/bin/env python3
"""Check reserved mapping capacity, partial EOF publication and independent replay."""

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

LARGE_EXTENT_BLOCKS = 69
SECONDS = 1700000160
SECTOR_BYTES = 512
EXTENT_HEADER_BYTES = 12
EXTENT_RECORD_BYTES = 12
MIN_BLOCK_SIZE = 1024


def extent_map(text):
    result = {}
    for line in text.splitlines():
        if not line.strip() or line.startswith("Level Entries"):
            continue
        match = re.fullmatch(
            r"\s*(\d+)/\s*(\d+)\s+\d+/\s*\d+\s+(\d+)\s+-\s+(\d+)\s+"
            r"(\d+)(?:\s+-\s+(\d+))?\s+(\d+)(?:\s+(Uninit))?\s*", line)
        if match is None:
            raise RuntimeError(f"Unexpected independent extent record: {line}")
        level, depth, first, last, physical = map(int, match.groups()[:5])
        if level != depth:
            continue
        length = int(match[7])
        if last - first + 1 != length or int(match[6] or physical) != physical + length - 1:
            raise RuntimeError("Inconsistent independent extent bounds")
        for index in range(length):
            logical = first + index
            if logical in result:
                raise RuntimeError("Overlapping independent extents")
            result[logical] = (physical + index, bool(match[8]))
    if not result:
        raise RuntimeError("Missing independently decoded data extents")
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--fixtures", type=Path, required=True)
    parser.add_argument("--exports", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--tools-root", type=Path)
    parser.add_argument("--tree", action="store_true")
    parser.add_argument("--faults", action="store_true")
    parser.add_argument("--recover", type=Path)
    args = parser.parse_args()
    if args.faults and (args.tree or args.recover is None):
        parser.error("--faults requires --recover and cannot select --tree")
    tools = resolve_tools(args.tools_root)
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    after_prefix = "reservation-after-" if args.faults else "reservation-written-"
    before_prefix = "reservation-before-" if args.faults else "reservation-reserved-"
    images = sorted(args.exports.resolve().glob(after_prefix + "*.img"))
    if not images:
        raise RuntimeError("No reservation exports")
    records = []
    for after in images:
        name = after.name.removeprefix(after_prefix)
        before = after.with_name(before_prefix + name)
        source = args.fixtures.resolve() / name
        protected = {path: digest(path) for path in (source, before, after)}
        if args.faults:
            for state in ("pending", "uncommitted"):
                path = after.with_name(f"reservation-{state}-{name}")
                protected[path] = digest(path)
            protected[args.recover.resolve()] = digest(args.recover)
        commands = []

        def run(command, raw=False):
            done = subprocess.run([str(x) for x in command], capture_output=True, timeout=90)
            commands.append(dict(command=[str(x) for x in command], status=done.returncode,
                                 stdout=done.stdout.decode("utf-8", "backslashreplace"),
                                 stderr=done.stderr.decode("utf-8", "backslashreplace")))
            done.check_returncode()
            return done.stdout if raw else done.stdout.decode("utf-8")

        def stat(image, path):
            value = inode_fields(run([tools["debugfs"], "-R", f"stat {path}", image]))
            if value is None:
                raise RuntimeError(f"Missing object {path}")
            return value

        def contents(image, path):
            target = output / f"{image.stem}-{path.removeprefix('/')}.data"
            run([tools["debugfs"], "-R", f"dump {path} {json.dumps(str(target))}", image])
            return target.read_bytes()

        try:
            header = run([tools["dumpe2fs"], "-h", source])
            block_size = int(re.search(r"^Block size:\s+(\d+)$", header, re.M)[1])
            blocks = (3 if block_size == MIN_BLOCK_SIZE else 2) if args.tree else LARGE_EXTENT_BLOCKS
            runs = ((block_size - EXTENT_HEADER_BYTES) // EXTENT_RECORD_BYTES - 2) if args.tree else 3
            owned = runs * blocks + int(args.tree)
            sectors = block_size // SECTOR_BYTES
            old_inode = stat(source, "/empty")
            old_filler = stat(source, "/filler")
            filler = bytearray(contents(source, "/filler"))
            filler[:owned * block_size] = bytes(owned * block_size)
            wanted_filler = dict(old_filler, blocks=old_filler["blocks"] - owned * sectors,
                                 mode=0o640, mtime=(SECONDS, 0), ctime=(SECONDS + 1, 0))
            neighbors = {path: stat(source, path) for path in ("/", "/target", "/reserve", "/linear", "/indexed")}
            neighbor_bytes = {path: contents(source, path) for path in ("/target", "/reserve")}
            names = entries(run([tools["debugfs"], "-R", "ls -p /", source], raw=True))
            counts = accounting(header)
            states = [("before", before, None), ("after", after, None)]
            if args.faults:
                for state in ("pending", "uncommitted"):
                    candidate = after.with_name(f"reservation-{state}-{name}")
                    for engine in ("core", "oracle"):
                        copy = output / f"{engine}-{candidate.name}"
                        shutil.copyfile(candidate, copy)
                        states.append(("after" if state == "pending" else "before", copy, engine))
            baseline_map = None
            for state, image, engine in states:
                record = dict(image=str(image), source=str(source), block_size=block_size,
                              reservation_state=state, capacity_state="written" if state == "after" else "reserved",
                              commands=[])
                records.append(record)
                if engine == "core":
                    replay = run([args.recover.resolve(), "--write", image])
                    transactions = re.search(r"transactions=(\d+)", replay)
                    if transactions is None or (state == "after" and int(transactions[1]) == 0):
                        raise RuntimeError("Missing committed EOF-boundary replay")
                elif engine == "oracle":
                    run([tools["e2fsck"], "-fy", "-E", "journal_only", image])
                if args.faults:
                    length = (blocks + 1 if state == "after" else blocks // 2) * block_size + 8
                    delta = 19 if state == "after" else 9
                else:
                    length = ((runs - 1) * (blocks + 1) + blocks // 2) * block_size + 8 if state == "after" else 0
                    delta = 9 if state == "after" else 0
                expected = bytearray(length)
                if args.faults:
                    expected[7] = 0xa1
                    expected[(blocks // 2) * block_size + 7] = 0xd1
                    if state == "after":
                        expected[-1] = 0xe3
                elif state == "after":
                    for index in range(runs):
                        expected[index * (blocks + 1) * block_size + 7] = (0xa1 + index) & 255
                        expected[(index * (blocks + 1) + blocks // 2) * block_size + 7] = (0xd1 + index) & 255
                wanted_inode = dict(old_inode, size=length, blocks=owned * sectors, mode=0o640,
                                    mtime=(SECONDS + delta, 0), ctime=(SECONDS + delta + 1, 0))
                inode = stat(image, "/empty")
                if contents(image, "/empty") != expected or inode != wanted_inode:
                    raise RuntimeError("Reservation bytes, EOF or attributes disagree")
                observed_counts = accounting(run([tools["dumpe2fs"], "-h", image]))
                if observed_counts != counts or observed_counts["Free blocks"] != 0:
                    raise RuntimeError("Reservation changed allocation counters")
                if stat(image, "/filler") != wanted_filler or contents(image, "/filler") != filler:
                    raise RuntimeError("Reservation setup changed the wrong filler blocks")
                if (any(stat(image, path) != value for path, value in neighbors.items()) or
                        any(contents(image, path) != value for path, value in neighbor_bytes.items()) or
                        entries(run([tools["debugfs"], "-R", "ls -p /", image], raw=True)) != names):
                    raise RuntimeError("Reservation changed an unrelated object")
                mapping = extent_map(run([tools["debugfs"], "-R", "extents /empty", image]))
                expected_blocks = {index * (blocks + 1) + part for index in range(runs) for part in range(blocks)}
                if set(mapping) != expected_blocks:
                    raise RuntimeError("Reservation changed the logical block map")
                physical = {logical: value[0] for logical, value in mapping.items()}
                if baseline_map is None:
                    baseline_map = physical
                elif physical != baseline_map:
                    raise RuntimeError("Reservation relocated existing data")
                boundary = (length + block_size - 1) // block_size
                if any(unwritten != (logical >= boundary) for logical, (_, unwritten) in mapping.items()):
                    raise RuntimeError("Initialized/unwritten boundary disagrees with committed EOF")
                run([tools["e2fsck"], "-fn", image])
                if engine == "core":
                    stable = digest(image)
                    run([args.recover.resolve(), "--write", image])
                    if digest(image) != stable:
                        raise RuntimeError("Repeated boundary recovery changed the image")
                record.update(passed=True, input_sha256=digest(image), accounting=observed_counts,
                              verified_range=dict(inode=inode, attributes=[], data_size=len(expected),
                                                  data_sha256=hashlib.sha256(expected).hexdigest()))
                if args.faults and image == after:
                    pending = after.with_name("reservation-pending-" + name)
                    record.update(pending=str(pending), pending_sha256=protected[pending], recovered_outcome="new")
                if engine:
                    record["recovery_engine"] = engine
                record["commands"] = list(commands)
                commands.clear()
                print(f"PASS {image.name}: exact EOF, bytes, extent flags, unchanged physical map and fsck", flush=True)
            if any(digest(path) != value for path, value in protected.items()):
                raise RuntimeError("Reservation checking modified an input or recovery binary")
        except Exception as error:
            if records and records[-1].get("source") == str(source):
                records[-1].update(passed=False, error=str(error))
            else:
                records.append(dict(source=str(source), passed=False, error=str(error), commands=[]))
            raise
        finally:
            if records and commands:
                records[-1]["commands"] += commands
            (output / "report.json").write_text(json.dumps(records, indent=2) + "\n")


if __name__ == "__main__":
    main()

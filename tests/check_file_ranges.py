#!/usr/bin/env python3
"""Check reserved/punched ranges and their interrupted journals with e2fsprogs."""

import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil
import subprocess

from check_index_write import entries
from check_namespace import inode_fields, INODE_EXTENTS
from check_orphans import accounting, digest
from check_writes import encoded_time
from generate_fixtures import resolve_tools

BLOCKS = 269
SEED_BLOCKS = 21
TAIL = 73
SECONDS = 1700000160
SECTOR_BYTES = 512
ATTRIBUTE = bytes((7, 255, 0, 128, 31)) + bytes(295)
STATES = ("reserved", "grown", "punched", "reallocated", "released", "reserve-atomic", "punch-atomic")


def pattern(length):
    return bytes((index * 13 + index // 257 + 11) & 255 for index in range(length))


def model(state, block_size):
    if state.endswith("atomic"):
        data = bytearray(pattern(8 * block_size + TAIL))
        mapped = set(range(9))
        if state == "reserve-atomic":
            mapped.update(range(32, 39))
        else:
            data[block_size - 7:4 * block_size + 22] = bytes(3 * block_size + 29)
            mapped.difference_update(range(1, 4))
        return bytes(data), mapped, True, 39
    end = (BLOCKS + 1) * block_size + 17 + TAIL
    data = bytearray(end)
    data[:SEED_BLOCKS * block_size + TAIL] = pattern(SEED_BLOCKS * block_size + TAIL)
    mapped = set(range(BLOCKS + 2))
    if state == "reserved":
        return bytes(data[:SEED_BLOCKS * block_size + TAIL]), mapped, True, BLOCKS + 2
    if state in ("punched", "reallocated"):
        data[block_size - 7:BLOCKS * block_size + 13] = bytes((BLOCKS - 1) * block_size + 20)
        mapped.difference_update(range(1, BLOCKS))
    if state == "reallocated":
        data[123 * block_size + 9:123 * block_size + 54] = bytes(range(1, 46))
        mapped.add(123)
    return bytes(data), mapped, False, BLOCKS + 2


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--fixtures", type=Path, required=True)
    parser.add_argument("--exports", type=Path, required=True)
    parser.add_argument("--recover", type=Path, required=True)
    parser.add_argument("--tools-root", type=Path)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--profile", action="append", default=[],
                        help="check only this exact source filename (repeatable)")
    args = parser.parse_args()
    tools = resolve_tools(args.tools_root)
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    exports = args.exports.resolve()
    recover = args.recover.resolve()
    recover_hash = digest(recover)
    records = []

    def save():
        (output / "report.json").write_text(json.dumps(records, indent=2) + "\n")

    sources = [args.fixtures.resolve() / image.name.removeprefix("range-punched-")
               for image in sorted(exports.glob("range-punched-*.img"))]
    if args.profile:
        if set(args.profile) - {source.name for source in sources}:
            raise RuntimeError("Requested range profile is absent")
        sources = [source for source in sources if source.name in args.profile]
    if not sources:
        raise RuntimeError("No file-range exports")
    for source in sources:
        probe_command = [str(tools["debugfs"]), "-R", "stat /empty", str(source)]
        probe = subprocess.run(probe_command, capture_output=True, timeout=90, check=True)
        source_inode = inode_fields(probe.stdout.decode())
        if source_inode is None:
            raise RuntimeError("Range fixture lacks its empty file")
        states = STATES if source_inode["flags"] & INODE_EXTENTS else (
            "punched", "reallocated", "released", "punch-atomic")
        for state in states:
            image = exports / f"range-{state}-{source.name}"
            atomic = state.endswith("atomic")
            prefix = state.removesuffix("-atomic")
            before = exports / f"range-{prefix}-before-{source.name}" if atomic else source
            protected = {path: digest(path) for path in (source, before, image)}
            record = dict(image=str(image), input_sha256=digest(image), state=state,
                          source=str(source), commands=[dict(command=probe_command, status=0,
                              stdout=probe.stdout.decode(), stderr=probe.stderr.decode())])
            records.append(record)

            def run(command, raw=False):
                done = subprocess.run([str(part) for part in command], capture_output=True, timeout=90)
                record["commands"].append(dict(command=[str(part) for part in command],
                    status=done.returncode, stdout=done.stdout.decode("utf-8", "backslashreplace"),
                    stderr=done.stderr.decode("utf-8", "backslashreplace")))
                if done.returncode:
                    save()
                    done.check_returncode()
                return done.stdout if raw else done.stdout.decode("utf-8")

            def stat(candidate, name):
                return inode_fields(run([tools["debugfs"], "-R", f"stat {name}", candidate]))

            def contents(candidate, name, suffix):
                dest = output / f"{candidate.stem}.{suffix}"
                run([tools["debugfs"], "-R", f"dump {name} {json.dumps(str(dest))}", candidate])
                return dest.read_bytes()

            def snapshot(candidate):
                counts = accounting(run([tools["dumpe2fs"], "-h", candidate]))
                inode = stat(candidate, "/empty")
                names = entries(run([tools["debugfs"], "-R", "ls -p /", candidate], raw=True))
                result = dict(accounting=counts, inode=inode, root=stat(candidate, "/"),
                              names={key.hex(): value for key, value in names.items()})
                if inode is not None:
                    data = contents(candidate, "/empty", "empty")
                    result.update(data_size=len(data), data_sha256=hashlib.sha256(data).hexdigest())
                    listing = run([tools["debugfs"], "-R", "ea_list /empty", candidate])
                    result["attributes"] = re.findall(r"^  (.+?) \((\d+)\)(?: =.*)?$", listing, re.M)
                    if result["attributes"]:
                        dest = output / f"{candidate.stem}.attribute"
                        run([tools["debugfs"], "-R",
                             f"ea_get -r -f {json.dumps(str(dest))} /empty user.range", candidate])
                        result["value"] = dest.read_bytes().hex()
                result["neighbor_sha256"] = hashlib.sha256(
                    contents(candidate, "/payload.bin", "payload")).hexdigest()
                run([tools["e2fsck"], "-fn", candidate])
                return result

            try:
                old = snapshot(before)
                new = snapshot(image)
                original = stat(source, "/empty")
                base_counts = accounting(run([tools["dumpe2fs"], "-h", source]))
                block_size = base_counts["Block size"]
                sectors = block_size // SECTOR_BYTES
                data, mapped, has_attribute, logical_limit = model(state, block_size)
                wanted_counts = dict(base_counts)
                wanted_root = dict(old["root"])
                wanted_names = dict(old["names"])
                if state == "released":
                    del wanted_names[b"empty".hex()]
                    wanted_root.update(mtime=encoded_time(SECONDS + 1, 0), ctime=encoded_time(SECONDS + 1, 0))
                    wanted_counts["Free inodes"] += 1
                    if new["inode"] is not None:
                        raise RuntimeError("Released inode remains reachable")
                else:
                    inode = new["inode"]
                    if inode is None or inode["blocks"] % sectors:
                        raise RuntimeError("Missing inode or fractional block accounting")
                    wanted_inode = dict(original, mode=0o640, size=len(data), blocks=inode["blocks"],
                        mtime=encoded_time(SECONDS, 0), ctime=encoded_time(SECONDS + 1, 0))
                    if inode != wanted_inode or new["data_sha256"] != hashlib.sha256(data).hexdigest():
                        raise RuntimeError("Range data or admitted inode attributes disagree")
                    wanted_counts["Free blocks"] -= inode["blocks"] // sectors
                    wanted_attrs = [("user.range", str(len(ATTRIBUTE)))] if has_attribute else []
                    if new["attributes"] != wanted_attrs or has_attribute and new.get("value") != ATTRIBUTE.hex():
                        raise RuntimeError("Range attribute transition disagrees")
                    actual_mapped = set()
                    for logical in range(logical_limit + 1):
                        text = run([tools["debugfs"], "-R", f"bmap /empty {logical}", image]).strip()
                        physical = re.fullmatch(r"(\d+)(?: \(uninit\))?", text)
                        if physical is None:
                            raise RuntimeError(f"Unexpected independent mapping: {text}")
                        if int(physical[1]):
                            actual_mapped.add(logical)
                    if actual_mapped != mapped:
                        raise RuntimeError("Reserved or punched logical blocks disagree")
                if (new["accounting"] != wanted_counts or new["root"] != wanted_root or
                        new["names"] != wanted_names or new["neighbor_sha256"] != old["neighbor_sha256"]):
                    raise RuntimeError("Range mutation changed namespace, neighbors or allocation accounting")
                record.update(block_size=block_size, accounting=new["accounting"], verified_range=new)
                if atomic:
                    for outcome, expected in (("pending", new), ("uncommitted", old)):
                        interrupted = exports / f"range-{prefix}-{outcome}-{source.name}"
                        protected[interrupted] = digest(interrupted)
                        if outcome == "pending":
                            record.update(pending=str(interrupted), pending_sha256=protected[interrupted])
                        for engine in ("core", "oracle"):
                            candidate = output / f"{engine}-{interrupted.name}"
                            shutil.copyfile(interrupted, candidate)
                            run([recover, "--write", candidate] if engine == "core" else
                                [tools["e2fsck"], "-y", "-E", "journal_only", candidate])
                            if snapshot(candidate) != expected:
                                raise RuntimeError(f"{engine} did not recover the complete {outcome} outcome")
                            if engine == "core":
                                stable = digest(candidate)
                                run([recover, "--write", candidate])
                                if digest(candidate) != stable:
                                    raise RuntimeError("Repeated range recovery is not idempotent")
                    record.update(recovered_outcome="new", idempotent=True, uncommitted_outcome="old")
                if any(digest(path) != before_hash for path, before_hash in protected.items()) or digest(recover) != recover_hash:
                    raise RuntimeError("Range checking changed a protected input or recovery executable")
                record["passed"] = True
                print(f"PASS {image.name}: exact bytes, maps, attributes, accounting and recovery", flush=True)
            finally:
                save()


if __name__ == "__main__":
    main()

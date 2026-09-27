#!/usr/bin/env python3
"""Independently check namespace allocation, hard links and interrupted transactions."""

import argparse
import json
from pathlib import Path
import re
import shutil
import subprocess

from check_orphans import accounting, digest
from check_writes import encoded_time
from generate_fixtures import resolve_tools

UID = (1 << 32) - 3
GID = 0x81234567
PERMISSIONS = 0o2751
NAME_MAX = 255
SECTOR_SIZE = 512


def inode_fields(text):
    if not text.strip():
        return None
    result = {}
    patterns = {
        "inode": r"Inode:\s+(\d+)", "mode": r"Mode:\s+([0-7]+)",
        "size": r"(?m)^User:.*?Size:\s+(\d+)", "blocks": r"Blockcount:\s+(\d+)",
        "uid": r"User:\s+(-?\d+)", "gid": r"Group:\s+(-?\d+)",
        "links": r"Links:\s+(\d+)", "generation": r"Generation:\s+(\d+)",
        "flags": r"Flags:\s+0x([0-9a-f]+)",
    }
    for key, pattern in patterns.items():
        match = re.search(pattern, text)
        if not match:
            raise RuntimeError(f"Missing independent inode field {key}: {text!r}")
        result[key] = int(match[1], 8 if key == "mode" else 16 if key == "flags" else 10)
    result["uid"] &= 0xffffffff
    result["gid"] &= 0xffffffff
    for key in ("atime", "mtime", "ctime", "crtime"):
        match = re.search(rf"\b{key}:\s+0x([0-9a-f]+)(?::([0-9a-f]+))?", text)
        result[key] = (int(match[1], 16), int(match[2] or "0", 16)) if match else None
    return result


def creation_attributes(inode, inode_size, modified_directory=False):
    times = {"atime": (-1, 0 if inode_size == 128 else 123456789),
             "mtime": (1700000001, 0 if inode_size == 128 else 999999999),
             "ctime": (1700000002, 0 if inode_size == 128 else 42)}
    if inode_size != 128:
        times["crtime"] = (1 << 32, 987654321)
    if modified_directory:
        times["mtime"] = times["ctime"]
    if (inode["uid"], inode["gid"], inode["mode"]) != (UID, GID, PERMISSIONS):
        raise RuntimeError("Incorrect independently decoded creation ownership or mode")
    if inode["generation"] == 0 or any(inode[key] != encoded_time(*value) for key, value in times.items()):
        raise RuntimeError("Creation lost its generation or precise captured timestamps")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--fixtures", type=Path, required=True)
    parser.add_argument("--exports", type=Path, required=True)
    parser.add_argument("--recover", type=Path, required=True)
    parser.add_argument("--tools-root", type=Path)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    exports = args.exports.resolve()
    tools = resolve_tools(args.tools_root)
    images = sorted(p for p in exports.glob("*.img") if re.match(
        r"^(basic-|exhaust-|(?:append-|group-)?atomic-[012]-)", p.name))
    if not images:
        raise RuntimeError("No namespace exports")
    records = []
    for image in images:
        match = re.fullmatch(r"(append-|group-)?atomic-([012])-(.+\.img)", image.name)
        atomic = match is not None
        scenario = (match[1] or "") if atomic else ""
        operation = int(match[2]) if atomic else None
        name = match[3] if atomic else image.name.split("-", 1)[1]
        before = exports / f"{scenario}before-{name}" if scenario else args.fixtures.resolve() / name
        pending = exports / f"{scenario}pending-{operation}-{name}" if atomic else None
        uncommitted = exports / f"{scenario}uncommitted-{operation}-{name}" if atomic else None
        protected = {p: digest(p) for p in (before, image, *([pending, uncommitted] if atomic else []))}
        record = {"image": str(image), "input_sha256": protected[image], "before": str(before),
                  "before_sha256": protected[before], "commands": []}
        records.append(record)

        def save():
            (output / "report.json").write_text(json.dumps(records, indent=2) + "\n")

        def run(command, allowed=(0,)):
            done = subprocess.run([str(part) for part in command], capture_output=True, text=True, timeout=90)
            record["commands"].append({"command": [str(part) for part in command],
                                       "status": done.returncode, "stdout": done.stdout, "stderr": done.stderr})
            save()
            if done.returncode not in allowed:
                raise RuntimeError(f"Failed ({done.returncode}): {command}")
            return done.stdout

        def stat(candidate, path):
            return inode_fields(run([tools["debugfs"], "-R", f"stat {path}", candidate]))

        def names(candidate, path):
            text = run([tools["debugfs"], "-R", f"ls -p {path}", candidate])
            result = {}
            for line in text.splitlines():
                if not line.strip():
                    continue
                fields = line.split("/")
                if len(fields) != 8 or fields[5] in result:
                    raise RuntimeError(f"Invalid or duplicate independent directory entry: {line!r}")
                result[fields[5]] = int(fields[1])
            return result

        def data(candidate, path, label):
            dump = output / f"{image.stem}.{label}.data"
            run([tools["debugfs"], "-R", f"dump {path} {dump}", candidate])
            return dump.read_bytes()

        def snapshot(candidate):
            run([tools["e2fsck"], "-fn", candidate])
            header = run([tools["dumpe2fs"], "-h", candidate])
            inode_size = int(re.search(r"^Inode size:\s+(\d+)$", header, re.M)[1])
            entry = stat(candidate, "/atomic-entry")
            contents = None
            if entry is not None and atomic:
                contents = (names(candidate, "/atomic-entry") if operation == 1 else
                            data(candidate, "/atomic-entry", f"{candidate.stem}.entry").hex())
            return {"accounting": accounting(header), "inode_size": inode_size,
                    "root": stat(candidate, "/"), "hello": stat(candidate, "/hello.txt"),
                    "names": names(candidate, "/"), "entry": entry, "entry_contents": contents}

        old = snapshot(before)
        new = snapshot(image)
        block_size = new["accounting"]["Block size"]
        inode_size = new["inode_size"]
        namespace_time = encoded_time(1700000002, 0 if inode_size == 128 else 42)
        expected_root = dict(old["root"])
        expected_root.update(ctime=namespace_time, mtime=namespace_time,
                             blocks=new["root"]["blocks"], size=new["root"]["size"])
        child_blocks = 0
        inode_delta = 0
        expected_names = dict(old["names"])
        if atomic:
            entry = new["entry"]
            if old["entry"] is not None or entry is None:
                raise RuntimeError("Atomic namespace output has the wrong entry existence")
            expected_names["atomic-entry"] = entry["inode"]
            if operation in (0, 1):
                creation_attributes(entry, inode_size)
                inode_delta = 1
                child_blocks = entry["blocks"]
                expected_size = block_size if operation == 1 else 0
                if (entry["size"], entry["links"], entry["blocks"]) != (
                        expected_size, 2 if operation == 1 else 1,
                        block_size // SECTOR_SIZE if operation == 1 else 0):
                    raise RuntimeError("New inode has incorrect size, links or allocated sectors")
                if operation == 1:
                    expected_root["links"] += 1
                    if names(image, "/atomic-entry") != {".": entry["inode"], "..": new["root"]["inode"]}:
                        raise RuntimeError("New directory has incorrect dot/dotdot ownership")
            else:
                expected_target = dict(old["hello"])
                expected_target.update(links=old["hello"]["links"] + 1, ctime=namespace_time)
                if entry != expected_target or new["hello"] != entry:
                    raise RuntimeError("Hardlink lost identity or changed unselected inode attributes")
                if data(image, "/atomic-entry", "hardlink") != b"Machlin ext4\n":
                    raise RuntimeError("Hardlink contents differ from the existing inode")
        elif image.name.startswith("basic-"):
            file = stat(image, "/created")
            directory = stat(image, "/created-dir")
            child = stat(image, "/created-dir/child")
            for inode in (file, child):
                creation_attributes(inode, inode_size)
            creation_attributes(directory, inode_size, modified_directory=True)
            if len({file["inode"], directory["inode"], child["inode"]}) != 3:
                raise RuntimeError("Distinct creations reused an allocated inode")
            if stat(image, "/created-dir/alias") != file or file["links"] != 2:
                raise RuntimeError("Cross-directory hardlink identity was not preserved")
            if names(image, "/created-dir") != {".": directory["inode"], "..": new["root"]["inode"],
                                                 "alias": file["inode"], "child": child["inode"]}:
                raise RuntimeError("Nested namespace differs from the expected complete operation sequence")
            payload = bytes(block_size + 7) + bytes((index * 29 + 7) & 255 for index in range(37))
            if data(image, "/created", "created") != payload or data(image, "/created-dir/alias", "alias") != payload:
                raise RuntimeError("New file allocation or hardlink reads have wrong bytes")
            symlink = stat(image, "/hello-link")
            if stat(image, "/symlink-alias") != symlink or symlink["links"] != 2:
                raise RuntimeError("Hardlink to a symlink lost its inode identity")
            expected_names.update({"created": file["inode"], "created-dir": directory["inode"],
                                   "symlink-alias": symlink["inode"]})
            expected_root["links"] += 1
            inode_delta = 3
            child_blocks = file["blocks"] + directory["blocks"] + child["blocks"]
            if (file["blocks"], directory["blocks"], child["blocks"], child["size"]) != (
                    block_size // SECTOR_SIZE, block_size // SECTOR_SIZE, 0, 0):
                raise RuntimeError("New data or directory blocks leaked")
        else:
            inode_delta = old["accounting"]["Free inodes"]
            if inode_delta > 256 or new["accounting"]["Free inodes"] != 0:
                raise RuntimeError("Exhaustion did not consume the bounded inode pool")
            directories = 0
            allocated = set()
            for index in range(inode_delta):
                entry_name = f"node-{index:08d}".ljust(NAME_MAX, "n")
                inode = stat(image, f"/{entry_name}")
                creation_attributes(inode, inode_size)
                if inode["inode"] in allocated:
                    raise RuntimeError("Inode allocation reused a live object")
                allocated.add(inode["inode"])
                expected_names[entry_name] = inode["inode"]
                child_blocks += inode["blocks"]
                if index % 5 == 0:
                    directories += 1
                    if (inode["size"], inode["blocks"], inode["links"]) != (
                            block_size, block_size // SECTOR_SIZE, 2):
                        raise RuntimeError("Empty directory has incorrect size, blocks or links")
                    if names(image, f"/{entry_name}") != {".": inode["inode"], "..": new["root"]["inode"]}:
                        raise RuntimeError("Allocated directory lost dot/dotdot")
                elif inode["blocks"] != 0 or inode["size"] != 0 or inode["links"] != 1:
                    raise RuntimeError("Empty creation has unwanted allocation")
            expected_root["links"] += directories
            expected_names["still-links"] = old["hello"]["inode"]
            if new["hello"]["links"] != old["hello"]["links"] + 1:
                raise RuntimeError("Hardlink failed after inode exhaustion")
        if new["root"] != expected_root or new["names"] != expected_names:
            raise RuntimeError("Parent attributes or namespace changed outside the intended operation")
        consumed = new["root"]["blocks"] - old["root"]["blocks"] + child_blocks
        if consumed < 0 or consumed % (block_size // SECTOR_SIZE):
            raise RuntimeError("Invalid new allocation accounting")
        expected_counts = dict(old["accounting"])
        expected_counts["Free inodes"] -= inode_delta
        expected_counts["Free blocks"] -= consumed // (block_size // SECTOR_SIZE)
        if new["accounting"] != expected_counts:
            raise RuntimeError("Namespace block/inode allocation disagrees with primary summaries")
        if atomic:
            for source, committed in ((pending, True), (uncommitted, False)):
                clean = output / f"core-{source.name}"
                oracle = output / f"oracle-{source.name}"
                shutil.copyfile(source, clean)
                shutil.copyfile(source, oracle)
                run([args.recover.resolve(), "--write", clean])
                core = snapshot(clean)
                expected = new if committed else old
                if core != expected:
                    raise RuntimeError("Namespace recovery did not preserve the commit boundary")
                run([tools["e2fsck"], "-y", "-E", "journal_only", oracle], allowed=(0, 1))
                independent = snapshot(oracle)
                if core != independent:
                    raise RuntimeError("Portable recovery disagrees with independent journal replay")
                recovered_sha = digest(clean)
                run([args.recover.resolve(), "--write", clean])
                if digest(clean) != recovered_sha:
                    raise RuntimeError("Repeated namespace recovery changed a clean image")
                if committed:
                    record.update(pending=str(source), pending_sha256=protected[source],
                                  recovered_sha256=recovered_sha, recovered_outcome="new")
                else:
                    record.update(uncommitted=str(source), uncommitted_sha256=protected[source],
                                  uncommitted_outcome="old")
        if data(image, "/hello.txt", "hello") != b"Machlin ext4\n":
            raise RuntimeError("Namespace edits damaged existing file data")
        if any(digest(p) != sha for p, sha in protected.items()):
            raise RuntimeError("Read-only checks modified protected namespace inputs")
        record.update(accounting=new["accounting"], inode_allocations=inode_delta, passed=True)
        save()
        print(f"PASS {image.name}: namespace, metadata, allocation, e2fsck and recovery", flush=True)


if __name__ == "__main__":
    main()

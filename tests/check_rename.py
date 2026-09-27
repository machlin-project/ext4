#!/usr/bin/env python3
"""Independently verify rename identity, topology, allocation and journal recovery."""

import argparse
import atexit
from copy import deepcopy
import hashlib
import json
from pathlib import Path
import re
import shutil
import subprocess

from check_namespace import inode_fields, symlink_bytes
from check_orphans import accounting, digest
from check_writes import encoded_time
from generate_fixtures import resolve_tools

DATA_BLOCKS = 35
NAME_MAX = 255
SECTOR_SIZE = 512
INODE_INDEX = 0x1000
DIRECTORY_HEADER_SIZE = 8
DIRECTORY_TAIL_SIZE = 12
DIRECTORY_ALIGNMENT = 4
NAMESPACE_TIME = encoded_time(1700000050, 0)


def scenario(operation):
    cases = [
        dict(source="regular", same=True),
        dict(source="directory"),
        dict(source="regular", target="regular", same=True, large=True),
        dict(source="directory", target="directory"),
        dict(source="symlink", target="symlink", length=59),
        dict(source="symlink", target="symlink", length=60),
        dict(source="regular", target="regular", large=True, alias=True),
        dict(source="regular", target="directory", exchange=True),
        dict(source="regular", target="regular", large=True, held=True),
        dict(source="regular", grow=True),
        dict(source="directory", target="directory", exchange=True),
    ]
    if operation not in range(len(cases)):
        raise RuntimeError(f"Unknown rename operation {operation}")
    return cases[operation]


def entries(text):
    result = {}
    for line in text.splitlines():
        if not line.strip():
            continue
        fields = line.split("/")
        if len(fields) != 8:
            raise RuntimeError(f"Malformed independently decoded directory: {line!r}")
        number = int(fields[1])
        if number == 0:
            continue
        if not fields[5] or fields[5] in result:
            raise RuntimeError("Missing or duplicate directory name")
        result[fields[5]] = number
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--exports", type=Path, required=True)
    parser.add_argument("--recover", type=Path, required=True)
    parser.add_argument("--tools-root", type=Path)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    exports = args.exports.resolve()
    tools = resolve_tools(args.tools_root)
    images = sorted(exports.glob("rename-atomic-*.img"))
    if not images:
        raise RuntimeError("No completed rename exports")
    records = []

    def save():
        (output / "report.json").write_text(json.dumps(records, indent=2) + "\n")

    # Preserve each command without rewriting the growing report on every
    # subprocess. Flush the complete report per case and on ordinary exceptions.
    atexit.register(save)
    for image in images:
        match = re.fullmatch(r"rename-atomic-(\d+)-(.+\.img)", image.name)
        if match is None:
            raise RuntimeError(f"Invalid rename export name: {image.name}")
        operation, name = int(match[1]), match[2]
        test = scenario(operation)
        before = exports / f"rename-before-{operation}-{name}"
        pending = exports / f"rename-pending-{operation}-{name}"
        uncommitted = exports / f"rename-uncommitted-{operation}-{name}"
        protected = {p: digest(p) for p in (before, image, pending, uncommitted)}
        record = dict(image=str(image), input_sha256=protected[image], operation=operation,
                      before=str(before), before_sha256=protected[before], commands=[])
        records.append(record)
        save()

        def run(command, allowed=(0,)):
            done = subprocess.run([str(part) for part in command], capture_output=True,
                                  text=True, errors="backslashreplace", timeout=90)
            event = dict(command=[str(part) for part in command], status=done.returncode,
                         stdout=done.stdout, stderr=done.stderr)
            record["commands"].append(event)
            with (output / "commands.log").open("a") as log:
                log.write(json.dumps(dict(image=image.name, **event)) + "\n")
            if done.returncode not in allowed:
                save()
                raise RuntimeError(f"Failed ({done.returncode}): {command}")
            return done.stdout

        def stat(candidate, path):
            return inode_fields(run([tools["debugfs"], "-R", f"stat {path}", candidate]))

        def names(candidate, path):
            return entries(run([tools["debugfs"], "-R", f"ls -p {path}", candidate]))

        def object_state(candidate, path, block_size, child=False):
            inode = stat(candidate, path)
            if inode is None:
                return None
            result = dict(inode=inode)
            if inode["type"] == "symlink":
                data = symlink_bytes(candidate, path, inode, block_size, tools["debugfs"], run)
            else:
                mapping = run([tools["debugfs"], "-R", f"blocks {path}", candidate])
                if any(not value.isdigit() for value in mapping.split()):
                    raise RuntimeError("Invalid independently decoded block map")
                result["mapping"] = [int(value) for value in mapping.split()]
                if inode["type"] == "directory":
                    result["names"] = names(candidate, path)
                    if "inside" in result["names"]:
                        if child:
                            raise RuntimeError("Unexpected recursive fixture directory")
                        result["inside"] = object_state(candidate, path + "/inside", block_size, True)
                    return result
                if inode["type"] != "regular":
                    raise RuntimeError("Unexpected rename fixture inode type")
                label = path.strip("/").replace("/", "_")
                # A NAME_MAX destination must not become an overlong host filename.
                label = hashlib.sha256(label.encode()).hexdigest()[:16]
                dump = output / f"{candidate.stem}.{label}.data"
                run([tools["debugfs"], "-R", f"dump {path} {dump}", candidate])
                data = dump.read_bytes()
            if len(data) != inode["size"]:
                raise RuntimeError("Independent data dump disagrees with the inode size")
            result.update(data_sha256=hashlib.sha256(data).hexdigest(), data_size=len(data))
            return result

        destination_parent = "/left" if test.get("same") else "/right"
        destination_name = "d" * NAME_MAX if test.get("grow") else "destination"
        destination_path = destination_parent + "/" + destination_name

        def snapshot(candidate):
            run([tools["e2fsck"], "-fn", candidate])
            header = run([tools["dumpe2fs"], "-h", candidate])
            counts = accounting(header)
            block_size = counts["Block size"]
            result = dict(accounting=counts, inode_size=int(re.search(
                r"^Inode size:\s+(\d+)$", header, re.M)[1]),
                metadata_checksum="metadata_csum" in header.split())
            paths = {"root": "/", "hello": "/hello.txt", "left": "/left",
                     "source": "/left/source", "destination": destination_path}
            if not test.get("same"):
                paths["right"] = "/right"
            if test.get("alias"):
                paths["alias"] = "/kept-name"
            if test.get("grow"):
                paths["filler"] = "/filler"
            for key, path in paths.items():
                result[key] = object_state(candidate, path, block_size)
            return result

        def fixture_object(value, kind, byte, block_size, length=None, populated=False):
            if value is None:
                raise RuntimeError("Missing rename fixture object")
            inode = value["inode"]
            mode = 0o750 if kind == "directory" else 0o777 if kind == "symlink" else 0o640
            if (inode["type"], inode["uid"], inode["gid"], inode["mode"]) != (
                    kind, (1 << 32) - 3, 0x81234567, mode) or inode["generation"] == 0:
                raise RuntimeError("Incorrect rename fixture kind, owners, mode or generation")
            if kind == "directory":
                if inode["size"] != block_size or inode["blocks"] != block_size // SECTOR_SIZE:
                    raise RuntimeError("Unexpected fixture directory allocation")
                expected_names = {".", "..", "inside"} if populated else {".", ".."}
                if set(value["names"]) != expected_names or inode["links"] != 2:
                    raise RuntimeError("Incorrect fixture directory entries or links")
                if populated:
                    fixture_object(value["inside"], "regular", byte, block_size, 1)
            elif value["data_size"] != length or value["data_sha256"] != hashlib.sha256(
                    byte * length).hexdigest():
                raise RuntimeError("Incorrect independently decoded rename fixture contents")

        old, new = snapshot(before), snapshot(image)
        block_size = old["accounting"]["Block size"]
        sectors = block_size // SECTOR_SIZE
        fixture_object(old["source"], test["source"], b"s", block_size,
                       test.get("length", 1), populated=True)
        target = old["destination"]
        if test.get("target"):
            fixture_object(target, test["target"], b"t", block_size,
                           DATA_BLOCKS * block_size if test.get("large") else test.get("length", 1),
                           populated=test.get("exchange", False))
        elif target is not None:
            raise RuntimeError("Move fixture already contains its destination")
        if old["hello"]["data_sha256"] != hashlib.sha256(b"Machlin ext4\n").hexdigest():
            raise RuntimeError("Unexpected existing file contents")
        expected = deepcopy(old)
        source_directory = test["source"] == "directory"
        target_directory = test.get("target") == "directory"
        exchange = test.get("exchange", False)
        same = test.get("same", False)
        last = target is not None and not exchange and not test.get("alias")
        source_number = old["source"]["inode"]["inode"]
        target_number = target["inode"]["inode"] if target else None

        def moved(value, parent):
            result = deepcopy(value)
            result["inode"]["ctime"] = NAMESPACE_TIME
            if result["inode"]["type"] == "directory":
                result["names"][".."] = parent
            return result

        destination_key = "left" if same else "right"
        expected["destination"] = moved(old["source"], old[destination_key]["inode"]["inode"])
        expected["source"] = moved(target, old["left"]["inode"]["inode"]) if exchange else None
        if exchange:
            expected["left"]["names"]["source"] = target_number
        else:
            del expected["left"]["names"]["source"]
        expected[destination_key]["names"][destination_name] = source_number
        if same:
            expected["left"]["inode"]["links"] -= int(target_directory and not exchange)
        else:
            expected["left"]["inode"]["links"] += int(exchange and target_directory) - int(source_directory)
            expected["right"]["inode"]["links"] += int(source_directory) - int(target_directory)
        for key in ({"left"} if same else {"left", "right"}):
            expected[key]["inode"].update(ctime=NAMESPACE_TIME, mtime=NAMESPACE_TIME)
        if test.get("alias"):
            if old["alias"] != target or target["inode"]["links"] != 2:
                raise RuntimeError("Replacement fixture has incorrect hardlink identity")
            expected["alias"]["inode"].update(links=1, ctime=NAMESPACE_TIME)
        elif target is not None and target["inode"]["links"] != (2 if target_directory else 1):
            raise RuntimeError("Unexpected victim link count")
        growth = int(test.get("grow", False))
        if growth:
            previous = old["right"]["mapping"]
            current = new["right"]["mapping"]
            indexed = bool(new["right"]["inode"]["flags"] & INODE_INDEX)
            record_size = (DIRECTORY_HEADER_SIZE + NAME_MAX + DIRECTORY_ALIGNMENT - 1) & ~(DIRECTORY_ALIGNMENT - 1)
            usable = block_size - (DIRECTORY_TAIL_SIZE if old["metadata_checksum"] else 0)
            total = (len(old["right"]["names"]) - 2 + 1) * record_size
            growth = (1 if total <= usable else 2) if indexed else 1
            if current[:len(previous)] != previous or len(current) != len(previous) + growth:
                raise RuntimeError("Destination growth changed old mappings or added extra blocks")
            expected["right"]["mapping"] = current
            expected["right"]["inode"]["size"] += growth * block_size
            expected["right"]["inode"]["blocks"] += growth * sectors
            if indexed:
                expected["right"]["inode"]["flags"] |= INODE_INDEX
        released = target["inode"]["blocks"] if last else 0
        if released % sectors:
            raise RuntimeError("Victim allocation is not a whole filesystem block")
        expected["accounting"]["Free inodes"] += int(last)
        expected["accounting"]["Free blocks"] += released // sectors - growth
        if new != expected:
            record.update(expected=expected, observed=new)
            save()
            raise RuntimeError("Rename changed identity, topology, metadata, data or allocation incorrectly")
        for source, committed in ((pending, True), (uncommitted, False)):
            clean = output / f"core-{source.name}"
            oracle = output / f"oracle-{source.name}"
            shutil.copyfile(source, clean)
            shutil.copyfile(source, oracle)
            run([args.recover.resolve(), "--write", clean])
            core = snapshot(clean)
            if core != (new if committed else old):
                raise RuntimeError("Rename recovery violated its atomic commit boundary")
            run([tools["e2fsck"], "-y", "-E", "journal_only", oracle], allowed=(0, 1))
            if snapshot(oracle) != core:
                raise RuntimeError("Rename recovery disagrees with independent journal/orphan replay")
            recovered = digest(clean)
            run([args.recover.resolve(), "--write", clean])
            if digest(clean) != recovered:
                raise RuntimeError("Repeated rename recovery changed a clean image")
            if committed:
                record.update(pending=str(source), pending_sha256=protected[source],
                              recovered_sha256=recovered, recovered_outcome="new")
            else:
                record.update(uncommitted=str(source), uncommitted_sha256=protected[source],
                              uncommitted_outcome="old")
        if any(digest(path) != sha for path, sha in protected.items()):
            raise RuntimeError("Independent checks modified protected rename images")
        record.update(block_size=block_size, accounting=new["accounting"], verified_rename=new,
                      destination_path=destination_path, passed=True)
        save()
        print(f"PASS {image.name}: rename topology, metadata, allocation and independent replay", flush=True)


if __name__ == "__main__":
    main()

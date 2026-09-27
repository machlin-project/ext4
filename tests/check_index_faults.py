#!/usr/bin/env python3
"""Independently verify atomic indexed-directory splits and both journal outcomes."""

import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil
import subprocess

from check_index_write import entries, filename, MUTATION_TIME, recorded_output
from check_namespace import inode_fields
from check_orphans import accounting, digest
from generate_fixtures import resolve_tools

INODE_INDEX = 0x1000
LINK_MAX = 65000
SECTOR_SIZE = 512


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--exports", type=Path, required=True)
    parser.add_argument("--recover", type=Path, required=True)
    parser.add_argument("--tools-root", type=Path)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    tools = resolve_tools(args.tools_root)
    images = sorted([*args.exports.resolve().glob("index-atomic-*.img"),
                     *args.exports.resolve().glob("links-atomic-*.img")])
    if not images:
        raise RuntimeError("No atomic indexed-directory exports found")
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    records = []

    def save():
        (output / "report.json").write_text(json.dumps(records, indent=2) + "\n")

    for image in images:
        match = re.fullmatch(r"index-atomic-(leaf|root|node|create)-(.+)\.img", image.name)
        if match is None and not image.name.startswith("links-atomic-"):
            raise RuntimeError(f"Unrecognized indexed split export: {image.name}")
        kind = match[1] if match else "links"
        before = image.with_name(image.name.replace("-atomic-", "-before-", 1))
        pending = image.with_name(image.name.replace("-atomic-", "-pending-", 1))
        uncommitted = image.with_name(image.name.replace("-atomic-", "-uncommitted-", 1))
        protected = {path: digest(path) for path in (before, image, pending, uncommitted)}
        record = dict(image=str(image), kind=kind, input_sha256=protected[image],
                      before=str(before), before_sha256=protected[before], commands=[])
        records.append(record)
        save()

        def require(condition, message):
            if not condition:
                save()
                raise RuntimeError(message)

        def run(command, raw=False, allowed=(0,)):
            done = subprocess.run([str(part) for part in command], capture_output=True, timeout=120)
            event = dict(command=[str(part) for part in command], status=done.returncode,
                         **recorded_output(done.stdout),
                         stderr=done.stderr.decode("utf-8", "backslashreplace"))
            record["commands"].append(event)
            with (output / "commands.log").open("a") as stream:
                stream.write(json.dumps(dict(image=image.name, **event)) + "\n")
            require(done.returncode in allowed, f"Independent command failed: {command}")
            return done.stdout if raw else done.stdout.decode("utf-8", "backslashreplace")

        def snapshot(candidate):
            run([tools["e2fsck"], "-fn", candidate])
            header = run([tools["dumpe2fs"], "-h", candidate])
            features = re.search(r"^Filesystem features:\s+(.+)$", header, re.M)
            require(features is not None, "Missing filesystem feature list")
            result = dict(accounting=accounting(header), objects={}, features=sorted(features[1].split()))
            root_names = entries(run([tools["debugfs"], "-R", "ls -p /", candidate], raw=True))
            paths = ["/", "/indexed", "/indexed/child", "/lost+found", "/hello.txt"]
            if b"peer" in root_names:
                paths += ["/peer", "/peer/child"]
            if b"alternate.txt" in root_names:
                paths += ["/alternate.txt"]
            for path in paths:
                inode = inode_fields(run([tools["debugfs"], "-R", f"stat {path}", candidate]))
                require(inode is not None, f"Missing retained inode {path}")
                mapping = run([tools["debugfs"], "-R", f"blocks {path}", candidate])
                require(all(value.isdigit() for value in mapping.split()), "Invalid independent block mapping")
                item = dict(inode=inode, mapping=[int(value) for value in mapping.split()])
                label = path.strip("/").replace("/", "_") or "root"
                dump = output / f"{candidate.stem}-{label}.data"
                run([tools["debugfs"], "-R", f"dump {path} {dump}", candidate])
                data = dump.read_bytes()
                require(len(data) == inode["size"], "Independent dump size disagrees with inode size")
                item.update(data_size=len(data), data_sha256=hashlib.sha256(data).hexdigest())
                if inode["type"] == "directory":
                    names = entries(run([tools["debugfs"], "-R", f"ls -p {path}", candidate], raw=True))
                    item["names"] = {name.hex(): number for name, number in names.items()}
                else:
                    expected_bytes = b"Alternate ext4 target\n" if path == "/alternate.txt" else b"Machlin ext4\n"
                    require(inode["type"] == "regular" and data == expected_bytes,
                            "Retained hardlink target bytes or type changed")
                result["objects"][path] = item
                if kind == "links" and path == "/indexed" and b"overflow".hex() in item["names"]:
                    paths.append("/indexed/overflow")
            if result["objects"]["/indexed"]["inode"]["flags"] & INODE_INDEX:
                htree = run([tools["debugfs"], "-R", "htree_dump /indexed", candidate])
                levels = re.search(r"Indirect levels:\s+(\d+)", htree)
                counts = re.search(r"Number of entries \(count\):\s+(\d+)", htree)
                require(levels is not None and counts is not None, "Missing independent htree header")
                result["index"] = dict(levels=int(levels[1]), root_entries=int(counts[1]))
            else:
                require(kind == "create", "An existing index lost its inode flag")
                result["index"] = None
            return result

        old = snapshot(before)
        new = snapshot(image)
        parent_old = old["objects"]["/indexed"]
        parent_new = new["objects"]["/indexed"]
        hello_old = old["objects"]["/hello.txt"]
        hello_new = new["objects"]["/hello.txt"]
        added = set(parent_new["names"]) - set(parent_old["names"])
        require(len(added) == 1 and all(parent_new["names"].get(name) == number
                                       for name, number in parent_old["names"].items()),
                "Split did not add exactly one name while preserving existing mappings")
        name_hex = next(iter(added))
        name = bytes.fromhex(name_hex)
        ordinal = re.match(rb"new-(\d+)-", name)
        if kind == "links":
            require(name == b"overflow", "Unexpected overflow directory name")
        else:
            require(ordinal is not None and name == filename(int(ordinal[1])), "Unexpected split name bytes")
            require(parent_new["names"][name_hex] == hello_old["inode"]["inode"], "Split linked the wrong inode")
        for path in old["objects"]:
            if path not in ("/indexed", "/hello.txt"):
                require(old["objects"][path] == new["objects"][path], f"Split changed unrelated object {path}")
        expected_hello = (hello_old["inode"] if kind == "links" else
                          dict(hello_old["inode"], links=hello_old["inode"]["links"] + 1, ctime=MUTATION_TIME))
        require(hello_new == {**hello_old, "inode": expected_hello}, "Split changed target data, identity or unrelated attributes")
        expected_parent = dict(parent_old["inode"], size=parent_new["inode"]["size"],
                               blocks=parent_new["inode"]["blocks"], mtime=MUTATION_TIME, ctime=MUTATION_TIME)
        if kind == "create":
            expected_parent["flags"] |= INODE_INDEX
        if kind == "links":
            expected_parent["links"] = 1
        require(parent_new["inode"] == expected_parent and
                (parent_new["inode"]["size"] == parent_old["inode"]["size"] if kind == "links"
                 else parent_new["inode"]["size"] > parent_old["inode"]["size"]) and
                new["accounting"]["Free inodes"] == old["accounting"]["Free inodes"] - int(kind == "links"),
                "Split changed directory metadata or inode accounting incorrectly")
        require(new["accounting"]["Free blocks"] < old["accounting"]["Free blocks"],
                "Split did not account its new blocks")
        if kind == "links":
            block_size = new["accounting"]["Block size"]
            child = new["objects"]["/indexed/overflow"]
            inode = child["inode"]
            require(parent_old["inode"]["links"] == LINK_MAX and
                    len(parent_old["names"]) == LINK_MAX and parent_new["mapping"] == parent_old["mapping"] and
                    parent_new["inode"]["blocks"] == parent_old["inode"]["blocks"] and
                    "dir_nlink" not in old["features"] and
                    new["features"] == sorted(old["features"] + ["dir_nlink"]) and
                    new["accounting"]["Free blocks"] == old["accounting"]["Free blocks"] - 1,
                    "Link-count overflow changed more than its admitted metadata and child allocation")
            require((inode["type"], inode["mode"], inode["uid"], inode["gid"], inode["links"],
                     inode["size"], inode["blocks"]) ==
                    ("directory", 0o750, 70000, 80000, 2, block_size, block_size // SECTOR_SIZE) and
                    inode["generation"] != 0 and
                    all(inode[field] == MUTATION_TIME for field in ("atime", "mtime", "ctime")) and
                    len(child["mapping"]) == 1 and
                    parent_new["names"][name_hex] == inode["inode"] and
                    child["names"] == {b".".hex(): inode["inode"], b"..".hex(): parent_old["inode"]["inode"]} and
                    new["index"] == old["index"], "Invalid overflow directory identity or topology")
        elif kind == "create":
            block_size = new["accounting"]["Block size"]
            require(old["index"] is None and new["index"] is not None and
                    new["index"]["levels"] == 0 and new["index"]["root_entries"] in (1, 2) and
                    parent_old["inode"]["size"] == block_size and
                    parent_new["inode"]["size"] == (1 + new["index"]["root_entries"]) * block_size,
                    "Expected linear-to-indexed conversion did not occur")
        elif kind == "root":
            require(old["index"]["levels"] in (0, 1) and
                    new["index"]["levels"] == old["index"]["levels"] + 1 and
                    new["index"]["root_entries"] == 1, "Expected root height growth did not occur")
        elif kind == "node":
            require(old["index"]["levels"] == new["index"]["levels"] and
                    new["index"]["levels"] in (1, 2) and
                    new["index"]["root_entries"] == old["index"]["root_entries"] + 1,
                    "Expected internal-node split did not occur")
        else:
            require(old["index"]["levels"] == new["index"]["levels"] and
                    new["index"]["root_entries"] == old["index"]["root_entries"] +
                    int(old["index"]["levels"] == 0), "Expected leaf-only split did not occur")
        for source, committed in ((pending, True), (uncommitted, False)):
            clean = output / f"core-{source.name}"
            oracle = output / f"oracle-{source.name}"
            shutil.copyfile(source, clean)
            shutil.copyfile(source, oracle)
            run([args.recover.resolve(), "--write", clean])
            core = snapshot(clean)
            require(core == (new if committed else old), "Core recovery violated the index split commit boundary")
            run([tools["e2fsck"], "-y", "-E", "journal_only", oracle], allowed=(0, 1))
            require(snapshot(oracle) == core, "Core index recovery disagrees with independent journal replay")
            recovered = digest(clean)
            run([args.recover.resolve(), "--write", clean])
            require(digest(clean) == recovered, "Repeated index recovery changed a clean image")
            if committed:
                record.update(pending=str(source), pending_sha256=protected[source], recovered_outcome="new")
            else:
                record.update(uncommitted=str(source), uncommitted_sha256=protected[source], uncommitted_outcome="old")
        require(all(digest(path) == sha for path, sha in protected.items()), "Checking changed a protected export")
        require(kind == "links" or old["features"] == new["features"], "Index mutation changed filesystem features")
        record.update(passed=True, ordinal=None if ordinal is None else int(ordinal[1]), exact_new_name=name_hex, verified_split=new,
                      has_peer="/peer" in new["objects"],
                      retained_paths=[path for path in ("/alternate.txt", "/indexed/overflow") if path in new["objects"]],
                      protected_inputs_unchanged=True, independent_replay_matches=True)
        save()
        print(f"PASS {image.name}: exact split, old/new outcomes, independent replay and idempotence", flush=True)


if __name__ == "__main__":
    main()

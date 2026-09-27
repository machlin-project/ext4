#!/usr/bin/env python3
"""Independently verify atomic indexed-directory splits and both journal outcomes."""

import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil
import subprocess

from check_index_write import entries, filename, MUTATION_TIME
from check_namespace import inode_fields
from check_orphans import accounting, digest
from generate_fixtures import resolve_tools


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--exports", type=Path, required=True)
    parser.add_argument("--recover", type=Path, required=True)
    parser.add_argument("--tools-root", type=Path)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    tools = resolve_tools(args.tools_root)
    images = sorted(args.exports.resolve().glob("index-atomic-*.img"))
    if not images:
        raise RuntimeError("No atomic indexed-directory exports found")
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    records = []

    def save():
        (output / "report.json").write_text(json.dumps(records, indent=2) + "\n")

    for image in images:
        match = re.fullmatch(r"index-atomic-(leaf|root|node)-(.+)\.img", image.name)
        if match is None:
            raise RuntimeError(f"Unrecognized indexed split export: {image.name}")
        kind = match[1]
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
                         stdout=done.stdout.decode("utf-8", "backslashreplace"),
                         stderr=done.stderr.decode("utf-8", "backslashreplace"))
            record["commands"].append(event)
            with (output / "commands.log").open("a") as stream:
                stream.write(json.dumps(dict(image=image.name, **event)) + "\n")
            require(done.returncode in allowed, f"Independent command failed: {command}")
            return done.stdout if raw else event["stdout"]

        def snapshot(candidate):
            run([tools["e2fsck"], "-fn", candidate])
            result = dict(accounting=accounting(run([tools["dumpe2fs"], "-h", candidate])), objects={})
            paths = ("/", "/indexed", "/peer", "/indexed/child", "/peer/child", "/lost+found", "/hello.txt")
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
                    require(inode["type"] == "regular" and data == b"Machlin ext4\n",
                            "Retained hardlink target bytes or type changed")
                result["objects"][path] = item
            htree = run([tools["debugfs"], "-R", "htree_dump /indexed", candidate])
            levels = re.search(r"Indirect levels:\s+(\d+)", htree)
            counts = re.search(r"Number of entries \(count\):\s+(\d+)", htree)
            require(levels is not None and counts is not None, "Missing independent htree header")
            result["index"] = dict(levels=int(levels[1]), root_entries=int(counts[1]))
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
        require(ordinal is not None and name == filename(int(ordinal[1])), "Unexpected split name bytes")
        require(parent_new["names"][name_hex] == hello_old["inode"]["inode"], "Split linked the wrong inode")
        for path in old["objects"]:
            if path not in ("/indexed", "/hello.txt"):
                require(old["objects"][path] == new["objects"][path], f"Split changed unrelated object {path}")
        expected_hello = dict(hello_old["inode"], links=hello_old["inode"]["links"] + 1, ctime=MUTATION_TIME)
        require(hello_new == {**hello_old, "inode": expected_hello}, "Split changed target data, identity or unrelated attributes")
        expected_parent = dict(parent_old["inode"], size=parent_new["inode"]["size"],
                               blocks=parent_new["inode"]["blocks"], mtime=MUTATION_TIME, ctime=MUTATION_TIME)
        require(parent_new["inode"] == expected_parent and
                parent_new["inode"]["size"] > parent_old["inode"]["size"] and
                new["accounting"]["Free inodes"] == old["accounting"]["Free inodes"],
                "Split changed directory metadata or inode accounting incorrectly")
        require(new["accounting"]["Free blocks"] < old["accounting"]["Free blocks"],
                "Split did not account its new blocks")
        if kind == "root":
            require(old["index"]["levels"] == 0 and new["index"]["levels"] == 1 and
                    new["index"]["root_entries"] == 1, "Expected root height growth did not occur")
        elif kind == "node":
            require(old["index"]["levels"] == new["index"]["levels"] == 1 and
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
        record.update(passed=True, ordinal=int(ordinal[1]), exact_new_name=name_hex, verified_split=new,
                      protected_inputs_unchanged=True, independent_replay_matches=True)
        save()
        print(f"PASS {image.name}: exact split, old/new outcomes, independent replay and idempotence", flush=True)


if __name__ == "__main__":
    main()

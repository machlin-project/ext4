#!/usr/bin/env python3
"""Independently verify distributed-group mutations and both recovery outcomes."""

import argparse
import copy
import hashlib
import json
from pathlib import Path
import re
import shutil
import subprocess

from check_rename import entries
from check_namespace import inode_fields
from check_orphans import accounting, digest
from generate_fixtures import resolve_tools
from generate_geometry_fixtures import group_layout, INODES_PER_GROUP


SECONDS = 1700000300
SECTOR_BYTES = 512
EXTENTS = 0x80000
TARGET = "/geometry-file"
EXTRA = "/geometry-extra"


def snapshot(image, output, tools, run, *, check=True, retained_paths=None):
    """Batch inode decoding and data exports in debugfs, including every seed."""
    output.mkdir(parents=True, exist_ok=False)
    if check:
        run([tools["e2fsck"], "-fn", image])
    counts = accounting(run([tools["dumpe2fs"], "-h", image]))
    layout = group_layout(run([tools["dumpe2fs"], "-g", image]))
    names = entries(run([tools["debugfs"], "-R", "ls -p /", image]))
    paths = (list(retained_paths) if retained_paths is not None else
             ["/"] + [f"/{name}" for name in sorted(names) if name not in (".", "..")])
    if any(not re.fullmatch(r"/[a-zA-Z0-9.+/_-]*", path) for path in paths):
        raise RuntimeError("Unexpected geometry fixture name")
    script = output / "stat.commands"
    script.write_text("".join(f"stat {path}\n" for path in paths))
    decoded = re.split(r"(?m)^debugfs: stat (.+)\n", run([tools["debugfs"], "-f", script, image]))
    if decoded[0].strip() or len(decoded) != 2 * len(paths) + 1:
        raise RuntimeError("Missing batched inode output")
    objects = {}
    dumps = {}
    for path, raw in zip(decoded[1::2], decoded[2::2]):
        inode = inode_fields(raw)
        if inode is None or path in objects or path not in paths:
            raise RuntimeError("Missing or duplicate independent inode")
        mapping = re.search(r"(?s)\nEXTENTS:\n(.*)", raw)
        if mapping is None:
            raise RuntimeError("Missing independently decoded extent map")
        item = dict(inode=inode, mapping=mapping[1].strip())
        if inode["type"] == "regular":
            if inode["size"]:
                dumps[path] = output / f"data-{len(objects)}"
            else:
                item["data_sha256"] = hashlib.sha256(b"").hexdigest()
        elif inode["type"] != "directory":
            raise RuntimeError("Unexpected object type in geometry fixture")
        objects[path] = item
    if dumps:
        script = output / "dump.commands"
        script.write_text("".join(f'dump {path} "{destination}"\n'
                                  for path, destination in dumps.items()))
        run([tools["debugfs"], "-f", script, image])
        for path, destination in dumps.items():
            if destination.stat().st_size != objects[path]["inode"]["size"]:
                raise RuntimeError("Independent geometry file length disagrees")
            objects[path]["data_sha256"] = digest(destination)
    return dict(accounting=counts, layout=layout, names=names, objects=objects)


def check_seed(state, source):
    objects = state["objects"]
    size = state["accounting"]["Block size"]
    groups = len(state["layout"])
    first = {}
    sequence = sorted(path for path in objects if path.startswith("/inode-"))
    if sequence != [f"/inode-{index:05d}" for index in range(len(sequence))]:
        raise RuntimeError("Created inode sequence has gaps")
    for path in sequence:
        inode = objects[path]["inode"]
        group = (inode["inode"] - 1) // INODES_PER_GROUP
        content = bytes([group + 1]) * (size + 3) if group not in first else b""
        first.setdefault(group, inode["inode"])
        if (inode["size"] != len(content) or inode["mode"] != 0o640 or
                inode["uid"] != 12345 or inode["gid"] != 23456 or inode["links"] != 1 or
                inode["generation"] != 1 or inode["mtime"] != (SECONDS, 0) or
                objects[path]["data_sha256"] != hashlib.sha256(content).hexdigest()):
            raise RuntimeError("Seed inode or group data differs from independent model")
    if set(first) != set(range(groups)):
        raise RuntimeError("Seed does not cover every block group")
    if (objects[TARGET]["inode"]["inode"] - 1) // INODES_PER_GROUP != groups - 1:
        raise RuntimeError("Mutation target is outside the last descriptor block")
    for path in ("/hello.txt", "/empty", "/lost+found"):
        if objects[path] != source["objects"][path]:
            raise RuntimeError("Setup changed an existing fixture inode")


def check_transition(before, after, operation):
    expected = copy.deepcopy(before)
    objects = expected["objects"]
    counts = expected["accounting"]
    size = counts["Block size"]
    if operation == "create":
        inode = dict(inode=objects[TARGET]["inode"]["inode"] + 1, generation=1,
                     type="regular", mode=0o640, uid=12345, gid=23456, links=1,
                     size=0, blocks=0, flags=EXTENTS, atime=(SECONDS, 0),
                     mtime=(SECONDS, 0), ctime=(SECONDS, 0), crtime=(0, 0))
        objects[EXTRA] = dict(inode=inode, mapping="", data_sha256=hashlib.sha256(b"").hexdigest())
        expected["names"][EXTRA[1:]] = inode["inode"]
        counts["Free inodes"] -= 1
    elif operation == "write":
        inode = objects[TARGET]["inode"]
        data = bytes(size + 7) + bytes((index * 31 + 7) & 255 for index in range(size + 3))
        inode.update(size=len(data), blocks=2 * size // SECTOR_BYTES,
                     mtime=(SECONDS, 0), ctime=(SECONDS, 0))
        objects[TARGET]["data_sha256"] = hashlib.sha256(data).hexdigest()
        # Allocation chooses physical blocks; fsck checks ownership and the
        # exact file bytes/block count constrain the new sparse extent map.
        objects[TARGET]["mapping"] = after["objects"][TARGET]["mapping"]
        counts["Free blocks"] -= 2
    elif operation == "remove":
        counts["Free blocks"] += objects[TARGET]["inode"]["blocks"] // (size // SECTOR_BYTES)
        counts["Free inodes"] += 1
        del objects[TARGET]
        del expected["names"][TARGET[1:]]
    else:
        raise RuntimeError("Unknown geometry operation")
    if operation in ("create", "remove"):
        root = objects["/"]["inode"]
        root.update(mtime=(SECONDS, 0), ctime=(SECONDS, 0))
        new_root = after["objects"]["/"]
        if operation == "create":
            growth = new_root["inode"]["size"] - root["size"]
            if growth not in (0, size, 2 * size):
                raise RuntimeError("Unexpected directory growth for one insertion")
            if new_root["inode"]["blocks"] - root["blocks"] != growth // SECTOR_BYTES:
                raise RuntimeError("Directory allocation differs from directory growth")
            counts["Free blocks"] -= growth // size
            root.update(size=new_root["inode"]["size"], blocks=new_root["inode"]["blocks"])
            objects["/"]["mapping"] = new_root["mapping"]
    if expected != after:
        raise RuntimeError(f"Geometry {operation} changed unexpected bytes, metadata, names or counts")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--fixtures", type=Path, required=True)
    parser.add_argument("--exports", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--recover", type=Path, required=True)
    parser.add_argument("--tools-root", type=Path)
    args = parser.parse_args()
    tools = resolve_tools(args.tools_root)
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    records = []
    images = sorted(args.exports.resolve().glob("geometry-create-before-*.img"))
    if not images:
        raise RuntimeError("No geometry exports")
    recover = args.recover.resolve()
    protected = {recover: digest(recover)}
    for baseline in images:
        name = baseline.name.removeprefix("geometry-create-before-")
        source = args.fixtures.resolve() / name
        protected[source] = digest(source)
        commands = []
        serial = 0

        def run(command):
            done = subprocess.run([str(part) for part in command], capture_output=True,
                                  text=True, errors="backslashreplace", timeout=120)
            log = output / f"{source.stem}-command-{len(commands)}.log"
            log.write_text(done.stdout + "\n--- stderr ---\n" + done.stderr)
            commands.append(dict(command=[str(part) for part in command], status=done.returncode,
                                 output=str(log)))
            if done.returncode:
                raise RuntimeError(f"Geometry oracle command failed: {command}; see {log}")
            return done.stdout

        def take(image):
            nonlocal serial
            serial += 1
            return snapshot(image, output / f"{source.stem}-snapshot-{serial}", tools, run)

        try:
            original = take(source)
            previous = None
            command_start = 0
            for operation in ("create", "write", "remove"):
                before_path = args.exports.resolve() / f"geometry-{operation}-before-{name}"
                image = args.exports.resolve() / f"geometry-{operation}-after-{name}"
                for path in (before_path, image):
                    protected[path] = digest(path)
                before, after = take(before_path), take(image)
                if before["layout"] != original["layout"]:
                    raise RuntimeError("Mutation changed immutable group layout")
                if previous is None:
                    check_seed(before, original)
                elif before != previous:
                    raise RuntimeError("Geometry operation lost its predecessor state")
                check_transition(before, after, operation)
                previous = after
                record = dict(image=str(image), input_sha256=protected[image], source=str(source),
                              geometry_operation=operation, accounting=after["accounting"],
                              block_size=after["accounting"]["Block size"], verified_geometry=after,
                              commands=[])
                records.append(record)
                for outcome, wanted in (("pending", after), ("uncommitted", before)):
                    pending = args.exports.resolve() / f"geometry-{operation}-{outcome}-{name}"
                    protected[pending] = digest(pending)
                    if outcome == "pending":
                        record.update(pending=str(pending), pending_sha256=protected[pending])
                    for engine in ("core", "oracle"):
                        copy_path = output / f"{engine}-{pending.name}"
                        shutil.copyfile(pending, copy_path)
                        run([recover, "--write", copy_path] if engine == "core" else
                            [tools["e2fsck"], "-y", "-E", "journal_only", copy_path])
                        if take(copy_path) != wanted:
                            raise RuntimeError(f"{engine} geometry replay differs from {outcome}")
                        if engine == "core":
                            stable = digest(copy_path)
                            run([recover, "--write", copy_path])
                            if digest(copy_path) != stable:
                                raise RuntimeError("Geometry recovery is not idempotent")
                record.update(passed=True, recovered_outcome="new", idempotent=True)
                record["commands"] = commands[command_start:]
                command_start = len(commands)
                print(f"PASS {name} {operation}: every inode, file data, group layout and replay", flush=True)
            if any(digest(path) != sha for path, sha in protected.items()):
                raise RuntimeError("Geometry checking modified an input")
        finally:
            (output / "report.json").write_text(json.dumps(records, indent=2) + "\n")


if __name__ == "__main__":
    main()

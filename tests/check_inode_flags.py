#!/usr/bin/env python3
"""Check persistent flags, protected namespaces and interrupted flag journals."""

import argparse
import copy
import json
from pathlib import Path
import shutil
import subprocess

from check_orphans import digest
from generate_fixtures import resolve_tools
from linux_xattrs import snapshot

SECONDS = 1700000200
SYNC = 0x8
IMMUTABLE = 0x10
APPEND = 0x20
NODUMP = 0x40
NOATIME = 0x80
JOURNAL_DATA = 0x4000
NOTAIL = 0x8000
DIRSYNC = 0x10000
TOPDIR = 0x20000
EXTENTS = 0x80000
PASSIVE = SYNC | NODUMP | NOATIME | JOURNAL_DATA | NOTAIL
POLICY = PASSIVE | IMMUTABLE | APPEND | DIRSYNC | TOPDIR


def model(before, actual, state):
    expected = copy.deepcopy(before)
    objects = expected["objects"]
    inode = objects["/block"]["inode"]
    inode["ctime"] = (SECONDS, 0)
    if state == "passive":
        inode["flags"] = (inode["flags"] & ~POLICY) | PASSIVE
    elif state in ("set-after", "clear-after"):
        inode["flags"] = (inode["flags"] & ~(IMMUTABLE | NODUMP)) | (
            IMMUTABLE | NODUMP if state == "set-after" else 0)
    else:
        block_size = before["accounting"]["Block size"]
        sectors = block_size // 512
        extent = inode["flags"] & EXTENTS
        inode.update(flags=extent | PASSIVE | APPEND, mode=0o640, size=3,
                     blocks=inode["blocks"] + sectors, mtime=(SECONDS, 0))
        objects["/block"]["data"] = b"abc".hex()
        root = objects["/"]
        root["inode"].update(links=root["inode"]["links"] + 1,
                             mtime=(SECONDS, 0), ctime=(SECONDS, 0))
        new_paths = {
            "/flag-dir": ("directory", block_size, sectors, 3,
                          extent | PASSIVE | DIRSYNC | TOPDIR | IMMUTABLE),
            "/flag-dir/child": ("regular", 0, 0, 1, extent | PASSIVE),
            "/flag-dir/nested": ("directory", block_size, sectors, 2, extent | PASSIVE | DIRSYNC),
            "/flag-dir/link": ("symlink", 5, 0, 1, NOATIME),
            "/flag-dir/fifo": ("FIFO", 0, 0, 1, NOATIME),
            "/flag-dir/incoming": ("regular", 0, 0, 1, extent),
        }
        existing = {obj["inode"]["inode"] for obj in objects.values()}
        new_ids = {}
        for path, (kind, size, blocks, links, flags) in new_paths.items():
            found = actual["objects"].get(path)
            if found is None or found["inode"]["inode"] in existing:
                raise RuntimeError("Missing or aliased inherited inode")
            identity = found["inode"]["inode"]
            existing.add(identity)
            new_ids[path] = identity
            wanted = dict(inode=identity, generation=1, type=kind, size=size,
                          blocks=blocks, links=links, flags=flags, mode=0o640,
                          uid=12345, gid=23456, atime=(SECONDS, 0),
                          mtime=(SECONDS, 0), ctime=(SECONDS, 0),
                          crtime=None if inode["crtime"] is None else (0, 0))
            objects[path] = dict(inode=wanted, attrs={})
            if kind == "regular":
                objects[path]["data"] = ""
            elif kind == "symlink":
                objects[path]["data"] = b"child".hex()
        root["names"]["flag-dir"] = new_ids["/flag-dir"]
        objects["/flag-dir"]["names"] = {
            ".": new_ids["/flag-dir"], "..": root["inode"]["inode"],
            **{name: new_ids[f"/flag-dir/{name}"] for name in
               ("child", "nested", "link", "fifo", "incoming")},
        }
        objects["/flag-dir/nested"]["names"] = {
            ".": new_ids["/flag-dir/nested"], "..": new_ids["/flag-dir"]}
        expected["accounting"]["Free inodes"] -= len(new_paths)
        expected["accounting"]["Free blocks"] -= 3
    return expected


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
    exports = args.exports.resolve()
    sources = [args.fixtures.resolve() / image.name.removeprefix("flags-passive-")
               for image in sorted(exports.glob("flags-passive-*.img"))]
    if not sources:
        raise RuntimeError("No flag exports")
    recover = args.recover.resolve()
    protected = {recover: digest(recover)}
    records = []
    for source in sources:
        protected[source] = digest(source)
        for state in ("passive", "protected", "set-after", "clear-after"):
            image = exports / f"flags-{state}-{source.name}"
            atomic = state.endswith("-after")
            kind = state.removesuffix("-after")
            before = exports / f"flags-{kind}-before-{source.name}" if atomic else source
            protected[image] = digest(image)
            protected[before] = digest(before)
            record = dict(image=str(image), source=str(source), input_sha256=protected[image],
                          flag_state=state, commands=[])
            records.append(record)
            serial = 0

            def run(command, allowed=(0,)):
                done = subprocess.run([str(x) for x in command], capture_output=True,
                                      text=True, errors="backslashreplace", timeout=120)
                record["commands"].append(dict(command=[str(x) for x in command],
                                                status=done.returncode, stdout=done.stdout,
                                                stderr=done.stderr))
                if done.returncode not in allowed:
                    raise RuntimeError(f"Flag oracle command failed: {command}")
                return done.stdout

            def take(candidate):
                nonlocal serial
                serial += 1
                return snapshot(candidate, output / f"{image.stem}-{serial}", tools, run)

            try:
                old = take(before)
                new = take(image)
                if atomic:
                    original = take(source)
                    expected_old = copy.deepcopy(original)
                    if state == "clear-after":
                        expected_old["objects"]["/block"]["inode"].update(
                            flags=original["objects"]["/block"]["inode"]["flags"] | IMMUTABLE | NODUMP,
                            ctime=(SECONDS, 0))
                    if old != expected_old:
                        raise RuntimeError("Flag baseline changed unrelated metadata")
                if new != model(old, new, state):
                    raise RuntimeError(f"Flag/inheritance state disagrees with independent model: {state}")
                record.update(accounting=new["accounting"], verified_flags=new,
                              block_size=new["accounting"]["Block size"])
                if atomic:
                    for outcome, expected in (("pending", new), ("uncommitted", old)):
                        interrupted = exports / f"flags-{kind}-{outcome}-{source.name}"
                        protected[interrupted] = digest(interrupted)
                        if outcome == "pending":
                            record.update(pending=str(interrupted), pending_sha256=protected[interrupted])
                        for engine in ("core", "oracle"):
                            candidate = output / f"{engine}-{interrupted.name}"
                            shutil.copyfile(interrupted, candidate)
                            run([recover, "--write", candidate] if engine == "core" else
                                [tools["e2fsck"], "-y", "-E", "journal_only", candidate])
                            if take(candidate) != expected:
                                raise RuntimeError(f"{engine} flag replay differs from {outcome} state")
                            if engine == "core":
                                stable = digest(candidate)
                                run([recover, "--write", candidate])
                                if digest(candidate) != stable:
                                    raise RuntimeError("Flag replay is not idempotent")
                    record.update(recovered_outcome="new", uncommitted_outcome="old", idempotent=True)
                if any(digest(path) != sha for path, sha in protected.items()):
                    raise RuntimeError("Flag verification changed a protected input")
                record["passed"] = True
                print(f"PASS {image.name}: exact flags, attributes, namespace, data and recovery", flush=True)
            finally:
                (output / "report.json").write_text(json.dumps(records, indent=2) + "\n")


if __name__ == "__main__":
    main()

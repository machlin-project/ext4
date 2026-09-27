#!/usr/bin/env python3
"""Check indexed namespace exports with independent ext4 tools and exact byte names."""

import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess

from check_namespace import inode_fields, symlink_bytes
from check_orphans import accounting, digest
from generate_fixtures import resolve_tools

NAME_MAX = 255
MIN_BLOCK_SIZE = 1024
INODE_INDEX = 0x1000
HIGH_BYTE_POSITION = 16
MUTATION_TIME = (1700000050, 0)
OUTPUT_INLINE_LIMIT = 32768


def recorded_output(payload):
    """Bound success logs; exact name maps and dumped bytes are checked separately."""
    result = dict(stdout_bytes=len(payload), stdout_sha256=hashlib.sha256(payload).hexdigest())
    if len(payload) <= OUTPUT_INLINE_LIMIT:
        result["stdout"] = payload.decode("utf-8", "backslashreplace")
    else:
        result["stdout_excerpt"] = (payload[:OUTPUT_INLINE_LIMIT // 2].decode("utf-8", "backslashreplace") +
                                    "\n[large command output omitted; full byte count and hash recorded]\n" +
                                    payload[-OUTPUT_INLINE_LIMIT // 2:].decode("utf-8", "backslashreplace"))
    return result


def filename(index):
    name = bytearray(f"new-{index:06d}-".ljust(NAME_MAX, "n").encode("ascii"))
    name[HIGH_BYTE_POSITION] = 0x80 + index % 128
    return bytes(name)


def entries(payload):
    result = {}
    # UTF-8 is not an ext4 filename requirement. Splitting decoded Unicode on
    # splitlines would also incorrectly treat filename byte 0x85 as a newline.
    for line in payload.split(b"\n"):
        if not line:
            continue
        fields = line.split(b"/")
        if len(fields) != 8:
            raise RuntimeError(f"Malformed debugfs directory record: {line!r}")
        number = int(fields[1])
        if number == 0:
            continue
        if not fields[5] or fields[5] in result:
            raise RuntimeError("Empty or duplicate independently decoded name")
        result[fields[5]] = number
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--fixtures", type=Path, required=True, help="Indexed fixture report.json")
    parser.add_argument("--exports", type=Path, required=True)
    parser.add_argument("--tools-root", type=Path)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--case", action="append", default=[], help="Select an exact source image filename")
    args = parser.parse_args()
    tools = resolve_tools(args.tools_root)
    fixtures = json.loads(args.fixtures.read_text())
    if not fixtures or not all(fixture.get("passed") for fixture in fixtures):
        raise RuntimeError("Expected verified indexed fixture profiles")
    if args.case:
        if set(args.case) - {Path(item["image"]).name for item in fixtures}:
            raise RuntimeError("Requested indexed profile is absent")
        fixtures = [item for item in fixtures if Path(item["image"]).name in args.case]
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    records = []

    def save():
        (output / "report.json").write_text(json.dumps(records, indent=2) + "\n")

    for fixture in fixtures:
        source = Path(fixture["image"])
        image = args.exports.resolve() / f"indexed-written-{source.name}"
        source_hash = digest(source)
        image_hash = digest(image)
        if source_hash != fixture["input_sha256"]:
            raise RuntimeError(f"Source fixture changed: {source}")
        record = dict(source=str(source), image=str(image), source_sha256=source_hash,
                      input_sha256=image_hash, commands=[])
        records.append(record)

        def run(command, raw=False):
            done = subprocess.run([str(part) for part in command], capture_output=True, timeout=120)
            record["commands"].append(dict(command=[str(part) for part in command], status=done.returncode,
                                            **recorded_output(done.stdout),
                                            stderr=done.stderr.decode("utf-8", "backslashreplace")))
            if done.returncode != 0:
                save()
                raise RuntimeError(f"Independent command failed: {command}")
            return done.stdout if raw else done.stdout.decode("utf-8", "backslashreplace")

        def listing(candidate, path):
            return entries(run([tools["debugfs"], "-R", f"ls -p {path}", candidate], raw=True))

        def stat(candidate, path):
            result = inode_fields(run([tools["debugfs"], "-R", f"stat {path}", candidate]))
            if result is None:
                raise RuntimeError(f"Missing independently resolved inode: {candidate}:{path}")
            return result

        def require(condition, message):
            if not condition:
                save()
                raise RuntimeError(message)

        run([tools["e2fsck"], "-fn", image])
        before = accounting(run([tools["dumpe2fs"], "-h", source]))
        after = accounting(run([tools["dumpe2fs"], "-h", image]))
        block_size = before["Block size"]
        large = "large_dir" in fixture["features"]
        added = 32 if large else 700 if block_size == MIN_BLOCK_SIZE else 160
        old_root = listing(source, "/")
        new_root = listing(image, "/")
        container = stat(image, "/container")
        require(new_root == {**{name: number for name, number in old_root.items() if name != b"peer"},
                             b"container": container["inode"]}, "Root directory lost or gained unexpected names")
        require(listing(image, "/container") == {b".": container["inode"], b"..": old_root[b"."]},
                "Indexed-directory roundtrip left an unexpected child or dotdot")
        old_parent = stat(source, "/indexed")
        parent = stat(image, "/indexed")
        old_hello = stat(source, "/hello.txt")
        hello = stat(image, "/hello.txt")
        old_entries = listing(source, "/indexed")
        old_peer_entries = listing(source, "/peer")
        new_entries = listing(image, "/indexed")
        expected = dict(old_entries)
        expected.update({filename(index): hello["inode"] for index in range(added)})
        new_objects = {}
        for name, kind in (("created", "regular"), ("nested", "directory"),
                           ("short-link", "symlink"), ("long-link", "symlink")):
            inode = stat(image, f"/indexed/{name}")
            require((inode["type"], inode["mode"], inode["uid"], inode["gid"]) ==
                    (kind, 0o750, 70000, 80000), "New indexed object has incorrect type or admitted attributes")
            require(all(inode[field] == MUTATION_TIME for field in ("atime", "mtime", "ctime")),
                    "New indexed object lost its captured timestamps")
            require(inode["generation"] != 0 and inode["links"] == (2 if kind == "directory" else 1),
                    "New indexed object has incorrect identity or link count")
            expected[name.encode()] = inode["inode"]
            new_objects[name] = inode
        require(new_entries == expected, "Indexed directory differs from the full expected byte-name mapping")
        require(listing(image, "/indexed/nested") ==
                {b".": new_objects["nested"]["inode"], b"..": parent["inode"]}, "New directory dotdot is incorrect")
        require(new_objects["created"]["size"] == 0 and new_objects["created"]["blocks"] == 0,
                "Exchanged regular file has unexpected content or allocation")
        for name, target in (("short-link", b"../hello.txt"), ("long-link", b"a" * 60)):
            require(symlink_bytes(image, f"/indexed/{name}", new_objects[name], block_size,
                                  tools["debugfs"], run) == target, "Indexed symlink lost its exact target")
        require(parent["inode"] == old_parent["inode"] and parent["generation"] == old_parent["generation"] and
                parent["flags"] & INODE_INDEX and parent["size"] > old_parent["size"] and
                parent["links"] == old_parent["links"] + 1, "Indexed directory identity, growth or link count changed incorrectly")
        require(hello["inode"] == old_hello["inode"] and hello["generation"] == old_hello["generation"] and
                hello["links"] == old_hello["links"] + added -
                sum(number == hello["inode"] for number in old_peer_entries.values()),
                "Indexed hardlink count or identity is incorrect")
        if b"alternate.txt" in old_root:
            old_alternate = stat(source, "/alternate.txt")
            alternate = stat(image, "/alternate.txt")
            removed = sum(number == old_alternate["inode"] for number in old_peer_entries.values())
            require(alternate == dict(old_alternate, links=old_alternate["links"] - removed,
                                      ctime=MUTATION_TIME if removed else old_alternate["ctime"]),
                    "Deleting the peer changed the alternate target incorrectly")
        targets = {"hello": b"Machlin ext4\n"}
        if b"alternate.txt" in old_root:
            targets["alternate"] = b"Alternate ext4 target\n"
        for target, contents in targets.items():
            for label, candidate in (("source", source), ("written", image)):
                dump = output / f"{source.stem}-{label}-{target}.data"
                run([tools["debugfs"], "-R", f"dump /{target}.txt {dump}", candidate])
                require(dump.read_bytes() == contents, "Retained file bytes changed")
        htree = run([tools["debugfs"], "-R", "htree_dump /indexed", image])
        levels = re.search(r"Indirect levels:\s+(\d+)", htree)
        version = re.search(r"Hash Version:\s+(\d+)", htree)
        require(levels is not None and version is not None and
                int(levels[1]) == (2 if large else int(block_size == MIN_BLOCK_SIZE)) and
                int(version[1]) == fixture["hash_version"], "Independent htree shape or algorithm is incorrect")
        require(after["Free inodes"] == before["Free inodes"] - 3, "Inode allocation/removal accounting is incorrect")
        require(digest(source) == source_hash and digest(image) == image_hash, "Independent checking changed an input image")
        record.update(passed=True, added_names=added, levels=int(levels[1]), parent=parent,
                      retained_paths=["/alternate.txt"] if b"alternate.txt" in old_root else [],
                      hello=hello, objects=new_objects, accounting_before=before, accounting_after=after,
                      unchanged_inputs=True, raw_filename_mapping_verified=True)
        save()
        print(f"PASS {image.name}: {added} added byte names, htree, exact objects, accounting and fsck", flush=True)


if __name__ == "__main__":
    main()

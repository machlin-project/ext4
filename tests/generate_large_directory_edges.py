#!/usr/bin/env python3
"""Build bounded, independently checked LARGEDIR split and capacity fixtures.

Retain names and their hashes from e2fsprogs-authored directories. Underfilled
index nodes are legal after deletion; they let us fill a selected ancestor chain
without millions of names. debugfs owns block release and inode accounting. A
separate wire encoder writes only the retained directory blocks, and nonrepairing
e2fsck must accept the result before it becomes a test input.
"""

import argparse
from collections import Counter, namedtuple
import hashlib
import json
from pathlib import Path
import re
import shutil
import struct
import subprocess
import uuid

from check_namespace import inode_fields
from check_rename import entries as directory_entries
from generate_fixtures import UUID, resolve_tools
from generate_index_fixtures import lookup_expectations

DIRECTORY_HEADER = struct.Struct("<IHBB")
DIRECTORY_TAIL = struct.Struct("<IHBBI")
INDEX_ENTRY = struct.Struct("<II")
INDEX_COUNTS = struct.Struct("<HH")
INDEX_TAIL = struct.Struct("<II")
ROOT_PREFIX = struct.Struct("<IHBB4sIHBB4sIBBBB")
Root = namedtuple("Root", "dot_inode dot_record dot_length dot_type dot_name "
                  "parent_inode parent_record parent_length parent_type parent_name "
                  "reserved hash_version info_length levels flags")
LE32 = struct.Struct("<I")
DIRECTORY_ALIGNMENT = 4
FILE_REGULAR = 1
FILE_DIRECTORY = 2
DIRECTORY_CHECKSUM_TYPE = 0xDE
CRC32C_POLYNOMIAL = 0x82F63B78
CRC32C_INITIAL = 0xFFFFFFFF
ENTRIES_PER_LEAF = 3


def checksum(seed, data):
    """Independent raw Castagnoli polynomial division, without core code/tables."""
    for byte in data:
        seed ^= byte
        for _ in range(8):
            seed = (seed >> 1) ^ (CRC32C_POLYNOMIAL if seed & 1 else 0)
    return seed


def digest(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def node(children):
    return dict(children=children)


def lower_node(count):
    return node([dict() for _ in range(count)])


def topology(kind, root_limit, node_limit):
    if kind == "grow":
        return node([lower_node(node_limit)] +
                    [lower_node(1) for _ in range(root_limit - 1)]), 1
    first = node([lower_node(node_limit)] +
                 [lower_node(1) for _ in range(node_limit - 1)])
    count = root_limit if kind == "capacity" else 2
    return node([first] + [node([lower_node(1)]) for _ in range(count - 1)]), 2


def flatten(tree):
    blocks = []
    leaves = []

    def visit(item):
        item["logical"] = len(blocks)
        blocks.append(item)
        if "children" in item:
            for child in item["children"]:
                visit(child)
        else:
            leaves.append(item)

    visit(tree)
    return blocks, leaves


def names_from_dump(dump):
    pattern = r"^(\d+) 0x([0-9a-f]{8})-[0-9a-f]{8} \(\d+\) ([a-z0-9-]+)\s*$"
    names = [dict(inode=int(number), major=int(major, 16), name=name)
             for number, major, name in re.findall(pattern, dump, re.M)]
    if not names or len({entry["name"] for entry in names}) != len(names):
        raise RuntimeError("Independent HTree dump has missing or duplicate names")
    return sorted(names, key=lambda entry: entry["major"])


def select_names(names, count):
    children = [entry for entry in names if entry["name"] == "child"]
    if len(children) != 1:
        raise RuntimeError("Expected one retained child directory")
    # Use distinct hashes at all boundaries; collision continuations have their
    # own fixtures. Keep the existing child and sample the whole hash domain.
    unique = {entry["major"]: entry for entry in names
              if entry["name"] != "child" and entry["major"] != children[0]["major"]}
    regular = sorted(unique.values(), key=lambda entry: entry["major"])
    if len(regular) < count - 1:
        raise RuntimeError("Insufficient independently hashed names")
    selected = [regular[index * len(regular) // (count - 1)] for index in range(count - 1)]
    return sorted(selected + children, key=lambda entry: entry["major"])


def lower_hash(item):
    while "children" in item:
        item = item["children"][0]
    return item["entries"][0]["major"]


def encode_block(item, block_size, root, seed, metadata_checksum):
    data = bytearray(block_size)
    if "children" in item:
        base = ROOT_PREFIX.size if item["logical"] == 0 else DIRECTORY_HEADER.size
        tail_size = INDEX_TAIL.size if metadata_checksum else 0
        limit = (block_size - base - tail_size) // INDEX_ENTRY.size
        if item["logical"] == 0:
            ROOT_PREFIX.pack_into(data, 0, *root)
        else:
            DIRECTORY_HEADER.pack_into(data, 0, 0, block_size, 0, 0)
        for index, child in enumerate(item["children"]):
            INDEX_ENTRY.pack_into(data, base + index * INDEX_ENTRY.size,
                                  0 if index == 0 else lower_hash(child), child["logical"])
        count = len(item["children"])
        if not 0 < count <= limit:
            raise RuntimeError("Index encoder exceeded node capacity")
        INDEX_COUNTS.pack_into(data, base, limit, count)
        if metadata_checksum:
            offset = base + limit * INDEX_ENTRY.size
            value = checksum(checksum(seed, data[:base + count * INDEX_ENTRY.size]),
                             data[offset:offset + INDEX_TAIL.size])
            INDEX_TAIL.pack_into(data, offset, 0, value)
    else:
        usable = block_size - (DIRECTORY_TAIL.size if metadata_checksum else 0)
        offset = 0
        for index, entry in enumerate(item["entries"]):
            name = entry["name"].encode("ascii")
            length = (DIRECTORY_HEADER.size + len(name) + DIRECTORY_ALIGNMENT - 1) & -DIRECTORY_ALIGNMENT
            if index + 1 == len(item["entries"]):
                length = usable - offset
            if length < DIRECTORY_HEADER.size + len(name):
                raise RuntimeError("Directory encoder exceeded leaf capacity")
            kind = FILE_DIRECTORY if entry["name"] == "child" else FILE_REGULAR
            DIRECTORY_HEADER.pack_into(data, offset, entry["inode"], length, len(name), kind)
            data[offset + DIRECTORY_HEADER.size:offset + DIRECTORY_HEADER.size + len(name)] = name
            offset += length
        if metadata_checksum:
            DIRECTORY_TAIL.pack_into(data, usable, 0, DIRECTORY_TAIL.size, 0,
                                     DIRECTORY_CHECKSUM_TYPE, checksum(seed, data[:usable]))
    return data


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--tools-root", type=Path)
    parser.add_argument("--fixtures", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    tools = resolve_tools(args.tools_root)
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    sources = json.loads((args.fixtures / "report.json").read_text())
    records = []

    def save():
        (output / "report.json").write_text(json.dumps(records, indent=2) + "\n")

    for source in sources:
        if source["directories"]["indexed"]["levels"] != 2:
            continue
        original = args.fixtures / Path(source["image"]).name
        if source.get("passed") is not True or digest(original) != source["input_sha256"]:
            raise RuntimeError("Large-directory source is not an unchanged verified fixture")
        block_size = source["block_size"]
        metadata_checksum = "metadata_csum" in source["features"]
        tail_size = INDEX_TAIL.size if metadata_checksum else 0
        root_limit = (block_size - ROOT_PREFIX.size - tail_size) // INDEX_ENTRY.size
        node_limit = (block_size - DIRECTORY_HEADER.size - tail_size) // INDEX_ENTRY.size
        dump = next(c["stdout"] for c in source["commands"] if "htree_dump /indexed" in c["command"])
        names = names_from_dump(dump)
        directory = inode_fields(next(c["stdout"] for c in source["commands"]
                                     if "stat /indexed" in c["command"]))
        if len(names) + 2 != source["directories"]["indexed"]["entries"]:
            raise RuntimeError("Independent HTree dump lost directory entries")
        seed = checksum(CRC32C_INITIAL, uuid.UUID(UUID).bytes)
        seed = checksum(checksum(seed, LE32.pack(directory["inode"])),
                        LE32.pack(directory["generation"]))
        retained_links = Counter()
        for parent in ("/", "/peer"):
            listing = next(c["stdout"] for c in source["lookup"]["commands"]
                           if f"ls -p {parent}" in c["command"])
            retained_links.update(number for name, number in directory_entries(listing).items()
                                  if name not in (".", ".."))
        for kind in ("grow", "cascade", "capacity"):
            tree, levels = topology(kind, root_limit, node_limit)
            blocks, leaves = flatten(tree)
            selected = select_names(names, len(leaves) * ENTRIES_PER_LEAF)
            for index, leaf in enumerate(leaves):
                leaf["entries"] = selected[index * ENTRIES_PER_LEAF:(index + 1) * ENTRIES_PER_LEAF]
            image = output / f"{original.stem}-{kind}.img"
            record = dict(image=str(image), source=str(original), source_sha256=digest(original),
                          kind=kind, levels=levels, blocks=len(blocks), leaves=len(leaves),
                          entries=len(selected) + 2, features=source["features"], commands=[])
            records.append(record)

            def run(command):
                command = [str(part) for part in command]
                done = subprocess.run(command, capture_output=True, text=True,
                                      errors="strict", timeout=120)
                record["commands"].append(dict(command=command, status=done.returncode,
                                               stdout=done.stdout, stderr=done.stderr))
                save()
                if done.returncode != 0:
                    raise RuntimeError(f"Independent compact fixture command failed: {command}")
                return done.stdout

            shutil.copyfile(original, image)
            run([tools["debugfs"], "-w", "-R", f"punch /indexed {len(blocks)}", image])
            links = retained_links.copy()
            links.update(entry["inode"] for entry in selected if entry["name"] != "child")
            commands = [f"set_inode_field /indexed size {len(blocks) * block_size}"]
            regular_inodes = {entry["inode"] for entry in selected if entry["name"] != "child"}
            commands += [f"set_inode_field <{number}> links_count {links[number]}"
                         for number in sorted(regular_inodes)]
            commands += [f"bmap /indexed {index}" for index in range(len(blocks))]
            script = output / f"{image.stem}.debugfs"
            script.write_text("\n".join(commands) + "\n")
            mapped = run([tools["debugfs"], "-w", "-f", script, image])
            physical = [int(value) for value in re.findall(r"^(\d+)\s*$", mapped, re.M)]
            if len(physical) != len(blocks) or min(physical) == 0 or len(set(physical)) != len(physical):
                raise RuntimeError("Retained logical blocks are not independently mapped uniquely")
            with image.open("r+b") as stream:
                stream.seek(physical[0] * block_size)
                root = Root(*ROOT_PREFIX.unpack(stream.read(ROOT_PREFIX.size)))._replace(levels=levels)
                for item, number in zip(blocks, physical, strict=True):
                    stream.seek(number * block_size)
                    stream.write(encode_block(item, block_size, root, seed, metadata_checksum))
            run([tools["e2fsck"], "-fn", image])
            listing = run([tools["debugfs"], "-R", "ls -p /indexed", image])
            observed = directory_entries(listing)
            expected = {entry["name"]: entry["inode"] for entry in selected}
            if {name: number for name, number in observed.items() if name not in (".", "..")} != expected:
                raise RuntimeError("Independent directory listing differs from retained names")
            checked = inode_fields(run([tools["debugfs"], "-R", "stat /indexed", image]))
            if checked["size"] != len(blocks) * block_size or checked["links"] != directory["links"]:
                raise RuntimeError("Retained directory inode accounting differs")
            record["input_sha256"] = digest(image)
            record["lookup"] = lookup_expectations(image, ("indexed", "peer"),
                                                    tools["debugfs"], record["input_sha256"])
            if digest(original) != source["input_sha256"]:
                raise RuntimeError("Compact fixture creation changed its source")
            record["passed"] = True
            save()
            print(f"PASS {image.name}: levels={levels}, blocks={len(blocks)}, names={len(selected)}", flush=True)


if __name__ == "__main__":
    main()

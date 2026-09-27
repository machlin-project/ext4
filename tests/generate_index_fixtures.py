#!/usr/bin/env python3
"""Create independently indexed directories with explicit hash and format variations."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess

from generate_fixtures import EXPECTED_FEATURES, UUID, resolve_tools
from check_rename import entries as directory_entries
from check_namespace import inode_fields

IMAGE_BYTES = 64 * 1024 * 1024
INODE_INDEX = 0x1000
HASH_VERSIONS = {"legacy": 0, "half_md4": 1, "tea": 2}
HASH_FLAGS = {"signed": 0x0001, "unsigned": 0x0002}
NAME_MAX = 255
ORPHAN_BLOCKS = 4
INDEX_ROOT_PREFIX_BYTES = 32
INDEX_NODE_PREFIX_BYTES = 8
INDEX_ENTRY_BYTES = 8
INDEX_TAIL_BYTES = 8
DIRECTORY_HEADER_BYTES = 8
DIRECTORY_TAIL_BYTES = 12
DIRECTORY_ALIGNMENT = 4


def lookup_expectations(image, parents, debugfs, expected_hash):
    """Save independent name-to-inode identities without reading through the core."""
    def digest(path):
        with path.open("rb") as stream:
            return hashlib.file_digest(stream, "sha256").hexdigest()

    if digest(image) != expected_hash:
        raise RuntimeError(f"Indexed fixture changed before lookup inspection: {image}")
    commands = []
    rows = []
    directories = {}
    for parent in ["/"] + ["/" + name for name in parents]:
        outputs = []
        for operation in ("stat", "ls -p"):
            command = [str(debugfs), "-R", f"{operation} {parent}", str(image)]
            done = subprocess.run(command, capture_output=True, text=True,
                                  errors="strict", timeout=120)
            commands.append(dict(command=command, status=done.returncode,
                                 stdout=done.stdout, stderr=done.stderr))
            if done.returncode != 0:
                raise RuntimeError(f"Independent lookup inspection failed: {command}")
            outputs.append(done.stdout)
        inode = inode_fields(outputs[0])
        names = directory_entries(outputs[1])
        if inode is None or inode["type"] != "directory" or names.get(".") != inode["inode"]:
            raise RuntimeError(f"Invalid independent directory identity: {parent}")
        for name, number in names.items():
            # These generated fixture names are ASCII. Do not reinterpret
            # debugfs quoting as raw name bytes if that fixture contract changes.
            if not name.isascii() or "\\" in name or not 0 < len(name) <= NAME_MAX:
                raise RuntimeError(f"Unexpected encoded fixture name: {name!r}")
            rows.append(f"{inode['inode']} {name.encode('ascii').hex()} {number}\n")
        directories[parent] = dict(inode=inode["inode"], entries=len(names))
    if digest(image) != expected_hash:
        raise RuntimeError(f"Lookup inspection changed its input: {image}")
    expected = image.with_suffix(".lookup")
    expected.write_text("".join(rows), encoding="ascii")
    return dict(image=str(image), input_sha256=expected_hash, expected=str(expected),
                expected_sha256=digest(expected), entries=len(rows),
                directories=directories, commands=commands, passed=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--tools-root", type=Path)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--capacity", action="store_true", help="Generate only a full-root capacity fixture")
    parser.add_argument("--large-dir", action="store_true",
                        help="Generate LARGEDIR trees and a root ready to grow another level")
    parser.add_argument("--lookup-only", action="store_true",
                        help="Inspect existing verified images and save lookup expectations without changing images")
    args = parser.parse_args()
    if args.capacity and args.large_dir:
        parser.error("select one indexed fixture package")
    tools = resolve_tools(args.tools_root)
    output = args.output.resolve()
    if args.lookup_only:
        inspected = []
        for record in json.loads((output / "report.json").read_text()):
            if record.get("passed") is not True:
                raise RuntimeError("Lookup inspection requires a verified fixture")
            image = output / Path(record["image"]).name
            inspected.append(lookup_expectations(image, record["directories"],
                                                  tools["debugfs"], record["input_sha256"]))
            (output / "lookup-report.json").write_text(json.dumps(inspected, indent=2) + "\n")
            print(f"PASS independent lookup expectations {image.name}: {inspected[-1]['entries']}", flush=True)
        return
    output.mkdir(parents=True, exist_ok=False)
    profiles = []
    for algorithm in HASH_VERSIONS:
        for signedness in HASH_FLAGS:
            profiles.append(dict(name=f"index-{algorithm}-{signedness}", block_size=1024,
                                 algorithm=algorithm, signedness=signedness))
    for block_size in (2048, 4096, 8192, 16384, 32768):
        profiles.append(dict(name=f"index-{block_size // 1024}k", block_size=block_size))
    profiles += [dict(name="index-deep", deep=True),
                 dict(name="index-deep-unsigned", deep=True, signedness="unsigned"),
                 dict(name="index-indirect", exclude={"extent", "64bit", "flex_bg"}),
                 dict(name="index-no-checksum", exclude={"metadata_csum"}),
                 dict(name="index-no-filetype", exclude={"filetype"}),
                 dict(name="index-inode128", exclude={"extra_isize"}, inode_size=128),
                 dict(name="index-checksum-seed", include={"metadata_csum_seed"}),
                 dict(name="index-orphan-file", include={"orphan_file"}),
                 dict(name="index-orphan-file-deep", include={"orphan_file"}, deep=True)]
    if args.capacity:
        root_entries = (1024 - INDEX_ROOT_PREFIX_BYTES - INDEX_TAIL_BYTES) // INDEX_ENTRY_BYTES
        node_entries = (1024 - INDEX_NODE_PREFIX_BYTES - INDEX_TAIL_BYTES) // INDEX_ENTRY_BYTES
        record_bytes = (DIRECTORY_HEADER_BYTES + NAME_MAX + DIRECTORY_ALIGNMENT - 1) & ~(DIRECTORY_ALIGNMENT - 1)
        leaf_entries = (1024 - DIRECTORY_TAIL_BYTES) // record_bytes
        # Fill every root slot, leaving room in its last internal node. A split
        # under any preceding full node would require an additional root slot.
        entries = ((root_entries - 1) * node_entries + 1) * leaf_entries
        profiles = [dict(name="index-capacity", deep=True, entries=entries, parents=("indexed",), capacity=True)]
    if args.large_dir:
        profiles = [
            dict(name="index-large-dir", include={"large_dir"}),
            dict(name="index-large-dir-indirect", include={"large_dir"},
                 exclude={"extent", "64bit", "flex_bg"}),
            dict(name="index-large-dir-gdt", include={"large_dir", "uninit_bg"},
                 exclude={"metadata_csum"}),
            dict(name="index-large-dir-capacity", include={"large_dir"},
                 capacity=True, parents=("indexed",)),
        ]
        for profile in profiles:
            checksummed = "metadata_csum" not in profile.get("exclude", set())
            tail = INDEX_TAIL_BYTES if checksummed else 0
            root_entries = (1024 - INDEX_ROOT_PREFIX_BYTES - tail) // INDEX_ENTRY_BYTES
            node_entries = (1024 - INDEX_NODE_PREFIX_BYTES - tail) // INDEX_ENTRY_BYTES
            record_bytes = (DIRECTORY_HEADER_BYTES + NAME_MAX + DIRECTORY_ALIGNMENT - 1) & ~(DIRECTORY_ALIGNMENT - 1)
            leaf_entries = (1024 - (DIRECTORY_TAIL_BYTES if checksummed else 0)) // record_bytes
            capacity = profile.get("capacity", False)
            leaves = (root_entries - int(capacity)) * node_entries + 1
            profile.update(large=True, deep=True, levels=1 if capacity else 2,
                           entries=leaves * leaf_entries, root_entries=root_entries,
                           node_entries=node_entries)
    records = []

    def save():
        (output / "report.json").write_text(json.dumps(records, indent=2) + "\n")

    for profile in profiles:
        block_size = profile.get("block_size", 1024)
        algorithm = profile.get("algorithm", "half_md4")
        signedness = profile.get("signedness", "signed")
        entries = profile.get("entries", 512 if profile.get("deep") else 400 if block_size >= 16384 else 96)
        parents = profile.get("parents", ("indexed", "peer"))
        features = EXPECTED_FEATURES - profile.get("exclude", set()) | profile.get("include", set())
        tree = output / f"{profile['name']}-tree"
        tree.mkdir()
        (tree / "hello.txt").write_bytes(b"Machlin ext4\n")
        alternate = profile.get("capacity") or profile.get("large")
        if alternate:
            # Keep each host inode's link count below common host filesystem limits.
            (tree / "alternate.txt").write_bytes(b"Alternate ext4 target\n")
        for parent in parents:
            directory = tree / parent
            directory.mkdir()
            (directory / "child").mkdir()
            parent_entries = 512 if profile.get("large") and parent != "indexed" else entries
            for index in range(parent_entries):
                name = f"entry-{index:06d}-".ljust(NAME_MAX, "n")
                target = "alternate.txt" if alternate and index & 1 else "hello.txt"
                os.link(tree / target, directory / name)
        image = output / f"{profile['name']}.img"
        per_directory = {parent: (512 if profile.get("large") and parent != "indexed" else entries) + 3
                         for parent in parents}
        record = dict(image=str(image), block_size=block_size, hash_version=HASH_VERSIONS[algorithm],
                      signedness=signedness, entries_by_directory=per_directory, commands=[])
        if len(set(per_directory.values())) == 1:
            record["entries_per_directory"] = entries + 3
        records.append(record)

        def run(command, allowed=(0,)):
            done = subprocess.run([str(part) for part in command], capture_output=True,
                                  text=True, errors="backslashreplace", timeout=120)
            record["commands"].append(dict(command=[str(part) for part in command],
                                            status=done.returncode, stdout=done.stdout, stderr=done.stderr))
            if done.returncode not in allowed:
                save()
                raise RuntimeError(f"Fixture command failed: {command}")
            return done.stdout

        extended = "lazy_itable_init=0,lazy_journal_init=0,nodiscard"
        if "orphan_file" in features:
            extended += f",orphan_file_size={ORPHAN_BLOCKS}"
        run([tools["mke2fs"], "-F", "-t", "ext4", "-b", block_size, "-N", 4096,
             "-I", profile.get("inode_size", 256), "-O", "none," + ",".join(sorted(features)),
             "-U", UUID, "-E", extended,
             "-d", tree, image, IMAGE_BYTES // block_size])
        for field, value in (("def_hash_version", algorithm), ("hash_seed", UUID),
                             ("flags", HASH_FLAGS[signedness])):
            run([tools["debugfs"], "-w", "-R", f"set_super_value {field} {value}", image])
        run([tools["e2fsck"], "-fyD", image], allowed=(0, 1))
        run([tools["e2fsck"], "-fn", image])
        header = run([tools["dumpe2fs"], "-h", image])
        observed_features = re.search(r"^Filesystem features:\s+(.+)$", header, re.M)
        observed_flags = re.search(r"^Filesystem flags:\s+(.+)$", header, re.M)
        observed_seed = re.search(r"^Directory Hash Seed:\s+(\S+)$", header, re.M)
        if (observed_features is None or set(observed_features[1].split()) != features or
                observed_flags is None or set(observed_flags[1].split()) != {f"{signedness}_directory_hash"} or
                observed_seed is None or observed_seed[1] != UUID):
            save()
            raise RuntimeError("Indexed fixture feature/hash negotiation changed")
        if "orphan_file" in features:
            orphan_number = re.search(r"^Orphan file inode:\s+(\d+)$", header, re.M)
            if orphan_number is None:
                raise RuntimeError("Missing indexed fixture orphan-file identity")
            orphan = inode_fields(run([tools["debugfs"], "-R", f"stat <{orphan_number[1]}>", image]))
            if orphan is None or orphan["type"] != "regular" or orphan["size"] != ORPHAN_BLOCKS * block_size:
                save()
                raise RuntimeError("Indexed fixture orphan file has an unexpected type or size")
            record["orphan_file"] = orphan
        directories = {}
        for parent in parents:
            parent_entries = 512 if profile.get("large") and parent != "indexed" else entries
            expected_levels = (1 if profile.get("large") and parent != "indexed"
                               else profile.get("levels", int(bool(profile.get("deep")))))
            stat = run([tools["debugfs"], "-R", f"stat /{parent}", image])
            dump = run([tools["debugfs"], "-R", f"htree_dump /{parent}", image])
            listing = run([tools["debugfs"], "-R", f"ls -p /{parent}", image])
            flags = int(re.search(r"Flags:\s+0x([0-9a-f]+)", stat)[1], 16)
            levels = int(re.search(r"Indirect levels:\s+(\d+)", dump)[1])
            version = int(re.search(r"Hash Version:\s+(\d+)", dump)[1])
            leaves = {int(value) for value in re.findall(r"Reading directory block (\d+),", dump)}
            names = directory_entries(listing)
            if (not flags & INODE_INDEX or levels != expected_levels or
                    version != HASH_VERSIONS[algorithm] or len(leaves) < 2 or
                    len(names) != parent_entries + 3 or len(set(names)) != len(names)):
                save()
                raise RuntimeError("Indexed fixture did not produce its required tree shape")
            directories[parent] = dict(levels=levels, hash_version=version, leaf_blocks=len(leaves),
                                       entries=len(names))
            if profile.get("capacity"):
                root_entries = profile.get("root_entries", root_entries)
                node_entries = profile.get("node_entries", node_entries)
                counts = [int(value) for value in re.findall(r"Number of entries \(count\):\s+(\d+)", dump)]
                if not counts or counts[0] != root_entries or node_entries not in counts[1:]:
                    save()
                    raise RuntimeError("Capacity fixture did not fill the root and internal nodes")
                directories[parent].update(root_entries=counts[0], full_internal_nodes=counts[1:].count(node_entries))
        with image.open("rb") as stream:
            image_hash = hashlib.file_digest(stream, "sha256").hexdigest()
        record.update(directories=directories, input_sha256=image_hash, features=sorted(features), passed=True)
        record["lookup"] = lookup_expectations(image, parents, tools["debugfs"], image_hash)
        save()
        print(f"PASS {image.name}: {algorithm}/{signedness}, {directories['indexed']}", flush=True)


if __name__ == "__main__":
    main()

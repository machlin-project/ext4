#!/usr/bin/env python3
"""Create and independently inspect inode-body, external and shared ext4 xattrs."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import struct
import subprocess

from check_namespace import inode_fields
from generate_fixtures import EXPECTED_FEATURES, UUID, resolve_tools

XATTR_MAGIC = 0xEA020000
XATTR_BLOCK_HEADER = struct.Struct("<8I")
XATTR_HEADER_FIELDS = ("magic", "references", "blocks", "hash", "checksum",
                       "reserved0", "reserved1", "reserved2")
XATTR_ENTRY_BYTES = 16
XATTR_ALIGNMENT = 4
XATTR_END_BYTES = 4
ACL_VERSION = 1
ACL_USER_OBJ = 1
ACL_USER = 2
ACL_GROUP_OBJ = 4
ACL_GROUP = 8
ACL_MASK = 16
ACL_OTHER = 32
SECTOR_BYTES = 512
XATTR_PROFILES = (
    dict(name="xattr-1k"), dict(name="xattr-4k", block_size=4096),
    dict(name="xattr-16k", block_size=16384), dict(name="xattr-64k", block_size=65536),
    dict(name="xattr-indirect", exclude={"extent", "64bit", "flex_bg"}),
    dict(name="xattr-no-checksum", exclude={"metadata_csum"}),
    dict(name="xattr-checksum-seed", include={"metadata_csum_seed"}),
    dict(name="xattr-inode128", inode_size=128, exclude={"extra_isize"}),
    dict(name="xattr-inode512", inode_size=512),
    dict(name="xattr-orphan-file", include={"orphan_file"}),
)


def payload(size):
    return bytes((index * 29 + 0x83) & 255 for index in range(size))


def acl():
    result = struct.pack("<I", ACL_VERSION)
    for tag, permissions, identity in ((ACL_USER_OBJ, 7, None), (ACL_USER, 6, 70000),
                                      (ACL_GROUP_OBJ, 5, None), (ACL_GROUP, 4, 80000),
                                      (ACL_MASK, 6, None), (ACL_OTHER, 1, None)):
        result += struct.pack("<HH", tag, permissions)
        if identity is not None:
            result += struct.pack("<I", identity)
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--tools-root", type=Path)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    tools = resolve_tools(args.tools_root)
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    values = {"tiny": b"abc", "binary": payload(600), "small": b"\0\x01\x80\xff/",
              "empty": b"", "high": payload(9), "marker": payload(19), "acl": acl()}
    for label, value in values.items():
        (output / f"{label}.data").write_bytes(value)
    records = []

    def save():
        (output / "report.json").write_text(json.dumps(records, indent=2) + "\n")

    for profile in XATTR_PROFILES:
        block_size = profile.get("block_size", 1024)
        inode_size = profile.get("inode_size", 256)
        features = (EXPECTED_FEATURES - {"resize_inode"} - profile.get("exclude", set()) |
                    profile.get("include", set()))
        image_bytes = max(8 * 1024 * 1024, block_size * 2048)
        tree = output / f"{profile['name']}-tree"
        tree.mkdir()
        for name in ("plain", "body", "block", "shared", "many", "long-name", "capacity"):
            (tree / name).write_bytes(b"")
        (tree / "directory").mkdir()
        os.symlink("../plain", tree / "symlink")
        os.symlink("q" * 100, tree / "mapped-symlink")
        image = output / f"{profile['name']}.img"
        record = dict(image=str(image), block_size=block_size, inode_size=inode_size, commands=[])
        records.append(record)

        def require(condition, message):
            if not condition:
                save()
                raise RuntimeError(f"{image.name}: {message}")

        def run(command, allowed=(0,), raw=False):
            done = subprocess.run([str(part) for part in command], capture_output=True, timeout=120)
            record["commands"].append(dict(command=[str(part) for part in command], status=done.returncode,
                                            stdout=done.stdout.decode("utf-8", "backslashreplace"),
                                            stderr=done.stderr.decode("utf-8", "backslashreplace")))
            save()
            require(done.returncode in allowed, "Independent tool failed")
            return done.stdout if raw else done.stdout.decode("utf-8", "backslashreplace")

        def debug(command, write=False):
            return run([tools["debugfs"], *(["-w"] if write else []), "-R", command, image])

        extended = "lazy_itable_init=0,lazy_journal_init=0,nodiscard"
        if "orphan_file" in features:
            extended += ",orphan_file_size=4"
        run([tools["mke2fs"], "-F", "-t", "ext4", "-b", block_size, "-g", 1024,
             "-N", 256, "-I", inode_size, "-m", 0, "-O", "none," + ",".join(sorted(features)),
             "-U", UUID, "-E", extended, "-d", tree, image, image_bytes // block_size])

        full_name = "full"
        entry_bytes = (XATTR_ENTRY_BYTES + len(full_name) + XATTR_ALIGNMENT - 1) & ~(XATTR_ALIGNMENT - 1)
        capacity_size = block_size - XATTR_BLOCK_HEADER.size - entry_bytes - XATTR_END_BYTES
        capacity_file = f"{profile['name']}-capacity.data"
        (output / capacity_file).write_bytes(payload(capacity_size))
        cases = [("/body", 1, "tiny", "tiny.data"), ("/block", 1, "binary", "binary.data"),
                 ("/symlink", 1, "binary", "binary.data"), ("/mapped-symlink", 1, "binary", "binary.data"),
                 ("/many", 1, "small", "small.data"), ("/many", 1, "empty", "empty.data"),
                 ("/many", 1, "binary", "binary.data"), ("/many", 1, "high-é", "high.data"),
                 ("/many", 4, "marker", "marker.data"), ("/many", 2, "", "acl.data"),
                 ("/directory", 2, "", "acl.data"), ("/directory", 3, "", "acl.data"),
                 ("/long-name", 1, "n" * 250, "tiny.data"),
                 ("/capacity", 1, full_name, capacity_file)]
        prefixes = {1: "user.", 2: "system.posix_acl_access", 3: "system.posix_acl_default", 4: "trusted."}
        for path, namespace, name, source in cases:
            debug(f'ea_set -r -f "{output / source}" {path} "{prefixes[namespace]}{name}"', write=True)
        for path in ("/many", "/directory"):
            mode = "040761" if path == "/directory" else "0100761"
            debug(f"set_inode_field {path} mode {mode}", write=True)

        # Share the independently created external block. e2fsck performs the
        # reference-count/checksum update; final nonrepairing inspection must pass.
        block_stat = debug("stat /block")
        match = re.search(r"^File ACL:\s+(\d+)", block_stat, re.M)
        require(match is not None and int(match[1]) != 0, "Missing forced external xattr block")
        shared_block = int(match[1])
        debug(f"set_inode_field /shared file_acl {shared_block}", write=True)
        debug(f"set_inode_field /shared blocks {block_size // SECTOR_BYTES}", write=True)
        run([tools["e2fsck"], "-fy", image], allowed=(0, 1))
        run([tools["e2fsck"], "-fn", image])
        with image.open("rb") as stream:
            stream.seek(shared_block * block_size)
            header = dict(zip(XATTR_HEADER_FIELDS, XATTR_BLOCK_HEADER.unpack(stream.read(XATTR_BLOCK_HEADER.size))))
        require(header["magic"] == XATTR_MAGIC and header["references"] == 2 and header["blocks"] == 1,
                "Independent shared xattr reference count differs")
        cases.append(("/shared", 1, "binary", "binary.data"))
        objects = {}
        for path in sorted({item[0] for item in cases} | {"/plain"}):
            objects[path] = inode_fields(debug(f"stat {path}"))
            require(objects[path] is not None, f"Missing {path}")
        require(objects["/shared"]["blocks"] == block_size // SECTOR_BYTES and
                objects["/block"]["blocks"] == block_size // SECTOR_BYTES, "Shared attributes have wrong inode block accounting")
        require(objects["/body"]["blocks"] == (block_size // SECTOR_BYTES if inode_size == 128 else 0),
                "Tiny xattr did not exercise the intended inode-body location")
        require(objects["/many"]["blocks"] == block_size // SECTOR_BYTES and
                objects["/capacity"]["blocks"] == block_size // SECTOR_BYTES, "Missing external or full-capacity attributes")

        expected = ["/plain -1 - -"]
        for index, (path, namespace, name, source) in enumerate(cases):
            extracted = output / f"{profile['name']}-oracle-{index}.data"
            debug(f'ea_get -r -f "{extracted}" {path} "{prefixes[namespace]}{name}"')
            require(extracted.is_file() and extracted.read_bytes() == (output / source).read_bytes(),
                    f"Independent xattr differs: {path} {namespace}:{name}")
            expected.append(f"{path} {namespace} {name.encode().hex() or '-'} {extracted.name}")
        expected_file = output / f"{profile['name']}.expected"
        expected_file.write_text("\n".join(expected) + "\n")
        header_text = run([tools["dumpe2fs"], "-h", image])
        found = re.search(r"^Filesystem features:\s+(.+)$", header_text, re.M)
        require(found is not None and set(found[1].split()) == features, "Fixture feature profile differs")
        with image.open("rb") as stream:
            image_hash = hashlib.file_digest(stream, "sha256").hexdigest()
        record.update(passed=True, input_sha256=image_hash, expected=str(expected_file),
                      expected_sha256=hashlib.sha256(expected_file.read_bytes()).hexdigest(),
                      shared_block=shared_block, shared_header=header, objects=objects, attributes=len(cases),
                      largest_value=capacity_size)
        save()
        print(f"PASS {image.name}: {len(cases)} independently read xattrs, shared block and full-block value", flush=True)


if __name__ == "__main__":
    main()

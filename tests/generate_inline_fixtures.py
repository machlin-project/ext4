#!/usr/bin/env python3
"""Generate and independently check inode-resident files and directories."""

import argparse
import json
from pathlib import Path
import re
import subprocess
import struct

from generate_fixtures import EXPECTED_FEATURES, UUID, resolve_tools
from check_namespace import inode_fields

INLINE_DATA_FLAG = 0x10000000
SIZES = (1, 59, 60, 61, 120)
DIRECTORY_HEADER = struct.Struct("<IHBB")
INLINE_HEAD_BYTES = 60
DIRECTORY_TAIL_BYTES = 56
PROFILES = (
    dict(name="1k"), dict(name="4k", block_size=4096),
    dict(name="no-checksum", exclude={"metadata_csum"}),
    dict(name="checksum-seed", include={"metadata_csum_seed"}),
    dict(name="no-filetype", exclude={"filetype"}),
    dict(name="inode512", inode_size=512),
    dict(name="ea-inode", include={"ea_inode"}),
    dict(name="orphan-file", include={"orphan_file"}),
)


def payload(size):
    return bytes((index * 29 + 0x83) & 0xff for index in range(size))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--tools-root", type=Path)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    tools = resolve_tools(args.tools_root)
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    reports = []
    for profile in PROFILES:
        name = "inline-" + profile["name"]
        image = output / (name + ".img")
        tree = output / (name + "-tree")
        tree.mkdir()
        for size in SIZES:
            (tree / f"file{size}").write_bytes(payload(size))
        for directory in ("entries", "empty", "destination"):
            (tree / directory).mkdir()
        for index in range(4):
            (tree / "entries" / f"e{index}").write_bytes(payload(index + 1))
        block_size = profile.get("block_size", 1024)
        features = ((EXPECTED_FEATURES - {"resize_inode"} - profile.get("exclude", set())) |
                    {"inline_data"} | profile.get("include", set()))
        record = dict(image=str(image), block_size=block_size, commands=[], objects={})
        reports.append(record)

        def run(command):
            command = [str(value) for value in command]
            result = subprocess.run(command, capture_output=True, timeout=120)
            record["commands"].append(dict(command=command, status=result.returncode,
                                           stdout=result.stdout.decode("utf-8", "backslashreplace"),
                                           stderr=result.stderr.decode("utf-8", "backslashreplace")))
            (output / "report.json").write_text(json.dumps(reports, indent=2) + "\n")
            result.check_returncode()
            return result.stdout.decode()

        extended = "lazy_itable_init=0,lazy_journal_init=0,nodiscard"
        if "orphan_file" in features:
            extended += ",orphan_file_size=4"
        run([tools["mke2fs"], "-F", "-t", "ext4", "-b", block_size, "-g", 1024,
             "-N", 256, "-I", profile.get("inode_size", 256), "-m", 0,
             "-O", "none," + ",".join(sorted(features)), "-J", f"size={max(2, block_size // 1024)}",
             "-U", UUID, "-E", extended, "-d", tree, image, 16 * 1024 * 1024 // block_size])
        # e2fsprogs expands a full inline directory directly to a block.
        # Install its second independent region through its xattr/inode APIs
        # before linking the last two children, then require clean e2fsck.
        tail = bytearray(DIRECTORY_TAIL_BYTES)
        DIRECTORY_HEADER.pack_into(tail, 0, 0, DIRECTORY_TAIL_BYTES, 0, 0)
        source = output / (name + "-directory-tail.data")
        source.write_bytes(tail)
        run([tools["debugfs"], "-w", "-R", f'ea_set -f "{source}" /entries system.data', image])
        run([tools["debugfs"], "-w", "-R", f'sif /entries size {INLINE_HEAD_BYTES + DIRECTORY_TAIL_BYTES}', image])
        for index in range(4, 6):
            source = output / (name + f"-e{index}.data")
            source.write_bytes(payload(index + 1))
            run([tools["debugfs"], "-w", "-R", f'write "{source}" /entries/e{index}', image])
        for path in [f"/file{size}" for size in SIZES] + ["/entries", "/empty", "/destination"]:
            text = run([tools["debugfs"], "-R", f"stat {path}", image])
            fields = inode_fields(text)
            flags = int(re.search(r"Flags:\s+(0x[0-9a-fA-F]+)", text)[1], 16)
            if not flags & INLINE_DATA_FLAG:
                raise RuntimeError(f"{name}: {path} is not inline")
            record["objects"][path] = fields
            if path.startswith("/file"):
                target = output / (name + path[1:] + ".data")
                run([tools["debugfs"], "-R", f'dump {path} "{target}"', image])
                # libext2fs's inline reader returns storage capacity, including
                # padding beyond i_size. Check the independently decoded EOF
                # separately and require that padding to be zero.
                content = target.read_bytes()
                size = int(path[5:])
                if fields["size"] != size or len(content) < size or content[:size] != payload(size) or any(content[size:]):
                    raise RuntimeError(f"{name}: wrong inline bytes in {path}")
        run([tools["dumpe2fs"], "-h", image])
        run([tools["e2fsck"], "-fn", image])
        record["passed"] = True
        (output / "report.json").write_text(json.dumps(reports, indent=2) + "\n")
        print(f"PASS {name}: inline file boundary and directory regions", flush=True)


if __name__ == "__main__":
    main()

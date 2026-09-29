#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
"""Independently check fs-verity files that ext4-verity-enable-test enabled.

For every exported image, strict fsck must accept it. For every file in its manifest,
debugfs must report the verity flag and dump the expected contents, and the Merkle
tree, descriptor and size field read through the file's extents past EOF must equal
what generate_verity_fixtures.layout computes from those contents and the file's
parameters, with no other blocks mapped past EOF. The core's measured digest must
equal the independently computed file digest. A tenth field names a built-in
signature, next to the manifest, that must follow the descriptor. Kinds other than
"good" name files a Linux keyring must refuse. The output directory then holds a
report in the verity fixtures' format for tests/run_linux_verity.py."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess

from generate_fixtures import resolve_tools
from generate_verity_fixtures import layout

VERITY_FLAG = 0x00100000
# Files Linux must verify, and files a keyring requiring signatures must refuse.
KINDS = ("good", "unsigned", "badsig")
EXTENT = re.compile(r"^\s*(\d+)/\s*(\d+)\s+\d+/\s*\d+\s+(\d+)\s*-\s*(\d+)\s+(\d+)\s*-\s*(\d+)"
                    r"\s+(\d+)\s*(\S*)\s*$")
FLAGS = re.compile(r"Flags:\s+(0x[0-9a-f]+)")


def extents(listing):
    """Leaf extents (logical, physical, length, unwritten) from debugfs dump_extents."""
    rows = []
    for line in listing.splitlines():
        match = EXTENT.match(line)
        if match is None:
            continue
        level, depth = int(match[1]), int(match[2])
        if level != depth:
            continue
        rows.append((int(match[3]), int(match[5]), int(match[7]), match[8] == "Uninit"))
    return rows


def read_file(image, mapping, block_size, offset, length):
    """Read bytes of a file through its extents; holes and unwritten extents read zero."""
    output = bytearray(length)
    with image.open("rb") as stream:
        for logical, physical, count, unwritten in mapping:
            start = max(offset, logical * block_size)
            end = min(offset + length, (logical + count) * block_size)
            if start >= end or unwritten:
                continue
            stream.seek(physical * block_size + start - logical * block_size)
            output[start - offset:end - offset] = stream.read(end - start)
    return bytes(output)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--exports", required=True, type=Path)
    parser.add_argument("--tools-root", type=Path)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    tools = resolve_tools(args.tools_root)
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    rows = []

    def run(row, command):
        result = subprocess.run([str(x) for x in command], capture_output=True, text=True,
                                errors="backslashreplace", timeout=300)
        row["commands"].append(dict(command=[str(x) for x in command], status=result.returncode,
                                    stdout=result.stdout[-2000:], stderr=result.stderr[-2000:]))
        (output / "report.json").write_text(json.dumps(rows, indent=2) + "\n")
        result.check_returncode()
        return result.stdout

    for manifest in sorted(args.exports.resolve().glob("enable-*.manifest")):
        image = manifest.with_suffix(".img")
        row = dict(profile=manifest.stem, image=str(image), manifest=str(output / manifest.name),
                   commands=[], files=[], passed=False)
        rows.append(row)
        run(row, [tools["e2fsck"], "-fn", image])
        header, *lines = manifest.read_text().splitlines()
        fs_block = int(re.fullmatch(r"algorithm \d+ block (\d+)", header)[1])
        fixture_lines = [header]
        for line in lines:
            kind, name, size, sha256, digest, _, algorithm, merkle, salt, *signed = line.split()
            size, algorithm, merkle = int(size), int(algorithm), int(merkle)
            salt = b"" if salt == "-" else bytes.fromhex(salt)
            signature = b"" if not signed or signed[0] == "-" else \
                (manifest.parent / signed[0]).read_bytes()
            dump = output / f"{manifest.stem}-{name}.data"
            run(row, [tools["debugfs"], "-R", f'dump "/{name}" "{dump}"', image])
            data = dump.read_bytes()
            dump.unlink()
            if kind not in KINDS or len(data) != size or \
                    hashlib.sha256(data).hexdigest() != sha256:
                raise RuntimeError(f"{manifest.stem}/{name}: contents differ")
            flags = int(FLAGS.search(run(row, [tools["debugfs"], "-R", f'stat "/{name}"',
                                               image]))[1], 16)
            if not flags & VERITY_FLAG:
                raise RuntimeError(f"{manifest.stem}/{name}: verity flag missing")
            pieces, expected_digest, metadata = layout(data, fs_block, merkle, algorithm, salt,
                                                       signature=signature)
            if expected_digest != digest:
                raise RuntimeError(f"{manifest.stem}/{name}: measured digest differs")
            mapping = extents(run(row, [tools["debugfs"], "-R", f'dump_extents "/{name}"',
                                        image]))
            size_offset, size_field = pieces[-1]
            end_block = (size_offset + len(size_field)) // fs_block
            data_blocks = -(-size // fs_block)
            for logical, _, count, _ in mapping:
                # Nothing is mapped between the data and the tree, or after the size.
                if logical + count > end_block or \
                        max(logical, data_blocks) < min(logical + count, metadata // fs_block):
                    raise RuntimeError(f"{manifest.stem}/{name}: unexpected mapping past EOF")
            if not mapping or max(logical + count for logical, _, count, _ in mapping) != \
                    end_block:
                raise RuntimeError(f"{manifest.stem}/{name}: size field is not the last block")
            for offset, payload in pieces[1:]:
                if read_file(image, mapping, fs_block, offset, len(payload)) != bytes(payload):
                    raise RuntimeError(f"{manifest.stem}/{name}: metadata at {offset} differs")
            fixture_lines.append(" ".join((kind, name, str(size), sha256, digest, "-")))
            row["files"].append(dict(name=name, kind=kind, size=size, algorithm=algorithm,
                                     merkle_block=merkle, salt=salt.hex(), file_digest=digest,
                                     signature_bytes=len(signature)))
        (output / manifest.name).write_text("\n".join(fixture_lines) + "\n")
        row["passed"] = True
        (output / "report.json").write_text(json.dumps(rows, indent=2) + "\n")
        print(f"PASS independent verity enable {manifest.stem}: {len(lines)} files", flush=True)
    if not rows:
        raise RuntimeError("No verity enable exports found")


if __name__ == "__main__":
    main()

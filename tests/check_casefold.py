#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
"""Independently check casefolded directories changed by a writable core owner.

Strict e2fsck recomputes every casefolded directory hash, so it verifies that the
core placed new names in the index leaves Linux would search. debugfs then checks
the expected names, removals and inherited flags."""
import argparse
import json
from pathlib import Path
import struct
import subprocess

from check_namespace import inode_fields
from check_rename import entries
from generate_fixtures import resolve_tools

CASEFOLD_FLAG = 0x40000000
INDEX_FLAG = 0x00001000
NEW_NAMES = 600
# The zero-width space folds to an empty name.
PRESENT = ["Neue-Datei-%04d-ÄÖÜ" % index for index in range(NEW_NAMES)] + [
    "Café-Neu", "Unterordner", "\u200b"]
REMOVED = ("café", "ﬁle")
OPAQUE = b"bad\xffx"
SUBDIRECTORY = "Grüße"
TOGGLED_NAME = "Datei"
# dumpe2fs does not print s_encoding_flags, so read it from the primary superblock.
SUPERBLOCK_OFFSET = 1024
ENCODING_FLAGS_OFFSET = 0x27E
ENCODING_STRICT = 0x0001


def printed(name):
    """Render a name the way debugfs ls -p output is decoded here."""
    return name.decode(errors="replace")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--tools-root", type=Path, required=True)
    parser.add_argument("--exports", type=Path, required=True, action="append",
                        help="directory holding one casefold-mutated-*.img export")
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    tools = resolve_tools(args.tools_root)
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    rows = []

    def run(row, command):
        command = [str(part) for part in command]
        done = subprocess.run(command, capture_output=True, text=True, errors="replace",
                              timeout=300, check=False)
        row["commands"].append(dict(command=command, status=done.returncode,
                                    stdout=done.stdout[-4000:], stderr=done.stderr[-4000:]))
        if done.returncode != 0:
            raise RuntimeError(f"Command failed ({done.returncode}): {command}\n{done.stderr}")
        return done.stdout

    for directory in args.exports:
        images = sorted(directory.glob("casefold-mutated-*.img"))
        if len(images) != 1:
            raise RuntimeError(f"{directory}: expected one casefold export")
        image = images[0].resolve()
        row = dict(profile=directory.name, image=str(image), commands=[], passed=False)
        rows.append(row)
        run(row, [tools["e2fsck"], "-fn", image])
        with image.open("rb") as stream:
            stream.seek(SUPERBLOCK_OFFSET + ENCODING_FLAGS_OFFSET)
            strict = struct.unpack("<H", stream.read(2))[0] & ENCODING_STRICT != 0
        names = set(entries(run(row, [tools["debugfs"], "-R", "ls -p cf", image])))
        expected = {printed(name.encode()) for name in PRESENT}
        if not strict:
            expected.add(printed(OPAQUE))
        if not expected <= names:
            raise RuntimeError(f"{directory.name}: {len(expected - names)} names are missing")
        if names & {printed(name.encode()) for name in REMOVED}:
            raise RuntimeError(f"{directory.name}: removed or renamed names remain")
        parent = inode_fields(run(row, [tools["debugfs"], "-R", "stat cf", image]))
        child = inode_fields(run(row, [tools["debugfs"], "-R", "stat cf/Unterordner", image]))
        if (parent["flags"] & (CASEFOLD_FLAG | INDEX_FLAG) != CASEFOLD_FLAG | INDEX_FLAG or
                not child["flags"] & CASEFOLD_FLAG or child["type"] != "directory"):
            raise RuntimeError(f"{directory.name}: casefold or index flags are wrong")
        grandchildren = entries(run(row, [tools["debugfs"], "-R", "ls -p cf/Unterordner", image]))
        if printed(SUBDIRECTORY.encode()) not in grandchildren:
            raise RuntimeError(f"{directory.name}: inherited directory lost its name")
        toggled = inode_fields(run(row, [tools["debugfs"], "-R", "stat plain", image]))
        if (not toggled["flags"] & CASEFOLD_FLAG or
                TOGGLED_NAME not in entries(run(row, [tools["debugfs"], "-R", "ls -p plain",
                                                      image]))):
            raise RuntimeError(f"{directory.name}: directory with enabled casefolding is wrong")
        row["passed"] = True
        (output / "report.json").write_text(json.dumps(rows, indent=2) + "\n")
        print(f"PASS casefold export {directory.name}: strict e2fsck hashes, {len(expected)} "
              "names, removals, inherited and enabled flags", flush=True)


if __name__ == "__main__":
    main()

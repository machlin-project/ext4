#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
"""Independently check fs-verity images changed by a writable core owner."""
import argparse
import json
from pathlib import Path
import subprocess

from check_namespace import inode_fields
from check_rename import entries
from generate_fixtures import resolve_tools

VERITY_FLAG = 0x00100000
EXTENTS_FLAG = 0x00080000
RENAMED = "verity-renamed"
PROTECTED_PERMISSIONS = 0o400
NOTE = "user.note"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--tools-root", type=Path, required=True)
    parser.add_argument("--exports", type=Path, required=True, action="append",
                        help="directory holding verity-mutated.img and its manifest")
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
        image = (directory / "verity-mutated.img").resolve()
        manifest = (directory / "verity-mutated.manifest").resolve()
        row = dict(profile=directory.name, image=str(image), manifest=str(manifest),
                   commands=[], passed=False)
        rows.append(row)
        run(row, [tools["e2fsck"], "-fn", image])
        names = [line.split()[1] for line in manifest.read_text().splitlines()[1:]]
        present = set(entries(run(row, [tools["debugfs"], "-R", "ls -p /", image])))
        if not set(names) <= present:
            raise RuntimeError(f"{directory.name}: manifest names are missing")
        inode = inode_fields(run(row, [tools["debugfs"], "-R", f"stat {RENAMED}", image]))
        if (inode["flags"] & (VERITY_FLAG | EXTENTS_FLAG) != VERITY_FLAG | EXTENTS_FLAG or
                inode["mode"] & 0o7777 != PROTECTED_PERMISSIONS or inode["links"] != 1):
            raise RuntimeError(f"{directory.name}: renamed verity inode lost its state")
        if NOTE not in run(row, [tools["debugfs"], "-R", f"ea_list {RENAMED}", image]):
            raise RuntimeError(f"{directory.name}: attribute written on the verity file is missing")
        row["passed"] = True
        (output / "report.json").write_text(json.dumps(rows, indent=2) + "\n")
        print(f"PASS verity export {directory.name}: strict e2fsck, names, flags and attribute",
              flush=True)


if __name__ == "__main__":
    main()

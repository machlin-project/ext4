#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
"""Independently check quota files written by a writable core owner.

Strict e2fsck recomputes every user, group and project usage from the inodes and
fails on any difference. debugfs then reads the quota trees for the IDs whose
charges the core test moved, released or created."""
import argparse
import json
from pathlib import Path
import re
import subprocess

from generate_fixtures import resolve_tools

TYPES = ("user", "group", "project")
# (type, id, inodes) the scenario must leave, independent of block geometry.
EXPECTED = (
    ("user", 2000, 0), ("user", 2004, 0), ("user", 2006, 0), ("user", 0x01000000, 0),
    ("user", 8000, 0), ("user", 2039, 1), ("user", 0xfffffffe, 1), ("user", 1000, 2),
    ("group", 0x01000000, 0), ("group", 0x00010000, 1),
)
PROJECT_EXPECTED = (("project", 42, 3), ("project", 7, 1))


def usage(text):
    result = {}
    for line in text.splitlines():
        fields = line.split()
        if len(fields) == 7 and fields[0].isdigit():
            result[int(fields[0])] = (int(fields[1]), int(fields[4]))
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--tools-root", type=Path, required=True)
    parser.add_argument("--exports", type=Path, required=True, action="append",
                        help="directory holding quota-mutated-*.img and fault results")
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
            raise RuntimeError(f"Command failed ({done.returncode}): {command}\n{done.stdout}")
        return done.stdout

    for directory in args.exports:
        images = sorted(directory.glob("quota-*.img"))
        mutated = [image for image in images if image.name.startswith("quota-mutated-")]
        if len(mutated) != 1 or len(images) < 3:
            raise RuntimeError(f"{directory}: expected a scenario export and fault results")
        row = dict(profile=directory.name, images=[str(x) for x in images], commands=[],
                   passed=False)
        rows.append(row)
        for image in images:
            run(row, [tools["e2fsck"], "-fn", image])
        state = run(row, [tools["dumpe2fs"], "-h", mutated[0]])
        features = re.search(r"Filesystem features:(.*)", state)[1].split()
        expected = EXPECTED + (PROJECT_EXPECTED if "project" in features else ())
        tables = {name: usage(run(row, [tools["debugfs"], "-R", f"list_quota {name}",
                                        mutated[0]]))
                  for name in TYPES[:3 if "project" in features else 2]}
        for name, number, inodes in expected:
            seen = tables[name].get(number, (0, 0))[1]
            if seen != inodes:
                raise RuntimeError(f"{directory.name}: {name} {number} has {seen} inodes, "
                                   f"expected {inodes}")
        row["passed"] = True
        (output / "report.json").write_text(json.dumps(rows, indent=2) + "\n")
        print(f"PASS quota export {directory.name}: strict e2fsck usage on {len(images)} "
              f"images and {len(expected)} expected entries", flush=True)


if __name__ == "__main__":
    main()

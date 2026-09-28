#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
"""Independently check core multi-mount-protection exports with e2fsprogs."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess

from generate_fixtures import resolve_tools
from generate_mmp_fixtures import MMP_SEQUENCE_CLEAN, mmp_fields

CORE_NODE = "machlin-test"
CHECK_MULTIPLIER = 2
MAX_CHECK_INTERVAL = 300


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--tools-root", type=Path, required=True)
    parser.add_argument("--exports", type=Path, required=True)
    parser.add_argument("--recover", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--e2fsck-write", action="append", default=[],
                        help="also let e2fsprogs acquire this released profile read-write")
    args = parser.parse_args()
    tools = resolve_tools(args.tools_root)
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    rows = []

    def run(row, command, timeout=600):
        command = [str(part) for part in command]
        done = subprocess.run(command, capture_output=True, text=True, errors="replace",
                              timeout=timeout, check=False)
        row["commands"].append(dict(command=command, status=done.returncode,
                                    stdout=done.stdout[-4000:], stderr=done.stderr[-4000:]))
        (output / "report.json").write_text(json.dumps(rows, indent=2) + "\n")
        if done.returncode != 0:
            raise RuntimeError(f"Command failed ({done.returncode}): {command}\n{done.stderr}")
        return done.stdout

    def released_state(row, image, names):
        run(row, [tools["e2fsck"], "-fn", image])
        mmp = mmp_fields(run(row, [tools["debugfs"], "-R", "dump_mmp", image]))
        listing = run(row, [tools["debugfs"], "-R", "ls -p /", image])
        present = {line.split("/")[5] for line in listing.splitlines() if line.count("/") == 7}
        if mmp["sequence"] != MMP_SEQUENCE_CLEAN or not names <= present:
            raise RuntimeError(f"{image.name}: MMP not clean or namespace incomplete")
        return mmp

    released = sorted(args.exports.glob("mmp-released-*.img"))
    pending = sorted(args.exports.glob("mmp-pending-*.img"))
    if not released or len(released) != len(pending):
        raise RuntimeError("Expected released and pending exports for each profile")
    for image in released:
        profile = image.name.removeprefix("mmp-released-mmp-").removesuffix(".img")
        row = dict(profile=profile, kind="released", commands=[], passed=False,
                   sha256=digest(image))
        rows.append(row)
        mmp = released_state(row, image, {"first", "second"})
        text = run(row, [tools["debugfs"], "-R", "dump_mmp", image])
        interval = min(CHECK_MULTIPLIER * mmp["interval"], MAX_CHECK_INTERVAL)
        if f"node_name: {CORE_NODE}" not in text or f"check_interval: {interval}" not in text:
            raise RuntimeError(f"{image.name}: core owner fields are missing")
        if profile in args.e2fsck_write:
            copy = output / f"e2fsck-{profile}.img"
            shutil.copyfile(image, copy)
            # e2fsprogs performs its own acquisition wait on the core's clean state.
            run(row, [tools["e2fsck"], "-fy", copy])
            released_state(row, copy, {"first", "second"})
            copy.unlink()
            row["e2fsck_write"] = True
        if digest(image) != row["sha256"]:
            raise RuntimeError("Independent inspection changed the export")
        row["passed"] = True
        print(f"PASS mmp released {profile}: strict e2fsck, CLEAN, core owner fields", flush=True)
    for image in pending:
        profile = image.name.removeprefix("mmp-pending-mmp-").removesuffix(".img")
        row = dict(profile=profile, kind="pending", commands=[], passed=False,
                   sha256=digest(image))
        rows.append(row)
        copy = output / f"recovered-{profile}.img"
        shutil.copyfile(image, copy)
        text = run(row, [args.recover, "--write", copy])
        if "recovery: success; transactions=1 " not in text:
            raise RuntimeError(f"{image.name}: unexpected recovery report: {text}")
        released_state(row, copy, {"recovered"})
        copy.unlink()
        row["passed"] = True
        print(f"PASS mmp pending {profile}: POSIX recovery waited, replayed and released",
              flush=True)


if __name__ == "__main__":
    main()

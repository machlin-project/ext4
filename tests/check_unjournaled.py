#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
"""Repair every exported power-cut image of a volume without a journal with e2fsck.

ext4-unjournaled-test --export leaves one image per power cut that left the volume
in use, and a manifest of the files synced before every operation. e2fsck -fy must
repair each image, a second strict e2fsck -fn must find nothing, and every manifest
file must keep its size, contents and symlink target. When a torn primary superblock
fails its checksum, e2fsck repairs from the first backup superblock of the pristine
fixture, as it advises."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil
import subprocess

from generate_fixtures import resolve_tools

# e2fsck exit codes: no errors, errors corrected, errors corrected and a reboot
# requested for a mounted volume.
REPAIRED = {0, 1, 2}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--tools-root", type=Path, required=True)
    parser.add_argument("--exports", type=Path, required=True)
    parser.add_argument("--fixtures", type=Path, required=True,
                        help="the pristine volumes, for their backup superblocks")
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    tools = resolve_tools(args.tools_root)
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    rows = []
    for manifest in sorted(args.exports.glob("unjournaled-*.manifest")):
        source = manifest.name[len("unjournaled-"):-len(".manifest")]
        expected = [line.split(" ") for line in manifest.read_text().splitlines()]
        images = sorted(args.exports.glob(f"unjournaled-*-[0-9][0-9][0-9]-{source}"))
        if not images:
            raise RuntimeError(f"No power-cut images of {source}")
        repaired_counts = dict.fromkeys(sorted(REPAIRED), 0)
        header = subprocess.run([str(tools["dumpe2fs"]), str(args.fixtures / source)],
                                capture_output=True, text=True, check=True).stdout
        backup = re.search(r"Backup superblock at (\d+)", header)
        block_size = re.search(r"^Block size:\s+(\d+)", header, re.M)
        torn = 0
        for image in images:
            row = dict(image=image.name, commands=[], passed=False)
            rows.append(row)
            work = output / image.name
            shutil.copyfile(image, work)

            def run(command, allowed=(0,), row=row):
                done = subprocess.run([str(part) for part in command], capture_output=True,
                                      text=True, errors="replace", timeout=300)
                row["commands"].append(dict(command=[str(part) for part in command],
                                            status=done.returncode,
                                            stdout=done.stdout[-3000:],
                                            stderr=done.stderr[-2000:]))
                if done.returncode not in allowed:
                    raise RuntimeError(f"{image.name}: {command[0]} returned {done.returncode}")
                return done

            repair = run([tools["e2fsck"], "-fy", work], REPAIRED | {8})
            if repair.returncode == 8:
                if backup is None or "Superblock checksum does not match" not in repair.stderr:
                    raise RuntimeError(f"{image.name}: e2fsck could not open the volume")
                repair = run([tools["e2fsck"], "-fy", "-b", backup[1], "-B", block_size[1],
                              work], REPAIRED)
                torn += 1
            repaired_counts[repair.returncode] += 1
            run([tools["e2fsck"], "-fn", work])
            for fields in expected:
                if fields[0] == "file":
                    dump = output / f"{image.name}.dump"
                    run([tools["debugfs"], "-R", f'dump "/{fields[1]}" "{dump}"', work])
                    data = dump.read_bytes()
                    dump.unlink()
                    if len(data) != int(fields[2]) or \
                            hashlib.sha256(data).hexdigest() != fields[3]:
                        raise RuntimeError(f"{image.name}: {fields[1]} changed")
                else:
                    stat = run([tools["debugfs"], "-R", f'stat "/{fields[1]}"', work]).stdout
                    match = re.search(r'Fast link dest: "(.*)"', stat)
                    if match is None or match[1] != fields[2]:
                        raise RuntimeError(f"{image.name}: {fields[1]} target changed")
            work.unlink()
            row["passed"] = True
            (output / "report.json").write_text(json.dumps(rows, indent=2) + "\n")
        print(f"PASS unjournaled {source}: e2fsck repaired {len(images)} power-cut images "
              f"(exit codes {repaired_counts}, {torn} from a backup superblock) and kept "
              f"every synced file", flush=True)
    if not rows:
        raise RuntimeError("No unjournaled exports")
    (output / "report.json").write_text(json.dumps(rows, indent=2) + "\n")


if __name__ == "__main__":
    main()

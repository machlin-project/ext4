#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
"""Replay exported logs of several committed transactions with e2fsck and the core.

The lazy-checkpointing exports of ext4-deferred-test hold every transaction committed
since the last checkpoint. debugfs must walk at least two of them, e2fsck's replay-only
mode must replay the log into a volume that strict fsck accepts, and ext4-recover must
produce the same bytes except the primary superblock, whose times and counters the two
replayers maintain differently. A repairing e2fsck is not used: it also reorganizes
directories it could index, which is policy rather than replay."""
import argparse
import json
from pathlib import Path
import re
import shutil
import subprocess

from generate_fixtures import resolve_tools

SUPER_OFFSET = 1024
SUPER_SIZE = 1024
MIN_TRANSACTIONS = 2
# e2fsck exit statuses: a clean volume, or one whose journal replay it reports.
E2FSCK_CLEAN = 0
E2FSCK_CORRECTED = 1
COMMIT_RECORD = re.compile(r"Found expected sequence (\d+), type 2 \(commit block\)")


def without_superblock(path):
    data = bytearray(path.read_bytes())
    data[SUPER_OFFSET:SUPER_OFFSET + SUPER_SIZE] = bytes(SUPER_SIZE)
    return bytes(data)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--exports", required=True, type=Path)
    parser.add_argument("--recover", required=True, type=Path, help="ext4-recover executable")
    parser.add_argument("--tools-root", type=Path)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    tools = resolve_tools(args.tools_root)
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    rows = []

    def run(command, row, accepted=(0,)):
        result = subprocess.run([str(x) for x in command], capture_output=True, text=True,
                                errors="backslashreplace", timeout=300)
        row["commands"].append(dict(command=[str(x) for x in command], status=result.returncode,
                                    stdout=result.stdout[-4000:], stderr=result.stderr[-4000:]))
        (output / "report.json").write_text(json.dumps(rows, indent=2) + "\n")
        if result.returncode not in accepted:
            raise RuntimeError(f"{command[0]} returned {result.returncode} for {row['image']}")
        return result.stdout

    for image in sorted(args.exports.resolve().glob("lazy-*.img")):
        row = dict(image=image.name, commands=[], passed=False)
        rows.append(row)
        sequences = [int(x) for x in COMMIT_RECORD.findall(
            run([tools["debugfs"], "-R", "logdump", image], row))]
        if len(sequences) < MIN_TRANSACTIONS or \
                sequences != list(range(sequences[0], sequences[0] + len(sequences))):
            raise RuntimeError(f"{image.name}: expected consecutive committed transactions, "
                               f"found {sequences}")
        replayed = output / f"e2fsck-{image.name}"
        shutil.copyfile(image, replayed)
        run([tools["e2fsck"], "-y", "-E", "journal_only", replayed], row,
            (E2FSCK_CLEAN, E2FSCK_CORRECTED))
        run([tools["e2fsck"], "-fn", replayed], row)
        recovered = output / f"core-{image.name}"
        shutil.copyfile(image, recovered)
        run([args.recover, "--write", recovered], row)
        run([tools["e2fsck"], "-fn", recovered], row)
        if without_superblock(replayed) != without_superblock(recovered):
            raise RuntimeError(f"{image.name}: e2fsck and core replay differ")
        row.update(transactions=len(sequences), passed=True)
        (output / "report.json").write_text(json.dumps(rows, indent=2) + "\n")
        replayed.unlink()
        recovered.unlink()
        print(f"PASS pending log {image.name}: {len(sequences)} transactions", flush=True)
    if not rows:
        raise RuntimeError("No lazy-checkpointing exports found")


if __name__ == "__main__":
    main()

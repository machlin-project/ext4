#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
"""Run the scale workloads on fresh e2fsprogs-authored volumes and record them.

Each workload runs on a new copy of a 1 GiB volume, 4 KiB and 1 KiB, held in memory
by ext4-scale, so device flushes cost nothing and times measure the core. Strict fsck
must accept the image each workload leaves."""
import argparse
import json
from pathlib import Path
import platform
import subprocess

from generate_fixtures import resolve_tools

VOLUME_BYTES = 1024 * 1024 * 1024
PROFILES = (("4k", 4096), ("1k", 1024))
WORKLOADS = (("sequential", 256), ("reclamation", 256), ("directory", 50000),
             ("fragmented", 20000))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--tools-root", type=Path, required=True)
    parser.add_argument("--scale", type=Path, required=True, help="ext4-scale executable")
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--label", required=True, help="name of the measured core revision")
    parser.add_argument("--commit-blocks", type=int, default=0,
                        help="deferred commit capacity; zero commits every mutation")
    args = parser.parse_args()
    tools = resolve_tools(args.tools_root)
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    rows = []
    for profile, block in PROFILES:
        image = output / f"scale-{profile}.img"
        subprocess.run([str(tools["mke2fs"]), "-q", "-F", "-t", "ext4", "-b", str(block),
                        "-m", "0", "-E", "lazy_itable_init=0,nodiscard", str(image),
                        str(VOLUME_BYTES // block)], check=True)
        result = output / f"scale-{profile}-result.img"
        for workload, size in WORKLOADS:
            options = ["--commit-blocks", str(args.commit_blocks)] if args.commit_blocks else []
            done = subprocess.run([str(args.scale), *options, str(image), workload, str(size),
                                   str(result)],
                                  capture_output=True, text=True, timeout=1800, check=True)
            subprocess.run([str(tools["e2fsck"]), "-fn", str(result)], capture_output=True,
                           timeout=1800, check=True)
            result.unlink()
            for line in done.stdout.splitlines():
                row = json.loads(line)
                row.update(label=args.label, profile=profile, size=size,
                           commit_blocks=args.commit_blocks, machine=platform.machine())
                rows.append(row)
        image.unlink()
    with (output / "measurements.jsonl").open("w") as stream:
        for row in rows:
            stream.write(json.dumps(row) + "\n")
    for row in rows:
        if row["workload"] != "directory-create-batch":
            print(f"{args.label} {row['profile']} {row['workload']}: "
                  f"{row['elapsed_ms']:.1f} ms, {row['reads']} reads, {row['writes']} writes, "
                  f"{row['flushes']} flushes, amplification {row['write_amplification']:.2f}, "
                  f"peak {row['peak_live_bytes']} bytes", flush=True)


if __name__ == "__main__":
    main()

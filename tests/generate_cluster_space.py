#!/usr/bin/env python3
"""Fill cluster fixtures to zero free blocks without reserved-pool simulation."""

import argparse
import json
from pathlib import Path
import re
import shutil
import subprocess

from check_orphans import accounting, digest
from generate_fixtures import resolve_tools


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--fixtures", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--tools-root", type=Path)
    args = parser.parse_args()
    tools = resolve_tools(args.tools_root)
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    payload = output / "filler.data"
    payload.write_bytes(b"F" * (32 * 1024 * 1024))
    reports = []
    sources = json.loads(args.fixtures.read_text())
    for name in ("cluster-1k.img", "cluster-4k.img"):
        source = next(row for row in sources if Path(row["image"]).name == name)
        if not source.get("passed"):
            raise RuntimeError("Unverified source fixture")
        image = output / name
        shutil.copyfile(source["image"], image)
        row = dict(image=str(image), block_size=source["block_size"],
                   cluster_blocks=source["cluster_blocks"], commands=[])
        reports.append(row)

        def run(command):
            command = [str(item) for item in command]
            result = subprocess.run(command, capture_output=True, timeout=120)
            row["commands"].append(dict(command=command, status=result.returncode,
                                         stdout=result.stdout.decode("utf-8", "backslashreplace"),
                                         stderr=result.stderr.decode("utf-8", "backslashreplace")))
            (output / "report.json").write_text(json.dumps(reports, indent=2) + "\n")
            result.check_returncode()
            return result.stdout.decode()

        # debugfs's write reports ENOSPC through stderr; the bitmap, counter
        # and nonrepairing checks establish that its partial allocation is valid.
        run([tools["debugfs"], "-w", "-R", f'write "{payload}" /filler', image])
        run([tools["e2fsck"], "-fn", image])
        counts = accounting(run([tools["dumpe2fs"], "-h", image]))
        groups = run([tools["dumpe2fs"], image])
        free = re.findall(r"^\s+(\d+) free clusters,", groups, re.M)
        if counts["Free blocks"] != 0 or not free or any(int(x) for x in free) or "BLOCK_UNINIT" in groups:
            raise RuntimeError("Not every block group is allocated")
        row.update(passed=True, image_sha256=digest(image), accounting=counts)
        (output / "report.json").write_text(json.dumps(reports, indent=2) + "\n")
        print(f"PASS {name}: every block allocated, strict fsck clean", flush=True)


if __name__ == "__main__":
    main()

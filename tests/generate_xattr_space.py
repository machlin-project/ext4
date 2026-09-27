#!/usr/bin/env python3
"""Fill independently verified xattr images to test shared-block ENOSPC rollback."""

import argparse
import json
from pathlib import Path
import re
import shutil
import subprocess

from check_namespace import inode_fields
from check_orphans import accounting, digest
from generate_fixtures import resolve_tools


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--fixtures", type=Path, required=True)
    parser.add_argument("--tools-root", type=Path)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    tools = resolve_tools(args.tools_root)
    fixtures = json.loads(args.fixtures.read_text())
    if not fixtures or not all(item.get("passed") for item in fixtures):
        raise RuntimeError("Expected independently verified xattr fixtures")
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    filler = output / "filler.data"
    size = max(Path(item["image"]).stat().st_size for item in fixtures)
    with filler.open("wb") as stream:
        for offset in range(0, size, 1024 * 1024):
            stream.write(b"F" * min(size - offset, 1024 * 1024))
    records = []

    def save():
        (output / "report.json").write_text(json.dumps(records, indent=2) + "\n")

    for fixture in fixtures:
        source = Path(fixture["image"])
        original = digest(source)
        if original != fixture["input_sha256"]:
            raise RuntimeError("Xattr source changed before full-space fixture creation")
        image = output / source.name.replace("xattr-", "xattr-space-", 1)
        shutil.copyfile(source, image)
        record = {key: value for key, value in fixture.items() if key not in ("commands", "image", "input_sha256", "passed")}
        record.update(image=str(image), source=str(source), source_sha256=original, commands=[])
        records.append(record)
        save()

        def run(command):
            done = subprocess.run([str(part) for part in command], capture_output=True,
                                  text=True, errors="backslashreplace", timeout=180)
            record["commands"].append(dict(command=[str(part) for part in command],
                                           status=done.returncode, stdout=done.stdout, stderr=done.stderr))
            save()
            if done.returncode != 0:
                raise RuntimeError(f"Xattr full-space fixture command failed: {command}")
            return done.stdout

        # debugfs may return zero for ENOSPC. Nonrepairing e2fsck and group
        # counters establish that the partially allocated oversized file is valid.
        run([tools["debugfs"], "-w", "-R", f'write "{filler}" /filler', image])
        run([tools["e2fsck"], "-fn", image])
        counts = accounting(run([tools["dumpe2fs"], "-h", image]))
        groups = run([tools["dumpe2fs"], image])
        free = re.findall(r"^\s+(\d+) free blocks,", groups, re.M)
        if counts["Free blocks"] != 0 or not free or any(int(value) != 0 for value in free) or "BLOCK_UNINIT" in groups:
            raise RuntimeError("Xattr fixture did not consume every free block")
        for path, expected in fixture["objects"].items():
            actual = inode_fields(run([tools["debugfs"], "-R", f"stat {path}", image]))
            if json.loads(json.dumps(actual)) != expected:
                raise RuntimeError(f"Filling image changed existing xattr inode: {path}")
        objects = dict(fixture["objects"])
        objects["/filler"] = inode_fields(run([tools["debugfs"], "-R", "stat /filler", image]))
        if objects["/filler"] is None or objects["/filler"]["size"] != size or digest(source) != original:
            raise RuntimeError("Filler inode or protected source differs")
        record.update(passed=True, input_sha256=digest(image), objects=objects,
                      full_block_groups=len(free), accounting=counts, full_space=True)
        save()
        print(f"PASS {image.name}: existing xattrs preserved, {len(free)} full block groups, nonrepairing e2fsck clean", flush=True)


if __name__ == "__main__":
    main()

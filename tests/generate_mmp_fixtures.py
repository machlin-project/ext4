#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
"""Generate multi-mount-protection images with e2fsprogs."""
import argparse
import json
from pathlib import Path
import re
import subprocess

from check_orphans import digest
from generate_fixtures import EXPECTED_FEATURES, UUID, resolve_tools

IMAGE_BYTES = 32 * 1024 * 1024
MMP_SEQUENCE_CLEAN = 0xFF4D4D50
PROFILES = (
    dict(name="4k", block_size=4096, interval=5),
    dict(name="1k", block_size=1024, interval=5),
    dict(name="no-checksum", block_size=1024, interval=7, exclude={"metadata_csum"}),
    dict(name="slow", block_size=4096, interval=40),
    dict(name="quota", block_size=4096, interval=5, include={"quota"}),
)


def mmp_fields(text):
    fields = dict(re.findall(r"^(\w+):\s+(\S+)", text, re.M))
    return dict(block=int(fields["block_number"]), interval=int(fields["update_interval"]),
                sequence=int(fields["sequence"], 16), magic=int(fields["magic"], 16))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--tools-root", type=Path)
    args = parser.parse_args()
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    tools = resolve_tools(args.tools_root)
    reports = []
    for profile in PROFILES:
        block = profile["block_size"]
        image = output / f"mmp-{profile['name']}.img"
        tree = output / f"mmp-{profile['name']}-tree"
        tree.mkdir()
        (tree / "existing").write_bytes(b"multi-mount protected contents\n" * 40)
        (tree / "directory").mkdir()
        features = ((EXPECTED_FEATURES - profile.get("exclude", set())) | {"mmp"}
                    | profile.get("include", set()))
        row = dict(profile=profile["name"], image=str(image), block_size=block,
                   interval=profile["interval"], commands=[], passed=False)
        reports.append(row)

        def run(command, row=row):
            result = subprocess.run([str(x) for x in command], capture_output=True, text=True,
                                    errors="backslashreplace", timeout=120)
            row["commands"].append(dict(command=[str(x) for x in command],
                                        status=result.returncode, stdout=result.stdout,
                                        stderr=result.stderr))
            (output / "report.json").write_text(json.dumps(reports, indent=2) + "\n")
            result.check_returncode()
            return result.stdout

        run([tools["mke2fs"], "-F", "-t", "ext4", "-b", block, "-N", 256, "-I", 256, "-m", 0,
             "-O", "none," + ",".join(sorted(features)), "-U", UUID,
             "-E", f"mmp_update_interval={profile['interval']},lazy_itable_init=0,nodiscard",
             "-d", tree, image, IMAGE_BYTES // block])
        run([tools["e2fsck"], "-fn", image])
        mmp = mmp_fields(run([tools["debugfs"], "-R", "dump_mmp", image]))
        if mmp["sequence"] != MMP_SEQUENCE_CLEAN or mmp["interval"] != profile["interval"]:
            raise RuntimeError("Unexpected independently authored MMP state")
        row.update(passed=True, mmp=mmp, sha256=digest(image))
        (output / "report.json").write_text(json.dumps(reports, indent=2) + "\n")
        print(f"PASS mmp {profile['name']}: block {mmp['block']}, interval {mmp['interval']}",
              flush=True)


if __name__ == "__main__":
    main()

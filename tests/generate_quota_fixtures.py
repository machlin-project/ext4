#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
"""Author quota-tracking volumes with e2fsprogs for core accounting tests.

debugfs creates files owned by several users, groups and projects; e2fsck -fy
then computes the quota files from the inodes, and strict e2fsck must accept the
result before the core changes it."""
import argparse
import json
from pathlib import Path
import subprocess

from generate_fixtures import EXPECTED_FEATURES, UUID, resolve_tools

IMAGE_BYTES = 64 * 1024 * 1024
PROFILES = (
    dict(name="4k", block_size=4096, features={"quota", "project"},
         types="usrquota:grpquota:prjquota"),
    dict(name="1k-ea-inode", block_size=1024, features={"quota", "project", "ea_inode"},
         types="usrquota:grpquota:prjquota"),
    dict(name="bigalloc", block_size=4096, cluster_size=16384,
         features={"quota", "bigalloc"}, types="usrquota:grpquota"),
)
# Existing owners: name, uid, gid, project, payload blocks.
OWNERS = (
    ("alice", 1000, 100, 5, 3),
    ("bob", 1001, 100, 5, 1),
    ("carol", 70000, 70001, 9, 2),
)


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
        image = output / f"quota-{profile['name']}.img"
        row = dict(profile=profile["name"], image=str(image), block_size=block, commands=[],
                   passed=False)
        reports.append(row)

        def run(command, row=row):
            result = subprocess.run([str(x) for x in command], capture_output=True, text=True,
                                    errors="backslashreplace", timeout=300)
            row["commands"].append(dict(command=[str(x) for x in command],
                                        status=result.returncode, stdout=result.stdout[-4000:],
                                        stderr=result.stderr[-4000:]))
            (output / "report.json").write_text(json.dumps(reports, indent=2) + "\n")
            result.check_returncode()
            return result.stdout

        features = EXPECTED_FEATURES | profile["features"]
        command = [tools["mke2fs"], "-F", "-t", "ext4", "-b", block, "-N", 4096, "-I", 256,
                   "-m", 5, "-O", "none," + ",".join(sorted(features)), "-U", UUID, "-E",
                   f"quotatype={profile['types']},lazy_itable_init=0,nodiscard"]
        if "cluster_size" in profile:
            command += ["-C", profile["cluster_size"]]
        run(command + [image, IMAGE_BYTES // block])
        script = ["mkdir owners"]
        for name, uid, gid, project, blocks in OWNERS:
            payload = output / f"{profile['name']}-{name}.bin"
            payload.write_bytes(bytes([uid % 251]) * (blocks * block))
            script += [f'write "{payload}" owners/{name}', f"sif owners/{name} uid {uid}",
                       f"sif owners/{name} gid {gid}"]
            if "project" in profile["features"]:
                script.append(f"sif owners/{name} projid {project}")
        commands = output / f"quota-{profile['name']}.debugfs"
        commands.write_text("\n".join(script) + "\n")
        run([tools["debugfs"], "-w", "-f", commands, image])
        # e2fsck recomputes every quota file from the inode tables.
        subprocess.run([str(tools["e2fsck"]), "-fy", str(image)], capture_output=True,
                       timeout=300)
        run([tools["e2fsck"], "-fn", image])
        row["passed"] = True
        (output / "report.json").write_text(json.dumps(reports, indent=2) + "\n")
        print(f"PASS quota {profile['name']}: {len(OWNERS)} owners", flush=True)


if __name__ == "__main__":
    main()

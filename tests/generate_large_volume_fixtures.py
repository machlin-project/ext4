#!/usr/bin/env python3
"""Author sparse multi-terabyte volumes with real metadata in their last group."""

import argparse
import json
import os
from pathlib import Path
import re
import shlex
import subprocess

from generate_fixtures import EXPECTED_FEATURES, UUID, resolve_tools
from check_large_files import mapped_blocks
from check_namespace import inode_fields
from sparse_image import sparse_digest

PHYSICAL_BOUNDARY = 1 << 32
PROFILES = (
    dict(name="1k-high", block=1024, ratio=1024, blocks=PHYSICAL_BOUNDARY + (1 << 20)),
    dict(name="1k-meta-high", block=1024, ratio=1024, blocks=PHYSICAL_BOUNDARY + (1 << 20),
         include={"meta_bg", "sparse_super2", "metadata_csum_seed"}),
    # Stay below the host ext4 single-file ceiling while testing the last
    # physical block groups and their 64-bit byte offsets at 4 KiB blocks.
    dict(name="4k-boundary", block=4096, ratio=256, blocks=PHYSICAL_BOUNDARY - (1 << 20)),
)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--tools-root", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    tools = resolve_tools(args.tools_root)
    build = args.tools_root.resolve()
    helper = output / "large-volume-fixture"
    command = shlex.split(os.environ.get("CC", "cc")) + [
        "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror", "-Wdeclaration-after-statement",
        f"-I{build / 'lib'}", f"-I{build.parent / 'lib'}", str(Path(__file__).with_name("large_volume_fixture.c")),
        str(build / "lib/libext2fs.a"), str(build / "lib/libcom_err.a"), "-lpthread", "-o", str(helper)]
    done = subprocess.run(command, capture_output=True, text=True)
    (output / "helper-build.json").write_text(json.dumps(dict(
        command=command, status=done.returncode, stdout=done.stdout, stderr=done.stderr), indent=2) + "\n")
    done.check_returncode()
    reports = []
    for profile in PROFILES:
        image = output / f"volume-{profile['name']}.img"
        row = dict(profile=profile["name"], image=str(image), block_size=profile["block"],
                   cluster_blocks=profile["ratio"], blocks=profile["blocks"], commands=[])
        reports.append(row)

        def run(command):
            command = [str(x) for x in command]
            result = subprocess.run(command, capture_output=True, text=True, timeout=300)
            row["commands"].append(dict(command=command, status=result.returncode,
                                         stdout=result.stdout, stderr=result.stderr))
            (output / "report.json").write_text(json.dumps(reports, indent=2) + "\n")
            if result.returncode:
                raise RuntimeError(f"Large-volume fixture command failed: {command}: {result.stderr}")
            return result.stdout

        features = (EXPECTED_FEATURES - {"resize_inode", "flex_bg"}) | {"bigalloc"} | profile.get("include", set())
        run([tools["mke2fs"], "-F", "-t", "ext4", "-b", profile["block"],
             "-C", profile["block"] * profile["ratio"], "-N", 16384, "-I", 256, "-m", 0,
             "-O", "none," + ",".join(sorted(features)), "-U", UUID, "-J", "size=8",
             "-E", "lazy_itable_init=0,lazy_journal_init=0,nodiscard", image, profile["blocks"]])
        authored = run([helper, image])
        parsed = re.fullmatch(r"directory=(\d+) seed=(\d+) first_block=(\d+) block_size=(\d+) cluster_blocks=(\d+)\n", authored)
        if parsed is None or int(parsed[4]) != profile["block"] or int(parsed[5]) != profile["ratio"]:
            raise RuntimeError("Independent high-address fixture identity differs")
        row.update(directory_inode=int(parsed[1]), seed_inode=int(parsed[2]), first_high_block=int(parsed[3]))
        run([tools["e2fsck"], "-fn", image])
        row["header"] = run([tools["dumpe2fs"], "-h", image])
        row["seed_stat"] = run([tools["debugfs"], "-R", "stat /upper/seed", image])
        inode = inode_fields(row["seed_stat"])
        data, metadata = mapped_blocks(image, "/upper/seed", inode, row["seed_stat"], tools, run)
        acl = re.search(r"File ACL:\s+(\d+)", row["seed_stat"])
        addresses = [physical for physical, _ in data.values()] + list(metadata) + [int(acl[1])]
        if len(data) != 5 or not metadata or any(address < row["first_high_block"] for address in addresses):
            raise RuntimeError("Independent data, mapping nodes and attributes must use the last physical group")
        row.update(seed_inode_state=inode, data_blocks=sorted(addresses))
        row.update(passed=True, sparse_sha256=sparse_digest(image), stored_bytes=image.stat().st_blocks * 512)
        (output / "report.json").write_text(json.dumps(reports, indent=2) + "\n")
        print(f"PASS {profile['name']}: {image.stat().st_size} logical bytes, {row['stored_bytes']} stored bytes", flush=True)


if __name__ == "__main__":
    main()

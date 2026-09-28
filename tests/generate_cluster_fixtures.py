#!/usr/bin/env python3
"""Generate clustered allocation images with sparse extents and ordinary metadata."""

import argparse
import json
from pathlib import Path
import re
import subprocess

from check_namespace import inode_fields
from check_orphans import accounting, digest
from generate_fixtures import EXPECTED_FEATURES, UUID, resolve_tools

PROFILES = (
    dict(name="1k", block_size=1024, cluster_blocks=4),
    dict(name="4k", block_size=4096, cluster_blocks=4),
    dict(name="wide", block_size=1024, cluster_blocks=16),
    dict(name="no-checksum", block_size=1024, cluster_blocks=4, exclude={"metadata_csum"}),
    dict(name="checksum-seed", block_size=1024, cluster_blocks=8, include={"metadata_csum_seed"}),
    dict(name="inline", block_size=1024, cluster_blocks=4, include={"inline_data"}),
    dict(name="ea-inode", block_size=1024, cluster_blocks=4, include={"ea_inode"}),
    dict(name="orphan-file", block_size=1024, cluster_blocks=4, include={"orphan_file"}),
)


def payload(size):
    return bytes((index * 37 + 0x91) & 255 for index in range(size))


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
        ratio = profile["cluster_blocks"]
        cluster = block * ratio
        stem = "cluster-" + profile["name"]
        image = output / (stem + ".img")
        tree = output / (stem + "-tree")
        tree.mkdir()
        (tree / "file").write_bytes(payload(3 * cluster + 17))
        (tree / "small").write_bytes(payload(61))
        (tree / "empty-file").touch()
        (tree / "empty").mkdir()
        (tree / "entries").mkdir()
        for index in range(6):
            (tree / "entries" / f"e{index}").write_bytes(payload(index + 1))
        (tree / "short-link").symlink_to("entries/e0")
        (tree / "long-link").symlink_to("q" * 300)
        with (tree / "sparse").open("wb") as stream:
            for logical in (0, 1, ratio - 1, ratio + 1, 3 * ratio, 3 * ratio + 2,
                            5 * ratio + 1, 7 * ratio + 1, 9 * ratio):
                stream.seek(logical * block)
                stream.write(payload(block))
        features = ((EXPECTED_FEATURES - {"resize_inode"} - profile.get("exclude", set())) |
                    {"bigalloc"} | profile.get("include", set()))
        row = dict(image=str(image), block_size=block, cluster_blocks=ratio, commands=[])
        reports.append(row)

        def run(command):
            result = subprocess.run([str(x) for x in command], capture_output=True,
                                    text=True, errors="backslashreplace", timeout=120)
            row["commands"].append(dict(command=[str(x) for x in command], status=result.returncode,
                                         stdout=result.stdout, stderr=result.stderr))
            (output / "report.json").write_text(json.dumps(reports, indent=2) + "\n")
            result.check_returncode()
            return result.stdout

        extended = "lazy_itable_init=0,lazy_journal_init=0,nodiscard"
        if "orphan_file" in features:
            extended += ",orphan_file_size=4"
        run([tools["mke2fs"], "-F", "-t", "ext4", "-b", block, "-C", cluster,
             "-g", 1024, "-N", 256, "-I", 256, "-m", 0,
             "-O", "none," + ",".join(sorted(features)), "-J", f"size={max(2, block // 1024)}",
             "-U", UUID, "-E", extended, "-d", tree, image, 32 * 1024 * 1024 // block])
        value = output / (stem + ".value")
        value.write_bytes(payload(block // 2))
        run([tools["debugfs"], "-w", "-R", f'ea_set -f "{value}" /file user.value', image])
        run([tools["e2fsck"], "-fn", image])
        header = run([tools["dumpe2fs"], "-h", image])
        row["accounting"] = accounting(header)
        if int(re.search(r"^Cluster size:\s+(\d+)$", header, re.M)[1]) != cluster:
            raise RuntimeError("Independent cluster geometry differs")
        row["groups"] = run([tools["dumpe2fs"], image])
        row["objects"] = {name: inode_fields(run([tools["debugfs"], "-R", f"stat /{name}", image]))
                          for name in ("file", "small", "sparse", "empty", "short-link", "long-link")}
        row.update(passed=True, image_sha256=digest(image))
        (output / "report.json").write_text(json.dumps(reports, indent=2) + "\n")
        print(f"PASS {stem}: {ratio} blocks per cluster, strict fsck clean", flush=True)


if __name__ == "__main__":
    main()

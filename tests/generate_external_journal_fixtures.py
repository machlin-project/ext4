#!/usr/bin/env python3
"""Create independent filesystem/journal device pairs with e2fsprogs."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import shlex
import subprocess

from generate_fixtures import EXPECTED_FEATURES, UUID, resolve_tools

JOURNAL_UUID = "b72798b8-d618-48c1-8e1c-4c392075c201"
JOURNAL_MIN_RING_BLOCKS = 1024
SUPERBLOCK_OFFSET = 1024
PROFILES = (
    ("legacy-1k", 1024, 0),
    ("v1-4k", 4096, 1),
    ("v2-1k", 1024, 2),
    ("v3-4k", 4096, 3),
    ("v3-64k", 65536, 3),
)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--tools-root", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    build = args.tools_root.resolve()
    tools = resolve_tools(build)
    helper = output / "external-journal-fixture"
    rows = []

    def run(row, command):
        command = [str(part) for part in command]
        done = subprocess.run(command, capture_output=True, text=True, timeout=300)
        row["commands"].append(dict(command=command, status=done.returncode,
                                    stdout=done.stdout, stderr=done.stderr))
        (output / "report.json").write_text(json.dumps(rows, indent=2) + "\n")
        if done.returncode:
            raise RuntimeError(f"External-journal fixture command failed: {command}: {done.stderr}")
        return done.stdout

    build_row = dict(kind="helper-build", commands=[])
    rows.append(build_row)
    run(build_row, shlex.split(os.environ.get("CC", "cc")) + [
        "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror", "-Wdeclaration-after-statement",
        f"-I{build}", f"-I{build / 'lib'}", f"-I{build.parent / 'lib'}",
        Path(__file__).with_name("external_journal_fixture.c"),
        build / "lib/libext2fs.a", build / "lib/libcom_err.a", "-lpthread", "-o", helper,
    ])
    root = output / "root"
    root.mkdir()
    (root / "hello.txt").write_bytes(b"Machlin ext4\n")
    (root / "hello.txt").chmod(0o644)
    for name, block, format_number in PROFILES:
        image = output / f"{name}.img"
        journal = output / f"{name}.journal"
        row = dict(profile=name, image=str(image), journal=str(journal),
                   block_size=block, journal_format=format_number, commands=[])
        rows.append(row)
        features = EXPECTED_FEATURES - {"has_journal"}
        if format_number < 2:
            features -= {"metadata_csum"}
        run(row, [tools["mke2fs"], "-F", "-t", "ext4", "-b", block, "-N", 128,
                  "-I", 256, "-m", 0, "-O", "none," + ",".join(sorted(features)),
                  "-U", UUID, "-E", "lazy_itable_init=0,nodiscard", "-d", root,
                  image, (16 * 1024 * 1024) // block])
        journal_features = "none,journal_dev" + (",metadata_csum" if format_number >= 2 else "")
        run(row, [tools["mke2fs"], "-F", "-b", block, "-O", journal_features,
                  "-U", JOURNAL_UUID, "-E", "nodiscard", journal,
                  max(JOURNAL_MIN_RING_BLOCKS + SUPERBLOCK_OFFSET // block + 2,
                      (4 * 1024 * 1024) // block)])
        run(row, [helper, image, journal, format_number])
        run(row, [tools["e2fsck"], "-fn", "-j", journal, image])
        row["filesystem_header"] = run(row, [tools["dumpe2fs"], "-h", image])
        row["journal_header"] = run(row, [tools["dumpe2fs"], "-h", journal])
        row.update(passed=True, image_sha256=hashlib.sha256(image.read_bytes()).hexdigest(),
                   journal_sha256=hashlib.sha256(journal.read_bytes()).hexdigest())
        (output / "report.json").write_text(json.dumps(rows, indent=2) + "\n")
        print(f"PASS external journal {name}", flush=True)


if __name__ == "__main__":
    main()

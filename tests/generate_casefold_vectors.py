#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
"""Produce ext4 utf8-12.1 casefold and hash vectors with an e2fsprogs-linked oracle."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shlex
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--tools-root", type=Path, required=True,
                        help="e2fsprogs build directory containing lib/libext2fs.a")
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    build = args.tools_root.resolve()
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    oracle = output / "casefold-oracle"
    command = shlex.split(os.environ.get("CC", "cc")) + [
        "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror", f"-I{build / 'lib'}",
        f"-I{build.parent / 'lib'}", f"-I{build}",
        str(Path(__file__).with_name("casefold_oracle.c")), str(build / "lib/libext2fs.a"),
        str(build / "lib/libcom_err.a"), "-o", str(oracle)]
    subprocess.run(command, check=True)
    vectors = output / "vectors.txt"
    with vectors.open("w") as stream:
        subprocess.run([str(oracle)], check=True, stdout=stream)
    counts = {}
    with vectors.open() as stream:
        for line in stream:
            counts[line[0]] = counts.get(line[0], 0) + 1
    report = dict(command=command, oracle_sha256=hashlib.sha256(oracle.read_bytes()).hexdigest(),
                  vectors_sha256=hashlib.sha256(vectors.read_bytes()).hexdigest(),
                  folds=counts.get("F", 0), hashes=counts.get("H", 0), passed=True)
    (output / "report.json").write_text(json.dumps(report, indent=2) + "\n")
    print(f"PASS casefold vectors: {report['folds']} folds, {report['hashes']} hashes")


if __name__ == "__main__":
    main()

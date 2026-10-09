#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
"""Independent hashlib digests for enable tests; no filesystem or core helper calls."""
import argparse
import hashlib
from pathlib import Path
from generate_verity_fixtures import descriptor, merkle, ALGORITHMS

CASES = (
    ("empty", 0, 0, 0, 0, 1, 0, 0),
    ("one", 0, 1, 0, 0, 1, 0, 0),
    ("block", 1, 0, 0, 0, 2, 0, 32),
    ("holes", 5, 77, 1, 3, 1, 0, 16),
    ("preallocated", 2, 5, 0, 0, 1, 0, 0),
    ("levels", 0, 1536 * 1024 + 77, 0, 0, 1, 1024, 0),
    ("sha512-levels", 0, 700 * 1024, 0, 0, 2, 1024, 32),
    ("inline", 0, 100, 0, 0, 1, 0, 0),
)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    for block in (1024, 4096, 16384, 65536):
        for index, (name, blocks, extra, hole, end, algorithm, size, salt_size) in enumerate(CASES):
            length = blocks * block + extra
            data = bytes(0 if hole <= byte // block < end else
                         (0x3b + index * 13 + byte % 251 + (byte >> 12)) & 255
                         for byte in range(length))
            salt = bytes(range(0x40, 0x40 + salt_size))
            size = size or block
            _, root = merkle(data, size, algorithm, salt)
            digest = hashlib.new(ALGORITHMS[algorithm][0],
                                 descriptor(length, size, algorithm, salt, root)).hexdigest()
            (args.output / f"enable-{block}-{name}.digest").write_text(digest + "\n")
    print("PASS 32 independent enable digest vectors")


if __name__ == "__main__":
    main()

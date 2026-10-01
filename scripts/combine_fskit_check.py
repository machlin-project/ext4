#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
"""Combine architecture-specific separate-checker libraries for Xcode."""

import argparse
from pathlib import Path
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('architectures', choices=('arm64', 'x86_64'), nargs='+')
    args = parser.parse_args()
    libraries = [args.output.parent / 'Maintenance' / arch / 'libe2fsck-resource.a'
                 for arch in args.architectures]
    if any(not library.is_file() for library in libraries):
        parser.error('A requested architecture library is missing')
    subprocess.run(['xcrun', 'lipo', '-create', *map(str, libraries), '-output', str(args.output)],
                   check=True)


if __name__ == '__main__':
    main()

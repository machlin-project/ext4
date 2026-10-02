#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
"""Package complete corresponding source for the separate GPL filesystem checker."""

import argparse
import gzip
import io
from pathlib import Path
import subprocess
import tarfile

from build_fskit_check import ROOT, UPSTREAM_COMMIT

PREFIX = 'Machlin-ext4-check-source/'
OWNED = ('tools/maintenance/main.c', 'tools/maintenance/io.c',
         'adapters/fskit/Ext4CheckProtocol.h', 'adapters/fskit/Ext4CheckWire.c',
         'scripts/build_fskit_check.py', 'tools/maintenance/format.c', 'LICENSE')
README = b'''Machlin ext4 separate filesystem maintenance tool: corresponding source

The portable ext4 engine and FSKit extension do not link this checker.
The separate executable combines e2fsck and mke2fs with the resource I/O bridge.
Upstream licenses are in vendor/e2fsprogs-maintenance/NOTICE and source files.
The Machlin resource bridge uses the BSD license in LICENSE.

Requirements: macOS, Xcode command-line tools, Python 3, make and Git.
Build arm64 (use x86_64 for the other architecture):

  python3 scripts/build_fskit_check.py --source-release \\
    --source vendor/e2fsprogs-maintenance --arch arm64 --output build

The result is build/arm64/ext4-check-resource. Its verify/repair/preen modes run
e2fsck; its format mode accepts a block size, volume label and UUID for mke2fs.
It requires an inherited stream
socket on stdin implementing Ext4CheckProtocol.h; it does not open a device path.
The SDK and minimum target are selected by the included build script.
Normal product builds use the pinned Git release; --source-release explicitly
accepts the supplied source, including user modifications, without a Git database.
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path, default=ROOT / 'vendor/e2fsprogs-maintenance')
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    if args.output.exists():
        parser.error('The output already exists')
    revision = subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=args.source, text=True).strip()
    if revision != UPSTREAM_COMMIT or subprocess.check_output(
            ['git', 'status', '--porcelain', '--untracked-files=no'], cwd=args.source):
        parser.error('Package the unchanged pinned upstream source')
    upstream = subprocess.check_output(['git', 'archive', '--format=tar',
                                        '--prefix=' + PREFIX + 'vendor/e2fsprogs-maintenance/',
                                        'HEAD'], cwd=args.source)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    with args.output.open('xb') as destination, \
            gzip.GzipFile(fileobj=destination, mode='wb', filename='', mtime=0) as compressed, \
            tarfile.open(fileobj=compressed, mode='w') as package:
        with tarfile.open(fileobj=io.BytesIO(upstream), mode='r') as source:
            for member in source:
                member.mtime = 0
                package.addfile(member, source.extractfile(member) if member.isfile() else None)
        for name in OWNED:
            content = (ROOT / name).read_bytes()
            member = tarfile.TarInfo(PREFIX + name)
            member.size = len(content)
            member.mode = 0o644
            package.addfile(member, io.BytesIO(content))
        member = tarfile.TarInfo(PREFIX + 'README')
        member.size = len(README)
        member.mode = 0o644
        package.addfile(member, io.BytesIO(README))
    print(f'Complete separate-checker source: {args.output}')


if __name__ == '__main__':
    main()

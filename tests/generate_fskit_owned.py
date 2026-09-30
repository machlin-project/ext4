#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
"""Prepare bounded, independently owned FSKit write/authorization fixtures."""
import argparse
import hashlib
import json
import shutil
import subprocess
from pathlib import Path
from generate_fixtures import FEATURE_LIST


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    source_group = parser.add_mutually_exclusive_group(required=True)
    source_group.add_argument('--fixtures', type=Path)
    source_group.add_argument('--empty-mib', type=int, choices=(16, 32, 64),
                              help='Create bounded empty volumes for native capacity checks')
    parser.add_argument('--mke2fs', type=Path)
    parser.add_argument('--output', type=Path, required=True, help='New directory')
    parser.add_argument('--debugfs', type=Path, required=True)
    parser.add_argument('--e2fsck', type=Path, required=True)
    parser.add_argument('--uid', type=int, default=501)
    parser.add_argument('--gid', type=int, default=20)
    args = parser.parse_args()
    if not 0 < args.uid < 2**32 - 1 or not 0 <= args.gid < 2**32:
        parser.error('Use an ordinary user UID and a valid GID')
    if args.empty_mib and args.mke2fs is None:
        parser.error('--empty-mib requires --mke2fs')
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=False)
    commands = [f'set_inode_field / uid {args.uid}', f'set_inode_field / gid {args.gid}',
                'set_inode_field / mode 040755', 'mkdir /sticky',
                f'set_inode_field /sticky uid {args.uid + 1}',
                f'set_inode_field /sticky gid {args.gid}', 'set_inode_field /sticky mode 041777']
    for name in ('/foreign-owned', '/sticky/foreign-owned'):
        commands += [f'write /dev/null {name}', f'set_inode_field {name} uid {args.uid + 1}',
                     f'set_inode_field {name} gid {args.gid}', f'set_inode_field {name} mode 0100600']
    script = out / 'debugfs.commands'
    script.write_text('\n'.join(commands) + '\n')
    results = {}
    for profile in ('4k', '1k'):
        image = out / f'ext4-owned-{profile}.img'
        before = None
        if args.empty_mib:
            with image.open('xb') as stream:
                stream.truncate(args.empty_mib * 1024 * 1024)
            with (out / f'{profile}-mke2fs.log').open('wb') as log:
                subprocess.run([str(args.mke2fs.resolve()), '-t', 'ext4', '-F', '-q',
                                '-b', '4096' if profile == '4k' else '1024', '-I', '256',
                                '-O', 'none,' + FEATURE_LIST, '-E',
                                'lazy_itable_init=0,lazy_journal_init=0', str(image)],
                               stdout=log, stderr=subprocess.STDOUT, check=True, timeout=60)
        else:
            source = args.fixtures / f'ext4-metadata-{profile}.img'
            before = hashlib.sha256(source.read_bytes()).hexdigest()
            shutil.copyfile(source, image)
        image.chmod(0o600)
        with (out / f'{profile}-debugfs.log').open('wb') as log:
            subprocess.run([str(args.debugfs.resolve()), '-w', '-f', str(script), str(image)],
                           stdout=log, stderr=subprocess.STDOUT, check=True, timeout=60)
        with (out / f'{profile}-fsck.log').open('wb') as log:
            subprocess.run([str(args.e2fsck.resolve()), '-fn', str(image)],
                           stdout=log, stderr=subprocess.STDOUT, check=True, timeout=60)
        if before is not None:
            assert hashlib.sha256(source.read_bytes()).hexdigest() == before
        results[profile] = {'original_sha256': before,
                            'sha256': hashlib.sha256(image.read_bytes()).hexdigest(),
                            'uid': args.uid, 'foreign_uid': args.uid + 1, 'gid': args.gid}
    (out / 'manifest.json').write_text(json.dumps(results, indent=2) + '\n')


if __name__ == '__main__':
    main()

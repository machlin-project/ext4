#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
"""Build bounded checksum-damaged media with usable backups and a file canary."""

import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil
import subprocess
import uuid

from test_fskit_installed_format import FORMAT_FEATURES

ROOT = Path(__file__).resolve().parents[1]
CANARY_NAME = 'maintenance-canary.txt'
CANARY = b'Machlin ext4 primary-superblock repair must preserve this file.\n'
PROFILES = {'1k': (1024, 64 * 1024 * 1024), '4k': (4096, 256 * 1024 * 1024)}


def digest(path):
    result = hashlib.sha256()
    with path.open('rb') as stream:
        while data := stream.read(1024 * 1024):
            result.update(data)
    return result.hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--tools-root', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    tools = args.tools_root.resolve()
    fixtures = output / 'fixtures'
    fixtures.mkdir()
    canary = fixtures / CANARY_NAME
    canary.write_bytes(CANARY)

    def run(label, command, accepted=(0,), seconds=90):
        with (output / (label + '.stdout.log')).open('wb') as stdout, \
                (output / (label + '.stderr.log')).open('wb') as stderr:
            result = subprocess.run(list(map(str, command)), stdout=stdout, stderr=stderr,
                                    timeout=seconds)
        (output / (label + '.status.json')).write_text(json.dumps(
            {'argv': list(map(str, command)), 'exit_code': result.returncode}, indent=2) + '\n')
        assert result.returncode in accepted, f'{label}: exit {result.returncode}'
        return (output / (label + '.stdout.log')).read_bytes()

    clang = subprocess.check_output(['xcrun', '--find', 'clang'], text=True).strip()
    sdk = subprocess.check_output(['xcrun', '--sdk', 'macosx', '--show-sdk-path'],
                                  text=True).strip()
    mutator = output / 'damage-primary'
    run('compile-mutator', [clang, '-isysroot', sdk, '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror',
                           '-Wdeclaration-after-statement', '-I', ROOT / 'include',
                           '-I', ROOT / 'core', ROOT / 'tests/fskit_primary_damage.c',
                           '-o', mutator])
    profiles = {}
    for profile, (block_size, image_bytes) in PROFILES.items():
        clean = fixtures / f'ext4-check-clean-{profile}.img'
        damaged = fixtures / f'ext4-check-damaged-{profile}.img'
        oracle = output / f'oracle-repair-{profile}.img'
        with clean.open('xb') as stream:
            stream.truncate(image_bytes)
        creation = run(profile + '-create', [tools / 'misc/mke2fs', '-F', '-t', 'ext4',
            '-b', block_size, '-m', '0', '-O', 'none,' + ','.join(sorted(FORMAT_FEATURES)),
            '-E', 'lazy_itable_init=0,lazy_journal_init=0', '-U', uuid.uuid4(), clean])
        assert b'Superblock backups stored on blocks:' in creation, 'Fixture needs a backup'
        quoted_canary = str(canary).replace('\\', '\\\\').replace('"', '\\"')
        run(profile + '-write-canary', [tools / 'debugfs/debugfs', '-w', '-R',
                                      f'write "{quoted_canary}" /{CANARY_NAME}', clean])
        run(profile + '-clean-fsck', [tools / 'e2fsck/e2fsck', '-fn', clean])
        before = digest(clean)
        stats = run(profile + '-clean-stats', [tools / 'debugfs/debugfs', '-R', 'stats', clean])
        feature_line = re.search(rb'^Filesystem features:\s*(.*)$', stats, re.MULTILINE)
        assert feature_line and set(feature_line[1].decode().split()) == FORMAT_FEATURES
        assert run(profile + '-clean-canary', [tools / 'debugfs/debugfs', '-R',
                                              f'cat /{CANARY_NAME}', clean]) == CANARY
        shutil.copyfile(clean, damaged)
        run(profile + '-damage-primary', [mutator, damaged])
        damaged_hash = digest(damaged)
        assert damaged_hash != before
        run(profile + '-damaged-fsck', [tools / 'e2fsck/e2fsck', '-fn', damaged], accepted=(4, 8))
        assert digest(damaged) == damaged_hash, 'Read-only checker changed the fixture'
        shutil.copyfile(damaged, oracle)
        run(profile + '-oracle-repair', [tools / 'e2fsck/e2fsck', '-fy', oracle], accepted=(0, 1))
        run(profile + '-oracle-final-fsck', [tools / 'e2fsck/e2fsck', '-fn', oracle])
        assert run(profile + '-oracle-canary', [tools / 'debugfs/debugfs', '-R',
                                               f'cat /{CANARY_NAME}', oracle]) == CANARY
        assert digest(clean) == before and digest(damaged) == damaged_hash
        profiles[profile] = {'bytes': image_bytes, 'block_size': block_size,
                             'clean_sha256': before, 'damaged_sha256': damaged_hash,
                             'oracle_repaired_sha256': digest(oracle),
                             'checksum_only_damage': True, 'backups_present': True,
                             'independent_repair_and_canary': True, 'passed': True}
    (output / 'summary.json').write_text(json.dumps(
        {'profiles': profiles, 'canary_name': CANARY_NAME,
         'canary_sha256': digest(canary), 'passed': True}, indent=2) + '\n')
    print('Primary-checksum fixtures and independent repairs passed for 1k and 4k')


if __name__ == '__main__':
    main()

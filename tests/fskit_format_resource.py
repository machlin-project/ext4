#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
"""Verify resource-only formatting and failures with an independent e2fsck."""

import argparse
import json
from pathlib import Path
import re
import shutil
import subprocess

from fskit_check_resource import digest, run_case

IMAGE_BYTES = 64 * 1024 * 1024
TEST_UUID = '01234567-89ab-4cde-8f01-23456789abcd'
FORMAT_FEATURES = {'has_journal', 'ext_attr', 'resize_inode', 'dir_index', 'filetype',
                   'extent', '64bit', 'flex_bg', 'sparse_super', 'large_file', 'huge_file',
                   'dir_nlink', 'extra_isize', 'metadata_csum'}


def blank(path):
    with path.open('xb') as image:
        image.truncate(IMAGE_BYTES)


def independent(command, output):
    process = subprocess.run(list(map(str, command)), stdout=subprocess.PIPE,
                             stderr=subprocess.STDOUT, timeout=60)
    output.write_bytes(process.stdout)
    process.check_returncode()
    return process.stdout.decode('utf-8', errors='strict')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--helper', type=Path, required=True)
    parser.add_argument('--tools-root', type=Path)
    parser.add_argument('--debugfs', type=Path)
    parser.add_argument('--e2fsck', type=Path)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    helper, output = args.helper.resolve(), args.output.resolve()
    debugfs = args.debugfs or (args.tools_root / 'debugfs/debugfs' if args.tools_root else
                              Path(shutil.which('debugfs') or ''))
    e2fsck = args.e2fsck or (args.tools_root / 'e2fsck/e2fsck' if args.tools_root else
                            Path(shutil.which('e2fsck') or ''))
    if not debugfs.is_file() or not e2fsck.is_file():
        parser.error('Supply independent debugfs and e2fsck executable paths')
    output.mkdir(parents=True, exist_ok=False)
    profiles = []
    for block_size, label in ((1024, 'ext4-12345678901'), (2048, ''),
                              (4096, 'ext4-🧪-abcdef')):
        case = output / str(block_size)
        case.mkdir()
        image = case / 'image.img'
        blank(image)
        result = run_case(helper, image, 'format', case,
                          arguments=(str(block_size), label, TEST_UUID))
        assert result['exit_code'] == 0, result
        assert result['counts']['write'] > 0 and result['counts']['flush'] > 0, result
        independent([e2fsck, '-fn', image], case / 'independent-fsck.log')
        stats = independent([debugfs, '-R', 'stats', image],
                            case / 'independent-stats.log')
        fields = dict(re.findall(r'^([^:\n]+):\s*([^\n]*)$', stats, re.MULTILINE))
        assert fields['Filesystem UUID'].strip() == TEST_UUID, fields
        assert fields['Filesystem volume name'].strip() == (label or '<none>'), fields
        assert int(fields['Block size']) == block_size, fields
        assert set(fields['Filesystem features'].split()) == FORMAT_FEATURES, fields
        result['independent_fsck_passed'] = True
        result['geometry_label_uuid_features_passed'] = True
        profiles.append(result)
    failures = []
    for name, arguments, fault, writable in (
            ('unsupported-block', ('8192', '', TEST_UUID), None, True),
            ('long-label', ('4096', 'abcdefghijklmnopq', TEST_UUID), None, True),
            ('invalid-uuid', ('4096', '', 'x' * 36), None, True),
            ('read-only', ('4096', '', TEST_UUID), None, False),
            ('write-error', ('4096', '', TEST_UUID), 'write-error', True),
            ('flush-error', ('4096', '', TEST_UUID), 'flush-error', True),
            ('bad-magic', ('4096', '', TEST_UUID), 'bad-magic', True),
            ('disconnect', ('4096', '', TEST_UUID), 'disconnect', True)):
        case = output / name
        case.mkdir()
        image = case / 'image.img'
        blank(image)
        result = run_case(helper, image, 'format', case, fault,
                          arguments=arguments, writable=writable)
        assert result['exit_code'] != 0, result
        if fault is None or fault == 'write-error':
            assert result['before_sha256'] == result['after_sha256'], result
            assert result['counts']['write'] == 0, result
        failures.append({'case': name, **result})
    (output / 'summary.json').write_text(json.dumps(
        {'helper_sha256': digest(helper), 'profiles': profiles,
         'failures': failures, 'passed': True}, indent=2) + '\n')
    print('Resource formats, independent fsck and failure checks passed')


if __name__ == '__main__':
    main()

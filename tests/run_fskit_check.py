#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
"""Exercise full FSKit check ownership without native mounting or device access."""

import argparse
import hashlib
import json
from pathlib import Path
import plistlib
import shutil
import subprocess
import time

ROOT = Path(__file__).resolve().parents[1]
ROOT_INODE = 2
DAMAGED_ROOT_LINKS = 1000
SOURCES = ('Ext4ResourceIO.m', 'Ext4Support.m', 'Ext4Control.m', 'Ext4Volume.m',
           'Ext4FileSystem.m', 'Ext4FileSystemMaintenance.m',
           'Ext4KeyStore.m', 'Ext4DeviceBarrier.m',
           'Ext4VolumeIO.m', 'Ext4VolumeReadState.m', 'Ext4VolumeCheck.m',
           'Ext4ResourceTask.m', 'Ext4CheckTask.m', 'Ext4FormatTask.m',
           'Ext4CheckWire.c', 'Ext4VolumeCompatibility.m',
           'Ext4VolumeModern.m', 'Ext4VolumeMutation.m', 'Ext4VolumeControl.m', 'Ext4Crypto.m')


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def run(command, output, seconds=60):
    started = time.monotonic()
    result = {'command': list(map(str, command))}
    try:
        with output.with_suffix('.stdout.log').open('wb') as stdout, \
                output.with_suffix('.stderr.log').open('wb') as stderr:
            process = subprocess.run(command, stdout=stdout, stderr=stderr, timeout=seconds)
        result['exit_code'] = process.returncode
        process.check_returncode()
    finally:
        result['seconds'] = time.monotonic() - started
        output.with_suffix('.status.json').write_text(json.dumps(result, indent=2) + '\n')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--derived-data', type=Path, required=True)
    parser.add_argument('--fixtures', type=Path, required=True)
    parser.add_argument('--helper', type=Path, help='Explicit resource helper; defaults to Debug products')
    parser.add_argument('--tools-root', type=Path)
    parser.add_argument('--debugfs', type=Path)
    parser.add_argument('--e2fsck', type=Path)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    if args.output.exists():
        parser.error('Use a new output directory to preserve earlier evidence')
    args.output.mkdir(parents=True)
    debugfs = args.debugfs or (args.tools_root / 'debugfs/debugfs' if args.tools_root else
                              Path(shutil.which('debugfs') or ''))
    e2fsck = args.e2fsck or (args.tools_root / 'e2fsck/e2fsck' if args.tools_root else
                            Path(shutil.which('e2fsck') or ''))
    if not debugfs.is_file() or not e2fsck.is_file():
        parser.error('Supply independent debugfs and e2fsck executable paths')
    products = args.derived_data / 'Build/Products/Debug'
    core = products / 'libExt4Core.a'
    helper = args.helper.resolve() if args.helper else products / 'Ext4CheckResource'
    if not core.is_file() or not helper.is_file():
        parser.error('The unsigned Debug FSKit products are missing')
    clang = subprocess.check_output(['xcrun', '--find', 'clang'], text=True).strip()
    sdk = subprocess.check_output(['xcrun', '--sdk', 'macosx', '--show-sdk-path'], text=True).strip()
    flags = [clang, '-isysroot', sdk, '-mmacosx-version-min=26.5', '-Wall', '-Wextra',
             '-Werror', '-Wdeclaration-after-statement', '-g', '-O1']
    peer = args.output / 'fskit-check-peer'
    bundle = args.output / 'FSKitMaintenanceTests.app'
    contents = bundle / 'Contents'
    (contents / 'MacOS').mkdir(parents=True)
    (contents / 'Helpers').mkdir()
    binary = contents / 'MacOS/fskit-check'
    with (contents / 'Info.plist').open('wb') as stream:
        plistlib.dump({'CFBundleExecutable': binary.name, 'CFBundlePackageType': 'APPL',
                       'CFBundleIdentifier': 'org.machlin.ext4.maintenance-tests'}, stream)
    bundled_helper = contents / 'Helpers/Ext4CheckResource'
    shutil.copyfile(helper, bundled_helper)
    bundled_helper.chmod(0o755)
    assert digest(bundled_helper) == digest(helper)
    run([*flags, str(ROOT / 'tests/fskit_check_peer.c'),
         str(ROOT / 'adapters/fskit/Ext4CheckWire.c'), '-o', str(peer)], args.output / 'peer-compile')
    run([*flags, '-fobjc-arc', '-fblocks', '-fsanitize=address,undefined',
         '-DSecItemCopyMatching=Ext4TestKeychainCopy',
         '-DSecItemAdd=Ext4TestKeychainAdd', '-DSecItemDelete=Ext4TestKeychainDelete',
         '-I', str(ROOT / 'include'), '-I', str(ROOT / 'core'),
         '-framework', 'Foundation', '-framework', 'FSKit',
         '-framework', 'Security', '-framework', 'LocalAuthentication',
         str(ROOT / 'tests/fskit_check.m'),
         *(str(ROOT / 'adapters/fskit' / source) for source in SOURCES),
         str(core), '-o', str(binary)], args.output / 'component-compile')
    profiles = []
    for name in ('ext4-1k', 'ext4-4k'):
        output = args.output / name
        output.mkdir()
        fixture = args.fixtures / (name + '.img')
        before = digest(fixture)
        damaged = output / 'damaged.img'
        repaired = output / 'repaired.img'
        formatted = output / 'formatted.img'
        supervised = output / 'supervised.img'
        shutil.copyfile(fixture, damaged)
        for index, command in enumerate(('set_super_value free_inodes_count 0',
                                         f'set_inode_field <{ROOT_INODE}> links_count {DAMAGED_ROOT_LINKS}')):
            run([str(debugfs), '-w', '-R', command, str(damaged)],
                output / f'damage-{index}')
        run([str(binary), str(helper), str(peer), str(fixture), str(damaged), str(repaired),
             str(formatted), str(supervised)],
            output / 'component')
        run([str(e2fsck), '-fn', str(repaired)],
            output / 'independent-fsck')
        run([str(e2fsck), '-fn', str(formatted)], output / 'independent-format-fsck')
        run([str(e2fsck), '-fn', str(supervised)], output / 'independent-supervisor-fsck')
        assert before == digest(fixture), 'Original clean fixture changed'
        profiles.append({'profile': name, 'fixture_sha256': before, 'fixture_unchanged': True,
                         'repaired_sha256': digest(repaired),
                         'formatted_sha256': digest(formatted),
                         'supervised_sha256': digest(supervised),
                         'filesystem_entry_points': {
                             'option_refusals_without_io': True,
                             'asynchronous_refusal_with_one_completion': True,
                             'exclusive_admission_and_unload': True,
                             'resource_only_check_repair_format': True,
                             'temporary_unary_identity_without_geometry': True,
                             'maintenance_activation_refused_and_accounting_empty': True,
                             'temporary_identity_released_after_success': True,
                             'final_barrier_and_validation_read_faults': True,
                             'progress_and_task_cancellation': True}, 'passed': True})
    (args.output / 'summary.json').write_text(json.dumps(
        {'helper_sha256': digest(helper), 'profiles': profiles, 'passed': True}, indent=2) + '\n')
    print('Full check component profiles passed for 1K and 4K')


if __name__ == '__main__':
    main()

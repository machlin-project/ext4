#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
"""Format only disposable VM images, then mount, write and independently check them."""

import argparse
import getpass
import hashlib
import json
from pathlib import Path
import plistlib
import re
import subprocess
import sys
import uuid

from fskit_test_vm import GuestTimeout, guest_commands, image_devices

IMAGE_BYTES = 64 * 1024 * 1024
BLOCK_SIZES = {'1k': 1024, '2k': 2048, '4k': 4096}
FORMAT_FEATURES = {'has_journal', 'ext_attr', 'resize_inode', 'dir_index', 'filetype',
                   'extent', '64bit', 'flex_bg', 'sparse_super', 'large_file', 'huge_file',
                   'dir_nlink', 'extra_isize', 'metadata_csum'}


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--tart', type=Path, required=True)
    parser.add_argument('--vm', required=True)
    parser.add_argument('--blank-image', type=Path, required=True)
    parser.add_argument('--guest-blank-image', required=True, help='Exact shared copy of the blank image')
    parser.add_argument('--guest-workdir', required=True)
    parser.add_argument('--build-number', type=int, required=True)
    parser.add_argument('--profiles', nargs='+', choices=tuple(BLOCK_SIZES), default=['1k', '2k', '4k'])
    parser.add_argument('--e2fsck', type=Path, required=True)
    parser.add_argument('--debugfs', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    if not sys.stdin.isatty():
        parser.error('Use a terminal for the no-echo dedicated VM password prompt')
    if args.build_number < 1 or not args.guest_workdir.startswith('/') or args.guest_workdir == '/':
        parser.error('Use a positive build number and a new absolute guest directory')
    if len(set(args.profiles)) != len(args.profiles):
        parser.error('Profiles must not repeat')
    blank = args.blank_image.resolve()
    if blank.stat().st_size != IMAGE_BYTES or any(blank.read_bytes()):
        parser.error('Use a dedicated all-zero 64MiB image')
    blank_hash = digest(blank)
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    guest = guest_commands(args.tart.resolve(), args.vm, output)
    root = args.guest_workdir.rstrip('/')
    app = '/Applications/Machlin ext4.app/Contents/MacOS/Machlin ext4'
    password = (getpass.getpass('Dedicated VM administrator password: ') + '\n').encode()
    results = {}

    def control(label, *command):
        return json.loads(guest(label, app, '--control', *command))

    def native_format(label, device, *options):
        assert re.fullmatch(r'/dev/disk[0-9]+', device), 'Use only the proven task-owned device'
        raw_device = '/dev/r' + device.removeprefix('/dev/')
        guest(label, '/usr/bin/sudo', '-S', '-p', '', '--', '/sbin/newfs_fskit',
              '-t', 'machlinext4', *options, raw_device, required=False, timeout=120, stdin=password)
        matches = list(output.glob('*-' + label + '.status.json'))
        assert len(matches) == 1
        return json.loads(matches[0].read_text())['exit_code']

    def attach(label, image, readonly):
        data = plistlib.loads(guest(label, '/usr/bin/hdiutil', 'attach', '-nomount',
            '-readonly' if readonly else '-readwrite', '-plist', '-imagekey',
            'diskimage-class=CRawDiskImage', image))
        entries = [entry for entry in data['system-entities'] if 'dev-entry' in entry]
        assert len(entries) == 1 and re.fullmatch(r'/dev/disk[0-9]+', entries[0]['dev-entry'])
        assert not entries[0].get('mount-point')
        device = entries[0]['dev-entry']
        assert image_devices(guest, label + '-ownership', image) == [device]
        return device

    def detach(label, image):
        for device in image_devices(guest, label + '-devices', image):
            guest(label, '/usr/bin/hdiutil', 'detach', device)
        assert image_devices(guest, label + '-final-devices', image) == []
        assert control(label + '-endpoints', 'list') == []

    def mount(label, image, directory, readonly):
        data = plistlib.loads(guest(label, '/usr/bin/hdiutil', 'attach',
            '-readonly' if readonly else '-readwrite', '-owners', 'off', '-nobrowse',
            '-mountpoint', directory, '-plist', '-imagekey', 'diskimage-class=CRawDiskImage', image))
        entries = [entry for entry in data['system-entities'] if entry.get('mount-point')]
        assert len(entries) == 1 and entries[0]['mount-point'] == directory
        assert entries[0].get('volume-kind') == 'machlinext4'
        device = entries[0]['dev-entry']
        assert image_devices(guest, label + '-ownership', image) == [device]
        return device

    def oracle(label, image):
        with (output / (label + '-fsck.log')).open('wb') as log:
            process = subprocess.run([str(args.e2fsck.resolve()), '-fn', str(image)],
                                     stdout=log, stderr=subprocess.STDOUT, timeout=90)
        assert process.returncode == 0, f'{label}: independent fsck rejected image'
        process = subprocess.run([str(args.debugfs.resolve()), '-R', 'stats', str(image)],
                                 stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=30)
        (output / (label + '-stats.log')).write_bytes(process.stdout)
        process.check_returncode()
        return dict(re.findall(r'^([^:\n]+):\s*([^\n]*)$',
                               process.stdout.decode('utf-8'), re.MULTILINE))

    assert int(guest('uid', '/usr/bin/id', '-u')) != 0
    guest('os', '/usr/bin/sw_vers')
    guest('kernel', '/usr/bin/uname', '-v')
    guest('sip', '/usr/bin/csrutil', 'status')
    assert guest('build', '/usr/libexec/PlistBuddy', '-c', 'Print :CFBundleVersion',
                 '/Applications/Machlin ext4.app/Contents/Info.plist').decode().strip() == str(args.build_number)
    modules = control('modules', 'modules')
    assert len(modules) == 1 and modules[0]['enabled'] is True
    assert control('service', 'device-service')['status'] == 'enabled'
    assert control('initial-endpoints', 'list') == []
    assert guest('source-hash', '/usr/bin/shasum', '-a', '256',
                 args.guest_blank_image).decode().split()[0] == blank_hash
    guest('prepare', '/bin/sh', '-eu', '-c', 'umask 077; test ! -e "$1"; mkdir -p "$1"',
          'prepare', root)

    for profile in args.profiles:
        result = {'passed': False, 'block_size': BLOCK_SIZES[profile],
                  'administrative_format': True, 'progress_display': False}
        results[profile] = result
        image = root + '/format-' + profile + '.img'
        directory = root + '/mount-' + profile
        label = 'ext4-12345678901'
        contents = 'Machlin ext4 native format ' + profile + '\n'
        attached = False
        try:
            guest(profile + '-copy-blank', '/bin/cp', args.guest_blank_image, image)
            device = attach(profile + '-attach-readonly', image, True)
            attached = True
            assert native_format(profile + '-readonly-refusal', device) != 0
            detach(profile + '-detach-readonly', image)
            attached = False
            assert guest(profile + '-readonly-unchanged', '/usr/bin/shasum', '-a', '256',
                         image).decode().split()[0] == blank_hash
            result['readonly_unchanged'] = True
            device = attach(profile + '-attach-format', image, False)
            attached = True
            for name, options in (('invalid-block', ('-b', '8192')),
                                  ('long-label', ('-L', 'abcdefghijklmnopq'))):
                assert native_format(profile + '-' + name, device, *options) != 0
                assert guest(profile + '-' + name + '-unchanged', '/usr/bin/shasum', '-a', '256',
                             image).decode().split()[0] == blank_hash
            assert native_format(profile + '-format', device, '-b', str(BLOCK_SIZES[profile]),
                                 '-L', label) == 0, 'Native format returned an error'
            detach(profile + '-detach-format', image)
            attached = False
            export = output / ('formatted-' + profile + '.img')
            guest(profile + '-export-format', '/bin/cat', image, destination=export, timeout=90)
            stats = oracle(profile + '-formatted', export)
            assert int(stats['Block size']) == BLOCK_SIZES[profile]
            assert stats['Filesystem volume name'].strip() == label
            assert set(stats['Filesystem features'].split()) == FORMAT_FEATURES
            volume_uuid = uuid.UUID(stats['Filesystem UUID'].strip())
            assert volume_uuid.int != 0
            result['volume_uuid'] = str(volume_uuid)
            result['formatted_sha256'] = digest(export)
            guest(profile + '-mount-directory', '/bin/mkdir', directory)
            device = mount(profile + '-mount-writable', image, directory, False)
            attached = True
            guest(profile + '-write', '/bin/sh', '-eu', '-c', 'printf "%s" "$2" > "$1"',
                  'write', directory + '/format-check.txt', contents)
            assert guest(profile + '-read', '/bin/cat', directory + '/format-check.txt').decode() == contents
            assert native_format(profile + '-mounted-refusal', device) != 0
            assert guest(profile + '-mounted-still-readable', '/bin/cat',
                         directory + '/format-check.txt').decode() == contents
            endpoints = control(profile + '-mounted-endpoints', 'list')
            assert len(endpoints) == 1
            info = control(profile + '-mounted-info', 'request', endpoints[0]['endpoint'], 'getInfo')
            assert uuid.UUID(info['volume']) == volume_uuid, info
            detach(profile + '-detach-writable', image)
            attached = False
            persisted = output / ('persisted-' + profile + '.img')
            guest(profile + '-export-persisted', '/bin/cat', image, destination=persisted, timeout=90)
            oracle(profile + '-persisted', persisted)
            persisted_hash = digest(persisted)
            mount(profile + '-remount-readonly', image, directory, True)
            attached = True
            assert guest(profile + '-remount-read', '/bin/cat', directory + '/format-check.txt').decode() == contents
            detach(profile + '-detach-remount', image)
            attached = False
            assert guest(profile + '-readonly-remount-unchanged', '/usr/bin/shasum', '-a', '256',
                         image).decode().split()[0] == persisted_hash
            assert digest(blank) == blank_hash
            result.update(passed=True, native_format_exit=0, independent_fsck_passed=True,
                          mounted_format_refused=True, file_persisted=True,
                          persisted_sha256=persisted_hash)
        except GuestTimeout as error:
            result.update(error=str(error), recovery_required=True,
                          cleanup_note='Preserve task device and process for diagnosis')
        except Exception as error:
            result['error'] = str(error)
        finally:
            if attached and not result.get('recovery_required'):
                try:
                    detach(profile + '-failure-cleanup', image)
                except Exception as error:
                    result['cleanup_error'] = str(error)
            (output / 'summary.json').write_text(json.dumps(results, indent=2) + '\n')
            print(json.dumps({profile: result}), flush=True)
        if not result['passed']:
            break
    return 0 if len(results) == len(args.profiles) and all(r['passed'] for r in results.values()) else 1


if __name__ == '__main__':
    raise SystemExit(main())

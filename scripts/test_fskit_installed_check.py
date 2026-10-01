#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
"""Check and repair disposable devices through the installed FSKit module."""

import argparse
import getpass
import hashlib
import json
from pathlib import Path
import plistlib
import re
import subprocess
import sys

from fskit_test_vm import GuestTimeout, guest_commands, image_devices


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--tart', type=Path, required=True)
    parser.add_argument('--vm', required=True)
    parser.add_argument('--fixtures', type=Path, required=True)
    parser.add_argument('--guest-share', required=True)
    parser.add_argument('--guest-workdir', required=True)
    parser.add_argument('--build-number', type=int, required=True)
    parser.add_argument('--profiles', nargs='+', choices=('1k', '4k'), default=['1k', '4k'])
    parser.add_argument('--sudo', action='store_true', help='Authorize fsck as the VM administrator')
    parser.add_argument('--e2fsck', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    if args.build_number < 1 or not args.guest_workdir.startswith('/') or args.guest_workdir == '/':
        parser.error('Use a positive build number and a new absolute guest directory')
    if len(set(args.profiles)) != len(args.profiles):
        parser.error('Profiles must not repeat')
    if args.sudo and not sys.stdin.isatty():
        parser.error('Use a terminal for the no-echo VM password prompt')
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    fixtures = args.fixtures.resolve()
    guest = guest_commands(args.tart.resolve(), args.vm, output)
    root = args.guest_workdir.rstrip('/')
    share = args.guest_share.rstrip('/')
    app = '/Applications/Machlin ext4.app/Contents/MacOS/Machlin ext4'
    password = (getpass.getpass('Dedicated VM administrator password: ') + '\n').encode() if args.sudo else None
    results = {}

    def control(label, *command):
        return json.loads(guest(label, app, '--control', *command))

    def command_status(label):
        matches = list(output.glob('*-' + label + '.status.json'))
        assert len(matches) == 1, f'Expected one invocation for {label}'
        return json.loads(matches[0].read_text())['exit_code']

    def check(label, device, repair=False):
        command = ['/sbin/fsck_fskit', '--progress', '-t', 'machlinext4', '-f',
                   '-y' if repair else '-n', device]
        if args.sudo:
            command = ['/usr/bin/sudo', '-S', '-p', '', '--', *command]
        guest(label, *command, required=False, timeout=120, stdin=password)
        return command_status(label)

    def attach(label, image, readonly):
        attached = plistlib.loads(guest(label, '/usr/bin/hdiutil', 'attach', '-nomount',
            '-readonly' if readonly else '-readwrite', '-plist', '-imagekey',
            'diskimage-class=CRawDiskImage', image))
        entries = [entry for entry in attached['system-entities'] if 'dev-entry' in entry]
        assert len(entries) == 1 and re.fullmatch(r'/dev/disk[0-9]+', entries[0]['dev-entry'])
        assert not entries[0].get('mount-point'), 'Maintenance must not run on a mounted volume'
        return entries[0]['dev-entry']

    def detach(label, image):
        for device in image_devices(guest, label + '-devices', image):
            guest(label, '/usr/bin/hdiutil', 'detach', device)
        assert control(label + '-endpoints', 'list') == []

    assert int(guest('uid', '/usr/bin/id', '-u')) != 0
    guest('os', '/usr/bin/sw_vers')
    guest('kernel', '/usr/bin/uname', '-v')
    guest('sip', '/usr/bin/csrutil', 'status')
    assert guest('build', '/usr/libexec/PlistBuddy', '-c', 'Print :CFBundleVersion',
                 '/Applications/Machlin ext4.app/Contents/Info.plist').decode().strip() == str(args.build_number)
    assert control('modules', 'modules')[0]['enabled'] is True
    assert control('service', 'device-service')['status'] == 'enabled'
    assert control('initial-endpoints', 'list') == []
    guest('prepare', '/bin/sh', '-eu', '-c',
          'umask 077; test ! -e "$1"; mkdir -p "$1"', 'prepare', root)

    for profile in args.profiles:
        result = {'passed': False, 'administrative_check': args.sudo}
        results[profile] = result
        clean_name = f'ext4-check-clean-{profile}.img'
        damaged_name = f'ext4-check-damaged-{profile}.img'
        clean = root + '/' + clean_name
        damaged = root + '/' + damaged_name
        current_image = None
        try:
            guest(profile + '-copy-clean', '/bin/cp', share + '/' + clean_name, clean)
            guest(profile + '-copy-damaged', '/bin/cp', share + '/' + damaged_name, damaged)
            clean_hash = digest(fixtures / clean_name)
            damaged_hash = digest(fixtures / damaged_name)
            assert guest(profile + '-clean-hash', '/usr/bin/shasum', '-a', '256', clean).decode().split()[0] == clean_hash
            assert guest(profile + '-damaged-hash', '/usr/bin/shasum', '-a', '256', damaged).decode().split()[0] == damaged_hash
            for name, image, expected_hash in (('clean', clean, clean_hash), ('damaged', damaged, damaged_hash)):
                current_image = image
                device = attach(profile + '-' + name + '-attach-readonly', image, True)
                code = check(profile + '-' + name + '-verify', device)
                assert (code == 0) == (name == 'clean'), f'{name} verification returned {code}'
                result[name + '_verify_exit'] = code
                detach(profile + '-' + name + '-detach-readonly', image)
                current_image = None
                assert guest(profile + '-' + name + '-unchanged', '/usr/bin/shasum', '-a', '256', image).decode().split()[0] == expected_hash
            result['readonly_unchanged'] = True
            current_image = damaged
            device = attach(profile + '-attach-repair', damaged, False)
            code = check(profile + '-repair', device, True)
            assert code == 0, f'Repair returned {code}'
            result['repair_exit'] = code
            detach(profile + '-detach-repair', damaged)
            current_image = None
            export = output / ('repaired-' + profile + '.img')
            guest(profile + '-export', '/bin/cat', damaged, destination=export, timeout=90)
            repaired_hash = digest(export)
            assert export.stat().st_size == (fixtures / damaged_name).stat().st_size
            assert repaired_hash != damaged_hash, 'Repair did not change damaged media'
            with (output / (profile + '-independent-fsck.log')).open('wb') as log:
                oracle = subprocess.run([str(args.e2fsck.resolve()), '-fn', str(export)],
                                        stdout=log, stderr=subprocess.STDOUT, timeout=90)
            assert oracle.returncode == 0, 'Independent checker rejected native repair'
            current_image = damaged
            device = attach(profile + '-attach-repaired-readonly', damaged, True)
            assert check(profile + '-verify-repaired', device) == 0
            detach(profile + '-detach-repaired-readonly', damaged)
            current_image = None
            mount = root + '/mount-' + profile
            guest(profile + '-mount-directory', '/bin/mkdir', mount)
            current_image = damaged
            attached = plistlib.loads(guest(profile + '-mount-repaired', '/usr/bin/hdiutil', 'attach',
                '-readonly', '-owners', 'off', '-nobrowse', '-mountpoint', mount, '-plist',
                '-imagekey', 'diskimage-class=CRawDiskImage', damaged))
            mounted = [entry for entry in attached['system-entities'] if entry.get('mount-point')]
            assert len(mounted) == 1 and mounted[0]['mount-point'] == mount
            assert mounted[0].get('volume-kind') == 'machlinext4'
            guest(profile + '-root-read', '/bin/ls', '-a', mount)
            detach(profile + '-detach-mount', damaged)
            current_image = None
            assert guest(profile + '-final-hash', '/usr/bin/shasum', '-a', '256', damaged).decode().split()[0] == repaired_hash
            assert digest(fixtures / clean_name) == clean_hash and digest(fixtures / damaged_name) == damaged_hash
            result.update(passed=True, fsck_exit=0, clean_sha256=clean_hash,
                          damaged_sha256=damaged_hash, repaired_sha256=repaired_hash)
        except GuestTimeout as error:
            result.update(error=str(error), recovery_required=True,
                          cleanup_note='Preserve the task device and process for diagnosis')
        except Exception as error:
            result['error'] = str(error)
        finally:
            if current_image is not None and not result.get('recovery_required'):
                try:
                    detach(profile + '-failure-cleanup', current_image)
                except Exception as error:
                    result['cleanup_error'] = str(error)
            (output / 'summary.json').write_text(json.dumps(results, indent=2) + '\n')
            print(json.dumps({profile: result}), flush=True)
        if not result['passed']:
            break
    return 0 if len(results) == len(args.profiles) and all(r['passed'] for r in results.values()) else 1


if __name__ == '__main__':
    raise SystemExit(main())

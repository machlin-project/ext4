#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
"""Recover real interrupted transactions through installed FSKit in a disposable VM."""
import argparse
import hashlib
import json
import plistlib
import re
import subprocess
from pathlib import Path
from fskit_test_vm import GuestTimeout, guest_commands, image_devices


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--tart', type=Path, required=True)
    parser.add_argument('--vm', required=True)
    parser.add_argument('--fixtures', type=Path, required=True)
    parser.add_argument('--guest-share', required=True)
    parser.add_argument('--checker', required=True)
    parser.add_argument('--guest-workdir', required=True)
    parser.add_argument('--build-number', type=int, required=True)
    parser.add_argument('--e2fsck', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    if args.build_number < 1 or not args.guest_workdir.startswith('/') or args.guest_workdir == '/':
        parser.error('Use a positive build number and a new absolute guest directory')
    if Path(args.checker).name != args.checker:
        parser.error('--checker must be a staged filename')
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=False)
    guest = guest_commands(args.tart.resolve(), args.vm, out)
    fixtures = args.fixtures.resolve()
    root = args.guest_workdir.rstrip('/')
    share = args.guest_share.rstrip('/')
    app = '/Applications/Machlin ext4.app/Contents/MacOS/Machlin ext4'

    def control(label, *command):
        return json.loads(guest(label, app, '--control', *command))

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
    guest('prepare', '/bin/sh', '-eu', '-c', 'umask 077; test ! -e "$1"; mkdir -p "$1"', 'prepare', root)
    guest('copy-checker', '/bin/cp', share + '/' + args.checker, root + '/checker')
    guest('checker-mode', '/bin/chmod', '755', root + '/checker')
    checker_hash = hashlib.sha256((fixtures / args.checker).read_bytes()).hexdigest()
    assert guest('checker-hash', '/usr/bin/shasum', '-a', '256', root + '/checker').decode().split()[0] == checker_hash
    results = {}

    for profile in ('4k', '1k'):
        filename = f'ext4-recovery-required-{profile}-build{args.build_number}.img'
        image = root + '/' + filename
        mount = root + '/mount-' + profile
        result = {'passed': False, 'checker_sha256': checker_hash}
        results[profile] = result
        try:
            guest(profile + '-copy', '/bin/cp', share + '/' + filename, image)
            before = hashlib.sha256((fixtures / filename).read_bytes()).hexdigest()
            assert guest(profile + '-initial-hash', '/usr/bin/shasum', '-a', '256', image).decode().split()[0] == before
            guest(profile + '-directory', '/bin/mkdir', mount)
            # Discovery must not repair an image supplied as read-only. A failed
            # automount can still attach the device; inspect its actual state.
            guest(profile + '-readonly-attempt', '/usr/bin/hdiutil', 'attach', '-readonly',
                  '-owners', 'off', '-nobrowse', '-mountpoint', mount, '-plist',
                  '-imagekey', 'diskimage-class=CRawDiskImage', image, required=False)
            for device in image_devices(guest, profile + '-readonly-devices', image):
                disk = plistlib.loads(guest(profile + '-readonly-info', '/usr/sbin/diskutil', 'info', '-plist', device))
                assert not disk.get('Mounted', False) and not disk.get('MountPoint')
            assert control(profile + '-readonly-endpoints', 'list') == []
            detach(profile + '-readonly-detach', image)
            assert guest(profile + '-readonly-hash', '/usr/bin/shasum', '-a', '256', image).decode().split()[0] == before
            result['readonly_unchanged'] = True
            for writable in (True, False):
                label = profile + ('-recover' if writable else '-remount')
                attached = plistlib.loads(guest(label + '-attach', '/usr/bin/hdiutil', 'attach',
                    '-readwrite' if writable else '-readonly', '-owners', 'off', '-nobrowse',
                    '-mountpoint', mount, '-plist', '-imagekey', 'diskimage-class=CRawDiskImage', image))
                entities = [entry for entry in attached['system-entities'] if 'dev-entry' in entry]
                assert len(entities) == 1 and re.fullmatch(r'/dev/disk[0-9]+', entities[0]['dev-entry'])
                assert entities[0].get('mount-point') == mount and entities[0].get('volume-kind') == 'machlinext4'
                endpoints = control(label + '-endpoints', 'list')
                assert len(endpoints) == 1 and endpoints[0]['info']['readOnly'] == (not writable)
                # The fixture failed after its commit record, before checkpointing.
                assert guest(label + '-committed-data', '/bin/cat', mount + '/failure') == b'failure'
                guest(label + '-writes', root + '/checker', mount, 'write' if writable else 'verify', timeout=180)
                if writable:
                    result['special_files'] = json.loads(guest(label + '-special', root + '/checker', mount, 'special'))
                detach(label + '-detach', image)
            export = out / ('recovered-' + profile + '.img')
            guest(profile + '-export', '/bin/cat', image, destination=export, timeout=90)
            after = hashlib.sha256(export.read_bytes()).hexdigest()
            assert after != before and export.stat().st_size == (fixtures / filename).stat().st_size
            assert guest(profile + '-export-hash', '/usr/bin/shasum', '-a', '256', image).decode().split()[0] == after
            with (out / (profile + '-fsck.log')).open('wb') as log:
                check = subprocess.run([str(args.e2fsck.resolve()), '-fn', str(export)],
                                       stdout=log, stderr=subprocess.STDOUT, timeout=90)
            assert check.returncode == 0
            result.update(passed=True, fsck_exit=0, interrupted_sha256=before, recovered_sha256=after)
        except GuestTimeout as error:
            result.update(error=str(error), recovery_required=True,
                          cleanup_note='Leave the device intact for stack capture; the guest command may still be running')
        except Exception as error:
            result['error'] = str(error)
        finally:
            if not result['passed'] and not result.get('recovery_required'):
                try:
                    detach(profile + '-failure-cleanup', image)
                except Exception as error:
                    result['cleanup_error'] = str(error)
            (out / 'summary.json').write_text(json.dumps(results, indent=2) + '\n')
            print(json.dumps({profile: result}), flush=True)
        if not result['passed']:
            break
    return 0 if len(results) == 2 and all(value['passed'] for value in results.values()) else 1


if __name__ == '__main__':
    raise SystemExit(main())

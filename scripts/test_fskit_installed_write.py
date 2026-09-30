#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
"""Exercise installed FSKit writes, remount and independent fsck in a disposable Tart VM."""
import argparse
import hashlib
import json
import plistlib
import re
import subprocess
from pathlib import Path


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--tart', type=Path, required=True)
    parser.add_argument('--vm', required=True)
    parser.add_argument('--fixtures', type=Path, required=True)
    parser.add_argument('--guest-share', required=True)
    parser.add_argument('--checker', required=True, help='Unique staged checker filename')
    parser.add_argument('--guest-workdir', required=True, help='New absolute guest directory')
    parser.add_argument('--build-number', type=int, required=True)
    parser.add_argument('--e2fsck', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True, help='New evidence directory')
    args = parser.parse_args()
    if args.build_number < 1 or not args.guest_workdir.startswith('/') or args.guest_workdir == '/':
        parser.error('Use a positive build number and a new absolute guest directory')
    if Path(args.checker).name != args.checker:
        parser.error('--checker must be a filename within the fixture directory')
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=False)
    tart = args.tart.resolve()
    fixtures = args.fixtures.resolve()
    root = args.guest_workdir.rstrip('/')
    app = '/Applications/Machlin ext4.app/Contents/MacOS/Machlin ext4'
    serial = 0

    def guest(label, *command, required=True, timeout=30, destination=None):
        nonlocal serial
        serial += 1
        prefix = out / f'{serial:03d}-{label}'
        argv = [str(tart), 'exec', args.vm, *command]
        with (destination or prefix.with_suffix('.stdout.log')).open('wb') as output:
            try:
                result = subprocess.run(argv, stdout=output, stderr=subprocess.PIPE, timeout=timeout)
            except subprocess.TimeoutExpired as error:
                prefix.with_suffix('.stderr.log').write_bytes(error.stderr or b'')
                prefix.with_suffix('.status.json').write_text(json.dumps(
                    {'argv': argv, 'timed_out': True, 'timeout': timeout}, indent=2) + '\n')
                raise RuntimeError(f'{label} timed out; inspect the task process before retry') from error
        prefix.with_suffix('.stderr.log').write_bytes(result.stderr)
        prefix.with_suffix('.status.json').write_text(json.dumps(
            {'argv': argv, 'exit_code': result.returncode}, indent=2) + '\n')
        if required and result.returncode:
            raise RuntimeError(f'{label}: exit {result.returncode}')
        return prefix.with_suffix('.stdout.log').read_bytes() if destination is None else b''

    def control(label, *command):
        return json.loads(guest(label, app, '--control', *command))

    assert int(guest('uid', '/usr/bin/id', '-u').strip()) != 0, 'Use an ordinary test user'
    guest('os', '/usr/bin/sw_vers')
    guest('loaded-kernel', '/usr/bin/uname', '-v')
    guest('sip', '/usr/bin/csrutil', 'status')
    assert guest('build', '/usr/libexec/PlistBuddy', '-c', 'Print :CFBundleVersion',
                 '/Applications/Machlin ext4.app/Contents/Info.plist').decode().strip() == str(args.build_number)
    assert control('modules', 'modules')[0]['enabled'] is True
    assert control('service', 'device-service')['status'] == 'enabled'
    assert control('endpoints', 'list') == []
    guest('prepare', '/bin/sh', '-eu', '-c', 'umask 077; test ! -e "$1"; mkdir -p "$1"', 'prepare', root)
    guest('copy-checker', '/bin/cp', args.guest_share.rstrip('/') + '/' + args.checker, root + '/checker')
    guest('checker-mode', '/bin/chmod', '755', root + '/checker')
    checker_hash = hashlib.sha256((fixtures / args.checker).read_bytes()).hexdigest()
    assert guest('checker-hash', '/usr/bin/shasum', '-a', '256', root + '/checker').decode().split()[0] == checker_hash
    results = {}

    for profile in ('4k', '1k'):
        filename = f'ext4-metadata-{profile}.img'
        image = root + '/' + filename
        mount = root + '/mount-' + profile
        device = None
        result = {'passed': False, 'checker_sha256': checker_hash}
        results[profile] = result
        try:
            guest(profile + '-copy', '/bin/cp', args.guest_share.rstrip('/') + '/' + filename, image)
            digest = hashlib.sha256((fixtures / filename).read_bytes()).hexdigest()
            assert guest(profile + '-initial-hash', '/usr/bin/shasum', '-a', '256', image).decode().split()[0] == digest
            guest(profile + '-mount-directory', '/bin/mkdir', mount)
            for mode in ('write', 'verify'):
                label = profile + '-' + mode
                attachment = plistlib.loads(guest(label + '-mount', '/usr/bin/hdiutil', 'attach',
                    '-readwrite' if mode == 'write' else '-readonly', '-owners', 'off', '-nobrowse',
                    '-mountpoint', mount, '-plist', '-imagekey', 'diskimage-class=CRawDiskImage', image))
                entries = [entry for entry in attachment['system-entities'] if 'dev-entry' in entry]
                assert len(entries) == 1 and re.fullmatch(r'/dev/disk[0-9]+', entries[0]['dev-entry'])
                device = entries[0]['dev-entry']
                assert entries[0].get('mount-point') == mount and entries[0].get('volume-kind') == 'machlinext4'
                disk = plistlib.loads(guest(label + '-disk', '/usr/sbin/diskutil', 'info', '-plist', device))
                assert disk['MountPoint'] == mount and disk['GlobalPermissionsEnabled'] is False
                endpoints = control(label + '-control', 'list')
                assert len(endpoints) == 1
                assert endpoints[0]['info']['readOnly'] == (mode == 'verify'), endpoints[0]['info']
                guest(label + '-check', root + '/checker', mount, mode, timeout=180)
                guest(label + '-detach', '/usr/bin/hdiutil', 'detach', device)
                device = None
                assert control(label + '-cleanup', 'list') == []
            export = out / ('written-' + profile + '.img')
            guest(profile + '-export', '/bin/cat', image, destination=export, timeout=90)
            assert export.stat().st_size == (fixtures / filename).stat().st_size
            after = hashlib.sha256(export.read_bytes()).hexdigest()
            assert after != digest
            assert guest(profile + '-export-hash', '/usr/bin/shasum', '-a', '256', image).decode().split()[0] == after
            with (out / (profile + '-fsck.log')).open('wb') as log:
                check = subprocess.run([str(args.e2fsck.resolve()), '-fn', str(export)], stdout=log,
                                       stderr=subprocess.STDOUT, timeout=90)
            result.update(fsck_exit=check.returncode, original_sha256=digest, written_sha256=after)
            assert check.returncode == 0, f'{profile}: independent fsck failed'
            result['passed'] = True
        except Exception as error:
            result['error'] = str(error)
        finally:
            if device is not None:
                try:
                    guest(profile + '-failure-detach', '/usr/bin/hdiutil', 'detach', device)
                    assert control(profile + '-failure-endpoints', 'list') == []
                except Exception as error:
                    result['cleanup_error'] = str(error)
            (out / 'summary.json').write_text(json.dumps(results, indent=2) + '\n')
            print(json.dumps({profile: result}), flush=True)
        if not result['passed']:
            break
    return 0 if len(results) == 2 and all(value['passed'] for value in results.values()) else 1


if __name__ == '__main__':
    raise SystemExit(main())

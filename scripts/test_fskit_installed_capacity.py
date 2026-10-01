#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
"""Check native ENOSPC against remount and independent inode evidence in a disposable VM."""
import argparse
import hashlib
import json
import plistlib
import re
import subprocess
import time
from pathlib import Path

from fskit_test_vm import GuestTimeout, guest_commands


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--tart', type=Path, required=True)
    parser.add_argument('--vm', required=True)
    parser.add_argument('--image', type=Path, required=True,
                        help='Disposable full-volume export containing the owned pressure file')
    parser.add_argument('--checker', type=Path, required=True)
    parser.add_argument('--build-number', type=int, required=True)
    parser.add_argument('--guest-workdir', required=True, help='New absolute guest directory')
    parser.add_argument('--layouts', nargs='+', choices=('aligned', 'tail'),
                        default=['aligned', 'tail'])
    parser.add_argument('--e2fsck', type=Path, required=True)
    parser.add_argument('--debugfs', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True, help='New evidence directory')
    args = parser.parse_args()
    if args.build_number < 1 or not args.guest_workdir.startswith('/') or args.guest_workdir == '/':
        parser.error('Use a positive build number and a new absolute guest directory')
    if len(set(args.layouts)) != len(args.layouts):
        parser.error('--layouts must not repeat a case')
    if not 0 < args.image.stat().st_size <= 256 * 1024 * 1024:
        parser.error('Use a bounded pressure image of at most 256 MiB')
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=False)
    guest = guest_commands(args.tart.resolve(), args.vm, out)
    root = args.guest_workdir.rstrip('/')
    app = '/Applications/Machlin ext4.app/Contents/MacOS/Machlin ext4'

    def control(label, *command):
        return json.loads(guest(label, app, '--control', *command))

    def attach(label, path, mount, readonly):
        attached = plistlib.loads(guest(label, '/usr/bin/hdiutil', 'attach',
            '-readonly' if readonly else '-readwrite', '-owners', 'on', '-nobrowse',
            '-mountpoint', mount, '-plist', '-imagekey', 'diskimage-class=CRawDiskImage', path))
        devices = [entry for entry in attached['system-entities'] if 'dev-entry' in entry]
        assert len(devices) == 1
        device = devices[0]
        assert device['mount-point'] == mount and device['volume-kind'] == 'machlinext4'
        return device['dev-entry']

    assert int(guest('uid', '/usr/bin/id', '-u').strip()) != 0
    guest('os', '/usr/bin/sw_vers')
    guest('loaded-kernel', '/usr/bin/uname', '-v')
    guest('sip', '/usr/bin/csrutil', 'status')
    assert guest('build', '/usr/libexec/PlistBuddy', '-c', 'Print :CFBundleVersion',
                 '/Applications/Machlin ext4.app/Contents/Info.plist').strip().decode() == str(args.build_number)
    assert control('modules', 'modules')[0]['enabled']
    assert control('service', 'device-service')['status'] == 'enabled'
    assert control('endpoints', 'list') == []
    guest('prepare', '/bin/sh', '-eu', '-c', 'umask 077; test ! -e "$1"; mkdir -p "$1"', 'prepare', root)
    provenance = {'build_number': args.build_number}
    for name, path in (('source.img', args.image), ('checker', args.checker)):
        data = path.read_bytes()
        digest = hashlib.sha256(data).hexdigest()
        guest('stage-' + name, '/bin/sh', '-eu', '-c', 'umask 077; cat > "$1"',
              'stage', root + '/' + name, stdin=data, timeout=90)
        assert guest('hash-' + name, '/usr/bin/shasum', '-a', '256',
                     root + '/' + name).decode().split()[0] == digest
        provenance[name] = {'source': str(path.resolve()), 'sha256': digest}
    (out / 'inputs.json').write_text(json.dumps(provenance, indent=2) + '\n')
    guest('checker-mode', '/bin/chmod', '700', root + '/checker')
    results = {}
    for layout in args.layouts:
        for mode in ('uncached', 'cached'):
            label = layout + '-' + mode
            result = {'passed': False}
            results[label] = result
            path = root + '/' + label + '.img'
            mount = root + '/mount-' + label
            device = None
            attachment_pending = False
            started = time.monotonic()
            try:
                guest(label + '-copy', '/bin/cp', root + '/source.img', path)
                guest(label + '-mkdir', '/bin/mkdir', mount)
                attachment_pending = True
                device = attach(label + '-mount', path, mount, False)
                attachment_pending = False
                disk = plistlib.loads(guest(label + '-disk', '/usr/sbin/diskutil', 'info', '-plist', device))
                assert disk['GlobalPermissionsEnabled']
                result['before'] = control(label + '-before', 'list')
                payload = guest(label + '-write', root + '/checker', mount, mode, layout,
                                required=False, timeout=90)
                check = json.loads(payload) if payload.strip() else {'passed': False}
                result['write'] = check
                result['after'] = control(label + '-after', 'list')
                guest(label + '-detach', '/usr/bin/hdiutil', 'detach', device)
                device = None
                assert control(label + '-cleanup', 'list') == []

                # A live stat can hide a committed tail and make its read loop
                # empty. Require a fresh native view and an independent inode
                # comparison even when the original checker reports success.
                if 'start' in check and check.get('size', -1) >= check['start']:
                    before = guest(label + '-readonly-before', '/usr/bin/shasum', '-a', '256', path).decode().split()[0]
                    attachment_pending = True
                    device = attach(label + '-remount', path, mount, True)
                    attachment_pending = False
                    payload = guest(label + '-verify', root + '/checker', mount, 'verify',
                                    str(check['start']), str(check['size']), required=False, timeout=90)
                    result['remount'] = json.loads(payload) if payload.strip() else {'passed': False}
                    guest(label + '-readonly-detach', '/usr/bin/hdiutil', 'detach', device)
                    device = None
                    assert control(label + '-readonly-cleanup', 'list') == []
                    after = guest(label + '-readonly-after', '/usr/bin/shasum', '-a', '256', path).decode().split()[0]
                    result['readonly_unchanged'] = before == after

                export = out / (label + '.img')
                guest(label + '-export', '/bin/cat', path, destination=export, timeout=90)
                result['sha256'] = hashlib.sha256(export.read_bytes()).hexdigest()
                assert guest(label + '-export-hash', '/usr/bin/shasum', '-a', '256', path).decode().split()[0] == result['sha256']
                with (out / (label + '-fsck.log')).open('wb') as log:
                    fsck = subprocess.run([str(args.e2fsck.resolve()), '-fn', str(export)],
                                          stdout=log, stderr=subprocess.STDOUT, timeout=90)
                result['fsck_exit'] = fsck.returncode
                inode = subprocess.run([str(args.debugfs.resolve()), '-R',
                    'stat /acceptance-write/space-pressure', str(export)],
                    stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=30, check=True)
                (out / (label + '-inode.log')).write_bytes(inode.stdout)
                size = re.search(rb'\bSize:\s*(\d+)', inode.stdout)
                assert size is not None, 'Independent inode size missing'
                result['independent_size'] = int(size[1])
                result['size_consistent'] = result['independent_size'] == check.get('size')
                result['passed'] = (check.get('passed', False) and
                    result.get('remount', {}).get('passed', False) and
                    result.get('readonly_unchanged', False) and result['size_consistent'] and fsck.returncode == 0)
            except GuestTimeout as error:
                result.update(error=str(error), recovery_required=True, device=device)
            except Exception as error:
                result.update(error=str(error),
                              recovery_required=device is not None or attachment_pending, device=device)
            finally:
                result['elapsed_seconds'] = time.monotonic() - started
                (out / 'summary.json').write_text(json.dumps(results, indent=2) + '\n')
                print(json.dumps({label: {key: value for key, value in result.items()
                                         if key not in ('before', 'after')}}), flush=True)
            if result.get('recovery_required'):
                # A timed-out process may still own the device. Preserve it for
                # diagnosis; an automatic detach/reboot would change the evidence.
                return 1
    guest('write-trace', '/usr/bin/log', 'show', '--style', 'compact', '--last', '10m', '--info', '--debug',
          '--predicate', 'process == "Ext4FSKitExtension" AND eventMessage CONTAINS "Machlin ext4 "', required=False)
    assert control('final-endpoints', 'list') == []
    assert control('final-service', 'device-service')['status'] == 'enabled'
    return 0 if all(result['passed'] for result in results.values()) else 1


if __name__ == '__main__':
    raise SystemExit(main())

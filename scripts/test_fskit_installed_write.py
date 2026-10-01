#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
"""Exercise installed FSKit writes, remount and independent fsck in a disposable Tart VM."""
import argparse
import errno
import getpass
import hashlib
import json
import plistlib
import re
import subprocess
import sys
import time
from pathlib import Path
from fskit_test_vm import GuestTimeout, guest_commands, image_devices

MEMORY_PRESSURE_SECONDS = 30
MEMORY_PRESSURE_IO_SECONDS = 20


class MemoryPressure:
    """Bounded notification simulation inside the disposable guest, never the host."""

    def __init__(self, tart, vm, output, label, level, password):
        self.prefix = output / (label + '-memory-pressure')
        self.stdout = self.prefix.with_suffix('.stdout.log').open('wb')
        self.stderr = self.prefix.with_suffix('.stderr.log').open('wb')
        self.argv = [str(tart), 'exec', '-i', vm, '/usr/bin/sudo', '-S', '-p', '', '--',
                     '/usr/bin/memory_pressure', '-S', '-l', level,
                     '-s', str(MEMORY_PRESSURE_SECONDS)]
        self.process = subprocess.Popen(self.argv, stdin=subprocess.PIPE,
                                        stdout=self.stdout, stderr=self.stderr)
        self.prefix.with_suffix('.command.json').write_text(json.dumps({
            'argv': self.argv, 'host_rpc_pid': self.process.pid,
        }, indent=2) + '\n')
        self.process.stdin.write(password)
        self.process.stdin.close()
        self.finished = False

    def finish(self):
        if self.finished:
            return
        try:
            code = self.process.wait(timeout=45)
            status = {'argv': self.argv, 'exit_code': code,
                      'scope': 'simulated guest notifications, no allocated-memory stress'}
        except subprocess.TimeoutExpired as error:
            status = {'argv': self.argv, 'timed_out': True, 'guest_may_be_running': True}
            raise GuestTimeout('Memory-pressure simulation did not finish; preserve guest state') from error
        finally:
            self.stdout.close()
            self.stderr.close()
            self.finished = True
            self.prefix.with_suffix('.status.json').write_text(json.dumps(status, indent=2) + '\n')
        if code != 0:
            raise RuntimeError(f'Memory-pressure simulation: exit {code}')


def await_read_state(guest, app, endpoint, label, active):
    deadline = time.monotonic() + 10
    while True:
        settings = json.loads(guest(label, app, '--control', 'request', endpoint, 'getSettings',
                                    timeout=5))
        assert settings.get('retainReadState') is True, settings
        if settings.get('readStateRetentionActive') is active:
            return settings
        if time.monotonic() >= deadline:
            raise RuntimeError(f'{label}: expected retention active={active}, got {settings}')
        time.sleep(0.25)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--tart', type=Path, required=True)
    parser.add_argument('--vm', required=True)
    parser.add_argument('--fixtures', type=Path, required=True)
    parser.add_argument('--guest-share', required=True)
    parser.add_argument('--checker', required=True, help='Unique staged checker filename')
    parser.add_argument('--fixture-prefix', default='ext4-metadata')
    parser.add_argument('--profiles', nargs='+', choices=('4k', '1k'), default=['4k', '1k'],
                        help='Select affected block sizes for a focused reproduction')
    parser.add_argument('--owners', choices=('on', 'off'), default='off')
    parser.add_argument('--mount-as-root', action='store_true',
                        help='Use sudo only for image attach/detach; keep file operations and control IPC unprivileged')
    parser.add_argument('--memory-pressure', choices=('warn', 'critical'),
                        help='Simulate bounded guest memory-pressure notifications during ordinary I/O')
    parser.add_argument('--extended', action='store_true',
                        help='Exercise native permissions, preallocation, ENOSPC and volume rename')
    parser.add_argument('--extended-checks', nargs='+', choices=('policy', 'setid', 'pressure', 'rename', 'seek'),
                        default=None, help='Select affected groups within the extended suite')
    parser.add_argument('--pressure-timeout', type=int, default=600,
                        help='Explicit deadline in seconds for the selected pressure workload')
    parser.add_argument('--checker-timeout', type=int, default=180,
                        help='Deadline in seconds for each native write/remount checker invocation')
    parser.add_argument('--guest-workdir', required=True, help='New absolute guest directory')
    parser.add_argument('--build-number', type=int, required=True)
    parser.add_argument('--e2fsck', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True, help='New evidence directory')
    args = parser.parse_args()
    if args.extended_checks is not None and not args.extended:
        parser.error('--extended-checks requires --extended')
    if len(set(args.profiles)) != len(args.profiles):
        parser.error('--profiles must not repeat a block size')
    if args.pressure_timeout <= 0 or args.checker_timeout <= 0:
        parser.error('Checker and pressure deadlines must be positive')
    if args.memory_pressure and args.extended:
        parser.error('Memory-pressure notification checks use the ordinary write/remount suite')
    if (args.mount_as_root or args.memory_pressure) and not sys.stdin.isatty():
        parser.error('Run in a terminal for the no-echo VM sudo password prompt')
    checks = args.extended_checks or ('policy', 'setid', 'pressure', 'rename')
    if args.build_number < 1 or not args.guest_workdir.startswith('/') or args.guest_workdir == '/':
        parser.error('Use a positive build number and a new absolute guest directory')
    if Path(args.checker).name != args.checker:
        parser.error('--checker must be a filename within the fixture directory')
    if Path(args.fixture_prefix).name != args.fixture_prefix or (args.extended and args.owners != 'on'):
        parser.error('Use a filename prefix; extended checks require --owners on and owned fixtures')
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=False)
    tart = args.tart.resolve()
    fixtures = args.fixtures.resolve()
    root = args.guest_workdir.rstrip('/')
    app = '/Applications/Machlin ext4.app/Contents/MacOS/Machlin ext4'
    guest = guest_commands(tart, args.vm, out)
    password = None

    def device_command(label, *command):
        if not args.mount_as_root:
            return guest(label, *command)
        return guest(label, '/usr/bin/sudo', '-S', '-p', '', '--', *command,
                     stdin=password)

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
    if args.mount_as_root or args.memory_pressure:
        password = (getpass.getpass('Guest sudo password: ') + '\n').encode()
        assert guest('sudo-uid', '/usr/bin/sudo', '-S', '-p', '', '--', '/usr/bin/id', '-u',
                     stdin=password).strip() == b'0'
    guest('prepare', '/bin/sh', '-eu', '-c', 'umask 077; test ! -e "$1"; mkdir -p "$1"', 'prepare', root)
    guest('copy-checker', '/bin/cp', args.guest_share.rstrip('/') + '/' + args.checker, root + '/checker')
    guest('checker-mode', '/bin/chmod', '755', root + '/checker')
    checker_hash = hashlib.sha256((fixtures / args.checker).read_bytes()).hexdigest()
    assert guest('checker-hash', '/usr/bin/shasum', '-a', '256', root + '/checker').decode().split()[0] == checker_hash
    results = {}

    for profile in args.profiles:
        filename = f'{args.fixture_prefix}-{profile}.img'
        image = root + '/' + filename
        mount = root + '/mount-' + profile
        device = None
        result = {'passed': False, 'checker_sha256': checker_hash,
                  'pressure_timeout_seconds': args.pressure_timeout,
                  'checker_timeout_seconds': args.checker_timeout,
                  'mount_as_root': args.mount_as_root}
        results[profile] = result

        def check(name, *command, timeout=30):
            # A conformance failure must not discard independent remount/fsck
            # evidence. A timeout can still own the device and stops the run.
            try:
                guest(profile + '-' + name, *command, timeout=timeout)
            except GuestTimeout:
                raise
            except RuntimeError as error:
                result['checks'][name] = {'passed': False, 'error': str(error)}
                return False
            result['checks'][name] = {'passed': True}
            return True

        try:
            guest(profile + '-copy', '/bin/cp', args.guest_share.rstrip('/') + '/' + filename, image)
            digest = hashlib.sha256((fixtures / filename).read_bytes()).hexdigest()
            assert guest(profile + '-initial-hash', '/usr/bin/shasum', '-a', '256', image).decode().split()[0] == digest
            guest(profile + '-mount-directory', '/bin/mkdir', mount)
            for mode in ('write', 'verify'):
                label = profile + '-' + mode
                attachment = plistlib.loads(device_command(label + '-mount', '/usr/bin/hdiutil', 'attach',
                    '-readwrite' if mode == 'write' else '-readonly', '-owners', args.owners, '-nobrowse',
                    '-mountpoint', mount, '-plist', '-imagekey', 'diskimage-class=CRawDiskImage', image))
                entries = [entry for entry in attachment['system-entities'] if 'dev-entry' in entry]
                assert len(entries) == 1 and re.fullmatch(r'/dev/disk[0-9]+', entries[0]['dev-entry'])
                device = entries[0]['dev-entry']
                assert entries[0].get('mount-point') == mount and entries[0].get('volume-kind') == 'machlinext4'
                disk = plistlib.loads(guest(label + '-disk', '/usr/sbin/diskutil', 'info', '-plist', device))
                assert disk['MountPoint'] == mount and disk['GlobalPermissionsEnabled'] == (args.owners == 'on')
                endpoints = control(label + '-control', 'list')
                assert len(endpoints) == 1
                assert endpoints[0]['info']['readOnly'] == (mode == 'verify'), endpoints[0]['info']
                pressure = None
                try:
                    if args.memory_pressure:
                        endpoint = endpoints[0]['endpoint']
                        await_read_state(guest, app, endpoint, label + '-normal-retention', True)
                        pressure = MemoryPressure(tart, args.vm, out, label, args.memory_pressure, password)
                        await_read_state(guest, app, endpoint, label + '-pressure-retention', False)
                    guest(label + '-check', root + '/checker', mount, mode,
                          timeout=min(args.checker_timeout, MEMORY_PRESSURE_IO_SECONDS)
                          if pressure else args.checker_timeout)
                    if pressure:
                        await_read_state(guest, app, endpoint, label + '-pressure-after-io', False)
                        pressure.finish()
                        await_read_state(guest, app, endpoint, label + '-restored-retention', True)
                        result.setdefault('memory_pressure', {})[mode] = {
                            'level': args.memory_pressure, 'passed': True,
                            'scope': 'simulated notifications; native I/O and automatic policy',
                            'user_preference_preserved': True,
                        }
                finally:
                    if pressure:
                        pending = sys.exc_info()[1]
                        try:
                            pressure.finish()
                        except Exception as error:
                            if pending is None:
                                raise
                            result['memory_pressure_cleanup_error'] = str(error)
                            if isinstance(error, GuestTimeout):
                                result['recovery_required'] = True
                if args.extended:
                    if mode == 'write':
                        setup = ('/Applications/Machlin ext4.app/Contents/Helpers/'
                                 'Ext4DeviceSetup.app/Contents/MacOS/Ext4DeviceSetup')
                        denial = json.loads(guest(label + '-service-removal-denied', '/bin/sh', '-c',
                            '"$1" --unregister 2>&1; result=$?; test "$result" -ne 0', 'guard', setup))
                        assert denial['error']['domain'] == 'NSPOSIXErrorDomain'
                        assert denial['error']['code'] == errno.EBUSY
                        assert control(label + '-service-retained', 'device-service')['status'] == 'enabled'
                        result['checks'] = {}
                        for group, timeout in (('policy', 90), ('setid', 90),
                                               ('pressure', args.pressure_timeout), ('seek', 90)):
                            if group in checks:
                                check(group, root + '/checker', mount, group, timeout=timeout)
                        if 'rename' in checks:
                            check('diskutil-rename-short', '/usr/sbin/diskutil', 'renameVolume', device, 'ext4')
                            check('diskutil-rename', '/usr/sbin/diskutil', 'renameVolume', device, 'Machlin writable')
                            check('native-rename', root + '/checker', mount, 'rename')
                    elif result['checks'].get('native-rename', {}).get('passed'):
                        check('rename-persistence', root + '/checker', mount, 'rename-verify')
                        result['checks']['diskutil-label'] = {'passed': disk['VolumeName'] == 'Machlin writable'}
                    result['extended'] = True
                device_command(label + '-detach', '/usr/bin/hdiutil', 'detach', device)
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
            result.update(fsck_exit=check.returncode, original_sha256=digest, written_sha256=after,
                          ownership=args.owners)
            assert check.returncode == 0, f'{profile}: independent fsck failed'
            result['passed'] = all(check['passed'] for check in result.get('checks', {}).values())
        except GuestTimeout as error:
            result.update(error=str(error), recovery_required=True,
                          cleanup_note='Leave the device intact for stack capture; the guest command may still be running')
        except Exception as error:
            result['error'] = str(error)
        finally:
            if not result['passed'] and not result.get('recovery_required'):
                try:
                    for owned in image_devices(guest, profile + '-failure-devices', image):
                        device_command(profile + '-failure-detach', '/usr/bin/hdiutil', 'detach', owned)
                    assert control(profile + '-failure-endpoints', 'list') == []
                except Exception as error:
                    result['cleanup_error'] = str(error)
            (out / 'summary.json').write_text(json.dumps(results, indent=2) + '\n')
            print(json.dumps({profile: result}), flush=True)
        if 'error' in result or 'cleanup_error' in result:
            break
    return 0 if len(results) == len(args.profiles) and all(value['passed'] for value in results.values()) else 1


if __name__ == '__main__':
    raise SystemExit(main())

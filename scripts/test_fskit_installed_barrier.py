#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
"""Fail the device barrier in a disposable VM; require errors, recovery and independent fsck."""
import argparse
import getpass
import hashlib
import json
import os
import plistlib
import re
import select
import subprocess
import sys
import time
from pathlib import Path
from fskit_test_vm import GuestTimeout, guest_commands, image_devices


class Probe:
    """Coordinate an ordinary-user checker without logging interactive input."""

    def __init__(self, tart, vm, checker, mount, output, mode='fault'):
        self.output = output
        self.pending = b''
        self.stdout = (output / 'probe.stdout.log').open('wb')
        self.stderr = (output / 'probe.stderr.log').open('wb')
        argv = [str(tart), 'exec', '-i', vm, checker, mount, mode]
        (output / 'probe.command.json').write_text(json.dumps(argv, indent=2) + '\n')
        self.process = subprocess.Popen(argv, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                        stderr=self.stderr)

    def phase(self, name, timeout=45):
        deadline = time.monotonic() + timeout
        while True:
            while b'\n' in self.pending:
                line, self.pending = self.pending.split(b'\n', 1)
                record = json.loads(line)
                if record.get('phase') != name:
                    raise RuntimeError(f'Expected probe phase {name}, got {record}')
                return record
            remaining = deadline - time.monotonic()
            if remaining <= 0 or not select.select([self.process.stdout], [], [], remaining)[0]:
                raise GuestTimeout(f'Probe phase {name} timed out; preserve the device')
            data = os.read(self.process.stdout.fileno(), 4096)
            if not data:
                raise RuntimeError(f'Probe exited before phase {name}')
            self.stdout.write(data)
            self.stdout.flush()
            self.pending += data
            if len(self.pending) > 16384:
                raise RuntimeError('Probe output exceeded the bounded record size')

    def send(self, command):
        self.process.stdin.write((command + '\n').encode())
        self.process.stdin.flush()

    def close(self):
        # EOF releases a checker waiting for the next phase. A timed-out native
        # syscall may still own the volume, so never detach from this cleanup.
        try:
            self.process.stdin.close()
        except BrokenPipeError:
            pass
        try:
            code = self.process.wait(timeout=15)
            status = {'exit_code': code}
        except subprocess.TimeoutExpired:
            self.process.kill()  # Stop only this host RPC client, not a guest process.
            self.process.wait(timeout=5)
            status = {'timed_out': True, 'guest_may_be_running': True}
        self.process.stdout.close()
        self.stdout.close()
        self.stderr.close()
        (self.output / 'probe.status.json').write_text(json.dumps(status, indent=2) + '\n')
        return status


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--tart', type=Path, required=True)
    parser.add_argument('--vm', required=True)
    parser.add_argument('--fixtures', type=Path, required=True)
    parser.add_argument('--guest-share', required=True)
    parser.add_argument('--checker', required=True)
    parser.add_argument('--guest-workdir', required=True)
    parser.add_argument('--build-number', type=int, required=True)
    parser.add_argument('--fault', choices=('stop', 'kill', 'both'), default='both')
    parser.add_argument('--e2fsck', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    if (not args.guest_workdir.startswith('/') or args.guest_workdir == '/' or
            Path(args.checker).name != args.checker or args.build_number <= 0):
        parser.error('Use a new absolute guest directory, checker filename and positive build number')
    if not sys.stdin.isatty():
        parser.error('Run in a terminal for the no-echo VM sudo password prompt')
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    tart = args.tart.resolve()
    fixtures = args.fixtures.resolve()
    root = args.guest_workdir.rstrip('/')
    app = '/Applications/Machlin ext4.app'
    control = app + '/Contents/MacOS/Machlin ext4'
    guest = guest_commands(tart, args.vm, output)
    uid = guest('uid', '/usr/bin/id', '-u').decode().strip()
    assert uid.isdigit() and int(uid) != 0
    guest('os', '/usr/bin/sw_vers')
    guest('kernel', '/usr/bin/uname', '-v')
    assert guest('build', '/usr/libexec/PlistBuddy', '-c', 'Print :CFBundleVersion',
                 app + '/Contents/Info.plist').decode().strip() == str(args.build_number)
    assert json.loads(guest('modules', control, '--control', 'modules'))[0]['enabled'] is True
    assert json.loads(guest('service', control, '--control', 'device-service'))['status'] == 'enabled'
    assert json.loads(guest('endpoints', control, '--control', 'list')) == []
    # Credentials exist only in memory and the sudo stdin pipe. They never enter
    # source, argv, evidence files or the guest checker, which remains unprivileged.
    password = (getpass.getpass('Disposable VM administrator password: ') + '\n').encode()
    guest('privilege', '/usr/bin/sudo', '-S', '-p', '', '--', '/usr/bin/true', stdin=password)
    guest('prepare', '/bin/sh', '-eu', '-c', 'umask 077; test ! -e "$1"; mkdir -p "$1"', 'prepare', root)
    guest('copy-checker', '/bin/cp', args.guest_share.rstrip('/') + '/' + args.checker, root + '/checker')
    guest('checker-mode', '/bin/chmod', '755', root + '/checker')
    checker_hash = hashlib.sha256((fixtures / args.checker).read_bytes()).hexdigest()
    assert guest('checker-hash', '/usr/bin/shasum', '-a', '256', root + '/checker').decode().split()[0] == checker_hash

    def signal(label, pid, kind):
        assert pid.isdigit() and kind in ('STOP', 'CONT', 'KILL')
        # The native helper revalidates root ownership and the exact installed
        # executable with libproc. launchd's relative argv[0] is not an identity.
        guest(label, '/usr/bin/sudo', '-S', '-p', '', '--', root + '/checker',
              '--barrier-signal', pid, kind, stdin=password)

    results = {}
    faults = ('stop', 'kill') if args.fault == 'both' else (args.fault,)
    for fault in faults:
        for profile in ('4k', '1k'):
            name = fault + '-' + profile
            evidence = output / name
            evidence.mkdir()
            case = root + '/' + name
            filename = f'ext4-owned-{profile}.img'
            image = case + '/' + filename
            mount = case + '/mount'
            result = {'passed': False, 'checker_sha256': checker_hash}
            results[name] = result
            probe = None
            stopped = None
            try:
                guest(name + '-prepare', '/bin/mkdir', '-p', mount)
                guest(name + '-copy', '/bin/cp', args.guest_share.rstrip('/') + '/' + filename, image)
                before = hashlib.sha256((fixtures / filename).read_bytes()).hexdigest()
                assert guest(name + '-initial-hash', '/usr/bin/shasum', '-a', '256', image).decode().split()[0] == before
                attachment = plistlib.loads(guest(name + '-mount', '/usr/bin/hdiutil', 'attach',
                    '-readwrite', '-owners', 'on', '-nobrowse', '-mountpoint', mount, '-plist',
                    '-imagekey', 'diskimage-class=CRawDiskImage', image))
                entries = [entry for entry in attachment['system-entities'] if 'dev-entry' in entry]
                assert len(entries) == 1 and re.fullmatch(r'/dev/disk[0-9]+', entries[0]['dev-entry'])
                device = entries[0]['dev-entry']
                assert entries[0].get('mount-point') == mount and entries[0].get('volume-kind') == 'machlinext4'
                endpoints = json.loads(guest(name + '-endpoint', control, '--control', 'list'))
                assert len(endpoints) == 1 and endpoints[0]['info']['readOnly'] is False
                probe = Probe(tart, args.vm, root + '/checker', mount, evidence)
                ready = probe.phase('ready')
                identity = guest(name + '-checker-identity', '/bin/ps', '-p', str(ready['pid']),
                                 '-o', 'uid=', '-o', 'comm=').decode().strip().split(None, 1)
                assert identity == [uid, root + '/checker'], identity
                pids = guest(name + '-service-pid', '/usr/bin/pgrep', '-u', '0', '-x',
                             'Ext4DeviceBarrier').decode().split()
                assert len(pids) == 1 and pids[0].isdigit(), pids
                pid = pids[0]
                result['service_pid'] = int(pid)
                if fault == 'stop':
                    # Mark the attempted STOP before RPC: even a timeout may have
                    # delivered it, so finally must attempt an identity-checked CONT.
                    stopped = pid
                signal(name + '-inject', pid, fault.upper())
                if fault == 'stop':
                    assert 'T' in guest(name + '-stopped', '/bin/ps', '-p', pid, '-o', 'stat=').decode()
                started = time.monotonic()
                probe.send('fault')
                result['fault'] = probe.phase('fault')
                result['fault_seconds'] = time.monotonic() - started
                if stopped is not None:
                    signal(name + '-resume', stopped, 'CONT')
                    stopped = None
                assert json.loads(guest(name + '-restored-service', control, '--control',
                                       'device-service'))['status'] == 'enabled'
                probe.send('restored')
                result['after_restore'] = probe.phase('finished')
                result['probe'] = probe.close()
                probe = None
                assert not result['probe'].get('timed_out'), result['probe']
                # An aborted owner cannot perform an ordinary successful final
                # sync. Forced detach here is failure cleanup, not normal-unmount
                # evidence. Recovery and ordinary unmount are tested separately.
                guest(name + '-force-detach', '/usr/bin/hdiutil', 'detach', '-force', device)
                result['forced_detach'] = True
                assert json.loads(guest(name + '-detached', control, '--control', 'list')) == []
                for mode, readonly in (('recover', False), ('verify', True)):
                    guest(name + '-' + mode + '-mount', '/usr/bin/hdiutil', 'attach',
                          '-readonly' if readonly else '-readwrite', '-owners', 'on', '-nobrowse',
                          '-mountpoint', mount, '-plist', '-imagekey', 'diskimage-class=CRawDiskImage', image)
                    devices = image_devices(guest, name + '-' + mode + '-device', image)
                    assert len(devices) == 1
                    endpoints = json.loads(guest(name + '-' + mode + '-endpoint', control, '--control', 'list'))
                    assert len(endpoints) == 1 and endpoints[0]['info']['readOnly'] == readonly
                    guest(name + '-' + mode, root + '/checker', mount, mode, timeout=60)
                    guest(name + '-' + mode + '-detach', '/usr/bin/hdiutil', 'detach', devices[0])
                    assert json.loads(guest(name + '-' + mode + '-cleanup', control, '--control', 'list')) == []
                export = evidence / 'recovered.img'
                guest(name + '-export', '/bin/cat', image, destination=export, timeout=90)
                assert export.stat().st_size == (fixtures / filename).stat().st_size
                after = hashlib.sha256(export.read_bytes()).hexdigest()
                assert before != after
                assert guest(name + '-export-hash', '/usr/bin/shasum', '-a', '256', image).decode().split()[0] == after
                with (evidence / 'fsck.log').open('wb') as log:
                    fsck = subprocess.run([str(args.e2fsck.resolve()), '-fn', str(export)],
                                          stdout=log, stderr=subprocess.STDOUT, timeout=90)
                result.update(fsck_exit=fsck.returncode, original_sha256=before, written_sha256=after)
                assert fsck.returncode == 0
                result['recovery_passed'] = True
                result['passed'] = result['after_restore']['passed'] and result['probe']['exit_code'] == 0
            except GuestTimeout as error:
                result.update(error=str(error), recovery_required=True,
                              cleanup_note='Preserve task devices; the guest operation may still be running')
            except Exception as error:
                result['error'] = f'{type(error).__name__}: {error}'
            finally:
                if stopped is not None:
                    try:
                        signal(name + '-emergency-resume', stopped, 'CONT')
                    except Exception as error:
                        result['service_restore_error'] = str(error)
                if probe is not None:
                    result['probe'] = probe.close()
                if not result.get('recovery_passed'):
                    result['recovery_required'] = True
                (output / 'summary.json').write_text(json.dumps(results, indent=2) + '\n')
                print(json.dumps({name: result}), flush=True)
            if 'error' in result or 'service_restore_error' in result:
                return 1
    return 0 if all(case['passed'] for case in results.values()) else 1


if __name__ == '__main__':
    raise SystemExit(main())

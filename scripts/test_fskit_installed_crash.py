#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
"""Kill a mounted extension after fsync, then verify discovery, remount and disk integrity."""
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
    if (not args.guest_workdir.startswith('/') or args.guest_workdir == '/' or
            Path(args.checker).name != args.checker or args.build_number <= 0):
        parser.error('Use a new absolute guest directory, checker filename and positive build number')
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=False)
    guest = guest_commands(args.tart.resolve(), args.vm, out)
    fixtures = args.fixtures.resolve()
    root = args.guest_workdir.rstrip('/')
    app = '/Applications/Machlin ext4.app'
    executable = app + '/Contents/MacOS/Machlin ext4'
    extension = app + '/Contents/Extensions/Ext4FSKitExtension.appex/Contents/MacOS/Ext4FSKitExtension'
    uid = guest('uid', '/usr/bin/id', '-u').decode().strip()
    assert uid.isdigit() and int(uid) != 0
    guest('os', '/usr/bin/sw_vers')
    guest('kernel', '/usr/bin/uname', '-v')
    assert guest('build', '/usr/libexec/PlistBuddy', '-c', 'Print :CFBundleVersion',
                 app + '/Contents/Info.plist').decode().strip() == str(args.build_number)
    assert json.loads(guest('modules', executable, '--control', 'modules'))[0]['enabled'] is True
    assert json.loads(guest('endpoints', executable, '--control', 'list')) == []
    guest('prepare', '/bin/sh', '-eu', '-c', 'umask 077; test ! -e "$1"; mkdir -p "$1"', 'prepare', root)
    guest('copy-checker', '/bin/cp', args.guest_share.rstrip('/') + '/' + args.checker, root + '/checker')
    guest('checker-mode', '/bin/chmod', '755', root + '/checker')
    digest = hashlib.sha256((fixtures / args.checker).read_bytes()).hexdigest()
    assert guest('checker-hash', '/usr/bin/shasum', '-a', '256', root + '/checker').decode().split()[0] == digest
    results = {}
    for profile in ('4k', '1k'):
        filename = f'ext4-owned-{profile}.img'
        image = root + '/' + filename
        mount = root + '/mount-' + profile
        result = {'passed': False, 'checker_sha256': digest}
        results[profile] = result
        try:
            guest(profile + '-copy', '/bin/cp', args.guest_share.rstrip('/') + '/' + filename, image)
            before = hashlib.sha256((fixtures / filename).read_bytes()).hexdigest()
            assert guest(profile + '-initial-hash', '/usr/bin/shasum', '-a', '256', image).decode().split()[0] == before
            guest(profile + '-mount-directory', '/bin/mkdir', mount)
            attachment = plistlib.loads(guest(profile + '-mount', '/usr/bin/hdiutil', 'attach',
                '-readwrite', '-owners', 'on', '-nobrowse', '-mountpoint', mount, '-plist',
                '-imagekey', 'diskimage-class=CRawDiskImage', image))
            entries = [entry for entry in attachment['system-entities'] if 'dev-entry' in entry]
            assert len(entries) == 1 and re.fullmatch(r'/dev/disk[0-9]+', entries[0]['dev-entry'])
            device = entries[0]['dev-entry']
            assert entries[0].get('mount-point') == mount and entries[0].get('volume-kind') == 'machlinext4'
            endpoints = json.loads(guest(profile + '-live-endpoint', executable, '--control', 'list'))
            assert len(endpoints) == 1 and endpoints[0]['info']['readOnly'] == 0, endpoints
            guest(profile + '-durable-write', root + '/checker', mount, 'write', timeout=180)

            # FSKit may retain idle extension processes. Resolve the active
            # instance by its unique listening socket, without reading the
            # capability manifest or killing a cached/system-wide agent.
            candidates = guest(profile + '-extension-pid', '/usr/bin/pgrep', '-u', uid,
                               '-x', 'Ext4FSKitExtension').decode().split()
            assert candidates and all(pid.isdigit() for pid in candidates), candidates
            sockets = guest(profile + '-extension-sockets', '/usr/sbin/lsof', '-nP', '-a',
                            '-p', ','.join(candidates), '-U', '-Fpn').decode().splitlines()
            endpoint = Path(endpoints[0]['endpoint']).stem
            owners = set()
            pid = None
            for field in sockets:
                if field.startswith('p'):
                    pid = field[1:]
                elif field.startswith('n') and Path(field[1:]).name == endpoint:
                    assert pid in candidates
                    owners.add(pid)
            assert len(owners) == 1, f'Endpoint socket owners: {sorted(owners)}'
            pid = owners.pop()
            process = guest(profile + '-extension-identity', '/bin/ps', '-p', pid,
                            '-o', 'uid=', '-o', 'comm=').decode().strip().split(None, 1)
            assert process == [uid, extension], process
            guest(profile + '-kill-extension', '/bin/kill', '-KILL', pid)
            result['crashed_process'] = int(pid)
            assert json.loads(guest(profile + '-crash-discovery', executable, '--control', 'list')) == []

            # An intentionally dead filesystem is forced off the disposable
            # image. This is crash cleanup, not evidence of normal unmount.
            guest(profile + '-force-detach', '/usr/bin/hdiutil', 'detach', '-force', device)
            result['forced_detach'] = True
            guest(profile + '-remount', '/usr/bin/hdiutil', 'attach', '-readonly', '-owners', 'on',
                  '-nobrowse', '-mountpoint', mount, '-plist', '-imagekey', 'diskimage-class=CRawDiskImage', image)
            devices = image_devices(guest, profile + '-remount-device', image)
            assert len(devices) == 1
            endpoints = json.loads(guest(profile + '-remounted-endpoint', executable, '--control', 'list'))
            assert len(endpoints) == 1 and endpoints[0]['info']['readOnly'] == 1, endpoints
            guest(profile + '-persistence', root + '/checker', mount, 'verify', timeout=180)
            guest(profile + '-detach', '/usr/bin/hdiutil', 'detach', devices[0])
            assert json.loads(guest(profile + '-cleanup', executable, '--control', 'list')) == []
            service = json.loads(guest(profile + '-service', executable, '--control', 'device-service'))
            assert service['status'] == 'enabled'
            export = out / ('crashed-' + profile + '.img')
            guest(profile + '-export', '/bin/cat', image, destination=export, timeout=90)
            assert export.stat().st_size == (fixtures / filename).stat().st_size
            after = hashlib.sha256(export.read_bytes()).hexdigest()
            assert after != before
            assert guest(profile + '-export-hash', '/usr/bin/shasum', '-a', '256', image).decode().split()[0] == after
            with (out / (profile + '-fsck.log')).open('wb') as log:
                fsck = subprocess.run([str(args.e2fsck.resolve()), '-fn', str(export)], stdout=log,
                                      stderr=subprocess.STDOUT, timeout=90)
            result.update(fsck_exit=fsck.returncode, original_sha256=before, written_sha256=after)
            assert fsck.returncode == 0
            result['passed'] = True
        except GuestTimeout as error:
            result.update(error=str(error), recovery_required=True,
                          cleanup_note='Preserve the device for diagnosis; the guest command may still be running')
        except Exception as error:
            result['error'] = f'{type(error).__name__}: {error}'
        finally:
            # Do not automatically issue another detach after the deliberate
            # crash or a timed-out operation. The main agent diagnoses that state.
            if not result['passed']:
                result['recovery_required'] = True
            (out / 'summary.json').write_text(json.dumps(results, indent=2) + '\n')
            print(json.dumps({profile: result}), flush=True)
        if not result['passed']:
            break
    return 0 if len(results) == 2 and all(value['passed'] for value in results.values()) else 1


if __name__ == '__main__':
    raise SystemExit(main())

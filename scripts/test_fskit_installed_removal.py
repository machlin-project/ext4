#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
"""Force-detach task-owned images with retained descriptors and mmap, then verify durable bytes."""
import argparse
import hashlib
import json
import plistlib
import re
import subprocess
from pathlib import Path
from fskit_test_vm import GuestTimeout, guest_commands, image_devices
from test_fskit_installed_barrier import Probe


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--tart', type=Path, required=True)
    parser.add_argument('--vm', required=True)
    parser.add_argument('--fixtures', type=Path, required=True)
    parser.add_argument('--checker', type=Path, required=True)
    parser.add_argument('--guest-workdir', required=True)
    parser.add_argument('--build-number', type=int, required=True)
    parser.add_argument('--e2fsck', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    if (not args.guest_workdir.startswith('/') or args.guest_workdir == '/' or
            args.build_number <= 0):
        parser.error('Use a new absolute guest directory and positive build number')
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    root = args.guest_workdir.rstrip('/')
    tart = args.tart.resolve()
    checker = args.checker.resolve()
    checker_hash = digest(checker)
    control = '/Applications/Machlin ext4.app/Contents/MacOS/Machlin ext4'
    guest = guest_commands(tart, args.vm, output)
    uid = guest('uid', '/usr/bin/id', '-u').decode().strip()
    assert uid.isdigit() and int(uid) != 0
    guest('os', '/usr/bin/sw_vers')
    guest('kernel', '/usr/bin/uname', '-v')
    assert guest('build', '/usr/libexec/PlistBuddy', '-c', 'Print :CFBundleVersion',
                 '/Applications/Machlin ext4.app/Contents/Info.plist').decode().strip() == str(args.build_number)
    assert json.loads(guest('modules', control, '--control', 'modules'))[0]['enabled'] is True
    assert json.loads(guest('service', control, '--control', 'device-service'))['status'] == 'enabled'
    assert json.loads(guest('endpoints', control, '--control', 'list')) == []
    guest('prepare', '/bin/sh', '-eu', '-c',
          'umask 077; test ! -e "$1"; mkdir -p "$1"', 'prepare', root)
    # Send bounded fixture bytes through the established CLI transport. No
    # dependency on a guest GUI, shared-folder registration or root credentials.
    guest('copy-checker', '/bin/sh', '-eu', '-c',
          'umask 077; cat > "$1"; chmod 700 "$1"', 'copy', root + '/checker',
          stdin=checker.read_bytes())
    assert guest('checker-hash', '/usr/bin/shasum', '-a', '256', root + '/checker').decode().split()[0] == checker_hash
    results = {}
    for profile in ('4k', '1k'):
        original = args.fixtures.resolve() / f'ext4-owned-{profile}.img'
        original_hash = digest(original)
        for writable in (False, True):
            name = profile + ('-readwrite' if writable else '-readonly')
            case = root + '/' + name
            image = case + '/image.img'
            mount = case + '/mount'
            evidence = output / name
            evidence.mkdir()
            result = {'passed': False, 'checker_sha256': checker_hash,
                      'original_sha256': original_hash,
                      'fault_kind': 'forced_image_detach', 'physical_power_loss': False}
            results[name] = result
            probe = None
            try:
                guest(name + '-prepare', '/bin/mkdir', '-p', mount)
                guest(name + '-copy', '/bin/sh', '-eu', '-c',
                      'umask 077; cat > "$1"', 'copy', image, stdin=original.read_bytes(), timeout=90)
                assert guest(name + '-initial-hash', '/usr/bin/shasum', '-a', '256', image).decode().split()[0] == original_hash
                attachment = plistlib.loads(guest(name + '-mount', '/usr/bin/hdiutil', 'attach',
                    '-readwrite' if writable else '-readonly', '-owners', 'on', '-nobrowse',
                    '-mountpoint', mount, '-plist', '-imagekey', 'diskimage-class=CRawDiskImage', image))
                entries = [entry for entry in attachment['system-entities'] if 'dev-entry' in entry]
                assert len(entries) == 1 and re.fullmatch(r'/dev/disk[0-9]+', entries[0]['dev-entry'])
                device = entries[0]['dev-entry']
                assert entries[0].get('mount-point') == mount and entries[0].get('volume-kind') == 'machlinext4'
                endpoints = json.loads(guest(name + '-endpoint', control, '--control', 'list'))
                assert len(endpoints) == 1 and endpoints[0]['info']['readOnly'] == (not writable)
                probe = Probe(tart, args.vm, root + '/checker', mount, evidence,
                              'remove-readwrite' if writable else 'remove-readonly')
                ready = probe.phase('ready')
                identity = guest(name + '-checker-identity', '/bin/ps', '-p', str(ready['pid']),
                                 '-o', 'uid=', '-o', 'comm=').decode().strip().split(None, 1)
                assert identity == [uid, root + '/checker'], identity
                # Revalidate ownership immediately before deliberately forcing
                # removal. Force is the injected fault, never a cleanup fallback.
                assert image_devices(guest, name + '-owned-device', image) == [device]
                guest(name + '-inject-removal', '/usr/bin/hdiutil', 'detach', '-force', device, timeout=45)
                result['forced_detach'] = True
                assert image_devices(guest, name + '-removed-device', image) == []
                assert json.loads(guest(name + '-removed-endpoint', control, '--control', 'list')) == []
                probe.send('removed')
                result['after_removal'] = probe.phase('finished')
                result['probe'] = probe.close()
                probe = None
                assert not result['probe'].get('timed_out'), result['probe']
                # Recovery may be necessary after abrupt device loss. This fault
                # uses a forced unmount, whose final sync should leave a clean
                # image and preserve the previously acknowledged data directly.
                attachment = plistlib.loads(guest(name + '-remount', '/usr/bin/hdiutil', 'attach',
                    '-readonly', '-owners', 'on', '-nobrowse', '-mountpoint', mount, '-plist',
                    '-imagekey', 'diskimage-class=CRawDiskImage', image))
                entries = [entry for entry in attachment['system-entities'] if 'dev-entry' in entry]
                assert len(entries) == 1 and entries[0].get('mount-point') == mount
                assert entries[0].get('volume-kind') == 'machlinext4'
                devices = image_devices(guest, name + '-remounted-device', image)
                assert devices == [entries[0]['dev-entry']]
                endpoints = json.loads(guest(name + '-remounted-endpoint', control, '--control', 'list'))
                assert len(endpoints) == 1 and endpoints[0]['info']['readOnly'] is True
                guest(name + '-verify', root + '/checker', mount,
                      'verify-written' if writable else 'verify-readonly', timeout=45)
                guest(name + '-normal-detach', '/usr/bin/hdiutil', 'detach', devices[0])
                assert image_devices(guest, name + '-final-devices', image) == []
                assert json.loads(guest(name + '-final-endpoints', control, '--control', 'list')) == []
                export = evidence / 'removed.img'
                guest(name + '-export', '/bin/cat', image, destination=export, timeout=90)
                assert export.stat().st_size == original.stat().st_size
                after = digest(export)
                assert guest(name + '-export-hash', '/usr/bin/shasum', '-a', '256', image).decode().split()[0] == after
                assert (after != original_hash) == writable
                with (evidence / 'fsck.log').open('wb') as log:
                    fsck = subprocess.run([str(args.e2fsck.resolve()), '-fn', str(export)],
                                          stdout=log, stderr=subprocess.STDOUT, timeout=90)
                result.update(fsck_exit=fsck.returncode, exported_sha256=after)
                assert fsck.returncode == 0
                assert digest(original) == original_hash
                result['cleanup_complete'] = True
                result['passed'] = result['after_removal']['passed'] and result['probe']['exit_code'] == 0
            except GuestTimeout as error:
                result.update(error=str(error), recovery_required=True,
                              cleanup_note='Preserve task devices; the guest operation may still be running')
            except Exception as error:
                result['error'] = f'{type(error).__name__}: {error}'
            finally:
                if probe is not None:
                    result['probe'] = probe.close()
                if not result.get('cleanup_complete'):
                    result['recovery_required'] = True
                (output / 'summary.json').write_text(json.dumps(results, indent=2) + '\n')
                print(json.dumps({name: result}), flush=True)
            if 'error' in result or result.get('recovery_required'):
                return 1
    return 0 if all(case['passed'] for case in results.values()) else 1


if __name__ == '__main__':
    raise SystemExit(main())

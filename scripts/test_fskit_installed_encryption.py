#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
"""Check installed FSKit encryption through a dedicated Tart VM, never on the host."""
import argparse
import hashlib
import json
import plistlib
import re
import subprocess
import sys
from pathlib import Path

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--tart', type=Path, required=True, help='Tart CLI or the lab scripts/tart.sh wrapper')
parser.add_argument('--vm', required=True, help='Dedicated, running test VM with its RPC agent')
parser.add_argument('--fixtures', type=Path, required=True, help='Host directory containing staged fixtures and checker')
parser.add_argument('--guest-share', required=True, help='The same fixture directory as exposed in the VM')
parser.add_argument('--guest-workdir', required=True, help='New absolute guest directory for this run')
parser.add_argument('--build-number', type=int, required=True)
parser.add_argument('--output', type=Path, required=True, help='New host evidence directory')
args = parser.parse_args()
if args.build_number < 1 or not args.guest_workdir.startswith('/') or args.guest_workdir == '/':
    parser.error('Use a positive build number and a new absolute guest work directory')

LAB = Path.cwd()
TART = args.tart.resolve()
VM = args.vm
STAGED = args.fixtures.resolve()
SHARE = args.guest_share.rstrip('/')
ROOT = args.guest_workdir.rstrip('/')
APP = '/Applications/Machlin ext4.app/Contents/MacOS/Machlin ext4'
KEY = ROOT + '/fixture.key'
OUT = args.output.resolve()
OUT.mkdir(parents=True, exist_ok=False)
serial = 0


def guest(label, *command, required=True, timeout=30):
    global serial
    serial += 1
    prefix = OUT / f'{serial:03d}-{label}'
    argv = [str(TART), 'exec', VM, *command]
    try:
        result = subprocess.run(argv, cwd=LAB, capture_output=True, timeout=timeout)
    except subprocess.TimeoutExpired as error:
        prefix.with_suffix('.stdout.log').write_bytes(error.stdout or b'')
        prefix.with_suffix('.stderr.log').write_bytes(error.stderr or b'')
        prefix.with_suffix('.status.json').write_text(json.dumps(
            {'argv': argv, 'timed_out': True, 'timeout': timeout}, indent=2) + '\n')
        raise RuntimeError(f'{label} timed out; inspect task process before retry')
    prefix.with_suffix('.stdout.log').write_bytes(result.stdout)
    prefix.with_suffix('.stderr.log').write_bytes(result.stderr)
    prefix.with_suffix('.status.json').write_text(json.dumps(
        {'argv': argv, 'exit_code': result.returncode}, indent=2) + '\n')
    if required and result.returncode:
        raise RuntimeError(f'{label}: exit {result.returncode}')
    return result


def control(label, *arguments):
    return json.loads(guest(label, APP, '--control', *arguments).stdout)


def mount(device, directory, label):
    guest(label, '/sbin/mount', '-F', '-t', 'machlin_ext4', '-o', 'rdonly', device, directory)
    volumes = control(label + '-control', 'list')
    assert len(volumes) == 1, f'{label}: expected one active endpoint'
    value = volumes[0]
    assert value['info']['mounted'] is True and value['info']['readOnly'] is True
    return value


def unmount(directory, label):
    guest(label, '/sbin/umount', directory)
    assert control(label + '-endpoints', 'list') == [], f'{label}: endpoint leaked'


def denied(directory, label):
    result = guest(label, '/bin/dd', f'if={directory}/plain/block-out', 'of=/dev/null',
                   'bs=1', 'count=1', required=False)
    assert result.returncode != 0 and not result.stdout
    assert b'Permission denied' in result.stderr, f'{label}: wrong read failure'


results = {}
assert int(guest('uid', '/usr/bin/id', '-u').stdout.strip()) != 0, 'Use an ordinary test user'
guest('os-version', '/usr/bin/sw_vers')
guest('loaded-kernel', '/usr/bin/uname', '-v')
guest('sip', '/usr/bin/csrutil', 'status')
guest('prepare-workdir', '/bin/sh', '-eu', '-c',
      'umask 077; test ! -e "$1"; mkdir -p "$1"', 'prepare-fixtures', ROOT)
assert guest('build', '/usr/libexec/PlistBuddy', '-c', 'Print :CFBundleVersion',
             '/Applications/Machlin ext4.app/Contents/Info.plist').stdout.decode().strip() == str(args.build_number)
modules = control('modules', 'modules')
assert len(modules) == 1 and modules[0]['enabled'] is True

assert control('initial-endpoint-state', 'list') == []
results['initial-endpoint-state'] = {'passed': True}

for profile in ('4k', '1k'):
    directory = ROOT + '/mount-' + profile
    image = f'encrypted-ext4-{profile}.img'
    manifest = f'encrypted-ext4-{profile}.manifest'
    checker = 'ext4-mounted-manifest-test'
    device = None
    mounted = False
    volume_id = None
    imported = None
    try:
        guest(profile + '-stage', '/bin/sh', '-eu', '-c', '''
umask 077
test ! -e "$1"
mkdir "$1"
for name in "$4" "$5" "$6"; do cp "$2/$name" "$3/$name"; done
chmod 755 "$3/$6"
''', 'stage-encrypted', directory, SHARE, ROOT, image, manifest, checker)
        names = (image, manifest, checker)
        hashes = guest(profile + '-hashes', '/usr/bin/shasum', '-a', '256',
                       *(ROOT + '/' + name for name in names)).stdout.decode().splitlines()
        expected = [hashlib.sha256((STAGED / name).read_bytes()).hexdigest() for name in names]
        assert [line.split()[0] for line in hashes] == expected
        attached = plistlib.loads(guest(profile + '-attach', '/usr/bin/hdiutil', 'attach',
                                        '-readonly', '-nomount', '-plist', '-imagekey',
                                        'diskimage-class=CRawDiskImage', ROOT + '/' + image).stdout)
        devices = [entry['dev-entry'] for entry in attached['system-entities'] if 'dev-entry' in entry]
        assert len(devices) == 1 and re.fullmatch(r'/dev/disk[0-9]+', devices[0])
        device = devices[0]
        info = plistlib.loads(guest(profile + '-device', '/usr/sbin/diskutil', 'info',
                                   '-plist', device).stdout)
        assert info['Writable'] is False and info['TotalSize'] == (STAGED / image).stat().st_size
        initial = mount(device, directory, profile + '-mount-no-key')
        mounted = True
        volume_id = initial['info']['volume']
        assert initial['info']['keyStoreAvailable'] and initial['info']['loadedKeys'] == 0
        assert initial['info']['blockSize'] == (4096 if profile == '4k' else 1024)
        assert control(profile + '-keys-empty', 'keys', volume_id) == {'keys': []}
        # A permissions failure must not masquerade as missing-key enforcement.
        guest(profile + '-plain-read', '/bin/dd', f'if={directory}/plain/visible',
              'of=/dev/null', 'bs=8192')
        guest(profile + '-encrypted-metadata', '/usr/bin/stat', '-f', '%HT %z',
              directory + '/plain/block-out')
        denied(directory, profile + '-missing-key-read')

        guest(profile + '-stage-key', '/bin/sh', '-eu', '-c', '''
umask 077
test ! -e "$2"
mkdir -p "${2%/*}"
cp "$1" "$2"
chmod 600 "$2"
''', 'stage-key', SHARE + '/fscrypt-fixture-v2.key', KEY)
        added = json.loads(guest(profile + '-import', '/bin/sh', '-eu', '-c',
                                 'exec "$1" --control import-key "$2" - < "$3"',
                                 'import-fixture-key', APP, volume_id, KEY).stdout)
        imported = added['identifier']
        assert re.fullmatch(r'v2:[0-9a-f]{32}', imported) and added['appliesOnNextMount'] is True
        guest(profile + '-remove-staged-key', '/bin/rm', KEY)
        assert control(profile + '-keys-imported', 'keys', volume_id) == {'keys': [imported]}
        assert control(profile + '-current-after-import', 'request', initial['endpoint'],
                       'getInfo')['loadedKeys'] == 0
        denied(directory, profile + '-import-does-not-mutate-mount')
        unmount(directory, profile + '-unmount-before-key')
        mounted = False
        keyed = mount(device, directory, profile + '-mount-keyed')
        mounted = True
        assert keyed['endpoint'] != initial['endpoint']
        assert keyed['info']['volume'] == volume_id
        assert keyed['info']['loadedKeys'] == 1 and keyed['info']['keyStoreAvailable']
        guest(profile + '-encrypted-manifest', ROOT + '/' + checker, directory,
              ROOT + '/' + manifest, timeout=90)
        assert control(profile + '-remove-saved-key', 'remove-key', volume_id, imported) == {
            'removed': True, 'appliesOnNextMount': True}
        imported = None
        assert control(profile + '-keys-removed', 'keys', volume_id) == {'keys': []}
        assert control(profile + '-current-after-removal', 'request', keyed['endpoint'],
                       'getInfo')['loadedKeys'] == 1
        guest(profile + '-mounted-key-retained', ROOT + '/' + checker, directory,
              ROOT + '/' + manifest, timeout=90)
        unmount(directory, profile + '-unmount-after-removal')
        mounted = False
        removed = mount(device, directory, profile + '-mount-key-removed')
        mounted = True
        assert removed['info']['loadedKeys'] == 0 and removed['info']['keyStoreAvailable']
        denied(directory, profile + '-removed-key-read')
        results[profile] = {'passed': True, 'image_sha256': expected[0],
                            'manifest_sha256': expected[1], 'checker_sha256': expected[2],
                            'manifest_entries': 36,
                            'key_import_next_mount': True, 'key_removal_next_mount': True}
    except Exception as error:
        results[profile] = {'passed': False, 'error': str(error)}
    finally:
        cleanup = []
        for label, operation in (
            ('staged-key', lambda: guest(profile + '-cleanup-keyfile', '/bin/rm', '-f', KEY)),
            ('saved-key', lambda: control(profile + '-cleanup-keychain', 'remove-key', volume_id, imported)
             if imported is not None else None),
            ('unmount', lambda: unmount(directory, profile + '-cleanup-unmount') if mounted else None),
            ('detach', lambda: guest(profile + '-cleanup-detach', '/usr/bin/hdiutil', 'detach', device)
             if device is not None else None),
        ):
            try:
                operation()
            except Exception as error:
                cleanup.append(f'{label}: {error}')
        if cleanup:
            results[profile]['cleanup_errors'] = cleanup
            results[profile]['passed'] = False
        (OUT / 'summary.json').write_text(json.dumps(results, indent=2) + '\n')
        print(json.dumps({profile: results[profile]}), flush=True)
    if not results[profile]['passed']:
        break

guest('fixed-markers', '/usr/bin/log', 'show', '--last', '10m', '--style', 'compact',
      '--predicate', 'eventMessage BEGINSWITH "Machlin ext4 " AND process != "log"', required=False)
print(json.dumps(results, indent=2))
sys.exit(0 if all(value['passed'] for value in results.values()) and '1k' in results else 1)

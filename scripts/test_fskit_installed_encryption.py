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
parser.add_argument('--write-checker', help='Unique staged mounted_write executable; enables writable roundtrip')
parser.add_argument('--e2fsck', type=Path, help='Independent checker, required with --write-checker')
parser.add_argument('--v1-descriptor', help='Public 8-byte synthetic fscrypt v1 descriptor, as 16 hex digits')
parser.add_argument('--key-file', default='fscrypt-fixture-v2.key', help='Synthetic key filename in the fixture directory')
parser.add_argument('--output', type=Path, required=True, help='New host evidence directory')
args = parser.parse_args()
if args.build_number < 1 or not args.guest_workdir.startswith('/') or args.guest_workdir == '/':
    parser.error('Use a positive build number and a new absolute guest work directory')
if bool(args.write_checker) != bool(args.e2fsck):
    parser.error('--write-checker and --e2fsck must be supplied together')
if args.write_checker and Path(args.write_checker).name != args.write_checker:
    parser.error('--write-checker must be a filename within the fixture directory')
if Path(args.key_file).name != args.key_file or (args.v1_descriptor and not re.fullmatch(r'[0-9a-f]{16}', args.v1_descriptor)):
    parser.error('Use a staged key filename and a valid lowercase v1 descriptor')

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


def guest(label, *command, required=True, timeout=30, destination=None):
    global serial
    serial += 1
    prefix = OUT / f'{serial:03d}-{label}'
    argv = [str(TART), 'exec', VM, *command]
    try:
        if destination is None:
            result = subprocess.run(argv, cwd=LAB, capture_output=True, timeout=timeout)
        else:
            with destination.open('wb') as output:
                result = subprocess.run(argv, cwd=LAB, stdout=output, stderr=subprocess.PIPE, timeout=timeout)
    except subprocess.TimeoutExpired as error:
        prefix.with_suffix('.stdout.log').write_bytes(error.stdout or b'')
        prefix.with_suffix('.stderr.log').write_bytes(error.stderr or b'')
        prefix.with_suffix('.status.json').write_text(json.dumps(
            {'argv': argv, 'timed_out': True, 'timeout': timeout}, indent=2) + '\n')
        raise RuntimeError(f'{label} timed out; inspect task process before retry')
    if destination is None:
        prefix.with_suffix('.stdout.log').write_bytes(result.stdout)
    prefix.with_suffix('.stderr.log').write_bytes(result.stderr)
    prefix.with_suffix('.status.json').write_text(json.dumps(
        {'argv': argv, 'exit_code': result.returncode}, indent=2) + '\n')
    if required and result.returncode:
        raise RuntimeError(f'{label}: exit {result.returncode}')
    return result


def control(label, *arguments):
    return json.loads(guest(label, APP, '--control', *arguments).stdout)


class TestImage:
    def __init__(self, name, directory):
        self.path = ROOT + '/' + name
        self.size = (STAGED / name).stat().st_size
        self.directory = directory
        self.device = None

    def mount(self, label, writable=False):
        assert self.device is None, f'{label}: image already attached'
        # Let Disk Arbitration discover and mount together. The encrypted fixtures
        # retain Linux owners; ownership preservation has its own mounted suite.
        attached = plistlib.loads(guest(label, '/usr/bin/hdiutil', 'attach',
                                        '-readwrite' if writable else '-readonly', '-owners', 'off', '-nobrowse',
                                        '-mountpoint', self.directory, '-plist', '-imagekey',
                                        'diskimage-class=CRawDiskImage', self.path).stdout)
        entities = [entry for entry in attached['system-entities'] if 'dev-entry' in entry]
        assert len(entities) == 1 and re.fullmatch(r'/dev/disk[0-9]+', entities[0]['dev-entry'])
        # Record ownership before checking the mount so failure cleanup can detach it.
        self.device = entities[0]['dev-entry']
        assert entities[0].get('mount-point') == self.directory
        assert entities[0].get('volume-kind') == 'machlinext4'
        info = plistlib.loads(guest(label + '-disk-arbitration', '/usr/sbin/diskutil',
                                   'info', '-plist', self.device).stdout)
        assert info.get('MountPoint') == self.directory, f'{label}: Disk Arbitration lost the mount point'
        assert info['Writable'] == writable and info['TotalSize'] == self.size
        assert info['GlobalPermissionsEnabled'] is False, f'{label}: unexpected ownership policy'
        volumes = control(label + '-control', 'list')
        assert len(volumes) == 1, f'{label}: expected one active endpoint'
        value = volumes[0]
        assert value['info']['mounted'] is True and value['info']['readOnly'] == (not writable)
        return value

    def unmount(self, label):
        if self.device is not None:
            guest(label, '/usr/bin/hdiutil', 'detach', self.device)
            self.device = None
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
if args.write_checker:
    assert control('device-service', 'device-service')['status'] == 'enabled'
    guest('copy-write-checker', '/bin/cp', SHARE + '/' + args.write_checker, ROOT + '/write-checker')
    guest('write-checker-mode', '/bin/chmod', '755', ROOT + '/write-checker')
    writer_hash = hashlib.sha256((STAGED / args.write_checker).read_bytes()).hexdigest()
    assert guest('write-checker-hash', '/usr/bin/shasum', '-a', '256', ROOT + '/write-checker').stdout.decode().split()[0] == writer_hash

for profile in ('4k', '1k'):
    directory = ROOT + '/mount-' + profile
    image = f'encrypted-ext4-{profile}.img'
    manifest = f'encrypted-ext4-{profile}.manifest'
    checker = 'ext4-mounted-manifest-test'
    resource = TestImage(image, directory)
    volume_id = None
    imported = None
    special_files = None
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
        initial = resource.mount(profile + '-mount-no-key')
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
''', 'stage-key', SHARE + '/' + args.key_file, KEY)
        added = json.loads(guest(profile + '-import', '/bin/sh', '-eu', '-c',
                                 'exec "$1" --control import-key "$2" - "$4" < "$3"',
                                 'import-fixture-key', APP, volume_id, KEY, args.v1_descriptor or '').stdout)
        imported = added['identifier']
        assert (imported == 'v1:' + args.v1_descriptor if args.v1_descriptor else re.fullmatch(r'v2:[0-9a-f]{32}', imported))
        assert added['appliesOnNextMount'] is True
        guest(profile + '-remove-staged-key', '/bin/rm', KEY)
        assert control(profile + '-keys-imported', 'keys', volume_id) == {'keys': [imported]}
        assert control(profile + '-current-after-import', 'request', initial['endpoint'],
                       'getInfo')['loadedKeys'] == 0
        denied(directory, profile + '-import-does-not-mutate-mount')
        resource.unmount(profile + '-unmount-before-key')
        keyed = resource.mount(profile + '-mount-keyed', writable=bool(args.write_checker))
        assert keyed['endpoint'] != initial['endpoint']
        assert keyed['info']['volume'] == volume_id
        assert keyed['info']['loadedKeys'] == 1 and keyed['info']['keyStoreAvailable']
        guest(profile + '-encrypted-manifest', ROOT + '/' + checker, directory,
              ROOT + '/' + manifest, timeout=90)
        if args.write_checker:
            guest(profile + '-encrypted-write', ROOT + '/write-checker', directory + '/vault', 'write', timeout=180)
            special_files = json.loads(guest(profile + '-special-files', ROOT + '/write-checker',
                                              directory + '/vault', 'special').stdout)
            resource.unmount(profile + '-unmount-written')
            keyed = resource.mount(profile + '-remount-written')
            assert keyed['info']['loadedKeys'] == 1
            guest(profile + '-verify-written', ROOT + '/write-checker', directory + '/vault', 'verify', timeout=90)
            guest(profile + '-original-content-preserved', ROOT + '/' + checker, directory,
                  ROOT + '/' + manifest, timeout=90)
        assert control(profile + '-remove-saved-key', 'remove-key', volume_id, imported) == {
            'removed': True, 'appliesOnNextMount': True}
        imported = None
        assert control(profile + '-keys-removed', 'keys', volume_id) == {'keys': []}
        assert control(profile + '-current-after-removal', 'request', keyed['endpoint'],
                       'getInfo')['loadedKeys'] == 1
        guest(profile + '-mounted-key-retained', ROOT + '/' + checker, directory,
              ROOT + '/' + manifest, timeout=90)
        resource.unmount(profile + '-unmount-after-removal')
        removed = resource.mount(profile + '-mount-key-removed')
        assert removed['info']['loadedKeys'] == 0 and removed['info']['keyStoreAvailable']
        denied(directory, profile + '-removed-key-read')
        if args.write_checker:
            denied_write = guest(profile + '-removed-key-created-file', '/bin/cat',
                                  directory + '/vault/acceptance-write/payload', required=False)
            assert denied_write.returncode != 0 and not denied_write.stdout
        results[profile] = {'passed': True, 'image_sha256': expected[0],
                            'manifest_sha256': expected[1], 'checker_sha256': expected[2],
                            'manifest_entries': 36,
                            'mount_method': 'hdiutil Disk Arbitration automount',
                            'ownership': 'ignored for encryption fixtures',
                            'key_import_next_mount': True, 'key_removal_next_mount': True}
        results[profile]['policy_version'] = 1 if args.v1_descriptor else 2
        if special_files is not None:
            results[profile]['special_files'] = special_files
    except Exception as error:
        results[profile] = {'passed': False, 'error': str(error)}
    finally:
        cleanup = []
        for label, operation in (
            ('staged-key', lambda: guest(profile + '-cleanup-keyfile', '/bin/rm', '-f', KEY)),
            ('saved-key', lambda: control(profile + '-cleanup-keychain', 'remove-key', volume_id, imported)
             if imported is not None else None),
            ('detach', lambda: resource.unmount(profile + '-cleanup-detach')),
        ):
            try:
                operation()
            except Exception as error:
                cleanup.append(f'{label}: {error}')
        if cleanup:
            results[profile]['cleanup_errors'] = cleanup
            results[profile]['passed'] = False
        if results[profile]['passed']:
            try:
                digest = guest(profile + '-final-image-hash', '/usr/bin/shasum', '-a', '256',
                               resource.path).stdout.decode().split()[0]
                if args.write_checker:
                    export = OUT / ('encrypted-written-' + profile + '.img')
                    guest(profile + '-export', '/bin/cat', resource.path, destination=export, timeout=90)
                    assert export.stat().st_size == resource.size
                    assert hashlib.sha256(export.read_bytes()).hexdigest() == digest
                    assert digest != expected[0], 'writable image did not change'
                    with (OUT / (profile + '-fsck.log')).open('wb') as log:
                        checked = subprocess.run([str(args.e2fsck.resolve()), '-fn', str(export)],
                                                 stdout=log, stderr=subprocess.STDOUT, timeout=90)
                    assert checked.returncode == 0, 'independent fsck failed'
                    results[profile].update(written_sha256=digest, writer_sha256=writer_hash,
                                            fsck_exit=checked.returncode, write_remount=True)
                else:
                    assert digest == expected[0], 'read-only fixture changed'
                    results[profile]['image_unchanged'] = True
            except Exception as error:
                results[profile].update(passed=False, error=str(error))
        (OUT / 'summary.json').write_text(json.dumps(results, indent=2) + '\n')
        print(json.dumps({profile: results[profile]}), flush=True)
    if not results[profile]['passed']:
        break

guest('fixed-markers', '/usr/bin/log', 'show', '--last', '10m', '--style', 'compact',
      '--predicate', 'eventMessage BEGINSWITH "Machlin ext4 " AND process != "log"', required=False)
print(json.dumps(results, indent=2))
sys.exit(0 if all(value['passed'] for value in results.values()) and '1k' in results else 1)

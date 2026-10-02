#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
"""Test installer refusal during a mount and fixed-path installation in a VM."""

import argparse
import getpass
import hashlib
import json
from pathlib import Path
import plistlib
import re
import sys

from fskit_test_vm import GuestTimeout, guest_commands, image_devices


APP = '/Applications/Machlin ext4.app'
CATALOG = '/Library/Filesystems/machlinext4.fs'
EXECUTABLES = (
    'Contents/MacOS/Machlin ext4',
    'Contents/Extensions/Ext4FSKitExtension.appex/Contents/MacOS/Ext4FSKitExtension',
    'Contents/Extensions/Ext4FSKitExtension.appex/Contents/Helpers/Ext4CheckResource',
    'Contents/Helpers/Ext4DeviceSetup.app/Contents/MacOS/Ext4DeviceSetup',
    'Contents/Helpers/Ext4DeviceSetup.app/Contents/Library/LaunchServices/Ext4DeviceBarrier',
)


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--tart', type=Path, required=True)
    parser.add_argument('--vm', required=True)
    parser.add_argument('--package', type=Path, required=True)
    parser.add_argument('--guest-package', required=True, help='Shared package path in the guest')
    parser.add_argument('--image', type=Path, required=True)
    parser.add_argument('--guest-image', required=True, help='Shared clean image path in the guest')
    parser.add_argument('--source-package', type=Path, required=True)
    parser.add_argument('--build-number', type=int, required=True)
    parser.add_argument('--previous-build-number', type=int,
                        help='Test an actual update from this installed bundle version')
    parser.add_argument('--expected-app', type=Path,
                        help='Exported app whose executable bytes the package must install')
    parser.add_argument('--guest-workdir', required=True)
    parser.add_argument('--allow-untrusted', action='store_true',
                        help='Permit an explicitly unsigned installer in this isolated VM')
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    if not sys.stdin.isatty():
        parser.error('Use a terminal for the no-echo dedicated VM password prompt')
    if args.build_number < 1 or not args.guest_workdir.startswith('/') or args.guest_workdir == '/':
        parser.error('Use a positive build number and a new absolute guest directory')
    previous_build = args.build_number if args.previous_build_number is None else args.previous_build_number
    if previous_build < 1 or (previous_build != args.build_number and args.expected_app is None):
        parser.error('An update requires a positive previous build and its exported replacement app')
    expected = None
    if args.expected_app is not None:
        expected_app = args.expected_app.resolve()
        info = plistlib.loads((expected_app / 'Contents/Info.plist').read_bytes())
        if info.get('CFBundleIdentifier') != 'org.machlin.ext4' or str(info.get('CFBundleVersion')) != str(args.build_number):
            parser.error('The expected app must match the requested identity and bundle version')
        expected = ''.join(digest(expected_app / path) + '  ' + APP + '/' + path + '\n'
                           for path in EXECUTABLES)
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    guest = guest_commands(args.tart.resolve(), args.vm, output)
    password = (getpass.getpass('Dedicated VM administrator password: ') + '\n').encode()
    root = args.guest_workdir.rstrip('/')
    image = root + '/clean.img'
    package = root + '/driver.pkg'
    mount = root + '/mount'
    current_image = None
    result = {'passed': False, 'untrusted_installer_allowed': args.allow_untrusted,
              'previous_bundle_version': previous_build, 'bundle_version': args.build_number}

    def control(label, *command):
        return json.loads(guest(label, APP + '/Contents/MacOS/Machlin ext4', '--control', *command))

    def hashes(label):
        # Older app versions may not contain the independent checker. Keep its
        # absence explicit so refusal must preserve both files and missing files.
        return guest(label, '/bin/sh', '-eu', '-c',
                     'for executable; do if test -f "$executable"; then '
                     '/usr/bin/shasum -a 256 "$executable"; else '
                     'printf "MISSING %s\\n" "$executable"; fi; done', 'hash-executables',
                     *[APP + '/' + path for path in EXECUTABLES]).decode()

    def installer(label):
        # PackageKit sends script diagnostics to install.log, even when
        # installer -dumplog omits them. Capture only this invocation's suffix;
        # an old refusal must never make a different installer failure pass.
        log_path = '/var/log/install.log'
        before_log = int(guest(label + '-log-size-before', '/usr/bin/stat', '-f', '%z', log_path))
        command = ['/usr/sbin/installer', '-verboseR', '-dumplog', '-pkg', package, '-target', '/']
        if args.allow_untrusted:
            command.append('-allowUntrusted')
        guest(label, '/usr/bin/sudo', '-S', '-p', '', '--', *command,
              required=False, stdin=password, timeout=120)
        statuses = list(output.glob('*-' + label + '.status.json'))
        assert len(statuses) == 1
        status = json.loads(statuses[0].read_text())['exit_code']
        after_log = int(guest(label + '-log-size-after', '/usr/bin/stat', '-f', '%z', log_path))
        assert before_log <= after_log <= before_log + 1024 * 1024, 'Installer log rotated or exceeded its diagnostic bound'
        script_log = guest(label + '-system-log', '/usr/bin/tail', '-c', '+' + str(before_log + 1), log_path)
        return status, script_log.decode(errors='replace')

    def detach(label):
        for device in image_devices(guest, label + '-devices', image):
            guest(label, '/usr/bin/hdiutil', 'detach', device)
        assert control(label + '-endpoints', 'list') == []

    try:
        assert int(guest('uid', '/usr/bin/id', '-u')) != 0
        guest('os', '/usr/bin/sw_vers')
        assert control('modules', 'modules')[0]['enabled'] is True
        assert control('service', 'device-service')['status'] == 'enabled'
        assert control('initial-endpoints', 'list') == []
        guest('catalog-absent', '/bin/test', '!', '-e', CATALOG)
        assert guest('build', '/usr/libexec/PlistBuddy', '-c', 'Print :CFBundleVersion',
                     APP + '/Contents/Info.plist').decode().strip() == str(previous_build)
        before = hashes('executable-hashes-before')
        assert all('MISSING ' + APP + '/' + path + '\n' not in before
                   for path in EXECUTABLES if '/Ext4CheckResource' not in path), 'The installed app is incomplete'
        expected = before if expected is None else expected
        assert 'MISSING ' not in expected, 'A complete replacement app is required'
        guest('prepare', '/bin/sh', '-eu', '-c',
              'umask 077; test ! -e "$1"; mkdir -p "$1/mount"', 'prepare', root)
        guest('copy-package', '/bin/cp', args.guest_package, package)
        guest('copy-image', '/bin/cp', args.guest_image, image)
        package_hash, image_hash = digest(args.package), digest(args.image)
        assert guest('package-hash', '/usr/bin/shasum', '-a', '256', package).decode().split()[0] == package_hash
        assert guest('image-hash', '/usr/bin/shasum', '-a', '256', image).decode().split()[0] == image_hash
        current_image = image
        attached = plistlib.loads(guest('mount-readonly', '/usr/bin/hdiutil', 'attach',
            '-readonly', '-owners', 'off', '-nobrowse', '-mountpoint', mount, '-plist',
            '-imagekey', 'diskimage-class=CRawDiskImage', image))
        entries = [entry for entry in attached['system-entities'] if entry.get('mount-point')]
        assert len(entries) == 1 and entries[0]['mount-point'] == mount
        assert entries[0].get('volume-kind') == 'machlinext4'
        assert re.fullmatch(r'/dev/disk[0-9]+', entries[0]['dev-entry'])
        guest('mounted-type', '/sbin/mount')
        busy_status, script_log = installer('busy-install')
        assert busy_status != 0, 'Installer replaced a mounted driver'
        assert './preinstall: Unmount all Machlin ext4 volumes before installing the driver.' in script_log, 'Installer refusal lacks the mount guard diagnostic'
        assert hashes('executable-hashes-after-refusal') == before
        guest('catalog-still-absent', '/bin/test', '!', '-e', CATALOG)
        guest('read-after-refusal', '/bin/ls', '-a', mount)
        detach('detach-refused')
        current_image = None
        assert guest('readonly-unchanged', '/usr/bin/shasum', '-a', '256', image).decode().split()[0] == image_hash
        result['mounted_install_refused'] = True
        install_status, _ = installer('unmounted-install')
        assert install_status == 0, 'Unmounted installation failed'
        assert hashes('executable-hashes-installed') == expected, 'Installed executables differ from the exported app'
        assert guest('installed-build', '/usr/libexec/PlistBuddy', '-c', 'Print :CFBundleVersion',
                     APP + '/Contents/Info.plist').decode().strip() == str(args.build_number)
        guest('installed-signatures', '/usr/bin/codesign', '--verify', '--deep', '--strict',
              '--all-architectures', APP)
        guest('installed-gatekeeper', '/usr/sbin/spctl', '--assess', '--type', 'execute', '--verbose=2', APP)
        catalog = plistlib.loads(guest('catalog-info', '/bin/cat', CATALOG + '/Contents/Info.plist'))
        extension = plistlib.loads(guest('extension-info', '/bin/cat',
            APP + '/Contents/Extensions/Ext4FSKitExtension.appex/Contents/Info.plist'))
        assert catalog['CFBundleIdentifier'] == 'org.machlin.ext4.filesystem-catalog'
        assert catalog['CFBundleVersion'] == str(args.build_number)
        assert catalog['FSPersonalities'] == extension['EXAppExtensionAttributes']['FSPersonalities']
        for label, path, mode in (('catalog-directory-owner', CATALOG, '755'),
                                  ('catalog-info-owner', CATALOG + '/Contents/Info.plist', '644')):
            assert guest(label, '/usr/bin/stat', '-f', '%u:%g:%Lp', path).decode().strip() == '0:0:' + mode
        source_hash = digest(args.source_package)
        assert guest('installed-source-hash', '/usr/bin/shasum', '-a', '256',
            '/Library/Application Support/Machlin ext4/Machlin-ext4-check-source.tar.gz').decode().split()[0] == source_hash
        modules = control('installed-modules', 'modules')
        assert len(modules) == 1 and modules[0]['enabled'] is True, 'Installed filesystem module is unavailable or disabled'
        assert control('installed-service', 'device-service')['status'] == 'enabled', 'Installation disabled the persistence service'
        assert control('final-endpoints', 'list') == []
        assert digest(args.package) == package_hash and digest(args.image) == image_hash
        result.update(passed=True, package_sha256=package_hash, image_sha256=image_hash,
                      source_sha256=source_hash, catalog_installed=True)
    except GuestTimeout as error:
        result.update(error=str(error), recovery_required=True,
                      cleanup_note='Preserve task process/device for diagnosis; do not retry the installer')
    except Exception as error:
        result['error'] = str(error)
    finally:
        if current_image is not None and not result.get('recovery_required'):
            try:
                detach('failure-cleanup')
            except Exception as error:
                result['cleanup_error'] = str(error)
        (output / 'summary.json').write_text(json.dumps(result, indent=2) + '\n')
        print(json.dumps(result), flush=True)
    return 0 if result['passed'] else 1


if __name__ == '__main__':
    raise SystemExit(main())

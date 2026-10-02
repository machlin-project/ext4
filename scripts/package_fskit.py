#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
"""Package the FSKit app, DiskManagement catalog and complete checker source."""

import argparse
import hashlib
import json
from pathlib import Path
import plistlib
import re
import shutil
import subprocess
import tempfile


ROOT = Path(__file__).resolve().parents[1]
APP_NAME = 'Machlin ext4.app'
CATALOG_NAME = 'machlinext4.fs'
SOURCE_NAME = 'Machlin-ext4-check-source.tar.gz'


def run(command):
    subprocess.run(command, check=True, cwd=ROOT)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--app', type=Path, required=True, help='Exported application bundle')
    parser.add_argument('--source-package', type=Path, required=True,
                        help='Corresponding source exported with this application')
    parser.add_argument('--output', type=Path, required=True, help='New installer package')
    parser.add_argument('--sign', help='Developer ID Installer identity')
    parser.add_argument('--allow-unsigned', action='store_true',
                        help='Build an unsigned package for isolated tests or CI')
    args = parser.parse_args()
    app, source, output = args.app.resolve(), args.source_package.resolve(), args.output.resolve()
    if not app.is_dir() or app.name != APP_NAME or not source.is_file():
        parser.error('Use the exported Machlin ext4 app and its corresponding source archive')
    if output.exists() or output.suffix != '.pkg':
        parser.error('Use a new .pkg output path')
    if bool(args.sign) == args.allow_unsigned:
        parser.error('Choose a Developer ID Installer identity or explicit --allow-unsigned')
    info = plistlib.loads((app / 'Contents/Info.plist').read_bytes())
    if info.get('CFBundleIdentifier') != 'org.machlin.ext4':
        parser.error('Unexpected application identity')
    version = str(info.get('CFBundleVersion', ''))
    if not re.fullmatch(r'[0-9]+(?:\.[0-9]+){0,2}', version):
        parser.error('The application needs a numeric bundle version')
    extension = app / 'Contents/Extensions/Ext4FSKitExtension.appex/Contents/Info.plist'
    extension_info = plistlib.loads(extension.read_bytes())
    attributes = extension_info['EXAppExtensionAttributes']
    if attributes.get('FSShortName') != 'machlinext4':
        parser.error('Unexpected filesystem identity')
    catalog_info = plistlib.loads((ROOT / 'adapters/fskit' / CATALOG_NAME / 'Contents/Info.plist').read_bytes())
    if catalog_info['FSPersonalities'] != attributes['FSPersonalities']:
        parser.error('The filesystem catalog differs from the extension personalities')
    if args.sign:
        run(['/usr/bin/codesign', '--verify', '--deep', '--strict', '--all-architectures', str(app)])
    catalog_info['CFBundleVersion'] = version
    catalog_info['CFBundleShortVersionString'] = info['CFBundleShortVersionString']
    output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='ext4-installer-') as temporary:
        work = Path(temporary)
        payload, scripts = work / 'root', work / 'scripts'
        applications = payload / 'Applications'
        catalog = payload / 'Library/Filesystems' / CATALOG_NAME
        support = payload / 'Library/Application Support/Machlin ext4'
        applications.mkdir(parents=True)
        (catalog / 'Contents').mkdir(parents=True)
        support.mkdir(parents=True)
        scripts.mkdir()
        # Installer metadata must remain readable regardless of the invoking
        # shell's umask. Existing exported app permissions are retained by ditto.
        for directory in (payload, applications, payload / 'Library',
                          catalog.parent, catalog, catalog / 'Contents',
                          support.parent, support):
            directory.chmod(0o755)
        # ditto retains nested code signatures, extended attributes and tickets.
        run(['/usr/bin/ditto', str(app), str(applications / APP_NAME)])
        if args.sign:
            run(['/usr/bin/codesign', '--verify', '--deep', '--strict', '--all-architectures',
                 str(applications / APP_NAME)])
        catalog_plist = catalog / 'Contents/Info.plist'
        catalog_plist.write_bytes(plistlib.dumps(catalog_info))
        shutil.copyfile(source, support / SOURCE_NAME)
        shutil.copyfile(ROOT / 'LICENSE', support / 'LICENSE')
        for readable in (catalog_plist, support / SOURCE_NAME, support / 'LICENSE'):
            readable.chmod(0o644)
        shutil.copyfile(ROOT / 'adapters/fskit/installer/preinstall', scripts / 'preinstall')
        (scripts / 'preinstall').chmod(0o755)
        components = work / 'components.plist'
        run(['/usr/bin/pkgbuild', '--analyze', '--root', str(payload), str(components)])
        bundle_rules = plistlib.loads(components.read_bytes())
        for rule in bundle_rules:
            rule['BundleIsRelocatable'] = False
            rule['BundleHasStrictIdentifier'] = True
            rule['BundleIsVersionChecked'] = True
            rule['BundleOverwriteAction'] = 'upgrade'
        components.write_bytes(plistlib.dumps(bundle_rules))
        command = ['/usr/bin/pkgbuild', '--root', str(payload), '--install-location', '/',
                   '--ownership', 'recommended', '--component-plist', str(components),
                   '--scripts', str(scripts), '--identifier', 'org.machlin.ext4.installer',
                   '--version', version]
        if args.sign:
            command.extend(['--sign', args.sign, '--timestamp'])
        run([*command, str(output)])
    print(json.dumps({'package': str(output), 'signed': bool(args.sign), 'bundle_version': version,
                      'package_sha256': hashlib.sha256(output.read_bytes()).hexdigest(),
                      'source_sha256': hashlib.sha256(source.read_bytes()).hexdigest(),
                      'installed_paths': ['/Applications/' + APP_NAME,
                                          '/Library/Filesystems/' + CATALOG_NAME,
                                          '/Library/Application Support/Machlin ext4']}, indent=2))


if __name__ == '__main__':
    main()

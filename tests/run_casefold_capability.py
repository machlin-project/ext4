#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
"""Probe native capabilities in owned disposable images, never a host volume.

This is preparation for a Linux interoperability oracle. A pass establishes only
the running kernel's basic encrypted+casefold operations, not core acceptance.
Run as root on a disposable standard CI VM. The outer process creates a private
mount namespace before the inner process can attach or mount an image.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import signal
import subprocess
import sys

from generate_fixtures import EXPECTED_FEATURES, UUID, resolve_tools


def digest(path):
    with Path(path).open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--probe', type=Path, required=True)
    parser.add_argument('--tools-root', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--outer-namespace', help=argparse.SUPPRESS)
    args = parser.parse_args()
    if os.geteuid() != 0:
        parser.error('requires an explicitly authorized disposable root CI environment')
    namespace = os.readlink('/proc/self/ns/mnt')
    if args.outer_namespace is None:
        command = ['unshare', '--mount', '--propagation', 'private', sys.executable,
                   str(Path(__file__).resolve()), *sys.argv[1:],
                   '--outer-namespace', namespace]
        # No supervising process may kill/orphan the namespace cleanup owner.
        # unshare without --fork replaces itself with the inner Python process.
        os.execvp(command[0], command)
        raise AssertionError('exec returned')
    if namespace == args.outer_namespace:
        parser.error('private mount namespace was not established')
    probe = args.probe.resolve(strict=True)
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    output.chmod(0o755)
    tools = resolve_tools(args.tools_root.resolve())
    report = {'kind': 'native-capability-only', 'passed': False,
              'kernel': os.uname().release, 'machine': os.uname().machine,
              'runner_image': os.environ.get('ImageVersion'),
              'probe_sha256': digest(probe), 'namespace': namespace,
              'profiles': []}
    report_path = output / 'report.json'

    def save():
        report_path.write_text(json.dumps(report, indent=2) + '\n')
        report_path.chmod(0o644)

    def run(row, command, timeout=120):
        command = [str(value) for value in command]
        done = subprocess.run(command, capture_output=True, text=True, timeout=timeout)
        row.setdefault('commands', []).append({'command': command,
                    'status': done.returncode, 'stdout': done.stdout, 'stderr': done.stderr})
        save()
        if done.returncode != 0:
            raise RuntimeError(f'Native capability command failed: {command}: {done.stderr}')
        return done.stdout.strip()

    def interrupted(signum, frame):
        raise KeyboardInterrupt(f'signal {signum}')

    signal.signal(signal.SIGTERM, interrupted)
    signal.signal(signal.SIGINT, interrupted)

    def cleanup(row, image, mountpoint):
        # Reconcile actual ownership, including a timeout/signal/logging error
        # after a kernel operation succeeded but before Python recorded it.
        # Cleanup does not depend on report writes succeeding.
        def inspect(command, absent=False):
            done = subprocess.run([str(value) for value in command], capture_output=True,
                                  text=True, timeout=30)
            row['commands'].append({'command': [str(value) for value in command],
                                    'status': done.returncode, 'stdout': done.stdout,
                                    'stderr': done.stderr, 'cleanup': True})
            if done.returncode != 0 and not (absent and done.returncode == 1):
                raise RuntimeError(f'Owned-resource cleanup failed: {command}: {done.stderr}')
            return done.stdout.strip() if done.returncode == 0 else ''

        previous = [(kind, signal.signal(kind, signal.SIG_IGN))
                    for kind in (signal.SIGTERM, signal.SIGINT)]
        try:
            loops = inspect(['losetup', '--associated', image, '--noheadings',
                             '--output', 'NAME']).splitlines()
            source = inspect(['findmnt', '--noheadings', '--output', 'SOURCE',
                              '--mountpoint', mountpoint], absent=True)
            if len(loops) > 1 or any(not loop.startswith('/dev/loop') for loop in loops):
                raise RuntimeError('Ambiguous disposable loop ownership during cleanup')
            if source and (len(loops) != 1 or source != loops[0]):
                raise RuntimeError('Refusing cleanup of an unexpected mounted source')
            for owned in loops:
                backing = inspect(['losetup', '--noheadings', '--output', 'BACK-FILE', owned])
                if Path(backing).resolve(strict=True) != image.resolve(strict=True):
                    raise RuntimeError('Refusing cleanup of a changed loop owner')
                if source:
                    inspect(['umount', mountpoint])
                inspect(['losetup', '--detach', owned])
        finally:
            for kind, handler in previous:
                signal.signal(kind, handler)

    save()
    try:
        config = Path('/boot') / ('config-' + os.uname().release)
        report['kernel_config_available'] = config.is_file()
        if config.is_file():
            wanted = ('CONFIG_EXT4_FS=', 'CONFIG_FS_ENCRYPTION=', 'CONFIG_UNICODE=')
            report['kernel_config'] = [line for line in config.read_text().splitlines()
                                       if line.startswith(wanted)]
        for block_size, strict in ((1024, False), (4096, True)):
            profile = output / str(block_size)
            profile.mkdir()
            profile.chmod(0o755)
            image = profile / 'capability.img'
            mountpoint = profile / 'mount'
            mountpoint.mkdir()
            row = {'block_size': block_size, 'strict': strict, 'passed': False,
                   'commands': []}
            report['profiles'].append(row)
            features = EXPECTED_FEATURES | {'encrypt', 'casefold'}
            encoding = 'encoding=utf8-12.1' + (',encoding_flags=strict' if strict else '')
            run(row, [tools['mke2fs'], '-F', '-t', 'ext4', '-b', block_size,
                      '-N', 512, '-I', 256, '-m', 0, '-O', 'none,' + ','.join(sorted(features)),
                      '-U', UUID, '-E', encoding + ',lazy_itable_init=0,nodiscard',
                      image, 32 * 1024 * 1024 // block_size])
            image.chmod(0o644)
            loop = None
            try:
                for phase in ('create', 'nokey', 'keyed'):
                    readonly = phase != 'create'
                    before = digest(image)
                    command = ['losetup', '--find', '--show']
                    if readonly:
                        command.append('--read-only')
                    loop = run(row, [*command, image])
                    if not loop.startswith('/dev/loop') or '\n' in loop:
                        raise RuntimeError('Invalid newly allocated loop identity')
                    backing = run(row, ['losetup', '--noheadings', '--output', 'BACK-FILE', loop])
                    if Path(backing).resolve(strict=True) != image.resolve(strict=True):
                        raise RuntimeError('Loop backing identity mismatch')
                    options = 'ro,noload,nodev,nosuid,noexec' if readonly else 'rw,nodev,nosuid,noexec'
                    run(row, ['mount', '-t', 'ext4', '-o', options, loop, mountpoint])
                    actual = run(row, ['findmnt', '--noheadings', '--output', 'SOURCE',
                                       '--mountpoint', mountpoint])
                    if actual != loop:
                        raise RuntimeError('Mounted source identity mismatch')
                    run(row, [probe, phase, mountpoint])
                    run(row, ['umount', mountpoint])
                    run(row, ['losetup', '--detach', loop])
                    loop = None
                    if readonly and digest(image) != before:
                        raise RuntimeError('Read-only native verification changed media')
                run(row, [tools['e2fsck'], '-fn', image])
                row.update(passed=True, image_sha256=digest(image))
            finally:
                cleanup(row, image, mountpoint)
            save()
        if digest(probe) != report['probe_sha256']:
            raise RuntimeError('Probe binary changed during execution')
        report['passed'] = True
        print('PASS native encrypted-casefold capability: 2 images, 8 policies; core untested')
        return 0
    except (OSError, RuntimeError, subprocess.SubprocessError, KeyboardInterrupt) as error:
        report['failure'] = str(error)
        print(f'BLOCKED/FAILED native capability gate: {error}', file=sys.stderr)
        return 1
    finally:
        save()


if __name__ == '__main__':
    sys.exit(main())

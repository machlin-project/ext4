#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
"""Unprivileged mocked lifecycle checks; no native capability claims."""
import contextlib
import io
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

import run_casefold_capability as capability


class Lifecycle(unittest.TestCase):
    def exercise(self, failure=None, roundtrip=False, indexed=False):
        with tempfile.TemporaryDirectory(prefix='casefold-capability-unit-') as temporary:
            root = Path(temporary)
            probe = root / 'probe'
            probe.write_bytes(b'unexecuted probe placeholder')
            core = root / 'core'
            core.write_bytes(b'unexecuted core placeholder')
            output = root / 'output'
            commands = []
            owner = None
            attached = False
            mounted = False
            injected = False

            def copy_mock(source, target):
                # Sparse, bounded mock bytes only; no filesystem image is mounted.
                with Path(source).open('rb') as reader, Path(target).open('wb') as writer:
                    writer.write(reader.read(64))
                    writer.truncate(Path(source).stat().st_size)
                return target

            def command(argv, **kwargs):
                nonlocal owner, attached, mounted, injected
                commands.append(argv)
                text = ''
                code = 0
                name = argv[0]
                if name == 'fake-mke2fs':
                    Path(argv[-2]).write_bytes(b'bounded mock media')
                    if indexed:
                        with Path(argv[-2]).open('ab') as stream:
                            stream.truncate(32 * 1024 * 1024)
                elif name == 'losetup':
                    if '--associated' in argv:
                        text = '/dev/loop999999' if attached else ''
                        if failure == 'multiple' and attached:
                            text += '\n/dev/loop999998'
                    elif '--find' in argv:
                        self.assertFalse(attached)
                        owner = Path(argv[-1])
                        attached = True
                        text = '/dev/loop999999\n'
                        if failure == 'attach-timeout' and not injected:
                            injected = True
                            raise subprocess.TimeoutExpired(argv, 120)
                        if failure == 'attach-signal' and not injected:
                            injected = True
                            raise KeyboardInterrupt('after attachment')
                    elif '--detach' in argv:
                        self.assertTrue(attached)
                        self.assertFalse(mounted)
                        self.assertEqual(argv[-1], '/dev/loop999999')
                        attached = False
                    else:
                        self.assertTrue(attached)
                        text = str(probe if failure == 'owner' else owner)
                elif name == 'mount':
                    self.assertTrue(attached)
                    self.assertFalse(mounted)
                    if failure == 'mount' and not injected:
                        code = 1
                        injected = True
                    else:
                        mounted = True
                        if failure == 'mount-signal' and not injected:
                            injected = True
                            raise KeyboardInterrupt('after mount')
                elif name == 'findmnt':
                    text = '/dev/loop999999' if mounted else ''
                    if failure == 'foreign-mount' and mounted:
                        text = '/dev/unrelated'
                    code = 0 if mounted else 1
                elif name == 'umount':
                    self.assertTrue(mounted)
                    if failure == 'unmount':
                        code = 1
                    else:
                        mounted = False
                elif name == str(probe):
                    self.assertTrue(mounted)
                    if failure in ('probe', 'multiple') and not injected:
                        code = 1
                        injected = True
                    elif failure == 'signal' and not injected:
                        injected = True
                        raise KeyboardInterrupt('mocked termination')
                    elif failure == 'readonly-change' and argv[1] == 'nokey':
                        owner.write_bytes(b'wrongly changed media')
                    text = 'mocked probe only'
                elif name == str(core):
                    self.assertFalse(mounted)
                    self.assertFalse(attached)
                    image, exports = Path(argv[2]), Path(argv[3])
                    is_indexed = argv[1] == '--casefold-native-indexed'
                    copy_mock(image, exports / (('indexed-' if is_indexed else 'combined-') + image.name))
                    if failure == 'core-input-change':
                        image.write_bytes(b'changed protected original')
                    if is_indexed:
                        if failure == 'indexed-input-change':
                            image.write_bytes(b'changed indexed original')
                        if failure == 'indexed-baseline-change':
                            (image.parent / 'baseline.json').write_text('{"changed":true}')
                        if failure == 'indexed-plan-change' and len(argv) == 5:
                            Path(argv[4]).write_text('changed plan')
                elif name == sys.executable:
                    if Path(argv[1]).name == 'check_encrypted_casefold_native.py':
                        self.assertFalse(mounted)
                        self.assertFalse(attached)
                        code = 1 if failure == 'raw' else 0
                    elif Path(argv[1]).name == 'verify_casefold_roundtrip_linux.py':
                        self.assertTrue(mounted)
                        self.assertTrue(attached)
                        code = 1 if failure == 'native' else 0
                    elif Path(argv[1]).name == 'linux_casefold_indexed.py':
                        self.assertTrue(mounted)
                        self.assertTrue(attached)
                        baseline = Path(argv[argv.index('--baseline') + 1])
                        if argv[2] == 'create':
                            baseline.write_text('{"identity":"mocked independent Linux baseline"}')
                            if '--collision-plan' in argv:
                                Path(argv[argv.index('--collision-plan') + 1]).write_text('mocked plan')
                        else:
                            code = 1 if failure == 'indexed-native' else 0
                    elif Path(argv[1]).name == 'check_encrypted_casefold_indexed.py':
                        self.assertFalse(mounted)
                        self.assertFalse(attached)
                        destination = Path(argv[argv.index('--output') + 1])
                        original = Path(argv[argv.index('--original') + 1])
                        destination.mkdir()
                        expected = '{"identity":"mocked independent Linux baseline"}'
                        if failure == 'indexed-baseline-disagree':
                            expected = '{"identity":"different raw baseline"}'
                        (destination / 'baseline.json').write_text(expected)
                        self.assertEqual(original.stat().st_size, 32 * 1024 * 1024
                            if failure != 'indexed-input-change' else len(b'changed indexed original'))
                        code = 1 if failure == 'indexed-raw' else 0
                    else:
                        self.fail(f'unexpected Python command: {argv}')
                elif name == 'fake-e2fsck':
                    self.assertFalse(attached)
                    self.assertFalse(mounted)
                else:
                    self.fail(f'unexpected command: {argv}')
                return subprocess.CompletedProcess(argv, code, stdout=text, stderr='mock failure' if code else '')

            real_write = Path.write_text

            def write_text(path, *args, **kwargs):
                nonlocal injected
                if failure == 'log' and attached and not injected:
                    injected = True
                    raise OSError('mock report failure after attachment')
                return real_write(path, *args, **kwargs)

            arguments = ['run_casefold_capability.py', '--probe', str(probe),
                         '--tools-root', str(root), '--output', str(output),
                         '--outer-namespace', 'mnt:[outer]']
            if roundtrip:
                arguments.extend(('--core-test', str(core)))
            if indexed:
                arguments.append('--indexed')
            with patch.object(sys, 'argv', arguments), \
                    patch.object(capability.os, 'geteuid', return_value=0), \
                    patch.object(capability.os, 'readlink', return_value='mnt:[inner]'), \
                    patch.object(capability.signal, 'signal'), \
                    patch.object(capability, 'resolve_tools', return_value={
                        'mke2fs': 'fake-mke2fs', 'e2fsck': 'fake-e2fsck'}), \
                    patch.object(capability.subprocess, 'run', side_effect=command), \
                    patch.object(Path, 'write_text', write_text), \
                    contextlib.redirect_stdout(io.StringIO()), \
                    contextlib.redirect_stderr(io.StringIO()):
                result = capability.main()
            report = json.loads((output / 'report.json').read_text())
            if failure in ('owner', 'unmount', 'multiple', 'foreign-mount'):
                self.assertTrue(attached, 'uncertain ownership/live mounts must never be detached')
                self.assertFalse(any('--detach' in item for item in commands))
            else:
                self.assertFalse(attached)
                self.assertFalse(mounted)
            return result, report, commands

    def test_success_is_labeled_capability_only(self):
        result, report, commands = self.exercise()
        self.assertEqual(result, 0)
        self.assertTrue(report['passed'])
        self.assertEqual(report['kind'], 'native-capability-only')
        self.assertEqual(len(report['profiles']), 2)
        self.assertEqual(sum(item[0] == 'mount' for item in commands), 6)
        self.assertEqual(sum('--detach' in item for item in commands), 6)

    def test_failures_cleanup_only_owned_resources(self):
        for failure in ('mount', 'probe', 'signal', 'readonly-change', 'owner', 'unmount',
                        'attach-timeout', 'attach-signal', 'mount-signal', 'log',
                        'multiple', 'foreign-mount'):
            with self.subTest(failure=failure):
                result, report, commands = self.exercise(failure)
                self.assertEqual(result, 1)
                self.assertFalse(report['passed'])
                self.assertIn('failure', report)

    def test_roundtrip_runs_detached_core_and_raw_checks(self):
        result, report, commands = self.exercise(roundtrip=True)
        self.assertEqual(result, 0)
        self.assertEqual(report['kind'], 'native-linear-core-roundtrip')
        self.assertEqual(sum(item[0] == 'mount' for item in commands), 10)
        self.assertEqual(sum('--detach' in item for item in commands), 10)
        self.assertEqual(sum('--casefold-native' in item for item in commands), 2)

    def test_roundtrip_failures_are_not_capability_passes(self):
        for failure in ('core-input-change', 'raw', 'native'):
            with self.subTest(failure=failure):
                result, report, commands = self.exercise(failure, roundtrip=True)
                self.assertEqual(result, 1)
                self.assertFalse(report['passed'])
                self.assertIn('failure', report)

    def test_indexed_keeps_linear_images_and_detached_checks(self):
        result, report, commands = self.exercise(roundtrip=True, indexed=True)
        self.assertEqual(result, 0)
        self.assertEqual(report['kind'], 'native-linear-indexed-core-roundtrip')
        self.assertEqual(sum(item[0] == 'mount' for item in commands), 20)
        self.assertEqual(sum('--detach' in item for item in commands), 20)
        self.assertEqual(sum('--casefold-native-indexed' in item for item in commands), 2)
        self.assertTrue(all(profile['indexed']['passed'] for profile in report['profiles']))

    def test_indexed_failure_never_becomes_partial_success(self):
        for failure in ('indexed-input-change', 'indexed-baseline-change', 'indexed-plan-change',
                        'indexed-baseline-disagree', 'indexed-raw', 'indexed-native'):
            with self.subTest(failure=failure):
                result, report, _ = self.exercise(failure, roundtrip=True, indexed=True)
                self.assertEqual(result, 1)
                self.assertFalse(report['passed'])

    def test_same_namespace_refuses_before_commands(self):
        arguments = ['run_casefold_capability.py', '--probe', '/unused', '--tools-root',
                     '/unused', '--output', '/unused', '--outer-namespace', 'mnt:[same]']
        with patch.object(sys, 'argv', arguments), \
                patch.object(capability.os, 'geteuid', return_value=0), \
                patch.object(capability.os, 'readlink', return_value='mnt:[same]'), \
                patch.object(capability.subprocess, 'run') as run, \
                contextlib.redirect_stderr(io.StringIO()):
            with self.assertRaises(SystemExit):
                capability.main()
            run.assert_not_called()

    def test_outer_replaces_process_before_resource_acquisition(self):
        arguments = ['run_casefold_capability.py', '--probe', '/unused', '--tools-root',
                     '/unused', '--output', '/unused']
        with patch.object(sys, 'argv', arguments), \
                patch.object(capability.os, 'geteuid', return_value=0), \
                patch.object(capability.os, 'readlink', return_value='mnt:[outer]'), \
                patch.object(capability.os, 'execvp', side_effect=RuntimeError('mock exec')) as execute, \
                patch.object(capability.subprocess, 'run') as run:
            with self.assertRaisesRegex(RuntimeError, 'mock exec'):
                capability.main()
            command = execute.call_args.args[1]
            self.assertEqual(command[:4], ['unshare', '--mount', '--propagation', 'private'])
            self.assertEqual(command[-2:], ['--outer-namespace', 'mnt:[outer]'])
            self.assertNotIn('--fork', command)
            run.assert_not_called()


if __name__ == '__main__':
    unittest.main()

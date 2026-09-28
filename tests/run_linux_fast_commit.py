#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
"""Capture actual Linux fast commits and independently replay them in the guest."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tests'))
from generate_fixtures import EXPECTED_FEATURES, UUID, resolve_tools
from fast_commit_reference import read_namespace

PROTOCOL_READBACK_PHASE = 2
INDIRECT_READBACK_PHASE = 3
LARGE_PREFIX_READBACK_PHASE = 4


def digest(path):
    with Path(path).open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def readback_fixtures(args, output, tools, recover, prep, runner, kernel, archive, rows, run,
                      build_archive):
    fixtures = (args.xattr_fixtures or args.indirect_fixtures or args.large_prefix_fixtures).resolve()
    if args.xattr_fixtures:
        profiles = ('xattr-reuse-1k', 'xattr-reuse-4k', 'xattr-reuse-legacy-1k')
        marker = 'LINUX_FAST_COMMIT_XATTR_REUSE_PASS'
    elif args.indirect_fixtures:
        profiles = ('indirect-1k', 'indirect-4k')
        marker = 'LINUX_FAST_COMMIT_INDIRECT_PASS'
    else:
        profiles = ('large-prefix-1k', 'large-prefix-4k', 'huge-prefix-1k')
        marker = 'LINUX_FAST_COMMIT_LARGE_PREFIX_PASS'
    source_rows = json.loads((fixtures / 'report.json').read_text())
    if not source_rows or not all(row.get('passed') for row in source_rows):
        raise RuntimeError('Protocol fixture generation is not accepted')
    cases = {row['profile']: row for row in source_rows if 'profile' in row}
    if args.large_prefix_fixtures:
        # A fixture set may contain the large prefixes, the huge prefix, or both.
        profiles = tuple(profile for profile in profiles if profile in cases)
        if not profiles:
            raise RuntimeError('No large-prefix protocol fixtures were generated')
    for profile in profiles:
        expected = cases[profile]
        if args.block_size and expected['block_size'] not in args.block_size:
            continue
        source = fixtures / profile / 'pending.img'
        if digest(source) != expected['pending_sha256']:
            raise RuntimeError('Protected protocol fixture changed')
        directory = output / profile
        directory.mkdir()
        image = directory / 'native-verified.img'
        shutil.copyfile(source, image)
        row = dict(profile=profile, commands=[], passed=False,
                   recovery='protocol fixture: core recovery followed by Linux readback',
                   expected_commit_path='fast commit', pending_sha256=digest(source))
        rows.append(row)
        if digest(recover) != prep['recover_sha256']:
            raise RuntimeError('Recovery executable changed during fixture readback')
        row['core_recovery'] = run(row, [recover, '--write', image])
        fast = re.search(r'\bfast_commits=(\d+)\b', row['core_recovery'])
        if not fast or int(fast[1]) != expected['commits']:
            raise RuntimeError('Core did not replay the complete protocol fixture')
        row['core_fast_commits'] = int(fast[1])
        run(row, [tools['e2fsck'], '-fn', image])
        row['core_recovered_sha256'] = digest(image)
        if args.large_prefix_fixtures:
            archive = build_archive(LARGE_PREFIX_READBACK_PHASE, expected['created_files'])
        console = run(row, [runner, kernel, archive, 2, 512,
                           'console=hvc0 rdinit=/init panic=-1 loglevel=4', image])
        (directory / 'readback.console.log').write_text(console)
        for required in ('LINUX_FAST_COMMIT_KERNEL=Linux 6.12.94-0-virt aarch64',
                         'LINUX_FAST_COMMIT_MOUNT_OPTIONS=data=ordered,commit=600',
                         marker,
                         'LINUX_FAST_COMMIT_FIXTURE_READBACK_PASS',
                         'LINUX_FAST_COMMIT_RESULT=PASS'):
            if required not in console:
                raise RuntimeError(f'Missing native protocol readback evidence: {required}')
        for forbidden in ('EXT4-fs error', 'Aborting journal', 'Data will be lost'):
            if forbidden in console:
                raise RuntimeError(f'Guest filesystem error: {forbidden}')
        run(row, [tools['e2fsck'], '-fn', image])
        exported = directory / 'files'
        exported.mkdir()
        actual = read_namespace(image, exported, expected['block_size'], tools['debugfs'],
                                lambda command: run(row, command))
        if any(actual[key] != expected[key]
               for key in ('files', 'directories', 'symlinks', 'special', 'xattrs')):
            raise RuntimeError('Namespace, data or attributes changed during Linux readback')
        if digest(source) != row['pending_sha256']:
            raise RuntimeError('Protected protocol fixture changed during native readback')
        row.update(passed=True, native_sha256=digest(image),
                   xattrs=sum(len(values) for values in actual['xattrs'].values()))
        (output / 'report.json').write_text(json.dumps(rows, indent=2) + '\n')
        print(f'PASS {profile}: {row["recovery"]}', flush=True)


def main():
    p = argparse.ArgumentParser()
    p.add_argument('--lab', type=Path, required=True)
    p.add_argument('--prepared', type=Path, required=True)
    p.add_argument('--runner', type=Path, required=True)
    p.add_argument('--output', type=Path, required=True)
    p.add_argument('--recover', type=Path,
                   help='recover a copy with this core utility before native verification')
    p.add_argument('--block-size', type=int, choices=(1024, 4096), action='append',
                   help='limit this capture to selected block sizes; default: both')
    p.add_argument('--orphan-file', action='store_true',
                   help='capture fast commits with the modern orphan-file feature enabled')
    p.add_argument('--special-files', action='store_true',
                   help='verify Linux full-commit fallback after symlinks and special inodes')
    fixtures = p.add_mutually_exclusive_group()
    fixtures.add_argument('--xattr-fixtures', type=Path,
                          help='verify core-recovered xattr protocol fixtures in Linux')
    fixtures.add_argument('--indirect-fixtures', type=Path,
                          help='verify core-recovered indirect protocol fixtures in Linux')
    fixtures.add_argument('--large-prefix-fixtures', type=Path,
                          help='verify large core-recovered protocol prefixes in Linux')
    a = p.parse_args()
    fixture_phase = (PROTOCOL_READBACK_PHASE if a.xattr_fixtures else
                     INDIRECT_READBACK_PHASE if a.indirect_fixtures else
                     LARGE_PREFIX_READBACK_PHASE if a.large_prefix_fixtures else None)
    if fixture_phase is not None and (not a.recover or a.orphan_file or a.special_files):
        p.error('protocol fixtures require --recover and exclude native capture options')
    lab, prepared, runner, output = (v.resolve() for v in (a.lab, a.prepared, a.runner, a.output))
    if Path.cwd() != lab:
        p.error('run with explicit lab working directory')
    previous = json.loads((prepared / 'report.json').read_text())
    assert previous[0]['passed'] and all(r['passed'] for r in previous[1:])
    kernel = prepared / 'Image'
    assert digest(kernel) == previous[0]['kernel_sha256']
    output.mkdir(parents=True, exist_ok=False)
    tools = resolve_tools(lab / 'vendor/e2fsprogs-ext4/build')
    recover = a.recover.resolve() if a.recover else None
    rows = []

    def run(row, command):
        command = [str(x) for x in command]
        done = subprocess.run(command, cwd=lab, capture_output=True, text=True, timeout=180)
        row['commands'].append(dict(command=command, status=done.returncode,
                                    stdout=done.stdout, stderr=done.stderr))
        (output / 'report.json').write_text(json.dumps(rows, indent=2) + '\n')
        if done.returncode:
            raise RuntimeError(f'Command failed: {command}: {done.stderr}')
        return done.stdout

    prep = dict(kind='preparation', commands=[], kernel_sha256=digest(kernel),
                runner_sha256=digest(runner))
    if recover:
        prep.update(recover_program=str(recover), recover_sha256=digest(recover))
    rows.append(prep)
    run(prep, ['/usr/bin/codesign', '--verify', '--strict', runner])
    compile_command = previous[0]['commands'][1]['command'].copy()
    assert Path(compile_command[0]).name == 'clang'
    assert compile_command[-2] == '-o'
    source_index = compile_command.index(str(ROOT / 'tests/linux_external_journal.c'))
    source = output / 'probe-source.c'
    shutil.copyfile(ROOT / 'tests/linux_fast_commit.c', source)
    prep['probe_source_sha256'] = digest(source)
    compile_command[source_index] = str(source)
    compile_command[-1] = str(output / 'init')
    run(prep, compile_command)
    def build_archive(phase, prefix_files=0):
        label = f'{phase}-{prefix_files}' if prefix_files else f'{phase}'
        tree = output / f'root-{label}'
        shutil.copytree(prepared / 'root-0', tree)
        (tree / 'proc').mkdir(exist_ok=True)
        shutil.copy2(output / 'init', tree / 'init')
        (tree / 'phase').write_text(f'{phase}\n')
        (tree / 'modern-orphans').write_text(f'{int(a.orphan_file)}\n')
        (tree / 'special-files').write_text(f'{int(a.special_files)}\n')
        (tree / 'prefix-files').write_text(f'{prefix_files}\n')
        archive = output / f'phase-{label}.cpio'
        listing = '\n'.join(str(x.relative_to(tree)) for x in sorted(tree.rglob('*'))) + '\n'
        with archive.open('wb') as stream:
            subprocess.run(['/usr/bin/cpio', '-o', '-H', 'newc'], cwd=tree,
                           input=listing.encode(), stdout=stream, check=True)
        return archive

    archives = {}
    phases = (fixture_phase,) if fixture_phase is not None else range(2)
    for phase in phases:
        if phase != LARGE_PREFIX_READBACK_PHASE:
            archives[phase] = build_archive(phase)
    prep.update(passed=True, probe_sha256=digest(output / 'init'))
    if fixture_phase is not None:
        readback_fixtures(a, output, tools, recover, prep, runner, kernel,
                          archives.get(fixture_phase), rows, run, build_archive)
        return
    for block in a.block_size or (1024, 4096):
        name = f'fast-commit-{block // 1024}k'
        directory = output / name
        directory.mkdir()
        seed = directory / 'root'
        seed.mkdir()
        (seed / 'hello.txt').write_bytes(b'A' * (3 * block + 17))
        (seed / 'hello.txt').chmod(0o644)
        original = directory / 'original.img'
        pending = directory / 'pending.img'
        native = directory / 'native-recovered.img'
        journal = directory / 'pending.journal'
        row = dict(profile=name, block_size=block, commands=[], passed=False,
                   recovery='core followed by native verification' if recover else 'native')
        rows.append(row)
        features = EXPECTED_FEATURES | {'fast_commit'}
        if a.orphan_file:
            features.add('orphan_file')
        row['orphan_file'] = a.orphan_file
        row['special_files'] = a.special_files
        row['expected_commit_path'] = 'ordinary fallback' if a.special_files else 'fast commit'
        run(row, [tools['mke2fs'], '-F', '-t', 'ext4', '-b', block, '-N', 256, '-I', 256,
                  '-m', 0, '-O', 'none,' + ','.join(sorted(features)), '-U', UUID,
                  '-J', 'size=8,fast_commit_size=256', '-E', 'lazy_itable_init=0,nodiscard', '-d', seed,
                  original, 64 * 1024 * 1024 // block])
        run(row, [tools['e2fsck'], '-fn', original])
        shutil.copyfile(original, pending)

        def boot(image, phase):
            text = run(row, [runner, kernel, archives[phase], 2, 512,
                            'console=hvc0 rdinit=/init panic=-1 loglevel=4', image])
            (directory / f'phase-{phase}.console.log').write_text(text)
            for required in ('LINUX_FAST_COMMIT_KERNEL=Linux 6.12.94-0-virt aarch64',
                             'LINUX_FAST_COMMIT_MOUNT_OPTIONS=data=ordered,commit=600',
                             'LINUX_FAST_COMMIT_RESULT=PASS'):
                if required not in text:
                    raise RuntimeError(f'Missing native evidence: {required}')
            if phase == 1 and 'LINUX_FAST_COMMIT_REPLAY_PASS' not in text:
                raise RuntimeError('Missing native replay check')
            if a.special_files and 'LINUX_FAST_COMMIT_SPECIALS_PASS' not in text:
                raise RuntimeError('Missing native special-file verification')
            for forbidden in ('EXT4-fs error', 'Aborting journal', 'Data will be lost'):
                if forbidden in text:
                    raise RuntimeError(f'Guest filesystem error: {forbidden}')
            if phase == 0:
                stats = re.search(r'(?m)^(\d+) commits$', text)
                if not stats or int(stats[1]) == 0:
                    raise RuntimeError('Missing actual native fast-commit counters')
                row['native_fast_commits'] = int(stats[1])
                ineligible = re.search(r'(?m)^(\d+) ineligible$', text)
                journal_data = re.search(r'(?m)^"Data journalling":\s*(\d+)$', text)
                if not ineligible or not journal_data:
                    raise RuntimeError('Missing native full-commit fallback counters')
                row['native_ineligible_commits'] = int(ineligible[1])
                row['native_data_journalling_reasons'] = int(journal_data[1])
                if a.special_files:
                    if int(ineligible[1]) != 1 or int(journal_data[1]) == 0:
                        raise RuntimeError('Missing expected special-inode full-commit fallback')
                elif int(ineligible[1]) != 0:
                    raise RuntimeError('Unexpected full-commit fallback in fast-commit capture')
                if a.orphan_file:
                    orphan = re.search(r'(?m)^LINUX_FAST_COMMIT_HELD_ORPHAN=(\d+)$', text)
                    if not orphan or int(orphan[1]) == 0:
                        raise RuntimeError('Missing checkpointed open-unlinked orphan')
                    row['held_orphan'] = int(orphan[1])

        boot(pending, 0)
        header = run(row, [tools['dumpe2fs'], '-h', pending])
        if 'needs_recovery' not in header or 'fast_commit' not in header:
            raise RuntimeError('Native filesystem lacks pending fast-commit features')
        if a.orphan_file and 'orphan_present' not in header:
            raise RuntimeError('Native filesystem lacks pending orphan-file state')
        row['pending_header'] = header
        run(row, [tools['debugfs'], '-R', f'dump <8> "{journal}"', pending])
        if not journal.is_file():
            raise RuntimeError('Journal dump was not produced')
        row.update(pending_sha256=digest(pending), journal_sha256=digest(journal))
        shutil.copyfile(pending, native)
        if recover:
            if digest(recover) != prep['recover_sha256']:
                raise RuntimeError('Recovery executable changed during capture')
            row['core_recovery'] = run(row, [recover, '--write', native])
            fast = re.search(r'\bfast_commits=(\d+)\b', row['core_recovery'])
            ordinary = re.search(r'\btransactions=(\d+)\b', row['core_recovery'])
            if not fast or not ordinary:
                raise RuntimeError('Missing core replay transaction counters')
            row['core_fast_commits'] = int(fast[1])
            row['core_ordinary_transactions'] = int(ordinary[1])
            if a.special_files:
                if int(fast[1]) != 0 or int(ordinary[1]) == 0:
                    raise RuntimeError('Special-inode fallback did not use ordinary replay')
            elif int(fast[1]) == 0:
                raise RuntimeError('Core did not replay the expected fast-commit prefix')
            run(row, [tools['e2fsck'], '-fn', native])
            row['core_recovered_sha256'] = digest(native)
        boot(native, 1)
        run(row, [tools['e2fsck'], '-fn', native])
        row['native_header'] = run(row, [tools['dumpe2fs'], '-h', native])
        if digest(pending) != row['pending_sha256'] or digest(journal) != row['journal_sha256']:
            raise RuntimeError('Protected pending inputs changed')
        row.update(passed=True, native_sha256=digest(native))
        (output / 'report.json').write_text(json.dumps(rows, indent=2) + '\n')
        print(f'PASS {name}: {row["expected_commit_path"]}, {row["recovery"]}', flush=True)


if __name__ == '__main__':
    main()

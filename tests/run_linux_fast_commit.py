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


def digest(path):
    with Path(path).open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


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
    a = p.parse_args()
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
    archives = []
    for phase in range(2):
        tree = output / f'root-{phase}'
        shutil.copytree(prepared / 'root-0', tree)
        (tree / 'proc').mkdir(exist_ok=True)
        shutil.copy2(output / 'init', tree / 'init')
        (tree / 'phase').write_text(f'{phase}\n')
        archive = output / f'phase-{phase}.cpio'
        listing = '\n'.join(str(x.relative_to(tree)) for x in sorted(tree.rglob('*'))) + '\n'
        with archive.open('wb') as stream:
            subprocess.run(['/usr/bin/cpio', '-o', '-H', 'newc'], cwd=tree,
                           input=listing.encode(), stdout=stream, check=True)
        archives.append(archive)
    prep.update(passed=True, probe_sha256=digest(output / 'init'))
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
            for forbidden in ('EXT4-fs error', 'Aborting journal', 'Data will be lost'):
                if forbidden in text:
                    raise RuntimeError(f'Guest filesystem error: {forbidden}')
            if phase == 0:
                stats = re.search(r'(?m)^(\d+) commits$', text)
                if not stats or int(stats[1]) == 0:
                    raise RuntimeError('Missing actual native fast-commit counters')
                row['native_fast_commits'] = int(stats[1])

        boot(pending, 0)
        header = run(row, [tools['dumpe2fs'], '-h', pending])
        if 'needs_recovery' not in header or 'fast_commit' not in header:
            raise RuntimeError('Native filesystem lacks pending fast-commit features')
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
            run(row, [tools['e2fsck'], '-fn', native])
            row['core_recovered_sha256'] = digest(native)
        boot(native, 1)
        run(row, [tools['e2fsck'], '-fn', native])
        row['native_header'] = run(row, [tools['dumpe2fs'], '-h', native])
        if digest(pending) != row['pending_sha256'] or digest(journal) != row['journal_sha256']:
            raise RuntimeError('Protected pending inputs changed')
        row.update(passed=True, native_sha256=digest(native))
        (output / 'report.json').write_text(json.dumps(rows, indent=2) + '\n')
        print(f'PASS {name}: native creation, {row["recovery"]}', flush=True)


if __name__ == '__main__':
    main()

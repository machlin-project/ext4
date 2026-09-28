#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
"""Exchange multi-mount-protected volumes between the core and the pinned Linux kernel.

Linux must acquire a volume the core released and one the core left with a pending
journal, waiting its check intervals and releasing clean. The core must then acquire
the Linux-released volume with the POSIX adapter's real sleeps and release it, and
Linux must acquire it again. Strict fsck follows every step."""
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
from generate_fixtures import resolve_tools
from generate_mmp_fixtures import MMP_SEQUENCE_CLEAN, mmp_fields

KERNEL = 'LINUX_MMP_KERNEL=Linux 6.12.94-0-virt aarch64'
PHASE_RELEASED, PHASE_CONTINUED, PHASE_PENDING = 0, 1, 2
PROFILES = ('4k', '1k', 'no-checksum')


def digest(path):
    with Path(path).open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--lab', type=Path, required=True)
    parser.add_argument('--prepared', type=Path, required=True)
    parser.add_argument('--runner', type=Path, required=True)
    parser.add_argument('--core-test', type=Path, required=True,
                        help='ext4-mmp-test built from the core under test')
    parser.add_argument('--exports', type=Path, required=True,
                        help='ext4-mmp-test export directory with released and pending images')
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    lab, prepared, runner, core_test, exports, output = (
        value.resolve() for value in (args.lab, args.prepared, args.runner, args.core_test,
                                      args.exports, args.output))
    if Path.cwd() != lab:
        parser.error('run with explicit lab working directory')
    previous = json.loads((prepared / 'report.json').read_text())
    kernel = prepared / 'Image'
    if not previous[0]['passed'] or digest(kernel) != previous[0]['kernel_sha256']:
        raise RuntimeError('Prepared Linux reference is not accepted')
    output.mkdir(parents=True, exist_ok=False)
    tools = resolve_tools(lab / 'vendor/e2fsprogs-ext4/build')
    rows = []

    def run(row, command, timeout=600):
        command = [str(part) for part in command]
        done = subprocess.run(command, cwd=lab, capture_output=True, text=True, timeout=timeout)
        row['commands'].append(dict(command=command, status=done.returncode,
                                    stdout=done.stdout[-8000:], stderr=done.stderr[-4000:]))
        (output / 'report.json').write_text(json.dumps(rows, indent=2) + '\n')
        if done.returncode:
            raise RuntimeError(f'Command failed: {command}: {done.stderr}')
        return done.stdout

    prep = dict(kind='preparation', commands=[], kernel_sha256=digest(kernel),
                runner_sha256=digest(runner), core_test_sha256=digest(core_test))
    rows.append(prep)
    run(prep, ['/usr/bin/codesign', '--verify', '--strict', runner])
    compile_command = previous[0]['commands'][1]['command'].copy()
    source_index = compile_command.index(str(ROOT / 'tests/linux_external_journal.c'))
    source = output / 'probe-source.c'
    shutil.copyfile(ROOT / 'tests/linux_mmp.c', source)
    compile_command[source_index] = str(source)
    compile_command[-1] = str(output / 'init')
    run(prep, compile_command)
    prep.update(passed=True, probe_sha256=digest(output / 'init'))

    def boot(row, label, disk, phase):
        tree = output / f'root-{label}'
        shutil.copytree(prepared / 'root-0', tree)
        shutil.copy2(output / 'init', tree / 'init')
        (tree / 'phase').write_text(f'{phase}\n')
        archive = output / f'{label}.cpio'
        listing = '\n'.join(str(x.relative_to(tree)) for x in sorted(tree.rglob('*'))) + '\n'
        with archive.open('wb') as stream:
            subprocess.run(['/usr/bin/cpio', '-o', '-H', 'newc'], cwd=tree,
                           input=listing.encode(), stdout=stream, check=True)
        console = run(row, [runner, kernel, archive, 2, 512,
                            'console=hvc0 rdinit=/init panic=-1 loglevel=4', disk])
        (output / f'{label}.console.log').write_text(console)
        shutil.rmtree(tree)
        archive.unlink()
        for required in (KERNEL, 'LINUX_MMP_RESULT=PASS'):
            if required not in console:
                raise RuntimeError(f'{label}: missing Linux MMP evidence: {required}')
        return (re.search(r'LINUX_MMP_NODE=(.*)', console)[1].strip(),
                int(re.search(r'LINUX_MMP_MOUNT_SECONDS=(\d+)', console)[1]))

    def released(row, disk, node, interval):
        run(row, [tools['e2fsck'], '-fn', disk])
        text = run(row, [tools['debugfs'], '-R', 'dump_mmp', disk])
        fields = mmp_fields(text)
        owner = re.search(r'^node_name:\s*(.*)$', text, re.M)[1].strip()
        if fields['sequence'] != MMP_SEQUENCE_CLEAN or owner != node[:len(owner)] or \
                not owner or fields['interval'] != interval:
            raise RuntimeError(f'{disk.name}: MMP not released clean by {node}: {text}')

    for profile in PROFILES:
        row = dict(profile=profile, commands=[], passed=False)
        rows.append(row)
        source = exports / f'mmp-released-mmp-{profile}.img'
        pending = exports / f'mmp-pending-mmp-{profile}.img'
        interval = mmp_fields(run(row, [tools['debugfs'], '-R', 'dump_mmp', source]))['interval']
        disk = output / f'{profile}.img'
        shutil.copyfile(source, disk)
        # The reference VM's monotonic clock does not advance across the kernel's
        # MMP sleeps, so the elapsed time is recorded but cannot prove the wait.
        node, seconds = boot(row, f'{profile}-released', disk, PHASE_RELEASED)
        released(row, disk, node, interval)
        core = run(row, [core_test, '--continue', disk], timeout=900)
        core_node = re.search(r'acquired after Linux by (.*)', core)[1].strip()
        released(row, disk, core_node, interval)
        boot(row, f'{profile}-continued', disk, PHASE_CONTINUED)
        released(row, disk, node, interval)
        shutil.copyfile(pending, disk)
        _, pending_seconds = boot(row, f'{profile}-pending', disk, PHASE_PENDING)
        released(row, disk, node, interval)
        disk.unlink()
        row.update(passed=True, interval=interval, linux_node=node, core_node=core_node,
                   released_seconds=seconds, pending_seconds=pending_seconds)
        (output / 'report.json').write_text(json.dumps(rows, indent=2) + '\n')
        print(f'PASS Linux MMP {profile}: Linux released as {node}, the core re-acquired '
              f'as {core_node} and Linux took over a pending volume', flush=True)


if __name__ == '__main__':
    main()

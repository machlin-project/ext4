#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
"""Exchange quota-tracking images between the core and the pinned Linux kernel.

For each core export: Linux reports usage through quotactl, which must equal the
independent e2fsprogs reading; Linux then removes, creates and moves files, which
frees and reuses quota entries; strict e2fsck and the e2fsprogs reading must agree
with Linux; the core continues on that image; and Linux reads the result again."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil
import struct
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tests'))
from generate_fixtures import resolve_tools
from squashfs_extract import Squashfs

KERNEL = 'LINUX_QUOTA_KERNEL=Linux 6.12.94-0-virt aarch64'
# Alpine 3.22.5 netboot modloop matching the pinned vmlinuz-virt and initramfs.
MODLOOP_SHA256 = '65a50040ab5129e6c1875353a8d8d91e695eb7f5fc2ba5a36809bd21539ab810'
MODULES = ('quota_tree', 'quota_v2')
MODULE_DIRECTORY = 'modules/6.12.94-0-virt/kernel/fs/quota'
TYPES = ('user', 'group', 'project')
PHASE_REPORT, PHASE_MUTATE, PHASE_ENFORCE = 0, 1, 2
# The owner whose limits ext4-quota-test --enforce wrote.
ENFORCED_OWNER = 1000
QUOTA_LIMIT_BLOCK = 1024
BLOCK_BYTES = 4096
LINUX_EDQUOT = 122
# Quota header: magic, version, block grace, inode grace, flags, then blocks.
QUOTA_INFO_BLOCKS = 20


def digest(path):
    with Path(path).open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def e2fsprogs_usage(run, row, tools, image, types):
    usage = set()
    for index, name in enumerate(TYPES[:types]):
        text = run(row, [tools['debugfs'], '-R', f'list_quota {name}', image])
        for line in text.splitlines():
            fields = line.split()
            if len(fields) == 7 and fields[0].isdigit():
                space, inodes = int(fields[1]), int(fields[4])
                if space or inodes:
                    usage.add((index, int(fields[0]), space, inodes))
    return usage


def quota_headers(run, row, tools, image, types):
    """Blocks, free-block head and free-entry head of each quota file."""
    headers = {}
    state = run(row, [tools['dumpe2fs'], '-h', image])
    for index, name in enumerate(TYPES[:types]):
        number = re.search(rf'{name.capitalize()} quota inode:\s+(\d+)', state)[1]
        dump = subprocess.run([str(tools['debugfs']), '-R', f'cat <{number}>', str(image)],
                              capture_output=True, timeout=300, check=True).stdout
        headers[name] = dict(zip(('blocks', 'free_block', 'free_entry'),
                                 struct.unpack_from('<III', dump, QUOTA_INFO_BLOCKS)))
    return headers


def linux_usage(console):
    return {tuple(int(part) for part in match.split())
            for match in re.findall(r'LINUX_QUOTA=([0-9 ]+)', console)}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--lab', type=Path, required=True)
    parser.add_argument('--prepared', type=Path, required=True)
    parser.add_argument('--runner', type=Path, required=True)
    parser.add_argument('--modloop', type=Path, required=True)
    parser.add_argument('--core-test', type=Path, required=True,
                        help='ext4-quota-test built from the core under test')
    parser.add_argument('--exports', type=Path, required=True, action='append')
    parser.add_argument('--enforced-exports', type=Path, action='append', default=[],
                        help='ext4-quota-test --enforce export directories')
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    lab, prepared, runner, modloop, core_test, output = (
        value.resolve() for value in (args.lab, args.prepared, args.runner, args.modloop,
                                      args.core_test, args.output))
    if Path.cwd() != lab:
        parser.error('run with explicit lab working directory')
    previous = json.loads((prepared / 'report.json').read_text())
    kernel = prepared / 'Image'
    if not previous[0]['passed'] or digest(kernel) != previous[0]['kernel_sha256']:
        raise RuntimeError('Prepared Linux reference is not accepted')
    if digest(modloop) != MODLOOP_SHA256:
        raise RuntimeError('Module image does not match the pinned kernel')
    output.mkdir(parents=True, exist_ok=False)
    tools = resolve_tools(lab / 'vendor/e2fsprogs-ext4/build')
    rows = []

    def run(row, command, timeout=300):
        command = [str(part) for part in command]
        done = subprocess.run(command, cwd=lab, capture_output=True, text=True, timeout=timeout)
        row['commands'].append(dict(command=command, status=done.returncode,
                                    stdout=done.stdout[-20000:], stderr=done.stderr[-4000:]))
        (output / 'report.json').write_text(json.dumps(rows, indent=2) + '\n')
        if done.returncode:
            raise RuntimeError(f'Command failed: {command}: {done.stderr}')
        return done.stdout

    prep = dict(kind='preparation', commands=[], kernel_sha256=digest(kernel),
                runner_sha256=digest(runner), modloop_sha256=MODLOOP_SHA256,
                core_test_sha256=digest(core_test))
    rows.append(prep)
    run(prep, ['/usr/bin/codesign', '--verify', '--strict', runner])
    compile_command = previous[0]['commands'][1]['command'].copy()
    source_index = compile_command.index(str(ROOT / 'tests/linux_external_journal.c'))
    source = output / 'probe-source.c'
    shutil.copyfile(ROOT / 'tests/linux_quota.c', source)
    compile_command[source_index] = str(source)
    compile_command[-1] = str(output / 'init')
    run(prep, compile_command)
    modules = output / 'modules'
    modules.mkdir()
    image = Squashfs(modloop)
    for name in MODULES:
        (modules / f'{name}.ko').write_bytes(image.read(f'{MODULE_DIRECTORY}/{name}.ko'))
    prep.update(passed=True, probe_sha256=digest(output / 'init'),
                modules={name: digest(modules / f'{name}.ko') for name in MODULES})

    def boot(row, label, disk, phase, project):
        tree = output / f'root-{label}'
        shutil.copytree(prepared / 'root-0', tree)
        shutil.copy2(output / 'init', tree / 'init')
        for name in MODULES:
            shutil.copy2(modules / f'{name}.ko', tree / 'modules' / f'{name}.ko')
        (tree / 'phase').write_text(f'{phase} {int(project)}\n')
        archive = output / f'{label}.cpio'
        listing = '\n'.join(str(x.relative_to(tree)) for x in sorted(tree.rglob('*'))) + '\n'
        with archive.open('wb') as stream:
            subprocess.run(['/usr/bin/cpio', '-o', '-H', 'newc'], cwd=tree,
                           input=listing.encode(), stdout=stream, check=True)
        console = run(row, [runner, kernel, archive, 2, 512,
                            'console=hvc0 rdinit=/init panic=-1 loglevel=4', disk])
        (output / f'{label}.console.log').write_text(console)
        for required in (KERNEL, 'LINUX_QUOTA_RESULT=PASS'):
            if required not in console:
                raise RuntimeError(f'{label}: missing Linux quota evidence: {required}')
        shutil.rmtree(tree)
        archive.unlink()
        if phase == PHASE_ENFORCE:
            return console
        return linux_usage(console)

    def agree(row, label, disk, project):
        types = 3 if project else 2
        expected = e2fsprogs_usage(run, row, tools, disk, types)
        seen = boot(row, label, disk, PHASE_REPORT, project)
        run(row, [tools['e2fsck'], '-fn', disk])
        if seen != expected:
            raise RuntimeError(f'{label}: Linux usage differs from e2fsprogs: '
                               f'{sorted(seen ^ expected)}')
        return len(seen)

    for directory in args.exports:
        exports = sorted(directory.glob('quota-mutated-*.img'))
        if len(exports) != 1:
            raise RuntimeError(f'{directory}: expected one quota export')
        source_image = exports[0]
        profile = directory.name
        row = dict(profile=profile, commands=[], passed=False,
                   image_sha256=digest(source_image))
        rows.append(row)
        state = run(row, [tools['dumpe2fs'], '-h', source_image])
        project = 'project' in re.search(r'Filesystem features:(.*)', state)[1].split()
        copy = output / f'{profile}-core.img'
        shutil.copyfile(source_image, copy)
        row['core_entries'] = agree(row, f'{profile}-core', copy, project)
        linux = output / f'{profile}-linux.img'
        shutil.copyfile(source_image, linux)
        seen = boot(row, f'{profile}-linux', linux, PHASE_MUTATE, project)
        run(row, [tools['e2fsck'], '-fn', linux])
        expected = e2fsprogs_usage(run, row, tools, linux, 3 if project else 2)
        if seen != expected:
            raise RuntimeError(f'{profile}: Linux changes differ from e2fsprogs: '
                               f'{sorted(seen ^ expected)}')
        row['linux_entries'] = len(seen)
        row['linux_headers'] = quota_headers(run, row, tools, linux, 3 if project else 2)
        if not any(header['free_block'] or header['free_entry']
                   for header in row['linux_headers'].values()):
            raise RuntimeError(f'{profile}: Linux left no freed quota entries or blocks')
        continued = output / f'{profile}-continued'
        continued.mkdir()
        run(row, [core_test, '--continue', linux, continued])
        result = next(continued.glob('quota-continued-*.img'))
        run(row, [tools['e2fsck'], '-fn', result])
        row['continued_entries'] = agree(row, f'{profile}-continued', result, project)
        for image_path in (copy, linux, result):
            image_path.unlink()
        row['passed'] = True
        (output / 'report.json').write_text(json.dumps(rows, indent=2) + '\n')
        print(f'PASS Linux quota {profile}: {row["core_entries"]} core, '
              f'{row["linux_entries"]} Linux and {row["continued_entries"]} continued '
              'entries agree', flush=True)

    for directory in args.enforced_exports:
        exports = sorted(directory.glob('quota-enforced-*.img'))
        if len(exports) != 1:
            raise RuntimeError(f'{directory}: expected one enforced export')
        source_image = exports[0]
        profile = f'{directory.name}-enforced'
        row = dict(profile=profile, commands=[], passed=False,
                   image_sha256=digest(source_image))
        rows.append(row)
        state = run(row, [tools['dumpe2fs'], '-h', source_image])
        project = 'project' in re.search(r'Filesystem features:(.*)', state)[1].split()
        fields = run(row, [tools['debugfs'], '-R', f'get_quota user {ENFORCED_OWNER}',
                           source_image]).splitlines()[-1].split()
        space, soft, hard, inodes, inode_soft, inode_hard = (int(value) for value in fields[1:7])
        if not hard or not inode_hard or inodes + 1 != inode_hard:
            raise RuntimeError(f'{profile}: unexpected enforced limits {fields}')
        # The probe first creates the owner's directory, charging one block.
        expected = (hard * QUOTA_LIMIT_BLOCK - space - BLOCK_BYTES) // BLOCK_BYTES * BLOCK_BYTES
        disk = output / f'{profile}.img'
        shutil.copyfile(source_image, disk)
        console = boot(row, profile, disk, PHASE_ENFORCE, project)
        written = int(re.search(r'LINUX_QUOTA_ENFORCE_BYTES=(\d+)', console)[1])
        write_error = int(re.search(r'LINUX_QUOTA_ENFORCE_WRITE_ERROR=(\d+)', console)[1])
        create_error = int(re.search(r'LINUX_QUOTA_ENFORCE_CREATE_ERROR=(\d+)', console)[1])
        if written != expected or write_error != LINUX_EDQUOT or create_error != LINUX_EDQUOT:
            raise RuntimeError(f'{profile}: Linux refused after {written} bytes with '
                               f'{write_error} and {create_error}; expected {expected} '
                               f'bytes and EDQUOT')
        run(row, [tools['e2fsck'], '-fn', disk])
        seen = linux_usage(console)
        if seen != e2fsprogs_usage(run, row, tools, disk, 3 if project else 2):
            raise RuntimeError(f'{profile}: Linux usage differs from e2fsprogs')
        disk.unlink()
        row.update(passed=True, space=space, soft_kib=soft, hard_kib=hard, inodes=inodes,
                   inode_hard=inode_hard, linux_bytes=written)
        (output / 'report.json').write_text(json.dumps(rows, indent=2) + '\n')
        print(f'PASS Linux quota {profile}: Linux refuses after {written} bytes and at '
              f'{inode_hard} inodes, as the core\'s limits require', flush=True)


if __name__ == '__main__':
    main()

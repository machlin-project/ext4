#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
"""Recover volumes whose journal replay by the pinned Linux kernel was interrupted.

Linux mounts each pending volume through a dm-log-writes target, which records every
write, flush and FUA write of its journal and fast-commit replay, the rest of the
mount and the clean unmount, in completion order. Replaying the complete log onto the
pending image must reproduce Linux's result exactly. Power-cut states follow the
device's volatile cache: a flush makes every earlier completed write durable, a FUA
write is durable when it completes, and any subset of the other completed writes
may have reached the medium, exhaustively for up to ten blocks and otherwise as
every prefix and every single omission.

While the journal still holds its log, the core must recover each state, strict
fsck must accept the result, and independently read names, data and attributes
must equal the core's recovery of the untouched volume, which must itself equal
Linux's result. Linux places replayed names by its own algorithm, so blocks are not
compared. Once Linux has emptied the log it continues with unjournaled orphan
release and superblock updates; the core may refuse such a state or leave what
strict fsck reports, as mount-time recovery does not audit every bitmap, and the
complete state must recover cleanly."""
import argparse
import hashlib
import itertools
import json
from pathlib import Path
import re
import shutil
import struct
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tests'))
from fast_commit_reference import read_namespace
from generate_fixtures import resolve_tools
from squashfs_extract import Squashfs

KERNEL = 'LINUX_LOG_WRITES_KERNEL=Linux 6.12.94-0-virt aarch64'
# Alpine 3.22.5 netboot modloop matching the pinned vmlinuz-virt and initramfs.
MODLOOP_SHA256 = '65a50040ab5129e6c1875353a8d8d91e695eb7f5fc2ba5a36809bd21539ab810'
MODULES = ('dm-mod', 'dm-log-writes')
MODULE_DIRECTORY = 'modules/6.12.94-0-virt/kernel/drivers/md'
LOG_BYTES = 512 * 1024 * 1024
# dm-log-writes: a header sector, then per entry one sector and its data sectors.
LOG_MAGIC = 0x6a736677736872
LOG_VERSION = 1
LOG_SUPER = struct.Struct('<QQQI')
LOG_ENTRY = struct.Struct('<QQQQ')
LOG_FLUSH = 1 << 0
LOG_FUA = 1 << 1
LOG_DISCARD = 1 << 2
LOG_MARK = 1 << 3
EXHAUSTIVE_EPOCH = 10
SUPERBLOCK_OFFSET = 1024
LOG_BLOCK_SIZE_OFFSET = 24
MINIMUM_BLOCK_SIZE = 1024
# jbd2 superblock: s_start, big-endian, is zero once the log holds nothing to replay.
JOURNAL_START = struct.Struct('>I')
JOURNAL_START_OFFSET = 28
FAILED_SAMPLES = 8
NAMESPACE_KEYS = ('files', 'directories', 'symlinks', 'special', 'xattrs')


def digest(path):
    with Path(path).open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def read_log(path):
    """Return (flags, byte offset, data) for every logged operation, in order."""
    with Path(path).open('rb') as stream:
        magic, version, count, sector = LOG_SUPER.unpack(stream.read(LOG_SUPER.size))
        if magic != LOG_MAGIC or version != LOG_VERSION or sector < 512 or \
                sector & (sector - 1):
            raise RuntimeError(f'{path}: not a dm-log-writes log')
        entries = []
        position = sector
        for _ in range(count):
            stream.seek(position)
            target, sectors, flags, length = LOG_ENTRY.unpack(stream.read(LOG_ENTRY.size))
            position += sector
            data = b''
            if not flags & (LOG_DISCARD | LOG_MARK) and sectors:
                stream.seek(position)
                data = stream.read(sectors * sector)
                if len(data) != sectors * sector:
                    raise RuntimeError(f'{path}: truncated log entry')
                position += sectors * sector
            if flags & LOG_DISCARD and sectors:
                raise RuntimeError(f'{path}: unexpected discard')
            entries.append((flags, target * sector, data))
    return entries


def split(offset, data, block, contents):
    """Merge one write into whole filesystem blocks read through contents()."""
    blocks = {}
    position = offset
    while position < offset + len(data):
        number = position // block
        start = position - number * block
        take = min(block - start, offset + len(data) - position)
        value = bytearray(blocks.get(number) or contents(number))
        value[start:start + take] = data[position - offset:position - offset + take]
        blocks[number] = bytes(value)
        position += take
    return blocks


def subsets(numbers):
    if len(numbers) <= EXHAUSTIVE_EPOCH:
        return [subset for size in range(len(numbers) + 1)
                for subset in itertools.combinations(numbers, size)]
    return ([tuple(numbers[:count]) for count in range(len(numbers) + 1)] +
            [tuple(number for number in numbers if number != omitted)
             for omitted in numbers])


def crash_states(original, entries, block):
    """Yield (label, blocks) for every modeled power cut, then the complete state."""
    durable = {}
    dirty = {}

    def contents(number):
        return dirty.get(number) or durable.get(number) or \
            original[number * block:(number + 1) * block]

    for index, (flags, offset, data) in enumerate(entries):
        if flags & (LOG_FLUSH | LOG_FUA):
            numbers = list(dirty)
            for subset in subsets(numbers):
                state = dict(durable)
                state.update((number, dirty[number]) for number in subset)
                yield f'entry-{index}-{len(subset)}-of-{len(numbers)}', state
        if flags & LOG_FLUSH:
            durable.update(dirty)
            dirty.clear()
        if data:
            written = split(offset, data, block, contents)
            if flags & LOG_FUA:
                durable.update(written)
                for number in written:
                    dirty.pop(number, None)
            else:
                dirty.update(written)
    durable.update(dirty)
    yield 'complete', durable


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--lab', type=Path, required=True)
    parser.add_argument('--prepared', type=Path, required=True)
    parser.add_argument('--runner', type=Path, required=True,
                        help='reference runner accepting two disks')
    parser.add_argument('--modloop', type=Path, required=True)
    parser.add_argument('--recover', type=Path, required=True)
    parser.add_argument('--image', type=Path, action='append', required=True,
                        help='pending volume; its two enclosing directories name the case')
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    lab, prepared, runner, modloop, recover, output = (
        value.resolve() for value in (args.lab, args.prepared, args.runner, args.modloop,
                                      args.recover, args.output))
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

    def run(row, command, allowed=(0,), timeout=600):
        command = [str(part) for part in command]
        done = subprocess.run(command, cwd=lab, capture_output=True, text=True,
                              timeout=timeout)
        row['commands'].append(dict(command=command, status=done.returncode,
                                    stdout=done.stdout[-8000:], stderr=done.stderr[-4000:]))
        if done.returncode not in allowed:
            (output / 'report.json').write_text(json.dumps(rows, indent=2) + '\n')
            raise RuntimeError(f'Command failed ({done.returncode}): {command}: '
                               f'{done.stdout[-1000:]}{done.stderr[-2000:]}')
        return done.stdout

    prep = dict(kind='preparation', commands=[], kernel_sha256=digest(kernel),
                runner_sha256=digest(runner), modloop_sha256=MODLOOP_SHA256,
                recover_sha256=digest(recover))
    rows.append(prep)
    run(prep, ['/usr/bin/codesign', '--verify', '--strict', runner])
    compile_command = previous[0]['commands'][1]['command'].copy()
    source_index = compile_command.index(str(ROOT / 'tests/linux_external_journal.c'))
    source = output / 'probe-source.c'
    shutil.copyfile(ROOT / 'tests/linux_log_writes.c', source)
    compile_command[source_index] = str(source)
    compile_command[-1] = str(output / 'init')
    run(prep, compile_command)
    modules = output / 'modules'
    modules.mkdir()
    squashfs = Squashfs(modloop)
    for name in MODULES:
        (modules / f'{name}.ko').write_bytes(squashfs.read(f'{MODULE_DIRECTORY}/{name}.ko'))
    tree = output / 'root'
    shutil.copytree(prepared / 'root-0', tree)
    shutil.copy2(output / 'init', tree / 'init')
    for name in MODULES:
        shutil.copy2(modules / f'{name}.ko', tree / 'modules' / f'{name}.ko')
    archive = output / 'probe.cpio'
    listing = '\n'.join(str(x.relative_to(tree)) for x in sorted(tree.rglob('*'))) + '\n'
    with archive.open('wb') as stream:
        subprocess.run(['/usr/bin/cpio', '-o', '-H', 'newc'], cwd=tree, input=listing.encode(),
                       stdout=stream, check=True)
    shutil.rmtree(tree)
    prep.update(passed=True, probe_sha256=digest(output / 'init'),
                archive_sha256=digest(archive),
                modules={name: digest(modules / f'{name}.ko') for name in MODULES})

    failed = []
    for source_image in (image.resolve() for image in args.image):
        name = f'{source_image.parent.parent.name}-{source_image.parent.name}'
        directory = output / name
        directory.mkdir()
        row = dict(name=name, source=str(source_image), source_sha256=digest(source_image),
                   commands=[], passed=False, states=0, recovered_images=0, post_replay=0,
                   refused=0, left_for_fsck=0, post_replay_clean=0)
        rows.append(row)
        with source_image.open('rb') as stream:
            stream.seek(SUPERBLOCK_OFFSET + LOG_BLOCK_SIZE_OFFSET)
            block = MINIMUM_BLOCK_SIZE << struct.unpack('<I', stream.read(4))[0]
        journal_super = int(run(row, [tools['debugfs'], '-R', 'bmap <8> 0',
                                      source_image]).split()[-1])

        def namespace(image, label):
            exported = directory / f'files-{label}'
            exported.mkdir()
            result = read_namespace(image, exported, block, tools['debugfs'],
                                    lambda command: run(row, command))
            shutil.rmtree(exported)
            return {key: result[key] for key in NAMESPACE_KEYS}

        reference = directory / 'core-recovered.img'
        shutil.copyfile(source_image, reference)
        run(row, [recover, '--write', reference])
        run(row, [tools['e2fsck'], '-fn', reference])
        expected = namespace(reference, 'core')
        reference.unlink()
        disk = directory / 'linux.img'
        log = directory / 'log.img'
        shutil.copyfile(source_image, disk)
        with log.open('wb') as stream:
            stream.truncate(LOG_BYTES)
        console = run(row, [runner, kernel, archive, 2, 512,
                            'console=hvc0 rdinit=/init panic=-1 loglevel=4', disk, log],
                      timeout=900)
        (directory / 'console.log').write_text(console)
        for required in (KERNEL, 'LINUX_LOG_WRITES_MOUNTED=1', 'LINUX_LOG_WRITES_RESULT=PASS'):
            if required not in console:
                raise RuntimeError(f'{name}: missing Linux log-writes evidence: {required}')
        row['kernel_log'] = re.findall(r'LINUX_LOG_WRITES_KERNEL_LOG=(.*)', console)
        run(row, [tools['e2fsck'], '-fn', disk])
        if namespace(disk, 'linux') != expected:
            raise RuntimeError(f'{name}: Linux and core recovery read differently')
        entries = read_log(log)
        row['entries'] = len(entries)
        row['writes'] = sum(1 for flags, _, data in entries if data)
        row['flushes'] = sum(1 for flags, _, _ in entries if flags & LOG_FLUSH)
        row['fua_writes'] = sum(1 for flags, _, data in entries if data and flags & LOG_FUA)
        original = source_image.read_bytes()
        final = disk.read_bytes()
        replayed = bytearray(original)
        for _, offset, data in entries:
            replayed[offset:offset + len(data)] = data
        if bytes(replayed) != final:
            raise RuntimeError(f'{name}: the complete log does not reproduce Linux\'s result')
        log.unlink()
        disk.unlink()
        state_path = directory / 'state.img'
        seen = set()
        checked = {}
        failures = []
        for label, blocks in crash_states(original, entries, block):
            row['commands'] = row['commands'][-8:]
            image = bytearray(original)
            for number, data in blocks.items():
                image[number * block:(number + 1) * block] = data
            key = hashlib.sha256(image).digest()
            if key in seen:
                continue
            seen.add(key)
            state_path.write_bytes(image)
            replaying = JOURNAL_START.unpack_from(
                image, journal_super * block + JOURNAL_START_OFFSET)[0] != 0
            row['state'] = label
            complete = label == 'complete'
            if not replaying:
                row['post_replay'] += 1
                run(row, [recover, '--write', state_path], allowed=(0,) if complete else (0, 1))
                if row['commands'][-1]['status'] != 0:
                    row['refused'] += 1
                    continue
                run(row, [tools['e2fsck'], '-fn', state_path],
                    allowed=(0,) if complete else (0, 4))
                if row['commands'][-1]['status'] != 0:
                    row['left_for_fsck'] += 1
                    continue
                row['post_replay_clean'] += 1
                if not complete:
                    continue
            else:
                row['states'] += 1
                run(row, [recover, '--write', state_path], allowed=(0, 1))
                if row['commands'][-1]['status'] != 0:
                    failures.append(dict(state=label, failure='refused',
                                         output=row['commands'][-1]['stdout'].strip()))
                    if len(failures) <= FAILED_SAMPLES:
                        shutil.copyfile(state_path, directory / f'failed-{len(failures):03d}.img')
                    continue
                run(row, [tools['e2fsck'], '-fn', state_path], allowed=(0, 4))
                if row['commands'][-1]['status'] != 0:
                    failures.append(dict(state=label, failure='strict fsck',
                                         output=row['commands'][-1]['stdout'][-2000:]))
                    continue
            recovered = digest(state_path)
            if recovered not in checked:
                checked[recovered] = namespace(state_path, f'{len(checked):04d}') == expected
                row['recovered_images'] = len(checked)
            if not checked[recovered]:
                failures.append(dict(state=label, failure='namespace or data'))
        state_path.unlink(missing_ok=True)
        row['failures'] = failures
        (output / 'report.json').write_text(json.dumps(rows, indent=2) + '\n')
        if failures:
            failed.append(name)
            print(f'FAIL interrupted Linux replay {name}: {len(failures)} of {row["states"]} '
                  f'states while the log was pending: '
                  f'{sorted(set(entry["failure"] for entry in failures))}', flush=True)
            continue
        row['passed'] = True
        (output / 'report.json').write_text(json.dumps(rows, indent=2) + '\n')
        print(f'PASS interrupted Linux replay {name}: {row["writes"]} writes, '
              f'{row["flushes"]} flushes, {row["fua_writes"]} FUA writes, '
              f'{row["states"]} recovered states ({row["recovered_images"]} distinct results), '
              f'{row["post_replay"]} after replay: {row["post_replay_clean"]} clean, '
              f'{row["refused"]} refused, {row["left_for_fsck"]} left for fsck', flush=True)
    if failed:
        raise SystemExit(f'Interrupted Linux replay failed for {", ".join(failed)}')


if __name__ == '__main__':
    main()

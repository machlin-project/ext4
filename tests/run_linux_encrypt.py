#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
"""Create fscrypt content with Linux, or verify it after the core changed a copy."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tests'))
from generate_fixtures import EXPECTED_FEATURES, UUID, resolve_tools

KERNEL = 'LINUX_ENCRYPT_KERNEL=Linux 6.12.94-0-virt aarch64'
IMAGE_BYTES = 32 * 1024 * 1024
BLOCK_SIZE = 4096
PHASES = {'create': (0, 'LINUX_ENCRYPT_CREATED'), 'verify': (1, 'LINUX_ENCRYPT_VERIFIED')}


def digest(path):
    with Path(path).open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--lab', type=Path, required=True)
    parser.add_argument('--prepared', type=Path, required=True)
    parser.add_argument('--runner', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    mode = parser.add_mutually_exclusive_group(required=True)
    mode.add_argument('--create', action='store_true',
                      help='create a new image with Linux-encrypted content')
    mode.add_argument('--verify', type=Path, help='verify an image changed by the core')
    args = parser.parse_args()
    lab, prepared, runner, output = (
        value.resolve() for value in (args.lab, args.prepared, args.runner, args.output))
    if Path.cwd() != lab:
        parser.error('run with explicit lab working directory')
    previous = json.loads((prepared / 'report.json').read_text())
    kernel = prepared / 'Image'
    if not previous[0]['passed'] or digest(kernel) != previous[0]['kernel_sha256']:
        raise RuntimeError('Prepared Linux reference is not accepted')
    output.mkdir(parents=True, exist_ok=False)
    tools = resolve_tools(lab / 'vendor/e2fsprogs-ext4/build')
    rows = []

    def run(row, command, timeout=300):
        command = [str(part) for part in command]
        done = subprocess.run(command, cwd=lab, capture_output=True, text=True, timeout=timeout)
        row['commands'].append(dict(command=command, status=done.returncode,
                                    stdout=done.stdout, stderr=done.stderr))
        (output / 'report.json').write_text(json.dumps(rows, indent=2) + '\n')
        if done.returncode:
            raise RuntimeError(f'Command failed: {command}: {done.stderr}')
        return done.stdout

    prep = dict(kind='preparation', commands=[], kernel_sha256=digest(kernel),
                runner_sha256=digest(runner))
    rows.append(prep)
    run(prep, ['/usr/bin/codesign', '--verify', '--strict', runner])
    compile_command = previous[0]['commands'][1]['command'].copy()
    source_index = compile_command.index(str(ROOT / 'tests/linux_external_journal.c'))
    source = output / 'probe-source.c'
    shutil.copyfile(ROOT / 'tests/linux_encrypt.c', source)
    compile_command[source_index] = str(source)
    compile_command[-1] = str(output / 'init')
    run(prep, compile_command)
    prep.update(passed=True, probe_sha256=digest(output / 'init'),
                probe_source_sha256=digest(source))
    name = 'create' if args.create else 'verify'
    phase, marker = PHASES[name]
    row = dict(kind=name, commands=[], passed=False)
    rows.append(row)
    image = output / 'encrypt.img'
    if args.create:
        features = EXPECTED_FEATURES | {'encrypt'}
        run(row, [tools['mke2fs'], '-F', '-t', 'ext4', '-b', BLOCK_SIZE, '-N', 256, '-I', 256,
                  '-m', 0, '-O', 'none,' + ','.join(sorted(features)), '-U', UUID,
                  '-E', 'lazy_itable_init=0,nodiscard', image, IMAGE_BYTES // BLOCK_SIZE])
    else:
        shutil.copyfile(args.verify, image)
        row['input_sha256'] = digest(args.verify)
    tree = output / 'root'
    shutil.copytree(prepared / 'root-0', tree)
    shutil.copy2(output / 'init', tree / 'init')
    (tree / 'phase').write_text(f'{phase}\n')
    archive = output / 'probe.cpio'
    listing = '\n'.join(str(x.relative_to(tree)) for x in sorted(tree.rglob('*'))) + '\n'
    with archive.open('wb') as stream:
        subprocess.run(['/usr/bin/cpio', '-o', '-H', 'newc'], cwd=tree, input=listing.encode(),
                       stdout=stream, check=True)
    console = run(row, [runner, kernel, archive, 2, 512,
                        'console=hvc0 rdinit=/init panic=-1 loglevel=4', image])
    (output / 'console.log').write_text(console)
    for required in (KERNEL, marker, 'LINUX_ENCRYPT_RESULT=PASS'):
        if required not in console:
            raise RuntimeError(f'Missing Linux encryption evidence: {required}')
    for forbidden in ('EXT4-fs error', 'Aborting journal'):
        if forbidden in console:
            raise RuntimeError(f'Guest filesystem error: {forbidden}')
    run(row, [tools['e2fsck'], '-fn', image])
    row.update(passed=True, image_sha256=digest(image))
    (output / 'report.json').write_text(json.dumps(rows, indent=2) + '\n')
    print(f'PASS Linux encryption {name}: {image}', flush=True)


if __name__ == '__main__':
    main()

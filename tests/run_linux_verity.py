#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
"""Let the pinned Linux kernel verify independently authored fs-verity fixtures."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tests'))
from generate_fixtures import resolve_tools

KERNEL = 'LINUX_VERITY_KERNEL=Linux 6.12.94-0-virt aarch64'
SUPERBLOCK_OFFSET = 1024
SUPERBLOCK_SIZE = 1024
# Linux records an invalid descriptor location in the superblock error fields,
# even on a read-only mount. That is the only permitted change.
DESCRIPTOR_ERROR = 'First error function:     ext4_get_verity_descriptor_locat'


def digest(path):
    with Path(path).open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--lab', type=Path, required=True)
    parser.add_argument('--prepared', type=Path, required=True)
    parser.add_argument('--runner', type=Path, required=True)
    parser.add_argument('--fixtures', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--certificate', type=Path,
                        help='DER certificate for the .fs-verity keyring')
    parser.add_argument('--require-signatures', action='store_true',
                        help='set fs.verity.require_signatures before mounting')
    args = parser.parse_args()
    if args.require_signatures and args.certificate is None:
        parser.error('--require-signatures needs --certificate')
    lab, prepared, runner, fixtures, output = (
        value.resolve() for value in (args.lab, args.prepared, args.runner, args.fixtures,
                                      args.output))
    if Path.cwd() != lab:
        parser.error('run with explicit lab working directory')
    previous = json.loads((prepared / 'report.json').read_text())
    kernel = prepared / 'Image'
    if not previous[0]['passed'] or digest(kernel) != previous[0]['kernel_sha256']:
        raise RuntimeError('Prepared Linux reference is not accepted')
    output.mkdir(parents=True, exist_ok=False)
    tools = resolve_tools(lab / 'vendor/e2fsprogs-ext4/build')
    source_rows = json.loads((fixtures / 'report.json').read_text())
    if not source_rows or not all(row['passed'] for row in source_rows):
        raise RuntimeError('Verity fixture generation is not accepted')
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
    shutil.copyfile(ROOT / 'tests/linux_verity.c', source)
    compile_command[source_index] = str(source)
    compile_command[-1] = str(output / 'init')
    run(prep, compile_command)
    prep.update(passed=True, probe_sha256=digest(output / 'init'),
                probe_source_sha256=digest(source))
    for fixture in source_rows:
        profile = fixture['profile']
        image = Path(fixture['image'])
        manifest = Path(fixture['manifest'])
        row = dict(profile=profile, commands=[], passed=False, image_sha256=digest(image))
        rows.append(row)
        tree = output / f'root-{profile}'
        shutil.copytree(prepared / 'root-0', tree)
        shutil.copy2(output / 'init', tree / 'init')
        shutil.copyfile(manifest, tree / 'manifest')
        if args.certificate is not None:
            shutil.copyfile(args.certificate, tree / 'cert.der')
        if args.require_signatures:
            (tree / 'require').write_text('')
        archive = output / f'{profile}.cpio'
        listing = '\n'.join(str(x.relative_to(tree)) for x in sorted(tree.rglob('*'))) + '\n'
        with archive.open('wb') as stream:
            subprocess.run(['/usr/bin/cpio', '-o', '-H', 'newc'], cwd=tree,
                           input=listing.encode(), stdout=stream, check=True)
        copy = output / f'{profile}.img'
        shutil.copyfile(image, copy)
        console = run(row, [runner, kernel, archive, 2, 512,
                            'console=hvc0 rdinit=/init panic=-1 loglevel=4', copy])
        (output / f'{profile}.console.log').write_text(console)
        files = len(manifest.read_text().splitlines()) - 1
        for required in (KERNEL, 'LINUX_VERITY_RESULT=PASS', f'LINUX_VERITY_FILES={files}'):
            if required not in console:
                raise RuntimeError(f'{profile}: missing Linux verity evidence: {required}')
        run(row, [tools['e2fsck'], '-fn', copy])
        changed = [index for index, (left, right) in
                   enumerate(zip(image.read_bytes(), copy.read_bytes())) if left != right]
        if changed:
            state = run(row, [tools['dumpe2fs'], '-h', copy])
            if (min(changed) < SUPERBLOCK_OFFSET or
                    max(changed) >= SUPERBLOCK_OFFSET + SUPERBLOCK_SIZE or
                    DESCRIPTOR_ERROR not in state):
                raise RuntimeError(f'{profile}: Linux changed more than its error record')
            row['linux_error_record'] = True
        copy.unlink()
        if args.certificate is not None and 'LINUX_VERITY_CERTIFICATE=trusted' not in console:
            raise RuntimeError(f'{profile}: certificate was not trusted')
        if args.require_signatures and 'LINUX_VERITY_SIGNATURES=required' not in console:
            raise RuntimeError(f'{profile}: signatures were not required')
        row.update(passed=True, files=files,
                   verified=console.count('LINUX_VERITY_VERIFIED='),
                   rejected=console.count('LINUX_VERITY_REJECTED='),
                   refused=console.count('LINUX_VERITY_REFUSED='))
        (output / 'report.json').write_text(json.dumps(rows, indent=2) + '\n')
        print(f'PASS Linux verity {profile}: {row["verified"]} verified, '
              f'{row["rejected"]} rejected, {row["refused"]} refused', flush=True)


if __name__ == '__main__':
    main()

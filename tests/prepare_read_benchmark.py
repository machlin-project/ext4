#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
"""Prepare a static same-guest core/Linux read benchmark; never boot the VM."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil
import subprocess

from generate_fixtures import FEATURE_LIST, resolve_tools

ROOT = Path(__file__).resolve().parents[1]
FILE_BYTES = 16 * 1024 * 1024
PAGE_BYTES = 4096
SEQUENTIAL_BYTES = 1024 * 1024


def digest(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def main(*, write=False):
    parser = argparse.ArgumentParser(description=(
        'Prepare a static same-guest core/Linux write benchmark; never boot the VM.'
        if write else __doc__))
    parser.add_argument('--lab', type=Path, required=True)
    parser.add_argument('--prepared', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--image', type=Path, help='Reuse an independently checked benchmark image')
    parser.add_argument('--backend-diagnostics', action='store_true',
                        help='Include demand-pread and mapped-I/O diagnostic controls')
    args = parser.parse_args()
    lab, prepared, output = (p.resolve() for p in (args.lab, args.prepared, args.output))
    if Path.cwd() != lab:
        parser.error('run from the absolute lab directory')
    output.mkdir(parents=True, exist_ok=False)
    report = dict(commands=[], source_revision=subprocess.check_output(
        ['git', '-C', str(ROOT), 'rev-parse', 'HEAD'], text=True).strip())

    def save():
        (output / 'preparation.json').write_text(json.dumps(report, indent=2) + '\n')

    def run(argv, **kwargs):
        command = [str(x) for x in argv]
        done = subprocess.run(command, capture_output=True, text=True, **kwargs)
        report['commands'].append(dict(command=command, status=done.returncode,
                                       stdout=done.stdout, stderr=done.stderr))
        save()
        if done.returncode:
            raise RuntimeError(f'command failed: {command}')
        return done.stdout

    tools = resolve_tools(lab / 'vendor/e2fsprogs-ext4/build')
    image = output / 'read.img'
    if args.image:
        shutil.copyfile(args.image.resolve(), image)
    else:
        tree = output / 'files'
        tree.mkdir()
        with (tree / 'contiguous.bin').open('xb') as dense, (tree / 'sparse.bin').open('xb') as sparse:
            for offset in range(0, FILE_BYTES, PAGE_BYTES):
                page = bytes((i * 131 + (offset // PAGE_BYTES) * 17 +
                              (offset // SEQUENTIAL_BYTES) * 29 + 7) & 255
                             for i in range(PAGE_BYTES))
                dense.write(page)
                sparse.write(page if (offset // PAGE_BYTES) % 2 == 0 else bytes(PAGE_BYTES))
        with image.open('xb') as stream:
            stream.truncate(128 * 1024 * 1024)
        run([tools['mke2fs'], '-q', '-t', 'ext4', '-F', '-b', '4096', '-I', '256',
             '-O', FEATURE_LIST, '-E', 'lazy_itable_init=0,lazy_journal_init=0', '-d', tree, image])
        report['expected_files'] = {p.name: digest(p) for p in tree.iterdir()}
    run([tools['e2fsck'], '-fn', image])
    for name in ('contiguous.bin', 'sparse.bin'):
        run([tools['debugfs'], '-R', f'stat /{name}', image])
    report['image_sha256'] = digest(image)
    previous = json.loads((prepared / 'report.json').read_text())
    kernel = prepared / 'Image'
    if not previous[0]['passed'] or digest(kernel) != previous[0]['kernel_sha256']:
        raise RuntimeError('unaccepted prepared kernel')
    command = previous[0]['commands'][1]['command'].copy()
    source_index = command.index(str(ROOT / 'tests/linux_external_journal.c'))
    core_sources = re.search(r'^core_sources = files\((.*?)^\)',
                             (ROOT / 'meson.build').read_text(), re.MULTILINE | re.DOTALL)
    if core_sources is None:
        raise RuntimeError('Meson core source list not found')
    sources = [ROOT / p for p in re.findall(r"'(core/[^']+\.c)'", core_sources.group(1))]
    probe = ROOT / ('tests/linux_write_benchmark.c' if write else 'tests/linux_read_benchmark.c')
    command[source_index:source_index + 1] = [str(probe)] + [str(p) for p in sources]
    command[1:1] = ['-march=armv8-a+crc', '-ffreestanding', '-fno-builtin', '-Wframe-larger-than=2048']
    if args.backend_diagnostics:
        command.insert(1, '-DEXT4_READ_BACKEND_DIAGNOSTICS')
    command[-1] = str(output / 'init')
    run(command, timeout=180)
    root = output / 'root'
    root.mkdir()
    for name in ('dev', 'mnt', 'proc'):
        (root / name).mkdir()
    shutil.copytree(prepared / 'root-0/modules', root / 'modules')
    shutil.copyfile(output / 'init', root / 'init')
    (root / 'init').chmod(0o755)
    archive = output / 'probe.cpio'
    listing = '\n'.join(str(p.relative_to(root)) for p in sorted(root.rglob('*'))) + '\n'
    with archive.open('wb') as stream:
        subprocess.run(['/usr/bin/cpio', '-o', '-H', 'newc'], cwd=root,
                       input=listing.encode(), stdout=stream, check=True)
    runner = lab / '.cache/linux-reference/linux-vm-external'
    run(['/usr/bin/codesign', '--verify', '--strict', runner])
    headers = sorted((ROOT / 'core').glob('*.h')) + sorted((ROOT / 'include').rglob('*.h'))
    disks = [str(image)]
    if write:
        core_image = output / 'core.img'
        shutil.copyfile(image, core_image)
        report['core_image_sha256'] = digest(core_image)
        disks.append(str(core_image))
    report.update(passed=True, mode='write' if write else 'read', kernel=str(kernel), kernel_sha256=digest(kernel),
                  probe_sha256=digest(output / 'init'), archive_sha256=digest(archive),
                  sources_sha256={str(p.relative_to(ROOT)): digest(p) for p in [probe] + sources + headers},
                  runner_command=[str(runner), str(kernel), str(archive), '2', '512',
                                  'console=hvc0 rdinit=/init panic=-1 loglevel=4', *disks])
    save()
    print(f'Prepared {output}; no VM was started')


if __name__ == '__main__':
    main()

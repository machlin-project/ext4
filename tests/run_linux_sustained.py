#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
"""Let the pinned Linux kernel verify a sustained-operation export read-only.

Without the key Linux must list exactly the exported no-key names of every
encrypted directory, reached through no-key names of encrypted ancestors. With the
Linux probe's key it must read every file's expected contents and every symlink's
target, find every object's type and measure every verity file's digest. The
kernel has no CONFIG_UNICODE, so casefolded exports are refused."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tests'))
from check_sustained import parse_manifest
from generate_fixtures import resolve_tools

KERNEL = 'LINUX_SUSTAINED_KERNEL=Linux 6.12.94-0-virt aarch64'
NOKEY_PREFIX = 'LINUX_SUSTAINED_NOKEY='


def digest(path):
    with Path(path).open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def guest_manifest(export, manifest):
    """Lines for the probe, and the no-key names each encrypted directory must list."""
    root, objects, _, features = parse_manifest(manifest)
    # Every name of an encrypted directory, hard links included, by entry inode.
    raw_nokey = {}
    for line in manifest.read_text().splitlines():
        fields = line.split(' ')
        if fields[0] == 'nokey':
            raw_nokey.setdefault(int(fields[1]), []).append((int(fields[2]), fields[3]))
    directories = {item['path']: item['number'] for item in objects
                   if item['kind'] == 'directory'}
    encrypted = features['encrypted']
    lines = []
    expected = {}
    for number in sorted(encrypted):
        path = next((path for path, value in directories.items() if value == number), None)
        if path is None:
            continue
        components = [root]
        parent = None
        prefix = ''
        for name in path.split('/')[1:]:
            prefix += '/' + name
            child = directories[prefix]
            components.append(next(nokey for entry, nokey in raw_nokey[parent] if entry == child)
                              if parent in encrypted else name)
            parent = child
        lines.append(f"nokeydir {number} {'/'.join(components)}")
        expected[number] = sorted(nokey for _, nokey in raw_nokey.get(number, []))
    seen = set()
    for item in objects:
        number = item['number']
        if number in seen:
            continue
        seen.add(number)
        path = root + item['path']
        data = export / f'object-{number}.data'
        if item['kind'] == 'file':
            contents = data.read_bytes()
            lines.append(f"file {path} {len(contents)} {hashlib.sha256(contents).hexdigest()}")
        elif item['kind'] == 'symlink':
            lines.append(f"symlink {path} {data.read_bytes().hex()}")
        else:
            lines.append(f"{item['kind']} {path}")
        if number in features['verity']:
            lines.append(f"verity {path} {features['verity'][number]['digest']}")
    return lines, expected, len(seen)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--lab', type=Path, required=True)
    parser.add_argument('--prepared', type=Path, required=True)
    parser.add_argument('--runner', type=Path, required=True)
    parser.add_argument('--export', type=Path, required=True,
                        help='directory holding one sustained export and its manifest')
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    lab, prepared, runner, export, output = (
        value.resolve() for value in (args.lab, args.prepared, args.runner, args.export,
                                      args.output))
    if Path.cwd() != lab:
        parser.error('run with explicit lab working directory')
    previous = json.loads((prepared / 'report.json').read_text())
    kernel = prepared / 'Image'
    if not previous[0]['passed'] or digest(kernel) != previous[0]['kernel_sha256']:
        raise RuntimeError('Prepared Linux reference is not accepted')
    images = sorted(export.glob('sustained-*.img'))
    if len(images) != 1:
        raise RuntimeError(f'Expected one exported image in {export}')
    _, _, _, features = parse_manifest(export / 'manifest.txt')
    if features['casefold']:
        raise RuntimeError('The reference kernel cannot mount casefolded volumes')
    output.mkdir(parents=True, exist_ok=False)
    tools = resolve_tools(lab / 'vendor/e2fsprogs-ext4/build')
    rows = []

    def run(row, command, timeout=600):
        command = [str(part) for part in command]
        done = subprocess.run(command, cwd=lab, capture_output=True, text=True, timeout=timeout)
        row['commands'].append(dict(command=command, status=done.returncode,
                                    stdout=done.stdout[-4000:], stderr=done.stderr[-4000:]))
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
    shutil.copyfile(ROOT / 'tests/linux_sustained.c', source)
    shutil.copyfile(ROOT / 'tests/linux_sha256.h', output / 'linux_sha256.h')
    compile_command[source_index] = str(source)
    compile_command[-1] = str(output / 'init')
    run(prep, compile_command)
    prep.update(passed=True, probe_sha256=digest(output / 'init'),
                probe_source_sha256=digest(source))
    row = dict(kind='verify', export=str(export), commands=[], passed=False)
    rows.append(row)
    image = output / 'sustained.img'
    shutil.copyfile(images[0], image)
    before = digest(image)
    row['input_sha256'] = before
    lines, expected, objects = guest_manifest(export, export / 'manifest.txt')
    tree = output / 'root'
    shutil.copytree(prepared / 'root-0', tree)
    shutil.copy2(output / 'init', tree / 'init')
    (tree / 'manifest').write_text('\n'.join(lines) + '\n')
    archive = output / 'probe.cpio'
    listing = '\n'.join(str(x.relative_to(tree)) for x in sorted(tree.rglob('*'))) + '\n'
    with archive.open('wb') as stream:
        subprocess.run(['/usr/bin/cpio', '-o', '-H', 'newc'], cwd=tree, input=listing.encode(),
                       stdout=stream, check=True)
    console = run(row, [runner, kernel, archive, 2, 512,
                        'console=hvc0 rdinit=/init panic=-1 loglevel=4', image], timeout=1800)
    (output / 'console.log').write_text(console)
    for required in (KERNEL, 'LINUX_SUSTAINED_VERIFIED', 'LINUX_SUSTAINED_RESULT=PASS',
                     f'LINUX_SUSTAINED_OBJECTS={objects}'):
        if required not in console:
            raise RuntimeError(f'Missing Linux sustained evidence: {required}')
    for forbidden in ('EXT4-fs error', 'Aborting journal'):
        if forbidden in console:
            raise RuntimeError(f'Guest filesystem error: {forbidden}')
    listed = {}
    for text in console.splitlines():
        text = text.strip()
        if text.startswith(NOKEY_PREFIX):
            number, name = text[len(NOKEY_PREFIX):].split(' ', 1)
            listed.setdefault(int(number), []).append(name)
    listed = {number: sorted(names) for number, names in listed.items()}
    expected = {number: names for number, names in expected.items() if names}
    if listed != expected:
        raise RuntimeError('Linux no-key names differ from the exported ones')
    run(row, [tools['e2fsck'], '-fn', image])
    if digest(image) != before:
        raise RuntimeError('The read-only verification changed the image')
    row.update(passed=True, objects=objects, encrypted_directories=len(expected),
               nokey_names=sum(len(names) for names in expected.values()),
               verity=len(features['verity']))
    (output / 'report.json').write_text(json.dumps(rows, indent=2) + '\n')
    print(f"PASS Linux sustained export {export.name}: {objects} objects, "
          f"{row['nokey_names']} no-key names in {len(expected)} encrypted directories, "
          f"{row['verity']} verity digests", flush=True)


if __name__ == '__main__':
    main()

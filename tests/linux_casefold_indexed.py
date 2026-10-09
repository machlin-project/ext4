#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
"""Author/check the fixed indexed fixture on an already mounted disposable ext4.

The private-namespace wrapper exclusively owns mounts and devices. This program
has no mount, unmount, installation, module-loading or reboot operations. Readback
uses original Linux-captured public identities and the fixed independent model,
never a manifest produced by the core under test. Fresh read-only mounts and
unchanged original/baseline hashes are additional wrapper responsibilities.
"""
import argparse
from collections import Counter
from contextlib import ExitStack
import errno
import fcntl
import json
import os
from pathlib import Path
import stat
import struct

from casefold_indexed_model import (collision_plan, directory_parents, encode_plan,
                                    exact_inventory, expected_data, expected_entry,
                                    namespace, padding_for, root_directories, validate_collision_plan)
from check_encrypted_casefold_native import CASEFOLD, ENCRYPT, INDEX, INLINE, context_keys, require
from generate_encrypted_casefold import Provider, hkdf, known_answers
from verify_casefold_roundtrip_linux import ADD_KEY, GETFLAGS, GET_NONCE, GET_POLICY, absent, listed_names

# asm-generic ioctl encoding; the public fscrypt policy setter retains its v1
# ioctl size even when the supplied policy.version requests the 24-byte v2 ABI.
SETFLAGS = 0x40086602
SET_POLICY = 0x800c6613
GETVERSION = 0x80087601
BASELINE_SCHEMA = 'combined-indexed-linux-baseline-v1'
ENDPOINTS = ('old', 'new', 'collision-new', 'rename-new')
OPEN_DIRECTORY = os.O_RDONLY | os.O_DIRECTORY | os.O_CLOEXEC | os.O_NOFOLLOW
OPEN_FILE = os.O_RDONLY | os.O_CLOEXEC | os.O_NOFOLLOW
MAX_BASELINE_BYTES = 32768


def check_abi():
    require(os.uname().sysname == 'Linux' and os.uname().machine in ('x86_64', 'aarch64') and
            struct.calcsize('P') == 8 and struct.calcsize('l') == 8 and
            struct.pack('=I', 1) == b'\1\0\0\0', 'Unsupported Linux ioctl ABI')


def add_key(root):
    argument = bytearray(144)
    struct.pack_into('<I', argument, 0, 2)
    struct.pack_into('<I', argument, 40, 64)
    argument[80:] = bytes((index * 7 + 3) & 255 for index in range(64))
    try:
        fcntl.ioctl(root, ADD_KEY, argument, True)
        require(bytes(argument[8:24]) == hkdf(1, 16), 'Linux v2 key identifier differs')
    finally:
        argument[:] = bytes(len(argument))


def context(fd):
    argument = bytearray(32)
    struct.pack_into('<Q', argument, 0, 24)
    fcntl.ioctl(fd, GET_POLICY, argument, True)
    require(struct.unpack_from('<Q', argument)[0] == 24, 'Linux policy is not v2')
    nonce = bytearray(16)
    fcntl.ioctl(fd, GET_NONCE, nonce, True)
    return bytes(argument[8:]) + bytes(nonce)


def flags(fd):
    argument = bytearray(8)
    fcntl.ioctl(fd, GETFLAGS, argument, True)
    return int.from_bytes(argument, 'little')


def generation(fd):
    # The long-sized GETVERSION request returns a uint32 generation, including
    # on the admitted LP64 hosts (Linux fs/ext4/ioctl.c GETVERSION handler).
    argument = bytearray(8)
    fcntl.ioctl(fd, GETVERSION, argument, True)
    return struct.unpack_from('<I', argument)[0]


def capture_public_directory(fd):
    metadata = os.fstat(fd)
    require(stat.S_ISDIR(metadata.st_mode), 'Public inode is not a directory')
    return dict(inode=metadata.st_ino, generation=generation(fd), type='directory',
                mode=stat.S_IMODE(metadata.st_mode), links=metadata.st_nlink,
                size=metadata.st_size, flags=flags(fd))


def verify_public_directories(root, baseline):
    expected_root = set(root_directories(baseline['block_size'])) | {b'lost+found'}
    require(listed_names(root) == expected_root, 'Linux exact root inventory differs')
    require(capture_public_directory(root) == baseline['public_directories']['root'],
            'Linux public root metadata changed')
    with ExitStack() as stack:
        lost = owned_fd(stack, b'lost+found', root)
        require(os.fstat(lost).st_dev == os.fstat(root).st_dev,
                'Linux lost+found device differs')
        require(capture_public_directory(lost) == baseline['public_directories']['lost+found'],
                'Linux public lost+found metadata changed')
        require(listed_names(lost) == set(), 'Linux lost+found is not empty')
        parent = os.stat(b'..', dir_fd=lost, follow_symlinks=False)
        require(parent.st_dev == os.fstat(root).st_dev and parent.st_ino == baseline['root_inode'],
                'Linux lost+found parent changed')


def padding(identity):
    return padding_for(identity)


def enable_policy(fd, pad, policy_first=False):
    policy = bytes((2, 1, 4, pad, 0, 0, 0, 0)) + hkdf(1, 16)
    if policy_first:
        fcntl.ioctl(fd, SET_POLICY, policy)
    fcntl.ioctl(fd, SETFLAGS, struct.pack('<Q', flags(fd) | CASEFOLD))
    if not policy_first:
        fcntl.ioctl(fd, SET_POLICY, policy)
    context_keys(context(fd), pad)


def owned_fd(stack, name, parent=None, directory=True, create=False):
    options = OPEN_DIRECTORY if directory else OPEN_FILE
    if create:
        options = os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_CLOEXEC | os.O_NOFOLLOW
    fd = os.open(name, options, 0o600, dir_fd=parent)
    stack.callback(os.close, fd)
    return fd


def capture(fd):
    return {'inode': os.fstat(fd).st_ino, 'context': context(fd).hex()}


def write_json(path, value):
    # Refuse accidental replacement of a prior fixture identity or report.
    with path.open('x') as output:
        output.write(json.dumps(value, indent=2, sort_keys=True) + '\n')


def outside_mount(path, mount):
    require(not path.resolve().is_relative_to(mount.resolve()),
            'Evidence output must be outside the mounted fixture')


def validate_baseline(baseline, provider):
    require(isinstance(baseline, dict) and set(baseline) ==
            {'schema', 'block_size', 'root_inode', 'directories', 'files', 'collision_plan',
             'public_directories'},
            'Unexpected baseline fields')
    require(baseline['schema'] == BASELINE_SCHEMA and type(baseline['block_size']) is int and
            baseline['block_size'] in (1024, 4096) and baseline['root_inode'] == 2,
            'Unexpected baseline schema/geometry/root')
    public = baseline['public_directories']
    require(isinstance(public, dict) and set(public) == {'root', 'lost+found'},
            'Unexpected public directory identities')
    for identity, row in public.items():
        require(isinstance(row, dict) and set(row) ==
                {'inode', 'generation', 'type', 'mode', 'links', 'size', 'flags'} and
                row['type'] == 'directory' and
                all(type(row[field]) is int for field in ('inode', 'generation', 'mode', 'links', 'size', 'flags')) and
                0 <= row['generation'] < 2 ** 32 and 0 <= row['mode'] <= 0o7777 and
                0 <= row['flags'] < 2 ** 32 and row['size'] > 0 and
                row['size'] % baseline['block_size'] == 0 and
                row['size'] <= 32 * 1024 * 1024 and
                (row['inode'] == 2 if identity == 'root' else 2 < row['inode'] < 2 ** 32) and
                row['links'] == (3 + len(root_directories(baseline['block_size'])) if identity == 'root' else 2),
                'Malformed public directory baseline')
    directories, files = baseline['directories'], baseline['files']
    require(isinstance(directories, dict) and isinstance(files, dict), 'Invalid baseline identities')
    plan = baseline['collision_plan']
    if baseline['block_size'] == 1024:
        require('collision' in directories, 'Missing collision directory')
        try:
            collision_context = bytes.fromhex(directories['collision']['context'])
        except (KeyError, TypeError, ValueError) as error:
            raise RuntimeError('Malformed collision context') from error
        validate_collision_plan(provider, plan, collision_context)
    else:
        require(plan is None, 'Unexpected 4KiB collision plan')
    names = namespace(baseline['block_size'], 'old', plan)
    expected_files = {identity for entries in names.values() for identity in entries.values()
                      if identity not in names}
    require(set(directories) == set(names) - {'root'} and set(files) == expected_files,
            'Baseline identity set differs from fixed scenario')
    all_records = {**directories, **files}
    inodes, nonces = {row['inode'] for row in public.values()}, set()
    for identity, row in all_records.items():
        require(isinstance(row, dict) and set(row) == {'inode', 'context'} and
                type(row['inode']) is int and row['inode'] > 2 and
                isinstance(row['context'], str) and len(row['context']) == 80,
                'Malformed baseline inode/context record')
        try:
            value = bytes.fromhex(row['context'])
        except ValueError as error:
            raise RuntimeError('Malformed baseline context hex') from error
        require(value.hex() == row['context'], 'Noncanonical baseline context hex')
        context_keys(value, padding(identity))
        require(row['inode'] not in inodes and value[24:] not in nonces,
                'Unexpected baseline inode or nonce aliasing')
        inodes.add(row['inode'])
        nonces.add(value[24:])
    return names


def read_baseline(path, provider):
    require(path.stat().st_size <= MAX_BASELINE_BYTES, 'Oversized Linux baseline')
    baseline = json.loads(path.read_text())
    validate_baseline(baseline, provider)
    return baseline


def create(root, block_size, provider):
    baseline = dict(schema=BASELINE_SCHEMA, block_size=block_size, root_inode=os.fstat(root).st_ino,
                    directories={}, files={}, collision_plan=None, public_directories={})
    require(listed_names(root) == {b'lost+found'}, 'Fixture creation requires a fresh root inventory')
    with ExitStack() as stack:
        lost = owned_fd(stack, b'lost+found', root)
        require(listed_names(lost) == set(), 'Fresh lost+found is not empty')
        original_lost = capture_public_directory(lost)
        descriptors = {'root': root}
        for name, identity in root_directories(block_size).items():
            os.mkdir(name, 0o700, dir_fd=root)
            fd = owned_fd(stack, name, root)
            descriptors[identity] = fd
            os.fchmod(fd, 0o700)
            enable_policy(fd, padding(identity), policy_first=identity.endswith('1'))
            baseline['directories'][identity] = capture(fd)
        if block_size == 1024:
            baseline['collision_plan'] = collision_plan(provider, context(descriptors['collision']))
        names = namespace(block_size, 'old', baseline['collision_plan'])
        directory_parents(names)
        # Stable insertion order creates the guard/A/guard collision leaves and
        # never creates the independently chosen B before the core mutation.
        first_names = {}
        pending = [identity for identity in names if identity != 'root']
        while pending:
            progress = False
            for identity in pending[:]:
                if identity not in descriptors:
                    continue
                directory = descriptors[identity]
                for name, target in names[identity].items():
                    if target in names:
                        os.mkdir(name, 0o700, dir_fd=directory)
                        child = owned_fd(stack, name, directory)
                        os.fchmod(child, 0o700)
                        descriptors[target] = child
                        baseline['directories'][target] = capture(child)
                        context_keys(context(child), padding(target))
                        require(flags(child) & (ENCRYPT | CASEFOLD) == ENCRYPT | CASEFOLD,
                                'Linux child did not inherit combined flags')
                    elif target in first_names:
                        previous_directory, previous_name = first_names[target]
                        os.link(previous_name, name, src_dir_fd=previous_directory,
                                dst_dir_fd=directory, follow_symlinks=False)
                    else:
                        with ExitStack() as file_stack:
                            file = owned_fd(file_stack, name, directory, directory=False, create=True)
                            os.fchmod(file, 0o600)
                            data = expected_data(target)
                            require(os.write(file, data) == len(data), 'Short Linux fixture write')
                            os.fsync(file)
                            baseline['files'][target] = capture(file)
                        first_names[target] = directory, name
                os.fsync(directory)
                pending.remove(identity)
                progress = True
            require(progress, 'Disconnected fixture creation order')
        os.fsync(root)
        baseline['public_directories'] = {
            'root': capture_public_directory(root), 'lost+found': capture_public_directory(lost)}
        require(baseline['public_directories']['lost+found'] == original_lost,
                'Fixture creation changed lost+found')
    validate_baseline(baseline, provider)
    return baseline


def file_metadata(metadata, record, identity, links, device):
    require(metadata.st_dev == device and metadata.st_ino == record['inode'] and
            metadata.st_mode == stat.S_IFREG | 0o600 and metadata.st_nlink == links and
            metadata.st_size == len(expected_data(identity)),
            'Linux file inode/mode/links/size differs: ' + identity)


def verify(root, baseline, endpoint, keyed, provider):
    verify_public_directories(root, baseline)
    names = namespace(baseline['block_size'], endpoint, baseline['collision_plan'])
    parents = directory_parents(names)
    identities = {**baseline['directories'], **baseline['files']}
    links = Counter(target for directory, entries in names.items() if directory != 'root'
                    for target in entries.values() if target not in names)
    expected_inventory, observed_inventory = [], []
    device = os.fstat(root).st_dev
    require(os.fstat(root).st_ino == baseline['root_inode'], 'Linux root identity changed')
    with ExitStack() as stack:
        descriptors = {'root': root}
        pending = ['root']
        visited = set()
        while pending:
            identity = pending.pop(0)
            require(identity not in visited, 'Repeated directory traversal')
            visited.add(identity)
            directory = descriptors[identity]
            if identity != 'root':
                record = identities[identity]
                metadata = os.fstat(directory)
                child_count = sum(target in names for target in names[identity].values())
                require(metadata.st_dev == device and metadata.st_ino == record['inode'] and
                        metadata.st_mode == stat.S_IFDIR | 0o700 and metadata.st_nlink == 2 + child_count,
                        'Linux directory identity/mode/links differs: ' + identity)
                require(context(directory).hex() == record['context'], 'Linux directory context changed')
                observed_flags = flags(directory)
                require(observed_flags & (ENCRYPT | CASEFOLD | INLINE) == ENCRYPT | CASEFOLD,
                        'Linux combined flags differ')
                if identity.startswith('main') or identity == 'collision' and endpoint in ('new', 'collision-new'):
                    require(observed_flags & INDEX, 'Linux expected indexed directory lacks INDEX')
                parent_stat = os.stat(b'..', dir_fd=directory, follow_symlinks=False)
                parent_inode = baseline['root_inode'] if parents[identity] == 'root' else identities[parents[identity]]['inode']
                require(parent_stat.st_dev == device and parent_stat.st_ino == parent_inode,
                        'Linux moved-directory parent identity differs')
            wire_names = {}
            for plain, target in names[identity].items():
                observed_name = plain
                if not keyed and identity != 'root':
                    observed_name = expected_entry(provider, bytes.fromhex(identities[identity]['context']),
                                                   padding(identity), plain)[3].encode('ascii')
                require(observed_name not in wire_names, 'Independent no-key name collision')
                wire_names[observed_name] = (plain, target)
            if identity != 'root':
                actual_names = listed_names(directory)
                require(actual_names == set(wire_names), 'Linux complete directory listing differs: ' + identity)
            for observed_name, (plain, target) in wire_names.items():
                record = identities[target]
                metadata = os.stat(observed_name, dir_fd=directory, follow_symlinks=False)
                require(metadata.st_dev == device and metadata.st_ino == record['inode'],
                        'Linux exact entry identity differs')
                if identity != 'root':
                    expected_inventory.append((identity, observed_name, record['inode']))
                    observed_inventory.append((identity, observed_name, metadata.st_ino))
                    if keyed:
                        alias = os.stat(plain.swapcase(), dir_fd=directory, follow_symlinks=False)
                        require(alias.st_ino == metadata.st_ino and alias.st_dev == device,
                                'Linux folded alias differs')
                    else:
                        absent(directory, plain)
                        absent(directory, plain.swapcase())
                if target in names:
                    descriptors[target] = owned_fd(stack, observed_name, directory)
                    pending.append(target)
                else:
                    file_metadata(metadata, record, target, links[target], device)
                    if keyed:
                        with ExitStack() as file_stack:
                            file = owned_fd(file_stack, observed_name, directory, directory=False)
                            require(context(file).hex() == record['context'], 'Linux file context changed')
                            data = expected_data(target)
                            require(os.read(file, len(data) + 1) == data and os.read(file, 1) == b'',
                                    'Linux plaintext differs: ' + target)
                    else:
                        try:
                            file = os.open(observed_name, OPEN_FILE, dir_fd=directory)
                        except OSError as error:
                            require(error.errno == errno.ENOKEY, 'Unexpected no-key content refusal')
                        else:
                            os.close(file)
                            raise RuntimeError('Linux exposed encrypted content without key')
            if keyed and identity != 'root' and endpoint != 'old':
                old = namespace(baseline['block_size'], 'old', baseline['collision_plan'])[identity]
                for removed in old.keys() - names[identity].keys():
                    absent(directory, removed)
                    absent(directory, removed.swapcase())
        require(visited == set(names), 'Missing inherited directory readback')
    exact_inventory(observed_inventory, expected_inventory)
    return [dict(parent=parent, name=name.hex(), inode=inode)
            for parent, name, inode in observed_inventory]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('phase', choices=('create', 'keyed', 'nokey'))
    parser.add_argument('--mount', type=Path, required=True)
    parser.add_argument('--block-size', type=int, choices=(1024, 4096), required=True)
    parser.add_argument('--baseline', type=Path, required=True)
    parser.add_argument('--collision-plan', type=Path)
    parser.add_argument('--endpoint', choices=ENDPOINTS, default='new',
                        help='fixed endpoint selected by the independent raw checker; never core output')
    parser.add_argument('--report', type=Path, required=True)
    args = parser.parse_args()
    check_abi()
    for output in (args.baseline, args.collision_plan, args.report):
        if output is not None:
            outside_mount(output, args.mount)
    require(args.phase != 'create' or args.block_size != 1024 or args.collision_plan is not None,
            '1KiB creation requires an external collision-plan destination')
    provider = Provider()
    known_answers(provider)
    with ExitStack() as stack:
        root = owned_fd(stack, args.mount)
        geometry = os.fstatvfs(root)
        require(geometry.f_bsize == args.block_size and
                0 < geometry.f_blocks * geometry.f_frsize <= 32 * 1024 * 1024,
                'Mounted filesystem exceeds bounded geometry')
        require(bool(geometry.f_flag & os.ST_RDONLY) == (args.phase != 'create'),
                'Create requires writable mount; readback requires fresh read-only mount')
        if args.phase != 'nokey':
            add_key(root)
        if args.phase == 'create':
            baseline = create(root, args.block_size, provider)
            entries = verify(root, baseline, 'old', True, provider)
            write_json(args.baseline, baseline)
            if baseline['collision_plan'] is not None:
                with args.collision_plan.open('xb') as output:
                    output.write(encode_plan(baseline['collision_plan']))
        else:
            baseline = read_baseline(args.baseline, provider)
            require(baseline['block_size'] == args.block_size, 'Baseline geometry changed')
            entries = verify(root, baseline, args.endpoint, args.phase == 'keyed', provider)
    write_json(args.report, dict(passed=True, kernel=os.uname().release, phase=args.phase,
                                endpoint='old' if args.phase == 'create' else args.endpoint,
                                block_size=args.block_size, entries=entries))
    print(f'PASS Linux indexed {args.phase}: {len(entries)} complete entry identities')


if __name__ == '__main__':
    main()

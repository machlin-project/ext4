#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
"""Verify an already mounted disposable roundtrip export through Linux syscalls.

No mount, loop-device, module-loading, namespace or reboot operation exists here.
The wrapper owns those resources. Only expected model data is shared with the raw
oracle; Linux performs all name comparison, decryption and inode interpretation.
"""
import argparse
import errno
import fcntl
import json
import os
from pathlib import Path
import stat
import struct

from check_encrypted_casefold_native import model, nokey_lines, require, ENCRYPT, CASEFOLD, INDEX, INLINE

# asm-generic ioctl encoding on the explicitly admitted 64-bit little-endian hosts.
GETFLAGS = 0x80086601
GET_POLICY = 0xc0096616
ADD_KEY = 0xc0506617
GET_NONCE = 0x8010661b


def listed_names(directory):
    names = [os.fsencode(name) for name in os.listdir(directory)]
    require(len(names) == len(set(names)), 'Linux returned duplicate directory entries')
    return set(names)


def policy(fd, context):
    argument = bytearray(32)
    struct.pack_into('<Q', argument, 0, 24)
    fcntl.ioctl(fd, GET_POLICY, argument, True)
    require(struct.unpack_from('<Q', argument)[0] == 24 and bytes(argument[8:]) == context[:24],
            'Linux policy differs from expected original context')
    nonce = bytearray(16)
    fcntl.ioctl(fd, GET_NONCE, nonce, True)
    require(bytes(nonce) == context[24:], 'Linux inode nonce differs')


def absent(directory, name):
    try:
        os.stat(name, dir_fd=directory, follow_symlinks=False)
    except FileNotFoundError:
        return
    raise RuntimeError(f'Unexpected present name: {name!r}')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--mount', type=Path, required=True)
    parser.add_argument('--exports', type=Path, required=True)
    parser.add_argument('--keyed', action='store_true')
    parser.add_argument('--report', type=Path, required=True)
    args = parser.parse_args()
    require(os.uname().sysname == 'Linux' and os.uname().machine in ('x86_64', 'aarch64') and
            struct.calcsize('P') == 8 and struct.pack('=I', 1) == b'\1\0\0\0',
            'Unsupported Linux ioctl ABI for this bounded probe')
    parents, files, names = model(args.exports / 'combined.manifest')
    observations = []
    nokey = []
    for line in nokey_lines(args.exports / 'combined.nokey'):
        parent, number, name = line.split()
        require(name and len(name) <= 252 and
                all(char in 'ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_' for char in name),
                'Invalid expected no-key name')
        nokey.append((int(parent), int(number), name.encode('ascii')))
    require(len(nokey) == 10 and len(set(nokey)) == 10, 'Unexpected no-key inventory size')
    root = os.open(args.mount, os.O_RDONLY | os.O_DIRECTORY | os.O_CLOEXEC | os.O_NOFOLLOW)
    try:
        if args.keyed:
            argument = bytearray(144)
            struct.pack_into('<I', argument, 0, 2)
            struct.pack_into('<I', argument, 40, 64)
            argument[80:] = bytes((index * 7 + 3) & 255 for index in range(64))
            fcntl.ioctl(root, ADD_KEY, argument, True)
            require(bytes(argument[8:24]) == parents[0]['context'][8:24], 'Linux key identifier differs')
            argument[:] = bytes(len(argument))
        for index, parent in parents.items():
            directory = os.open(parent['path'], os.O_RDONLY | os.O_DIRECTORY | os.O_CLOEXEC |
                                os.O_NOFOLLOW, dir_fd=root)
            try:
                metadata = os.fstat(directory)
                require(metadata.st_ino == parent['inode'] and metadata.st_mode == stat.S_IFDIR | 0o700
                        and metadata.st_nlink == 2, 'Linux parent identity/mode/links differ')
                flags = bytearray(8)
                fcntl.ioctl(directory, GETFLAGS, flags, True)
                require(int.from_bytes(flags, 'little') & (ENCRYPT | CASEFOLD | INDEX | INLINE) ==
                        ENCRYPT | CASEFOLD, 'Linux combined linear flags differ')
                policy(directory, parent['context'])
                actual = listed_names(directory)
                expected = [entry for entry in names if entry[0] == index]
                if args.keyed:
                    require(actual == {entry[2] for entry in expected}, 'Linux exact keyed listing differs')
                    for _, file_index, name, alias in expected:
                        file = files[file_index]
                        metadata = os.stat(name, dir_fd=directory, follow_symlinks=False)
                        alternate = os.stat(alias, dir_fd=directory, follow_symlinks=False)
                        links = sum(entry[1] == file_index for entry in names)
                        require(metadata.st_ino == alternate.st_ino == file['inode'] and
                                metadata.st_dev == alternate.st_dev and metadata.st_mode == file['mode'] and
                                metadata.st_size == file['size'] and metadata.st_nlink == links,
                                'Linux file/alias identity, mode, size or links differ')
                        fd = os.open(name, os.O_RDONLY | os.O_CLOEXEC | os.O_NOFOLLOW, dir_fd=directory)
                        try:
                            policy(fd, file['context'])
                            data = os.read(fd, 114)
                            require(data == file['data'] and os.read(fd, 1) == b'', 'Linux plaintext differs')
                        finally:
                            os.close(fd)
                        observations.append(dict(parent=index, inode=file['inode'], name=name.hex()))
                    absent(directory, b'Remove')
                    if index % 2 == 0:
                        absent(directory, b'STRASSE')
                else:
                    expected_nokey = [entry for entry in nokey if entry[0] == index]
                    require(actual == {entry[2] for entry in expected_nokey}, 'Linux full no-key envelopes differ')
                    for _, _, name, alias in expected:
                        absent(directory, name)
                        absent(directory, alias)
                    for _, number, name in expected_nokey:
                        metadata = os.stat(name, dir_fd=directory, follow_symlinks=False)
                        require(metadata.st_ino == number and stat.S_ISREG(metadata.st_mode),
                                'Linux no-key inode identity differs')
                        try:
                            fd = os.open(name, os.O_RDONLY | os.O_CLOEXEC | os.O_NOFOLLOW, dir_fd=directory)
                        except OSError as error:
                            require(error.errno == errno.ENOKEY, 'Unexpected no-key content refusal')
                        else:
                            os.close(fd)
                            raise RuntimeError('Linux exposed content without the key')
                        observations.append(dict(parent=index, inode=number, name=name.decode('ascii')))
            finally:
                os.close(directory)
    finally:
        os.close(root)
    args.report.write_text(json.dumps({'passed': True, 'kernel': os.uname().release,
                                      'keyed': args.keyed, 'entries': observations}, indent=2) + '\n')
    print(f'PASS Linux combined roundtrip {"keyed" if args.keyed else "nokey"}: 10 retained entries')


if __name__ == '__main__':
    main()

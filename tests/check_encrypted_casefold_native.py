#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
"""Independently check the fixed linear Linux/core encrypted-casefold roundtrip."""
import argparse
import base64
import hashlib
import json
from pathlib import Path
import struct
import subprocess

from check_namespace import inode_fields
from check_sustained import read_encrypted_file
from check_verity_enable import extents
from generate_encrypted_casefold import Provider, hashes, hkdf, known_answers
from generate_fixtures import resolve_tools

ENCRYPT = 0x800
CASEFOLD = 0x40000000
INDEX = 0x1000
INLINE = 0x10000000
FOLDS = {b'Stra\xc3\x9fe': b'strasse', b'\xc3\x89': b'e\xcc\x81',
         b'Beta': b'beta', b'Moved': b'moved', b'Native-Link': b'native-link'}


def require(condition, message):
    if not condition:
        raise RuntimeError(message)


def digest(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def nokey_lines(path):
    require(path.stat().st_size <= 8192, 'Oversized fixed no-key inventory')
    return path.read_text().splitlines()


def format_fields(superblock):
    require(len(superblock) == 1024, 'Short superblock')
    exponent = struct.unpack_from('<I', superblock, 24)[0]
    require(exponent in (0, 2), 'Unexpected filesystem block exponent')
    block_size = 1024 << exponent
    # Free-block/free-inode counters at bytes 12..19 legitimately change.
    identity = (superblock[0:12], superblock[20:44], superblock[56:58],
                superblock[76:80], superblock[84:120], superblock[0x27c:0x280])
    encoding, flags = struct.unpack_from('<HH', superblock, 0x27c)
    require(encoding == 1 and flags == (1 if block_size == 4096 else 0),
            'Fixture Unicode/strict profile differs')
    return block_size, identity


def context_keys(context, padding):
    require(len(context) == 40 and context[:8] == bytes((2, 1, 4, padding, 0, 0, 0, 0))
            and context[8:24] == hkdf(1, 16), 'Unexpected v2 policy/context/identifier')
    nonce = bytes(context[24:])
    return hkdf(2, 32, nonce), hkdf(5, 16, nonce), hkdf(2, 64, nonce)


def expected_entry(provider, context, padding, name):
    cts, sip, _ = context_keys(context, padding)
    padded = min(255, ((max(16, len(name)) + (4 << padding) - 1) // (4 << padding)) * (4 << padding))
    ciphertext = provider.cts(cts, name.ljust(padded, b'\0'))
    major, minor = hashes(provider, sip, FOLDS[name])
    envelope = struct.pack('<II', major, minor) + ciphertext
    require(len(ciphertext) <= 149, 'This bounded oracle admits only short no-key names')
    return ciphertext, major, minor, base64.urlsafe_b64encode(envelope).rstrip(b'=').decode('ascii')


def directory_entries(block, number, root):
    """Separate on-disk decoder, limited to one checksum-bearing linear block."""
    require(len(block) in (1024, 4096), 'Unexpected bounded directory block size')
    block = bytes(block)
    entries = {}
    dots = {}
    offset = 0
    tail = False
    while offset < len(block):
        require(offset + 8 <= len(block), 'Truncated directory header')
        inode, length, size, kind = struct.unpack_from('<IHBB', block, offset)
        require(length >= 8 and length % 4 == 0 and offset + length <= len(block),
                'Invalid directory record bounds')
        require(size <= length - 8, 'Name exceeds record')
        name = block[offset + 8:offset + 8 + size]
        if inode == 0 and size == 0 and kind == 0xde:
            require(length == 12 and offset + length == len(block), 'Malformed checksum tail')
            tail = True
        elif inode and name in (b'.', b'..'):
            require(kind == 2 and length >= 12 and name not in dots, 'Malformed/duplicate dot record')
            dots[name] = inode
        else:
            used = 8 + ((size + 3) & ~3) + 8
            require(length >= max(20, used), 'Missing combined hash extension')
            if inode:
                require(kind == 1 and 16 <= size <= 255 and name not in entries,
                        'Unexpected or duplicate live encrypted entry')
                major, minor = struct.unpack_from('<II', block, offset + used - 8)
                entries[name] = (inode, major, minor)
        offset += length
    require(tail and dots == {b'.': number, b'..': root}, 'Missing tail or incorrect dot identities')
    return entries


def model(path):
    parents, files, names = {}, {}, []
    require(path.stat().st_size <= 32768, 'Oversized fixed model')
    lines = path.read_text().splitlines()
    require(lines and lines[0] == 'combined-linear-v1', 'Unsupported model version')
    for line in lines[1:]:
        fields = line.split()
        kind = fields.pop(0)
        if kind == 'parent':
            index, name, number, generation, context = fields
            index = int(index)
            require(index not in parents, 'Duplicate model parent')
            parents[index] = dict(path=name, inode=int(number), generation=int(generation),
                                  context=bytes.fromhex(context))
        elif kind == 'file':
            index, number, generation, mode, size, context, data = fields
            index = int(index)
            require(index not in files, 'Duplicate model file')
            files[index] = dict(inode=int(number), generation=int(generation), mode=int(mode),
                                size=int(size), context=bytes.fromhex(context), data=bytes.fromhex(data))
        elif kind == 'name':
            parent, file, name, alias = fields
            names.append((int(parent), int(file), bytes.fromhex(name), bytes.fromhex(alias)))
        else:
            raise RuntimeError('Unknown model record')
    expected = []
    for pair in range(2):
        a, b = pair * 2, pair * 2 + 1
        expected.extend(((a, 4 + b, b'\xc3\x89', b'e\xcc\x81'),
                         (a, a, b'Native-Link', b'native-link'),
                         (b, b, b'Stra\xc3\x9fe', b'STRASSE'),
                         (b, 4 + a, b'Beta', b'bETA'), (b, a, b'Moved', b'MOVED')))
    require(set(parents) == set(range(4)) and set(files) == set(range(8)) and
            sorted(names) == sorted(expected), 'Model differs from fixed mutation sequence')
    require(len({item['inode'] for item in parents.values()}) == 4 and
            len({item['inode'] for item in files.values()}) == 8, 'Unexpected inode aliasing')
    for index, parent in parents.items():
        require(parent['path'] == f'probe-{index // 2}-{index % 2}', 'Unexpected parent path')
    for index, file in files.items():
        size = 1 if index < 4 else (97 if index % 2 == 0 else 113)
        data = b'K' if index < 4 else bytes((index * 37 + at * 13) & 255 for at in range(size))
        mode = 0o100600 if index < 4 else 0o100640
        require((file['mode'], file['size'], file['data']) == (mode, size, data),
                'Manifest payload/mode was not independently reproduced')
    return parents, files, names


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--original', type=Path, required=True)
    parser.add_argument('--exports', type=Path, required=True)
    parser.add_argument('--tools-root', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    original = args.original.resolve(strict=True)
    exported = args.exports.resolve() / ('combined-' + original.name)
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    parents, files, names = model(args.exports / 'combined.manifest')
    tools = resolve_tools(args.tools_root)
    provider = Provider()
    known_answers(provider)
    report = {'passed': False, 'scope': 'linear-independent-media', 'provider': provider.version,
              'commands': [], 'images': [], 'entries': []}

    def save():
        (output / 'report.json').write_text(json.dumps(report, indent=2) + '\n')

    def run(command):
        done = subprocess.run([str(part) for part in command], capture_output=True,
                              text=True, errors='backslashreplace', timeout=120)
        report['commands'].append(dict(command=[str(part) for part in command],
                                       status=done.returncode, stdout=done.stdout, stderr=done.stderr))
        save()
        done.check_returncode()
        return done.stdout

    def inode(image, selector):
        return inode_fields(run([tools['debugfs'], '-R', f'stat {selector}', image]))

    def context(image, number):
        target = output / 'context.bin'
        require(not target.exists(), 'Stale context temporary')
        run([tools['debugfs'], '-R', f'ea_get -r -f "{target}" <{number}> encryption.c', image])
        value = target.read_bytes()
        target.unlink()
        return value

    expected_nokey = []
    all_nonces = set()
    original_format = None
    for image in (original, exported):
        require(image.stat().st_size == 32 * 1024 * 1024, 'Unexpected image bound')
        before = digest(image)
        run([tools['e2fsck'], '-fn', image])
        with image.open('rb') as stream:
            stream.seek(1024)
            superblock = stream.read(1024)
        block_size, format_identity = format_fields(superblock)
        if original_format is None:
            original_format = format_identity
        require(format_identity == original_format, 'Filesystem format identity changed')
        original_image = image == original
        for index, parent in parents.items():
            padding = 0 if index < 2 else 3
            metadata = inode(image, '"/' + parent['path'] + '"')
            require(metadata is not None and metadata['inode'] == parent['inode'] and
                    metadata['generation'] == parent['generation'] and metadata['type'] == 'directory'
                    and metadata['mode'] == 0o700 and metadata['links'] == 2 and metadata['size'] == block_size and
                    metadata['flags'] & (ENCRYPT | CASEFOLD | INDEX | INLINE) == ENCRYPT | CASEFOLD,
                    'Parent identity/flags/layout changed')
            actual_context = context(image, parent['inode'])
            require(actual_context == parent['context'], 'Native parent context changed')
            context_keys(actual_context, padding)
            if original_image:
                require(actual_context[24:] not in all_nonces, 'Repeated parent nonce')
                all_nonces.add(actual_context[24:])
            mapping = extents(run([tools['debugfs'], '-R', f'dump_extents <{parent["inode"]}>', image]))
            require(len(mapping) == 1 and mapping[0][0] == 0 and mapping[0][2:] == (1, False),
                    'Unexpected directory extent mapping')
            with image.open('rb') as stream:
                stream.seek(mapping[0][1] * block_size)
                block = stream.read(block_size)
            require(len(block) == block_size, 'Short directory block')
            actual = directory_entries(block, parent['inode'], 2)
            wanted = [(index, index, b'Stra\xc3\x9fe', b'STRASSE')] if original_image else \
                [entry for entry in names if entry[0] == index]
            expected_entries = {}
            for _, file_index, name, _ in wanted:
                ciphertext, major, minor, nokey = expected_entry(provider, actual_context, padding, name)
                expected_entries[ciphertext] = (files[file_index]['inode'], major, minor)
                if not original_image:
                    expected_nokey.append(f'{index} {files[file_index]["inode"]} {nokey}')
            require(actual == expected_entries, 'Ciphertext/inode/stored SipHash inventory differs')
            report['entries'].append(dict(image=image.name, parent=index, count=len(actual)))
        for index in range(4 if original_image else 8):
            file = files[index]
            metadata = inode(image, f'<{file["inode"]}>')
            padding = 0 if index % 4 < 2 else 3
            links = 1 if original_image else sum(entry[1] == index for entry in names)
            require(metadata is not None and metadata['type'] == 'regular' and
                    metadata['generation'] == file['generation'] and metadata['inode'] == file['inode'] and
                    metadata['mode'] == file['mode'] & 0o7777 and metadata['size'] == file['size'] and
                    metadata['links'] == links and metadata['flags'] & (ENCRYPT | CASEFOLD | INLINE) == ENCRYPT,
                    'File identity/type/links/mode/flags/size mismatch')
            actual_context = context(image, file['inode'])
            require(actual_context == file['context'], 'File fscrypt context changed')
            _, _, key = context_keys(actual_context, padding)
            if original_image or index >= 4:
                require(actual_context[24:] not in all_nonces, 'Repeated file nonce')
                all_nonces.add(actual_context[24:])
            mapping = extents(run([tools['debugfs'], '-R', f'dump_extents <{file["inode"]}>', image]))
            data = read_encrypted_file(image, mapping, block_size, 0, file['size'], key, True)
            require(data == file['data'], 'Independently decrypted file contents differ')
        require(digest(image) == before, 'Independent checking changed media')
        report['images'].append(dict(path=str(image), sha256=before, block_size=block_size))
    observed = nokey_lines(args.exports / 'combined.nokey')
    require(sorted(observed) == sorted(expected_nokey), 'Full no-key envelopes differ')
    report['passed'] = True
    save()
    print('PASS independent combined linear media: original+export, 4 parents, 10 retained names')


if __name__ == '__main__':
    main()

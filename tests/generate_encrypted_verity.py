#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
"""Independent byte fixtures for the mapped encrypted-verity reader unit test.

These are file-fork fixtures, not complete ext4 volumes or Linux acceptance.
Python hashlib constructs Merkle metadata; cryptography/OpenSSL supplies HKDF,
AES-ECB and AES-XTS independently of the C core and its test crypto adapter.
"""
import argparse
import hashlib
from pathlib import Path
import struct

from cryptography.hazmat.primitives import hashes
from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes
from cryptography.hazmat.primitives.kdf.hkdf import HKDF

from generate_verity_fixtures import layout


def create(output, name, fs_block, merkle_block, size, algorithm=1, version=2,
           sparse=False, signed=False):
    plain = bytearray((i * 17 + (i >> 9) + 23) & 255 for i in range(size))
    if sparse:
        plain[fs_block:2 * fs_block] = bytes(fs_block)
    salt = bytes(range(16))
    signature = bytes((i * 11 + 5) & 255 for i in range(fs_block + 17)) if signed else b""
    pieces, digest, tree = layout(bytes(plain), fs_block, merkle_block,
                                  algorithm, salt, signature=signature)
    length = max(offset + len(data) for offset, data in pieces)
    stored = bytearray(length)
    for offset, data in pieces:
        stored[offset:offset + len(data)] = data
    assert len(stored) % fs_block == 0
    nonce = bytes(range(16))
    master = bytes((i * 7 + 3) & 255 for i in range(64))
    if version == 2:
        key = HKDF(algorithm=hashes.SHA512(), length=64, salt=bytes(64),
                   info=b"fscrypt\0\x02" + nonce).derive(master)
    else:
        enc = Cipher(algorithms.AES(nonce), modes.ECB()).encryptor()
        key = enc.update(master) + enc.finalize()
    cipher = bytearray()
    for logical, offset in enumerate(range(0, len(stored), fs_block)):
        enc = Cipher(algorithms.AES(key), modes.XTS(logical.to_bytes(16, "little"))).encryptor()
        cipher += enc.update(stored[offset:offset + fs_block]) + enc.finalize()
    stem = output / name
    stem.with_suffix('.plain').write_bytes(plain)
    stem.with_suffix('.stored').write_bytes(stored)
    stem.with_suffix('.cipher').write_bytes(cipher)
    stem.with_suffix('.meta').write_text(
        f"{fs_block} {merkle_block} {size} {len(stored)} {tree} {pieces[2][0]} "
        f"{algorithm} {version} {int(sparse)} {len(signature)}\n{digest}\n")
    return name


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', required=True, type=Path)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=False)
    names = []
    for block in (1024, 4096, 16384, 65536):
        for label, size in (('empty', 0), ('tiny', 1), ('tail', block - 1),
                            ('aligned', block), ('sparse', 3 * block + 17)):
            names.append(create(args.output, f'{block}-{label}', block,
                                min(block, 4096), size, sparse=label == 'sparse'))
    names.append(create(args.output, 'v1-sha512', 4096, 1024, 150 * 1024 + 33,
                        algorithm=2, version=1))
    names.append(create(args.output, 'v2-signed', 4096, 1024, 150 * 1024 + 33,
                        signed=True))
    names.append(create(args.output, 'boundary', 4096, 4096, 65536))
    (args.output / 'manifest').write_text('\n'.join(names) + '\n')
    print(f'Generated {len(names)} independent file-fork fixtures (not volume images).')


if __name__ == '__main__':
    main()

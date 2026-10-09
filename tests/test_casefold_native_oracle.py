#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
"""Bounded decoder/oracle tests using independent filename vectors, not volumes."""
from pathlib import Path
import struct
import sys
import tempfile
import unittest
from unittest.mock import patch

from check_encrypted_casefold_native import context_keys, directory_entries, expected_entry, format_fields, nokey_lines
from generate_encrypted_casefold import Provider, known_answers
from verify_casefold_roundtrip_linux import listed_names

FIXTURES = Path(sys.argv.pop(1))


def block(entries):
    result = bytearray(1024)
    struct.pack_into('<IHBB', result, 0, 11, 12, 1, 2)
    result[8] = ord('.')
    struct.pack_into('<IHBB', result, 12, 2, 12, 2, 2)
    result[20:22] = b'..'
    offset = 24
    for index, (cipher, inode, major, minor) in enumerate(entries):
        used = 8 + ((len(cipher) + 3) & ~3) + 8
        length = used if index + 1 < len(entries) else 1012 - offset
        struct.pack_into('<IHBB', result, offset, inode, length, len(cipher), 1)
        result[offset + 8:offset + 8 + len(cipher)] = cipher
        struct.pack_into('<II', result, offset + used - 8, major, minor)
        offset += length
    struct.pack_into('<IHBBI', result, 1012, 0, 12, 0, 0xde, 0)
    return result


class Oracle(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.provider = Provider()
        known_answers(cls.provider)
        cls.context = (FIXTURES / 'context.bin').read_bytes()

    def fixture(self, label):
        cipher = (FIXTURES / (label + '.cipher')).read_bytes()
        metadata = [int(value) for value in (FIXTURES / (label + '.meta')).read_text().split()]
        return cipher, metadata[5], metadata[6]

    def test_expected_bytes_match_frozen_independent_vectors(self):
        for label in ('sharp-s', 'canonical-accent'):
            with self.subTest(label=label):
                name = (FIXTURES / (label + '.name')).read_bytes()
                cipher, major, minor, nokey = expected_entry(self.provider, self.context, 0, name)
                self.assertEqual((cipher, major, minor), self.fixture(label))
                self.assertEqual(nokey, (FIXTURES / (label + '.nokey')).read_text())

    def test_exact_wire_hash_and_inode_decoding(self):
        cipher, major, minor = self.fixture('sharp-s')
        data = block([(cipher, 12, major, minor)])
        self.assertEqual(directory_entries(data, 11, 2), {cipher: (12, major, minor)})
        struct.pack_into('<I', data, 24 + 8 + ((len(cipher) + 3) & ~3), major ^ 2)
        self.assertNotEqual(directory_entries(data, 11, 2), {cipher: (12, major, minor)})

    def test_malformed_directory_bounds_and_identity_refuse(self):
        cipher, major, minor = self.fixture('sharp-s')
        original = block([(cipher, 12, major, minor)])
        cases = []
        changed = original.copy()
        struct.pack_into('<H', changed, 24 + 4, 0)
        cases.append(changed)
        changed = original.copy()
        struct.pack_into('<H', changed, 24 + 4, 8 + len(cipher))
        cases.append(changed)
        changed = original.copy()
        struct.pack_into('<I', changed, 0, 99)
        cases.append(changed)
        changed = original.copy()
        changed[1019] = 0
        cases.append(changed)
        cases.append(block([(cipher, 12, major, minor), (cipher, 13, major, minor)]))
        for data in cases:
            with self.subTest(data=data[:32].hex()):
                with self.assertRaises(RuntimeError):
                    directory_entries(data, 11, 2)

    def test_context_policy_and_nonce_domain_separation(self):
        key = context_keys(self.context, 0)
        for offset in (0, 1, 2, 3, 4, 8):
            changed = bytearray(self.context)
            changed[offset] ^= 1
            with self.subTest(offset=offset), self.assertRaises(RuntimeError):
                context_keys(changed, 0)
        changed = bytearray(self.context)
        changed[24] ^= 1
        other = context_keys(changed, 0)
        self.assertTrue(all(left != right for left, right in zip(key, other)))

    def test_format_bounds_and_immutable_identity(self):
        for exponent in (0, 2):
            superblock = bytearray(1024)
            struct.pack_into('<I', superblock, 24, exponent)
            struct.pack_into('<HH', superblock, 0x27c, 1, int(exponent == 2))
            size, identity = format_fields(superblock)
            self.assertEqual(size, 1024 << exponent)
            changed = superblock.copy()
            changed[12:20] = bytes(range(8))
            self.assertEqual(format_fields(changed)[1], identity)
            for offset in (92, 96, 100, 104):
                changed = superblock.copy()
                changed[offset] ^= 1
                self.assertNotEqual(format_fields(changed)[1], identity)
            changed = superblock.copy()
            changed[0x27e] ^= 1
            with self.assertRaises(RuntimeError):
                format_fields(changed)
            struct.pack_into('<I', changed, 24, 0xffffffff)
            with self.assertRaises(RuntimeError):
                format_fields(changed)

    def test_duplicate_native_listing_refuses(self):
        with patch('verify_casefold_roundtrip_linux.os.listdir', return_value=['Beta', 'Beta']):
            with self.assertRaises(RuntimeError):
                listed_names(-1)
        with patch('verify_casefold_roundtrip_linux.os.listdir', return_value=['Beta', 'Moved']):
            self.assertEqual(listed_names(-1), {b'Beta', b'Moved'})

    def test_no_key_inventory_is_bounded(self):
        with tempfile.TemporaryDirectory(prefix='casefold-oracle-unit-') as temporary:
            path = Path(temporary) / 'names'
            path.write_text('0 12 abc\n')
            self.assertEqual(nokey_lines(path), ['0 12 abc'])
            path.write_bytes(bytes(8193))
            with self.assertRaises(RuntimeError):
                nokey_lines(path)


if __name__ == '__main__':
    unittest.main()

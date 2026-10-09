#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
"""Independent synthetic decoder/model negatives; no mounted images or core code."""
import hashlib
import json
from pathlib import Path
import struct
import tempfile
import subprocess
from unittest.mock import patch
import unittest

from casefold_indexed_model import (bulk_name, collision_name, counts, content, expected_entry,
    namespace, directory_parents, padding_for, exact_inventory, endpoint, load_plan, encode_plan,
    validate_collision_plan, link_counts)
from check_encrypted_casefold_indexed import (crc32c, bounded_mapping, directory_tree,
    index_entries, leaf_entries, check_file_metadata, Media, check_media,
    public_entries, public_identity, check_public_metadata, select_endpoint, check_payload)
from check_encrypted_casefold_native import context_keys
from generate_encrypted_casefold import Provider, hkdf, known_answers

SEED = 0x12345678


def leaf(rows, dots=None):
    block = bytearray(1024)
    offset = 0
    if dots:
        for name, inode in dots.items():
            struct.pack_into('<IHBB', block, offset, inode, 12, len(name), 2)
            block[offset + 8:offset + 8 + len(name)] = name
            offset += 12
    for index, (name, inode, major, minor, kind) in enumerate(rows):
        used = 8 + ((len(name) + 3) & ~3) + 8
        length = used if index + 1 < len(rows) else 1012 - offset
        struct.pack_into('<IHBB', block, offset, inode, length, len(name), kind)
        block[offset + 8:offset + 8 + len(name)] = name
        struct.pack_into('<II', block, offset + used - 8, major, minor)
        offset += length
    if offset < 1012:
        struct.pack_into('<IHBB', block, offset, 0, 1012 - offset, 0, 0)
    struct.pack_into('<IHBBI', block, 1012, 0, 12, 0, 0xde, crc32c(SEED, block[:1012]))
    return block


def index(rows, depth=0, root=True):
    block = bytearray(1024)
    base = 32 if root else 8
    if root:
        struct.pack_into('<IHBB', block, 0, 11, 12, 1, 2)
        block[8] = 46
        struct.pack_into('<IHBB', block, 12, 2, 1012, 2, 2)
        block[20:22] = b'..'
        struct.pack_into('<IBBBB', block, 24, 0, 6, 8, depth, 0)
    else:
        struct.pack_into('<IHBB', block, 0, 0, 1024, 0, 0)
    for at, row in enumerate(rows):
        struct.pack_into('<II', block, base + 8 * at, *row)
    limit = (1024 - base - 8) // 8
    struct.pack_into('<HH', block, base, limit, len(rows))
    checksum = crc32c(crc32c(SEED, block[:base + 8 * len(rows)]), bytes(8))
    struct.pack_into('<II', block, 1016, 0, checksum)
    return block


def repair_index(block, base=32):
    count = struct.unpack_from('<H', block, base + 2)[0]
    struct.pack_into('<I', block, 1020, crc32c(crc32c(SEED, block[:base + count * 8]), bytes(8)))


class IndexedOracle(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.provider = Provider()
        known_answers(cls.provider)
        cls.context = bytes((2, 1, 4, 0, 0, 0, 0, 0)) + hkdf(1, 16) + bytes(range(16))

    def tree(self):
        return {0: index([(0, 1), (101, 2)]),
                1: leaf([(b'A' * 16, 12, 100, 7, 1)]),
                2: leaf([(b'B' * 16, 13, 100, 9, 1)])}

    def test_single_operation_endpoint_models_and_no_unrelated_changes(self):
        plan = dict(schema='combined-indexed-collision-v1', context=self.context.hex(),
                    low=17933, a=32625, b=76089, high=44844)
        for block_size in (1024, 4096):
            old = namespace(block_size, 'old', plan)
            renamed = namespace(block_size, 'rename-new', plan)
            wanted = {parent: dict(entries) for parent, entries in old.items()}
            wanted['peer0'][b'Moved'] = wanted['main0'].pop(bulk_name(1))
            self.assertEqual(renamed, wanted)
            self.assertEqual(link_counts(renamed), link_counts(old))
            self.assertEqual(select_endpoint('auto-rename', False, True), 'old')
            self.assertEqual(select_endpoint('auto-rename', True, False), 'rename-new')
            for source, target in ((True, True), (False, False)):
                with self.subTest(source=source, target=target), self.assertRaises(RuntimeError):
                    select_endpoint('auto-rename', target, source)
            duplicated = {parent: dict(entries) for parent, entries in renamed.items()}
            duplicated['main0'][bulk_name(1)] = old['main0'][bulk_name(1)]
            lost = {parent: dict(entries) for parent, entries in renamed.items()}
            del lost['peer0'][b'Moved']
            for hybrid in (duplicated, lost):
                with self.assertRaises(RuntimeError):
                    endpoint(hybrid, old, renamed)
        old = namespace(1024, 'old', plan)
        collided = namespace(1024, 'collision-new', plan)
        wanted = {parent: dict(entries) for parent, entries in old.items()}
        wanted['collision'][collision_name(plan['b'])] = 'p2s1'
        self.assertEqual(collided, wanted)
        self.assertEqual(select_endpoint('auto-collision', False), 'old')
        self.assertEqual(select_endpoint('auto-collision', True), 'collision-new')
        with self.assertRaises(RuntimeError):
            namespace(4096, 'collision-new', plan)

    def test_collision_endpoint_wrong_identity_link_count_and_payload_rejected(self):
        plan = dict(schema='combined-indexed-collision-v1', context=self.context.hex(),
                    low=17933, a=32625, b=76089, high=44844)
        old, new = namespace(1024, 'old', plan), namespace(1024, 'collision-new', plan)
        self.assertEqual((link_counts(old)['p2s1'], link_counts(new)['p2s1']), (1, 2))
        wrong = {parent: dict(entries) for parent, entries in new.items()}
        wrong['collision'][collision_name(plan['b'])] = 'p2s0'
        with self.assertRaises(RuntimeError):
            endpoint(wrong, old, new)
        metadata = dict(inode=42, generation=8, type='regular', mode=0o600,
                        size=113, links=2, flags=0x800)
        check_file_metadata(metadata, metadata, self.context, self.context, content(2, 1), 2)
        with self.assertRaises(RuntimeError):
            check_file_metadata(dict(metadata, links=1), metadata, self.context, self.context, content(2, 1), 2)
        with self.assertRaises(RuntimeError):
            check_file_metadata(dict(metadata, inode=41), metadata, self.context, self.context, content(2, 1), 2)
        with self.assertRaises(RuntimeError):
            check_payload(content(2, 0), content(2, 1))
        with self.assertRaises(RuntimeError):
            check_payload(content(2, 1)[:-1] + bytes((content(2, 1)[-1] ^ 1,)), content(2, 1))

    def test_directory_size_rejected_before_regular_file_rounding(self):
        media = object.__new__(Media)
        media.block_size = 1024
        with patch.object(media, 'run') as run:
            with self.assertRaisesRegex(RuntimeError, 'whole-block'):
                media.mapping(dict(type='directory', inode=11, size=1025))
            run.assert_not_called()

    def test_failure_report_retains_stderr_and_source_identity(self):
        with tempfile.TemporaryDirectory() as temporary:
            output = Path(temporary)
            original, exported = output / 'original', output / 'exported'
            original.write_bytes(b'original')
            exported.write_bytes(b'exported')
            def fail(original, exported, tools, output, report, *args):
                media = object.__new__(Media)
                media.output, media.report, media.tools = output, report, {'debugfs': 'debugfs'}
                media.run(['debugfs', '-R', 'stat /', original])
            failure = subprocess.CompletedProcess([], 0, '', 'unrecognized debugfs diagnostic\n')
            with patch('check_encrypted_casefold_indexed._check_media', side_effect=fail), \
                    patch('check_encrypted_casefold_indexed.subprocess.run', return_value=failure), \
                    self.assertRaises(RuntimeError):
                check_media(original, exported, {}, output)
            report = json.loads((output / 'report.json').read_text())
            self.assertFalse(report['passed'])
            self.assertEqual(report['error']['type'], 'RuntimeError')
            self.assertEqual(report['original'], str(original))
            self.assertEqual(report['sources'][0]['sha256'], hashlib.sha256(b'original').hexdigest())
            self.assertEqual(report['sources'][1]['sha256'], hashlib.sha256(b'exported').hexdigest())
            self.assertEqual(report['commands'][0]['stderr'], failure.stderr)
            timeout = subprocess.TimeoutExpired(['debugfs'], 120, output=b'partial', stderr=b'timeout detail')
            with patch('check_encrypted_casefold_indexed._check_media', side_effect=fail), \
                    patch('check_encrypted_casefold_indexed.subprocess.run', side_effect=timeout), \
                    self.assertRaises(subprocess.TimeoutExpired):
                check_media(original, exported, {}, output)
            report = json.loads((output / 'report.json').read_text())
            self.assertEqual(report['commands'][0]['stderr'], 'timeout detail')
            self.assertEqual(report['commands'][0]['stdout'], 'partial')

    def test_public_directory_preservation_and_empty_lost_found(self):
        metadata = dict(inode=2, generation=7, type='directory', mode=0o755,
                        links=7, size=1024, flags=0x80000)
        identities = {'root': public_identity(metadata),
                      'lost+found': public_identity(dict(metadata, inode=11, links=2, mode=0o700))}
        check_public_metadata(identities, identities)
        for field, value in (('inode', 12), ('generation', 8), ('mode', 0o750), ('links', 3),
                             ('size', 2048), ('flags', 0), ('type', 'regular')):
            changed = dict(identities, **{'lost+found': dict(identities['lost+found'], **{field: value})})
            with self.subTest(field=field), self.assertRaises(RuntimeError):
                check_public_metadata(changed, identities)
        empty = leaf([], {b'.': 11, b'..': 2})
        self.assertEqual(public_entries(empty, SEED, {b'.': 11, b'..': 2}), {})
        occupied = leaf([(b'lost-entry', 13, 0, 0, 2)], {b'.': 11, b'..': 2})
        self.assertEqual(public_entries(occupied, SEED, {b'.': 11, b'..': 2}), {b'lost-entry': 13})
        with self.assertRaises(RuntimeError):
            public_entries(empty, SEED, {b'.': 11, b'..': 3})
        corrupt = empty.copy()
        corrupt[0] ^= 1
        with self.assertRaises(RuntimeError):
            public_entries(corrupt, SEED, {b'.': 11, b'..': 2})

    def test_debugfs_context_key_and_zero_status_error(self):
        with tempfile.TemporaryDirectory() as temporary:
            media = object.__new__(Media)
            media.output = Path(temporary)
            media.path = Path(temporary) / 'image'
            media.tools = {'debugfs': 'debugfs'}
            media.report = {'commands': []}
            missing = subprocess.CompletedProcess([], 0, '',
                'ea_get: Extended attribute key not found while getting extended attribute\n')
            with patch('check_encrypted_casefold_indexed.subprocess.run', return_value=missing), \
                    self.assertRaisesRegex(RuntimeError, 'Debugfs command diagnostic'):
                media.context(11)
            self.assertIn('Extended attribute key not found', media.report['commands'][-1]['stderr'])
            def extract(command, **kwargs):
                self.assertTrue(command[2].endswith('<11> c'))
                (media.output / 'context.tmp').write_bytes(self.context)
                return subprocess.CompletedProcess(command, 0, '', '')
            with patch('check_encrypted_casefold_indexed.subprocess.run', side_effect=extract):
                self.assertEqual(media.context(11), self.context)
            self.assertFalse((media.output / 'context.tmp').exists())
            with patch('check_encrypted_casefold_indexed.subprocess.run',
                       return_value=subprocess.CompletedProcess([], 0, '', '')), \
                    self.assertRaisesRegex(RuntimeError, 'Missing or invalid'):
                media.context(11)

    def test_link_counts_and_metadata_hybrid_rejection(self):
        old, new = namespace(4096, 'old'), namespace(4096, 'new')
        before, after = link_counts(old), link_counts(new)
        self.assertEqual((before['main0'], after['main0']), (3, 2))
        self.assertEqual((before['peer0'], after['peer0']), (2, 3))
        self.assertEqual((before['child0'], after['child0']), (3, 3))
        self.assertEqual((before['p0s0'], after['p0s0']), (16, 23))
        self.assertEqual((before['p0s3'], after['p0s3']), (16, 25))
        metadata = dict(inode=12, generation=9, type='regular', mode=0o600,
                        size=97, links=23, flags=0x800)
        check_file_metadata(metadata, metadata, self.context, self.context, content(0, 0), 23)
        for field, value in (('links', 16), ('generation', 10), ('mode', 0o640), ('size', 98)):
            with self.subTest(field=field), self.assertRaises(RuntimeError):
                check_file_metadata(dict(metadata, **{field: value}), metadata,
                                    self.context, self.context, content(0, 0), 23)
        changed = self.context[:24] + bytes(reversed(self.context[24:]))
        with self.assertRaises(RuntimeError):
            check_file_metadata(metadata, metadata, changed, self.context, content(0, 0), 23)

    def test_crc32c_published_check_value(self):
        self.assertEqual(crc32c(0xffffffff, b'123456789') ^ 0xffffffff, 0xe3069283)

    def test_formula_and_endpoint_model(self):
        self.assertEqual(counts(1024), (384, 64))
        self.assertEqual(counts(4096), (64, 32))
        self.assertEqual(len(bulk_name(447)), 255)
        self.assertEqual(bulk_name(0)[:12], b'Bulk-000000-')
        self.assertEqual(len(content(1, 4)), 161)
        old, new = namespace(4096, 'old'), namespace(4096, 'new')
        self.assertEqual(directory_parents(old)['child0'], 'main0')
        self.assertEqual(directory_parents(new)['child0'], 'peer0')
        self.assertEqual(new['main0'][bulk_name(2)], 'p0s4')
        self.assertEqual(new['peer0'][b'Exchange'], 'p0s2')
        self.assertNotIn(bulk_name(0), new['main0'])
        self.assertEqual(endpoint(old, old, new), 'old')
        self.assertEqual(endpoint(new, old, new), 'new')
        hybrid = dict(new, peer0=old['peer0'])
        with self.assertRaises(RuntimeError):
            endpoint(hybrid, old, new)
        for identity in ('p0s1', 'p2s1', 'main0', 'collision'):
            self.assertEqual(padding_for(identity), 0)
        for identity in ('p1s1', 'main1', 'grand1'):
            self.assertEqual(padding_for(identity), 3)

    def test_long_nokey_digest_and_parent_name_identity(self):
        import base64
        cipher, major, minor, name = expected_entry(self.provider, self.context, 0, bulk_name(0))
        raw = base64.urlsafe_b64decode(name + '=' * (-len(name) % 4))
        self.assertEqual(len(cipher), 255)
        self.assertEqual(raw, struct.pack('<II', major, minor) + cipher[:149] + hashlib.sha256(cipher[149:]).digest())
        expected = [('main0', name, 12), ('main0', name + 'A', 12)]
        exact_inventory(expected, expected)
        for changed in ([('main1', name, 12), expected[1]],
                        [('main0', name[:-1] + ('A' if name[-1] != 'A' else 'B'), 12), expected[1]],
                        [expected[0], expected[0]], [expected[0]]):
            with self.assertRaises(RuntimeError):
                exact_inventory(changed, expected)
        with self.assertRaises(RuntimeError):
            exact_inventory(expected, [expected[0], expected[0]])

    def test_real_continuation_routing(self):
        entries, topology = directory_tree(self.tree(), 11, 2, SEED, required_depth=0)
        self.assertEqual(len(entries), 2)
        self.assertEqual(topology['separators'], [101])
        self.assertNotEqual(topology['locations'][b'A' * 16], topology['locations'][b'B' * 16])

    def test_duplicate_leaf_missing_block_and_unowned_block(self):
        for mode in ('duplicate', 'missing', 'unowned'):
            tree = self.tree()
            if mode == 'duplicate':
                tree[0] = index([(0, 1), (101, 1)])
            elif mode == 'missing':
                del tree[2]
            else:
                tree[3] = leaf([])
            with self.subTest(mode=mode), self.assertRaises(RuntimeError):
                directory_tree(tree, 11, 2, SEED)

    def test_invalid_limit_count_depth_and_checksum(self):
        for offset, value, width in ((32, 0, 'H'), (34, 124, 'H'), (30, 2, 'B'), (1020, 0, 'I')):
            tree = self.tree()
            struct.pack_into('<' + width, tree[0], offset, value)
            with self.subTest(offset=offset), self.assertRaises(RuntimeError):
                directory_tree(tree, 11, 2, SEED)

    def test_routing_even_boundary_and_wrong_hash_minor(self):
        tree = self.tree()
        tree[0] = index([(0, 1), (100, 2)])
        with self.assertRaises(RuntimeError):
            directory_tree(tree, 11, 2, SEED)
        good, _ = directory_tree(self.tree(), 11, 2, SEED)
        tree = self.tree()
        tree[2] = leaf([(b'B' * 16, 13, 100, 10, 1)])
        changed, _ = directory_tree(tree, 11, 2, SEED)
        self.assertNotEqual(changed, good)
        tree[2] = leaf([(b'B' * 16, 13, 98, 9, 1)])
        with self.assertRaises(RuntimeError):
            directory_tree(tree, 11, 2, SEED)

    def test_depth_one_nodes_and_duplicate_names(self):
        tree = {0: index([(0, 3)], depth=1), 3: index([(0, 1), (101, 2)], root=False),
                1: self.tree()[1], 2: self.tree()[2]}
        self.assertEqual(directory_tree(tree, 11, 2, SEED, required_depth=1)[1]['depth'], 1)
        tree[2] = leaf([(b'A' * 16, 12, 100, 7, 1)])
        with self.assertRaises(RuntimeError):
            directory_tree(tree, 11, 2, SEED)

    def test_extent_ownership_holes_and_limits(self):
        self.assertEqual(bounded_mapping([(0, 20, 2, False)], 2048, 1024, 32768), {0: 20, 1: 21})
        for mapping, size in (([(0, 20, 2, True)], 2048), ([(1, 20, 1, False)], 2048),
                              ([(0, 20, 1, False), (1, 20, 1, False)], 2048),
                              ([(0, 20, 1, False)], 1025), ([(0, 20, 1, False)], 1049600)):
            with self.subTest(mapping=mapping), self.assertRaises(RuntimeError):
                bounded_mapping(mapping, size, 1024, 32768)

    def test_context_and_dot_identity_negatives(self):
        changed = bytearray(self.context)
        changed[3] = 3
        with self.assertRaises(RuntimeError):
            context_keys(changed, 0)
        block = leaf([], {b'.': 11, b'..': 2})
        self.assertEqual(leaf_entries(block, SEED, {b'.': 11, b'..': 2}), {})
        with self.assertRaises(RuntimeError):
            leaf_entries(block, SEED, {b'.': 11, b'..': 3})
        with self.assertRaises(RuntimeError):
            directory_tree(self.tree(), 12, 2, SEED)

    def test_frozen_evp_collision_and_strict_plan(self):
        # OpenSSL EVP birthday-search vector at the fixed public bytes0..15 nonce.
        plan = dict(schema='combined-indexed-collision-v1', context=self.context.hex(),
                    low=17933, a=32625, b=76089, high=44844)
        values = validate_collision_plan(self.provider, plan, self.context)
        self.assertEqual(values, {'low': (102232, 2136041350),
                                 'a': (2237583938, 2493600251),
                                 'b': (2237583938, 3091213632),
                                 'high': (4294939560, 3954223256)})
        names = namespace(1024, 'new', plan)['collision']
        self.assertNotEqual(names[collision_name(plan['a'])], names[collision_name(plan['b'])])
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / 'plan'
            raw = encode_plan(plan)
            path.write_bytes(raw)
            self.assertEqual(load_plan(path, self.provider, self.context,
                                       hashlib.sha256(raw).hexdigest()), plan)
            for malformed in (raw.replace(b'\n', b'\r\n'), raw + b'\n',
                              raw.replace(b'17933', b'017933'), raw[:-1]):
                path.write_bytes(malformed)
                with self.assertRaises(RuntimeError):
                    load_plan(path, self.provider, self.context)
        for key, value in (('b', plan['a']), ('low', plan['high']), ('a', 262144)):
            with self.subTest(key=key), self.assertRaises(RuntimeError):
                validate_collision_plan(self.provider, dict(plan, **{key: value}), self.context)

    def test_collision_plan_strict_framing_and_hash(self):
        # Validation rejects invented collisions without a costly birthday search in unit tests.
        plan = dict(schema='combined-indexed-collision-v1', context=self.context.hex(), low=0, a=1, b=2, high=3)
        with self.assertRaises(RuntimeError):
            validate_collision_plan(self.provider, plan, self.context)
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / 'plan'
            path.write_bytes(encode_plan(plan))
            with self.assertRaises(RuntimeError):
                load_plan(path, self.provider, self.context, '0' * 64)
            path.write_bytes(b'x' * 4097)
            with self.assertRaises(RuntimeError):
                load_plan(path, self.provider, self.context)


if __name__ == '__main__':
    unittest.main()

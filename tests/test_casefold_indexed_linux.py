#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
"""Pure/mock checks for the mounted indexed Linux driver; never mounts a volume."""
import copy
import errno
from pathlib import Path
import stat
import struct
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import patch

import linux_casefold_indexed as driver
from casefold_indexed_model import bulk_name, directory_parents, expected_data, namespace
from generate_encrypted_casefold import Provider, hkdf, known_answers


def baseline(block_size=4096):
    # Independently searched EVP vector for synthetic syscall tests only. The
    # mounted fixture always searches against the directory's real Linux nonce.
    plan = dict(schema='combined-indexed-collision-v1',
                context='0201040000000000a525b310d975604e26c761134e6c35d1'
                        '000102030405060708090a0b0c0d0e0f',
                low=17933, a=32625, b=76089, high=44844) if block_size == 1024 else None
    names = namespace(block_size, 'old', plan)
    result = dict(schema=driver.BASELINE_SCHEMA, block_size=block_size, root_inode=2,
                  directories={}, files={}, collision_plan=plan, public_directories={
                      'root': dict(inode=2, generation=0, type='directory', mode=0o755,
                                   links=3 + len(names['root']), size=block_size, flags=0x80000),
                      'lost+found': dict(inode=11, generation=0, type='directory', mode=0o700,
                                         links=2, size=4 * block_size, flags=0x80000)})
    files = sorted({target for entries in names.values() for target in entries.values()
                    if target not in names})
    identities = sorted(set(names) - {'root'}) + files
    for index, identity in enumerate(identities, 20):
        context = (bytes((2, 1, 4, driver.padding(identity), 0, 0, 0, 0)) +
                   hkdf(1, 16) + index.to_bytes(16, 'little'))
        section = 'directories' if identity in names else 'files'
        result[section][identity] = dict(inode=index, context=plan['context'] if identity == 'collision' else context.hex())
    return result


class FakeLinux:
    """Independent syscall state supporting only the verifier's read contract."""
    def __init__(self, source, provider, endpoint='new', keyed=True):
        self.source = source
        self.names = namespace(source['block_size'], endpoint, source['collision_plan'])
        self.endpoint = endpoint
        self.parents = directory_parents(self.names)
        self.identities = {**source['directories'], **source['files']}
        self.keyed = keyed
        self.fds = {'root': 9, 'lost+found': 8}
        self.fds.update({identity: index for index, identity in enumerate(self.identities, 10)})
        self.by_fd = {fd: identity for identity, fd in self.fds.items()}
        self.offsets = {}
        self.entries = {}
        self.metadata = {}
        self.public = copy.deepcopy(source['public_directories'])
        for identity, record in self.public.items():
            self.metadata[identity] = SimpleNamespace(st_ino=record['inode'], st_dev=77,
                st_mode=stat.S_IFDIR | record['mode'], st_nlink=record['links'], st_size=record['size'])
        self.parents['lost+found'] = 'root'
        for directory, entries in self.names.items():
            self.entries[directory] = {}
            for name, target in entries.items():
                if not keyed and directory != 'root':
                    name = driver.expected_entry(provider, bytes.fromhex(self.identities[directory]['context']),
                                                 driver.padding(directory), name)[3].encode()
                self.entries[directory][name] = target
        self.entries['root'][b'lost+found'] = 'lost+found'
        self.entries['lost+found'] = {}
        for identity, record in self.identities.items():
            is_directory = identity in self.names
            links = 2 + sum(target in self.names for target in self.names[identity].values()) if is_directory else sum(
                target == identity for entries in self.names.values() for target in entries.values())
            self.metadata[identity] = SimpleNamespace(st_ino=record['inode'], st_dev=77,
                st_mode=(stat.S_IFDIR | 0o700) if is_directory else (stat.S_IFREG | 0o600),
                st_nlink=links, st_size=0 if is_directory else len(expected_data(identity)))

    def resolve(self, name, dir_fd):
        directory = self.by_fd[dir_fd]
        if name == b'..':
            return self.parents[directory]
        entries = self.entries[directory]
        if name in entries:
            return entries[name]
        if self.keyed and directory != 'root':
            matches = [target for key, target in entries.items() if key.lower() == name.lower()]
            if len(matches) == 1:
                return matches[0]
        raise FileNotFoundError(errno.ENOENT, 'absent')

    def stat(self, name, *, dir_fd, follow_symlinks):
        return self.metadata[self.resolve(name, dir_fd)]

    def open(self, name, options, mode=0o777, *, dir_fd):
        identity = self.resolve(name, dir_fd)
        if not self.keyed and identity not in self.names and identity not in self.public:
            raise OSError(errno.ENOKEY, 'no key')
        self.offsets[self.fds[identity]] = 0
        return self.fds[identity]

    def read(self, fd, size):
        data = expected_data(self.by_fd[fd])
        offset = self.offsets[fd]
        self.offsets[fd] += size
        return data[offset:offset + size]

    def context(self, fd):
        return bytes.fromhex(self.identities[self.by_fd[fd]]['context'])

    def generation(self, fd):
        return self.public[self.by_fd[fd]]['generation']

    def flags(self, fd):
        identity = self.by_fd[fd]
        if identity in self.public:
            return self.public[identity]['flags']
        indexed = identity.startswith('main') or identity == 'collision' and self.endpoint in ('new', 'collision-new')
        return driver.ENCRYPT | driver.CASEFOLD | (driver.INDEX if indexed else 0)

    def install(self, stack):
        for target, function in (
                ('os.fstat', lambda fd: self.metadata[self.by_fd[fd]]),
                ('os.stat', self.stat), ('os.open', self.open), ('os.close', lambda fd: None),
                ('os.read', self.read), ('context', self.context), ('flags', self.flags),
                ('generation', self.generation),
                ('listed_names', lambda fd: set(self.entries[self.by_fd[fd]]))):
            stack.enter_context(patch('linux_casefold_indexed.' + target, side_effect=function))


class LinuxIndexed(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.provider = Provider()
        known_answers(cls.provider)

    def test_fixed_padding_identities(self):
        for identity in ('p0s1', 'p2s1', 'main0', 'peer0', 'child0', 'grand0', 'collision'):
            self.assertEqual(driver.padding(identity), 0)
        for identity in ('p1s0', 'p1s4', 'main1', 'peer1', 'child1', 'grand1'):
            self.assertEqual(driver.padding(identity), 3)

    def test_baseline_requires_fixed_original_identities(self):
        original = baseline()
        driver.validate_baseline(original, self.provider)
        changes = []
        changed = copy.deepcopy(original)
        changed['files']['extra'] = changed['files']['p0s0']
        changes.append(changed)
        changed = copy.deepcopy(original)
        changed['files']['p0s1'] = changed['files']['p0s0'].copy()
        changes.append(changed)
        changed = copy.deepcopy(original)
        changed['directories']['child0']['context'] = changed['directories']['main0']['context']
        changes.append(changed)
        changed = copy.deepcopy(original)
        changed['files']['p0s0']['context'] = '00' * 40
        changes.append(changed)
        changed = copy.deepcopy(original)
        changed['collision_plan'] = {}
        changes.append(changed)
        for changed in changes:
            with self.subTest(changed=changed), self.assertRaises(RuntimeError):
                driver.validate_baseline(changed, self.provider)

    def verify_fake(self, fake, endpoint='new'):
        from contextlib import ExitStack
        with ExitStack() as stack:
            fake.install(stack)
            return driver.verify(9, fake.source, endpoint, fake.keyed, self.provider)

    def test_keyed_and_nokey_complete_endpoints(self):
        for block_size in (1024, 4096):
            for endpoint in ('old', 'new'):
                for keyed in (False, True):
                    source = baseline(block_size)
                    driver.validate_baseline(source, self.provider)
                    fake = FakeLinux(source, self.provider, endpoint, keyed)
                    with self.subTest(block_size=block_size, endpoint=endpoint, keyed=keyed):
                        observations = self.verify_fake(fake, endpoint)
                        expected = sum(len(entries) for directory, entries in fake.names.items() if directory != 'root')
                        self.assertEqual(len(observations), expected)
                        self.assertEqual(len({(row['parent'], row['name']) for row in observations}), expected)
                        if not keyed:
                            lengths = [len(bytes.fromhex(row['name'])) for row in observations]
                            self.assertIn(252, lengths)

    def test_single_operation_crash_endpoints_both_key_modes(self):
        self.assertEqual(set(driver.ENDPOINTS), {'old', 'new', 'collision-new', 'rename-new'})
        for block_size, endpoint in ((1024, 'collision-new'), (1024, 'rename-new'), (4096, 'rename-new')):
            for keyed in (False, True):
                fake = FakeLinux(baseline(block_size), self.provider, endpoint, keyed)
                with self.subTest(block_size=block_size, endpoint=endpoint, keyed=keyed):
                    observations = self.verify_fake(fake, endpoint)
                    expected = sum(len(entries) for directory, entries in fake.names.items() if directory != 'root')
                    self.assertEqual(len(observations), expected)

    def test_single_operation_endpoint_rejects_other_completed_mutations(self):
        for endpoint in ('collision-new', 'rename-new'):
            for keyed in (False, True):
                # A complete full-batch namespace must never pass as the single
                # operation chosen independently for crash readback.
                fake = FakeLinux(baseline(1024), self.provider, 'new', keyed)
                with self.subTest(endpoint=endpoint, keyed=keyed), self.assertRaises(RuntimeError):
                    self.verify_fake(fake, endpoint)

    def test_single_operation_namespace_and_link_hybrids_refuse(self):
        for keyed in (False, True):
            for endpoint, inode, change in (('collision-new', 'p2s1', -1),
                                             ('old', 'p2s1', 1),
                                             ('rename-new', 'p0s1', -1)):
                fake = FakeLinux(baseline(1024), self.provider, endpoint, keyed)
                fake.metadata[inode].st_nlink += change
                with self.subTest(endpoint=endpoint, keyed=keyed, defect='links'), self.assertRaisesRegex(RuntimeError, 'links'):
                    self.verify_fake(fake, endpoint)
            fake = FakeLinux(baseline(1024), self.provider, 'rename-new', keyed)
            moved = b'Moved' if keyed else driver.expected_entry(
                self.provider, bytes.fromhex(fake.identities['peer0']['context']), 0, b'Moved')[3].encode()
            # Source removal with no destination insertion is neither endpoint.
            del fake.entries['peer0'][moved]
            with self.subTest(keyed=keyed, defect='missing destination'), self.assertRaisesRegex(RuntimeError, 'listing'):
                self.verify_fake(fake, 'rename-new')
            with self.subTest(keyed=keyed, defect='old source absent'), self.assertRaises(RuntimeError):
                self.verify_fake(fake, 'old')

    def test_nokey_hardlink_inventory_cannot_match_only_inodes(self):
        fake = FakeLinux(baseline(), self.provider, keyed=False)
        directory = fake.entries['main0']
        name, target = next(iter(directory.items()))
        del directory[name]
        directory[b'wrong-full-no-key-name'] = target
        with self.assertRaisesRegex(RuntimeError, 'complete directory listing'):
            self.verify_fake(fake)

    def test_wrong_inode_link_mode_context_and_parent_rejected(self):
        for defect in ('inode', 'links', 'mode', 'context', 'parent'):
            fake = FakeLinux(baseline(), self.provider)
            if defect == 'inode':
                fake.metadata['p0s0'].st_ino += 1000
            elif defect == 'links':
                fake.metadata['p0s3'].st_nlink -= 1
            elif defect == 'mode':
                fake.metadata['p1s4'].st_mode = stat.S_IFREG | 0o640
            elif defect == 'context':
                fake.identities = copy.deepcopy(fake.identities)
                fake.identities['p0s0']['context'] = fake.identities['p0s1']['context']
            else:
                fake.parents['child0'] = 'main0'
            with self.subTest(defect=defect), self.assertRaises(RuntimeError):
                self.verify_fake(fake)

    def test_wrong_plaintext_and_missing_alias_rejected(self):
        fake = FakeLinux(baseline(), self.provider)
        fake.read = lambda fd, size: b'wrong'
        with self.assertRaisesRegex(RuntimeError, 'plaintext'):
            self.verify_fake(fake)
        fake = FakeLinux(baseline(), self.provider)
        original = fake.stat
        def wrong_alias(name, **kwargs):
            if name == bulk_name(4).swapcase():
                return SimpleNamespace(st_ino=0, st_dev=77)
            return original(name, **kwargs)
        fake.stat = wrong_alias
        with self.assertRaisesRegex(RuntimeError, 'alias'):
            self.verify_fake(fake)

    def test_no_key_must_refuse_file_open_with_enokey(self):
        for error in (errno.EACCES, None):
            fake = FakeLinux(baseline(), self.provider, keyed=False)
            original = fake.open
            def wrong_open(name, options, mode=0o777, *, dir_fd):
                identity = fake.resolve(name, dir_fd)
                if identity not in fake.names and identity not in fake.public:
                    if error is not None:
                        raise OSError(error, 'unexpected')
                    return fake.fds[identity]
                return original(name, options, mode, dir_fd=dir_fd)
            fake.open = wrong_open
            with self.subTest(error=error), self.assertRaises(RuntimeError):
                self.verify_fake(fake)

    def test_exact_public_root_inventory(self):
        for keyed in (False, True):
            for defect in ('extra', 'missing-indexed', 'missing-lost'):
                fake = FakeLinux(baseline(), self.provider, keyed=keyed)
                if defect == 'extra':
                    fake.entries['root'][b'Unexpected'] = 'p0s0'
                elif defect == 'missing-indexed':
                    del fake.entries['root'][b'indexed-0']
                else:
                    del fake.entries['root'][b'lost+found']
                with self.subTest(keyed=keyed, defect=defect), self.assertRaisesRegex(RuntimeError, 'root inventory'):
                    self.verify_fake(fake)

    def test_public_directory_metadata_and_empty_lost_found(self):
        for keyed in (False, True):
            for directory in ('root', 'lost+found'):
                for field in ('inode', 'type', 'mode', 'links', 'size', 'flags', 'generation'):
                    fake = FakeLinux(baseline(), self.provider, keyed=keyed)
                    metadata = fake.metadata[directory]
                    if field == 'inode':
                        metadata.st_ino += 100
                    elif field == 'type':
                        metadata.st_mode = stat.S_IFREG | 0o700
                    elif field == 'mode':
                        metadata.st_mode ^= 0o010
                    elif field == 'links':
                        metadata.st_nlink += 1
                    elif field == 'size':
                        metadata.st_size += 4096
                    else:
                        fake.public[directory][field] += 1
                    with self.subTest(keyed=keyed, directory=directory, field=field), self.assertRaises(RuntimeError):
                        self.verify_fake(fake)
            fake = FakeLinux(baseline(), self.provider, keyed=keyed)
            fake.entries['lost+found'][b'Recovered'] = 'p0s0'
            with self.subTest(keyed=keyed, defect='contents'), self.assertRaisesRegex(RuntimeError, 'not empty'):
                self.verify_fake(fake)
            fake = FakeLinux(baseline(), self.provider, keyed=keyed)
            fake.parents['lost+found'] = 'main0'
            with self.subTest(keyed=keyed, defect='parent'), self.assertRaisesRegex(RuntimeError, 'parent changed'):
                self.verify_fake(fake)

    def test_public_baseline_requires_exact_shape_and_root_links(self):
        for defect in ('missing', 'extra', 'root-links', 'lost-links', 'type'):
            source = baseline()
            if defect == 'missing':
                del source['public_directories']['lost+found']
            elif defect == 'extra':
                source['public_directories']['extra'] = source['public_directories']['root'].copy()
            elif defect == 'type':
                source['public_directories']['lost+found']['type'] = 'regular'
            else:
                source['public_directories']['root' if defect == 'root-links' else 'lost+found']['links'] += 1
            with self.subTest(defect=defect), self.assertRaises(RuntimeError):
                driver.validate_baseline(source, self.provider)

    def test_generation_ioctl_abi(self):
        def ioctl(fd, command, argument, mutate):
            self.assertEqual((fd, command, len(argument), mutate), (9, 0x80087601, 8, True))
            struct.pack_into('<I', argument, 0, 0xfedcba98)
        with patch.object(driver.fcntl, 'ioctl', side_effect=ioctl):
            self.assertEqual(driver.generation(9), 0xfedcba98)

    def test_add_key_abi_and_scrubbing(self):
        references = []
        def ioctl(fd, command, argument, mutate):
            self.assertEqual((fd, command, len(argument), mutate), (3, driver.ADD_KEY, 144, True))
            self.assertEqual(struct.unpack_from('<I', argument, 0)[0], 2)
            self.assertEqual(struct.unpack_from('<I', argument, 40)[0], 64)
            self.assertEqual(argument[80:], bytes((i * 7 + 3) & 255 for i in range(64)))
            argument[8:24] = hkdf(1, 16)
            references.append(argument)
        with patch.object(driver.fcntl, 'ioctl', side_effect=ioctl):
            driver.add_key(3)
        self.assertEqual(references[0], bytes(144))

    def test_external_evidence_and_no_replacement(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            mount = root / 'mount'
            mount.mkdir()
            with self.assertRaises(RuntimeError):
                driver.outside_mount(mount / 'baseline.json', mount)
            driver.outside_mount(root / 'baseline.json', mount)
            path = root / 'baseline.json'
            driver.write_json(path, {'passed': True})
            with self.assertRaises(FileExistsError):
                driver.write_json(path, {'passed': False})


if __name__ == '__main__':
    unittest.main()

#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
"""Bounded independent raw-media oracle for the fixed indexed Linux/core scenario.

Debugfs resolves inode records, xattrs and extent runs only. This decoder owns
HTree topology, directory checksums, routing and exact encrypted-name inventory.
Original Linux media, the fixed formulas and a hash-pinned pre-core collision
plan are the sole expectation sources; no core-authored model is consumed.
"""
import argparse
import json
from pathlib import Path
import struct
import subprocess

from casefold_indexed_model import (namespace, root_directories, padding_for, expected_data,
    expected_entry, directory_parents, exact_inventory, load_plan, collision_name, bulk_name, link_counts)
from check_encrypted_casefold_native import (require, context_keys, format_fields,
    debugfs_success, extract_context, digest, ENCRYPT, CASEFOLD, INDEX, INLINE)
from check_namespace import inode_fields
from check_sustained import read_encrypted_file
from check_verity_enable import extents
from generate_encrypted_casefold import Provider, known_answers
from generate_fixtures import resolve_tools

MAX_DIRECTORY_BYTES = 1024 * 1024
MAX_ENTRIES = 512


def crc32c(seed, data):
    for byte in data:
        seed ^= byte
        for _ in range(8):
            seed = (seed >> 1) ^ (0x82f63b78 if seed & 1 else 0)
    return seed


def inode_seed(superblock, number, generation):
    incompat = struct.unpack_from('<I', superblock, 96)[0]
    seed = (struct.unpack_from('<I', superblock, 0x270)[0] if incompat & 0x2000
            else crc32c(0xffffffff, superblock[104:120]))
    return crc32c(crc32c(seed, struct.pack('<I', number)), struct.pack('<I', generation))


def bounded_mapping(mapping, size, block_size, image_size):
    require(0 < size <= MAX_DIRECTORY_BYTES and size % block_size == 0,
            'Directory size outside bounded whole-block model')
    result, physical = {}, set()
    require(0 < len(mapping) <= 1024, 'Extent count exceeds bound')
    for logical, start, count, unwritten in mapping:
        require(type(logical) is int and type(start) is int and type(count) is int and
                not unwritten and logical >= 0 and start > 0 and 0 < count <= size // block_size and
                logical + count <= size // block_size and (start + count) * block_size <= image_size,
                'Extent bounds/unwritten mapping')
        for offset in range(count):
            require(logical + offset not in result and start + offset not in physical,
                    'Overlapping logical/physical extent ownership')
            result[logical + offset] = start + offset
            physical.add(start + offset)
    require(set(result) == set(range(size // block_size)), 'Directory holes or unmapped tail')
    return result


def index_entries(block, base, seed):
    limit, count = struct.unpack_from('<HH', block, base)
    require(limit == (len(block) - base - 8) // 8 and 0 < count <= limit,
            'Invalid index count/limit')
    tail = base + limit * 8
    reserved, checksum = struct.unpack_from('<II', block, tail)
    require(reserved == 0 and tail + 8 == len(block), 'Invalid index checksum tail')
    actual = crc32c(crc32c(seed, block[:base + count * 8]), bytes(8))
    require(actual == checksum, 'Index checksum mismatch')
    result = []
    for i in range(count):
        major, logical = struct.unpack_from('<II', block, base + i * 8)
        require(0 < logical < MAX_DIRECTORY_BYTES // len(block), 'Index child out of bounds')
        result.append((0 if i == 0 else major, logical))
    require(all(result[i - 1][0] <= result[i][0] for i in range(1, count)),
            'Index separator order')
    return result


def leaf_entries(block, seed, dots=None):
    require(block[-12:-4] == struct.pack('<IHBB', 0, 12, 0, 0xde), 'Missing directory tail')
    require(crc32c(seed, block[:-12]) == struct.unpack_from('<I', block, len(block) - 4)[0],
            'Leaf checksum mismatch')
    entries, seen_dots = {}, {}
    offset = 0
    while offset < len(block) - 12:
        require(offset + 8 <= len(block) - 12, 'Truncated directory header')
        number, length, size, kind = struct.unpack_from('<IHBB', block, offset)
        require(length >= 8 and length % 4 == 0 and offset + length <= len(block) - 12 and
                size <= length - 8, 'Invalid directory record bounds')
        name = bytes(block[offset + 8:offset + 8 + size])
        if number and name in (b'.', b'..'):
            require(dots is not None and kind == 2 and name not in seen_dots and length >= 12,
                    'Unexpected/duplicate dot record')
            seen_dots[name] = number
        elif number:
            used = 8 + ((size + 3) & ~3) + 8
            require(16 <= size <= 255 and length >= used and kind in (1, 2) and name not in entries,
                    'Invalid/duplicate encrypted leaf record')
            major, minor = struct.unpack_from('<II', block, offset + used - 8)
            require(major & 1 == 0 and major != 0xfffffffe, 'Invalid stored major hash')
            entries[name] = (number, major, minor, kind)
        offset += length
    require(seen_dots == (dots or {}), 'Dot identities differ')
    return entries


def directory_tree(blocks, number, parent, seed, indexed=True, required_depth=None):
    require(isinstance(blocks, dict) and blocks and 0 in blocks and
            len(blocks) * len(blocks[0]) <= MAX_DIRECTORY_BYTES and
            len(blocks[0]) in (1024, 4096) and
            all(len(block) == len(blocks[0]) for block in blocks.values()), 'Directory block bounds')
    if not indexed:
        require(set(blocks) == {0} and required_depth is None, 'Nonlinear unindexed directory')
        entries = leaf_entries(blocks[0], seed, {b'.': number, b'..': parent})
        require(len(entries) <= MAX_ENTRIES, 'Entry count bound')
        return entries, {'depth': None, 'leaves': [0], 'locations': {name: 0 for name in entries},
                         'separators': []}
    root = blocks[0]
    require(struct.unpack_from('<IHBB', root, 0) == (number, 12, 1, 2) and root[8:9] == b'.' and
            struct.unpack_from('<IHBB', root, 12) == (parent, len(root) - 12, 2, 2) and
            root[20:22] == b'..', 'Indexed root dot identities/records')
    reserved, version, length, depth, flags = struct.unpack_from('<IBBBB', root, 24)
    require((reserved, version, length, flags) == (0, 6, 8, 0) and depth in (0, 1),
            'Unsupported index root/hash/depth')
    require(required_depth is None or depth == required_depth, 'Required index depth missing')
    roots = index_entries(root, 32, seed)
    visited, leaves = {0}, []
    def claim(logical):
        require(logical in blocks and logical not in visited, 'Missing/duplicate index ownership')
        visited.add(logical)
    for position, (lower, logical) in enumerate(roots):
        upper = roots[position + 1][0] if position + 1 < len(roots) else 0x100000000
        if depth == 0:
            claim(logical)
            leaves.append((lower, upper, logical))
        else:
            claim(logical)
            node = blocks[logical]
            require(struct.unpack_from('<IHBB', node, 0) == (0, len(node), 0, 0), 'Invalid node header')
            children = index_entries(node, 8, seed)
            for index, (separator, child) in enumerate(children):
                boundary = lower if index == 0 else separator
                end = children[index + 1][0] if index + 1 < len(children) else upper
                require(lower <= boundary <= end <= upper, 'Node routing outside parent range')
                claim(child)
                leaves.append((boundary, end, child))
    require(visited == set(blocks), 'Unowned directory block')
    entries, locations = {}, {}
    for lower, upper, logical in leaves:
        for name, entry in leaf_entries(blocks[logical], seed).items():
            require(name not in entries, 'Duplicate encrypted name across leaves')
            major = entry[1]
            require((lower & ~1) <= major and
                    (major < (upper & ~1) or (upper & 1 and major == (upper & ~1))),
                    'Leaf hash violates HTree routing interval')
            entries[name], locations[name] = entry, logical
    require(len(entries) <= MAX_ENTRIES, 'Entry count bound')
    return entries, {'depth': depth, 'leaves': [row[2] for row in leaves],
                     'locations': locations, 'separators': [row[0] for row in leaves[1:]]}


def check_file_metadata(metadata, original, context, original_context, payload, links):
    require(context == original_context and metadata['inode'] == original['inode'] and
            metadata['generation'] == original['generation'] and metadata['type'] == 'regular' and
            metadata['mode'] == 0o600 and metadata['size'] == len(payload) and metadata['links'] == links
            and metadata['flags'] & (ENCRYPT | CASEFOLD | INLINE) == ENCRYPT,
            'File identity/context/mode/link count/flags/size differ')


def check_payload(actual, expected):
    require(actual == expected, 'Independent XTS plaintext differs')


def select_endpoint(requested, target_present, source_present=None):
    if requested == 'auto-collision':
        return 'collision-new' if target_present else 'old'
    require(requested == 'auto-rename' and source_present is not None and
            source_present != target_present, 'Rename source/destination hybrid duplication or loss')
    return 'rename-new' if target_present else 'old'


def resolve_endpoint(media, requested, bindings, contexts, provider, plan):
    if requested not in ('auto-collision', 'auto-rename'):
        return requested
    if requested == 'auto-collision':
        require(media.block_size == 1024 and plan is not None, 'Auto-collision requires1KiB')
        identity, name = 'collision', collision_name(plan['b'])
    else:
        identity, name = 'peer0', b'Moved'
    metadata = media.inode('<%d>' % bindings[identity]['inode'])
    entries, _ = media.directory(metadata, 2)
    cipher = expected_entry(provider, contexts[identity], padding_for(identity), name)[0]
    target_present = cipher in entries
    source_present = None
    if requested == 'auto-rename':
        metadata = media.inode('<%d>' % bindings['main0']['inode'])
        entries, _ = media.directory(metadata, 2, 1 if media.block_size == 1024 else 0)
        cipher = expected_entry(provider, contexts['main0'], 0, bulk_name(1))[0]
        source_present = cipher in entries
    return select_endpoint(requested, target_present, source_present)


PUBLIC_FIELDS = ('inode', 'generation', 'type', 'mode', 'links', 'size', 'flags')


def public_identity(metadata):
    require(metadata['type'] == 'directory' and metadata['flags'] & (ENCRYPT | CASEFOLD | INDEX | INLINE) == 0,
            'Unexpected public directory type/flags')
    return {field: metadata[field] for field in PUBLIC_FIELDS}


def check_public_metadata(current, original):
    require(set(current) == {'root', 'lost+found'} and current == original,
            'Root/lost+found metadata changed')


def public_entries(block, seed, dots=None):
    require(block[-12:-4] == struct.pack('<IHBB', 0, 12, 0, 0xde) and
            crc32c(seed, block[:-12]) == struct.unpack_from('<I', block, len(block) - 4)[0],
            'Public directory checksum/tail mismatch')
    result, seen_dots, offset = {}, {}, 0
    while offset < len(block) - 12:
        require(offset + 8 <= len(block) - 12, 'Public directory truncated header')
        number, length, size, kind = struct.unpack_from('<IHBB', block, offset)
        require(length >= 8 and length % 4 == 0 and offset + length <= len(block) - 12 and
                size <= length - 8, 'Public directory record bounds')
        name = bytes(block[offset + 8:offset + 8 + size])
        if number:
            require(kind == 2 and name and b'/' not in name and b'\0' not in name,
                    'Unexpected public directory entry')
            target = seen_dots if name in (b'.', b'..') else result
            require(name not in target, 'Duplicate public directory entry')
            target[name] = number
        offset += length
    require(seen_dots == (dots or {}), 'Public directory dot identities differ')
    return result


def save_report(output, report):
    (output / 'report.json').write_text(json.dumps(report, indent=2) + '\n')


class Media:
    def __init__(self, path, tools, output, report):
        self.path, self.tools, self.output, self.report = path, tools, output, report
        require(path.stat().st_size == 32 * 1024 * 1024, 'Unexpected image size')
        self.before = digest(path)
        self.report['images'].append(dict(path=str(path), sha256=self.before, verified=False))
        save_report(self.output, self.report)
        with path.open('rb') as stream:
            stream.seek(1024)
            self.superblock = stream.read(1024)
        self.block_size, self.identity = format_fields(self.superblock)
        require(struct.unpack_from('<I', self.superblock, 100)[0] & 0x400,
                'Metadata checksums required')
        self.owners = {}
        self.run([tools['e2fsck'], '-fn', path])

    def run(self, command):
        try:
            result = subprocess.run([str(part) for part in command], capture_output=True,
                                    text=True, errors='backslashreplace', timeout=120)
        except (OSError, subprocess.TimeoutExpired) as error:
            def text(value):
                return value.decode('utf-8', 'backslashreplace') if isinstance(value, bytes) else value
            self.report['commands'].append(dict(command=list(map(str, command)), status=None,
                error=str(error), stdout=text(getattr(error, 'stdout', None)),
                stderr=text(getattr(error, 'stderr', None))))
            save_report(self.output, self.report)
            raise
        self.report['commands'].append(dict(command=list(map(str, command)), status=result.returncode,
                                           stdout=result.stdout, stderr=result.stderr))
        save_report(self.output, self.report)
        result.check_returncode()
        if str(command[0]) == str(self.tools['debugfs']):
            debugfs_success(result)
        return result.stdout

    def inode(self, selector):
        result = inode_fields(self.run([self.tools['debugfs'], '-R', 'stat ' + str(selector), self.path]))
        require(result is not None, 'Missing expected inode')
        return result

    def context(self, number):
        return extract_context(self.run, self.tools['debugfs'], self.path, number,
                               self.output / 'context.tmp')

    def mapping(self, metadata):
        if metadata['type'] == 'directory':
            require(metadata['size'] > 0 and metadata['size'] % self.block_size == 0,
                    'Directory size must be whole-block before extent mapping')
        mapping = extents(self.run([self.tools['debugfs'], '-R',
                                   f'dump_extents <{metadata["inode"]}>', self.path]))
        # Even small files own full data blocks; regular file sizes are not block aligned.
        size = ((metadata['size'] + self.block_size - 1) // self.block_size) * self.block_size
        physical = bounded_mapping(mapping, size, self.block_size, self.path.stat().st_size)
        for block in physical.values():
            require(block not in self.owners or self.owners[block] == metadata['inode'],
                    'Cross-inode data-block ownership alias')
            self.owners[block] = metadata['inode']
        return mapping, physical

    def directory(self, metadata, parent, depth=None):
        _, mapping = self.mapping(metadata)
        blocks = {}
        with self.path.open('rb') as stream:
            for logical, physical in mapping.items():
                stream.seek(physical * self.block_size)
                blocks[logical] = stream.read(self.block_size)
        return directory_tree(blocks, metadata['inode'], parent,
                              inode_seed(self.superblock, metadata['inode'], metadata['generation']),
                              bool(metadata['flags'] & INDEX), depth)


    def public_directory(self, metadata, parent):
        public_identity(metadata)
        _, mapping = self.mapping(metadata)
        entries = {}
        seed = inode_seed(self.superblock, metadata['inode'], metadata['generation'])
        with self.path.open('rb') as stream:
            for logical, physical in mapping.items():
                stream.seek(physical * self.block_size)
                block = stream.read(self.block_size)
                require(len(block) == self.block_size, 'Short public directory block')
                decoded = public_entries(block, seed,
                    {b'.': metadata['inode'], b'..': parent} if logical == 0 else None)
                require(not set(entries) & set(decoded), 'Duplicate public name across blocks')
                entries.update(decoded)
        require(len(entries) <= MAX_ENTRIES, 'Public directory entry bound')
        return entries


def _check_media(original, exported, tools, output, report, plan_path=None, plan_hash=None,
                 nokey_path=None, selected_endpoint='new'):
    require(selected_endpoint in ('old', 'new', 'collision-new', 'rename-new',
                                  'auto-collision', 'auto-rename'), 'Unknown requested endpoint')
    provider = Provider()
    known_answers(provider)
    report['provider'] = provider.version
    bindings, contexts, nonces = {}, {}, set()
    baseline = Media(original, tools, output, report)
    plan = None
    if baseline.block_size == 1024:
        require(plan_path is not None and plan_hash is not None, 'Hash-pinned collision plan required')
        collision = baseline.inode('/indexed-collision')
        plan = load_plan(plan_path, provider, baseline.context(collision['inode']), plan_hash)
    old_names = namespace(baseline.block_size, 'old', plan)
    expected_nokey = []
    public_directories = {}
    for media, original_image in ((baseline, True), (Media(exported, tools, output, report), False)):
        if original_image:
            names = old_names
        else:
            selected_endpoint = resolve_endpoint(media, selected_endpoint, bindings, contexts, provider, plan)
            report['resolved_endpoint'] = selected_endpoint
            names = namespace(baseline.block_size, selected_endpoint, plan)
        require(media.identity == baseline.identity and media.block_size == baseline.block_size,
                'Filesystem format/UUID/Unicode identity changed')
        current_public = {'root': media.inode('/'), 'lost+found': media.inode('/lost+found')}
        identities_public = {key: public_identity(value) for key, value in current_public.items()}
        require(current_public['root']['inode'] == 2 and current_public['lost+found']['inode'] != 2
                and current_public['lost+found']['links'] == 2, 'Public inode/link identities differ')
        if original_image:
            public_directories = identities_public
        check_public_metadata(identities_public, public_directories)
        require(media.public_directory(current_public['lost+found'], 2) == {},
                'lost+found is not empty')
        parents = directory_parents(names)
        links = link_counts(names)
        if original_image:
            bindings['root'] = media.inode('/')
            require(bindings['root']['inode'] == 2, 'Unexpected root inode')
            for name, identity in root_directories(media.block_size).items():
                bindings[identity] = media.inode('/' + name.decode('ascii'))
        else:
            for name, identity in root_directories(media.block_size).items():
                require(media.inode('/' + name.decode('ascii'))['inode'] == bindings[identity]['inode'],
                        'Root parent record changed')
        expected_root = {name: bindings[identity]['inode']
                         for name, identity in root_directories(media.block_size).items()}
        expected_root[b'lost+found'] = current_public['lost+found']['inode']
        require(media.public_directory(current_public['root'], 2) == expected_root,
                'Exact public root inventory differs')
        require(current_public['root']['links'] == 2 + len(expected_root),
                'Root link count differs from fixed directory inventory')
        queue = list(root_directories(media.block_size).values())
        for identity in queue:
            metadata = media.inode('<%d>' % bindings[identity]['inode'])
            padding = padding_for(identity)
            context = media.context(metadata['inode'])
            context_keys(context, padding)
            if original_image:
                require(context[24:] not in nonces, 'Repeated original inode nonce')
                nonces.add(context[24:])
                contexts[identity] = context
            require(context == contexts[identity] and metadata['generation'] == bindings[identity]['generation'],
                    'Directory context/generation changed')
            require(metadata['type'] == 'directory' and metadata['mode'] == 0o700 and
                    metadata['links'] == links[identity] and
                    metadata['flags'] & (ENCRYPT | CASEFOLD | INLINE) == ENCRYPT | CASEFOLD,
                    'Directory type/mode/link count/flags differ')
            expected_index = bool(bindings[identity]['flags'] & INDEX)
            if identity == 'collision' and not original_image and selected_endpoint in ('new', 'collision-new'):
                expected_index = True
            require(bool(metadata['flags'] & INDEX) == expected_index,
                    'Directory indexed state differs from resolved endpoint')
            depth = (1 if media.block_size == 1024 else 0) if identity.startswith('main') else None
            require(depth is None or metadata['flags'] & INDEX, 'Bulk directory is not indexed')
            entries, topology = media.directory(metadata, bindings[parents[identity]]['inode'], depth)
            if identity.startswith('main'):
                require(len(topology['leaves']) > 1, 'Multiple bulk leaves required')
            expected = {}
            for name, child in names[identity].items():
                cipher, major, minor, nokey = expected_entry(provider, context, padding, name)
                require(cipher not in expected, 'Duplicate independently expected ciphertext')
                if original_image:
                    require(cipher in entries, 'Missing Linux-authored expected name')
                    number = entries[cipher][0]
                    if child not in bindings:
                        bindings[child] = media.inode('<%d>' % number)
                    require(bindings[child]['inode'] == number, 'Original hardlink identity differs')
                expected[cipher] = (bindings[child]['inode'], major, minor, 2 if child in names else 1)
                if child in names:
                    queue.append(child)
                if not original_image:
                    expected_nokey.append((identity, nokey, bindings[child]['inode']))
            require(entries == expected, 'Exact ciphertext/inode/type/major/minor inventory differs')
            if identity == 'collision' and not original_image and selected_endpoint in ('new', 'collision-new'):
                a = expected_entry(provider, context, padding, collision_name(plan['a']))
                b = expected_entry(provider, context, padding, collision_name(plan['b']))
                require(topology['depth'] == 0 and topology['locations'][a[0]] != topology['locations'][b[0]]
                        and (a[1] | 1) in topology['separators'],
                        'Real collision lacks distinct leaves and odd-major continuation separator')
            report['directories'].append(dict(image=str(media.path), identity=identity,
                                               entries=len(entries), depth=topology['depth'],
                                               leaves=len(topology['leaves'])))
        identities = {item for entries in names.values() for item in entries.values()} - set(names)
        require(len({bindings[item]['inode'] for item in bindings}) == len(bindings),
                'Different model objects alias one inode')
        for identity in sorted(identities):
            metadata = media.inode('<%d>' % bindings[identity]['inode'])
            payload = expected_data(identity)
            context = media.context(metadata['inode'])
            _, _, key = context_keys(context, padding_for(identity))
            if original_image:
                require(context[24:] not in nonces, 'Repeated original inode nonce')
                nonces.add(context[24:])
                contexts[identity] = context
            check_file_metadata(metadata, bindings[identity], context, contexts[identity],
                                payload, links[identity])
            mapping, _ = media.mapping(metadata)
            check_payload(read_encrypted_file(media.path, mapping, media.block_size, 0, len(payload), key, True),
                          payload)
        require(digest(media.path) == media.before, 'Oracle changed media')
        next(row for row in report['images'] if row['path'] == str(media.path))['verified'] = True
    report['expected_nokey'] = expected_nokey
    if nokey_path is not None:
        require(nokey_path.stat().st_size <= 512 * 1024, 'No-key inventory bound')
        observed = []
        for line in nokey_path.read_text().splitlines():
            parent, number, name = line.split()
            require(len(name) <= 252 and all(char in 'ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_'
                                           for char in name), 'Malformed no-key envelope')
            observed.append((parent, name, int(number)))
        exact_inventory(observed, expected_nokey)
    report['baseline'] = dict(schema='combined-indexed-linux-baseline-v1', block_size=baseline.block_size,
        root_inode=2, public_directories=public_directories, directories={key: dict(inode=bindings[key]['inode'], context=contexts[key].hex())
                                  for key in old_names if key != 'root'},
        files={key: dict(inode=value['inode'], context=contexts[key].hex())
               for key, value in bindings.items() if key not in old_names}, collision_plan=plan)
    if plan_path is not None:
        require(digest(plan_path) == plan_hash, 'Pre-core collision plan changed')
    report['passed'] = True
    return report


def check_media(original, exported, tools, output, plan_path=None, plan_hash=None,
                nokey_path=None, selected_endpoint='new'):
    report = dict(passed=False, scope='indexed-independent-media', commands=[], images=[], directories=[],
                  original=str(original), exported=str(exported), endpoint=selected_endpoint,
                  collision_plan=str(plan_path) if plan_path is not None else None,
                  collision_plan_sha256=plan_hash)
    try:
        report['sources'] = []
        for path in (original, exported):
            source = {'path': str(path)}
            report['sources'].append(source)
            try:
                source['size'] = path.stat().st_size
                if source['size'] <= 32 * 1024 * 1024:
                    source['sha256'] = digest(path)
            except OSError as error:
                source['error'] = str(error)
        save_report(output, report)
        return _check_media(original, exported, tools, output, report, plan_path, plan_hash,
                            nokey_path, selected_endpoint)
    except Exception as error:
        report['error'] = dict(type=type(error).__name__, message=str(error))
        raise
    finally:
        save_report(output, report)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--original', required=True, type=Path)
    parser.add_argument('--exports', required=True, type=Path)
    parser.add_argument('--tools-root', required=True, type=Path)
    parser.add_argument('--output', required=True, type=Path)
    parser.add_argument('--collision-plan', type=Path)
    parser.add_argument('--collision-plan-sha256')
    parser.add_argument('--endpoint', choices=('old', 'new', 'collision-new', 'rename-new',
                                              'auto-collision', 'auto-rename'), default='new')
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=False)
    report = check_media(args.original, args.exports / ('indexed-' + args.original.name),
                         resolve_tools(args.tools_root), args.output, args.collision_plan,
                         args.collision_plan_sha256, args.exports / 'indexed.nokey', args.endpoint)
    (args.output / 'report.json').write_text(json.dumps(report, indent=2) + '\n')
    (args.output / 'baseline.json').write_text(json.dumps(report['baseline'], indent=2) + '\n')
    print('PASS independent indexed media: original+export, topology, names, contexts, links, contents')


if __name__ == '__main__':
    main()

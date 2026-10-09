#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
"""Fixed indexed scenario, independent of core manifests and host Unicode data."""
import base64
from collections import Counter
import hashlib
import struct

from check_encrypted_casefold_native import context_keys, require
from generate_encrypted_casefold import hashes

ALPHABET = b'ABCDEFGHIJKLMNOPQRSTUVWXYZ'
PLAN_SCHEMA = 'combined-indexed-collision-v1'
MAX_CANDIDATES = 262144


def counts(block_size):
    require(block_size in (1024, 4096), 'Unsupported indexed block size')
    return (384, 64) if block_size == 1024 else (64, 32)


def fixed_name(prefix, index):
    require(type(index) is int and 0 <= index < MAX_CANDIDATES, 'Name index bound')
    head = (prefix + '-%06d-' % index).encode('ascii')
    return head + (ALPHABET * 10)[:255 - len(head)]


def bulk_name(index):
    return fixed_name('Bulk', index)


def collision_name(index):
    return fixed_name('Collision', index)


def content(parent, slot):
    require(parent in (0, 1, 2) and slot in range(5), 'Payload identity bound')
    return bytes((parent * 53 + slot * 29 + at * 13) & 255 for at in range(97 + slot * 16))


def expected_names(block_size, parent, endpoint='new'):
    require(parent in (0, 1) and endpoint in ('old', 'new'), 'Unknown endpoint/parent')
    baseline, added = counts(block_size)
    return {bulk_name(i): i % 4 for i in range(baseline + (added if endpoint == 'new' else 0))}


def expected_entry(provider, context, padding, name):
    require(isinstance(name, bytes) and 1 <= len(name) <= 255 and
            all(0 < char < 128 and char != 47 for char in name), 'Fixed ASCII name required')
    cts, sip, _ = context_keys(context, padding)
    length = min(255, ((max(16, len(name)) + (4 << padding) - 1) // (4 << padding)) * (4 << padding))
    cipher = provider.cts(cts, name.ljust(length, b'\0'))
    major, minor = hashes(provider, sip, name.lower())
    envelope = struct.pack('<II', major, minor) + cipher[:149]
    if len(cipher) > 149:
        envelope += hashlib.sha256(cipher[149:]).digest()
    return cipher, major, minor, base64.urlsafe_b64encode(envelope).rstrip(b'=').decode('ascii')


def validate_collision_plan(provider, plan, context):
    require(isinstance(plan, dict) and set(plan) == {'schema', 'context', 'low', 'a', 'b', 'high'},
            'Collision plan fields differ')
    require(plan['schema'] == PLAN_SCHEMA and plan['context'] == bytes(context).hex(),
            'Collision plan schema/context changed')
    _, sip, _ = context_keys(context, 0)
    indices = [plan[key] for key in ('low', 'a', 'b', 'high')]
    require(all(type(i) is int and 0 <= i < MAX_CANDIDATES for i in indices) and
            len(set(indices)) == 4, 'Collision candidate identities invalid')
    values = [hashes(provider, sip, collision_name(i).lower()) for i in indices]
    low, a, b, high = values
    require(low[0] < a[0] == b[0] < high[0] and a[1] != b[1],
            'Collision must have equal major, different minor and two strict guards')
    return dict(zip(('low', 'a', 'b', 'high'), values))


def collision_plan(provider, context):
    _, sip, _ = context_keys(context, 0)
    seen = {}
    smallest = largest = pair = None
    for i in range(MAX_CANDIDATES):
        major, minor = hashes(provider, sip, collision_name(i).lower())
        if smallest is None or major < smallest[0]:
            smallest = (major, i)
        if largest is None or major > largest[0]:
            largest = (major, i)
        prior = seen.get(major)
        if pair is None and prior is not None and prior[1] != minor:
            pair = (major, prior[0], i)
        seen.setdefault(major, (i, minor))
        if pair and smallest[0] < pair[0] < largest[0]:
            plan = dict(schema=PLAN_SCHEMA, context=bytes(context).hex(), low=smallest[1],
                        a=pair[1], b=pair[2], high=largest[1])
            validate_collision_plan(provider, plan, context)
            return plan
    raise RuntimeError('No guarded independent collision within 262144 candidates')


def load_plan(path, provider, context, expected_sha256=None):
    require(path.stat().st_size <= 4096, 'Oversized collision plan')
    raw = path.read_bytes()
    if expected_sha256 is not None:
        require(hashlib.sha256(raw).hexdigest() == expected_sha256, 'Collision plan changed')
    try:
        lines = raw.decode('ascii').splitlines()
        require(len(lines) == 3 and raw.endswith(b'\n'), 'Collision plan line framing')
        values = lines[2].split(' ')
        require(len(values) == 4 and all(value.isdecimal() and str(int(value)) == value
                                       for value in values), 'Noncanonical collision IDs')
        plan = dict(schema=lines[0], context=lines[1],
                    **dict(zip(('low', 'a', 'b', 'high'), map(int, values))))
    except (UnicodeError, ValueError) as error:
        raise RuntimeError('Malformed collision plan') from error
    require(encode_plan(plan) == raw, 'Noncanonical collision plan framing')
    validate_collision_plan(provider, plan, context)
    return plan


def encode_plan(plan):
    return (plan['schema'] + '\n' + plan['context'] + '\n' +
            ' '.join(str(plan[key]) for key in ('low', 'a', 'b', 'high')) + '\n').encode('ascii')


def namespace(block_size, endpoint='new', plan=None):
    require(endpoint in ('old', 'new', 'collision-new', 'rename-new'), 'Unknown namespace endpoint')
    require(endpoint != 'collision-new' or block_size == 1024, 'Collision endpoint requires1KiB')
    result = {'root': {}}
    for parent in (0, 1):
        main, peer, child, grand = [kind + str(parent) for kind in ('main', 'peer', 'child', 'grand')]
        result['root'][('indexed-%d' % parent).encode()] = main
        result['root'][('indexed-peer-%d' % parent).encode()] = peer
        result[main] = {name: 'p%ds%d' % (parent, slot)
                        for name, slot in expected_names(block_size, parent, 'new' if endpoint == 'new' else 'old').items()}
        result[peer] = {b'Exchange': 'p%ds4' % parent}
        result[child] = {b'Grandchild': grand}
        result[grand] = {}
        if endpoint != 'new':
            result[main][b'Child'] = child
            if endpoint == 'rename-new' and parent == 0:
                result[peer][b'Moved'] = result[main].pop(bulk_name(1))
        else:
            del result[main][bulk_name(0)]
            result[peer][b'Moved'] = result[main].pop(bulk_name(1))
            result[main][bulk_name(2)], result[peer][b'Exchange'] = (
                result[peer][b'Exchange'], result[main][bulk_name(2)])
            result[peer][b'Linked'] = 'p%ds3' % parent
            result[peer][b'Moved-Child'] = child
    if block_size == 1024:
        require(plan is not None, 'Collision plan required for 1KiB namespace')
        result['root'][b'indexed-collision'] = 'collision'
        result['collision'] = {collision_name(plan[key]): ('p2s0' if key in ('low', 'a') else 'p2s1')
                               for key in (('low', 'a', 'high') if endpoint not in ('new', 'collision-new')
                                           else ('low', 'a', 'b', 'high'))}
    return result


def expected_data(identity):
    require(len(identity) == 4 and identity[0] == 'p' and identity[2] == 's', 'File identity')
    return content(int(identity[1]), int(identity[3]))


def directory_parents(names):
    result = {'root': 'root'}
    for directory, entries in names.items():
        for identity in entries.values():
            if identity in names:
                require(identity not in result, 'Repeated directory identity')
                result[identity] = directory
    require(set(result) == set(names), 'Disconnected directory model')
    return result


def exact_inventory(actual, expected):
    """Lists of (parent, full encoded name, inode); reject hidden duplicate names."""
    for inventory in (actual, expected):
        identities = [(parent, name) for parent, name, _ in inventory]
        require(len(identities) == len(set(identities)), 'Duplicate expected/observed name')
    require(Counter(actual) == Counter(expected), 'Complete no-key identity inventory differs')


def endpoint(actual, old, new):
    """Atomic endpoint gate for a later single bounded mutation/crash cut."""
    if actual == old:
        return 'old'
    if actual == new:
        return 'new'
    raise RuntimeError('Namespace/metadata hybrid is neither complete endpoint')


def root_directories(block_size):
    result = {('indexed-%d' % p).encode(): 'main%d' % p for p in (0, 1)}
    result.update({('indexed-peer-%d' % p).encode(): 'peer%d' % p for p in (0, 1)})
    if block_size == 1024:
        result[b'indexed-collision'] = 'collision'
    counts(block_size)
    return result


def padding_for(identity):
    require(identity != 'root', 'Root has no encryption policy')
    return 3 if identity.startswith('p1s') or identity in ('main1', 'peer1', 'child1', 'grand1') else 0


expected_namespace = namespace


def link_counts(names):
    result = Counter(identity for entries in names.values() for identity in entries.values())
    for identity in names:
        if identity != 'root':
            result[identity] = 2 + sum(child in names for child in names[identity].values())
    return result

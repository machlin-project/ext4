#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
"""Check the isolated maintenance helper against an independent bounded resource."""

import argparse
import errno
import hashlib
import json
import os
from pathlib import Path
import select
import shutil
import socket
import struct
import subprocess
import time

MAGIC = 0x4534434B
VERSION = 1
OPEN, READ, WRITE, FLUSH, CLOSE = range(1, 6)
WRITABLE = 1
SECTOR_SIZE = 512
MAX_TRANSFER = 1024 * 1024
FRAME = struct.Struct('>IIIIQII')
CASE_SECONDS = 60
FSCK_CORRECTED = 1
FSCK_UNCORRECTED = 4
FSCK_OPERATIONAL_ERROR = 8
EXT4_ROOT_INODE = 2
DAMAGED_ROOT_LINKS = 1000


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def receive(connection, length, deadline):
    result = bytearray()
    while len(result) < length:
        if time.monotonic() >= deadline:
            raise TimeoutError('Maintenance resource deadline expired')
        readable, _, _ = select.select([connection], [], [], max(0, min(1, deadline - time.monotonic())))
        if not readable:
            continue
        part = connection.recv(length - len(result))
        if not part:
            if not result:
                return None
            raise RuntimeError('Truncated maintenance request')
        result.extend(part)
    return result


def run_case(helper, image, mode, output, fault=None):
    before = digest(image)
    descriptor = os.open(image, (os.O_RDONLY if mode == 'verify' else os.O_RDWR) | os.O_NOFOLLOW)
    parent, child = socket.socketpair()
    parent.settimeout(5)
    opened = 0
    counts = {name: 0 for name in ('open', 'read', 'write', 'flush', 'close')}
    deadline = time.monotonic() + CASE_SECONDS
    stdout = (output / 'stdout.log').open('wb')
    stderr = (output / 'stderr.log').open('wb')
    process = subprocess.Popen([str(helper), mode], stdin=child, stdout=stdout, stderr=stderr,
                               close_fds=True, env={'PATH': '/usr/bin:/bin', 'LC_ALL': 'C'})
    child.close()
    size = os.fstat(descriptor).st_size
    fault_sent = False
    try:
        while True:
            raw = receive(parent, FRAME.size, deadline)
            if raw is None:
                break
            magic, version, operation, error, offset, length, flags = FRAME.unpack(raw)
            assert magic == MAGIC and version == VERSION and error == 0
            assert operation in (OPEN, READ, WRITE, FLUSH, CLOSE)
            assert flags == 0 or (operation == OPEN and flags == WRITABLE)
            payload = b''
            response_offset, response_length, response_flags = offset, length, 0
            error = 0
            if operation == OPEN:
                assert offset == length == 0
                if flags == WRITABLE and mode == 'verify':
                    error = errno.EROFS
                else:
                    opened += 1
                    counts['open'] += 1
                    response_offset, response_length = size, SECTOR_SIZE
                    response_flags = WRITABLE if mode != 'verify' else 0
            elif operation in (READ, WRITE):
                assert opened > 0 and 0 < length <= MAX_TRANSFER
                assert offset <= size and length <= size - offset
                if operation == WRITE:
                    incoming = receive(parent, length, deadline)
                    assert incoming is not None and len(incoming) == length
                    assert mode != 'verify', 'Read-only check attempted a write'
                    if fault == 'write-error':
                        error = errno.EIO
                        fault_sent = True
                    else:
                        assert os.pwrite(descriptor, incoming, offset) == length
                        counts['write'] += 1
                else:
                    if fault == 'read-error':
                        error = errno.EIO
                        fault_sent = True
                    else:
                        payload = os.pread(descriptor, length, offset)
                        assert len(payload) == length
                        counts['read'] += 1
            elif operation == FLUSH:
                assert opened > 0 and mode != 'verify'
                assert offset == length == 0
                if fault == 'flush-error':
                    error = errno.EIO
                    fault_sent = True
                else:
                    os.fsync(descriptor)
                    counts['flush'] += 1
            elif operation == CLOSE:
                assert opened > 0 and offset == length == 0
                opened -= 1
                counts['close'] += 1
            if error:
                response_offset, response_length, response_flags = 0, 0, 0
                payload = b''
            response_magic = MAGIC
            if fault == 'bad-magic' and not fault_sent:
                response_magic ^= 1
                fault_sent = True
            if fault == 'disconnect' and not fault_sent:
                parent.shutdown(socket.SHUT_RDWR)
                fault_sent = True
                break
            parent.sendall(FRAME.pack(response_magic, VERSION, operation, error,
                                      response_offset, response_length, response_flags) + payload)
        code = process.wait(timeout=max(1, deadline - time.monotonic()))
    finally:
        if process.poll() is None:
            process.terminate()
            try:
                process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait(timeout=5)
        parent.close()
        os.close(descriptor)
        stdout.close()
        stderr.close()
    after = digest(image)
    result = {'mode': mode, 'exit_code': code, 'fault': fault, 'fault_sent': fault_sent,
              'counts': counts, 'before_sha256': before, 'after_sha256': after}
    (output / 'result.json').write_text(json.dumps(result, indent=2) + '\n')
    if mode == 'verify':
        assert before == after, result
    if fault:
        assert fault_sent and code != 0, result
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--helper', type=Path, required=True)
    parser.add_argument('--fixtures', type=Path, required=True)
    parser.add_argument('--tools-root', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    helper, fixtures, tools = args.helper.resolve(), args.fixtures.resolve(), args.tools_root.resolve()
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    results = {}
    for profile in ('1k', '4k'):
        original = fixtures / ('ext4-' + profile + '.img')
        original_digest = digest(original)
        image = output / (profile + '.img')
        shutil.copyfile(original, image)
        for label, mode, expected, fault in (
                ('clean', 'verify', 0, None),
                ('bad-magic', 'verify', None, 'bad-magic'),
                ('disconnect', 'verify', None, 'disconnect'),
                ('read-error', 'verify', None, 'read-error')):
            case = output / (profile + '-' + label)
            case.mkdir()
            result = run_case(helper, image, mode, case, fault)
            if expected is not None:
                assert result['exit_code'] == expected, result
            results[case.name] = result
        # debugfs updates checksums as well. The inode's bad reference count is
        # a required repair; e2fsck alone treats the free-inode summary mismatch
        # as a noncritical discrepancy and can return zero with it unresolved.
        with (output / (profile + '-damage.log')).open('wb') as log:
            subprocess.run([str(tools / 'debugfs/debugfs'), '-w', '-R',
                            'set_super_value free_inodes_count 0', str(image)],
                           check=True, stdout=log, stderr=subprocess.STDOUT)
            subprocess.run([str(tools / 'debugfs/debugfs'), '-w', '-R',
                            f'set_inode_field <{EXT4_ROOT_INODE}> links_count {DAMAGED_ROOT_LINKS}',
                            str(image)], check=True, stdout=log, stderr=subprocess.STDOUT)
        case = output / (profile + '-damaged-verify')
        case.mkdir()
        result = run_case(helper, image, 'verify', case)
        assert result['exit_code'] & FSCK_UNCORRECTED, result
        results[case.name] = result
        damaged = output / (profile + '-damaged.img')
        shutil.copyfile(image, damaged)
        for fault in ('write-error', 'flush-error'):
            case = output / (profile + '-' + fault)
            case.mkdir()
            failed_image = case / 'image.img'
            shutil.copyfile(damaged, failed_image)
            results[case.name] = run_case(helper, failed_image, 'repair', case, fault)
        case = output / (profile + '-repair')
        case.mkdir()
        result = run_case(helper, image, 'repair', case)
        assert result['exit_code'] == FSCK_CORRECTED, result
        assert result['counts']['write'] > 0 and result['counts']['flush'] > 0, result
        results[case.name] = result
        with (output / (profile + '-independent-fsck.log')).open('wb') as log:
            subprocess.run([str(tools / 'e2fsck/e2fsck'), '-fn', str(image)],
                           check=True, stdout=log, stderr=subprocess.STDOUT)
        case = output / (profile + '-repaired-verify')
        case.mkdir()
        result = run_case(helper, image, 'verify', case)
        assert result['exit_code'] == 0, result
        results[case.name] = result
        assert digest(original) == original_digest, 'Source fixture changed'
        print('PASS', profile, 'full check, repair, read-only bytes and resource failures', flush=True)
    (output / 'summary.json').write_text(json.dumps(results, indent=2) + '\n')


if __name__ == '__main__':
    main()

# SPDX-License-Identifier: BSD-3-Clause
"""Bounded guest commands and task-owned image discovery for installed FSKit tests."""
import json
import plistlib
import re
import subprocess


class GuestTimeout(RuntimeError):
    """The guest command may still own its resources after the RPC timed out."""


def guest_commands(tart, vm, output):
    serial = 0

    def guest(label, *command, required=True, timeout=30, destination=None):
        nonlocal serial
        serial += 1
        prefix = output / f'{serial:03d}-{label}'
        argv = [str(tart), 'exec', vm, *command]
        with (destination or prefix.with_suffix('.stdout.log')).open('wb') as stream:
            try:
                result = subprocess.run(argv, stdout=stream, stderr=subprocess.PIPE, timeout=timeout)
            except subprocess.TimeoutExpired as error:
                prefix.with_suffix('.stderr.log').write_bytes(error.stderr or b'')
                prefix.with_suffix('.status.json').write_text(json.dumps(
                    {'argv': argv, 'timed_out': True, 'timeout': timeout}, indent=2) + '\n')
                raise GuestTimeout(f'{label} timed out; inspect the task process before retry') from error
        prefix.with_suffix('.stderr.log').write_bytes(result.stderr)
        prefix.with_suffix('.status.json').write_text(json.dumps(
            {'argv': argv, 'exit_code': result.returncode}, indent=2) + '\n')
        if required and result.returncode:
            raise RuntimeError(f'{label}: exit {result.returncode}')
        return prefix.with_suffix('.stdout.log').read_bytes() if destination is None else b''

    return guest


def image_devices(guest, label, path):
    """Discover only this image's devices, including after an unsuccessful mount."""
    attached = plistlib.loads(guest(label, '/usr/bin/hdiutil', 'info', '-plist'))
    devices = []
    for image in attached.get('images', []):
        if image.get('image-path') != path:
            continue
        for entity in image.get('system-entities', []):
            device = entity.get('dev-entry', '')
            # These fixtures are raw single-volume images, without a partition table.
            assert re.fullmatch(r'/dev/disk[0-9]+', device), f'Unexpected task device: {device}'
            devices.append(device)
    return list(dict.fromkeys(devices))

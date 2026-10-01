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

    def guest(label, *command, required=True, timeout=30, destination=None, stdin=None):
        nonlocal serial
        serial += 1
        prefix = output / f'{serial:03d}-{label}'
        # Only argv and results enter evidence. In particular, a sudo password
        # supplied through stdin must never become an argument or a saved input.
        argv = [str(tart), 'exec', *(['-i'] if stdin is not None else []), vm, *command]
        # Stream both channels so a stalled guest retains progress evidence
        # before its deadline, without buffering the whole diagnostic in memory.
        with (destination or prefix.with_suffix('.stdout.log')).open('wb') as stream, \
                prefix.with_suffix('.stderr.log').open('wb') as errors:
            try:
                result = subprocess.run(argv, input=stdin, stdout=stream,
                                        stderr=errors, timeout=timeout)
            except subprocess.TimeoutExpired as error:
                prefix.with_suffix('.status.json').write_text(json.dumps(
                    {'argv': argv, 'timed_out': True, 'timeout': timeout}, indent=2) + '\n')
                raise GuestTimeout(f'{label} timed out; inspect the task process before retry') from error
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

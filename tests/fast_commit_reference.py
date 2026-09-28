#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
"""Read fixture objects through debugfs without materializing host special files."""
import hashlib
import re

from check_namespace import inode_fields, symlink_bytes
from check_rename import entries


def read_namespace(image, output, block_size, debugfs, run):
    result = dict(files={}, directories=[], symlinks={}, special={}, xattrs={})
    pending = [""]
    visited = set()
    dump_index = 0
    while pending:
        directory = pending.pop()
        listing = entries(run([debugfs, "-R", f'ls -p "/{directory}"', image]))
        for name, number in listing.items():
            if name in (".", ".."):
                continue
            if '"' in name or "\n" in name or "\\" in name:
                raise RuntimeError("Fixture name cannot be safely quoted for debugfs")
            path = f"{directory}/{name}" if directory else name
            inode = inode_fields(run([debugfs, "-R", f'stat "/{path}"', image]))
            if inode is None or inode["inode"] != number:
                raise RuntimeError("Directory and independent inode lookup disagree")
            attributes = re.findall(r'^\s+([^\s]+)\s+\((\d+)\)', run(
                [debugfs, "-R", f'ea_list "/{path}"', image]), re.M)
            for key, size in attributes:
                if any(char in key for char in ('"', "\n", "\\")):
                    raise RuntimeError("Fixture attribute cannot be safely quoted for debugfs")
                dump = output / f"attribute-{dump_index:04d}"
                dump_index += 1
                run([debugfs, "-R", f'ea_get -r -f "{dump}" "/{path}" "{key}"', image])
                value = dump.read_bytes()
                if len(value) != int(size):
                    raise RuntimeError("Independent attribute size and value disagree")
                result["xattrs"].setdefault(path, {})[key] = hashlib.sha256(value).hexdigest()
            kind = inode["type"]
            if kind == "directory":
                if number in visited:
                    raise RuntimeError("Repeated directory in independent traversal")
                visited.add(number)
                result["directories"].append(path)
                pending.append(path)
            elif kind == "regular":
                dump = output / f"file-{dump_index:04d}"
                dump_index += 1
                run([debugfs, "-R", f'dump "/{path}" "{dump}"', image])
                with dump.open("rb") as stream:
                    result["files"][path] = hashlib.file_digest(stream, "sha256").hexdigest()
            elif kind == "symlink":
                result["symlinks"][path] = symlink_bytes(
                    image, f'"/{path}"', inode, block_size, debugfs, run).hex()
            elif kind in ("character", "block", "FIFO", "socket"):
                result["special"][path] = {key: inode.get(key, 0) for key in (
                    "type", "mode", "uid", "gid", "links", "device_major", "device_minor")}
            else:
                raise RuntimeError(f"Unexpected independent inode type: {kind}")
    result["directories"].sort()
    return result

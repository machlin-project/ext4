#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
"""Extract regular files from an xz- or zstd-compressed squashfs 4.0 image.

The Linux reference loads kernel modules from the pinned Alpine modloop, which is
a squashfs image; this reader avoids depending on host squashfs tools."""
import argparse
import lzma
from pathlib import Path
import struct

MAGIC = 0x73717368
COMPRESSION_XZ = 4
METADATA_SIZE = 8192
METADATA_UNCOMPRESSED = 0x8000
BLOCK_UNCOMPRESSED = 1 << 24
FRAGMENT_NONE = 0xFFFFFFFF
BASIC_DIRECTORY, BASIC_FILE, EXTENDED_DIRECTORY, EXTENDED_FILE = 1, 2, 8, 9
SUPERBLOCK = struct.Struct("<IIIIIHHHHHHQQQQQQQQ")


class Squashfs:
    def __init__(self, path):
        self.data = Path(path).read_bytes()
        fields = SUPERBLOCK.unpack_from(self.data, 0)
        (magic, _, _, self.block_size, _, self.compression, _, _, _, major, _, self.root,
         _, _, _, self.inode_table, self.directory_table, self.fragment_table, _) = fields
        if magic != MAGIC or major != 4:
            raise ValueError("not a squashfs 4.0 image")
        if self.compression != COMPRESSION_XZ:
            raise ValueError(f"unsupported squashfs compression {self.compression}")

    def decompress(self, payload):
        return lzma.decompress(payload, format=lzma.FORMAT_XZ)

    def metadata(self, table, block, offset, length):
        """Read length bytes from a metadata table starting at a block reference."""
        position = table + block
        output = b""
        while len(output) < offset + length:
            header = struct.unpack_from("<H", self.data, position)[0]
            size = header & ~METADATA_UNCOMPRESSED & 0xFFFF
            payload = self.data[position + 2:position + 2 + size]
            output += payload if header & METADATA_UNCOMPRESSED else self.decompress(payload)
            position += 2 + size
        return output[offset:offset + length]

    def inode(self, reference, length=64):
        return self.metadata(self.inode_table, reference >> 16, reference & 0xFFFF, length)

    def directory(self, reference):
        raw = self.inode(reference)
        kind = struct.unpack_from("<H", raw, 0)[0]
        if kind == BASIC_DIRECTORY:
            start, _, size, offset = struct.unpack_from("<IIHH", raw, 16)
        elif kind == EXTENDED_DIRECTORY:
            _, size, start, _, _, offset = struct.unpack_from("<IIIIHH", raw, 16)
        else:
            raise ValueError("not a directory inode")
        listing = self.metadata(self.directory_table, start, offset, size - 3)
        entries = {}
        position = 0
        while position < len(listing):
            count, block, _ = struct.unpack_from("<III", listing, position)
            position += 12
            for _ in range(count + 1):
                inode_offset, _, _, name_size = struct.unpack_from("<HhHH", listing, position)
                name = listing[position + 8:position + 9 + name_size].decode()
                entries[name] = block << 16 | inode_offset
                position += 9 + name_size
        return entries

    def lookup(self, path):
        reference = self.root
        for part in Path(path).parts:
            reference = self.directory(reference)[part]
        return reference

    def fragment(self, index):
        table = struct.unpack_from("<Q", self.data, self.fragment_table + (index // 512) * 8)[0]
        raw = self.metadata(table, 0, (index % 512) * 16, 16)
        start, size, _ = struct.unpack_from("<QII", raw, 0)
        payload = self.data[start:start + (size & ~BLOCK_UNCOMPRESSED)]
        return payload if size & BLOCK_UNCOMPRESSED else self.decompress(payload)

    def read(self, path):
        raw = self.inode(self.lookup(path), 64 + 4 * 4096)
        kind = struct.unpack_from("<H", raw, 0)[0]
        if kind == BASIC_FILE:
            start, fragment, fragment_offset, size = struct.unpack_from("<IIII", raw, 16)
            sizes_at = 32
        elif kind == EXTENDED_FILE:
            start, size, _, _, fragment, fragment_offset, _ = struct.unpack_from(
                "<QQQIIII", raw, 16)
            sizes_at = 56
        else:
            raise ValueError(f"{path} is not a regular file")
        full = size // self.block_size if fragment != FRAGMENT_NONE else \
            (size + self.block_size - 1) // self.block_size
        output = b""
        position = start
        for index in range(full):
            stored = struct.unpack_from("<I", raw, sizes_at + 4 * index)[0]
            length = stored & ~BLOCK_UNCOMPRESSED
            payload = self.data[position:position + length]
            output += payload if stored & BLOCK_UNCOMPRESSED else self.decompress(payload)
            position += length
        if fragment != FRAGMENT_NONE:
            block = self.fragment(fragment)
            output += block[fragment_offset:fragment_offset + size - len(output)]
        return output[:size]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("image", type=Path)
    parser.add_argument("paths", nargs="+")
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    image = Squashfs(args.image)
    args.output.mkdir(parents=True, exist_ok=True)
    for path in args.paths:
        target = args.output / Path(path).name
        target.write_bytes(image.read(path))
        print(f"{path}: {target.stat().st_size} bytes")


if __name__ == "__main__":
    main()

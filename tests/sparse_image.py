"""Bounded copying and content identities for very large sparse test images."""

import errno
import hashlib
import os
from pathlib import Path
import struct

PAGE = 4096
MAX_STORED_BYTES = 2 << 30


def data_ranges(fd):
    """Yield page-aligned stored ranges; never fall back to a whole-file scan."""
    size = os.fstat(fd).st_size
    position = total = 0
    while position < size:
        try:
            first = os.lseek(fd, position, os.SEEK_DATA)
        except OSError as error:
            if error.errno == errno.ENXIO:
                break
            raise
        end = min(size, (os.lseek(fd, first, os.SEEK_HOLE) + PAGE - 1) // PAGE * PAGE)
        first = max(position, first // PAGE * PAGE)
        if end <= first:
            raise RuntimeError("Sparse extent enumeration did not advance")
        total += end - first
        if total > MAX_STORED_BYTES:
            raise RuntimeError("Sparse image exceeds the bounded stored-data budget")
        yield first, end
        position = end


def sparse_digest(path):
    """Hash logical size and nonzero pages, independently of host hole layout."""
    result = hashlib.sha256(b"ext4-test-sparse-pages-v1\0")
    with Path(path).open("rb", buffering=0) as source:
        result.update(struct.pack("<Q", os.fstat(source.fileno()).st_size))
        for first, end in data_ranges(source.fileno()):
            for position in range(first, end, PAGE):
                data = os.pread(source.fileno(), min(PAGE, end - position), position)
                if len(data) != min(PAGE, end - position):
                    raise RuntimeError("Short sparse-image read")
                if any(data):
                    result.update(struct.pack("<Q", position))
                    result.update(data.ljust(PAGE, b"\0"))
    return result.hexdigest()


def sparse_copy(source, destination):
    with Path(source).open("rb", buffering=0) as original, Path(destination).open("xb", buffering=0) as target:
        target.truncate(os.fstat(original.fileno()).st_size)
        for first, end in data_ranges(original.fileno()):
            for position in range(first, end, 1 << 20):
                length = min(1 << 20, end - position)
                data = os.pread(original.fileno(), length, position)
                if len(data) != length or os.pwrite(target.fileno(), data, position) != length:
                    raise RuntimeError("Incomplete sparse-image copy")

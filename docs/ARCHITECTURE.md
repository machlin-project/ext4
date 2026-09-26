# Architecture

## Ownership

The portable C core owns ext4 disk structures, validation, inode metadata,
directories, extent trees, allocation and journal transactions. It accepts a
bounded block-resource interface and explicit operation policy. Platform adapters
own their allocation, synchronization, scheduling, error conversion, page cache
and resource lifetime. No FSKit or XNU object appears in an on-disk structure.

The stock-macOS product uses FSKit. The kernel product compiles the same core
with an XNU VFS/UBC adapter. It does not embed the FSKit runtime or call a userspace
server for ordinary filesystem operations. The core is a separate library target;
it must compile without libc assumptions and with bounded kernel-stack use.

Disk structures use byte-sized little-endian wrapper fields so parsing does not
depend on alignment, packing pragmas or the compiler's native endianness. JBD2
has its own big-endian wire definitions. Geometry and overflow checks precede
allocation, multiplication and I/O. Metadata checksum verification precedes
trusting pointers or record lengths. Unsupported incompatible features reject
mounting; an unknown read-only-compatible feature never permits writing.

## I/O and cache contract

A resource reports its byte size and exact-read operation. A short underlying
read is an error unless the adapter completes it before returning. Reads never
exceed the supplied resource. A read-only mount does not repair or replay media.
Filesystems requiring recovery are rejected until recovery is implemented.

Write support must add explicit ordering and durable-flush contracts. Completion
of a userspace callback is not evidence of device persistence. FSKit's direct
and cached metadata paths must not access the same ranges incoherently. XNU file
data uses the native UBC owner; the core must not introduce another file-page
cache. Journal buffers and metadata transaction snapshots have explicit ownership.

The initial kernel reader uses a fixed device-sector buffer-cache key for
metadata. Regular file reads and page-in use `cluster_read`/`cluster_pagein`,
with validated byte mappings from the C core and `buf_strategy` device dispatch.
The mapping API may include padding up to the last filesystem block; the native
page-cache caller owns EOF zeroing. Read-only mappings remain immutable for the
mount lifetime. Writable support must replace that assumption with explicit
mapping lifetime protection coordinated with truncate, allocation and UBC.
The adapter registers local mount arguments so XNU resolves, authorizes, opens
and closes the backing block device at its existing mount boundary.

The kernel inode index serializes vnode creation independently of its lookup
lock, so vnode creation can reclaim another inode without recursively taking
the index lock. Cached vnode references are checked with XNU's vnode identity
before use. FSKit uses a weak item identity table, retaining one item while any
concurrent framework operation owns it. Neither identity table is a file-data
cache.

## Linux operation policy

The independent driver has no mandatory imports from LXNU. A separate integration
module connects a versioned driver interface to the LXNU kernel. Linux lookup,
mount namespaces, credentials and capabilities remain in LXNU; the filesystem
applies admitted metadata transitions atomically at the inode owner.

Policy belongs to a captured operation, never to a global Linux-mode switch or
the identity of a later writeback thread. Native and Linux accesses to the same
inode share locking, data and lifetime. The bridge cannot fabricate credentials,
bypass native MAC checks, restore mode bits after writes or expose policy authority
through an untrusted user ioctl. Missing bridge support remains an explicit gap.

## Sources

- [ext4 format](https://docs.kernel.org/filesystems/ext4/index.html)
- [FSKit](https://developer.apple.com/documentation/fskit)
- [FSKit block resources](https://developer.apple.com/documentation/fskit/fsblockdeviceresource)

The implementation is original code based on the format specification. External
utilities generate and inspect fixtures; no Linux or e2fsprogs implementation is
copied or linked into the core.

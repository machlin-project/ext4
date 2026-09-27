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
Filesystems requiring recovery are rejected by the read-only mount. The separate
offline recovery API requires an explicit write capability and exclusive resource
ownership; a platform never invokes it as a side effect of a read-only mount.

The write capability requires exact writes, coherent read-after-write and a flush
that persists preceding writes through every volatile cache. Completion
of a userspace callback is not evidence of device persistence. FSKit's direct
and cached metadata paths must not access the same ranges incoherently. XNU file
data uses the native UBC owner; the core must not introduce another file-page
cache. Journal buffers and metadata transaction snapshots have explicit ownership.

The internal journal owner admits one transaction at a time under the filesystem
owner's serialization. Each transaction has bounded credits and private block
snapshots; affected metadata locks remain held through checkpoint completion.
The ordered sequence is: persist the recovery marker, publish the log start,
persist log records and ordered data, persist the commit, persist home blocks,
then clear the log start. Only a successful clean finish clears the filesystem's
recovery marker. Any uncertain write or barrier aborts the instance. Closing an
aborted journal only releases memory, leaving recovery evidence on disk. The
owner must finish or cancel its active transaction before closing the journal.

Recovery checks the committed prefix before changing home blocks, records bounded
replay locations and applies the last committed event for each block. A revoke
wins against data from its own transaction and earlier transactions; a later
logged allocation supersedes it. Journal and transaction-ID wrap are explicit.
The journal inode's data and mapping blocks are frozen before writes, and replay
cannot target those blocks or ranges outside the filesystem. Checksummed primary-superblock
damage remains an offline-repair condition. Recovery does not invent geometry.

The current journal engine handles internal v2-superblock journals with legacy,
v2 or v3 checksums/tags (legacy means no journal checksums), 64-bit addresses and
revokes. It bounds the journal to 1,048,576 data blocks, 1,024 data runs and 8,192
mapping blocks, a writing transaction to 256 snapshots, and recovery to 1,048,576
records. Exceeding a bound
is an explicit unsupported result. External journals, checksum v1, async/fast
commit and general filesystem mutation remain separate work.
The internal block transaction interface is not an application or driver ioctl.
Platform adapters remain read-only until their metadata ownership, native cache
integration and durable device-barrier paths are implemented and tested.

The writable core instance is an exclusive resource owner. Its caller serializes
all reads, mutations and snapshot/mapping consumers through operation completion.
It is not safe to call the instance concurrently without that owner lock. A
mutation resolves the inode number and generation again, checks allocation and
the inode checksum, and edits only selected fields in a private full-block
snapshot. It never writes back a caller's cached inode. Unselected bytes and
neighboring inode records survive unchanged, and unrepresentable timestamps fail
without I/O instead of losing precision. Mount does not implicitly recover;
`ext4_sync` performs clean finish, while `ext4_unmount` only releases memory.
An uncertain commit or clean-finish error poisons the instance, including reads.

Bounded writes journal data, allocation metadata and inode attributes together.
They allocate holes, convert unwritten extents and extend EOF. The entire request
must fit the actual journal's credit bound; inode, bitmap, group, superblock and
mapping-node snapshots consume credits alongside data. Credit exhaustion and
allocation failure cancel the private transaction before any resource writes.
The public API reports the full length on success and zero on error. Platform
adapters will need a separate partial-progress/chunking contract for large I/O.

The allocator validates group descriptors, bitmap checksums and free counts before
selecting blocks. It initializes lazy block bitmaps from protected system ranges
and marks the invalid tail of the final group. A sorted, merged range index covers
superblocks, GDT/reserved GDT, bitmaps, inode tables, journal and orphan-file data/mapping nodes,
including flex_bg placement. Writable mount bounds its input index to 1,048,576
ranges. It is a metadata exclusion index, not a global filesystem consistency
checker. Ordinary allocation preserves the reserved-block pool; admitted use of
reserved space remains a separate policy contract.

New blocks are fully zeroed before partial writes. Growth skips sparse/unwritten
runs and zeroes exposed bytes in existing written allocations beyond the old EOF.
Extent insertion supports bounded tree growth and node splits, including splitting
an unwritten extent around initialized data; legacy mapping allocates through
triple-indirect blocks. Both mapping nodes and data contribute to inode block
accounting. Allocation counters become visible in the filesystem instance only
after commit succeeds. The primary-superblock snapshot always retains RECOVER and
a fresh checksum through checkpoint; only clean finish clears that marker.
Large-volume performance and a concurrent allocator remain unaccepted.

`ext4_truncate_atomic` performs the size, tail-zeroing, data/mapping removal, bitmap,
group/superblock accounting and admitted attribute transition in one journal
transaction. It validates the entire inode map first: all physical ranges must
be allocated, disjoint from each other and protected metadata, and agree with
the inode's block count. Its temporary ownership index is capped at 1,048,576
ranges (16 MiB). Validation reads mapping nodes into at most five heap block buffers;
only changed paths consume journal credits. This checks ownership within that inode,
not across every inode on disk.
Extent suffix removal frees empty nodes and collapses a small sole child back
into the inode. Indirect removal releases empty paths through all three levels.
Shrink zeroes a retained written partial block; growth exposes zero bytes and
does not allocate sparse holes. Unwritten backing bytes remain inaccessible.

Every successful transaction completes home writes and resets the log before
another transaction can reuse freed blocks. There are no deferred home writes
from a previous owner. Relaxing this ordering would require revoke and reuse
ownership rules. The atomic API cancels before resource writes when its whole
changed mapping/metadata set exceeds the transaction bound.

`ext4_truncate` uses bounded batches to shrink a live regular file. Its first
transaction validates the complete map, captures the admitted attributes and
target size, zeroes the retained tail, removes a first batch and records a legacy
orphan intent if more work remains. Further cleanup uses the same restartable
owner as offline recovery; the final batch removes the intent. The filesystem
owner excludes all other reads and mutations throughout the call. A successful
call returns the final inode snapshot only after all batches have checkpointed.
After the intent commits, any subsequent error, including a private allocation
or read failure, poisons the instance until explicit recovery finishes that intent.
Errors before the first commit leave the caller's result and resource unchanged;
an uncertain commit still requires recovery. A crash may therefore leave the
original file or finish the captured size and attributes during recovery.

Journals too small to reserve the minimum cleanup paths retain the atomic
contract, preventing an intent that a later cleanup batch cannot fit. Live growth
still uses a single transaction: sparse and unwritten runs need no data snapshots,
but zeroing many written allocations beyond the previous EOF remains credit-bound.
These APIs do not yet own open-unlinked files or native page-cache concurrency.

Offline recovery completes legacy orphan-list and modern orphan-file operations after journal replay.
It validates the entire inode-number chain, allocation, checksums, types and cycles
before the first cleanup transaction. Each inode's complete block map is checked
before releasing its first batch. Linked regular files retain their recorded size,
links and attributes; cleanup removes blocks beyond EOF and zeroes the retained
partial block. Unlinked files, directories and long symlinks release their maps,
then their inode bit. Short symlinks and special-file device fields are never read
as block pointers. Inode release updates bitmap checksums, free-inode and directory
counts, clears the record, and preserves its generation for future reuse.

Removal uses bounded batches, retaining the orphan entry until the final inode/map
and list update commit together. Interrupted cleanup can restart from the remaining
map; it never reuses a block before the previous transaction's checkpoint completes.
The recovery report separates replayed transactions, cleanup transactions and
completed orphan entries. Unsupported inode attributes/flags leave cleanup pending.
Online open-unlinked lifetime is not yet implemented.

The orphan file has a fixed, validated inode and mapping for the writable owner's
lifetime. Preparation checks its allocation, complete map and block count, rejects
holes/unwritten mappings, verifies each tail magic/checksum, and adds its data and
mapping nodes to the protected system index. Ordinary inode mutation cannot target
that private inode. The implementation supports up to 512 file blocks and 1,048,576
active entries, with explicit rejection beyond either bound. A clean writable
mount also rejects nonempty slots when ORPHAN_PRESENT is absent.

Recovery validates every active entry's inode and checks duplicates within the file
and against the entire legacy chain before starting cleanup. It first finishes the
legacy chain, then atomically clears one modern slot and makes that inode the legacy
head in the same journal transaction. The existing bounded cleaner completes it
before the next transfer. An interruption therefore retains each pending inode in
one of the two supported representations. Both may legitimately coexist on disk.
The file checksum binds the inode seed and generation, physical block address and
entry array; tail magic is checked separately. Recovery reports transfers separately
from completed orphan entries and total cleanup transactions.

Every writing transaction on this feature retains ORPHAN_PRESENT alongside RECOVER;
clean finish clears both only after the validated file and legacy list are empty.
Read-only mounts reject ORPHAN_PRESENT even if RECOVER is absent. Live truncation
continues to use the legacy list on these volumes. Scalable concurrent insertion,
orphan-file growth and the owning platform's open-unlinked lifetime remain separate
work; this recovery owner does not provide those contracts.

Linux's primary free-block/inode summaries may lag committed group-descriptor
changes. Explicit recovery reconstructs those totals from the validated, replayed
group descriptors in a journal transaction before cleanup. This is restricted to
recovery; ordinary clean writable mounts still reject inconsistent summaries.

The admitted operation supplies final permission bits and captured timestamps
under the same owner lock as authorization and mutation. Ownership changes must
include the admitted permission transition; a write requires final permissions,
mtime and ctime. The core applies them in the data transaction, never repairs
set-ID bits afterward. No untrusted ioctl exposes this authority. Inodes with
xattrs or unsupported flags (including immutable and append-only) currently
reject mutation pending their actual policy implementation. Native UBC/FSKit
integration and LXNU policy acceptance remain separate from this portable API.

`ext4_create`, `ext4_mkdir`, `ext4_symlink` and `ext4_link` share the exclusive writable owner and
generation-checked inode resolution. Creation receives admitted permissions,
full-width owners and captured atime/mtime/ctime, with optional representable birth
time. The caller supplies one parent mutation time. The core does not derive
credentials, umask, group inheritance or authorization from that request.

One bounded transaction owns the directory entry, new inode record and bitmap,
group/primary accounting, parent timestamps and link counts. Mkdir also initializes
dot/dotdot and accounts its block separately from any parent growth. A hard link
increments the existing non-directory inode's links and ctime, preserving its
other fields. Allocation reads the released record's generation, advances it
without producing zero, and clears all remaining old bytes. Lazy inode bitmaps
are initialized from the group's actual inode bounds; allocation advances the
initialized-table high-water mark while preserving the table-zeroed flag.

Symlink creation includes its target in that same transaction. Targets shorter
than the 60-byte inode mapping area use inline storage with no extent flag or
data allocation; longer targets receive one zeroed data block. The target plus
its terminating NUL must fit that block, while the inode size excludes the NUL.
Targets contain opaque non-NUL bytes and may include slashes or invalid UTF-8;
empty targets are rejected. Only the parent's NODUMP and NOATIME flags propagate
to a symlink. The reader rejects an inline length that leaves no terminator space.
Adapter readlink operations bound allocation by the filesystem block size and
preserve target bytes; native path traversal limits remain the platform's policy.

The current namespace writer scans linear directories, validating their full maps
and every record before insertion, even after finding a candidate slot. It reuses
record slack or appends a zeroed block, preserving legacy name-length encoding and
metadata checksum tails. Directory size is bounded to 1,048,576 blocks. Existing
indexed parents explicitly reject mutation. Duplicate names, stale generations,
invalid dot records, exhausted inodes, reserved-space exclusion and journal-credit
exhaustion cancel before resource writes; uncertain commits poison the owner.
These operations do not yet implement directory indexing,
unlink/rmdir/rename or open-unlinked lifetime, and neither adapter exposes them.

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
- [JBD2 journal format](https://docs.kernel.org/filesystems/ext4/journal.html)
- [Orphan-file format](https://docs.kernel.org/filesystems/ext4/orphan.html)
- [FSKit](https://developer.apple.com/documentation/fskit)
- [FSKit block resources](https://developer.apple.com/documentation/fskit/fsblockdeviceresource)

The implementation is original code based on the format specification. External
utilities generate and inspect fixtures; no Linux or e2fsprogs implementation is
copied or linked into the core.

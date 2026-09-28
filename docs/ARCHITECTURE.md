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

CRC32C uses an immutable 1 KiB byte-remainder table. It retains the raw seeded
state API used by UUID, inode, group and journal checksum chains, with no implicit
initial or final complement. Byte loads admit unaligned buffers on every target;
there is no initialization, allocation, mutable global state or CPU-feature probe.
When the compilation target guarantees the ARMv8 CRC32 extension, Clang builds
use its CRC32C instructions for 8-byte words and a byte tail instead. They use
general registers only and compute the same raw reflected update; the choice is
fixed at compile time. Other targets, including the x86_64 kernel build, keep the
table.

Legacy `GDT_CSUM` group descriptors use CRC16 over the UUID, little-endian group
number and full descriptor with the checksum field omitted. The 32-byte remainder
table is immutable. `METADATA_CSUM` takes precedence and retains its CRC32C rules.
Both formats verify descriptors before trusting geometry or lazy-initialization
flags, and update their checksum whenever allocation counters or flags change.
Legacy group checksums do not add checksums to bitmaps, inodes or directory data.
Lazy inode/block initialization uses the same protected-range and accounting
validation for both formats. See the [ext4 group descriptor format](https://docs.kernel.org/filesystems/ext4/group_descr.html).

Group descriptor lookup is shared by inode reads, block/inode allocation and final
inode release. META_BG places each descriptor block in its owning metagroup, while
`first_meta_bg` retains the contiguous prefix of hybrid filesystems. The protected
range index includes the second/last-group descriptor backups even when those
groups have no superblock. SPARSE_SUPER2 selects only its two explicit backup group
fields, with zero denoting an unused slot; it takes precedence over SPARSE_SUPER.
Bitmap and inode-table placement remains descriptor-owned, including FLEX_BG.
Mount validates geometry bounds and rejects the incompatible META_BG/RESIZE_INODE
combination. Journal replay cannot change these captured geometry fields. Online
filesystem resizing is not implemented. See the [ext4 block-group layouts](https://docs.kernel.org/filesystems/ext4/blockgroup.html).

BIGALLOC keeps extent offsets and public accounting in filesystem blocks, while
group bitmaps and their on-disk free counts use clusters. The allocator rounds
metadata allocations to whole clusters. A data hole first searches its logical
cluster for existing backing owned by that inode; physical and logical cluster
offsets must agree. Reclamation frees a cluster only after excluding all surviving
extent references. Map validation counts each data cluster once and rejects
conflicting backing, duplicate ownership and data/metadata cluster aliases.
Logical unmapping progress is distinct from physically freed blocks, allowing
restartable orphan cleanup to advance while a cluster remains referenced.
For 1 KiB BIGALLOC volumes, group zero starts at block zero but the primary
superblock remains at byte 1024; transaction and replay paths use its actual
address independently of the first data block. See the
[cluster allocation format](https://docs.kernel.org/filesystems/ext4/bigalloc.html).

## I/O and cache contract

Regular-file growth validates the destination mapping's logical-address ceiling.
An inline file that will expand into extents uses the extent limit before any
conversion writes. Without HUGE_FILE, the 32-bit sector counter also bounds size;
legacy indirect mapping reserves its metadata overhead within that budget, matching
Linux's dense-map ceiling. Logical file size is independent of image/device size:
sparse holes do not require allocated backing or an iteration per missing block.

Large writable volumes may arrive with a clean 32-bit-tag journal. The writer
selects 64-bit tags in memory when the volume exceeds the 32-bit block count and
persists the feature in the existing start/sequence publication barrier, before
any new descriptor. Mounting, reading and canceled private transactions do not
write the upgrade. A pending journal always retains its recorded interpretation
during recovery; selecting a new writer format requires an empty journal.

Implementation and acceptance proceed through the portable core first, FSKit on
stock macOS second, and LXNU-specific policy third. Adapter development is deferred
until the required core format, mutation, metadata and recovery contracts pass.

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

The journal owner admits one transaction at a time under the filesystem
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
Journal freezing consumes validated contiguous mapping runs while retaining each
run's metadata path. It reads an indirect leaf once per run instead of reopening
the same path for every data block; hole, repeated-node and allocation-overlap
checks still cover the complete journal mapping.

The current journal engine handles internal and single-user external v2-superblock
journals with legacy, v1, v2 or v3 checksums/tags (legacy means no journal checksums),
64-bit addresses and revokes. It bounds the journal to 1,048,576 data blocks,
1,024 data runs and 8,192
mapping blocks, a writing transaction to 256 snapshots, and recovery to 1,048,576
records. Exceeding a bound is an explicit unsupported result.
Recovery conversions, such as the fast-commit replacement transaction, may snapshot
more blocks: half of the ordinary ring, conservatively allowing one descriptor per
logged block, and at most 32 MiB of private snapshot buffers. That is 8,192 blocks
at 4 KiB and 512 at 64 KiB, never fewer than the ordinary bound. Each transaction
indexes its snapshots by block number in an open-addressed table sized to twice its
credit bound, so lookups stay constant-time while commit retains insertion order.

Fast-commit recovery retains only the CRC-validated prefix for the transaction ID
following the ordinary journal prefix. A digest binds each subsequently reread
record to that scan. The ordinary ring excludes the fast-commit area. Recovery
checkpoints the ordinary prefix without clearing its authority, then materializes
the fast records in private metadata snapshots. Exact logged data ranges are
excluded from metadata allocation; inode and block claims update their bitmaps
and allocation summaries. Directory layout and extent or indirect trees are rebuilt
through the core's existing mutation mechanisms, and inode block charges are
derived from the resulting maps.

Before materialization, replay gathers every referenced inode into a sorted,
unique state index and merges logged data ranges into a sorted exclusion union.
Inode lookup and allocation exclusion use binary search. Repeated, nested and
adjacent logged ranges remain admissible; the union only prevents their data
from being reused for new metadata. Per-inode mapping validation retains its
separate overlap checks. The semantic records retain their original committed order.
Each sequential pass retains one journal block in the existing replay buffer,
checking each record's saved digest before exposing its payload. The block cache
is invalidated between preparation and materialization and has no lifetime beyond
that pass. Neither index introduces persistent filesystem state or changes journal
credit limits, commit ordering or resource ownership.

Creation and link replay combine name lookup with insertion-slot preparation in
one directory scan. An existing name reports its inode identity; replay accepts
the same identity without changing link counts and rejects a conflicting owner.
Indexed parents use the namespace writer's hash-targeted scan described below, so
each name operation reads the index graph and only the leaves eligible for that
name. Linear parents still validate every record for each operation.

The complete semantic conversion commits as an ordinary journal transaction with
the same ID as the fast prefix. Before its commit becomes durable, the original
fast prefix remains authoritative. Afterward, ordinary recovery advances past
that ID and ignores the stale fast records. Home metadata is never partially
published without that ordinary recovery record. Last unlinks join legacy orphan
cleanup; replacing an inode generation requires a preceding last unlink and
reclaims its previous backing within the same transaction. New directories retain
their reconstructed size and index representation rather than the logged layout.

The complete modern and legacy orphan sets are validated before conversion.
Generation reuse removes any old modern slot within the conversion transaction;
the in-memory pending count changes only after commit. Remaining orphans use the
ordinary restartable cleaner after fast replay.

Generation replacement detaches every old attribute reference in the same private
transaction. Private value inodes lose their backing when their last reference
is released; shared values and shared external blocks retain surviving owners.
The external-block snapshot reads the transaction's current reference count, so
multiple old generations can detach the same block in one conversion. Legacy
orphan linkage is removed before changing the old inode's checksummed contents.
Per-inode allocation and attribute charges reset at each replay-record boundary.
Ordinary orphan cleanup retains its incremental, restartable attribute removal.

Embedded short-symlink targets and special-device identities are copied from the
logged inode payload. Mapped symlinks retain their logged block count while their
new, private mapping root awaits its range records, so that intermediate state is
not decoded as a short symlink. Range replay and final validation derive block
charges from actual ownership. Final validation reads the target through the private
mapping, rejects holes and unwritten data, and checks its terminating NUL; a missing
required range cannot be committed.

Indirect replay preserves checkpointed pointer roots and reconstructs missing paths
around the exact logged data blocks. Pointer allocation remains within 32-bit
physical addresses even when the shared replay allocator came from an extent-mapped
root directory. Indirect records cannot encode unwritten data or addresses outside
their logical map. Generation replacement releases all three indirect levels before
installing the new inode. An extent/indirect format change within the same inode
generation requires an explicit conversion and currently returns unsupported.
Indirect lookup coalesces adjacent empty slots within a pointer block into one hole
range, so sparse deletion does not allocate scratch and reread that block for every
missing logical block. Coalescing stops at an allocated slot or the node boundary.

This implementation bounds the fast area to 4,096 blocks, the prefix to 65,536
records, and the complete conversion to the recovery transaction bound above.
Capacity exhaustion returns unsupported, including an old generation whose
complete attribute reclamation cannot fit the private conversion transaction.
Additional format combinations, partially completed foreign replay and broader
ownership-corruption cases still require acceptance. The writer continues to
emit ordinary full transactions; it does not emit fast commits.

External journals require an explicitly supplied `ext4_journal_environment` and
exclusive ownership of two distinct resources. The filesystem environment still
owns all allocations. The device superblock, journal superblock and single user
UUID must agree with the filesystem association; stale host device numbers do not
select a resource. Geometry, feature and checksum checks precede writes. Shared
multi-user journals are rejected. Read-only mounts do not access an external
journal; explicit clean recovery with a supplied journal verifies its association
without writing either resource.

External log addresses are device-relative, while descriptor tags retain home
filesystem addresses. The journal superblock follows the device superblock; ring
addresses are not shifted by that prefix. External addresses never enter the home
filesystem's protected-range index. Recovery cannot change the recorded journal
association through a logged filesystem superblock.

The recovery marker is flushed on the home device before publishing the external
log start. Ordered home data and log records are both flushed before the commit;
the log's commit is flushed before checkpointing home blocks. Home checkpoint
completion precedes flushing the empty log, which precedes clearing the recovery
marker. Errors on either device poison the writer. Internal journals retain their
existing barrier count. The POSIX recovery utility accepts `--journal` for a second
exclusively locked offline image; native adapters still require integration.
Checksum v1 uses seeded, non-reflected IEEE CRC32 over each complete descriptor
and its escaped on-log data, in logical order. Revoke and commit blocks are excluded.
The writer computes that order from its private snapshots while retaining its
existing data-before-descriptor submission and durability barriers. Recovery
validates the whole transaction before replay; private per-block CRC32C digests
bind subsequently reread data to that validated scan. The exact all-zero legacy
commit encoding remains admissible when the compatible feature was enabled after
an older transaction. V1 cannot coexist with v2/v3.
The CRC32 implementation has a read-only 64-byte table and no allocation or mutable
initialization. See the [JBD2 format](https://docs.kernel.org/filesystems/ext4/journal.html).

Async-format journals require v1, v2 or v3 checksums. The writer retains its
ordered log/commit barriers; accepting this format does not enable overlapping
commit I/O. Recovery can discard an invalid async commit at the tail after
checking the remaining bounded ring for the next transaction's commit. Finding
that later commit rejects the damaged gap before any home write. This search does
not trust damaged descriptor tag lengths. An earlier validated commit supplies
the 64-bit timestamp boundary for excluding stale ring records; without one,
ambiguous later records reject conservatively. A valid modern commit with damaged
descriptor/data checksums still rejects corruption. V1 validates its aggregate
descriptor/data checksum, including when scanning an incomplete tail.

Recovery accepts a well-formed revoke even if REVOKE is absent from the saved
journal feature word: Linux can commit that record before persisting the newly
enabled bit. Record lengths, checksums, protected/out-of-range targets and the
transaction commit remain mandatory. Revoke semantics never depend on that bit
being durable, and later committed reuse still supersedes an earlier revoke.
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
`ext4_sync` performs clean finish when no held unlinked objects remain, while
`ext4_unmount` only releases memory.
An uncertain commit or clean-finish error poisons the instance, including reads.

Bounded writes journal data, allocation metadata and inode attributes together.
They allocate holes, convert unwritten extents and extend EOF. The entire request
must fit the actual journal's credit bound; inode, bitmap, group, superblock and
mapping-node snapshots consume credits alongside data. Credit exhaustion and
allocation failure cancel the private transaction before any resource writes.
`ext4_write` reports the full length on success and zero on error.

`ext4_write_partial` accepts larger requests under one exclusive owner. It validates
the complete byte range, then resolves the inode again for each bounded transaction.
It reduces a cancelled private batch only after a proven snapshot-credit shortage
or allocation shortage. Other range errors, I/O errors and poisoned instances never
trigger an automatic retry. A successful batch checkpoints all of its data and
metadata before the next starts; the journal's ordering and reuse rules are unchanged.
On error, `completed` retains the durable prefix from those successful batches.
Recovery can add the failed transaction when its commit reached stable storage.
Even a read error while preparing commit conservatively poisons the current owner.

Permissions and the operation's captured times accompany every data batch. The
admitted xattr changes apply only with the first successful prefix; later batches
preserve that state instead of repeating CREATE/REMOVE or security transitions.
A caller retrying a suffix must refresh policy and preserve already applied changes.
The input buffer and update remain immutable while the owner serializes the entire
call. Native adapters must translate the progress/error result into their cache and
I/O contracts.

If written preallocation in an EOF gap exceeds available transaction credits,
`ext4_write_partial` and growing `ext4_truncate` first prepare that gap in bounded
transactions. Preparation validates the complete inode ownership map and admitted
attribute transition before writing. Its exclusive owner then retains the unchanged
mapping throughout the call; no allocation or persistent validation cache is added.
Each step resolves the inode again and zeros only bytes beyond its old visible size.
Sparse and unwritten runs require no data snapshots. The old size, attributes and
visible prefix remain intact until the final size/data transaction publishes growth.
The same preparation handles a gap that fits alone but exhausts credits together
with the final data or attribute change.

A private failure can therefore leave durable zeros outside EOF without changing
visible data or metadata. A later call can repeat preparation safely. Uncertain
commits poison the instance as usual; recovery before publication retains the old
size, while a committed publication exposes the fully prepared range. Preparation
does not contribute to a partial write's completed byte count. The atomic write and
truncate APIs retain their no-write rejection on capacity exhaustion.

The allocator validates group descriptors, bitmap checksums and free counts before
selecting blocks. It initializes lazy block bitmaps from protected system ranges
and marks the invalid tail of the final group. A sorted, merged range index covers
superblocks, GDT/reserved GDT, bitmaps, inode tables, journal and orphan-file data/mapping nodes,
including flex_bg placement. Writable mount bounds its input index to 1,048,576
ranges. It is a metadata exclusion index, not a global filesystem consistency
checker. Ordinary allocation preserves the reserved-block pool; admitted use of
reserved space remains a separate policy contract.
Bitmap validation starts at the first protected range intersecting the group,
checks or initializes each covered cluster interval, and reserves the incomplete
tail and bitmap padding. A byte population pass then verifies the free count.
It preserves complete bitmap and checksum validation without searching the system
range index separately for every cluster. It needs no additional cache or allocation.
Inode bitmaps use the same byte-wise checks for padding, reserved records, the
uninitialized tail beyond the group's high-water mark, the free count and the
lowest free record.

New blocks are fully zeroed before partial writes. Growth skips sparse/unwritten
runs and zeroes exposed bytes in existing written allocations beyond the old EOF.
Extent insertion supports bounded tree growth and node splits, including splitting
an unwritten extent around initialized data; legacy mapping allocates through
triple-indirect blocks. Both mapping nodes and data contribute to inode block
accounting. Allocation counters become visible in the filesystem instance only
after commit succeeds. The primary-superblock snapshot always retains RECOVER and
a fresh checksum through checkpoint; only clean finish clears that marker.
Large-volume performance and a concurrent allocator remain unaccepted.

`ext4_fallocate` reserves holes as unwritten extents, with optional EOF growth or
KEEP_SIZE. Reservation requires extent mapping; indirect files reject before any
write. PUNCH_HOLE requires KEEP_SIZE and supports both maps: full blocks are freed,
written partial edges are zeroed, and EOF remains unchanged. Removing a middle
extent can split its leaf; empty extent or indirect paths are pruned, with small
extent trees collapsed back into the inode. Hidden written data is cleared before
publishing growth, using the same preparation as large writes and truncate.

The complete ownership map and admitted attributes are validated before mutation.
Bounded transactions report a durable byte prefix through `completed`, including
runs already satisfying the request. Cancelled steps can reduce their work budget
on allocation or credit exhaustion. Attributes accompany each checkpoint, while
the xattr batch applies only to the first successful prefix. Uncertain commits
poison the owner. The exclusive owner and orphan holds remain in force throughout.
Reservation retains one spare record in each leaf containing a multi-block
unwritten extent beyond EOF. An existing contiguous initialized/unwritten pair
can already own that capacity: zeroing and merging its suffix releases the record.
Leaf insertion, splitting and collapse preserve this invariant. Reserving an
already backed range checks it too and can require a mapping allocation. A
reservation can expand a reduced imported leaf maximum to its physical capacity
before splitting; a singleton leaf must never produce an empty child. Growing
reservation steps use their actual committed prefix as projected EOF; a requested
final size cannot spend capacity that a later failed step would still need.

If conversion of an existing unwritten extent cannot allocate a mapping node,
`ext4_write_partial` validates its complete ownership map and admitted attributes,
then zeros the required prefix in bounded transactions. The prefix stops within
committed or requested EOF; the remaining tail stays unwritten. When moving EOF
into another extent in a full leaf, preparation can also zero the old boundary's
suffix so its merge releases the record needed by the new boundary. Size-only
growth may leave that reclaimable pair behind EOF, so lookup searches the target
leaf rather than relying on the old size alone.

For a single range entirely inside current EOF, the final zeroing transaction
can initialize it. Other conversions remain deferred: the final data transaction
publishes their mappings, bytes, new EOF and attributes together. A retry stops
at the prepared target extent so a later allocation failure cannot discard that
checkpoint's conversion. Recovery never exposes initialized whole blocks beyond
the committed size. No new mapping block, relocation or persistent private
reservation format is needed by this fallback.

Interrupted preparation preserves visible zeros, EOF, attributes and allocation
counts. Inside current EOF, recovery may retain an initialized zero extent even
with no reported data prefix. Growth preparation may retain only hidden zeroing
until the data/EOF transaction commits. The atomic write API retains its
unchanged-media rejection on exhaustion. An imported full leaf without spare
capacity or a reclaimable pair can still require a mapping block; reservation
must obtain that capacity before promising it. Short growth of such a tree can
reject without writes when space is exhausted. The fallback can zero a large
prefix under pressure; its sustained cost remains part of scale acceptance.

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

Journals too small to reserve the minimum cleanup paths retain the atomic shrink
contract, preventing an intent that a later cleanup batch cannot fit. Live growth
uses the preparation contract above when its existing written backing exceeds one
transaction; its final inode and admitted attribute update must still fit together.
An unlinked inode retained by a core hold can also shrink in batches; it remains
on the orphan list after cleanup reaches its new EOF. A linked truncate intent
can coexist with those held objects and removes only its own list entry.
Native page-cache concurrency remains the platform owner's responsibility.

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
The serialized live owner uses the same restartable cleaner for final inode release.

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
and unlink continue to use the legacy list on these volumes. Scalable concurrent
insertion, orphan-file growth and integration with the owning platform's object
lifetime remain separate work.

Linux's primary free-block/inode summaries may lag committed group-descriptor
changes. Explicit recovery reconstructs those totals from the validated, replayed
group descriptors in a journal transaction before cleanup. This is restricted to
recovery; ordinary clean writable mounts still reject inconsistent summaries.

## Multi-mount protection

Volumes with the MMP incompatibility are admitted by read-only mounts without
reading the MMP block, as Linux does. Writable mounts and offline recovery require
the owner's `ext4_mmp_environment`: interruptible sleeps, unpredictable values, wall
time and host/device names. Without it they return unsupported before any write.
Mount validation bounds the MMP block and update interval, substitutes the usual
five-second default for zero, and adds the block to the protected metadata ranges.

Acquisition follows Linux and e2fsprogs. The check interval is the larger of twice
the update interval, five seconds and the interval recorded in the block, capped at
the Linux updater's 300 seconds; each wait is `min(2 * check + 1, check + 60)`. The
checker sequence rejects immediately. Any other non-clean sequence must survive one
wait unchanged. The owner then publishes a random sequence, waits again and requires
that value to remain. A writable owner immediately publishes the next value, as the
Linux updater's first pass does; offline recovery publishes the checker sequence
instead and holds it until recovery ends. Magic and, with metadata checksums, the
seeded CRC32C are verified on every read. MMP writes cover the whole block and are
followed by a device flush.

A writable owner calls `ext4_mmp_update` at least once per `info.mmp_interval`. The
core also refreshes a sequence at least one interval old before a transaction or
sync writes. A refresh first requires the current block to carry this owner's
sequence and node name; otherwise it poisons the instance and returns BUSY without
writing. The sequence wraps from its largest active value to one. After a
successful sync, `ext4_mmp_release` publishes the clean value, again only while the
block remains this owner's, and the instance then rejects mutation. Offline recovery
releases the checker value the same way. Unmounting without release leaves the
active sequence, so the next owner waits as after a crash.
The owner must deliver MMP reads from the device rather than a cache, because
another host's writes are the subject of the check. This protocol assumes the
other hosts implement it; it does not arbitrate concurrent access by itself.

## fs-verity

Files with the VERITY inode flag require the read-only-compatible VERITY feature,
extent mapping and a regular type. The descriptor size occupies the last four bytes
of the last extent; the descriptor starts at the preceding filesystem-block
boundary and must lie after the first 64 KiB boundary at or beyond EOF, where the
Merkle tree begins. Version 1, SHA-256 or SHA-512, 1–64 KiB Merkle blocks, salts up
to 32 bytes, zero reserved bytes, a signature inside the stored descriptor and a data
size equal to i_size are required; unsupported algorithms or block sizes are
reported as unsupported, other violations as corruption. Tree levels are stored from
the root toward the data, as fs-verity defines them.

`ext4_read` verifies every Merkle data block it returns: the zero-padded data block
is hashed after the salt, which is zero-padded to the hash input block, and each
tree block on the path must contain the child's digest until the root hash matches
the descriptor. A read stops before the first unverifiable block and reports the
verified prefix. The level-zero hash block last verified through the root is reused
within one call; nothing is cached across calls or mutations, so each call parses
the descriptor again. `ext4_map_read` refuses verity files because a native mapping
would bypass verification; adapters must read them through the core. Built-in
signatures are not checked: like Linux without a keyring policy, the core establishes
consistency with the descriptor, whose digest is the file's trust anchor.

Writable owners may rename, link, unlink and change attributes and permissions of
verity files. Writes, both truncate forms, preallocation, hole punching and growth
preparation return permission denied before any write, because Merkle metadata lives
beyond EOF. Final deletion releases the complete map, including that metadata.
Offline cleanup refuses a linked orphan with the flag instead of truncating its tree.
Enabling verity on a file, measurement and signature policy are not implemented.

## Encryption without keys

Volumes with the ENCRYPT incompatibility, and the STABLE_INODES compatibility
feature that some fscrypt policies require, are admitted. The core holds no keys and
performs no cryptography. Objects with the ENCRYPT inode flag keep ciphertext
contents, names and symlink targets, so every operation needing plaintext returns
`EXT4_ENCRYPTED`: reads and native mappings of encrypted files and symlinks,
iteration and lookup in encrypted directories, any name addition, removal or rename
whose parent directory is encrypted, and writes, truncation, preallocation or growth
of encrypted files. Denials occur before any write.

Operations that need no plaintext remain available: owner, permission, timestamp and
ordinary attribute changes on encrypted objects; rename, link and removal of an
encrypted object whose parent is unencrypted, including a moved directory's dotdot
update; rmdir of an empty encrypted directory; and final deletion, which releases
the complete map. Directory validation accepts any name byte in encrypted
directories, whose names are ciphertext, while still checking records, checksums and
the index hash of the stored bytes. The fscrypt context attribute (index 9) cannot be
created, replaced or removed. Offline cleanup refuses a linked encrypted orphan, since
its partial-block zeroing would corrupt ciphertext; unlinked encrypted orphans are
released. Supplying keys, deriving per-file keys and presenting Linux-style no-key
names are product decisions and are not implemented.

## Casefolded directories

Volumes with the CASEFOLD incompatibility are admitted when the superblock names
encoding 1, utf8-12.1, and its encoding flags contain at most STRICT; any other
encoding or flag is unsupported. A CASEFOLD inode flag on a volume without the
feature is corruption, as in Linux. Directories with the flag compare and hash names
through ext4's utf8-12.1 casefold: full (C and F) case folding, canonical
decomposition without compatibility mappings, canonical ordering and removal of
default ignorable code points, which still end a reordering run. Hangul syllables
decompose algorithmically, and code points without Unicode 12.1 data map to
themselves. `scripts/generate_unicode_data.py` produces `core/unicode_data.h` from
the pinned Unicode 12.1.0 database files; the tables are read-only data and the
transform works on bounded caller-allocated storage without recomposition.

Malformed UTF-8 or an encoded surrogate makes a name opaque. Folded names hash their
folded UTF-8 bytes, which may be empty or longer than 255 bytes; opaque names hash
their raw bytes. Two well-formed names are equal when their bytes or folded forms are
equal. As in Linux, a relaxed encoding matches an opaque name only by its exact
bytes, and a strict encoding never matches one and rejects its creation with
`EXT4_INVALID_ARGUMENT`. Stored names keep the bytes they were created with, and
iteration returns those bytes. Lookup, link, unlink, rmdir and rename find entries
through any equivalent name, and adding a name that folds to an existing one
returns `EXT4_EXISTS`. Renaming between two equivalent names addresses the same
inode, so it follows the rename contract for aliases: a no-op that keeps the stored
bytes, `EXT4_EXISTS` with NOREPLACE, or `EXT4_STALE` when absence was expected.

New subdirectories inherit the flag; other object types do not, as in Linux. Linear,
inline and indexed directories share these rules, including leaf splits, which
recompute every folded hash. A name made only of ignorable code points folds to the
empty string, so all such names are equal. `ext4_set_inode_flags` sets or clears the
flag, as Linux's `FS_IOC_SETFLAGS` does, only on an empty directory of a casefold
volume, so no stored name or index hash depends on the previous rule: other volumes
return `EXT4_UNSUPPORTED`, other types `EXT4_NOT_DIRECTORY` and directories with
entries `EXT4_NOT_EMPTY`. Encrypted casefolded
directories store an extra hash in each name record; since every name operation in
an encrypted directory returns `EXT4_ENCRYPTED`, the core never parses those records.

## Persistent inode flags

`ext4_set_inode_flags` changes selected policy bits and captured ctime in one
inode transaction. It preserves mapping flags, data, allocation and xattrs.
The admitted bits are SYNC, IMMUTABLE, APPEND, NODUMP, NOATIME, JOURNAL_DATA,
NOTAIL, DIRSYNC and TOPDIR. The last two require directories; symlinks and special
inodes accept only NODUMP and NOATIME. CASEFOLD is also admitted under the rules of
"Casefolded directories". Changing other flags while retaining an existing
IMMUTABLE bit is rejected. The separate flag operation can clear protection.

Mutation boundaries resolve the current flags under the exclusive owner.
Immutable inodes reject data and attribute changes. Append-only regular files
accept writes at current EOF and reservation, but reject overwrite, truncate,
punching, hardlinks and removal. Append-only directories accept new entries,
including an absent rename destination, while rejecting removal and replacement.
Immutable directories reject additions as well. Orphan cleanup finishes a deletion
already accepted before a held inode acquired protection flags.

The adapter still owns credentials, descriptor append mode, explicit timestamp
authorization, automatic atime policy, pending I/O and mapping revocation. An
append write must be admitted as such before the core verifies its exact EOF.
Append-only timestamp-only updates represent admitted automatic/touch behavior;
the owner must reject unauthorized explicit timestamp changes. All portable commits
already synchronously journal data, so SYNC/DIRSYNC/JOURNAL_DATA do not weaken the
write ordering contract. NODUMP and TOPDIR remain platform/allocation hints.
The contract follows the Linux [flag operation](https://github.com/torvalds/linux/blob/v6.12/fs/ext4/ioctl.c),
[namespace checks](https://github.com/torvalds/linux/blob/v6.12/fs/namei.c) and
[range-operation checks](https://github.com/torvalds/linux/blob/v6.12/fs/open.c);
the implementation shares no Linux source code.

## Extended attributes

`ext4_get_xattr` and `ext4_list_xattrs` resolve the allocated inode and generation
again, verify its checksum and snapshot its inode-body and external attribute
storage before publishing any result. An external block must be allocated, have
valid geometry and reference count, and pass the metadata checksum when enabled.
Writable owners additionally exclude their protected system ranges. This is local
storage validation, not a global cross-inode consistency check.

The bounded parser validates all entries, names, sorted external keys, value
ranges and nonzero hashes, including duplicate keys across both storage areas.
It accepts zero legacy entry hashes and bounded shared value ranges for reading.
Values remain opaque bytes, including compact POSIX ACLs and security metadata.
The public key consists of an on-disk namespace index and the exact suffix bytes;
the adapter owns visibility, naming and authorization. Unknown indices are retained
for that decision. Lists sort by namespace, name length and unsigned name bytes.
Queries and insufficient-buffer or failed-I/O paths leave outputs unchanged on error.

The reader follows the documented [ext4 attribute layout](https://docs.kernel.org/filesystems/ext4/attributes.html).
e2fsprogs can read the synthetic shared-value test, but e2fsck rejects its overlapping
values and zero external entry hashes. That case establishes reader compatibility,
not clean filesystem acceptance.

`EXT4_ATTR_XATTRS` selects a batch of distinct raw keys in `ext4_set_attributes`.
SET, CREATE, REPLACE and REMOVE test existence against the original inode. A batch
requires a captured ctime and commits with the other selected inode fields, allowing
an owner to supply one admitted ACL/security and permission/ownership transition.
Names and values remain caller-owned until return. Values are opaque; this storage
operation does not parse an ACL, authorize access or derive privilege removal.

The writer validates complete inode mapping ownership and attribute storage before
changing private snapshots. It preserves storage assignments when they fit, including
an unchanged shared external block during an inode-body-only update. When repacking
is necessary, bounded subset selection avoids false ENOSPC from a greedy placement.
It writes sorted entries, disjoint padded values, entry/block hashes and metadata
checksums. Existing nonzero extra_isize bounds inode-body storage; 128-byte inodes
and records with no extended body use external storage.

INLINE_DATA owns the first 60 data bytes in `i_block` and the remaining capacity in
an inode-body `system.data` attribute. That internal key remains in the inode body
when other attributes are repacked; callers cannot modify it through raw attribute
batches. The raw get/list API exposes disk keys, so adapters must hide this
implementation key from native attribute namespaces. Files without enough inline
capacity convert within their current transaction. The conversion preserves all
visible bytes and attributes, releases the internal key and installs an extent or
indirect map before accounting and checksum publication. New empty files and
directories use available inline storage only when no inherited attributes already
occupy the inode body; attributed creation can use ordinary mapped storage.

Inline directories have an inode-number parent followed by two independent entry
regions. The core validates both regions before presenting a linear directory
view with synthetic dot entries. Cookies identify stable positions within that
view, including conversion to a mapped block. Removal never merges records across
a region boundary. Directory growth first expands the inode-body region, then
allocates an ordinary block only when inline capacity is insufficient. Inline
objects own no data blocks, including at zero free blocks and during held-unlinked
lifetime; external attributes and EA_INODE logical charges remain separately
accounted. Byte reads handle this representation, while physical mapping APIs
return unsupported because inline bytes are metadata protected by the inode's
checksum and ownership.

On EA_INODE filesystems, values up to 64 KiB can move into private regular inodes
when inode-body and external-block packing is insufficient. Their entries retain
the name, size, private inode number and hash; their value offset is zero. Reads
validate the private inode, initialized allocation map and complete CRC32C before
publishing any value bytes. These inodes cannot be opened through the public inode
or lifetime APIs. Legacy Lustre value backpointers are explicitly unsupported.

Value references count physical attribute entries. An inode-body entry owns one
reference; a shared external block owns each value reference once regardless of
the number of owners of that block. Copying the block increments retained value
references, while detaching one owner leaves them intact. Each owner separately
accounts for the rounded value size in its logical block charge. The private
value inode owns its actual data and mapping allocations. Changes to all three
owners commit together, including multiple inode bitmap/counter changes. Credit
exhaustion cancels before device writes; values are not truncated to fit a journal.

Final deletion can drop one value entry per transaction before releasing its
owner. The zero-reference private orphan representation is also admitted for
recovery. Large values on short symlinks reject before writes because their block
charge conflicts with Linux's fast-symlink encoding; ordinary small attributes and
mapped symlinks remain supported. Value deduplication is not implemented, but
existing shared values retain correct reference counts during copy and deletion.

Changing a shared block decrements its reference count and allocates a private copy
in the same transaction. Dropping a shared reference changes the inode's block count
without freeing that physical block; dropping its final reference releases it.
Allocation and feature summaries become visible only after a successful commit.
Validation, resource and capacity failures cancel the snapshots without device writes
or changed output. An uncertain commit aborts the filesystem until recovery.

Creation, data writes and truncate can include an admitted attribute batch in the
same transaction. The attribute editor uses the private inode record, including
a newly allocated record or preceding changes in that transaction. Creating below
an attributed directory requires an explicit XATTRS field; an empty batch explicitly
admits creating without inherited attributes. Permission or ownership changes on an
attributed inode likewise require that field, with an empty batch admitting preservation.
Time-only changes and hard links preserve existing attributes without deriving policy.

Held unlinked inodes retain readable and mutable attributes. Copying a shared block
and changing their data/attributes uses the same atomic writer; admission reserves
enough journal credits for eventual orphan cleanup. Final release drops the attribute
reference together with the inode, freeing an external block only for its last owner.
Linked truncate recovery preserves attributes while freeing data beyond EOF. Inode
block accounting includes external attributes for mapped files and fast symlinks.
The private orphan-file inode cannot acquire ordinary attributes.

The first attribute transaction enables EXT_ATTR with its inode/storage changes.
The live feature summary is published only after commit. Replay permits that
monotonic enablement, rejecting clearing EXT_ATTR or changing another compatibility
feature before writing the replayed superblock. Raw ACL/security storage is implemented;
authorization, inheritance decisions and privilege semantics remain owner policy
and are not yet accepted in the platform adapters.

## Admitted inode and namespace changes

The admitted operation supplies final permission bits and captured timestamps
under the same owner lock as authorization and mutation. Ownership changes must
include the admitted permission transition; a write requires final permissions,
mtime and ctime. The core applies them in the data transaction, never repairs
set-ID bits afterward. No untrusted ioctl exposes this authority. Unsupported flags
(including immutable and append-only) reject mutation. Attribute-owning inodes use
the explicit admitted attribute transition when their permissions or owners change.
Native UBC/FSKit
integration and LXNU policy acceptance remain separate from this portable API.

`ext4_create`, `ext4_mkdir`, `ext4_mknod`, `ext4_symlink` and `ext4_link` share the exclusive writable owner and
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

`ext4_mknod` creates character/block devices, FIFOs and sockets with the same
admitted attributes and transactional allocation. Character/block records expose
12-bit major and 20-bit minor numbers; the decoder accepts legacy 8/8-bit and
extended encodings. FIFO/socket requests require zero device numbers. Special
inodes have no data map: only external attributes contribute to their block count,
and file data APIs reject them. Device access, pipe/socket behavior and permission
to create these objects belong to the platform owner. Hardlinks, replacement and
open-unlinked retention use the existing inode lifetime contract.

Symlink creation includes its target in that same transaction. Targets shorter
than the 60-byte inode mapping area use inline storage with no extent flag or
data allocation; longer targets receive one zeroed data block. The target plus
its terminating NUL must fit that block, while the inode size excludes the NUL.
Targets contain opaque non-NUL bytes and may include slashes or invalid UTF-8;
empty targets are rejected. Only the parent's NODUMP and NOATIME flags propagate
to a symlink. The reader rejects an inline length that leaves no terminator space.
Adapter readlink operations bound allocation by the filesystem block size and
preserve target bytes; native path traversal limits remain the platform's policy.

The namespace writer validates the complete directory map before mutation. Linear
directories validate every record, even after finding a candidate slot, then reuse
record slack or append a zeroed block. Existing indexed directories validate every
index edge, hash interval, child ownership and checksum, then the root records and
every record, checksum and hash placement in each leaf whose interval contains the
name, including collision continuations. A validated index confines the name to
those leaves, so lookup, duplicate detection and slot selection do not read other
leaves; emptiness checks still validate every leaf. Unvisited leaves are not
revalidated by each operation, matching the on-demand validation of other metadata.
The hash implementation handles legacy, half-MD4 and TEA, each with signed and
unsigned byte variants. Filename bytes need not be UTF-8.

Public read-only lookup follows the HTree hash ranges and scans continuation
leaves, including continuations across internal-node boundaries. It validates
the checksum, layout, sorted ranges and child bounds of each visited index node,
then every record and hash range in the selected leaf before publishing an inode.
Selected child aliases and references from leaves back to index nodes are rejected.
This is path validation, not the complete graph validation required for mutation.
The directory traversal owns three block buffers for the legacy index limit or
four when LARGEDIR is enabled, regardless of directory size. A bounded frame array
retains the cursor at each level while following collision continuations;
ordinary mapping and inode decoding retain their own bounded scratch allocations.
Linear directories read each block once per lookup. An older indexed image without
hash-signedness flags uses that exact-byte linear path instead of guessing which
architecture wrote its names. Explicit unsigned hash versions remain unambiguous.
The mount captures the checked hash seed and signedness; writable ownership does
not permit another agent to change them behind its back. Failed lookup leaves the
caller's output unchanged. These read paths obey the same owner serialization as
other core operations and perform no writes, including on a writable mount.

`ext4_iterate_dir` streams a directory through one temporary block and a visitor.
It verifies the checksum, all records and the requested resume boundary before
delivering any entry from that block. The visitor can accept and continue, accept
and stop, or stop before accepting an entry when its output buffer is full.
Cookies retain accepted progress across a later error. The existing single-entry
API uses the same parser. No directory buffers survive the call; writable owners
hold serialization through the visitor and refresh snapshots after a mutation.
Read-only inode queries inside the visitor are permitted.

File reads and native mapping queries return contiguous physical ranges or hole
ranges. Extent runs stop at extent and ancestor-index boundaries; legacy runs
stop at their pointer-table boundary, while an absent ancestor represents its
remaining sparse subtree. Unwritten extents return zeros. A read allocates mapping
scratch only when it reaches an external node and reuses that one block for the
rest of the call. Inline extent maps and direct pointers need no scratch buffer.
Checksums and range validation precede each returned mapping; there is no mapping
cache shared across operations or retained across mutations. Native mappings end
at the block containing EOF even if later blocks are preallocated. The platform
owner zeroes padding in that final block before exposing it through its page cache.
Focused tests, warm-cache benchmarks and the full 249-test CI regression pass.

An indexed insertion reuses record slack, compacts a fragmented leaf, or splits
the leaf at a balanced record boundary. Equal hashes retain the collision
continuation bit. Separator insertion, internal-node splitting, root height growth,
new block allocation and inode accounting share the namespace transaction.
Checksums and structural bounds are checked again on fresh buffers before editing.
Removal preserves the index and coalesces leaf records; a moved indexed directory
updates dotdot with the root's index checksum.

Indexed mutation bounds directory size to 1,048,576 blocks. The temporary graph is bounded to
24 MiB; there is no recursive traversal. The index supports a root alone, one
internal level, or two internal levels when the LARGEDIR incompatibility feature
is present. Splits propagate through the validated original parent chain in one
transaction; a full root can grow only within that negotiated height. Further
growth cancels without resource writes. A full single-block linear directory
automatically converts when DIR_INDEX is enabled. The transaction retains dot and
dotdot in the new root, hashes and packs all other records and the new name into
one or two leaves, and publishes mapping, size, allocation and INDEX together.
It uses the superblock's default algorithm and recorded signedness; without legacy
signedness flags, the new root explicitly records an unsigned hash version.
Malformed records, unsupported defaults, exhausted space or credits cancel before
resource writes. Existing multiblock linear directories retain their linear form,
as do filesystems without DIR_INDEX. See the
[directory format](https://docs.kernel.org/filesystems/ext4/directory.html).
These bounds and algorithms are not evidence of accepted
large-volume performance. Duplicate names, stale generations, invalid dot records,
exhausted inodes, reserved-space exclusion and journal-credit exhaustion cancel
before resource writes; uncertain commits poison the owner. Neither adapter exposes
mutations yet.

`ext4_unlink` and `ext4_rmdir` verify the named target's inode number and generation
under that same owner. The complete parent directory is validated before removing
a record; rmdir also requires valid dot/dotdot, an empty child and matching link
counts. Removal coalesces record space without shrinking the parent's map. A first
record in a later block becomes a reusable empty record. Name removal, link counts,
parent mtime/ctime and target ctime commit together. Removing the last link adds
the inode to the legacy orphan list in that transaction, before any reclamation.
Enough journal credits must be available to complete a minimal cleanup batch.

`ext4_rename` receives two generation-checked parent/name/inode identities under
the same exclusive owner. A zero destination inode requires absence; an existing
destination must match its supplied identity. It supports ordinary replacement,
NOREPLACE and EXCHANGE; the last two flags are mutually exclusive. Two names for
the same inode are a no-op, except that NOREPLACE rejects an existing destination.
Symlinks are renamed as objects without following their targets.

One transaction owns both names, parent mtime/ctime, child ctimes, changed dotdot
records and parent link counts. Cross-parent directory moves check the original
ancestry before mutation, rejecting descendants and malformed parent-chain cycles
without recursion. Replacement requires compatible directory/non-directory types;
a replaced directory must be empty. Exchange permits different object types and
populated directories while preserving an acyclic tree. Existing record space is
reused, or the destination parent grows in that transaction. Credit exhaustion
cancels the entire private change before any device write.

A replaced last-link inode enters the same orphan/lifetime path as unlink. A held
victim remains accessible after its old name resolves to the replacement; otherwise
bounded cleanup finishes before returning. Replacing one of several hardlinks
preserves the other names and allocation. Authorization, sticky-directory rules
and native rename locking remain responsibilities of the
platform owner; the portable operation cannot authorize itself.

`ext4_rename_whiteout` adds a character device 0:0 at the old source name in the
same transaction as the move, destination growth, replacement and any dotdot
change. Its separate creation attributes supply explicit owners, zero permissions,
captured times and admitted ACL/security inheritance. It supports NOREPLACE,
rejects EXCHANGE and preserves the ordinary same-inode no-op. Allocation failure
cancels the private transaction; no intermediate whiteout reaches storage.

Directory link counts preserve the DIR_NLINK sentinel of one through mkdir,
rmdir, moves, exchange and replacement. An indexed directory crossing the normal
65,000-link limit publishes that sentinel and monotonic DIR_NLINK enablement in
the same transaction as its new child; replay admits this feature enablement but
rejects clearing it. A private failure changes neither the feature summary nor
the inode. An empty directory with an unknown count is validated by its complete
contents before removal or replacement. A sentinel without the filesystem feature
is corrupt, while ordinary file hardlinks and nonindexed-directory growth retain
the normal link limit. See the
[inode link-count format](https://docs.kernel.org/filesystems/ext4/inodes.html).

The opaque `ext4_inode_hold` represents lifetime under the exclusive core owner.
The platform retains one while a descriptor, mapping or other native object can
still access that inode. Repeated holds share number/generation identity and each
requires release. A held unlinked file remains allocated and supports fresh
snapshots, reads, writes, attribute changes and truncate. Ordinary namespace lookup
cannot resurrect it. Unlinked directories expose size zero and reject new children;
held symlinks retain their target bytes. The last release reclaims the inode and its
map in bounded transactions, atomically updating its predecessor if the orphan is
not at the list head. Inode reuse advances the preserved generation.

Unheld deletion finishes reclamation before returning. A last-release error consumes
the reference and poisons the instance, leaving recovery to finish committed work.
Unmount invalidates all holds without writing. With held unlinked objects, sync
checks the complete list against the live registry and flushes it while retaining
the recovery markers; it cannot declare the volume clean. The hold registry is a
bounded linear list, not an accepted scalable concurrent inode cache. These APIs
do not replace native vnode/FD/mapping lifetime, page-cache locking or policy.

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

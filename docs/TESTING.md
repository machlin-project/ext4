# Automated tests

The test matrix is part of the filesystem implementation. Each mutating contract
needs observable success, errors and recovery behavior before an adapter exposes
it. Tests compare contents and metadata, and use independent filesystem tools or
Linux where the wire format or semantics are the subject. A successful build or
fixture-generation command does not prove the corresponding runtime behavior.

## Portable suites

`make test` runs the reader, malformed-image, file-write and journal durability tests
with ASan/UBSan. The 1 KiB and 4 KiB journal suites use separate volatile and
persistent device states. They interrupt each write and flush, then reopen the
surviving medium. Cases retain none, all or alternating pending blocks; partial
writes additionally tear the selected request. This tests the stated storage
contract, not physical power-loss protection on a particular disk.

| Area | Cases and required observation |
| --- | --- |
| Reader | Eleven explicit format profiles; exact bytes, sparse/unwritten data, extents/indirects, indexed directories, cookies, links, timestamps and mappings |
| Validation | Invalid geometry/features and checksums, malformed inode fields/timestamps, bounded reads/allocations |
| Inode updates | Full-width UID/GID, permission bits, generation identity, selective updates, hardlink visibility, preserved mappings/counts and neighboring inode records |
| Writable timestamps | Signed and extended epoch boundaries, nanosecond bounds, birth time, 128-byte inode limits and each extra_isize field boundary; rejected updates perform no writes |
| File overwrite | Complete-file comparison after an unaligned three-block overwrite, preserved EOF and untouched bytes, hardlinks, zero-length operation and read-only rejection |
| Write admission | Stale generation, invalid fields/types/ranges, unsupported holes/growth, xattrs/flags, metadata-target exclusion; no writes before successful validation |
| File mutation failures | Every allocation/read in the small overwrite, every write/flush cut through clean finish, torn writes and three survival patterns; consistent inode and data together after recovery, poisoned-instance read/write rejection |
| Transaction ownership | One active writer/transaction, duplicate buffer identity, credit exhaustion, cancellation, empty commit, protected journal/control ranges |
| Journal layouts | Legacy without checksums, checksum v2/v3, 32/64-bit tags, escape records, multiple descriptor blocks, ring wrap and sequence wrap |
| Persistence | Every write/flush interruption, three pending-write survival patterns, partial writes, durable commit before home writes, all-old or all-new metadata after recovery |
| Recovery faults | Interrupted replay and retry, allocation/read failures in each distinct ownership phase, no leaks, no writes on already clean media |
| Corruption | Corrupt committed payload/descriptor rejects replay, invalid commit discards the incomplete tail; a torn checksummed control block fails closed |
| Independent replay | debugfs-authored commits, unfinished tail, revokes and later reuse; exact recovered blocks, repeated recovery and e2fsck |
| Linux roundtrip | Linux mounts/replays our pending log, commits metadata changes, stops without unmount; our core replays the Linux-authored transaction, then contents, owners, mode and e2fsck are checked |

Read/allocation failure loops select the beginnings and ends of repeated journal
mapping operations and every distinct surrounding phase. They do not claim to
inject at every identical mapper call. Write/flush interruption loops do cover
every operation in the small transaction and recovery sequence.
The file-write suite additionally injects every allocation/read after its writable
mount, including validation and snapshot preparation. It does not enumerate every
identical call made while mapping the journal during mount.

Each unsupported format, fail-closed corruption case and unavailable runtime is
reported separately from successfully recovered transactions. In particular,
damaging a primary superblock across sectors can make its checksum unverifiable;
the core rejects that medium instead of guessing geometry or declaring it clean.

## Platform suites and remaining coverage

Mounted tests verify the adapter's ordinary file operations, metadata, directory
positions, read-only enforcement, concurrent readers and mmap. XNU lifetime tests
retain open files or mappings across attempted unmounts. These do not establish
writable UBC, cache coherence during truncate or filesystem operation concurrency.

Before enabling general writes, extend the matrix to allocation/full devices,
create/link/unlink/rename, orphan cleanup, open-but-unlinked files, truncate versus
mmap/pageout, failed writeback, metadata locking and forced unmount. Exercise
large physical addresses and fragmented journals as real images, not only flag
variations. ACL/xattr/security and LXNU operation-policy tests must include native
controls and mixed-ABI races. Journal v1 checksums, asynchronous/fast commits and
external journals require their own accepted recovery cases.

Keep CPU sanitizers, freestanding stack checks, unsigned FSKit builds, kext builds,
stock FSKit mounts, custom-kernel execution and LXNU acceptance as distinct rows.
The current evidence and missing rows are recorded in [ACCEPTANCE.md](ACCEPTANCE.md).

Prepared builds and test execution run on Luna; VM preparation belongs to Sol.
The main agent writes tests, investigates failures and reviews evidence. Workers
preserve complete command output in ignored artifacts and never repair a failing
fixture or skip a case to manufacture a passing report.

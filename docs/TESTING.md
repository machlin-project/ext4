# Automated tests

The test matrix is part of the filesystem implementation. Each mutating contract
needs observable success, errors and recovery behavior before an adapter exposes
it. Tests compare contents and metadata, and use independent filesystem tools or
Linux where the wire format or semantics are the subject. A successful build or
fixture-generation command does not prove the corresponding runtime behavior.

## Portable suites

`make test` runs the reader, malformed-image, namespace, file-write, allocation, truncate, orphan and journal tests
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
| Write admission | Stale generation, invalid fields/types/ranges, unsupported xattrs/flags, metadata-target exclusion; no writes before successful validation |
| Namespace creation | Regular files, mkdir dot/dotdot and parent link counts, full-width owners and precise captured times, nested hard links and hard links to symlinks, sparse writes through new aliases |
| Symlink creation | Lengths 1/59/60/61 and block-size-minus-one, opaque non-UTF-8 bytes, zeroed terminator/tail, inline versus mapped accounting, hardlink identity, precise metadata and readonly remount |
| Symlink validation | Empty/NUL/oversized targets, stale/duplicate/indexed destinations, inode and journal exhaustion, mapped-target ENOSPC versus successful inline creation, checksummed invalid inline lengths |
| Namespace allocation | Full-directory append, lazy inode bitmap/table ownership, group transitions, all inodes exhausted, hard links after exhaustion, cleared released records, generation increment/wrap and inherited inode flags |
| Namespace validation | Invalid/duplicate names, stale parent/target, directory hard links, indexed/immutable parents, link limits, malformed records/dot entries, inode bitmap/count/high-water corruption and small-journal credit exhaustion |
| Namespace failures | Every allocation/read after mount and every write/flush cut for create/mkdir/link and short/long/maximum symlinks in existing and appended blocks and at an inode-group transition; full-resource old/new comparison, with durable commits forced to the new state |
| Removal | Last and nonlast hardlinks, nonempty and empty directories, dot/dotdot ownership, short/long symlinks, first records in later blocks, free-record reuse, coalescing and empty multiblock directory reclamation |
| Removal validation | Invalid names/arguments, stale parent/target generation, mismatched target identity, readonly/type errors, indexed directories, malformed late/duplicate entries, immutable objects, protected target mappings and minimum cleanup credits; no mutation on rejection |
| Held unlinked objects | Shared hold identity/refcounts, reads/writes/setattr/truncate after unlink, independent linked truncate while orphans remain, held directories/symlinks, no early inode reuse, generation advance after release, nonhead orphan removal, sync markers and unmount without implicit writes |
| Removal and release failures | Every operation allocation/read and every write/flush cut for unlink/rmdir, held unlinked truncate and final release at the head or inside the orphan list; consumed-reference error poisoning, leak balance, committed deletion completion and whole-resource comparison outside the journal |
| Allocation and growth | Unaligned initial writes, sparse gaps, written allocations beyond EOF, deterministic fragmented insertion, extent root/leaf/parent splits, and direct through triple-indirect boundaries |
| Unwritten conversion | Independent debugfs allocation with deliberately nonzero backing bytes; partial writes preserve zero semantics and split/merge extent records |
| Free-space ownership | Group and superblock counters, inode data/mapping block counts, lazy bitmaps, short final groups, exhaustion to zero free blocks, reserved-space rejection, late credit failure with no writes |
| Allocator corruption | Damaged bitmap CRC, forged free system blocks with matching checksums/counters, and inconsistent bitmap/free-count pairs; journal data and mapping blocks stay protected |
| Allocation failures | Every allocation/read in bounded growth and mapping promotion; every write/flush cut with three survival patterns and partial writes; all blocks outside the journal match the complete old or new image |
| Truncate | Partial/aligned/zero sizes, fragmented trees with surviving branches, repeated shrink/grow, root collapse, unwritten preallocation, direct/single/double/triple path release, maximum sparse size, hardlink identity and zeroed reuse after disk exhaustion |
| Truncate validation | Duplicated data blocks, data/mapping aliases and protected metadata with repaired checksums, wrong inode block counts, stale generation, invalid admission, read-only/type/range errors and late credit exhaustion without writes |
| Truncate failures | Every allocation/read and every write/flush cut in tail shrink, complete removal and exposure of allocated bytes past EOF; recovered inode, mapping, bitmaps, counters and data agree on one commit |
| Live truncate | Multiple committed batches, persistent target size/permissions/timestamps, post-intent error poisoning, unchanged output on error, 257 indirect leaves beyond atomic capacity, retained nonzero unwritten backing bytes |
| Live truncate failures | Every operation allocation/read and write/flush cut, including the large indirect map; a known durable first commit must recover the new outcome, never roll back to the original inode |
| File mutation failures | Every allocation/read in the small overwrite, every write/flush cut through clean finish, torn writes and three survival patterns; consistent inode and data together after recovery, poisoned-instance read/write rejection |
| Transaction ownership | One active writer/transaction, duplicate buffer identity, credit exhaustion, cancellation, empty commit, protected journal/control ranges |
| Journal layouts | Legacy without checksums, checksum v2/v3, 32/64-bit tags, escape records, multiple descriptor blocks, ring wrap and sequence wrap |
| Persistence | Every write/flush interruption, three pending-write survival patterns, partial writes, durable commit before home writes, all-old or all-new metadata after recovery |
| Recovery faults | Interrupted replay and retry, allocation/read failures in each distinct ownership phase, no leaks, no writes on already clean media |
| Orphan recovery | Linked partial truncates and hardlinks; Linux-authored open-unlinked ordinary/sparse files, directories, short/long symlinks and FIFO; inode/data/mapping reclamation, partial-tail zeroing, repeated recovery |
| Orphan validation | Reserved/free/out-of-range inode numbers, self/pair/tail cycles, zero/unsupported types and flags, xattrs, inode checksum/block-count errors and protected data targets; no cleanup transaction on rejection |
| Orphan failures and scale | Every replay/recount/cleanup allocation/read, every write/flush cut with three survival patterns and torn writes; a 257-leaf indirect map exceeds atomic journal capacity but completes in batches |
| Orphan-file recovery | Independently allocated modern files across writable profiles; entries at the first and last slots, mixed legacy/file ownership, indirect file maps, empty marker-only recovery and live truncate on modern volumes |
| Orphan-file validation | Missing/inconsistent feature markers, private inode bounds/type/size/links/flags, tail magic/CRC/address/generation binding, sparse/aliased/protected maps, duplicate slots within/across blocks and against the legacy chain; late invalid entries reject before cleanup |
| Orphan-file ownership and faults | Private inode, data and mapping nodes reject ordinary mutation; hidden slots reject clean writable mount; each transfer and cleanup allocation/read/write/barrier is faulted, retaining one recoverable owner per inode |
| Corruption | Corrupt committed payload/descriptor rejects replay, invalid commit discards the incomplete tail; a torn checksummed control block fails closed |
| Independent replay | debugfs-authored commits, unfinished tail, revokes and later reuse; exact recovered blocks, repeated recovery and e2fsck |
| Linux roundtrip | Linux mounts/replays our pending log or checks a clean mutation export, commits metadata/growth/truncate, stops without unmount; our core replays the Linux-authored transaction, then contents, owners, mode and e2fsck are checked |

Read/allocation failure loops select the beginnings and ends of repeated journal
mapping operations and every distinct surrounding phase. They do not claim to
inject at every identical mapper call. Write/flush interruption loops do cover
every operation in the small transaction and recovery sequence.
The file-write suite additionally injects every allocation/read after its writable
mount, including validation and snapshot preparation. It does not enumerate every
identical call made while mapping the journal during mount.
The orphan suite samples the first and last 32 callbacks of the repeated journal
mapping phase, then injects every callback through replay, summary reconstruction
and cleanup. Its large indirect-map case checks successful reclamation beyond the
atomic credit limit; exhaustive fault cuts use smaller multi-transaction maps.
The separate live API suites enumerate every callback after writable mount and
target lookup, including all subsequent intent cleanup and final inode refresh.
Their large-map suite also runs the full fault matrix, with its own time limit.

`ext4-namespace-test` runs create/mkdir/link and directory-growth cases on every
writable profile, including modern orphan files. Six small multi-group images
add legacy indirect mapping, absent checksums, absent FILETYPE, 128-byte inodes
and uninitialized inode tables. `--groups` fills the first inode group
before faulting the next creation; `--exhaust` fills all eight groups with long
names, growing the parent through legacy indirect blocks. An actual indexed
directory is a separate unsupported-operation test, not an accepted write case.

`--symlinks` adds inline and block-backed targets, including a hardlinked binary
target and the maximum permitted length. It combines with `--groups` to exercise
allocation into initialized and lazy inode groups. Short targets can still be
created when all remaining data blocks are reserved; mapped targets must fail
without leaking the inode or changing its caller output.

`--smoke --export DIR` preserves completed operations and two distinct interrupted
states: before the commit write and immediately after its successful durability
barrier. `check_namespace.py` requires old and new outcomes respectively, then
compares each to independent journal-only replay. It checks exact directory names,
inode identity, owners/times, link counts, sparse bytes, free-space accounting,
nonrepairing e2fsck, unchanged sources and idempotent recovery. Malformed input,
allocation failures and deliberately torn primary superblocks remain separate
from successful journal recovery.
For symlinks, debugfs independently resolves inode locations or data block maps;
the checker reads raw target bytes and requires a zeroed tail, avoiding loss from
text decoding or a C-string display of binary targets.

`ext4-removal-test` uses the same ordinary, modern and small profiles. It injects
failures into five namespace operations: removing one alias, the last regular-file
link, an empty directory, an inline symlink and a mapped symlink. A 35-block file
forces bounded cleanup across transactions. The last-release matrix starts after
unlink is durably committed; every recoverable interruption must finish deletion,
even when release fails before writing. A separate live predecessor exercises
nonhead list removal and subsequent cleanup of the remaining held object. A third
held-object sequence faults truncate-to-zero before releasing either hold, checking
that a retained cleanup entry remains recoverable throughout the operation.

The removal exporter preserves `removed-` operation sequences and paired
`remove-before-`, `remove-atomic-`, `remove-pending-` and `remove-uncommitted-`
images. The independent checker verifies exact retained names/data/attributes,
directory link counts, released inode/data/mapping space and both commit outcomes.
Linux checks the removed namespace and remaining hardlink before creating its own
objects; reverse recovery requires that no removed name reappears and the alias's
identity, metadata and bytes remain intact.

The Linux namespace probe reads expectations independently decoded from the
checked clean image. It checks stat, lookup, complete readdir, file bytes and
exact/truncated readlink results without a returned terminator,
then creates and links new objects and commits a cross-directory rename. On an
inode-exhausted image it first requires ENOSPC and then reuses the only released
inode. Returned pending Linux transactions are replayed by both the core and
e2fsprogs. Linux can leave stale primary free-space summaries: the oracle admits
only the exact summary diagnostics confirmed by independently summed groups;
the core result must already have correct primary totals and no such diagnostics.

`ext4-write-test --truncate` runs resize and freeing cases across the selected
profiles. Truncate exports include the final reused block and three intermediate
sizes retaining different portions of the fragmented mapping tree; independent
byte oracles and e2fsck inspect those states as well. `--export-only --export DIR`
runs the successful export scenarios without repeating the fault loops already
run by CTest. It is an artifact-generation mode, not the complete test suite.

`ext4-orphan-test` constructs linked-truncate intents in copies of the ordinary
fixtures. `--pending` instead consumes untouched Linux orphan images. All mutations
and errors use RAM copies. `--smoke --export DIR` exports successful pending/clean
pairs for independent inspection. `check_orphans.py` uses e2fsck as a separate
replayer, checks inode allocation and live contents, and requires a nonrepairing
e2fsck pass on the core result before any oracle comparison. It measures unrelated
directory indexing performed by e2fsck separately; unlinked cases must also return
exactly to the pre-Linux baseline's free-block/inode totals.

`generate_orphan_file_fixtures.py` uses tune2fs to add a fixed orphan file to
copies of the checked clean fixtures. It verifies the exact feature change,
private inode size, initialized mappings, empty tails, source hashes and e2fsck.
The ordinary ten profiles have four file blocks; additional 17- and 512-block
indirect fixtures exercise external mapping nodes and the supported size bound.
`ext4-orphan-test --orphan-file` creates retained linked entries at opposite ends
of that file; `--mixed` puts one inode in each representation. These run the same
complete cleanup fault matrix as legacy lists. The size-bound case is a smoke
test; the 17-block mapping case runs the full matrix.

For modern Linux fixtures, the independent checker requires six reported slot
transfers, six reclaimed inodes, restored baseline accounting and an empty orphan
file identical to e2fsck's result. It verifies unchanged private-inode identity,
size and physical mapping, and compares existing `/empty` contents to the actual
hash-protected source baseline. Orphan generation must not assume that every
accepted source profile began with the same size for that live file.

`ext4-orphan-test --live` invokes the public live truncate API on ordinary and
independently allocated unwritten fixtures. `--large` builds a sparse indirect
map beyond atomic capacity. `--smoke --export DIR` writes before/after images and
a representative interrupted live call. `check_resize.py` requires nonrepairing
e2fsck on the initial and completed states, verifies exact bytes, captured inode
fields, retained physical mappings and released space, then compares portable
and e2fsck recovery of the interrupted call. Written tails must be zero; unwritten
retained backing data must remain unchanged and inaccessible. Original exports
are hash-protected and repeated recovery must make no changes.

Each unsupported format, fail-closed corruption case and unavailable runtime is
reported separately from successfully recovered transactions. In particular,
damaging a primary superblock across sectors can make its checksum unverifiable;
the core rejects that medium instead of guessing geometry or declaring it clean.

## Platform suites and remaining coverage

Mounted tests verify the adapter's ordinary file operations, metadata, directory
positions, read-only enforcement, concurrent readers and mmap. XNU lifetime tests
retain open files or mappings across attempted unmounts. These do not establish
writable UBC, cache coherence during truncate or filesystem operation concurrency.
`ext4-mounted-symlink-test MOUNTPOINT BLOCK_SIZE` consumes a clean `symlinks-`
export. It checks six target lengths/encodings and their hardlinks, inode identity,
mode/owners/accounting and 7,680 readlink calls from four concurrent workers.
Buffer capacities cross the inline and native path-size boundaries; each call
must preserve the exact opaque bytes and leave the byte after its result untouched.

Before enabling general writes, extend the matrix to platform allocation/full devices,
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

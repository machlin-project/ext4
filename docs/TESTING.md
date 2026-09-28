# Automated tests

The test matrix is part of the filesystem implementation. Each mutating contract
needs observable success, errors and recovery behavior before an adapter exposes
it. Tests compare contents and metadata, and use independent filesystem tools or
Linux where the wire format or semantics are the subject. A successful build or
fixture-generation command does not prove the corresponding runtime behavior.

Develop related implementation and tests as one batch. Compilation and focused
checks provide feedback during development; complete regression and applicable
independent/Linux checks establish acceptance after the batch is ready. A small
edit or local commit does not by itself require another full run. Repeat checks
when later changes affect the behavior they covered, and preserve completed
evidence for unchanged code. An active run keeps its compiled revision, binaries
and inputs while development continues on the next batch.
The full GitHub workflow ignores pushes and pull requests that change only
Markdown or `.clang-format`; manual dispatch remains available. Publish related
code commits together at the batch boundary to schedule one complete CI run.

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
| Legacy group checksums | CRC16 vectors and independent polynomial reference; 32 through 1024-byte descriptors, group identity and every descriptor byte; real 32/64-byte groups, lazy bitmap/table initialization, allocation/freeing and recovery |
| Xattr reads | Ten independently authored profiles; inode-body/external/shared blocks, raw ACL and binary bytes, empty/full-block values, exact sorted get/list, generation identity, failure-atomic outputs, all allocation/read faults and malformed late/duplicate entries |
| Xattr independent inspection | Exact values and lists against debugfs; unknown namespaces remain opaque and fsck-clean; synthetic aliased values are explicitly reader-only compatibility cases with expected e2fsck rejection |
| Xattr mutation | Atomic distinct-key batches and inode metadata; create/replace/remove existence policy; shared-block copy-on-write and reference release; symlink storage preservation; every allocation/read failure and write/flush interruption |
| Large attribute values | EA_INODE values through 64 KiB; private-inode validation and CRC; shared body/value/block references; logical versus physical allocation accounting; create/replace/shrink/remove, attributed namespace/data operations and staged final release; zero-reference orphan recovery; allocation/read failures and interrupted value transactions |
| Xattr recovery inspection | Exact values, names, metadata, allocation and shared references in completed, committed and uncommitted exports; separate core/e2fsprogs replay, nonrepairing checks and unchanged repeated recovery |
| Xattr capacity | Completely full bitmaps, rollback after private shared-reference changes, unchanged/body-only updates without allocation, unique-block replacement and freed-block reuse; full values, exact two-region packing and byte-preserving ENOSPC |
| Xattr ownership | Invalid file types, data/attribute aliases, invalid references, protected metadata targets, inconsistent i_blocks and invalid captured times; no device/output/counter changes on rejection |
| Inode updates | Full-width UID/GID, permission bits, generation identity, selective updates, hardlink visibility, preserved mappings/counts and neighboring inode records |
| Writable timestamps | Signed and extended epoch boundaries, nanosecond bounds, birth time, 128-byte inode limits and each extra_isize field boundary; rejected updates perform no writes |
| File overwrite | Complete-file comparison after an unaligned three-block overwrite, preserved EOF and untouched bytes, hardlinks, zero-length operation and read-only rejection |
| Large writes | Requests larger than a transaction, exact durable prefixes on error, one-time xattr CREATE/REMOVE, held-unlinked writes, reserved-space exhaustion and retry after explicit recovery |
| Write admission | Stale generation, invalid fields/types/ranges, unsupported xattrs/flags, metadata-target exclusion; no writes before successful validation |
| Namespace creation | Regular files, mkdir dot/dotdot and parent link counts, full-width owners and precise captured times, nested hard links and hard links to symlinks, sparse writes through new aliases |
| Symlink creation | Lengths 1/59/60/61 and block-size-minus-one, opaque non-UTF-8 bytes, zeroed terminator/tail, inline versus mapped accounting, hardlink identity, precise metadata and readonly remount |
| Symlink validation | Empty/NUL/oversized targets, stale/duplicate destinations, malformed indexed parents, inode and journal exhaustion, mapped-target ENOSPC versus successful inline creation, checksummed invalid inline lengths |
| Namespace allocation | Full-directory append, lazy inode bitmap/table ownership, group transitions, all inodes exhausted, hard links after exhaustion, cleared released records, generation increment/wrap and inherited inode flags |
| Namespace validation | Invalid/duplicate names, stale parent/target, directory hard links, malformed indexed and immutable parents, link limits, malformed records/dot entries, inode bitmap/count/high-water corruption and small-journal credit exhaustion |
| Namespace failures | Every allocation/read after mount and every write/flush cut for create/mkdir/link and short/long/maximum symlinks in existing and appended blocks and at an inode-group transition; full-resource old/new comparison, with durable commits forced to the new state |
| Removal | Last and nonlast hardlinks, nonempty and empty directories, dot/dotdot ownership, short/long symlinks, first records in later blocks, free-record reuse, coalescing and empty multiblock directory reclamation |
| Removal validation | Invalid names/arguments, stale parent/target generation, mismatched target identity, readonly/type errors, indexed directories, malformed late/duplicate entries, immutable objects, protected target mappings and minimum cleanup credits; no mutation on rejection |
| Held unlinked objects | Shared hold identity/refcounts, reads/writes/setattr/truncate after unlink, independent linked truncate while orphans remain, held directories/symlinks, no early inode reuse, generation advance after release, nonhead orphan removal, sync markers and unmount without implicit writes |
| Removal and release failures | Every operation allocation/read and every write/flush cut for unlink/rmdir, held unlinked truncate and final release at the head or inside the orphan list; consumed-reference error poisoning, leak balance, committed deletion completion and whole-resource comparison outside the journal |
| Rename | Same/cross-parent moves, both directory-record orders, regular files/directories/inline and mapped symlinks, replacement of held victims, mixed-type and populated-directory exchange, dotdot/parent links, unchanged source identity and captured times |
| Rename validation | Same-inode hardlink no-op, NOREPLACE, stale identities, invalid names/arguments, nonempty replacement, descendant moves, malformed ancestry cycles/types, late corrupt/duplicate entries, parent link limits, indexed move/exchange, cleanup-credit exhaustion and reserved-space rejection with successful record reuse |
| Indexed namespace | Six independent hash variants, complete index-graph validation, leaf compaction/splitting, root growth, internal-node splits, real colliding names, empty leading collision leaves, indexed moves/exchange/removal and modern orphan-file combinations |
| Indexed recovery | Every allocation/read failure and write/flush cut during three split transitions, changing-read structural corruption, private cancellation on capacity limits, exact committed/uncommitted replay against e2fsprogs |
| Rename failures | Every allocation/read and every write/flush cut for eleven move/replacement/exchange/growth cases, including 35-block victims, retained hardlinks and held victim release; exact old/new resource comparison outside the journal and independent replay of both commit outcomes |
| Allocation and growth | Unaligned initial writes, sparse gaps, written allocations beyond EOF, deterministic fragmented insertion, extent root/leaf/parent splits, and direct through triple-indirect boundaries |
| Unwritten conversion | Independent debugfs allocation with deliberately nonzero backing bytes; partial writes preserve zero semantics and split/merge extent records |
| Preallocation and hole punching | Extent reservation with growth/KEEP_SIZE, explicit indirect reservation rejection, full-block removal and partial-edge zeroing in both maps, middle-extent splitting, path pruning/collapse, durable partial progress, one-time xattr transitions, held-unlinked cleanup and private/I/O failure recovery |
| Free-space ownership | Group and superblock counters, inode data/mapping block counts, lazy bitmaps, short final groups, exhaustion to zero free blocks, reserved-space rejection, late credit failure with no writes |
| Preallocated writes on full filesystems | Full inode roots and external extent leaves, bounded initialization of nonzero unwritten backing, no new allocation or relocation, exact metadata/data preservation, resource failures and every write/flush cut across preparation and final data commits |
| KEEP_SIZE growth on full filesystems | Reserved leaf capacity for partial EOF; bounded prefix zeroing; atomic old-boundary merge/new-boundary split with data/EOF; repeated growth across root/external-leaf reservations; size-only growth and re-reservation; capacity retained after punching or failed growing fallocate; imported full-tree rejection; exact durable prefix before an unallocated hole; cross-engine pending/uncommitted replay |
| Persistent inode flags | Atomic masked flag/ctime updates; protected writes, metadata and namespace operations; append-at-EOF and reservation; type-specific inheritance; held-unlinked cleanup; interrupted set/clear transactions with exact xattr/data preservation |
| Distributed group geometry | META_BG, hybrid contiguous/distributed descriptors, SPARSE_SUPER2 with zero/one/two backups, 32/64-byte descriptors and CRC16/CRC32C; public creation through every inode group, local block allocation, final deletion, backup preservation, malformed bounds and immutable geometry during replay |
| Full-block namespace | Independently filled bitmaps; create/mkdir/inline and mapped symlink/link/rename ENOSPC in linear and indexed parents; record reuse, existing-block overwrite and sparse growth; rollback after allocating the only available block; successful mkdir after releasing two blocks |
| Allocator corruption | Damaged bitmap CRC, forged free system blocks with matching checksums/counters, and inconsistent bitmap/free-count pairs; journal data and mapping blocks stay protected |
| Allocation failures | Every allocation/read in bounded growth and mapping promotion; every write/flush cut with three survival patterns and partial writes; all blocks outside the journal match the complete old or new image |
| Truncate | Partial/aligned/zero sizes, fragmented trees with surviving branches, repeated shrink/grow, root collapse, unwritten preallocation, direct/single/double/triple path release, maximum sparse size, hardlink identity and zeroed reuse after disk exhaustion |
| Truncate validation | Duplicated data blocks, data/mapping aliases and protected metadata with repaired checksums, wrong inode block counts, stale generation, invalid admission, read-only/type/range errors and late credit exhaustion without writes |
| Truncate failures | Every allocation/read and every write/flush cut in tail shrink, complete removal and exposure of allocated bytes past EOF; recovered inode, mapping, bitmaps, counters and data agree on one commit |
| Live truncate | Multiple committed batches, persistent target size/permissions/timestamps, post-intent error poisoning, unchanged output on error, 257 indirect leaves beyond atomic capacity, retained nonzero unwritten backing bytes |
| Live truncate failures | Every operation allocation/read and write/flush cut, including the large indirect map; a known durable first commit must recover the new outcome, never roll back to the original inode |
| File mutation failures | Every allocation/read in the small overwrite, every write/flush cut through clean finish, torn writes and three survival patterns; consistent inode and data together after recovery, poisoned-instance read/write rejection |
| Transaction ownership | One active writer/transaction, duplicate buffer identity, credit exhaustion, cancellation, empty commit, protected journal/control ranges |
| Journal layouts | Legacy without checksums, checksum v1/v2/v3, 32/64-bit tags, escape records, multiple descriptor blocks, ring wrap and sequence wrap |
| External journals | Independent filesystem/journal UUIDs and device geometry; malformed association rejection before writes; distinct cache survival and interrupted replay; unchanged device control prefix; paired idempotence and exact core/e2fsprogs/Linux replay, including native open-unlinked cleanup |
| Transaction checksum v1 | Descriptor/data/commit corruption before replay, type/size validation, all-zero legacy transition, incompatible checksum combinations, independent committed prefix and later rewrite, native Linux revoke records |
| Async commit | V1/v2/v3 and 32/64-bit tags; interrupted tail versus later-commit corruption, valid preceding transactions, sequence and timestamp bounds; independent committed/discarded replay and native Linux revokes |
| Revoke advertisement | Committed revokes before the feature bit is durable; 32/64-bit targets, checksum variations, later reuse across sequence wrap, and malformed-record rejection before writes |
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
directory separately exercises create/mkdir/link/symlink; the dedicated indexed
matrix adds growth, structural validation and recovery.

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

`ext4-rename-test` runs 160 functional sequences per writable profile, varying
parent identity, entry creation order, source type, replacement, lifetime and
exchange, including every mixed non-directory replacement and every exchange type
pair. Separate guards verify unchanged media/output on rejection and retained
identity for hardlink aliases and held sources. The actual indexed fixture separately
checks missing sources, indexed destinations, mixed exchange and child rename.
`--functional-only` omits the fault/export scenarios;
it does not stand in for the complete Meson matrix.
Meson registers each writable image as its own rename test, retaining all 160
functional sequences and eleven fault operations for that image. This keeps
per-test timeouts independent of the number of enabled image profiles; CI still
limits execution to two concurrent tests. Removal also schedules each image
separately, retaining its complete fault matrix. It has a separate CI job so its
runtime and independent checks do not consume the namespace job's whole deadline.

Eleven fault scenarios cover a same-parent file move, a cross-parent populated
directory move, last-link file/directory/short-symlink/long-symlink replacement,
replacement of one hardlink, mixed file/directory exchange, replacement and final
release of a held file, destination growth with a NAME_MAX entry, and exchange of
two populated directories. Allocation/read failures start after the prepared
mount/lookups/hold. Every write/flush cut combines the three survival patterns and
partial writes. A known durable namespace commit must recover the complete new
state, including victim cleanup.

`--smoke --export DIR` preserves `rename-before-`, `rename-atomic-`,
`rename-pending-` and `rename-uncommitted-` images for all eleven operations.
`check_rename.py` independently decodes names, dotdot, inode identity, generations,
owners/times, link counts, mappings and contents. It checks exact free-space
changes, nonrepairing e2fsck, separate journal/orphan replay for both outcomes,
idempotence and unchanged protected input hashes. Linux roundtrips consume those
checked expectations and require the complete renamed tree to remain unchanged
after Linux creates additional objects and commits its own journal transaction.

`ext4-namespace-test --special` checks character/block device encodings, FIFO and
socket creation, invalid device numbers, inherited attributes, hardlinks, held
unlink, replacement and inode reuse. Six creation operations reuse the ordinary
allocation/read and interrupted-I/O fault engine. `ext4-rename-test --whiteout`
adds six atomic scenarios: file move, populated cross-parent directory move,
replacement, destination index growth, whiteout attributes and held replacement.
The independently checked exports preserve committed and uncommitted journals.
The Linux namespace probe verifies these objects with `lstat`, then creates its
own device/FIFO/socket and performs `RENAME_WHITEOUT`. Portable and e2fsprogs replay
must agree on returned metadata; `ext4-namespace-test --special-read` checks the
portable decoder against those Linux-created special inodes without writes.

`generate_index_fixtures.py` builds twenty independently checked indexed profiles.
`directory-index-*` validates complete graphs and malformed structures;
`indexed-write-*` exercises namespace operations and tree growth;
`indexed-faults-*` enumerates failures at leaf splits, root growth and internal-node
splits. Explicit skips identify transitions not applicable to a profile.
`check_index_write.py` verifies byte-name mappings, attributes, links and accounting.
`check_index_faults.py` compares complete old/new directory bytes and object state
against separate journal-only recovery, including idempotence and source hashes.

`directory-create-index-*` fills a one-block linear directory and verifies atomic
conversion to an HTree with one or two leaves, across all writable base profiles.
The 1/4 KiB cases enumerate preparation and storage failures; the hash case checks
all six versions with signed, unsigned and absent legacy flags. The no-filetype
and separately journaled 64 KiB profiles cover their distinct wire encodings.
`directory-links-*` uses compact in-memory counter models for exhaustive boundary
faults, then exercises mkdir, rmdir, moves, exchange and replacement with unknown
parent or empty-victim counts. Models with intentionally inflated counts are never
claimed as clean filesystem images. `generate_directory_links.py` separately
authors an e2fsck-clean directory with 64,998 real child directories and no
DIR_NLINK feature. Its real overflow exports pass the same independent recovery
checker and Linux roundtrip as index creation. The generator's `--large-block-only`
mode authors the journaled 64 KiB image in a separate directory.

`generate_index_fixtures.py --large-dir` uses e2fsprogs to create three trees with
two internal levels (metadata checksums, indirect maps and legacy group checksums)
and a full root ready to grow. They retain over 46,000 names each. The separate
`generate_large_directory_edges.py` copies those verified sources and builds
underfilled trees with full selected ancestor chains, retaining independently
hashed names. debugfs frees unused blocks and updates inode accounting; a separate
wire/checksum encoder writes the retained graph. Nonrepairing e2fsck must accept
every fixture. Nine graphs occupy 372 through 751 directory blocks, bounding
exhaustive failure tests without reducing the transitions they exercise.

`large-dir-*` checks complete large graphs, independent lookup/iteration names,
namespace mutations, root height growth, two internal splits in one transaction,
and rejection/reuse at maximum height. Compact graphs cover structural damage and
every allocation/read and write/flush cut of the two growth transitions. An
isolated in-memory reader tree puts one matching name at successive positions in
an eight-leaf collision chain spanning both internal levels; it also checks the
absent result and all lookup resource failures. That modeled collision tree is
separate from the independently accepted fixture images. The large functional
probe adds 32 long names; smaller legacy profiles retain their existing workloads.

`generate_index_collisions.py` finds pairs of NAME_MAX byte names with the same
major hash and independently confirms both through numeric-version debugfs queries.
`indexed-edges-*` forces the equal hashes across separate leaves with different
target inodes, then renames/removes the leading name while retaining the continuation.
It also creates separated free record gaps whose combined capacity fits a long
name; insertion must compact without growing the directory or allocating blocks.
The independent edge checker verifies both collision states and the compacted
image, including the empty leading leaf, exact identities and accounting.
Deep fixtures also place the odd collision separator in the root: the two leaves
belong to different internal nodes, even after the leading leaf becomes empty.
Repartitioning retains every mapped block, reusing an empty leaf as an additional
index node when needed. The independent dump must show those distinct parents;
nonrepairing e2fsck still checks the complete exported image. Lookup allocation
and read failures are injected for both colliding names and the removed name.

`directory-lookup-*` compares every root, indexed and peer name against independently
decoded debugfs inode identities. The generator saves byte names as hex in `.lookup`
files. Both read-only and exclusive writable owners must leave resource bytes
unchanged. The same suite covers every lookup allocation/read failure for found,
absent, dot and dotdot queries, unchanged outputs, invalid arguments, malformed
root/node/leaf records, child aliases, wrong hash ranges and missing or conflicting
hash-signedness flags. Header decoding must preserve its input on success and error.
The full-root capacity fixture uses the same checks, with a callback/allocation
bound that detects a return to entry-by-entry indexed lookup. These bounds count
core work; they are not wall-clock benchmarks. Existing verified images can gain
independent expectations without regeneration:

```sh
python3 tests/generate_index_fixtures.py --lookup-only --output artifacts/index-fixtures
```

The Linux namespace probe reads expectations independently decoded from the
checked clean image. It checks stat, lookup, complete readdir, file bytes and
exact/truncated readlink results without a returned terminator,
then creates and links new objects and commits a cross-directory rename. On an
inode-exhausted image it first requires ENOSPC and then reuses the only released
inode. Expected names are encoded as hex bytes, preserving non-UTF8 names without
whitespace or Unicode line-separator ambiguity. Indexed cases verify every name
through Linux lookup and create Linux's additional objects inside the indexed
parent. Returned pending Linux transactions are replayed by both the core and
e2fsprogs. Linux can leave stale primary free-space summaries: the oracle admits
only the exact summary diagnostics confirmed by independently summed groups;
the core result must already have correct primary totals and no such diagnostics.
Large directories use sorted name expectations with binary search. Optional
extent-collapse advice is admissible only when the exact message is already
recorded in a successful nonrepairing check of that verified source image; both
the source and observed advice stay in the report.

`ext4-write-test --truncate` runs resize and freeing cases across the selected
profiles. Truncate exports include the final reused block and three intermediate
sizes retaining different portions of the fragmented mapping tree; independent
byte oracles and e2fsck inspect those states as well. `--export-only --export DIR`
runs the successful export scenarios without repeating the fault loops already
run by Meson. It is an artifact-generation mode, not the complete test suite.

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

`file-write-partial` exercises ten write profiles. The two
`file-write-partial-faults-*` cases use small journals to cross three transactions
while injecting every observed allocation/read failure and every write/flush cut.
They compare recovered state against independently observed checkpoint boundaries;
failed commit-preparation reads require recovery even when no new data was committed.
The normal storage model's torn-superblock rejection remains explicit.

To generate the three completed states for independent checking, run
`ext4-write-partial-test --export DIRECTORY IMAGES...`; export mode omits the
already separate guards and fault matrices. Then use
`tests/check_partial_writes.py --fixtures FIXTURE_DIRECTORY --exports DIRECTORY
--output REPORT_DIRECTORY` with the selected e2fsprogs tools. The checker verifies
complete bytes, single-application attribute changes, unchanged neighbors, namespace
identity, free counts and clean nonrepairing e2fsck. The core CI suite includes both
the focused cases and this independent check. Changing only a fixture workload or
export path does not require repeating unchanged fault matrices during development.

`file-growth-preallocation` checks write and truncate growth through written blocks
beyond EOF, with a sparse gap, partial first/last blocks and retained unlinked inodes.
The requested zeroing exceeds maximum transaction credits. Atomic APIs still reject
without writes; growing APIs preserve the existing visible prefix, skip sparse holes,
apply external xattr REMOVE/CREATE once, and retain hidden bytes after the requested
end. Invalid attribute transitions and timestamps reject before preparation writes.

The two `file-growth-faults-*` tests use small journals with extent and indirect maps.
One write gap fits alone but exceeds capacity with its external xattr; truncate
requires two preparation commits followed by final publication. Every observed
allocation/read failure and write/flush cut is injected, with three survival patterns
and torn writes. Old-size outcomes must retain the exact old inode and visible data;
new-size outcomes must contain the complete zeroed gap and admitted metadata. Both
can resume safely, and torn primary-superblock rejection is reported separately.

`ext4-write-growth-test --export DIRECTORY IMAGES...` exports completed write and
truncate states. `tests/check_growth.py --fixtures FIXTURE_DIRECTORY --exports
DIRECTORY --output REPORT_DIRECTORY` checks their bytes, sparse mappings, retained
hidden tail, xattrs, namespace, free counts and nonrepairing e2fsck. CI runs the same
checker. Generated evidence records distinguish pending, focused and full acceptance.

## Attribute lifetime and feature transitions

`ext4-xattr-lifetime-test` combines raw attribute storage with creation, write,
truncate, rename/removal and held unlinked inodes. The standard run also tests last
reference release for shared blocks, fast/mapped symlinks, directories and a large
file. A second held orphan makes the victim a nonhead list entry. Faults cover every
allocation/read and write/flush boundary of final release; even a pre-write error
must consume the hold once, poison the owner and leave recoverable deletion intent.

`--enable` uses `generate_xattr_enable_fixtures.py` images with no EXT_ATTR feature.
It tests first inode-body/external attributes and compound creation/data/size changes.
Recovery must allow the committed feature enablement together with the attribute,
while repeated attempts to clear EXT_ATTR or change another compatibility feature
must reject without writes. These are registered as `xattr-first-attribute-*` when
the `xattr_enable_fixtures` Meson option is selected.

`check_xattr_lifetime.py` compares inode identities, complete directory names, exact
attribute/data bytes, shared reference counts, feature flags and allocation totals.
It applies core and e2fsck recovery separately to each pending/uncommitted export and
requires clean e2fsck results and unchanged repeated core recovery. Release and first
attribute modes pass both backends. The ordinary linked-truncate mode retains the
known e2fsck attribute-loss failure; `--keep-going` collects later evidence without
changing its failed status or exit code. Linux recovery is independent evidence for
the affected case, not permission to discard that failure.

`run_linux_journal.py --xattrs --recover PATH --xattr-reader PATH` consumes
independently checked image manifests. Run it from the explicit Machlin lab
directory with the pinned Linux module report and a new output directory.
`linux_xattrs.py` derives complete inode, namespace, data and raw attribute
expectations with e2fsprogs. The guest checks exact get/list results, empty/binary
values, long/UTF-8 names, compact-to-userspace ACL conversion and Linux namespace
visibility. It mutates existing attributes, creates ACL/capability metadata and
leaves a committed journal for portable and independent recovery.

The portable `ext4-xattr-test --verify IMAGE EXPECTED` checks these Linux-authored
values under both read-only and writable owners. Its `--roundtrip OUTPUT IMAGE
EXPECTED` mode atomically changes attributes and owner/mode metadata on the
Linux-created file. The harness returns that image to Linux for complete verification
and clean unmount. `--pending` additionally selects a checked committed core log as
the first Linux input. An unsupported reference-kernel block size is rejected
explicitly; it is not counted as a successful Linux case.

## Portable performance acceptance

Functional and crash-test passes do not establish throughput, latency or scaling.
Before accepting the portable core, measure optimized builds without sanitizers
and retain compiler options, fixture geometry, workload size, operation counts,
read/write/flush callbacks, allocation counts, CPU time and latency distributions.
Report cold and warm reads separately. Timings of the fault harness are not
filesystem benchmarks. Core-only measurements and mounted platform measurements
remain separate evidence.

The workloads must include sequential and random reads/writes, small synchronous
updates, large contiguous and fragmented files, sparse growth and truncate,
directory lookup/enumeration and namespace churn, near-full allocation and files
with external/shared attributes. Increase file extent counts, directory entries
and live inode holds to expose scaling costs rather than repeating only small
fixtures. Compare identical completed work and durability guarantees; a fully
checkpointed core operation is not comparable to a buffered write that has not
reached stable storage. Linux comparisons must record journal mode and flush
boundaries as well as caching and thread counts.

The read-path batch adds contiguous read ranges, reusable mapping scratch within
a read and a streamed directory API. Its focused tests and warm-cache benchmarks
pass; complete CI acceptance is pending. The compatibility single-entry API still
reads and validates a block per returned entry, so adapters need to adopt the visitor
to obtain streamed enumeration's benefit.
CRC32C uses an immutable byte-remainder table, or compile-time ARMv8 CRC32C
instructions when the target guarantees them. Attributed writes can still
validate the complete inode map; each transaction journals data and checkpoints
synchronously; writable access is serialized by its owner. Measure these costs
before choosing further optimizations.
Buffer reuse, range mapping and bounded metadata caching must preserve validation
and mutation invalidation. Changes to transaction batching, checkpoint timing or
concurrency require renewed crash, ordering and lifetime acceptance. Native page
cache ownership stays in the adapters. These requirements remain pending until
the generated measurements and representative application workloads are reviewed.

Reader tests compare complete and unaligned ranges with the
independent fixture bytes and inject failures at each observed read/allocation.
Indexed-directory tests stream the independent name expectations, stop and resume
visitors, seek saved cookies and retry after I/O or allocation errors. Small
directories sweep every failure position; large directory runs explicitly sample
the first, middle and last positions. These focused checks provide development
feedback without repeating the entire write/recovery and Linux matrix after each
edit. `file-read-ranges` additionally checks explicit physical-block vectors for
extent/index boundaries, holes and unwritten data, direct through triple-indirect
addressing, EOF preallocation and the logical-block limit. It uses four block sizes
with checksums enabled and disabled. Directory tests reject late malformed records
before any callback, allow read-only queries inside the visitor and refresh the
directory snapshot after create/unlink. The lookup counts below predate this batch.

The read-only probe compares public directory lookup cost on three
independently generated indexed images. It links the optimized freestanding core
without sanitizers, then counts POSIX resource callbacks and environment
allocations for a lookup of an absent name after complete enumeration:

| Directory entries | Directory blocks | Read callbacks before / after indexed lookup | Allocations before / after |
| --- | --- | --- | --- |
| 99 | 33 | 131 / 2 | 231 / 3 |
| 515 | 174 | 688 / 3 | 1,204 / 4 |
| 46,122 | 15,497 | 61,618 / 3 | 107,741 / 4 |

On those same images, looking up the final enumerated entry takes 4, 5 and 5 read
callbacks, respectively, including inode loading. Enumeration at that checkpoint
retains its earlier counts. The input hashes, compiler options, exact work and allocation
balance are recorded in `artifacts/checks/core-read-cost-report.json` and
`artifacts/checks/indexed-lookup-cost-report.json`. This demonstrates the reduced
search work for these inputs; it does not measure physical I/O or throughput.

All three images remain unchanged, with zero writes and balanced allocations.
The probe source is retained alongside the reports. These software operation
counts are not a comparison with Linux.

`ext4-read-benchmark` links the optimized freestanding core without sanitizers,
coverage or LTO even in a sanitized test build. It accepts an image, a regular-file
or directory path and a repeat count:

```sh
.build/ext4-read-benchmark artifacts/fixtures/ext4-4k.img /sparse.bin 31
.build/ext4-read-benchmark artifacts/index-capacity-fixture/index-capacity.img /indexed 31
```

Each run opens the image read-only, warms the workload once, checks stable digests
and balanced allocations, and reports minimum, median and p95 monotonic times,
read callbacks and core allocations as JSON. File requests are at most 1 MiB;
file digest computation is outside the timed read calls. Directory timing includes
the visitor's digest. Building the same tool with `EXT4_BENCH_SINGLE_ENTRY` uses the
compatibility API for comparison with an older core. Preserve the exact compiler,
library, executable and image identities in the generated report.

The 14-pair warm-cache comparison is recorded under
`artifacts/checks/read-path-benchmark-results/`. It does not measure cold reads,
CPU time, device-level operations, writable or concurrent workloads, or mounted
platform behavior. Representative performance acceptance therefore remains open.

`metadata-checksum` compares the selected CRC32C implementation with bit-serial polynomial
division, a known check value, all 256 byte remainders and 3,648 combinations of
seeds, alignment, lengths and streamed partitions through 64 KiB. Empty updates
preserve the seed. Existing reader, malformed-image, journal and mutation cases
exercise its use with independent filesystem bytes.

The subsequent 14-pair checksum comparison under
`artifacts/checks/crc32c-benchmark-results/` uses streamed enumeration and range
reads in both builds. It changes no callback/allocation counts or content digests.
The 46,122-entry warm directory median decreases from 76.088 to 49.252 ms. Six
extent/indirect write exports are byte-identical to the previously independently
verified images, preserving checksum conventions as well as filesystem contents.

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
controls and mixed-ABI races. Checksum v1, async commits and single-user external
journals have focused, independent, Linux and full-regression acceptance. Fast
commit has bounded semantic replay, interrupted-recovery tests and independent
protocol fixtures, with wider compatibility acceptance still open. Native special
inode changes force ordinary-commit fallback in the pinned Linux reference;
their successful recovery must not be reported as native fast-record replay.

Keep CPU sanitizers, freestanding stack checks, unsigned FSKit builds, kext builds,
stock FSKit mounts, custom-kernel execution and LXNU acceptance as distinct rows.
The current evidence and missing rows are recorded in [ACCEPTANCE.md](ACCEPTANCE.md).

Prepared builds and test execution run on Luna; VM preparation belongs to Sol.
The main agent writes tests, investigates failures and reviews evidence. Workers
preserve complete command output in ignored artifacts and never repair a failing
fixture or skip a case to manufacture a passing report.

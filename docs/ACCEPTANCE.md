# Acceptance

The end goal is a working read/write filesystem in stock macOS through FSKit and
in Machlin through a kernel adapter, with the agreed Linux metadata contracts.
Complete and validate the portable core first, then integrate FSKit on stock macOS,
then implement LXNU-specific policy. Adapter development is deferred until the
core's required format, mutation, metadata and recovery contracts are accepted.
Unsigned builds remain the development default while signing is deferred.
The rows below are requirements, not claims of implementation. A checkpoint does
not complete the project. Format support must expand with tested real images;
safe rejection of a feature is recorded separately from supporting it.

| Contract | Required evidence | Current state |
| --- | --- | --- |
| Geometry, feature negotiation, metadata checksums | Real mke2fs images and malformed-input tests under sanitizers | Eleven base read profiles pass; CRC16 group variants pass targeted portable, independent and Linux checks; broader format and size coverage pending |
| Inodes, directories, links, extents, sparse data | Independent contents and metadata comparison | Portable reader and mounted arm64e kext profiles pass; FSKit runtime pending |
| Modern format variations | Explicit feature/size matrix including checksums, 64-bit fields, indexed directories and additional enabled features | Not accepted |
| Create/write/truncate, allocation, rename, unlink | Linux roundtrips, full disks, partial I/O and open-file lifetime | Bounded writes, allocation, growth and truncate/freeing pass independent and Linux checks; live shrink spans transactions; create/mkdir/symlink/link/unlink/rmdir/rename and bounded indexed mutation pass portable, independent and Linux checks; core holds retain open-unlinked or replaced objects; platform writes and broader capacity/concurrency acceptance pending |
| Journal and recovery | Interrupted transactions, ordering faults, device errors, Linux replay and e2fsck | Bounded internal and single-user external journals, legacy lists and modern orphan files pass portable faults, independent recovery, Linux reuse and the full regression; fast commit and platform write integration remain pending |
| Xattrs, permissions and ACLs | Preserve and mutate metadata across macOS/Linux roundtrips | Selective owner/mode/timestamp updates pass portable and Linux checks; raw xattr get/list, atomic attribute batches and mutation lifetime integration pass portable tests and targeted independent checks; bidirectional Linux attribute/ACL/security checks and direct replay of core attribute transactions pass eight profiles; the linked-truncate e2fsck defect remains explicit below; ACL enforcement and platform policy pending |
| Stock macOS FSKit | Actual mount, ordinary application I/O, concurrency, mmap and unmount on an Apple kernel | Read-only adapter builds; installed tests await signing profile |
| Kernel adapter | Actual loaded kext, vnode/UBC behavior, fault/truncate/writeback and resource balance | Loaded arm64e read-only profile passes; writable paths and full resource accounting pending; x86_64 compilation only |
| LXNU policy | CAP_FSETID and privilege removal, xattrs, mixed-ABI races, inherited descriptions and attachment restrictions | Not implemented |
| Compatibility and regression | Shared Linux/LXNU fixtures, native controls and identified stock/custom boots | Not run |
| Distribution | Reproducible standalone build, packaged FSKit extension, documented installation and supported versions | Unsigned app with embedded extension builds; signing and installation pending |

Each accepted row must identify its test command and generated evidence location.
Raw identities, hashes and logs stay in ignored artifacts; source revisions stay
in Git. Generated images never enter source history. Unsupported advanced ext4
features remain visible requirements or explicit scope decisions; they may not be
silently reclassified to declare the project complete.

## Remaining portable-core work

Use this queue instead of an estimated completion percentage. A block closes only
when its behavior and independent acceptance are complete; test count is not a
completion metric. New requirements must be added here explicitly, with their
effect on the remaining scope. FSKit and LXNU work follows this queue and is not
counted as portable-core implementation.

| Block | Concrete remaining work | State |
| --- | --- | --- |
| Ordinary filesystem operations | Retain accepted mutation and allocator-exhaustion behavior as formats expand | Reserved mapping capacity and partial KEEP_SIZE growth pass focused faults, 30 independent states, six Linux roundtrips and the full 412-test regression; earlier ordinary operations retain their accepted evidence below |
| Format compatibility | Quota limit enforcement, key-based encryption and enabling verity await product decisions; unjournaled writes are refused; retain the Linux delayed-allocation maximum-offset exception | Open; MMP, fs-verity, keyless encryption, casefolded directories and quota/project accounting are implemented with their own evidence below, and CI run 36483787970 passed every suite that includes them. 29 further format and geometry variants pass sustained operation with power cuts (see "Format and geometry variant evidence"). BIGALLOC, large logical files and high physical addresses pass the 469-test regression; high addresses also pass three native profiles and 21 independent states; INLINE_DATA and EA_INODE retain full acceptance |
| Journal compatibility | Recovering volumes whose Linux fast-commit replay was interrupted needs a replay-authority decision; until then the core refuses torn bitmap and inode checksums and leaves the log pending | Open; BIGALLOC, casefold, quota and 64 KiB fast-commit profiles, ownership-corruption rejection, interrupted e2fsck replay of ordinary logs and interrupted Linux replay of two native captures are accepted, and the torn states of two range-heavy captures fail closed (see "Fast-commit combinations and interrupted foreign replay evidence" and "Interrupted Linux replay evidence"). Fast-commit conversions may exceed 256 snapshots, and a 1,024-file 1 KiB prefix passes strict independent checks and actual Linux readback. External journals pass focused faults, 30 independent states, four native Linux roundtrips and the expanded 476-test regression. V1 and async compatibility retain their accepted evidence |
| Scale and sustained operation | Indexed operations still classify the whole index tree; first-fit allocation fragments large writes in fragmented free space; decisions on larger live transactions, ordered data writes and group commit | Open; measured workloads show bounded peak memory, about 2.1 times device writes for journaled data and five barriers per operation, and the sustained suite, a second soak and checksum-repairing operations fuzzing pass (see "Scale measurement evidence" and "Sustained operation and fuzzing evidence") |

MMP, fs-verity reading with protected writable metadata, encrypted volumes without
keys, casefolded directories and quota/project accounting are implemented. Quota
limits and grace times are preserved but not enforced; Linux enforces them only with
quota mount options, so enforcement needs an adapter policy decision.
Enabling verity, measurement, built-in signatures and any key-based decryption need
product decisions: the core has no key source and performs no cryptography. They are not accepted merely because mounting rejects
them safely. They must not disappear from a future readiness claim. The core
currently requires one serialized resource owner; native operation locking,
page-cache coordination, ACL authorization and platform lifetime acceptance belong
to the later adapters, with the core's metadata-transition contracts retained.

Already implemented, with evidence below: ordinary reads and sparse mapping;
transactional writes and allocation; restartable truncate and final deletion;
create/link/symlink/mkdir/mknod/unlink/rmdir/rename and atomic whiteout; raw xattr storage and lifetime;
internal-journal recovery and both orphan representations; HTree creation, lookup
and mutation, including LARGEDIR and DIR_NLINK. These are working foundations, not a claim that
the remaining blocks have equal size or that the full core is accepted.

## Scale measurement evidence

`tests/run_scale.py` ran every `ext4-scale` workload on fresh 1 GiB e2fsprogs
volumes with 4 KiB and 1 KiB blocks, alternating two runs each of the parent
revision of this work and of the changed core with the same driver on the
development host; strict fsck accepted every resulting image. Times are means of
the two runs on an in-memory device and exclude device latency; counts are exact.

| Workload | Block | Time (ms) | Device reads | Device writes / barriers |
| --- | --- | --- | --- | --- |
| 256 MiB sequential write | 4 KiB | 181.8 → 123.4 | 142,998 → 12,120 | 140,293 / 3,841, unchanged |
| 1,000 random 64 KiB overwrites | 4 KiB | 45.5 → 8.6 | 91,003 → 7,009 | 38,002 / 5,002, unchanged |
| Truncate 256 MiB to zero | 4 KiB | 7.4 → 0.2 | 14,363 → 175 | 24,582 / 10,242 → 270 / 112 |
| Offline reclamation of a 256 MiB orphan | 4 KiB | 7.5 → 0.3 | 14,443 → 191 | 24,584 / 10,243 → 152 / 63 |
| 64 MiB write into fragmented free space | 4 KiB | 929.5 → 441.4 | 1,880,579 → 15,975 | 35,867 / 961, unchanged |
| 50,000 creates in one directory | 4 KiB | 673.8 → 642.3 | 930,377 → 702,570 | 802,669 / 250,001 → 802,489 / 250,001 |
| 256 MiB sequential write | 1 KiB | 503.3 → 435.5 | 1,319,333 → 566,610 | 557,717 / 11,521, unchanged |
| 1,000 random 64 KiB overwrites | 1 KiB | 138.5 → 41.1 | 586,018 → 315,943 | 134,002 / 5,002, unchanged |
| Truncate 256 MiB to zero | 1 KiB | 14.8 → 0.6 | 77,051 → 319 | 111,386 / 40,962 → 396 / 132 |
| 64 MiB write into fragmented free space | 1 KiB | 620.7 → 479.8 | 2,081,666 → 199,206 | 140,929 / 2,881, unchanged |
| 50,000 creates in one directory | 1 KiB | 597.8 → 557.9 | 3,728,195 → 1,339,971 | 813,101 / 250,001 → 812,987 / 250,001 |

Directory creation with 4 KiB blocks now reads 14 blocks per create at every size
up to 50,000 entries, down from 16 to 21. With 1 KiB blocks it reads 16 to 36 per
create from the first to the fifth 10,000 entries, down from 27 to 120; the
remaining growth is the whole-tree index classification. A lookup reads four blocks
with 4 KiB blocks and five with 1 KiB blocks at 50,000 entries.

Peak live core allocation stays at 1.16 MiB with 4 KiB blocks and 372 KiB with
1 KiB blocks for 256 MiB files, the bound of a 256-snapshot transaction, and at
156 KiB for the directory workloads. Journaled file data costs 2.09 to 2.38 times
the requested bytes in device writes. Every namespace operation commits on its own
with five barriers, which a real device's flush latency would dominate. First-fit
allocation fills single-block holes: the 64 MiB write into fragmented free space
produces 4,067 extents with 4 KiB blocks and 2,617 with 1 KiB blocks.

The complete 627-test regression passes (`artifacts/checks/scale-regression-11/`).
Its predecessor, `artifacts/checks/scale-regression-10/`, preserves the failures
that exposed two defects in the first version of this work: a map record changed
behind a writable mount skipped revalidation, and a history-dependent reclamation
step made recovery after a power cut differ from the uninterrupted image.
Measurements are in `artifacts/checks/scale-measurements-2/`.

## Format and geometry variant evidence

`tests/generate_format_fixtures.py` authors one empty volume per optional feature or
geometry that the other writable fixtures do not already cover, each changing the
base feature set in one respect: 64 KiB blocks, META_BG with 4 KiB and 1 KiB blocks,
SPARSE_SUPER2 with and without backup superblocks, no SPARSE_SUPER, no FLEX_BG, no
64BIT, no HUGE_FILE, no DIR_NLINK, no EXTRA_ISIZE, no FILETYPE, no DIR_INDEX, no
EXT_ATTR, GDT_CSUM instead of METADATA_CSUM, 512- and 1,024-byte inodes, 64 inodes,
1,024-block groups, LARGEDIR, STABLE_INODES, the orphan file, fast commits,
INLINE_DATA, EA_INODE, BIGALLOC with 64 KiB clusters, encryption, casefolding and
verity. Strict fsck accepts every image, and 1,500 sustained operations with power
cuts pass on each; the complete sustained suite passes all 44 cases
(`artifacts/checks/sustained-format-first/`). The first run exposed a model error
in the sustained test, not in the core: the raw attribute list also reports an
inline-data inode's internal `system.data` key, as its contract states. A volume
without a journal and a revision-0 ext2 volume are read correctly and refuse
writable mounting. Multi-mount protection needs the adapter's sleep operation and
retains its own format tests.

## Sustained operation and fuzzing evidence

`ext4-sustained-test` drives deterministic mixed operations against a model and
repeats about one in six eligible atomic operations with a power cut. The CI
`sustained` suite runs every writable base profile and two wide profiles; CI run
36450344817 passed all twelve cases and its five exported images passed strict
e2fsck and debugfs comparison. Locally, 200 additional seeds of 5,000 operations each
passed under ASan/UBSan: one million operations, 55,968 recoveries after a power cut
(25,555 from a durable commit), 950 torn-superblock rejections, 112,232
allocation-exhaustion rejections and directories of up to 741 names. Evidence is in
`artifacts/checks/sustained-soak-1/`. After the allocation and reclamation changes,
120 further runs of 5,000 operations on 4 KiB, 1 KiB and wide 4 KiB profiles passed:
600,000 operations, 33,883 power cuts (15,337 after a durable commit) and 506
torn-superblock rejections (`artifacts/checks/sustained-soak-2/`). These are
sequential core sequences; they do not exercise platform concurrency.

`ext4-operations-fuzzer` mutates nonzero blocks through a copy-on-write overlay and
then runs recovery, a read-only walk and writable operations. Four 25-minute
campaigns without checksums, with an indirect map, with a pending ordinary journal
and with a pending fast-commit prefix executed about 950,000 inputs without a finding.
Corpora and logs are under `artifacts/fuzz-operations/`.

Checksum-repairing journal mutation then ran four further 25-minute campaigns: the
1 KiB fast-commit prefix without metadata checksums, continuing its corpus, pending
4 KiB and indirect 4 KiB fast-commit prefixes, and an ordinary v3-checksum log with
revokes. About 612,000 inputs found nothing. The fast-commit corpus grew from 8,666
edges and 32,515 features to 10,295 and 41,976. This remains bounded coverage.

## Multi-mount protection evidence

MMP acquisition, refresh, release and offline-recovery ownership pass the `format`
suite on four e2fsprogs profiles, including a 40-second interval. Independently,
released images pass strict fsck with a clean sequence and the core's node name and
check interval, e2fsck acquires a released 4 KiB image read-write, and the POSIX
recovery utility waits, replays and releases each pending image. The complete local
583-test regression with MMP passes (`artifacts/checks/scale-regression-3/`). In the
reference VM, Linux 6.12 acquires the released 4 KiB, 1 KiB and unchecksummed images
read-write, writes a file and releases them with the clean sequence and its node
name; the core then acquires each with the POSIX adapter's real waits, finds the Linux
file, adds its own and releases it; Linux finds both files and releases again; and
Linux takes over each pending image, replays its journal and releases it. Strict fsck
passes after every step (lab `artifacts/ext4-mmp/exchange/`). The guest clock does not
advance across the kernel's MMP sleeps, so this does not measure Linux's wait.

## fs-verity evidence

Independently authored fs-verity files at 4 KiB SHA-256, salted 1 KiB SHA-256 and salted
4 KiB SHA-512 verify through `ext4_read`, including every allocation and read failure
on the largest file. Damaged data and tree blocks fail exactly at the covered block,
and invalid root hashes, versions and descriptor sizes reject. Linux 6.12 in the
reference VM reads the same images: FS_IOC_MEASURE_VERITY matches every independently
computed digest, valid files read completely and damaged files return EIO or fail to
open. After a writable core owner changes permissions, adds an attribute, links,
renames and unlinks names and deletes another verity file, the images pass strict
fsck, and Linux still verifies the renamed file with its original digest. Evidence is
in `artifacts/verity-fixtures/`, `artifacts/checks/verity-first/` and the lab's
`artifacts/ext4-verity/readback/` and `artifacts/ext4-verity/mutated-readback/`.

## Encryption without keys evidence

The pinned Linux 6.12 kernel creates a v2 fscrypt tree on a 4 KiB ENCRYPT volume.
The core denies encrypted names, contents, symlink targets, data changes and context
changes, then renames plain names, moves the encrypted directory object and back,
removes an empty encrypted directory and changes an encrypted file's permissions.
The result passes strict fsck, and Linux with the original key reads all 24
encrypted files byte for byte and the encrypted symlink target. Synthetic encrypted
trees on the 4 KiB and 1 KiB base images exercise the same core rules in the `format`
suite. Evidence is in `artifacts/checks/encrypt-first/` and the lab's
`artifacts/ext4-encrypt/created/` and `artifacts/ext4-encrypt/verified/`. Keys and
decryption remain outside the core.

## Casefolded directories evidence

The core's utf8-12.1 casefold matches e2fsprogs 1.47.3, which follows Linux's
utf8data, for every code point, ten malformed forms and 200,000 random combining
sequences: 1,314,091 vectors, 2,058 of them opaque, plus 19,998 casefolded hashes
across all six hash versions. On a 4 KiB strict and a 1 KiB relaxed volume authored
and indexed by e2fsprogs, every equivalent lookup resolves to the stored inode in the
indexed and the linear directory, and absent names stay absent. A writable owner
then rejects folded duplicates, applies the strict and relaxed malformed-name rules,
adds 600 names through indexed leaf splits, treats a case-only rename as an alias,
creates an inheriting subdirectory and unlinks and renames through equivalent names.
The changed images pass strict e2fsck, which recomputes every casefolded hash in the
index, and debugfs finds the expected names and flags. The local ASan/UBSan
regression with casefold passed 590 of 591 tests. The failure was the hash guard
test, which still required empty input to be rejected although a name of ignorable
code points folds to nothing and Linux hashes that empty form; the corrected test
passes. Evidence is in `artifacts/checks/casefold-first/` and
`artifacts/checks/scale-regression-6/`. The Linux reference kernel lacks
`CONFIG_UNICODE`, so these images have no Linux readback.

The oracle now also folds and hashes 4,000 near-maximal names of expanding code
points; 1,318,086 folds and 23,994 hashes match e2fsprogs, including 3,995 hashes of
folds longer than 255 bytes and 41 of names that fold to nothing. The writable test
additionally enables casefolding on a new directory only once it is empty and treats
names of ignorable code points as one empty name; strict e2fsck and debugfs accept
both exported images. The complete 591-test regression passes
(`artifacts/checks/casefold-toggle/`, `artifacts/checks/scale-regression-7/`).

## Quota and project accounting evidence

On e2fsprogs-authored 4 KiB, 1 KiB EA_INODE and 16 KiB-cluster BIGALLOC volumes, the
writable core keeps user, group and project quota files exact through creation of 40
new owners and three IDs that diverge at every index level, ownership changes,
truncation, an external attribute block, a value inode, held and final removal,
replacement by rename, project changes and PROJINHERIT inheritance, cross-project
link and rename refusal and offline reclamation of a held orphan. A reader in the
test and strict e2fsck, which recomputes all usage, agree; a patched entry makes
strict e2fsck exit 4. Every allocation and read failure and 1,452 power cuts during
operations that grow the quota trees recover to the unchanged or committed image,
whose committed forms also pass strict fsck. Twelve 2,000-operation sustained runs
with 1,753 power cuts end with exact usage. In the reference VM, Linux 6.12 with its
matching quota modules reads usage equal to e2fsprogs from every core image; after
Linux removes, creates and moves files, which leaves freed quota blocks and entries,
strict fsck and e2fsprogs agree with Linux; the core then reuses those entries and
blocks, and Linux and strict fsck agree again. The complete 600-test regression
passes. Evidence is in `artifacts/checks/quota-first/`,
`artifacts/checks/scale-regression-8/` and the lab's `artifacts/ext4-quota/exchange/`.

## Fast-commit combinations and interrupted foreign replay evidence

Independent fast-commit fixtures now cover a 4 KiB BIGALLOC volume with 16 KiB
clusters, a casefolded directory, a quota volume and 1,024 long names in a casefolded
directory. The core replays each exactly, including exhaustive allocation and read
failures and 168 to 204 interrupted recoveries per small profile; strict e2fsck
accepts every replayed image, including the replay-time quota usage and the
casefolded index. Two ownership-corruption logs per special and indirect profile, a
range claiming an inode-table block and one claiming a block of `lost+found`, reject
without writes; before this change the second replayed and would have cross-linked
the block. The fast-commit suite passes all 113 cases.

A 64 KiB profile needs a 64 MiB journal, the 1,024-block minimum of mke2fs, on a
2,176-block volume; a smaller volume silently received no journal. The core replays
its three commits exactly, through 156 interrupted recoveries and every allocation
and read failure, and strict e2fsck and independent namespace and data reads accept
the result (`artifacts/checks/fast-commit-64k-independent/`). The pinned Linux
reference uses 4 KiB pages, so this profile has portable and independent evidence
only. The fast-commit suite now passes all 117 cases in the 627-test regression.

e2fsck, whose jbd2 recovery code is a copy of the kernel's, replayed six
debugfs-authored logs and fifteen logs Linux left pending in the lab's cluster,
inline and attribute runs while a preload library recorded its 1,094 writes and
93 flushes. Under page-cache crash semantics, all 828 distinct states in which the log
still held transactions recover under the core to exactly the contents of its own
recovery of the untouched log, and strict fsck passes. Of 192 states after e2fsck
had emptied the log, during its unjournaled orphan release and superblock updates,
56 recover cleanly, the core refuses 119 and 17 remain for fsck, as mount-time
recovery does not audit every bitmap; every complete state recovers cleanly. Evidence
is in `artifacts/checks/interrupted-replay-first/` and
`artifacts/checks/scale-regression-9/`. CI run 36483787970 repeated the six
debugfs-authored cases with `LD_PRELOAD` on Linux.

e2fsck cannot yet serve as the foreign replayer of fast-commit logs: on the 4 KiB
profile its replay stops with "No free space in the directory" in
`ext4_fc_handle_link_and_create`, because it does not grow a directory while
adding names, and it leaves the volume marked with errors. The remaining oracle is
Linux's own replay, recorded below the filesystem, for example with the
`dm-log-writes` target that the reference kernel's module set provides.

## Interrupted Linux replay evidence

Quota accounting had removed the fast-commit replay's free-block baseline together
with the post-commit publications it replaced. Linux leaves the superblock's free
counters behind its group descriptors, so from that change the core refused Linux's
own pending fast-commit volumes as corrupt; bisection identified the change. With
the baseline restored, all nine Linux-captured pending volumes whose recovered
digests were recorded at their acceptance recover to exactly those images again.
Every fixture profile now also carries a pending image with stale superblock
counters, and four profiles recover it in the fast-commit suite; the complete
631-test regression passes (`artifacts/checks/scale-regression-12/`).

The reference kernel replays pending volumes directly through the same probe. Of the
native captures, those without the orphan file replay in Linux; every capture made
with the orphan file and a held open-unlinked file fails in Linux itself with
"JBD2: journal recovery failed" and EUCLEAN, while the core recovers all of them.
Linux replays 15 of the independently authored fixture profiles. The casefold
profiles need Unicode support that the reference kernel lacks, the quota profile
needs quota modules that the probe does not load, and Linux's replay of the two
indirect-map logs did not finish within 120 seconds; their partially written disks
are kept. Linux replays a logged directory creation with one link and without the
parent's link, so strict fsck reports link and directory counts in its result for
these fixtures. The fixtures now log each regular file's ranges before its final
record, as Linux does; with that order Linux's replay also derives their block
counts. Linux never logs a directory or reserved inode outside a creation, and
replaying a logged root inode fails in Linux.

A dm-log-writes target recorded Linux's complete replay of four native captures;
each complete log reproduces Linux's result exactly, and that result reads like the
core's recovery of the untouched volume. For a capture whose commit Linux had
converted to an ordinary transaction (31 writes, 7 flushes, 6 FUA writes), all 122
states while the log was pending recover under the core to the untouched result
and strict fsck passes; so do all 68 states of a small fast-commit capture (48
writes, 13 flushes, 4 FUA writes). Every state after Linux emptied the log is clean.
The two captures with 53 range-heavy fast commits (363 writes, 74 flushes and 4 FUA
writes each) recover in 9 of 285 pending states with 4 KiB blocks and 111 of 301
with 1 KiB blocks. In the others Linux's non-atomic replay has left a bitmap whose
checksum in the group descriptor is stale or an inode record with an invalid
checksum, and the core refuses the volume before any fast-commit write. The log
stays pending: Linux replays such a volume again, and in one sampled case its own
result still needs fsck for the group's unused and free inode counts. Recovering
these states in the core needs the replay-authority decision described in
`docs/ARCHITECTURE.md`. Evidence is in the lab's `artifacts/ext4-log-writes/`.

## Fast-commit development evidence

Fast-commit recovery is a development checkpoint, not completed journal
compatibility. The decoder checks record bounds, expected transaction ID, committed
CRC prefixes and reread identity. Semantic replay stages inode metadata, exact
mapping ranges and namespace changes in one ordinary conversion transaction. The
original fast prefix remains authoritative until its replacement commit is durable.
Recovery reports fast commits separately from ordinary transactions.

All 76 focused sanitized fast-commit cases and thirteen independent output profiles pass.
The affected regression also passes eight ordinary indirect cases, eight ordinary
journal cases, malformed-image checks and orphan recovery: 94 cases in total.
The complete freestanding build and source-format checks also pass. The bounded
32 MiB protocol fixtures are authored with e2fsprogs, without linking this core;
expected filesystems pass strict nonrepairing fsck before use as references.

| Profiles | Contracts exercised |
| --- | --- |
| 1/4 KiB, explicit checksum seed, absent metadata checksums | Three committed prefixes, holes, exact allocation, directory growth, long names, hard links and rename |
| Modern orphan file at 1/4 KiB | Generation reuse, final unlink, checkpointed unlinked allocation, linked truncate and a simultaneous legacy orphan |
| Special inodes at 1/4 KiB | Symlink targets of 7/59/60 bytes, legacy/extended/zero device identities, FIFO and socket |
| Attribute reuse at 1/4 KiB plus legacy orphan chain at 1 KiB | Two reused inode numbers sharing a 64 KiB value and an external attribute block with a surviving third owner; two private values reclaimed |
| Indirect maps at 1/4 KiB | Sparse direct/single/double boundaries, new indirect allocation, old triple-indirect reclamation during generation reuse, legacy orphans and mapped symlinks |

Comparisons require exact namespace, file bytes, inode metadata, link targets,
device identities and attribute values. Free-inode counts must match; any free-block
difference must be charged exactly to the reconstructed directory/index layout.
All thirteen core-recovered outputs pass strict nonrepairing e2fsck and independent
namespace/value hashes. A second clean recovery leaves the image unchanged.
Special objects are inspected inside the image, never materialized on the host.

Malformed tests reject record lengths, CRCs, stale IDs and changed reread data.
Twelve orphan states reject duplicate slots, overlap with the legacy chain,
reserved/out-of-range numbers and damaged tails before conversion writes. Twenty
valid-CRC special-inode logs reject bad link size, embedded NUL, missing terminator,
nonzero special-inode size and missing mapped-link data without home or journal
writes. Resource failures and modeled partial writes/cache loss retain durable
recovery authority through conversion and subsequent orphan cleanup.

Attribute-generation replacement releases all old references in the same private
transaction. Shared external-block snapshots observe earlier detachments in that
transaction; the two reuses reduce the shared block and body-value references from
three to one. Both new generations have no attributes, and the survivor retains
six exact values. Legacy orphan linkage is removed while the old inode checksum is
still valid. Per-inode attribute charges cannot leak into the next namespace record.
The three attribute profiles pass 2,895 allocation failures, 2,282 read failures
and 2,724 interrupted recoveries across 454 write/barrier cut points. Thirty-nine
deliberately torn primary superblocks reject for offline repair; all other
interrupted states converge to the expected filesystem.

Attribute-reuse evidence is under
`artifacts/checks/fast-commit-xattr-multiple-fixed/` and
`artifacts/fast-commit-xattr-multiple-independent/`. The reproducible stale-reference
failure is preserved under `artifacts/checks/fast-commit-xattr-multiple/`.
Expected-image construction detaches shared external blocks with the public
whole-block e2fsprogs operation: pinned debugfs per-key removal freed surviving
value inodes and failed strict fsck. That failed image was never used as an oracle.

The completed 552-test GitHub CI regression passes all six jobs with no test
failures, errors or skips, including independent checks and artifact upload. It
covers seventy-six fast-commit cases including modern-orphan, special-inode,
attribute-reuse and indirect replay.
The existing declared reader-only xattr fsck exceptions and fixture-normalization
steps remain separate from unexpected command failures. CI identities and complete
artifact inventories are in the generated `artifacts/checks/fast-commit-ci-*/` reports.

Native Linux captures at 1 and 4 KiB provide separate evidence. The basic captures
contain 37 fast commits and match native direct recovery in namespace, data,
metadata and allocation totals. Expanded captures contain 53 fast commits and add
inode reuse, new directories, long names, fragmented extents, holes, preallocation
and partial unwritten initialization. Core recovery followed by Linux verification
passes both expanded profiles. The expanded 1 KiB image fails direct recovery in
the pinned Linux reference while loading its journal; e2fsck also reports a directory
recovery error and repairs its separate diagnostic copy. That copy is not an accepted
oracle, and native direct replay of this pending log remains unresolved. The preserved
failure is in the lab's `artifacts/ext4-journal/fast-commit-native-expanded/`.

Native orphan and special-file acceptance requires the actual replay path. The
latest batch passes eight actual boots and twelve strict fsck checks. Normal 1/4 KiB
captures report 53 replayed fast commits and no ineligible commit. Special-file
captures require one native `Data journalling` fallback and recover two ordinary
transactions with zero fast commits; Linux verifies three symlinks and six special
nodes. Both paths reclaim the checkpointed open-unlinked inode through one modern
orphan-slot transfer. An earlier capture's aggregate pass with zero fast replay
is retained as ordinary-journal evidence, never as native special-inode fast replay.
Evidence is in `artifacts/checks/fast-commit-special-native-path-final/` and the lab's
`artifacts/ext4-journal/fast-commit-native-path-final/` and
`artifacts/ext4-journal/fast-commit-special-fallback-final/`.

Linux consumption of the three attribute-reuse protocol fixtures also passes.
Each image is recovered by the frozen core executable with three fast commits,
then mounted in a separate actual Linux boot. Linux reads every byte of the six
surviving values, verifies both replacement files have no old attributes, and
cleanly unmounts. All 215 commands and six strict fsck checks pass; subsequent
independent reads retain the exact namespace, data and attributes. Protected
fixtures and the core executable remain unchanged. Evidence is in
`artifacts/checks/fast-commit-xattr-native-readback/` and the lab's
`artifacts/ext4-journal/fast-commit-xattr-readback/`. This is protocol replay followed
by Linux consumption, not proof that the Linux writer emits this combination or
that Linux directly replays the pending protocol image.

Both indirect profiles pass 1,451 allocation failures, 1,044 read failures and 1,644
interrupted recoveries over 274 write/barrier cut points. Eighteen torn primary
superblocks reject for offline repair. Four additional valid-CRC logs reject
unwritten indirect data and logical addresses outside the format; ten damaged
journal maps reject holes, duplicate data, data/node overlap, reused nodes and
pointer cycles without writes. Mapped symlink targets are read and validated before
the private conversion commits. The initial combined fixture incorrectly reused a
deleted regular inode for a device with unchanged generation; the core rejected it,
and expected-image construction now allocates special inodes before releasing those
numbers.

The first indirect resource runs exceeded their 600-second case limits. Journal
freezing had reopened a mapping path for every journal data block. It now consumes
the existing validated contiguous mapping runs and retains path/overlap checks;
indirect mutation lookup also coalesces adjacent empty pointers. On the unchanged
1 KiB fixture, successful recovery drops from 9,458 allocations and 17,141 read
callbacks to 795 and 592, with the same 148 durability events. The 4 KiB profile
uses 656 allocations, 452 reads and 126 durability events. Exhaustive resource
tests now finish in 47 and 80 seconds under the original limits. These are fixture
operation counts and test durations, not a general filesystem throughput benchmark.
All 838 independent-check commands pass, and clean second recovery leaves every
image unchanged. Current evidence is in `artifacts/checks/fast-commit-indirect-accepted/`
and `artifacts/fast-commit-indirect-independent/`; initial timeouts and the deliberately
stopped intermediate run remain under `artifacts/checks/fast-commit-indirect-final/`.
Both indirect outputs also pass actual Linux readback with ordered mounts. Each
reports three core fast commits and four legacy-orphan cleanup transactions before
Linux verifies sparse data, created files, hardlink identity, replacement/truncated
inodes, symlinks and special devices. All 162 commands and four strict fsck checks
pass; subsequent independent reads preserve the namespace and contents. Source
fixtures and the frozen recovery executable remain unchanged. Evidence is in
`artifacts/checks/fast-commit-indirect-native-readback/` and the lab's
`artifacts/ext4-journal/fast-commit-indirect-readback/`. This establishes Linux
consumption of core output, not Linux generation or direct fast replay of the
pending indirect protocol images.

The recovery-scaling checkpoint adds two larger protocol profiles: 256 created
files at 1 KiB and 1,024 at 4 KiB, reversed inode/range revisits, repeated overlapping
ranges and duplicate name records. Conflicting name owners reject without writes.
All 129 affected sanitized cases pass, including 86 fast-commit cases, the range
index unit test, allocator exhaustion/corruption, BIGALLOC and distributed group
geometry. Fixture generation passes all fifteen profiles; the 137-target build
includes freestanding compilation and the 2 KiB stack-frame check.

The first two large-prefix runs retained 102/103 and 104/105 passes with the same
4 KiB sampled-fault case timing out at 600 seconds. Replay now combines FIND and
INSERT preparation, and bitmap validation visits protected range intersections
and counts bits by byte rather than searching for every cluster. Complete directory
and bitmap validation remain in place. With those changes the same fault case
passes in 431.76 seconds under its original bound. This is a sanitized fault-test
duration, not a throughput benchmark. The accepted local evidence is
`artifacts/checks/fast-commit-bitmap/results.json`; both failed attempts remain in
`artifacts/checks/fast-commit-large-prefix-accepted/` and
`artifacts/checks/fast-commit-large-prefix-fixed/`.

The frozen checkpoint executable then passed independent verification of all
fifteen recovered outputs: 4,710 commands, strict nonrepairing e2fsck, exact
namespace/data/attribute reads and an unchanged second clean recovery. Evidence is
in `artifacts/fast-commit-large-prefix-independent/`. Two actual Linux 6.12 boots
read every created file of both large-prefix profiles, the sparse source and the
hardlink identity; all 3,876 commands and four strict fsck checks pass. Evidence is
in `artifacts/checks/fast-commit-large-prefix-native-prep/readiness-recheck.json` and
the lab's `artifacts/ext4-journal/fast-commit-large-prefix-readback/`. This is Linux
consumption of core output, not native generation or direct Linux replay.

Release builds without sanitizers compare the checkpoint with its parent using the
same current driver and final fixtures: one checked warmup and 31 measured recoveries
from the memory-backed device, alternating version order by profile. Medians fall
from 2.12 to 0.94 ms at 1 KiB, 3.97 to 1.85 ms for indirect 1 KiB, 113.6 to 51.7 ms
for large-prefix 1 KiB and 1,413.5 to 704.7 ms for large-prefix 4 KiB. Reads drop,
for example from 24,461 to 11,352 at 4 KiB; peak core allocation and durability
events are unchanged. Recovered images from separate copies are byte-identical.
Evidence is in `artifacts/checks/fast-commit-release-benchmark/`.

The push-triggered CI for that checkpoint (run 36439569809) passed five jobs and 288
of 289 core tests. `fast-commit-sampled-faults-large-prefix-4k` timed out at its
unchanged 600-second limit on the hosted runner; its reports are preserved under
`artifacts/checks/fast-commit-ci-36439569809/`. A release profile showed name replay
dominated by full validation of every indexed leaf for each record, then bitmap
checksums and a bit-serial inode-bitmap scan. Indexed namespace operations now
validate the map, the complete index graph, the root and only the leaves eligible
for the name's hash. Inode bitmaps use byte-wise checks, and targets guaranteeing
ARMv8 CRC32 use its CRC32C instructions. With the same fixtures and driver, release
medians fall from 1.03 to 0.19 ms at 1 KiB, 2.00 to 0.50 ms for indirect 1 KiB, 56.5 to
3.6 ms for large-prefix 1 KiB and 765.2 to 44.6 ms for large-prefix 4 KiB. Reads, peak
core allocation and durability events are unchanged, and every recovered image is
byte-identical to the checkpoint's. This host was not idle during that comparison
(load average 3.8–7.6), so the medians carry more noise than the ratios suggest;
evidence is in `artifacts/checks/fast-commit-release-benchmark-scaled/`. The sanitized
sampled 4 KiB fault case takes 99 seconds locally, and the complete sanitized 563-test
local regression passes (`artifacts/checks/scale-regression-1/`). These are memory-backed
recovery measurements, not mounted or device throughput.

The push-triggered CI for that fix and the sustained suite (run 36450344817) passed
all seven jobs: 575 unique tests with no failures, errors or skips, including the
563-test inventory expected at handoff, every independent step and artifact upload.
The sampled 4 KiB large-prefix fault case took 196 seconds on the hosted runner.
Reports are under `artifacts/checks/ci-36450344817/`.

Fast-commit conversions may now use a recovery transaction bound derived from half
of the ordinary ring and 32 MiB of snapshot buffers. The huge-prefix profile creates
1,024 long-name files at 1 KiB; the handoff executable rejects it as unsupported
because its conversion exceeds 256 snapshots. The new core replays it, passes
sampled fault and resource tests and strict independent verification, and an actual
Linux 6.12 boot reads all 1,024 files with two strict fsck checks and 3,089 passing
commands. Evidence is in `artifacts/fast-commit-huge-independent/`,
`artifacts/checks/recovery-capacity-frozen/` and the lab's
`artifacts/ext4-journal/fast-commit-huge-prefix-readback/`.

Open work includes larger transaction/prefix capacity, additional format combinations,
broader ownership-corruption coverage and interrupted foreign replay. Native fixture
capture remains separate from ordinary CI. The writer emits ordinary full commits.

## External journals

Writable mount and explicit recovery accept a separately owned journal resource.
Device and JBD2 superblocks, the filesystem's journal UUID and the single registered
filesystem UUID must agree. The core validates geometry and checksums before any
write, keeps external ring addresses separate from home block addresses, and
orders persistence across both devices. Shared multi-user journals remain
unsupported. Native adapters do not yet acquire or expose the second resource.

Five independently generated profiles cover legacy, checksum v1, v2 and v3 journals
with 1, 4 and 64 KiB blocks. Seven sanitized cases pass: five functional and
association-validation cases plus two exhaustive fault cases. They reject 80
malformed associations without writes, enumerate 32 mount read failures and 47
mount allocation failures, and preserve the external device's control prefix.
The fault cases interrupt every transaction write/flush with independent none,
all and alternating survival on each device, including partial writes: 936 cuts.
Another 396 cuts interrupt recovery itself, then retry it; 63 recovery read failures
and 21 allocation failures also retry successfully. The 18 transaction cuts and 12
recovery cuts that tear a checksummed primary superblock fail closed with an
actually invalid checksum. They are repair conditions, not successful recovery.

Independent verification accepts six states per profile: original and completed
filesystems, both core replay outcomes and both e2fsprogs replay outcomes. All 165
commands and 30 strict nonrepairing fsck checks pass. File data, permissions,
timestamps, external attribute bytes and allocation accounting agree, and clean
recovery changes neither device. The independent oracle performs journal-only
replay without forcing a full check; no filesystem repair pass is permitted.

Four profiles also pass four actual Linux boots each using two virtio devices and
`data=ordered,journal_path=/dev/vdb`, with ordinary delayed allocation. Linux mutates
the core-created file and leaves a committed journal plus an open-unlinked inode.
Core and native Linux recovery agree on exact contents, attributes, metadata and
allocation totals; all four orphans are reclaimed. Linux also replays the core's
pending journal directly and verifies a later core mutation of the Linux-authored
file. All 154 commands, 24 nonrepairing fsck checks and 16 identified guest boots
pass. The 64 KiB profile has portable/independent evidence only because the pinned
Linux reference uses 4 KiB pages. The first native attempt exposed a fixture
minimum-size error: Linux requires 1,024 usable ring blocks in addition to the
device control prefix. Correcting the fixture required no core change.

Evidence is in `artifacts/checks/external-journal-focused-ring/`,
`artifacts/external-journal-independent-ring/` and the lab's
`artifacts/ext4-journal/external-journal-native-retry2/`. The main review is
`artifacts/checks/external-journal-accepted.json`. The expanded full regression
passes all six jobs and all 476 registered tests, with no Meson failures or skips.
All applicable independent stages, artifact uploads and job cleanup succeed.
The 291 report files contain no unexpected failures; the ten existing shared-value
reader-only fsck limitations and ten fixture refcount normalization commands remain
explicitly classified. The new external-journal CI checks independently repeat all
30 states, 165 commands and 30 strict nonrepairing fsck checks, including exact
state agreement between core recovery and journal-only oracle replay. Raw evidence
is in `artifacts/checks/external-journal-ci/`; the main review is
`artifacts/checks/external-journal-ci-accepted.json`.

## Large physical addresses and volume geometry

Three independently authored sparse volumes cover 1 KiB blocks above the 32-bit
physical-block boundary, distributed descriptors with an explicit checksum seed,
and 4 KiB blocks near that boundary. The logical image sizes are approximately
4 TiB and 16 TiB, but each original occupies less than 18 MB on the test host.
Their last group contains real inode tables, a directory, data extents, an external
mapping node and an external xattr block. Independent debugfs inspection verifies
the physical addresses; merely placing an inode in the last group is insufficient.

mke2fs leaves these empty journals using 32-bit tags. On a large writable volume,
the core selects wide tags in memory and persists that feature together with the
new log start/sequence before writing any descriptor. Read-only access and canceled
private transactions do not change the format. The existing publication barrier
orders the change; no additional per-transaction flush is introduced. Recovery
continues to decode the actual recorded format before opening a new writer.

The sanitized program and optimized freestanding core compile successfully. All
three profiles pass seven independently checked states each: original reads,
high-address creation/write/preallocation/punch/xattr mutation, complete held-unlink
reclamation, and core/oracle recovery of both uncommitted and committed journals.
All 258 commands and 21 strict nonrepairing fsck checks pass. Exact mappings, data
and zero padding, inode/cluster charges, free totals, unchanged seeds and replay
idempotence agree. This is selected large-volume geometry evidence, not a sustained
throughput result or coverage of every group layout.

The expanded CI run passes all 469 registered tests across six suites, with no
Meson failures or skips. All applicable independent checks finish successfully;
289 report files contain no unexpected failures. The existing ten shared-value
reader-only fsck exceptions and ten fixture refcount normalization commands retain
their separate classifications. The namespace job reaches its 45-minute timeout
after its final check and artifact upload succeed, so that workflow is cancelled,
not a successful whole run. The limit is now 60 minutes. A targeted namespace rerun
passes all 63 tests, all six independent stages, artifact upload and job cleanup.
The other five suites retain their successful full-run evidence. Main reviews are
`artifacts/checks/large-volume-ci-accepted.json` and
`artifacts/checks/namespace-timeout-ci-accepted.json`.

All three profiles also pass four native Linux boots each with ordinary ordered
mounts and delayed allocation enabled. Linux reads and overwrites high-address
core data, creates a file and leaves an open-unlinked inode. Core recovery agrees
exactly with native Linux recovery, including orphan reclamation. The core then
shrinks and regrows the Linux-authored file and releases its own held file; Linux
verifies the returned state. A separate boot directly replays the core's committed
high-address journal. All 282 recorded commands and 18 strict nonrepairing fsck
checks return zero, covering 12 Linux transactions and three orphans.

Linux may place a new file's data and xattr in lower groups despite its high inode
number. The native checker permits that allocator policy while retaining exact
bytes, mappings, charges, high inode-table locations and the existing high-address
data/extent-node/xattr checks.

Native evidence is `lab/artifacts/ext4-journal/large-volume-native-retry2/report.json`
and main review is `artifacts/checks/large-volume-linux-accepted.json`. This adds
selected large-volume interoperability, not sustained-load or adapter acceptance.

Reports: `artifacts/large-volume-fixtures-high/report.json`,
`artifacts/large-volume-independent/report.json` and
`artifacts/checks/large-volume-focused-accepted.json`. `generate_large_volume_fixtures.py`
authors the images and `check_large_volumes.py` runs the operation/recovery batch.
The `large_volume_fixtures` Meson option registers the read profiles. CI additionally
runs the independent mutation and replay batch. Sparse copying and content identities
enumerate stored ranges with SEEK_DATA/SEEK_HOLE; they reject unsupported enumeration
or more than 2 GiB of stored ranges instead of scanning a multi-terabyte hole.

## Large logical files

Nine independently authored profiles cover 1/4/64 KiB blocks, indirect mapping,
INLINE_DATA, BIGALLOC, their combination and volumes without HUGE_FILE. A small
fixture helper uses libext2fs's public 64-bit seek/write API: e2fsprogs 1.47.3's
host-file importer narrows offsets through a signed 32-bit parameter. Images
remain 32 MiB (128 MiB at 64 KiB blocks), with logical files reaching the applicable
format ceiling. This tests logical addressing, not large-device capacity.

The sanitized core reads independent sparse extents and indirect trees, writes
across 2/4 GiB byte boundaries and the signed 32-bit block boundary, expands inline
files directly to the extent ceiling, reserves/converts unwritten blocks, punches
partial blocks, truncates across high mappings, regrows zero-filled holes and
reclaims all allocation. Over-limit growth rejects before device writes and keeps
output snapshots unchanged. The optimized freestanding build retains the 2 KiB
stack-frame limit.

All nine focused profiles and 36 exported states pass. Independent checks read
every mapped block, including zero padding, and compare exact logical mappings,
inode/cluster charges, untouched seed files and free counts. All 279 verification
commands and 45 strict nonrepairing fsck checks pass. Reports are
`artifacts/large-file-fixtures-api-retry1/report.json` and
`artifacts/large-file-independent/report.json`; main review is
`artifacts/checks/large-file-focused-accepted.json`. The expanded regression passes
all six jobs and 466 registered tests, with no Meson failures or skips. The new
clustered, inline and large-file independent reports contain only zero-status
commands. The ten existing shared-value reader-only fsck failures and ten fixture
refcount normalization commands retain their explicit classifications below.
Main review is `artifacts/checks/large-file-ci-accepted.json`; raw CI evidence is
under `artifacts/checks/large-file-ci-36386048148/`. This does not cover the later
large-volume journal-width change.

Six Linux/core/Linux roundtrips pass with ordinary ordered mounts: 1/4 KiB,
indirect, inline and the two profiles without HUGE_FILE. The two BIGALLOC profiles
pass the same maximum-offset operations with explicit `nodelalloc`. Together these
runs contain 24 actual boots identifying Linux 6.12.94-0-virt aarch64, 41 recovered
transactions and eight reclaimed orphans. All 352 recorded commands and 32 strict
nonrepairing fsck checks pass. Exact inode identities, modes, ownership, data,
holes, allocation and native replay agree. Returned core mutations survive another
Linux mount unchanged. Main review is `artifacts/checks/large-file-linux-reviewed.json`;
raw lab reports are under `artifacts/ext4-journal/large-file-clean/`,
`large-file-remaining-retry2/`, `large-file-sector-limits/` and `large-file-nodelalloc/`.

The default delayed-allocation BIGALLOC case remains a failed conformance case.
Linux accepts the core-authored image and its maximum-offset data, then commits
an inode charging 64 sectors for 72 sectors of cluster ownership and an unlinked
inode charging 16 sectors for 24. Independent `debugfs journal_run` on the original
Linux journal reproduces both counts without repairing them; evidence is in
`artifacts/checks/large-file-raw-journal/`. The core replays the validated journal
but rejects the inconsistent orphan before reclamation. An earlier diagnostic
`e2fsck -fy -E journal_only` also performed repairs, so its subsequent clean check
is not acceptance evidence. Disabling Linux delayed allocation removes the
failure in this control, but does not establish default-mount compatibility.
The combined inline/BIGALLOC default-mount case was not run after that failure.

The pinned Linux reference also applies its indirect-write limit before converting
new inline files into extents. The probe retains the expected EFBIG check and proves
size/allocation remain unchanged, then completes conversion at a small offset and
exercises the full extent range. Core-authored files expanded directly to that
range are read and mutated successfully by Linux. This native restriction does not
reduce the portable core's tested extent ceiling.

Reproduce with `generate_large_file_fixtures.py`, `ext4-large-file-test --export`
and `check_large_files.py`. Meson registers these profiles through
`-Dlarge_file_fixtures=PATH`; the core CI job runs focused and independent checks.
`prepare_large_file_linux.py` selects the eight profiles supported by the pinned
guest's 4 KiB pages. Run them through `run_linux_journal.py --xattrs` with the exact
`ext4-large-file-test` binary as `--xattr-reader`. `--no-delalloc` selects and records
the separate native allocation control; it is never an automatic fallback.

## Clustered allocation

BIGALLOC reads and mutations use cluster bitmap geometry with block-based extent
addresses. Data allocation reuses the same inode's partially occupied cluster;
hole punching and truncate preserve a cluster until its final data reference is
removed. Directory, mapping and external-attribute blocks each own a full cluster.
Private attribute values and open-unlinked cleanup retain cluster-based charges.

Eight independent 32 MiB profiles cover 1/4 KiB blocks, cluster ratios 4/8/16,
absent metadata checksums, explicit checksum seeds, INLINE_DATA, EA_INODE and
orphan files. Functional checks cover fragmented sparse mappings, partial/final
cluster release, truncate/regrowth, unwritten reservations, external attribute
replacement/reclamation and held-unlinked inode lifetime. Eighty-eight checksummed
malformed cases reject invalid geometry, unaligned or conflicting backing, aliasing
and inconsistent sector accounting before issuing writes. Sanitized and optimized
freestanding builds pass, including the 2 KiB stack-frame limit.

Independent checks accept 32 mutation/lifetime states and 72 clean, committed and
uncommitted replay states. All 5,935 recorded commands exit zero, including 112
strict nonrepairing fsck checks and 24 journal-only oracle replays. Exact data,
namespace, attributes and allocation totals agree; replay is idempotent.
Twelve fault sweeps cover 246 allocation failures, 292 read failures and 1,536
storage cuts. Of these cuts, 1,488 recover the complete old/new state and 48 reject
a torn primary superblock; the latter are not successful crash recovery.

Main-agent review: `artifacts/checks/cluster-focused-accepted.json`. Raw reports:
`artifacts/cluster-independent-retry4/report.json` and
`artifacts/cluster-fault-independent/report.json`. Reproduce with
`generate_cluster_fixtures.py`, `ext4-cluster-test` (ordinary and `--faults` modes),
`check_clusters.py` and `check_cluster_faults.py`; the core CI job runs the same
independent checks.

Two independently filled images have zero free clusters in every group. Ordinary
allocation and reservation return ENOSPC without writes; holes in an existing
cluster remain writable. Partial removal retains the cluster, final removal frees
it, and unwritten reservation plus adjacent writes consume that same space again.
Both final images remain full and pass exact independent state and nonrepairing
fsck checks. A private EA value spanning three clusters also passes exact read,
logical/physical accounting and replacement with an inode-body value, returning
both block and inode totals to baseline. Four additional states and 378 independent
commands pass. Reports: `artifacts/cluster-space-independent/report.json` and
`artifacts/cluster-values-independent/report.json`; main reviews are
`artifacts/checks/cluster-space-accepted.json` and
`artifacts/checks/cluster-values-accepted.json`.

Eight native Linux/core/Linux roundtrips pass: six clean profiles and two pending
core journals, with 24 actual boots identifying Linux 6.12.94-0-virt aarch64.
Linux validates core contents, punches sparse mappings, creates clustered files,
directories and attributes, reserves unwritten space and leaves a fragmented
open-unlinked orphan. Core replay of 40 Linux transactions reclaims eight orphans
and exactly matches native recovery. Core then reads, shrinks, writes, punches,
changes attributes and reclaims Linux-created objects; Linux verifies the returned
state. All 2,032 commands and 32 strict fsck checks return zero. Main review:
`artifacts/checks/cluster-linux-accepted.json`; lab reports are
`artifacts/ext4-journal/cluster-clean-retry1/report.json` and
`artifacts/ext4-journal/cluster-pending/report.json`. The first attempt stopped at
guest-probe compilation because geometry scan variables lacked initialization;
no VM boot occurred until that harness fix. No core fix was needed for native
acceptance. The expanded 466-test regression passes all six jobs; the ten known
shared-value reader-only fsck exceptions remain separate from clean exports.
This does not establish adapter support.

The first expanded regression ran all 454 cases: 452 passed and two hand-built
filesystem models failed because they omitted the new cluster geometry fields.
Both model initializers are corrected and the two targeted tests pass locally.
The same run exposed artifact discovery following an overlong host symlink;
the upload step now removes fixture-host links after testing. The corrected
466-test full run and later 469-test acceptance above pass; the historical failed
run remains available as diagnostic evidence.

## Inode-resident file and directory data

INLINE_DATA is implemented for regular files and directories. Reads and mutations
validate the inode, internal `system.data` entry, both directory regions and
attribute/data accounting. Writes, shrink/growth and hole punching retain inline
storage while it fits; larger data, directory entries and attribute pressure
convert it to ordinary mapping in the owning transaction. Newly created empty
files and directories use inline storage when the inode has room and no inherited
attributes. Raw mutation of `system.data` is rejected. Physical mapping APIs
return unsupported for this representation; adapters must use the byte-read API.

Eight independent 16 MiB profiles cover 1/4 KiB blocks, absent metadata checksums,
explicit checksum seed, absent directory file types, 512-byte inodes, EA_INODE and
orphan files. Ten checksummed malformed cases per profile reject before publishing
data or issuing writes. Functional checks cover every file offset around the
60-byte boundary, both directory regions and cookies, namespace mutation, body
growth, conversion, attribute coexistence and held-unlinked lifetime. Two
independently filled block maps demonstrate creation, inline writes and reclamation
with zero free blocks; oversized directory insertion rejects without writes.

The seven operations on each of the 1/4 KiB profiles cover 422 allocation failures,
175 read failures and 2,016 storage cuts. Recovery accepts 1,956 cuts as the exact
old or new home state and rejects 60 torn primary-superblock states. These rejects
are not successful crash recovery. Independent nonrepairing e2fsck and exact data,
namespace, attributes and accounting checks accept 80 mutation states, four
full-block lifetime states, and 84 before/after/committed/uncommitted replay states.
All 8,442 independent commands exit zero, without repair. Journal-only oracle runs
omit `-f` and reject full fsck passes; repeated core recovery preserves clean bytes.

The main-agent review is `artifacts/checks/inline-focused-accepted.json`; raw logs
are under `artifacts/checks/inline-development/acceptance-*`. Reports are
`artifacts/inline-accepted-independent/report.json`,
`artifacts/inline-full-independent/report.json` and
`artifacts/inline-fault-independent/report.json`.

All eight native Linux/core/Linux roundtrips pass: six clean profiles and two
pending core journals. Twenty-four boots identify Linux 6.12.94-0-virt aarch64.
Linux validates core-authored contents, attributes and directory identities,
mutates across the 60-byte boundary, creates inline files/directories and leaves
an open-unlinked inline inode. Core replay of 80 Linux transactions reclaims all
eight orphans and exactly matches native Linux recovery. Core reads and changes
Linux-authored objects, converts attributed data when needed, removes a child,
renames its directory and creates another inline file; the final Linux mount
validates the complete returned state. All 3,358 recorded commands and 32 strict
nonrepairing fsck checks exit zero. Raw internal data attributes are compared
between recovery implementations but excluded from the native xattr manifest.

The main-agent review is `artifacts/checks/inline-linux-accepted.json`; lab reports
are `artifacts/ext4-journal/inline-clean-retry1/report.json` and
`artifacts/ext4-journal/inline-pending-retry2/report.json`. Two earlier harness
failures remain recorded: creation-only fields supplied to a write, and a
127-character manifest path limit that rejected a valid 255-byte directory name.
Neither required a core change. The expanded 444-test regression passes all six
suites with no failures, errors or skips. Raw JUnit names match the frozen test
inventory exactly; CI also repeats all 168 independent states successfully.
Main-agent acceptance is `artifacts/checks/inline-ci-accepted.json`.
This evidence does not establish installed FSKit or kext inline reads.

## Large values in private attribute inodes

Modern EA_INODE values through 64 KiB are readable and writable. Private value
inodes, entry hashes, whole-value CRC32C, physical reference counts and each
owner's logical block charges are validated separately. Mutation supports creation,
replacement, shrinking back to ordinary storage, removal, shared-block copying,
attributed file/directory/mapped-symlink creation, data writes/truncate, held
replacement and staged final deletion. Multiple allocated/freed inode counters
become visible only after a successful transaction checkpoint.

Six independently authored profiles cover 1/4 KiB blocks, missing metadata
checksums, a stored checksum seed, 128-byte inodes and orphan-file capability.
Exact reader values pass, together with seventeen malformed cases per profile and
every getter allocation/read failure. Create, replace, remove and shared-block
copy faults cover 1,129 allocation failures, 567 read failures and 4,320 storage
cuts: 4,276 recover a complete allowed state, while 44 torn primary superblocks
reject explicitly. These fault tests do not yet cover every staged final-release
boundary. Sanitized and freestanding builds pass.

Independent checks accept 96 functional states and twelve zero-reference private
orphan states. All 2,736 commands return zero, including 108 journal-only replays
and 108 subsequent nonrepairing checks. Exact values, attribute names, namespace,
reference counts and reclamation agree; source/export hashes remain unchanged.
The orphan tests compare complete home blocks against ordinary successful removal.
They synthesize the private orphan representation; actual Linux-authored recovery
is still pending. The final focused compatibility check passes eight tests, and
the separate private-orphan check passes six. Earlier fault evidence is retained
separately in `artifacts/checks/ea-inode-hardening-summary.json`; final evidence is
in `artifacts/checks/ea-inode-{compatibility,orphans}-summary.json`.

Forty-eight interrupted-transaction states also pass independent replay: two
profiles, four operations and clean/committed/uncommitted inputs. Both engines
preserve exact metadata, values and data, and repeated core recovery is unchanged.
All 1,674 commands return zero, including 48 nonrepairing checks and sixteen
journal-only replays. Evidence is in
`artifacts/checks/ea-inode-interop-prepare-summary.json`. Fixtures with a larger
journal repeat these 48 states and pass 24 functional states; their preparation
is reviewed in `artifacts/checks/ea-inode-native-inputs-summary.json`.

Six actual Linux roundtrips pass: four clean inputs and two committed core
journals. Eighteen successful guest boots identify Linux 6.12.94-0-virt aarch64.
Linux reads exact core-authored values and namespace, shares large values across
keys and owners, creates attributed files/directories and leaves an open-unlinked
owner in its committed journal. Core and actual Linux recovery agree on all
metadata, values, data and allocation counts. Core then changes ownership/mode,
replaces one shared value and removes another reference; Linux checks the result.
All 1,144 recorded commands return zero, including 24 strict nonrepairing fsck
checks. Eighteen Linux-authored transactions replay and six orphan owners are
reclaimed. Source images and frozen portable executables remain unchanged.
Evidence is in `artifacts/checks/ea-inode-linux-summary.json`; the successful 4 KiB
case is retained from the first retry, with the remaining five accepted in the
next run. Failed cases below remain excluded.

The expanded CI regression passes all 432 unique registered tests without JUnit
failures, errors or skips. Five unchanged suites contribute 274 passes from the
original full run; the corrected core job contributes 158 from its targeted run.
These are two runs with the same core implementation, not one successful full
workflow. The core job also repeats all 108 functional/orphan states and 48 fault
states with 4,410 zero-status independent commands. The historical ten reader-only
xattr compatibility failures and ten fixture-repair statuses remain separate.
Main acceptance and raw report references are recorded in
`artifacts/checks/ea-inode-ci-accepted.json`.
The initial core job failed while compiling its fixture helper because Linux
requires `sys/types.h` before the e2fsprogs header; that failed job is excluded. The initial 1 KiB fixture exposed Linux's journal credit requirement for a 64 KiB
replacement: 341 requested credits exceed the default small journal's 335-credit
limit. Fixtures now reserve at least 2 MiB and 1,024 journal blocks. This failed
native invocation is not counted as filesystem acceptance.

The pinned e2fsprogs journal-only orphan cleaner also leaves an EA value reference
stale in a Linux-authored image: its later nonrepairing check reports five
references instead of four. Core cleanup passes strict fsck and matches actual
Linux recovery. The failed oracle result remains in the lab's
`artifacts/ext4-journal/ea-inode-clean/report.json`. Native orphan acceptance uses
Linux itself as the independent replayer, with strict nonrepairing checks after
both implementations; the no-orphan transaction matrix above retains e2fsprogs
replay. No oracle image is repaired to make a pass.

Large values on short
symlinks reject before writes: their logical block charge conflicts with Linux's
fast-symlink interpretation. Small short-symlink attributes remain supported.
Legacy Lustre value-inode encodings and new-value deduplication are unsupported;
existing shared modern values are preserved. This checkpoint does not close the
remaining format-compatibility block.

## Transaction checksum v1

The internal journal reads and writes synchronous JBD2 checksum v1 with 32/64-bit
tags. It checks the complete descriptor/data stream, including escaped bytes,
before replay and preserves Linux's exact all-zero legacy-commit transition.
Mixed v1/v2/v3 feature combinations reject before writes. Six focused tests pass,
covering the new profiles, existing journal modes, CRC reference comparisons and
malformed images. The four v1 profiles exercise 408 power cuts: 396 recover allowed
states and twelve torn primary superblocks reject explicitly. Revoke cases pass
64 applicable checks; sixteen revoke-checksum corruption cases are inapplicable
because v1 does not checksum revoke blocks. The 1 KiB profiles also pass 128-block
transactions spanning descriptors. Sanitized and freestanding builds pass.

Nine v1 independent cases and six existing-format controls pass both portable and
e2fsck journal-only replay, exact block comparison, idempotence and nonrepairing
consistency checks. Native Linux then recovers two v1 writer images, changes data
and inode attributes, and commits a directory-block revoke. Its returned journal
retains v1 and replays three transactions with one revoke in each case. Core and
e2fsck agree on complete file bytes and inode metadata, with unchanged original
images. Reports are in `artifacts/checks/journal-v1-local-summary.json` and
`artifacts/checks/journal-v1-linux-summary.json`. The combined 400-test full
regression, including the async support below, passes all six jobs. Its reviewed
evidence is in `artifacts/checks/journal-compat-ci-36367767361/summary.json`.

## Asynchronous journal compatibility

The core accepts async commit with checksum v1/v2/v3 and 32/64-bit tags. Its writer
keeps the existing durability barriers. Recovery distinguishes an interrupted
async tail from a damaged transaction followed by another commit, rejects the
latter before writes, and retains any complete preceding transactions. The
bounded search handles untrusted tags, sequence wrap and 64-bit commit timestamps;
checksum-free async journals reject during admission.

Ten focused tests pass, including four new async suites and existing journal,
malformed-image and checksum controls. Twelve async profiles cover 120 tail
boundary cases and 1,224 write/barrier cuts: 1,188 recover allowed states and 36
torn primary superblocks reject explicitly. Revoke coverage includes 224
applicable async cases and sixteen checksum-absent skips within those cases;
there are no skipped Meson tests. Sanitized and freestanding builds pass.

Fourteen committed writer profiles and fourteen copies with damaged commits pass
portable and independent e2fsck replay, exact home-block comparisons, repeated
recovery and nonrepairing consistency checks. All 182 commands succeed, including
56 core recoveries, 56 nonrepairing checks and 28 journal-only oracle replays.
The remaining commands independently locate commit records. Inputs remain
unchanged. Evidence is in `artifacts/checks/journal-async-local-summary.json` and
`artifacts/checks/journal-async-independent-summary.json`.

Six Linux roundtrips pass: four modern-checksum inputs and two v1 inputs. The guest
uses `data=writeback,journal_async_commit`, fsyncs each tested mutation and leaves
three committed transactions with one directory-block revoke. Both replayers
agree on every file byte and inode attribute. All twelve nonrepairing checks and
six journal-only replays pass; repeated portable recovery changes nothing and
guest logs contain no warnings. Linux retains async commit, with checksum v3 on
modern profiles and v1 on the two legacy profiles. Evidence is in
`artifacts/checks/journal-async-linux-summary.json`. The combined 400-test full
regression passes all six jobs. External journals have subsequent acceptance above;
fast commit remains open work.

## Persistent inode flags

The atomic flag API updates selected policy bits and ctime while preserving
mapping, allocation, bytes and xattrs. It validates type-specific flag masks and
prevents changing other flags while retaining immutable protection. Public write,
truncate, range and namespace operations enforce immutable and append-only state;
creation inherits the supported flags by inode type. Protected held-unlinked inodes
still finish their already-admitted deletion. Native credentials, descriptor append
mode, cache/mapping revocation and timestamp policy remain adapter responsibilities.

All twelve focused tests pass across ten profiles, together with five existing
write/range/namespace controls. Setting and clearing flags each cover allocation,
read and interrupted-write failures on checksummed and 128-byte-inode images:
40 allocation failures, 32 read failures and 312 write/flush cuts. Of those cuts,
304 recover complete old/new states and eight torn primary superblocks reject
explicitly. Builds include sanitizers and the freestanding stack-frame check.
Evidence is under `artifacts/checks/inode-flags-development-evidence/`.

Twelve independent records on extent, indirect and 128-byte-inode profiles pass
exact whole-namespace, inode, data, xattr and allocation comparison. All 54
nonrepairing e2fsck checks and twelve journal-only replays pass; core replay and
idempotence agree for both committed and uncommitted journals. Thirty source
exports remain unchanged. Evidence is in
`artifacts/inode-flags-independent-retry1/report.json`.

Six actual Linux roundtrips pass, including pending set/clear transactions. Linux
reads the exact core-created flags, enforces protected file/directory operations,
clears protection, appends a distinct byte and commits new protection and inherited
flags. Core and independent recovery agree on every inode, byte, attribute and
allocation count; all twelve nonrepairing checks and six journal-only replays pass.
The portable reader accepts all six returned images. No guest warnings occur.
Evidence is in `artifacts/checks/inode-flags-linux-summary.json` and the lab's
`artifacts/ext4-journal/linux-reference/inode-flags-{functional,pending}-linux/`.
The expanded 373-test full regression passes all six jobs with exact inventory,
zero skipped/failed cases and the twelve independently checked flag transitions.
Raw evidence is in `artifacts/checks/inode-flags-ci-36360670891/`; see the
[accepted CI run](https://github.com/machlin-project/ext4/actions/runs/36360670891).

## Distributed descriptors and sparse superblocks

The shared geometry implementation handles META_BG, hybrid contiguous/distributed
descriptor tables and SPARSE_SUPER2 with zero, one or two backup superblocks.
Allocation excludes every descriptor backup, including copies in groups without
superblocks. Mount rejects invalid first-metagroup/backup bounds and contradictory
resize formats; replay rejects geometry changes before writing home blocks.

Nine independent mke2fs profiles cover 1/4 KiB blocks, 32/64-byte descriptors,
CRC16/CRC32C and FLEX_BG on/off. Public creation traverses all 19–67 groups and
writes data in each, then allocates, writes and finally releases an inode in the
last group. Fifteen selected development tests pass, including existing geometry,
journal and group-checksum controls. Two fault profiles cover 228 allocation and
318 read failures, plus 1,008 write/barrier cuts: 980 recover to the correct state
and 28 reject torn primary-superblock checksums without recovery writes.

All 27 exported transitions pass independent comparison of every inode, file byte,
root name, group location and free count. Core and e2fsck agree on committed and
uncommitted recovery; repeated core recovery changes no bytes. The 1,188 unique
oracle commands include 171 nonrepairing fsck checks and 54 journal-only replays,
all successful. All 108 export images and original fixtures remain unchanged.
Evidence is in `artifacts/checks/geometry-development-retry2-evidence/` and
`artifacts/geometry-independent-retry1/report.json`. The original report repeats
each profile's command list across its three records; the counts above deduplicate
by the retained command-log path. Future reports assign commands once.

Eight actual Linux roundtrips pass on the identified reference kernel: six clean
profiles and two pending journals, covering 32/64-byte descriptors, 1/4 KiB blocks,
CRC16, hybrid tables and sparse backups. Linux reads the prepared namespace and
data from every group, writes a distinct byte into the last-group file, and commits
ordinary namespace changes. Core and e2fsck replay agree on all retained inodes,
file bytes and descriptor locations. All 24 nonrepairing/replay fsck invocations
and all eight portable reads pass, with no guest warnings. Evidence is in
`artifacts/checks/geometry-linux-summary.json` and the lab's
`artifacts/ext4-journal/linux-reference/geometry-{functional-linux-retry1,pending-linux}/`.
The later invalid META_BG/RESIZE_INODE admission guard passes four final focused
checks in `artifacts/checks/geometry-final-evidence/`; it does not change the valid
images accepted using the frozen Linux-test binaries. The expanded 394-test full
regression passes all six jobs, with exact inventory and no skipped/failed cases.
Its 27 independently checked transitions also pass. Main-reviewed evidence is in
`artifacts/checks/geometry-ci-36365266842/summary.json`; see the
[accepted CI run](https://github.com/machlin-project/ext4/actions/runs/36365266842).

## Preallocation and hole punching

`ext4_fallocate` reserves holes as unwritten extents, optionally preserving EOF.
Hole punching supports extent and indirect maps, frees complete blocks and zeroes
partial written edges without changing size. Large ranges span bounded transactions
and report a durable prefix on failure. Attributes accompany each checkpoint;
xattr CREATE/REMOVE applies once. Extent splitting, path pruning/collapse and held
unlinked cleanup use the existing allocation and orphan ownership rules.

All twelve focused range tests pass across ten writable profiles, with five
unchanged allocation/write/truncate controls also passing. The final fault matrix
covers 124 allocation failures, 68 read failures and 426 write/flush cuts: 414
recover a complete old/new state, while twelve torn primary superblocks reject
explicitly. Sanitized and freestanding builds pass. Evidence is under
`artifacts/checks/file-range-development-evidence/`.

Independent inspection accepts eighteen records across extent, indirect and
128-byte-inode profiles: exact bytes, maps, attributes, namespace and allocation
accounting. All 56 nonrepairing e2fsck checks, ten journal-only oracle replays and
twenty core recovery/idempotence calls return zero. The initial checker exposed a
real admission bug: indirect reservation past EOF produced an invalid image.
Reservation now rejects that format before any write; indirect punching remains
supported. The failed image and report are retained separately. Accepted evidence
is in `artifacts/checks/file-range-independent-retry1-summary.json`.

Five actual Linux roundtrips pass, including three pending core journals. Linux
preserves the core-created ranges and authors new preallocation and holes; core
and independent replay agree. All ten nonrepairing checks and five journal-only
replays return zero, and the portable reader verifies all five returned images.
Reports are in `artifacts/checks/file-range-linux-summary.json` and the lab's
`artifacts/ext4-journal/linux-reference/file-range-{pending,functional}-linux/`.
The combined 361-test CI regression passes. This accepts bounded range behavior;
the reserved mapping-capacity extension is documented below.

## Preallocated writes without free mapping space

When splitting an unwritten extent cannot allocate a mapping node,
`ext4_write_partial` can zero and initialize the existing extent in bounded
transactions before committing the requested bytes. No block allocation or
relocation is needed. Preparatory commits preserve visible bytes, EOF, attributes
and allocation counts; uncertain commits poison the owner. The atomic API still
rejects the same exhausted split without changing the medium.

Seven small-root profiles, a multi-transaction extent and a full external leaf
pass. Four existing allocation/growth/write controls also pass. The fault matrix
covers 126 allocation failures, 115 read failures and 1,116 write/flush cuts;
1,114 cuts recover an allowed state and two torn primary superblocks reject
explicitly. Inaccessible backing is deliberately nonzero. All eighteen exported
states pass exact byte, physical-map, inode and allocation comparison plus
nonrepairing e2fsck. Evidence is in `artifacts/checks/preallocation-capacity-*-summary.json`
and `artifacts/preallocation-capacity-{full,large,tree}-independent*/report.json`.

The combined 361-test CI regression passes all six jobs with exact inventory
coverage and no skipped tests. The independent steps also pass; ten previously
documented reader-only shared-value e2fsck exit-4 cases remain explicit exceptions,
not accepted writable images. Evidence is in
`artifacts/checks/preallocation-ci-36358407253/summary.json`.

Three Linux roundtrips of core-converted full-disk images pass: full inode root,
multi-transaction extent and full external leaf. Linux changes a byte before any
space is released, then authors new range operations and a pending journal. Both
replays retain the exact changed byte and all surrounding data; all six
nonrepairing checks, three journal-only replays and three portable reads pass.
Evidence is in `artifacts/checks/preallocation-capacity-written-linux-summary.json`.

An initial Linux check of a large still-unwritten reservation reported a delayed
allocation error and possible data loss despite successful syscalls. Its same-byte
write could not detect a lost write, so that report is not accepted. The harness
now changes the byte, compares exact post-replay contents and rejects those kernel
diagnostics. The initial evidence remains in
`artifacts/checks/preallocation-capacity-linux-summary.json`.

## KEEP_SIZE growth without free mapping space

A write reaching the last block of a reserved extent can zero its backing in
bounded transactions, retain the unwritten mapping through preparation, then
commit initialization, bytes, EOF and attributes together. It requires no new
block or relocation. Imported full leaves without spare capacity still require
splitting space for shorter growth and reject unchanged when exhausted. New
reservations retain that capacity as described below.

Eleven focused cases pass: seven full-root profiles, a large extent, a full external
leaf, interrupted growth and a durable prefix preceding an unallocated hole.
Six existing range, full-space and flag controls also
pass. The new fault matrix covers 128 allocation failures, 115 read failures and
1,116 write/flush cuts: 1,114 recover complete allowed states, and two torn primary
superblocks reject explicitly. EOF remains zero throughout preparation, even with
deliberately nonzero backing, and appears with the committed byte and timestamps.
Sanitized and freestanding builds pass. Evidence is under
`artifacts/checks/preallocation-growth-development-evidence/`.
Twenty-two independent states pass across small roots, large extents, external
leaves and committed/uncommitted journals. Exact bytes, physical mappings,
allocation counts, inode attributes and neighboring objects agree; 22 nonrepairing
e2fsck checks and four journal-only oracle replays return zero. Both recovery
engines preserve old EOF for uncommitted data and publish new EOF only with its
byte; repeated core recovery changes nothing. Evidence is in
`artifacts/checks/preallocation-growth-independent-summary.json`.
Six Linux roundtrips pass, including pending data/EOF journals on checksummed and
128-byte-inode profiles. Linux reads the grown data, changes a distinct byte while
the disk remains full, then authors new ranges and a reverse journal. Core and
oracle replay agree on retained bytes, metadata and allocation; twelve nonrepairing
checks, six journal-only replays and six portable reads pass. No guest warnings
occur. Evidence is in `artifacts/checks/preallocation-growth-linux-summary.json`.
The expanded 383-test full regression passes all six jobs with exact inventory
coverage and no skipped tests. All 22 growth records and their 4,862 commands pass.
The previously documented ten reader-only shared-value e2fsck exit-4 cases and ten
fixture repair exit-1 statuses remain separate from writable acceptance. Evidence
is in `artifacts/checks/preallocation-growth-ci-36363055799/summary.json`.

## Reserved mapping capacity and partial EOF growth

KEEP_SIZE reservation now retains room for one initialized/unwritten boundary in
each affected leaf, obtaining mapping blocks before promising the backing. The
same check applies to already allocated ranges. A contiguous old boundary can
release its record when preparation zeros and merges its suffix. Growth can then
publish a new partial prefix in another extent with no free blocks. Insertion,
hole punching and tree collapse preserve this capacity; no private disk format
or hidden allocation pool is introduced.

Sixteen focused checks pass across seven root profiles and two external-leaf
geometries, including earlier full-tree and durable-prefix controls. Preparation
handles nonzero inaccessible backing and transfers EOF between separate reserved
extents. Two fault profiles cover 214 allocation failures, 174 read failures and
1,440 write/barrier cuts: 1,436 recover exact allowed old/new states, while four
torn primary superblocks reject explicitly. The final checkpoint review also
corrected growing fallocate to use each checkpoint's actual EOF rather than the
requested final size. Its regression preserves an earlier reservation after
failed growth and another file's consumption of the last free block. That case
and twelve affected range controls pass. Sanitized/freestanding builds pass.

Thirty independently checked states pass: fourteen root, four external-leaf and
twelve clean/replayed fault states. Exact data, initialized/unwritten boundaries,
unchanged physical mappings, inode attributes, neighboring objects and zero free
blocks agree. All 594 oracle commands succeed, including thirty nonrepairing
e2fsck checks, four journal-only replays and eight core recoveries. Six actual
Linux roundtrips pass, including two pending boundary-transfer transactions.
Linux reads the exact data and changes a distinct byte while the disk is full,
then commits new range operations. Core/oracle replay, eighteen fsck invocations
and six portable reads pass without guest warnings.

Evidence is in `artifacts/checks/reservation-{development,independent,linux}-summary.json`
and `artifacts/checks/reservation-{checkpoint,final}-summary.json`. Independent/Linux
acceptance used frozen binaries before the final non-KEEP_SIZE checkpoint fix;
that fix has separate focused evidence. Final review also covers imported leaves
whose advertised capacity is below their physical room, expanding that bound
before a reservation-driven split. Root/external controls verify reuse of that
room without new allocation. The full 412-test regression passes all six jobs,
with all registered tests represented once and no failures or skipped tests.
Its independent reservation checks again pass all 30 states and 594 commands.
Evidence is in `artifacts/checks/reservation-ci-36370249935/summary.json`.
The historical checker invokes `e2fsck -fy -E journal_only`; `-f` also requests full
checking, so these are not evidence of journal-only execution. All reservation
oracle statuses are zero, without repairs. New EA_INODE checks below omit `-f`
and reject any unexpected filesystem-check passes during journal replay.
Imported full leaves without a spare or reclaimable boundary can still
require new metadata, and the fallback's sustained zeroing cost is unaccepted.

## Legacy group checksum evidence

`GDT_CSUM` descriptor verification and mutation are implemented with CRC16;
lazy inode and block initialization now admits this format as well as metadata
checksums. Six independently generated images pass exact feature/geometry checks
and nonrepairing e2fsck. They cover 32/64-byte descriptors, 1/4 KiB blocks, extent
and indirect maps, and 128-byte inodes. Both small eight-group profiles retain
lazy block/inode bitmaps and seven uninitialized inode tables.

The sanitized build and optimized freestanding 2 KiB frame check pass. All 17
selected Meson tests pass: the 14 format tests plus checksum, existing reader
and malformed-image controls. They exercise allocation/freeing, large writes,
growth, namespace mutations, group transitions, orphan cleanup and interrupted
commits. Both small images exhaust all 115 available inodes across eight groups.
The independent polynomial reference agrees on 2,016 seeded/aligned CRC16 cases;
72 descriptor profiles check CRC16/CRC32C selection, and 8,064 descriptor-byte
mutations reject without publishing output. Bitmap/inode/directory checksum cases
remain explicitly inapplicable where that checksum format is absent.

Evidence is under `artifacts/group-checksum-fixtures/` and
`artifacts/checks/group-checksum-development-retry1-*`.

Independent mutation checks pass all 48 case records: four writes, four allocation
states, sixteen truncate checkpoints, four growth cases with both write and
truncate outcomes, and twenty namespace cases. Contents, attributes, mappings,
group accounting and recovery comparisons pass; all 168 e2fsck commands return
zero: 136 nonrepairing checks and 32 journal-only oracle replays on copied images.
Fixture and exported input bytes remain unchanged. Reports
are under `artifacts/group-checksum-*-independent/`, with the combined review in
`artifacts/checks/group-checksum-independent-retry1-summary.json`.

Ten roundtrips through the actual Linux reference kernel pass: four allocated
files, two exhausted inode pools, two clean cross-group mkdir states and two
pending mkdir journals. Linux verifies or replays the portable results, reuses
inodes and authors new committed transactions. Portable and independent replay
agree on the returned namespace/data and allocation accounting; all 22 recorded
e2fsck commands return zero. Source images remain unchanged. Reports are in the
lab under `artifacts/ext4-journal/linux-reference/group-checksum-*-linux/`, with
review in `artifacts/checks/group-checksum-linux-summary.json` in this repository.
The complete six-job CI regression passes all 270 registered tests exactly once,
with no missing, duplicate, unexpected or skipped Meson tests. Its independent
checks reproduce all 48 format cases, 88 exports and 168 successful e2fsck commands.
The ten earlier shared-value reader-only exceptions remain separate from clean
filesystem acceptance. Evidence is under
`artifacts/checks/group-checksum-ci-36346361547/`; see the
[accepted CI run](https://github.com/machlin-project/ext4/actions/runs/36346361547).
Large-volume and native writable behavior are not established by these bounded
profiles.

## Large-directory development evidence

LARGEDIR feature negotiation admits two internal HTree levels. Lookup follows
bounded per-level cursors and collision continuations; mutation propagates splits
through the validated parent chain in one namespace transaction. A full legacy
root grows only when the feature is enabled. A full root at the negotiated maximum
height rejects further growth without writes, while existing leaf slack remains
usable. The existing 1,048,576-block mutation bound remains explicit.

Four e2fsprogs-authored 64 MiB images pass nonrepairing e2fsck and independent
name/hash/shape checks. Three have over 46,000 entries and two internal levels,
covering extent maps, indirect maps and CRC16 groups; the fourth has a full
123-entry root ready to grow. Nine compact derived graphs retain independently
hashed names and pass nonrepairing e2fsck, covering root growth, cascading splits
and maximum-height capacity in all three formats. They occupy 372 through 751
directory blocks, using underfilled nodes to bound exhaustive fault enumeration.

The sanitized build and optimized freestanding 2 KiB frame check pass. All 32
selected Meson tests pass on their first execution: 25 large-directory tests and
seven earlier indexed/malformed-image controls. No Meson tests skip; 46 embedded
applicability notices are recorded separately. Lookup/iteration compares every
name against debugfs on read-only and writable owners. An isolated eight-leaf
collision model exercises backtracking across both internal levels, missing
names and resource failures. Functional mutation includes long names, creation,
symlinks, cross-parent moves/exchange, dotdot and indexed removal/lifetime.

The six compact growth transitions pass all 3,256 allocation and 13,335 read
failures and all 1,320 write/flush cuts. Of the cuts, 1,296 recover the complete old
or new state; 24 torn checksummed primary superblocks explicitly fail closed.
Every successful transition adds one leaf and two internal nodes in a single
commit. Evidence is under `artifacts/large-directory-fixtures/`,
`artifacts/large-directory-edge-fixtures/` and
`artifacts/checks/large-directory-development-*`.

All 34 exported images pass the independent functional, split/recovery and
capacity checks: 13 case records, 48 clean nonrepairing e2fsck checks, 14 matching
e2fsprogs journal replays and 28 successful core replay/idempotence commands.
Exact byte-name mappings, retained objects, file contents, directory topology and
allocation accounting agree. Protected inputs and binaries remain unchanged.
Evidence is under `artifacts/large-directory-independent/` and
`artifacts/checks/large-directory-independent-summary.json`. The final functional
report is in its `functional-content/` directory; it additionally verifies exact
bytes of the alternate hardlink target without repeating mutation or fault runs.

The first Linux attempt stopped in the guest expectation parser's old 4,096-name
limit, before namespace acceptance. The probe now has an explicit larger bound
and sorted binary lookup for expected names, avoiding a quadratic comparison.
Its original failed run remains in the lab under
`artifacts/ext4-journal/linux-reference/large-directory-functional-linux/`.
The first pending-journal attempt stopped at the snapshot checker on optional
extent-collapse advice. The same exact advice already appears in the successful
nonrepairing checks of the independently authored compact extent fixtures; it
does not indicate a repair. The checker now admits only advice already recorded
for that verified clean source, retaining it separately in the roundtrip report.
All other unexpected diagnostics remain failures. This initial run is retained
under the lab's `large-directory-pending-linux/` output directory.

All ten final Linux roundtrips pass: three large functional states, six compact
pending root/cascade transitions and the independently authored full-root growth
journal. The guest checks exact names and retained objects, replays the portable
commit where applicable, and commits its own namespace changes. Core and e2fsprogs
reverse replay agree, including data, metadata and allocation accounting; all
20 nonrepairing checks return zero and repeat recovery is idempotent. Reports
are under the lab's `artifacts/ext4-journal/linux-reference/large-directory-*`
directories, with the accepted functional and compact pending runs suffixed
`-retry1`. Earlier successful stages were not repeated after the harness fixes.
The complete six-job CI regression passes all 295 registered tests exactly once,
without missing, duplicate, unexpected, skipped or failed Meson results. Its
independent checks reproduce the functional, split/recovery and capacity cases.
Evidence is under `artifacts/checks/large-directory-ci-36348971572/`; see the
[accepted CI run](https://github.com/machlin-project/ext4/actions/runs/36348971572).
These bounded profiles do not establish large-volume performance or native writes.

## Automatic directory growth evidence

A full one-block linear directory now acquires an HTree root and one or two
leaves in the insertion transaction. Parent mapping, size, checksums and INDEX
flag commit together. Indexed parents that exceed 65,000 links atomically enable
DIR_NLINK and retain the unknown-count sentinel through subsequent namespace
operations. File hardlink and nonindexed-parent limits remain enforced.

The sanitized and optimized freestanding builds pass, including the 2 KiB frame
budget. All 25 selected cases pass across the recorded runs: eighteen new cases
and seven regression controls. The initial compile found an unused local; two
test-preparation corrections replaced an index-only helper used on a linear
directory and a 64 KiB reader image without a journal. Only unfinished cases were
rerun. The final suite covers 1 through 64 KiB blocks, eighteen hash/signedness
combinations, 180 resource failures and 684 write/flush cuts. Of those cuts, 666
recover the complete old or new state; eighteen torn checksummed primary
superblocks fail closed. Compact counter models bound fault enumeration; a
separate independently authored image contains all 64,998 actual child directories
at the link-count boundary. Evidence is under `artifacts/checks/directory-growth-development-*`.

Independent checks pass sixteen index conversions, the real link-count overflow
and twenty-two rename cases, including one- and two-leaf conversions. All 234
nonrepairing e2fsck checks and 78 journal-only oracle replays return zero; 156 core
replay/idempotence commands agree on old/new contents, metadata and accounting.
Reports are under `artifacts/directory-growth-independent/` and
`artifacts/directory-growth-rename-independent/`.

Three pending-journal roundtrips through Linux pass: extent and indirect index
creation, and real DIR_NLINK enablement. Linux replays the core commit, verifies
the namespace and commits new changes; core and oracle reverse replay agree,
with six nonrepairing checks, three journal-only replays and idempotent repeated
recovery. Inputs and binaries remain unchanged. Evidence is in
`artifacts/checks/directory-growth-linux-summary.json` and the lab's
`artifacts/ext4-journal/linux-reference/directory-growth-pending-linux/`.
The 313-test CI run found a checker defect in both namespace and legacy-group
inspection: it expected the old parent flags after legitimate automatic index
creation. The checker now admits that transition with bounded growth, exact names,
unchanged unrelated attributes and clean e2fsck. Its 16 KiB regression control
passes independently with the special-file package below. The complete regression
has not yet been accepted. The reference Linux kernel has 4 KiB pages; 64 KiB
format acceptance here is portable and independent only.

## Special files and atomic whiteout

Character/block devices, FIFOs and sockets now share creation attributes, xattrs,
hardlinks and held-inode lifetime with existing namespace operations. Both device
encodings preserve 12/20-bit identifiers. Atomic whiteout retains a character
device 0:0 at the old source name while moving the source, including populated
directories, replacement, destination growth and admitted whiteout attributes.
Platform device access and authorization remain adapter responsibilities.

All 29 selected sanitized tests pass, including 25 new cases across the writable
format profiles, indexed/no-filetype parents and CRC16 groups. The freestanding
2 KiB frame check passes. The fault matrix covers 1,274 resource failures and
4,644 write/flush cuts: 4,524 recover a complete old/new state, while 120 torn
checksummed primary superblocks explicitly fail closed. Three checksum cases are
inapplicable; no Meson test skips. Evidence is in
`artifacts/checks/special-files-development-*`.

Independent checks pass 43 records: 18 special-file creations, 18 whiteout scenarios
and seven 16 KiB namespace controls. All 254 nonrepairing checks, 84 journal-only
oracle replays and 168 core recovery/idempotence commands return zero. Reports are
under `artifacts/special-files-independent/` and `artifacts/whiteout-independent/`.

Nine pending-journal roundtrips through Linux pass. Linux checks the core-created
objects, creates its own devices/FIFOs/sockets and performs RENAME_WHITEOUT. Core
and oracle replay agree, including the whiteout's external attribute; eighteen
nonrepairing checks and nine journal-only replays return zero. The portable reader
then verifies all nine returned images without writes. Source images and prepared
binaries remain unchanged. Evidence is in `artifacts/checks/special-files-linux-summary.json`
and the lab's `artifacts/ext4-journal/linux-reference/special-files-pending-linux/`
and `whiteout-pending-linux/`. The expanded CI run caught another stale INDEX-flag
expectation in the independent full-space mkdir checker. The corrected checker
accepts only the bounded one-block linear-to-indexed transition; exact identity,
names, bytes, counters and e2fsck remain required. All eight profiles and 32 states
pass locally, with forty successful nonrepairing checks. Evidence is in
`artifacts/checks/space-autoindex-summary.json`; the combined 361-test CI regression
now passes with both independent checker corrections.

## Meson build acceptance

Meson replaces CMake for the portable library, utilities, tests and CI. The complete
207-test registration retains the executable arguments and existing explicit
deadlines, with six disjoint CI suites. All 207 tests pass under ASan/UBSan across
the initial run and a six-test follow-up: the first invocation passed 201 before
an incorrect local unwritten-fixture path caused a failure and stopped remaining
work. The follow-up uses the existing images and runs only the six unfinished
cases. The original failed attempt remains in the generated evidence; it is not
reported as one uninterrupted successful run.

Fresh and repeated Make builds, selected tests and clean pass with the Xcode
compiler selected through `xcrun`; clean retains reports and fixture images.
Meson 1.3 also configures all 207 cases, passes three uninstrumented attribute
tests and builds the bounded fuzzer, which completes 100 smoke executions with
ASan/UBSan. All 21 core translation units pass the separate optimized freestanding
compilation and 2 KiB frame budget. Evidence is in
`artifacts/checks/meson-migration-acceptance.json` and
`artifacts/checks/meson-toolchains-summary.txt`. This validates the build migration;
it adds no filesystem-format or platform-runtime acceptance.
Published Meson CI also passes all six jobs and 207 tests, with all 369 existing
independent attribute mutation/packing/full-space states checked. The downloaded
artifacts and run metadata are verified in `artifacts/checks/meson-ci-manifest.json`.

## Portable reader evidence

`tests/generate_fixtures.py` creates three 64 MiB images: 4 KiB blocks, 1 KiB
blocks, and an e2fsck-indexed copy. The explicit feature set includes extents,
64-bit descriptors, flex_bg and metadata_csum. Each generated image must pass
e2fsck; the generator rejects extra or missing feature flags.

With `--extended`, it creates eleven profiles covering every power-of-two block
size from 1 to 64 KiB, indirect addressing with 32-bit group descriptors, no
metadata checksums and an explicit checksum seed. The 64 KiB fixture has no
journal. All images in `artifacts/fixtures-extended/` passed e2fsck and the
sanitized reader, with exact expected feature sets. These small images do not
exercise large physical block numbers, triple-indirect addressing or all ext4
features.

The accepted profile has no POSIX ACLs, security xattrs or verity inodes. Their
access and integrity policies are not implemented or accepted by the current
adapters. Supporting such files requires preserving and enforcing their owning
contracts, rather than treating mode bits and ordinary data reads as equivalent.

`make test` runs the image reader and malformed-image suites. The reader verifies
exact bytes for ordinary, empty, short/long symlink and sparse files, hard-link
identity, nested and indexed directory enumeration, unaligned reads, EOF and
allocation/I/O failure cleanup during mount. The malformed-image suite checks
the CRC32C standard vector, geometry, incompatible flags, recovery state and
superblock/group/inode/directory checksum rejection. Current local reports live
under `artifacts/checks/`. Independently regenerated images under
`artifacts/fixtures-reproduced/` passed the same reader suite and e2fsck.

The freestanding compilation is not a loaded kernel driver. Its only non-core
undefined symbols are compiler stack-protector support, which XNU exports through
Libkern. The sanitizer build has a different frame layout and is tested separately.

The metadata fixture also covers 32-bit UID/GID, signed pre-epoch time, dates
beyond 2038 and 2106, birth time and nanosecond precision. A malformed timestamp
with a valid inode checksum is rejected. Current regenerated images are in
`artifacts/fixtures-metadata/`; the Meson `fixtures` option selects them.

The bounded metadata fuzzer completed 106,119 executions in 61 seconds with
Clang 22.1.5, ASan and UBSan, without a crash or sanitizer diagnostic. It mutates
the superblock, group descriptor, root inode and root directory, optionally
repairing checksums to exercise structural validation. Reports are in
`artifacts/checks/fuzzer-llvm-build.log` and `artifacts/checks/fuzzer-run.log`.
This run does not cover every disk feature, recovery, writes or concurrency.

## Portable extended-attribute reader evidence

Ten independently generated profiles exercise inode-body, external and shared-block
attributes at 1/4/16/64 KiB block sizes, indirect mapping, checksum variations,
128/512-byte inodes and modern orphan files. Tests compare all 15 attributes per
profile against raw debugfs output, including binary/empty values, long and non-ASCII
names, compact access/default ACLs, short/mapped symlinks and a full-block value
of 65,480 bytes on the largest profile. Get/list run under both read-only and
exclusive writable owners while performing no writes.

All ten sanitized suites pass. Forty complete get/list fault sweeps inject 600
allocation/read failures, preserving output buffers, size/count results and
allocation balance. There are 179 external malformed-record cases and 36 inode-body
cases, including duplicates across both storage areas; each must reject both get
and list before output changes. One checksum-absent case and four inode-body cases
on 128-byte inodes are explicitly inapplicable. All 258 fixture files retain their
hashes. Reports are in `artifacts/xattr-reader-edges.xml` and
`artifacts/checks/xattr-reader-edges-summary.json`.

Independent inspection checks 320 exact values across 20 exported states, plus
complete names, inode metadata, accounting and protected input hashes. Ten unknown
namespace states pass nonrepairing e2fsck. The other ten deliberately share value
ranges and zero external entry hashes: debugfs reads the expected bytes, while
e2fsck reports an allocation collision and invalid zero hash. Those ten are reader
compatibility cases, not clean exports. Evidence is in
`artifacts/xattr-independent-hex-names/`. The initial checker failed to decode
debugfs's hex-rendered non-ASCII names; the corrected run used the same untouched
images. The final selected-Xcode build, formatter and freestanding stack check
pass, as do 23 combined xattr, image-reader, malformed-input, inode-write,
journal and full-space suites. All 30 selected source images remain unchanged.
Evidence is in `artifacts/xattr-final.xml` and
`artifacts/checks/xattr-final-summary.json`. Published reader CI passes all six
jobs and 177 JUnit suites. Downloaded evidence and full test logs are pinned in
`artifacts/checks/xattr-ci-manifest.json`; reader compatibility and embedded
applicability skips remain separate from clean-image and successful recovery counts.

Raw attribute access does not implement POSIX ACL authorization, macOS xattr naming
or Linux privilege transitions. Both platform adapters remain read-only.

## Portable extended-attribute mutation evidence

Six operations on ten profiles test create/set/replace/remove batches, unchanged
outputs on errors, shared-block copy-on-write, shared reference release, final
unique-block freeing, inline/mapped symlink attributes and atomic changes to raw
ACL/security bytes with mode/UID/GID/ctime. Complete allocation/read failure sweeps
inject 1,204 allocation failures and 1,006 read failures. The 6,768 write/flush cuts
recover 6,556 consistent old/new states and explicitly reject 212 torn primary
superblocks with invalid checksums. A durable commit must recover the new state.
Those 212 cases establish rejection, not successful media repair. Reports are in
`artifacts/xattr-write-faults-first.xml` and
`artifacts/checks/xattr-write-smoke-first-summary.json`.

Independent e2fsprogs inspection verifies 320 snapshots, 4,810 raw values, complete
names, inode metadata, block accounting, symlink bytes and shared reference counts.
All snapshots pass nonrepairing e2fsck. Separate copies of pending/uncommitted exports
are replayed by the core and by e2fsprogs; both must produce the expected state,
including the exact shared-block ownership. Repeated core recovery changes no bytes.
All 12,860 commands return zero and protected inputs remain unchanged. Evidence is
in `artifacts/xattr-write-independent-tools/`; an earlier tool-path startup failure
is retained separately and did not inspect or modify any image.

This accepts the linked-inode raw attribute batch at that checkpoint. Attribute
lifetime integration has separate evidence below; ACL enforcement and broader Linux
xattr roundtrips remain required. Full-space,
packing and additional ownership checks also pass: ten full-space profiles exercise
in-place/unchanged writes, shared-block allocation rollback and reuse of a released
block. Ten packing/ownership profiles cover maximum values, exact packing across
both storage areas and malformed type/map/refcount/system-block/accounting rejection.
The 128-byte inode profile explicitly has no two-region packing case. Independent
inspection accepts 29 packing snapshots and 20 full-space snapshots, including
unchanged filler data and block mappings. All 2,033 independent commands return zero.
The combined selected regression passes 53 suites under sanitizers, and the
freestanding stack-budget build passes. Evidence is in
`artifacts/checks/xattr-write-edges-summary.json`, `artifacts/xattr-write-edges.xml`,
`artifacts/xattr-write-independent-edges/` and `artifacts/xattr-write-independent-full/`.
Published writer CI passes all six jobs and 207 tests, including independent
verification of all 369 writer, packing and full-space states. Complete logs retain
the fault counts and explicit applicability skips. The downloaded evidence and
reviewed hashes are in `artifacts/checks/xattr-write-ci-manifest.json`.
Linux runtime roundtrips for these attribute mutations remain pending.

## Attribute lifetime and first-attribute evidence

The working core supports attributes through new files/directories and both symlink
representations, ordinary writes/truncate, hard links, rename replacement, held
unlinked mutation and final release. Ten profiles pass the targeted lifetime fault
sweeps: 4,920 allocation failures, 4,107 read failures and 23,448 persistence cuts.
The cuts recover 22,863 consistent states and reject 585 torn primary superblocks.
Independent inspection records 760 clean states across these profiles; ten additional
e2fsck replay outcomes fail and remain reported as failures, as described below.
Evidence is in `artifacts/checks/xattr-lifetime-fault-remaining-summary.json` and
`artifacts/checks/xattr-lifetime-independent-all-summary.json`.

Final-release tests retain duplicate references and a second orphan ahead of the
victim, covering shared/exclusive attributes, fast/mapped symlinks, directories and
large files. The last reference must be consumed exactly once even on failure; the
aborted instance must reject reads and recovery must complete the already committed
deletions. Fifty sweeps inject 2,201 allocation failures, 1,930 read failures and
7,236 persistence cuts, recovering 7,146 states
and rejecting 90 torn superblocks. Independent core and e2fsck replay verifies all
360 states. Evidence is in `artifacts/checks/xattr-release-summary.json` and
`artifacts/checks/xattr-release-independent-summary.json`.

Ten separate e2fsprogs-created images begin without EXT_ATTR and without attributes.
First-attribute tests cover inode-body/external values, creation and compound
data/size changes. They exposed and fixed recovery's rejection of a committed
EXT_ATTR enablement. Repeated recovery must also reject clearing that feature or
changing an unrelated compatibility bit without writing the image. Eighty sweeps
inject 1,481 allocation failures, 1,097 read failures and 12,276 cuts: 11,916 recover
and 360 reject torn superblocks. All 410 independent states pass e2fsck, exact values,
accounting, feature flags and both recovery implementations. The original failure
is retained separately from the corrected results in
`artifacts/checks/xattr-enable-corrected-summary.json` and
`artifacts/checks/xattr-enable-independent-summary.json`.

The pinned e2fsck loses an external attribute when cleaning a linked truncate orphan,
then reports stale inode block accounting. The same defect reproduces on an original
e2fsprogs fixture with only debugfs setting its orphan head, without executing this
core. That reproduction is in `artifacts/e2fsck-linked-xattr-reproducer/`.
`check_xattr_lifetime.py --keep-going` retains failed replay images and returns nonzero;
it does not reclassify them as successful recovery. Real Linux recovers and preserves
the attributes on all eight profiles supported by its 4 KiB page size, followed by
clean unmount, nonrepairing e2fsck and unchanged repeated core recovery. The 16/64 KiB
profiles have portable/independent checks but no Linux runtime result. Linux evidence
is in `artifacts/xattr-lifetime-linux-truncate-first/` and
`artifacts/xattr-lifetime-linux-truncate-remaining/`.

The combined 227-test regression also passes under ASan/UBSan, with zero failed or
JUnit-skipped tests and all 104 fixture/expected-file hashes unchanged. Build,
style and optimized freestanding checks pass. Evidence is in
`artifacts/checks/xattr-lifetime-regression-summary.json`. All six published CI jobs
also pass, registering the same 227 tests. Downloaded artifacts independently
confirm 360 final-release states and 410 first-attribute states. Inode-body checks
on 128-byte inodes and checksum-only revoke cases on checksum-free journals retain
explicit applicability skips inside test output; they are not extra passes.
The reviewed CI evidence is in `artifacts/checks/xattr-lifetime-ci-manifest.json`.
This does not complete broader bidirectional Linux xattr/ACL/security tests,
performance acceptance or writable platform integration.

## Linux attribute interoperability

Twenty-four images pass a complete Linux/core/Linux attribute roundtrip:
the original independent fixture, a core metadata mutation and core-created
attributed objects on each of eight profiles. These cover 1/4 KiB blocks, indirect
mapping, checksum-free and checksum-seed formats, 128/512-byte inodes and orphan
files. Linux verifies all names and values, inode identities and file
contents before modifying shared attributes, chmod/ACL state and inherited ACLs.
It creates user/trusted attributes and a valid file capability, and checks ACL
read/write decisions and trusted-namespace visibility under an unprivileged UID.
These are Linux policy checks; the portable core continues to store opaque metadata.

Linux then stops with committed journal transactions pending. Both the portable
recovery engine and independent e2fsck replay preserve the exact expected state.
The core reads every attribute under read-only and writable owners, then atomically
changes Linux-created attributes and ownership while removing the ACL and capability.
A second Linux boot verifies the returned image, followed by clean unmount and
nonrepairing e2fsck. Each image replays two Linux-authored transactions; protected
sources and the prepared recovery/reader executables remain unchanged.
Evidence is in `artifacts/xattr-linux-verified-command/report.json` and
`artifacts/xattr-linux-remaining/report.json`, with reviewed summaries under
`artifacts/checks/`.

Another 48 cases start with a committed core attribute transaction waiting for
recovery: creation, shared-block copy-on-write, detachment, final block release,
replacement and compound metadata mutation on every profile. Linux directly
replays that log before checking the independently expected state, then completes
the same Linux/core/Linux cycle. All 48 pass, including exact pending-input identity,
unchanged source hashes, core/oracle recovery and returned Linux state. Evidence is
in `artifacts/xattr-linux-pending/report.json` and
`artifacts/checks/xattr-linux-pending-summary.json`.

Across the two input modes, 72 complete cycles execute 144 guest boots identifying
Linux 6.12.94-0-virt aarch64 and replay 144 Linux-authored transactions through the
portable recovery engine. The 48 pending-input cases additionally verify Linux
replay of the core-authored transaction. No checker command fails in these runs.

The first guest compilation failure and a separate command-transcription error are
preserved outside these successful results. The 16/64 KiB-block profiles have no
Linux runtime result with this 4 KiB-page reference kernel. This evidence does not
accept platform ACL/capability enforcement or writable adapters.

## Journal evidence

`ext4-journal-test` exercises 1 KiB and 4 KiB media with five tag/feature
combinations: legacy checksums absent, checksum v2/v3, and v2/v3 with 64-bit tags.
The independent volatile/stable storage model interrupts every write and flush
in a two-block transaction, retains none/all/alternating pending blocks, and
also tests partial writes. Across 1,020 cases, 990 recover to consistent old or
new contents and 30 deliberately torn primary-superblock cases reject recovery
with a checksum error. Those 30 are fail-closed tests, not successful repairs.

The same suites check interrupted recovery and repeat recovery, ownership and
credit limits, canceled transactions, allocation/read errors, balanced resources,
escaped records, ring and sequence wrap, and 128-block transactions spanning
multiple descriptors. Corrupted committed payloads/descriptors reject writes;
invalid v2/v3 commits discard the incomplete tail. Structural cases with repaired
checksums cover protected/out-of-range targets, unterminated tag lists, invalid
flags/features and journal geometry. The parser must not lose a valid commit
merely because malformed tag counts consumed it as apparent data.

`tests/generate_journal_fixtures.py` independently creates six pending journals
with debugfs: plain, checksum v2/v3, an unfinished tail after a committed prefix,
revocation, and later reuse of a revoked block. All six passed exact recovered
block comparison, unchanged repeated recovery and e2fsck. Reports and untouched
fixtures are in `artifacts/journal-debugfs-recovery-validated/` and
`artifacts/journal-fixtures-debugfs-tail/`.

`tests/run_linux_journal.py` passed ten isolated VM roundtrips, covering the five
writer profiles at both block sizes. The actual Linux kernel identified itself
in every guest, mounted and recovered our pending log, and checked the file bytes.
It then changed the file's UID/GID, mode and data, committed with fsync, and powered
off without unmounting. The portable core replayed one Linux-authored transaction
in every returned image; full file bytes, ownership, permissions and e2fsck passed.
Only independent image copies were attached. Lab reports are under
`artifacts/ext4-journal/linux-reference/roundtrip-complete/` and
`logs/ext4-journal-linux-roundtrip-complete.log`. Earlier preparation failures
remain separate from this successful run.

This establishes the bounded journal engine, not general read/write filesystem
operations. Namespace and held-inode evidence are recorded separately below;
writable UBC/FSKit coherence and durable platform device barriers still need
implementation and acceptance. Fast-commit development evidence and accepted
external-journal, checksum-v1 and async profiles are described above.
Configured resource bounds are explicit in
[ARCHITECTURE.md](ARCHITECTURE.md). Arbitrary media corruption and physical-device
power-loss protection are not established by the modeled crash tests.

Final source checks passed in `artifacts/checks/journal-validated-*`: four CTest
suites under ASan/UBSan, the freestanding stack check, clang-format, unsigned
arm64e/x86_64 kext builds, and the unsigned universal FSKit app/extension build.
These new kernel and FSKit binaries have compilation evidence only; the mounted
read-only kernel evidence below belongs to the preceding verified reader build.

## Portable mutation evidence

`ext4-write-test` passes ten profiles: block sizes 1 through 32 KiB, indirect
mapping, absent metadata checksums, explicit checksum seed and 128-byte inodes.
It verifies selective UID/GID/mode/time updates, signed/extended date boundaries,
shared hardlink identity, unchanged neighboring inode records, whole-file bytes
after an unaligned three-block overwrite, zero-length writes, stale generation,
read-only and unsupported-operation rejection. Permission transitions and data
share the transaction. It does not authorize a platform operation by itself.

Across those profiles, every allocation/read after mount is injected: the current
overwrite path has 130 allocation points and 200 read points. The commit/finish crash matrix contains
1,260 cuts: 1,236 recover to matching old/new inode and file data, and 24 torn
primary-superblock cases reject with a checksum error. The latter are explicit
fail-closed cases. Reads and further mutations reject on a poisoned instance.
The modeled tests are not evidence of device-specific power-loss protection.

Ten clean exports passed independent file/metadata checks and nonrepairing
e2fsck; source hashes stayed unchanged. Evidence is under
`artifacts/inode-write-exports/`, `artifacts/inode-write-independent-owners/` and
`artifacts/checks/inode-write-expanded-tests.log`. debugfs prints large UID/GID
values as signed decimal in this build; the checker compares their 32-bit values.

Seven selected exports (1/2/4 KiB and indirect/checksum/inode-size variations)
also passed actual Linux mounts, full file comparison, hardlink attributes,
wide owners and signed/extended timestamps. Linux then committed its own changes;
our replayer recovered one Linux-authored transaction in each returned image,
followed by exact file/owner/mode checks and clean e2fsck. The reference kernel
uses 4 KiB pages. Larger-block exports have portable/e2fsprogs evidence and were
not selected for these Linux runs. Lab evidence is under
`artifacts/ext4-journal/linux-reference/file-write-roundtrip/` and
`logs/ext4-file-write-linux-roundtrip.log`.

The reproducible generator now has `--inode128`; the fresh twelve-image set in
`artifacts/fixtures-write-regression/` passes feature and e2fsck checks. Final
checks in `artifacts/checks/inode-write-final-*` pass all five sanitized CTest
suites, the freestanding stack budget, formatter, both unsigned kext builds
without core warnings, and the unsigned universal FSKit build. These adapters
still expose read-only operations and were not runtime-tested in this change.
Security xattrs, concurrent native page-cache ownership and LXNU policy remain
unaccepted; namespace operations have separate evidence below.

## Portable large-write development evidence

`ext4_write_partial` spans bounded transactions while preserving the atomic
`ext4_write` contract. Successful batches contribute to a durable completed prefix;
an uncertain transaction requires recovery and may add one more complete batch.
The admitted xattr change accompanies the first successful batch only. Later
batches retain its result and the same captured permissions and times.

Ten existing format profiles pass large unaligned growth, overwrite, one-time
xattr CREATE/REMOVE, held-unlinked writes, invalid inputs and reserved-space exhaustion.
That checkpoint also verified an explicit limit on zeroing written preallocation;
the subsequent growth batch removes it from the non-atomic APIs.
The workload exceeds the maximum transaction capacity while fitting the ordinary
free pool of each fixture. The previous atomic-write suite also passes unchanged.

Two small-journal profiles pass 370 allocation/read failures with retry of the
remaining suffix. Six of those reads fail during commit preparation and require
recovery of the exact earlier prefix. Their 972 write/flush interruption cases
include 960 recoverable outcomes and 12 torn primary-superblock cases that explicitly
reject with a checksum error before writing. Recoverable outcomes match either the
reported prefix or that prefix plus the next complete transaction, including data,
size, permissions, timestamps and attribute state. These are storage-model results.

All 30 completed growth, overwrite and final-release exports pass independent
debugfs byte/metadata/attribute comparisons, exact free-block/inode accounting and
nonrepairing e2fsck. Original inputs remain unchanged. Evidence is under
`artifacts/checks/partial-write-development-*`,
`artifacts/checks/partial-write-independent-summary.json` and
`artifacts/partial-write-independent/`. The combined large-write/checksum CI run
passes all six jobs and the exact 253-test inventory with no Meson failures or
skips. Its 30 partial-write states pass nonrepairing e2fsck and all independent
comparisons. The run review and downloaded reports are under
`artifacts/checks/partial-crc-ci-36341405091/`. Platform
adapters still expose read-only operations, and concurrent writable owners remain
outside this evidence.

## Growth through written preallocation

Large writes and live truncate growth can prepare a written EOF gap in bounded
transactions. Each preparatory commit retains the old size and attributes; the
final transaction publishes the new data/size and captured attribute transition.
Errors can leave invisible zeros beyond the old EOF, without contributing to the
write's completed byte count. Atomic APIs retain their original capacity limit.

The development batch compiles under sanitizers and the freestanding 2 KiB frame
limit, and passes nine focused tests including the existing write, allocation,
truncate and live-shrink controls. Ten profiles pass growth beyond maximum
transaction capacity, sparse gaps, partial first/last blocks, external xattr
REMOVE/CREATE, unchanged hidden tails and retained unlinked objects. Invalid
timestamps and attribute transitions reject before any preparation writes.

The extent and indirect small-journal cases pass 907 allocation/read failures and
1,404 write/flush cuts. Of those cuts, 1,398 recover the exact old visible file or
the complete requested growth; six torn primary superblocks explicitly reject
before recovery writes. Retried old-size outcomes finish successfully. The write
case exhausts credits only when its external xattr is included, while truncate
requires two preparation commits before final publication. These are modeled
storage results, recorded under `artifacts/checks/growth-development-evidence/`
and `artifacts/checks/growth-development-summary.json`.

All 20 completed states across ten profiles pass independent byte, inode, external
attribute, sparse-map, hidden-tail, namespace and allocation comparisons, and
nonrepairing e2fsck. Inputs and exports remain unchanged. Reports are under
`artifacts/growth-independent/` and
`artifacts/checks/growth-independent-retry2-summary.json`. Earlier attempts stopped
on a command input path and report-directory setup; no filesystem checks failed.
The default Meson profile also configures correctly without the optional indirect
fixture. The full CI regression passes all six jobs and exactly 256 registered
tests, with no missing, duplicate, skipped or failed Meson cases. Its independent
checks again pass all 20 growth states and all 30 partial-write states with
nonrepairing e2fsck. The ten known shared-value reader-only e2fsck exceptions are
recorded separately. Evidence is retained under
`artifacts/checks/growth-ci-36344007575/`. This checkpoint precedes the CRC16 format
package. Writable platform and concurrent-cache acceptance remain separate.

## Portable namespace evidence

`ext4-namespace-test` covers creation, mkdir and hard links across ten ordinary
writable profiles, ten modern orphan-file profiles and six small multi-group
profiles. Cases include nested names and dot/dotdot, hard links to regular files
and symlinks, wide owners, signed/extended timestamps, 255-byte names, directory
growth, generation reuse/wrap and inherited flags. The small profile without
FILETYPE retains metadata checksums and tests correct recognition of checksum
tails in legacy directory records.

The six small filesystems each exhaust 115 available inodes, including 23 new
directories, across eight groups. Hard links remain possible with no free inodes;
create/mkdir return an error without modifying media. Separate group-transition
tests fault both regular-file and directory creation in the next lazy inode group.
Malformed records, dot entries, bitmap checksums/counts/high-water marks, immutable
or indexed parents, stale identities and link limits reject without writes. A
small admitted journal verifies private cancellation on late credit exhaustion.

Every allocation/read after writable mount and target lookup is faulted for the
bounded operations: 2,804 allocation and 2,705 read points across 168 scenarios.
The 25,680 write/flush cuts use three pending-write survival patterns and partial
writes; 25,042 recover consistently and 638 torn-primary cases fail closed.
Recovered resources outside the journal must match
the complete old or new image; a durable commit requires the new image. Deliberate
torn primary-superblock failures remain fail-closed cases, not successful repairs.
The full optimized ASan/UBSan matrix passed 24 suites; all six namespace suites
passed again after the generation/credit and dual-commit-boundary additions.
There are 537 malformed-case passes and nine explicit checksum-absent skips.

Independent namespace checks passed 200 records: 70 ordinary, 70 modern and
60 small-filesystem results. Each of the 168 atomic
records separately verifies uncommitted rollback and committed replay against
e2fsprogs journal-only recovery, with exact names, metadata, link counts, bytes and
accounting. Actual Linux checks passed 95 cases: 32 clean exports and 63 pending
commits, including every operation, directory append, inode-group transitions,
modern orphan files and all six inode-exhaustion formats. Linux required ENOSPC,
reused the sole released inode, then authored new namespace commits. Reverse core
replay, independent journal-only replay and unchanged repeated recovery passed.
The reference Linux kernel has 4 KiB pages; larger blocks have portable and
e2fsprogs evidence only. Generated evidence is under
`artifacts/namespace-final-*/`, `artifacts/checks/namespace-*` and the lab's
`artifacts/ext4-journal/linux-reference/namespace-*` directories.
The selected-Xcode formatter, freestanding stack budget, unsigned arm64e/x86_64
kext builds and unsigned universal FSKit app/extension build also pass. These new
platform binaries have compilation evidence only.

This create/mkdir/link checkpoint does not establish indexed-directory mutation
or rename. Symlink creation and removal/held-inode lifetime have separate evidence
below. Both platform adapters remain read-only.
The namespace API takes caller-admitted attributes and does not implement native
authorization, LXNU policy, writable page-cache ownership or concurrent mutation.

## Portable symlink evidence

`ext4-namespace-test --symlinks` adds targets of 1, 59, 60, 61 and block-size-minus-one
bytes, plus an opaque non-UTF-8 target. It verifies the inline/data-block boundary,
zeroed terminator and tail, hardlink identity, owners/timestamps, readonly remount
and exact partial reads. Invalid targets and stale or duplicate names leave media
and caller output unchanged. Data-space exclusion rejects mapped targets while
allowing inline creation without a data allocation. Inode exhaustion and small
journal cancellation also cover symlinks. Generation reuse and parent flag
inheritance distinguish symbolic links from regular files and directories.

The full optimized ASan/UBSan matrix passed 28 suites. Review then found that the
reader admitted an inline target of exactly 60 bytes, leaving no terminator space.
The guard now rejects that length; tests use valid checksums for the 59-byte
positive boundary and invalid 60/61-byte records. All twelve reader, malformed
and namespace suites passed after this change. Reports are
`artifacts/symlink-final-tests.xml`, `artifacts/symlink-decoder-tests.xml` and
`artifacts/checks/symlink-*-ctest*.log`.

The 174 fault scenarios cover 3,059 allocation points, 2,886 reads and 29,196
write/flush cuts. Of those cuts, 28,494 recover to the required complete state;
702 torn-primary-superblock cases fail closed. The same totals pass before and
after the decoder guard. Independent checks pass 200 records across 26 profiles,
including both committed replay and uncommitted rollback for all 174 atomic
records. Target bytes, terminator space, inode/block accounting and metadata
are checked independently with e2fsprogs, including journal-only recovery.

The reference Linux kernel passes 158 roundtrips: 20 clean exports and 138 pending
transactions. Cases cover inline, mapped and maximum targets, parent-directory
growth and inode-group transitions. Linux checks exact and truncated readlink
bytes and hardlink identity, then commits further namespace changes; reverse
core recovery preserves the links and passes independent checking. A separate
two-case smoke also passes. The reference kernel has 4 KiB pages, so larger block
profiles have portable and e2fsprogs evidence only.

Independent reports are under `artifacts/symlink-final-*-independent/`, Linux
reports under the lab's `artifacts/ext4-journal/linux-reference/symlink-linux-*`,
and the combined evidence is `artifacts/checks/symlink-acceptance.json`.
Both unsigned kext architectures and the unsigned FSKit app/extension build.
The arm64e kext passes all 26 clean exports on the identified custom kernel in
the dedicated macOS VM. Each profile checks six targets and their hardlink
aliases with five buffer sizes and four concurrent workers: 199,680 readlink
calls in total. Owners, modes, link counts and storage accounting match, all
220 guest commands pass, and source/device bytes remain unchanged. Kernel,
boot-session and loaded-module identities remain constant across the run.
Normal unload, reload and final unload also pass; the VM is then shut down.
Native reports are under the lab's `artifacts/ext4-symlink-kext/` and
`logs/ext4-symlink-kext/`. This verifies reading core-created links through XNU;
both platform adapters remain read-only and FSKit runtime remains pending signing.

## Allocation and growth evidence

`ext4-write-test --allocation` passes ten ordinary write profiles and four
independently generated unwritten profiles. It grows an empty/preallocated file
through 350 permuted sparse writes, checks all bytes and inode/free-space accounting,
and exercises extent root, leaf and parent splits. It tests direct, single, double
and triple-indirect transitions and the legacy logical-address limit. A multi-group
fixture reaches zero free blocks, initializing two lazy block groups along the way.
Late credit exhaustion, reserved-space exclusion, malformed bitmap checksums,
forged free system blocks and counter mismatches reject before resource writes.
Written allocations beyond the previous EOF must also expose only zeroed gaps.

`generate_allocation_fixtures.py` uses debugfs to allocate 128 unwritten blocks in
each of four copies (1/4 KiB, absent checksums and explicit checksum seed). It maps
each block independently, replaces its backing bytes with nonzero data, and
requires clean e2fsck. The core must preserve unwritten zero semantics during
partial conversion and later tree growth. `check_allocation.py` independently
checks full sparse-file contents, unchanged neighboring file data, mapping/accounting
and e2fsck in all fourteen exports. Source hashes remain unchanged.

The growth and mapping-promotion suites inject 476 allocation and 336 read failures.
Across 3,912 commit/finish cuts, 3,832 recover to the complete old or new resource
outside the journal. The other 80 are deliberately torn checksummed primary-superblock
cases that reject recovery without writes; they are not successful repairs.
The compared resource includes data, inode, extent/indirect nodes, bitmaps, group
descriptors and primary accounting. This remains a modeled storage contract.

Eleven selected exports (seven ordinary 1/2/4 KiB and feature/inode variations,
plus four unwritten cases) also passed real Linux mounts and complete grown-file byte
checks. Linux then allocated past EOF and committed with fsync before powering off
without unmounting. The portable replayer recovered one Linux-authored transaction
with five replayed blocks in every returned image. Exact extended contents,
owners, mode and e2fsck passed. These runs use the identified 4 KiB-page reference
kernel; larger-block profiles have portable/e2fsprogs evidence only.

Evidence lives under `artifacts/allocation-final-exports/`,
`artifacts/allocation-final-independent/`, `artifacts/allocation-unwritten-fixtures/`
and `artifacts/checks/allocation-final-*`. Linux evidence is in the lab under
`artifacts/ext4-journal/linux-reference/allocation-unwritten-roundtrip/` and
`artifacts/ext4-journal/linux-reference/allocation-final-roundtrip/`, with matching
`logs/ext4-allocation-*-roundtrip.log` files. All seven configured sanitized
CTest suites, formatting and the freestanding stack check pass. Both unsigned
kext architectures and universal FSKit build successfully; these new binaries
have compilation evidence only and still expose read-only operations.

Writes remain bounded atomic operations. Large requests, concurrent mapping
ownership, inode/directory allocation, online orphan lifetime, policy
for reserved space, platform durability and writable cache integration remain
required work. The allocation tests do not complete the filesystem acceptance matrix.

## Truncate and freeing evidence

`ext4-write-test --truncate` passes ten ordinary write profiles and four independently
generated unwritten profiles. It shrinks a fragmented file at partial/aligned/zero
boundaries, retains different branches, collapses extent roots, frees indirect paths,
grows again without exposing removed bytes, and reuses released blocks. Separate cases
cover unwritten preallocation, hardlink identity, maximum sparse size, invalid admission,
stale generation and late credit exhaustion. Repaired-checksum corruption cases reject
duplicated data blocks, data/mapping aliases and protected metadata. Inode block-count
mismatches also reject before writes. The allocation suite additionally fills a whole
multi-group image, truncates it and verifies zeroed reuse after ENOSPC; all direct through
triple-indirect boundaries are checked again while their paths are removed.

Three operations per profile (partial shrink/tree collapse, complete removal, and
exposure of already allocated bytes past EOF) inject every allocation/read failure:
708 allocation and 1,161 read failures. Across 6,396 write/flush cuts, 6,268 recover
to the complete old or new resource outside the journal. The remaining 128 deliberately
torn checksummed primary-superblock cases fail closed without recovery writes.
These are modeled errors, not successful repairs or physical-device power-loss tests.

All fourteen final exports and forty-two intermediate truncate states pass independent
contents/mapping checks and e2fsck in `artifacts/truncate-final-independent/`. Final
images also have exactly one data block and no leaked mapping nodes. The same fourteen
final image hashes match the earlier `artifacts/truncate-independent/` report used
for Linux verification. Eleven selected final exports also pass
real Linux mounts and exact contents/allocated-block checks. Linux then shrinks, grows
and reallocates the file, commits with fsync and powers off without unmounting. The core
replays one Linux-authored transaction (four or five blocks), preserving exact bytes,
owners and mode; e2fsck passes. Linux reports and console evidence are in the lab under
`artifacts/ext4-journal/linux-reference/truncate-roundtrip/`, with
`logs/ext4-truncate-roundtrip.log`. The reference kernel uses 4 KiB pages; 8/16/32 KiB
filesystem profiles retain portable/e2fsprogs evidence only.

The nine configured sanitized CTest suites pass, including the optimized freestanding
2 KiB frame check. Logs and JUnit output are under `artifacts/checks/truncate-*`.
The shared core also compiles in both unsigned kext architectures and universal FSKit.
Those new adapter binaries remain read-only and have compilation evidence only.
These tests exercise `ext4_truncate_atomic`, where changed mapping nodes and other
metadata must fit one transaction. The separate live shrink contract is described
below. Native open-unlinked lifetime and UBC/FSKit resize concurrency remain
unaccepted; serialized core holds have separate evidence below.

## Legacy orphan recovery evidence

`ext4-orphan-test` checks linked regular-file truncates, hardlink identity,
retained data and partial-block zeroing. Sixteen malformed chain/inode cases
cover reserved/free/out-of-range numbers, cycles, checksums, unsupported flags,
xattrs, wrong block accounting and protected data pointers. Stale primary
summaries are tested in both directions; a nonempty orphan list is recovered
even without the RECOVER flag. The indirect stress case allocates 257 sparse
leaves, exceeds atomic truncate's credit capacity without resource writes,
then releases the complete map in 17 cleanup transactions. Across ten linked
profiles, 159 malformed cases pass and one checksum case is explicitly skipped
on the checksum-absent profile. There are 1,192 allocation and 633 read-error
injections, and 4,770 crash cuts: 4,694 recover and 76 torn-super cases fail closed.

All ten ordinary linked-truncate exports independently match e2fsck recovery and
pass nonrepairing e2fsck, including the large indirect result. Reports are under
`artifacts/orphan-linked-independent/`, with exported images under
`artifacts/orphan-linked-exports/`. The checker compares POSIX recovery bytes
with the modeled result and verifies that linked inodes remain allocated.

The Linux fixture generator keeps six unlinked objects open: regular and sparse
files, a directory, short and long symlinks, and a FIFO. Seven profiles cover
1/2/4 KiB blocks, indirect mapping, absent/seeded checksums and 128-byte inodes.
All 42 orphan entries are reclaimed with unchanged live contents, correct free
counts and nonrepairing e2fsck. Repeated recovery makes no changes. e2fsck may
index an unrelated directory during its oracle run; the checker measures that
allocation separately and also requires the core result to regain the exact
pre-Linux baseline's free-block and free-inode counts.

The seven Linux-authored images pass 1,092 allocation and 1,758 read-error cases,
with repeated journal mapping sampled as described in TESTING.md. Across 6,090
write/flush cuts, 6,018 recover to the complete expected resource outside the
journal; 72 deliberately torn checksummed primary-superblock cases fail closed
without recovery writes. The source images remain unchanged. Generation reports
are in the lab's `artifacts/ext4-journal/linux-reference/orphan-fixtures/`;
independent results are in `artifacts/orphan-independent-accounting/`, and fault
logs are in `artifacts/checks/orphan-final-linux-faults.log`.

Linux subsequently remounted each of the seven core-cleaned images and allocated
and unlinked another six objects. All returned images passed portable recovery,
live-byte checks and e2fsck again. Lab evidence is under
`artifacts/ext4-journal/linux-reference/orphan-reuse/` and
`logs/ext4-orphan-reuse.log`; final checks are in
`artifacts/orphan-reuse-independent/`.

All ten sanitized CTest suites, the freestanding 2 KiB frame budget, style and
Python checks pass. Both unsigned kext architectures and universal FSKit compile
with the shared core; these binaries have compilation evidence only. Complete
logs and the JUnit report are under `artifacts/checks/orphan-final-*` and
`artifacts/orphan-tests.xml`.

This accepts offline legacy-list cleanup for the tested inode profiles. Modern
orphan-file and serialized held-inode evidence is recorded below; orphaned ACL/xattr
inodes, native open-unlinked lifetime or writable
platform cache/concurrency behavior remain unaccepted. Both adapters continue to expose read-only
operations.

## Live truncate evidence

`ext4-orphan-test --live` checks the public `ext4_truncate` operation against ten
ordinary profiles and four independently allocated unwritten profiles. The target
size is a block plus seven bytes, preserving a written prefix or inaccessible
nonzero unwritten backing data. A separate `--large` case builds 257 sparse indirect
leaves, reaches triple-indirect mapping and completes shrink in 17 transactions.
The atomic API and its no-write credit-exhaustion contract remain separately tested.

The live suites inject every allocation and read after writable mount/lookup,
including intent cleanup and final inode refresh. Every write/flush cut runs with
three volatile-write survival patterns and partial writes. Once the first commit
is known durable, recovery must produce the complete new resource outside the
journal; earlier cuts may also retain the original resource. Errors leave the
caller's output unchanged. Any post-intent error must poison all further access.
Torn checksummed primary superblocks remain explicit no-write rejection cases.
Across fifteen profiles, 975 allocation faults and 1,445 read faults pass. Of
10,692 write/flush cuts, 10,572 recover successfully and 120 deliberately torn
primary-superblock cases fail closed. The large-map case alone checks 425
allocation faults, 928 read faults and 5,238 crash cuts. All thirteen Debug
ASan/UBSan CTest suites pass; logs are under `artifacts/checks/live-final-*`, with
`artifacts/live-tests.xml` and the full `live-final-ctest-full.log` retaining output.
The same large matrix also passes in sanitized `RelWithDebInfo`: about 166 seconds
versus 455 seconds in Debug on this host, with identical fault counts. CI selects
that configuration and two CTest workers; this is a test-execution measurement,
not a filesystem throughput benchmark.
The other twelve suites pass in the optimized sanitized configuration as well,
with the same fifteen live fault-count profiles as Debug. Logs and JUnit output
are under `artifacts/checks/live-optimized-*` and `artifacts/live-optimized-tests.xml`.
Both unsigned kext architectures and universal FSKit compile with this core;
these builds remain read-only and have no new installed-runtime acceptance.

All fourteen completed/interrupted export pairs and the large-map pair pass
`check_resize.py`: nonrepairing e2fsck, exact file contents and selected attributes,
preserved inode identity/owners/links, retained physical mappings, released block
counts, separate e2fsck recovery and unchanged repeat recovery. The final build's
45 before/pending/after images match the earlier independently checked exports.
Reports are in `artifacts/live-final-independent/`,
`artifacts/live-final-large-independent/` and `artifacts/checks/live-export-comparison.json`.

Twenty-four Linux cases pass: completed and interrupted images for seven ordinary,
four unwritten and one large indirect profile with 1/2/4 KiB blocks. Linux verifies
size, allocation, mode and all retained bytes, grows/writes each file, commits and
stops without unmounting. Portable replay then passes full contents, owners/mode
and e2fsck. Clean inputs retain their exact captured timestamps. The identified
Linux kernel refreshes mtime/ctime while completing linked orphan truncation;
pending-input checks require equal refreshed times within the measured mount
interval, allowing legacy inode second precision. Core and e2fsck recovery retain
the captured timestamps. This difference is checked rather than silently ignored.
Linux reports and console evidence are in the lab's
`artifacts/ext4-journal/linux-reference/live-final-roundtrip/` and
`logs/ext4-live-final-roundtrip.log`.

Live growth retains the atomic credit limit when many written allocations beyond
the previous EOF need zeroing. Very small journals retain the atomic shrink limit
rather than admitting an intent they cannot complete. Mapping validation and all
other documented format/resource bounds still apply. Native cache ownership,
native open-unlinked lifetime and platform writes remain unaccepted.

## Modern orphan-file evidence

`generate_orphan_file_fixtures.py` uses independent `tune2fs` to add an orphan file
to ten clean profiles: 1 through 32 KiB blocks, indirect mapping, absent metadata
checksums, an explicit checksum seed and 128-byte inodes. Separate indirect images
have 17 and 512 file blocks, exercising external mapping nodes and the supported
size bound. The generator verifies every physical mapping, tail and empty slot,
exact feature changes, nonrepairing e2fsck and unchanged source hashes.

The five orphan-file CTest suites cover linked entries at opposite ends of the
file, mixed legacy/file ownership, indirect file mapping, the size bound and live
truncate on modern volumes. Malformed cases cover feature/flag inconsistencies,
private-inode identity and allocation, size/link/type/flag constraints, holes,
aliases and protected mappings, block accounting, tail magic, checksum binding to
generation and physical address, invalid entries and duplicates within or between
representations. Across eleven malformed profiles, 381 cases pass and four
checksum-specific cases are explicitly skipped on the checksum-absent image.
Private file data, mapping nodes and the inode reject ordinary writes and freeing.
An empty file with ORPHAN_PRESENT alone still requires explicit recovery.

Thirteen cleanup profiles inject 2,003 allocation and 1,338 read errors. Repeated
journal-mapping allocations/reads are sampled as described in [TESTING.md](TESTING.md);
the unsampled paths contain 33,008 allocations and 17,042 reads. Every write/flush
cut is exercised: 8,895 of 9,042 recover to the complete resource outside the
journal, and 147 torn checksummed primary-superblock cases fail closed. The ten
modern live-truncate profiles additionally check 378 allocation and 357 read
faults, with 3,710 successful recoveries and 64 fail-closed cases across 3,774 cuts.
These are modeled persistence/error contracts, not physical-device power-loss tests.

All fourteen pending/clean cleanup export pairs and ten live-truncate profiles
pass independent `debugfs`, e2fsck and repeated-recovery checks. The checker compares
private-file identity, mappings and full cleared bytes with a separate e2fsck
oracle. Three large indirect cleanup exports also pass nonrepairing e2fsck.
Evidence is under `artifacts/orphan-file*-independent/`, with matching export
directories; fixture reports are in `artifacts/orphan-file-fixtures/`.

The identified Linux reference kernel generated seven modern pending images with
six open-unlinked objects each: regular/sparse files, a directory, short/long
symlinks and a FIFO. Portable recovery transfers and releases all six entries,
preserves unrelated live bytes and matches independent e2fsck accounting and
private-file contents. The seven actual Linux images also pass 1,471 allocation
and 2,120 read-error injections, with repeated journal mapping sampled. Across
9,924 write/flush cuts, 9,798 recover and 126 torn-super cases fail closed. Linux
then remounted each cleaned image and created/unlinked six new objects; all seven
returned images passed portable recovery and independent checking again.
This verifies Linux-created state and Linux reuse of cleaned state; it does not
claim a Linux mount at every modeled interruption of the transfer transaction.

Linux reports and console logs are in the lab under
`artifacts/ext4-journal/linux-reference/orphan-file-linux-pending/`,
`artifacts/ext4-journal/linux-reference/orphan-file-reuse/` and
`logs/ext4-orphan-file-*.log`. Core/oracle reports are in
`artifacts/orphan-file-linux-independent-baseline/` and
`artifacts/orphan-file-reuse-independent/`. The reference kernel has 4 KiB pages;
8/16/32 KiB block profiles have portable/e2fsprogs evidence only.

All eighteen optimized ASan/UBSan CTest suites pass, including the freestanding
2 KiB frame check. After the final zero-private-inode guard, the five affected
orphan-file suites pass again. Logs and JUnit reports are under
`artifacts/checks/orphan-file-final-*`, `artifacts/checks/orphan-file-guard-*`,
`artifacts/orphan-file-final-tests.xml` and `artifacts/orphan-file-guard-tests.xml`.
Both unsigned kext architectures and universal FSKit compile with the final core;
style, Python and CI workflow syntax checks pass. These adapters remain read-only
and have compilation evidence only for this change. Concurrent orphan-file insertion,
orphan-file growth, ACL/xattr cleanup, native object lifetime and writable cache/durability
contracts remain required work.

The preceding live-truncate CI run exhausted runner storage after its CTest suites
and ten ordinary independent live profiles passed. The workflow now removes each
stage's generated images/dumps only after successful independent validation,
retaining JSON reports, logs, hashes and JUnit output. This corrects CI storage
use; that failed run is not counted as completed CI acceptance.

## Portable removal and held-inode evidence

`ext4-removal-test` covers unlink/rmdir on ten ordinary writable profiles, ten
modern orphan-file profiles and six small multi-group profiles. It checks last
and nonlast links, nested/nonempty directories, inline/mapped symlinks, exact
accounting, directory-record coalescing, a first entry in a later block, empty-slot
reuse and reclamation of an empty multiblock directory. Held unlinked files retain
read/write/setattr/truncate access, while ordinary lookup fails. Held directories
reject new children and held symlinks retain their target bytes. Shared references,
nonhead orphan release, linked truncate while other orphans remain, delayed inode
reuse, generation advance and unmount without implicit writes are checked.

The five namespace fault operations and three held-object sequences inject every
allocation/read after their prepared mount/lookup or committed unlink: 7,356
allocation and 7,406 read failures across 208 operation/profile combinations.
The held sequences release the head, release a nonhead inode, or truncate a
nonhead held inode before releasing both objects. Every write/flush cut uses all
three pending-write survival patterns and partial writes. Of 55,980 cuts, 54,926
recover to the required complete resource outside the journal; 1,054 torn
checksummed primary-superblock cases fail closed. A committed deletion must finish
during recovery even if last release fails before its first write. Failed final
release consumes the hold and poisons further access without leaking allocations.
These modeled storage tests do not establish physical-device power-loss behavior.

Malformed guards pass 204 cases, with three checksum-absent skips and one
FILETYPE-absent skip. Late invalid records, duplicate names, wrong dotdot, immutable
objects, protected target mappings and inconsistent entry types reject without
mutation. Invalid arguments, stale identities, readonly/type errors and journals
too small for restartable cleanup also leave media and output unchanged. A real
indexed directory separately rejected mutation at this checkpoint; the later
indexed writer has its own evidence below.

All four removal CTest suites pass under optimized ASan/UBSan. The earlier
29-suite regression also passes with the same core, covering the reader, writers,
namespace creation, truncate and both orphan representations. Evidence is under
`artifacts/checks/removal-*`, `artifacts/removal-initial-regression.xml` and
`artifacts/removal-lifetime-final.xml`. The published removal checkpoint also
passes all 32 CI suites across three jobs, including independent namespace,
symlink and removal checks. Downloaded CI evidence is retained under
`artifacts/checks/removal-ci-artifacts/`; the reviewed manifest and acceptance
report are in `artifacts/checks/`.

Independent checks pass 156 records: 26 clean operation sequences and 130 atomic
cases. Each atomic case checks both uncommitted rollback and durable-commit replay
against e2fsprogs journal-only recovery, with exact names, remaining alias identity,
contents, metadata, released space, nonrepairing e2fsck and idempotent recovery.
Reports are in `artifacts/removal-guards-*-independent/`. All 546 final regenerated
exports are byte-identical to these verified inputs; their comparison is recorded
in `artifacts/checks/removal-export-comparison.json`.

The actual Linux reference kernel passes 120 cases with 1/2/4 KiB blocks: 20 clean
sequences and 100 pending deletion commits. It verifies the namespace and retained
hardlink, creates further objects, and leaves a committed Linux journal. Portable
and independent reverse replay agree, preserving the remaining alias and never
restoring a removed name. Reports and console evidence are in the lab under
`artifacts/ext4-journal/linux-reference/removal-linux-*` and
`logs/ext4-removal-linux-*`; the separate two-case smoke is not included in 120.
Larger block sizes have portable/e2fsprogs evidence only.

Both unsigned kext architectures and the universal FSKit app/embedded extension
build with this core; frame/style, Python and workflow checks pass. Product
inspection includes FSKit debug dylibs as well as their launcher executables and
is recorded in `artifacts/checks/removal-platform-builds.json`. These new binaries
have compilation evidence only. Both adapters remain read-only; native object
lifetime, cache coherence, authorization, writable mounts and LXNU policy remain
unaccepted. Indexed mutation is covered separately below; ACL/xattr handling
remains required work.

## Portable rename evidence

`ext4-rename-test` covers ten ordinary writable profiles, ten modern orphan-file
profiles and six small multi-group profiles. Its 160 functional sequences per
profile cover both parent relationships and creation orders, absent destinations,
replacement, held victims, populated directory exchange, all file/symlink type
combinations and mixed-type exchange. All 4,160 sequences pass with exact inode
identity, contents, ctimes, parent times/link counts, dotdot and released space.
NOREPLACE, same-inode aliases, stale identities, nonempty directories, read-only
instances and held source lifetime have separate checks. Reserving all remaining
free blocks rejects a rename that needs parent growth without changing media or
output; a shorter name still succeeds by reusing existing record space. This is
reserved-space exhaustion, not a completely allocated bitmap.

Eleven fault sequences include parent growth, populated directory moves/exchange,
inline/mapped symlinks, a retained hardlink and multibatch victim reclamation with
or without a hold. Across 286 operation/profile combinations, every allocation
and read after the prepared mount/lookup/hold is injected: 11,689 allocation and
11,083 read failures. Every write/flush cut uses three pending-write survival
patterns and partial writes. Of 57,312 cuts, 56,122 recover to the complete required
resource outside the journal, while 1,190 deliberately torn checksummed primary
superblocks reject recovery without writes. Once the namespace commit is durable,
recovery must finish the new namespace and victim reclamation. These are modeled
persistence tests, not physical-device power-loss tests.

Malformed guards pass 335 cases, with three checksum-absent skips. They cover
ancestry cycles, invalid parent types, wrong dotdot, late malformed records,
duplicate queried names, invalid parent link counts, immutable parents and directory
checksums. At this checkpoint an actual indexed directory separately rejected
four mutation paths without writes. All three rename fault suites and the earlier 32-suite regression
pass under optimized ASan/UBSan. The expanded functional/indexed matrix passes
separately with the final tests. Reports are `artifacts/rename-fault1-regression.xml`,
`artifacts/rename-existing-regression.xml` and `artifacts/checks/rename-final-*`.

Independent checks pass all 286 atomic cases. Each compares durable-commit replay
and uncommitted rollback with separate e2fsprogs journal-only recovery, including
exact topology, identities, data, attributes, allocation and idempotent recovery.
Nonrepairing e2fsck passes for the completed and recovered states. Reports are in
`artifacts/rename-final-*-independent/`, with 1,170 protected source exports in the
corresponding export directories.

The actual Linux reference kernel passes 240 cases with 1/2/4 KiB blocks: 20 clean
exchanged-directory images and all 220 pending operation/profile combinations.
It verifies the retained namespace and objects, creates more objects and leaves a
committed Linux journal. Portable reverse replay and independent e2fsprogs agree,
preserving renamed data, attributes, dotdot and names. Reports and console evidence
are in the lab under `artifacts/ext4-journal/linux-reference/rename-linux-*` and
`logs/ext4-rename-linux-*`; the separate two-case smoke is excluded from 240.
Larger block profiles have portable/e2fsprogs evidence only.

Both unsigned kext architectures and universal FSKit compile with this core.
The final FSKit extension remains read-only and does not link the unused namespace
object; the universal core library contains the new API. Platform build evidence
is in `artifacts/checks/rename-platform-builds.json`. No new mounted native tests
are claimed for this change. The owner must serialize core operations; native
rename locking, authorization, cache coherence, writable adapters, whiteouts and
ACL/xattr policy remain unaccepted. Later indexed mutation has separate evidence below.

The initial rename CI run timed out the two aggregated ten-image suites at 900
seconds; the small-format and indexed-rejection suites passed. That run is not
accepted as full CI validation. The test runner schedules each image separately with
the same complete functional/fault workload, so enabling more profiles does not
consume one shared per-test deadline. The failed run's output remains under
`artifacts/checks/rename-ci-artifacts/` and the job log in `artifacts/checks/`.

The follow-up CI run passed all 27 per-profile rename tests and all 286 independent
rename cases, including both journal outcomes. Core and orphan-file jobs also
passed. The overall run still failed: the aggregated ordinary removal test reached
its 900-second deadline, with 13 of 14 namespace tests passing. No assertion or
sanitizer failure appeared in that timeout output. Its logs and reviewed reports
are under `artifacts/checks/rename-profile-ci-*`. Removal is now scheduled per
image in its own CI job as well; the later indexed CI run below passes that
scheduling change together with all earlier suites.

## Indexed namespace development evidence

`generate_index_fixtures.py` independently builds 20 profiles with all six hash
variants, 1 through 32 KiB blocks, shallow and internal-node indexes, indirect
mapping, missing checksums or FILETYPE, 128-byte inodes, explicit checksum seeds,
and shallow/deep indexes combined with a modern orphan file.
Every source passes nonrepairing e2fsck and explicit feature/tree checks. The hash
implementation agrees with 1,608 independent numeric-version debugfs queries,
including high-byte names, length boundaries and four seeds.

The 76-test sanitized matrix passes graph validation, functional writes,
split faults, collisions, compaction and hash guards. Functional tests add 11,300
NAME_MAX byte names across the 20 profiles, forcing leaf splits, root growth and internal-node splits. They
also cover creation, symlinks, cross-parent rename/exchange, indexed dotdot changes,
empty indexed removal and retained inode lifetime. Independent e2fsprogs inspection
checks exact name-to-inode mappings, metadata, link counts, symlink bytes, tree
shape and accounting; all 20 outputs pass e2fsck. The separate 59-test regression
of earlier behavior also passes with the same core.

The fault matrix covers 47 leaf/root/node transitions: 6,048 allocation failures,
6,966 read failures and 8,562 write/flush cuts. Of those cuts, 8,313 recover to the
required old/new image outside the journal; 249 deliberately torn primary
superblocks fail closed. Thirteen inapplicable transition/profile combinations are
reported separately. An additional 141 checks reject checksummed structural changes
between reads without writes. Independent replay of 47 committed/uncommitted pairs
agrees with the core, including exact directory bytes, mappings, objects, accounting
and repeat-recovery idempotence. All 188 split exports retain their hashes.

Fifteen 1 KiB profiles force real equal-hash names into distinct leaves and target
inodes. Lookup/rename/removal preserves the remaining continuation after its leading
leaf becomes empty. Fragmented record gaps compact without allocating blocks or
growing the directory. All 45 resulting collision/deletion/compaction images pass
independent exact-name, inode, accounting and e2fsck checks. Ninety-four separate
capacity guards cancel leaf/root/node splits with four journal credits or all
remaining blocks reserved, without writes, changed output or leaked allocation.
This reserved-pool test does not claim a completely allocated block bitmap.

One additional independently generated 64 MiB image has 46,122 directory entries,
a full 123-entry root and 122 full internal nodes. Its test rejects a split that
would require another index level without changing any device byte. A shorter
name still reuses leaf space; all 46,123 resulting names, unrelated metadata, data
and unchanged allocation pass independent inspection and e2fsck. This fixture
does not enable LARGEDIR and retains that tested rejection boundary. The later
large-directory batch separately negotiates the extra index level.

The real Linux kernel passes 106 cases across the 17 profiles compatible with its
4 KiB pages: 62 clean functional/collision/compaction states and all 44 committed
split states. It checks exact byte-name lookup/readdir and retained objects, then
creates, links and renames new objects inside the indexed parent. Core and e2fsprogs
reverse replay agree on the Linux-authored journal, with exact retained names,
attributes, data and accounting. The changed namespace harness also passes an
earlier linear rename control. Larger block profiles and the full-root capacity
fixture have portable/e2fsprogs evidence, not Linux runtime coverage.

Evidence is in `artifacts/index-expanded-tests.xml`,
`artifacts/index-root-capacity.xml`, `artifacts/index-existing-regression.xml`,
`artifacts/index-*-independent/` and `artifacts/checks/index-*.json`. Linux reports
and actual kernel console markers are in the lab's
`artifacts/ext4-journal/linux-reference/index-linux-*` directories. All six published
CI jobs pass: 159 CTest tests with no failed or skipped tests, plus the independent
hash, collision, functional, split, edge and capacity checks. Explicit applicability
skips within tests remain separate from their CTest result. Downloaded reports and
reviewed artifact hashes are in `artifacts/checks/index-ci-manifest.json`.
These results establish the bounded portable
profile; large-volume performance, automatic indexing of linear directories,
writable adapters, native concurrency and policy remain separate requirements.

## Portable indexed lookup evidence

The public lookup path follows checked HTree ranges and collision continuations
instead of enumerating the entire indexed directory. Its directory buffers are
bounded independently of the number of entries. Ordinary linear lookup reads
each directory block once. At this checkpoint enumeration retained its earlier
per-entry cost; the subsequent read-path batch is recorded below.

Twenty format profiles and the full-root capacity image pass 21 targeted sanitized
suites. They perform 107,840 successful name-to-inode comparisons against independent
debugfs expectations under read-only and exclusive writable owners. All 298 lookup
fault sweeps pass: 1,129 allocation failures and 835 read failures preserve output,
allocation balance and device bytes. The 1,284 structure checks include checksum,
pointer, alias, record and hash-range damage, unsupported headers, and the valid
reserved checksum-tail field. Another 312 checks are explicitly inapplicable
because a node level or checksum is absent. Explicit unsigned hash versions without
superblock signedness flags are exercised on four profiles; the other 17 are
reported separately. Missing signedness retains exact-byte linear lookup, and
conflicting flags reject lookup. Invalid argument checks run under both owners.

Three deep profiles repartition the independently verified collision names across
different internal nodes. Both names retain distinct inode identities; the second
remains reachable after deletion empties the leading leaf. All nine collision,
retained and compaction exports pass independent topology, namespace, accounting
and nonrepairing e2fsck checks. Six actual Linux boots verify the collision and
retained states, commit additional indexed operations, and leave journals that
the core and separate e2fsprogs replay agree on. Original byte names, data and
metadata survive, and repeated core recovery is unchanged. The independently
counted oracle-only primary-summary lag uses the namespace contract documented
in [the portable test suite](TESTING.md#portable-suites); the core results have
correct primary accounting.

The optimized freestanding build stays within the 2 KiB frame limit; formatting,
Python syntax and Clang analysis of the four changed translation units pass with
no diagnostics. Callback measurements on the 46,122-entry image reduce an absent
lookup from 61,618 reads and 107,741 allocations to three reads and four allocations.
These are resource callback counts, not physical I/O or throughput. Timing, broader
scale, enumeration optimization and native concurrency remain unaccepted.

Evidence is in `artifacts/checks/indexed-lookup-targeted-review.json`,
`artifacts/checks/indexed-lookup-edges-summary.json`,
`artifacts/indexed-lookup-linux/report.json`, and the compiler, source and input
reports under `artifacts/checks/indexed-lookup-*`. The complete regression for this
implementation passes all 248 registered tests under ASan/UBSan, including the
20 indexed fault suites. Meson reports zero failures or skipped tests; embedded
format-applicability notices remain separately recorded. The compiled sources
match the Git snapshot identified in `artifacts/checks/indexed-lookup-acceptance.json`.
Later read-path development is not covered by that completed run.

## Portable read-range and streamed-enumeration evidence

Reads combine consecutive physical blocks and sparse ranges, reusing one mapping
buffer through each call. The new directory visitor validates each complete block
before publishing entries and supports accept, stop and resume without persistent
directory state. The single-entry compatibility API retains its external contract.
Native adapters have not yet adopted the visitor API.

The batch passes 24 focused tests under ASan/UBSan. Real-image reads compare full
and unaligned contents and inject every observed read/allocation failure. Eight
modeled configurations cover 1, 4, 16 and 64 KiB blocks, with and without metadata
checksums: initialized/unwritten extents, ancestor boundaries, sparse subtrees,
direct through triple-indirect mapping, EOF preallocation and the 32-bit logical
block limit. Returned mappings are compared with explicit physical-block vectors.

Twenty-one directory profiles stream 107,840 independently expected names across
read-only and exclusive writable owners. They pass 2,132 iteration fault cases,
42 argument/late-record-corruption/read-only-reentry suites and 21 create/unlink
refresh suites. Small directories sweep every failure position; large directories
sample first, middle and last positions. Stop/resume preserves accepted cookies,
and an invalid later record prevents every entry in its block from being delivered.

Fourteen warm-cache benchmark pairs compare identical work against the previous
implementation, using optimized unsanitized core/POSIX builds, one warmup and 31
measured repeats. All pairs preserve the same data/entry digests, write no device
bytes and balance allocations. On the 46,122-entry directory, the median changes
from 251.692 to 75.986 ms, callbacks from 61,618 to 15,497 and allocations from
107,741 to one. The 4 KiB sparse-file workload changes from 7.216 to 0.368 ms and
524 to 37 read callbacks. These measurements cover warm image reads only; cold
device I/O, writes, concurrency and mounted-filesystem performance remain pending.

Evidence is in `artifacts/checks/read-path-batch-targeted-retry1-summary.json` and
`artifacts/checks/read-path-benchmark-results/`. All six jobs in the batch's CI run
pass the exact 249-test inventory, with no Meson failures or skips. Independent
reports contain no failed states; declared reader-only compatibility cases and
embedded format-applicability notices remain separate. Downloaded artifacts and
the bounded review are under `artifacts/checks/read-path-ci-36339470208/`. This
accepts the read-path snapshot, independently of later write/checksum development.

## Portable checksum execution evidence

The portable CRC32C uses a 1 KiB read-only byte-remainder table instead of bitwise
division for every input byte. Six focused sanitized tests pass: the dedicated
checksum test, the reader, malformed images, journal durability, large-write
functionality and the 4 KiB partial-write fault matrix. The dedicated test compares
all 256 remainders and 3,648 seeded/aligned/range/stream cases through 64 KiB with
bit-serial polynomial division and a known CRC32C check value.

All 14 warm-cache benchmark pairs preserve identical work, digests, callback and
allocation counts, with no writes or leaked allocations. Both builds use the
streamed/range read APIs, isolating the checksum change. The large-directory
median decreases from 76.088 to 49.252 ms. This is a warm POSIX image measurement,
not mounted or cold-device throughput. Three write states on each of two
extent/indirect profiles are byte-identical to the earlier independently verified
images. Evidence is in `artifacts/checks/crc32c-development-summary.json`,
`artifacts/checks/crc32c-benchmark-results/` and
`artifacts/checks/crc32c-write-export-comparison.json`.
Full regression passes with the partial-write batch: six successful CI jobs,
253 unique tests matching the registered inventory, no Meson failures or skips,
and no failed independent states. The existing shared-value reader-only oracle
limitations and embedded applicability notices remain separate. Evidence is under
`artifacts/checks/partial-crc-ci-36341405091/`; it does not cover later growth work.

## Namespace on completely allocated block bitmaps

Eight independently generated 8 MiB profiles have no free blocks in any group and
no reserved-block pool. They cover 1/4 KiB blocks, indirect mapping, absent metadata
checksums or FILETYPE, 128-byte inodes, an explicit checksum seed and a modern orphan
file. Both linear and indexed parents must grow for the selected long name.

The eight sanitized `namespace-space-*` tests pass 96 failed namespace operations
without writes, changed output or leaked allocation. An empty file, inline symlink,
hardlink and rename still fit existing directory records; an existing data block
can be overwritten. Sparse growth reads as zero without allocating a block, while
a write to that hole reports ENOSPC. Releasing just one block is insufficient for
mkdir plus parent growth: both linear and indexed cases cancel their prepared
allocation. Releasing two blocks allows the same operation to finish and fills the
filesystem again. Clean recovery is idempotent.

Independent e2fsprogs checks pass all 32 prepared/reused/created states with exact
names, identities, attributes, data, block/inode accounting and nonrepairing e2fsck.
All 40 source/export hashes remain unchanged by inspection. Reports are in
`artifacts/space-tests-first.xml`, `artifacts/space-independent-final/` and
`artifacts/checks/space-*`.

The actual Linux reference kernel passes all 32 states. It checks the retained
objects, observes zero free blocks, rejects mkdir and mapped symlink allocation,
and overwrites an existing block without allocation. It then truncates the filler
to release blocks, creates new indexed objects and leaves a committed journal.
Portable recovery and separate e2fsprogs replay agree on all retained names,
attributes, contents and exact allocation accounting. Every case contains three
recovered transactions; repeat recovery is idempotent and the inputs retain their
hashes. Reports and guest console markers are in the lab under
`artifacts/ext4-journal/linux-reference/space-linux-fixed/`.

The initial Linux smoke found a recovery error: a committed revoke can precede
the durable REVOKE feature advertisement in the journal superblock. Recovery now
accepts the checked record independently of that bit. A regression first failed
against the earlier implementation, then passed 192 checks across checksum and
address-width profiles, advertised/unadvertised records, later block reuse and
transaction sequence wrap. Eight checksum-absent cases are explicitly inapplicable.
Malformed lengths, protected/out-of-range targets and bad checksums still fail
before writes. The exact saved failing Linux journal now matches independent
replay, including its data and accounting; the original image remains unchanged.
Evidence is in `artifacts/revoke-space-green.xml`,
`artifacts/space-recovery-fixed-retained/` and `artifacts/checks/revoke-*`.

The complete local regression and published CI each pass 167 CTest suites with
no failures or CTest-level skips. CI independently checks all eight capacity
profiles and all 32 exported states. Explicit case-level applicability skips
remain separate in the full test logs; JUnit can truncate successful-test output.
Evidence is in `artifacts/space-acceptance.xml` and
`artifacts/checks/space-ci-manifest.json`. This bounded profile does not establish
large-volume performance or writable native behavior.

The indexed core and recovery correction also compile into both unsigned kext
architectures and the universal FSKit core. Symbol inspection confirms indexed
operations and recovery are present in those core products, with no unresolved
internal ext4 symbols in either kext. Reports are in
`artifacts/checks/space-platform-builds.json`. Both adapters remain read-only;
these builds add no installed or mounted native acceptance.

## FSKit build evidence

The app and embedded extension compile using the macOS 27 SDK with deployment
target macOS 26.4. The adapter maps resource I/O, inode identity, attributes,
lookup, directory cookies, links and reads to the same C library. Mutations return
`EROFS`; its requested mount options include read-only. Only a quick clean-volume
check is implemented; it is not a full consistency checker or repair utility.

Unsigned build reports are in `artifacts/checks/fskit-reader-final-build.log` and
the earlier FSKit build logs. Automatic signing found no Xcode
account or provisioning profile for `org.machlin.ext4.filesystem`. Signing is
deferred by the user. No extension has been installed or mounted.

`ext4-mounted-test MOUNTPOINT` is built, but its FSKit runtime results remain pending.
It checks ordinary reads, metadata, hard links and symlinks, indexed directory
enumeration, sparse data, mmap, concurrent opens/reads/closes and read-only
enforcement. The disposable stock macOS guest was booted and its actual loaded
Apple kernel identified, then shut down. This proves guest readiness only.

## Kernel build evidence

`make kext` compiles every shared core source with the selected Xcode kernel
headers and emits an unsigned arm64e Mach-O kext bundle. The x86_64 build also
passes. Both builds enforce the C declaration/style checks and an optimized
2 KiB stack-frame limit. Inspection reports live under `artifacts/checks/`.
The listed imports correspond to XNU's BSD and Libkern exports; that comparison
does not establish compatibility with a running kernel.

The bundle declares `OSBundleAllowUserLoad`, allowing XNU to retain the mutable
segment state needed for unloading and reloading a module in the primary kernel
collection. It does not alter the system's kext signing or authorization policy.
Normal load/unload/reload passed, followed by another successful normal unload
after the final mounted suites. The lifecycle report records statuses and loaded
module observations; the dedicated VM was then shut down cleanly.

The adapter implements read-only mounts, inode/vnode identity, lookup, stat,
directory enumeration, symbolic links and native UBC reads/page-in. It currently
creates vnodes for regular files, directories and symbolic links; special-file
operations remain unsupported. All mutating vnode operations reject writes.
The arm64e module was packaged with the preserved native custom XNU kernel and
the matching stock kext inventory, then actually booted and explicitly loaded
in the separate `lxnu-ext4-kext-lab` clone. The loaded module UUID, collection
hash, boot slot and new session were checked. This is custom-kernel execution;
it does not establish signed kext installation or stock FSKit acceptance.

The expanded mounted test passed three mount/test/unmount cycles on the attached
indexed image, with unchanged raw device bytes. It checks exact UID/GID and
timestamps, links, sparse bytes, directory positions, eight concurrent readers,
read-only errors, mmap after descriptor close, EOF padding and private copy-on-write.
The same mounted test passed all eleven read-only image devices from the extended
profile. A separate `ext4-mounted-lifetime-test MOUNTPOINT` checks that both an
open descriptor and an untouched mapping keep unmount busy, and that releasing
the last mapping permits normal unmount. It passed without a kernel/session change.

Lab evidence lives under `logs/ext4-kext/` and `artifacts/ext4-kext/`: collection
identity, `userload-*` loaded-kernel/module reports, `mounted-*`, `matrix-*`,
`lifetime-userload-20260927.log` and `final-userload-lifecycle-20260927.json`.
Earlier failed attempts remain separate. These
tests do not establish writable pageout, truncate races, forced unmount, complete
allocation balance, physical hardware or x86_64 runtime behavior.

The reader suite independently checks block mappings by reading returned device
offsets through the raw POSIX resource and comparing file contents, including
unaligned offsets, holes and the last file block. These checks do not substitute
for actual page-in and buffer strategy execution in XNU.

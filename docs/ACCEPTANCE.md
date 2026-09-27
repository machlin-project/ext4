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
| Geometry, feature negotiation, metadata checksums | Real mke2fs images and malformed-input tests under sanitizers | Eleven read profiles pass; broader format and size coverage pending |
| Inodes, directories, links, extents, sparse data | Independent contents and metadata comparison | Portable reader and mounted arm64e kext profiles pass; FSKit runtime pending |
| Modern format variations | Explicit feature/size matrix including checksums, 64-bit fields, indexed directories and additional enabled features | Not accepted |
| Create/write/truncate, allocation, rename, unlink | Linux roundtrips, full disks, partial I/O and open-file lifetime | Bounded writes, allocation, growth and truncate/freeing pass independent and Linux checks; live shrink spans transactions; create/mkdir/symlink/link/unlink/rmdir/rename and bounded indexed mutation pass portable, independent and Linux checks; core holds retain open-unlinked or replaced objects; platform writes and broader capacity/concurrency acceptance pending |
| Journal and recovery | Interrupted transactions, ordering faults, device errors, Linux replay and e2fsck | Bounded internal journal engine, legacy lists and modern orphan files pass portable faults, independent recovery and Linux reuse; advanced journal formats and platform write integration remain pending |
| Xattrs, permissions and ACLs | Preserve and mutate metadata across macOS/Linux roundtrips | Selective owner/mode/timestamp updates pass portable and Linux checks; raw xattr get/list and atomic attribute batches pass portable and independent checks; mutation lifetime integration, ACL enforcement, Linux xattr roundtrips and platform policy pending |
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
`artifacts/fixtures-metadata/`; the CMake `EXT4_FIXTURES` setting selects them.

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

This accepts the linked-inode raw attribute batch. Ordinary file writes, creation,
namespace mutation and orphan cleanup still reject attribute-owning inodes. Their
integration, ACL enforcement and Linux xattr roundtrips remain required. Full-space,
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
Linux runtime roundtrips and published CI for this writer are still pending.

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
invalid commits discard the incomplete tail. Structural cases with repaired
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
implementation and acceptance. Internal journals with external devices, old checksum v1, async or
fast commits are unsupported; configured resource bounds are explicit in
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
accepted as full CI validation. CTest now schedules each image separately with
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
and unchanged allocation pass independent inspection and e2fsck. LARGEDIR growth
itself remains unsupported; this is a tested rejection boundary.

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

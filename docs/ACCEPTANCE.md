# Acceptance

The end goal is a working read/write filesystem in stock macOS through FSKit and
in Machlin through a kernel adapter, with the agreed Linux metadata contracts.
The rows below are requirements, not claims of implementation. A checkpoint does
not complete the project. Format support must expand with tested real images;
safe rejection of a feature is recorded separately from supporting it.

| Contract | Required evidence | Current state |
| --- | --- | --- |
| Geometry, feature negotiation, metadata checksums | Real mke2fs images and malformed-input tests under sanitizers | Eleven read profiles pass; broader format and size coverage pending |
| Inodes, directories, links, extents, sparse data | Independent contents and metadata comparison | Portable reader and mounted arm64e kext profiles pass; FSKit runtime pending |
| Modern format variations | Explicit feature/size matrix including checksums, 64-bit fields, indexed directories and additional enabled features | Not accepted |
| Create/write/truncate, allocation, rename, unlink | Linux roundtrips, full disks, partial I/O and open-file lifetime | Bounded writes, allocation, sparse growth, unwritten conversion and truncate/freeing pass independent and Linux checks; live shrink spans transactions; directory mutation and platform writes pending |
| Journal and recovery | Interrupted transactions, ordering faults, device errors, Linux replay and e2fsck | Bounded internal journal engine, legacy lists and modern orphan files pass portable faults, independent recovery and Linux reuse; advanced journal formats and platform write integration remain pending |
| Xattrs, permissions and ACLs | Preserve and mutate metadata across macOS/Linux roundtrips | Selective owner/mode/timestamp updates pass portable and Linux checks; ACLs/xattrs and platform policy pending |
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
operations. Directory mutation, online orphan lifetime, writable UBC/FSKit
coherence, and durable platform device barriers still need implementation and
acceptance. Internal journals with external devices, old checksum v1, async or
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
Directory mutation, security xattrs,
concurrent native page-cache ownership and LXNU policy remain unaccepted.

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
below. Open-unlinked lifetime and native UBC/FSKit resize
concurrency remain unaccepted.

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
orphan-file evidence is recorded below; orphaned ACL/xattr inodes, online
open-unlinked lifetime or writable
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
open-unlinked lifetime and platform writes remain unaccepted.

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
and have compilation evidence only for this change. Online orphan insertion,
file growth, ACL/xattr cleanup, native object lifetime and writable cache/durability
contracts remain required work.

The preceding live-truncate CI run exhausted runner storage after its CTest suites
and ten ordinary independent live profiles passed. The workflow now removes each
stage's generated images/dumps only after successful independent validation,
retaining JSON reports, logs, hashes and JUnit output. This corrects CI storage
use; that failed run is not counted as completed CI acceptance.

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

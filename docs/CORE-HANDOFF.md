# Core review handoff

The portable core's functional queue is accepted with the explicit limits in
[ACCEPTANCE.md](ACCEPTANCE.md). The active task is final review, structural
refactoring and measured performance. Preserve those limits; old test counts do
not establish acceptance of later source. Detailed evidence is in
[CORE-REVIEW.md](CORE-REVIEW.md).

## Working rules

Read the workspace and repository `AGENTS.md`, `README.md`, `ARCHITECTURE.md` and
`DEVELOPMENT.md`. Work in `/Users/darekhta/Development/machlin/ext4` on `development`.
The main agent owns architecture, implementation, tests, diagnosis and acceptance.
Delegate prepared CLI execution to GPT-6 Luna and VM operations to GPT-6.1 Sol,
with explicit models, absolute paths and one owner per VM. Build and VM commands
use `/Users/darekhta/Development/machlin/lab` as their working directory.

The user has a constrained usage budget. Make coherent implementation batches,
run focused checks during development and full regression at batch boundaries.
Do not wait for remote CI before continuing useful work. Preserve compiled binaries
for running checks, serialize heavy builds, and keep the host quiet during timing.
Run every Meson command with `env -i PATH="$PATH"`; otherwise its text log records
inherited environment variables. Do not print those records or credential contents.
Keep generated inputs, hashes, logs and measurements in ignored artifacts; use Git
for source history. Verify personal identity and signing before publication.

## Acceptance target and current measurements

Reads require a geometric mean of at least **1.15 relative to Linux across eight
fixed profiles**, and **every profile at least as fast as Linux**. These are dense
and sparse files, warm and guest-cold cache, sequential and random access. Writes
are evaluated separately with equivalent durability. SHA/AES are separate again.

The latest read mean is **1.162**, but all four dense-file profiles are below Linux:
0.977 warm sequential, 0.963 warm random, 0.909 cold sequential and 0.997 cold random.
The agreed read target is **not met**. Evidence and the failing acceptance gate are
in lab `artifacts/ext4-journal/read-run-cache/`. The gate is
`tests/check_read_benchmark.py --require-target`; a valid measurement can still
fail its performance target. Do not remove difficult profiles or count raw-backend
controls as core results. Guest-cold is not host/storage-cold.

Dense throughput is close to the raw-device control, which also trails Linux in
some profiles. Further algorithm changes cannot be assumed to close that backend
gap. Demand-pread and mmap diagnostics hurt cold performance and were not adopted.
The user prioritizes algorithms and architecture usable by both FSKit and LXNU.

The latest matched write run is in lab
`artifacts/ext4-journal/write-held-inode/`: 28 samples, final Linux
readback and strict host fsck of both volumes pass. Sequential and random median
ratios are 0.969 and 0.966, with wide paired ranges. These wall times do not support
precise speedup claims. The core now holds its inode for the Linux file descriptor's
lifetime, explicitly identified by `CORE_WRITE_INODE`. Against the preceding
`write-compare-ordered-source/` run, sequential metadata reads fall from 16,384 to
8,192 and allocations from 33,024 to 24,832 per 256 MiB; random reads fall from
262,144 to 131,072 and allocations from 524,544 to 393,472. Write bytes, barriers
and device-write medians are unchanged.
Both use identical preallocated files, ordered data and a timed durability barrier
every 1 MiB. Neither write profile beats Linux.

## Current implementation batch

- `read_state.c` owns held inode snapshots, mapping caches and invalidation shared
  by `ext4_read_held` and `ext4_map_read_held`. Stateless APIs retain their contracts.
- Readers reuse the last checked run without another extent search or indirect
  traversal. Four focused read tests and all three native builds pass in
  `artifacts/checks/read-run-cache/`; the 140-sample Linux run passes functionally
  but fails the strict performance gate. No general speedup is claimed from it.
- `memory.c` owns copy, equality and zeroing. Guarded ARM64 DC ZVA uses only general
  registers and whole aligned normal-memory blocks within the caller range. The
  portable fallback is independently tested. Kernel SIMD remains excluded.
- Journal payload encoding borrows unescaped immutable snapshots, copying only
  blocks requiring JBD escape. Fragmented log-run lookup uses binary search.
- `transaction.c` owns private snapshots, indexing and buffer lifetime;
  `journal.c` owns ordering, log emission, commit and checkpoint. The private
  `transaction.h` layout is shared only by these modules.
- Full unencrypted data writes borrow immutable caller bytes until commit returns.
  Anything retained in a compound/checkpoint becomes owned before I/O. Ordered
  writes coalesce only physically adjacent blocks inside one bounded source range.
  Partial/encrypted changes retain private buffers. Barriers are unchanged.
- Commit rejection distinguishes clean memory/quota refusal from the same status
  returned by a write callback: an aborted journal always makes the failure fatal.
- Checked inode resolution shares one group descriptor between allocation-bitmap
  validation and record location for writes, held refresh and xattr reads.
- Held inodes retain the checked record address while the writable owner prevents
  freeing/relocation. Edits still check current record checksum, generation and
  operation policy. Read-only owners and failed refresh cannot use the shortcut.
  Hold creation now uses the common refresh/validation path. Eight focused tests,
  including complete 1 KiB/4 KiB/indirect removal fault profiles, and all three
  native builds pass in `artifacts/checks/held-inode-location/`, without skips.

Focused ownership/fault checks before the module split passed in
`artifacts/checks/ordered-source/`. The final split's full 718-test run is recorded
in `artifacts/checks/ordered-source-final/`: 716 passed, two partial-write fault
profiles failed, and 29 explicit applicability skips occurred in 17 tests.
It exposed a pre-I/O allocation failure being misclassified as fatal. The correction
and its focused verification are separate in
`artifacts/checks/ordered-source-rejection-fix/`: the rebuild and 68/68 focused
tests pass, including both prior failures, with no explicit skips. Unsigned
FSKit and arm64e/x86_64 builds pass in `artifacts/checks/ordered-source-native/`;
they establish compilation only.

A final compact transaction-entry layout stores bounded source lengths in complete
blocks. Its 13 focused tests and unsigned FSKit/arm64e/x86_64 builds pass in
`artifacts/checks/source-block-bounds/`. Before/after counter runs produced identical
images and deterministic counters, both passed nonrepairing fsck, and peak live
memory fell by 4,096 bytes in all three workloads. The latest Linux writer includes
this refinement and held-inode resolution; its peak reduction is their combined
effect, not an isolated measurement of one change.

The last prior complete regression was 716/716 with 29 explicit applicability
skips, recorded in `artifacts/checks/write-targets/`. Later snapshot initialization
and data-edit improvements had focused checks, native compilation and independent
RAM comparisons, not another full regression. Their details remain in the review.

## CI investigation

The latest published Linux format job rejected the relaxed casefold export with
an HTREE minimum-hash error. The exact image is now preserved in
`artifacts/checks/ci-latest/` and fails unchanged under the original e2fsprogs.
An oracle self-test reproduces an e2fsprogs malformed-UTF-8 error before involving
the core: casefold returns ENAMETOOLONG instead of EINVAL, defeating opaque-name
hash fallback. e2fsck then uses a stale hash. A recorded one-line patch exists at
`tests/patches/e2fsprogs-casefold-invalid-sequence.patch`; CI applies it explicitly.
The isolated corrected tool accepts that exact image with unchanged bytes; the
604-name/flag verifier and 1,318,086 fold/24,120 hash comparison also pass. The fix
is committed and pushed separately, and the corrected remote Linux format job
passes. Keep the original failure, corrected-tool evidence and filesystem checks;
inspect the remaining matrix asynchronously.

## Next work

Implementation and benchmark batches are committed. Inspect CI asynchronously.
Continue closing the per-profile read deficits without
weakening the comparator; writes remain a separate performance direction.
The user selected native I/O integration: FSKit first, then LXNU, and expanded
the immediate task to a complete FSKit adapter with an app control channel.
Do not replace the fixed pread comparator, exclude a profile, or call unsigned
adapter tests production FSKit/LXNU performance evidence. See [FSKit](FSKIT.md)
for the integration contracts and outstanding native acceptance.

Both adapters remain read-only. FSKit integration precedes LXNU policy; signing
is deferred. FSKit now adopts held reads; installed mapping and lifetime acceptance remains
pending. FSKit now has a CommonCrypto fscrypt provider and Keychain-backed immutable
mount keys; the app can import v1/v2 raw master keys. Native crypto/manifest tests
pass; signed Keychain sharing and mounted encrypted I/O are still unaccepted.
The minimum stays 26.4, with 27-only APIs separated by availability and compile
guards. The public durable device-barrier contract is still unresolved; do not
wire metadataFlush or callback completion into a falsely successful barrier.
The kernel adapter has not yet adopted these APIs.
XNU/LXNU retains UBC/cluster I/O; do not build a replacement file page cache in core.
Native cryptography, writes, concurrency and page-cache integration require their
own acceptance. No host kernel, boot policy, NVRAM or system-file changes.

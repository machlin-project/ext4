# Core review and performance

## Current conclusion

The functional core queue is accepted with the explicit limits in
[ACCEPTANCE.md](ACCEPTANCE.md). This bounded review covers mapping, read lifetime,
cryptography, mutation ownership, journal publication and failure handling. It
is not an assertion that every implementation path is defect-free.

The performance goal remains **unmet**. Reads require a geometric mean of at least
1.15 across eight fixed profiles and every individual profile at least as fast as
Linux. The latest mean is **1.162**, but all four contiguous-file profiles remain
below Linux. Writes are assessed separately; their latest sequential/random
ratios are **0.969/0.966**. SHA/AES microbenchmarks cannot satisfy either file-I/O
criterion. All comparisons and unfavorable results remain in the raw reports.

Both native adapters remain read-only and do not yet adopt the held-read APIs.
Unsigned FSKit and arm64e/x86_64 compilation passes; that is not installation,
mount, native write, concurrency or performance acceptance. FSKit signing remains
deferred. FSKit precedes LXNU-specific policy.

## Architecture and correctness review

The shared boundary remains appropriate: core owns disk formats, algorithms,
metadata transactions and recovery; adapters own credentials, key services,
serialization, native page caches and resource lifetime. A filesystem instance
still requires one serialized owner. No portable optimization introduces a second
file-data cache or licenses kernel SIMD use.

The current source separates the following responsibilities:

- `read_state.c` owns held snapshots, revisions and disposal; `map_read.c` owns
  validated mapping; `read_io.c` plans/delivers byte ranges; `read.c` handles EOF,
  decryption and verity routing. Copied reads and native mappings share their
  checked state. Native mappings retain journal-home, EOF-padding and crypto
  restrictions.
- Fully checked extent leaves have bounded retention and ancestor intervals.
  Sequential boundaries use a cursor; other searches use binary search. The last
  decoded extent/indirect run answers interior requests without another decode
  or tree walk. It has no pointer into replaceable scratch. Failed misses do not
  damage it; refresh, revision invalidation and close discard stale state.
- Mapped reads batch physically adjacent data across holes using at most 32 spans
  and 256 KiB of data. Planning cannot fetch new metadata or move a later error
  ahead of a readable prefix. Packing, backward expansion and zeroing use the
  caller's buffer; completed-prefix and range bounds remain explicit.
- `transaction.c` owns enrollment, indexing, snapshots and transfer; `journal.c`
  owns ordering, emission and checkpointing; `journal_read.c` delivers the live
  compound/checkpoint/home view. Held journal bytes no longer trigger obsolete
  home reads before being overwritten by snapshots.
- Complete unencrypted write blocks may borrow immutable caller input until the
  synchronous operation returns. Before I/O, commit owns every view requiring
  journal retention. Coalescing requires contiguous device blocks and source
  pointers within one explicit source bound. Neither compound nor checkpoint
  retains caller pointers. Complete-block bounds keep entries compact.
- Clean pre-I/O memory/quota refusal cancels without aborting the filesystem. The
  same status from a write callback remains fatal: the journal's aborted state
  distinguishes the cases. Durability ordering and barriers are unchanged.
- Group resolution shares one checked descriptor between inode allocation
  validation and record location. A held inode additionally retains its checked
  address for the exclusive writable owner's allocation lifetime. Edits still
  validate current record checksum, generation, live state and operation policy.
  Failed refresh revokes the address; read-only owners recheck allocation.
- `memory.c` owns byte primitives. Guarded ARM64 DC ZVA zeroes only permitted whole
  aligned normal-memory blocks inside the requested range, with ordinary-store
  edges and a separately tested portable fallback. It uses no SIMD registers.

The mapping review found and fixed a direct data pointer aliasing an indirect
root outside the requested path. The regression fails against the saved earlier
core. Current checks cover explicit roots, traversed ancestors, duplicate targets
and protected metadata; they do not claim a global scan of other inodes or every
unvisited descendant. Write target ownership distinguishes the one repeated
logical boundary from illegal physical aliases. Whole-block initialization and
shared partial/encrypted edits avoid unnecessary reads and copies while retaining
private ownership before publication.

Structural definitions precede functions, fscrypt context layout has one private
source, and the selected clang-format profile remains enforced. See
[ARCHITECTURE.md](ARCHITECTURE.md) for the full ownership contracts.

## Read comparison

The dedicated Virtualization.framework guest runs Linux 6.12.94-0-virt aarch64,
with two CPUs, 512 MiB and contenders pinned to CPU 0. An immutable 128 MiB ext4
image contains a 16 MiB contiguous file and a 16 MiB file alternating 4 KiB data
and holes. The core uses exact buffered raw-device pread; Linux uses file pread.
Seven pairs are interleaved. Requests are 1 MiB sequential or permuted 4 KiB.
Warm samples read 8 GiB/1 GiB; guest-cold samples read 2 GiB/64 MiB respectively.
Every cold pass discards core metadata and guest page caches; reconstruction is
timed. Guest-cold does not mean host/storage-cold.

| File | Cache | Access | Linux MiB/s | Core MiB/s | Core/Linux |
| --- | --- | --- | ---: | ---: | ---: |
| Contiguous | Warm | Sequential | 31,818.9 | 31,073.2 | 0.977 |
| Contiguous | Warm | Random | 4,276.9 | 4,120.4 | 0.963 |
| Contiguous | Guest-cold | Sequential | 8,998.9 | 8,177.4 | 0.909 |
| Contiguous | Guest-cold | Random | 170.8 | 170.3 | 0.997 |
| Sparse | Warm | Sequential | 24,324.8 | 33,008.1 | 1.357 |
| Sparse | Warm | Random | 10,482.4 | 16,374.5 | 1.562 |
| Sparse | Guest-cold | Sequential | 6,347.9 | 10,906.8 | 1.718 |
| Sparse | Guest-cold | Random | 313.5 | 336.1 | 1.072 |

All 140 rows, independent byte checks, clean guest shutdown and unchanged image
hash pass. The checker with `--require-target` correctly exits 1: mean **1.1622**
passes, but four profiles fail. Evidence is in lab
`artifacts/ext4-journal/read-run-cache/`; the preceding comparison remains in
`read-memory-candidate/` with mean 1.1859. Between those boots Linux warm sparse
rates rose 7.4–8.1%, while core rates fell 0.5–1.5%. These are not an isolated
old/new-core experiment and do not establish a general speedup from run reuse.

The raw control bypasses filesystem work using the known contiguous mapping.
Its ratios also remain below Linux: 0.981 warm sequential, 0.989 warm random,
0.946 cold sequential and 0.997 cold random. Core mapping changes alone therefore
do not demonstrate that the present backend can close the remaining threshold.
Raw results never contribute to the core score.

The separate `read-backend-diagnostics/` run preserved the baseline and added
Linux/core mmap and demand-pread controls. All 308 rows passed. Mmap helped warm
core reads but hurt cold reads and lost to Linux mmap on contiguous data;
demand-pread reduced speculation but also sequential throughput. Neither was
adopted or counted as a portable-core win. Mapped controls required an immutable,
fault-free resource and are not production error-handling acceptance.

The earlier timing qualification in `core-review-timing/` checked the actual
arch_sys_counter guest clock against host console-marker intervals and CPU-loop
scaling. It supports long, warmed, interleaved same-guest comparisons, not a
host-versus-guest claim or nanosecond measurement accuracy. Full paired ranges,
CPU times, callback counts and device traffic remain in generated reports.

## Write comparison

The latest protocol retains a core inode hold for the Linux file descriptor's
lifetime, explicitly identified by `CORE_WRITE_INODE` in the console and report.
Earlier reports had no hold. Two identical initial volumes receive warm,
preallocated overwrites with ordered data. Each sample writes 256 MiB and includes
a durability barrier every 1 MiB, using 64 KiB sequential or permuted 4 KiB requests.
Seven pairs are interleaved. Permissions, timestamps and attribute admission remain
part of the core operation; writes do not bypass record validation.

| Profile | Linux MiB/s | Core MiB/s | Core/Linux | Paired range | Linux/core CPU ms |
| --- | ---: | ---: | ---: | --- | ---: |
| Sequential | 73.5 | 71.3 | 0.969 | 0.664–1.573 | 403.8 / 517.0 |
| Random | 77.4 | 74.8 | 0.966 | 0.569–0.979 | 265.3 / 623.8 |

All 28 samples pass, Linux independently reads the final core output, the guest
powers off cleanly, and both output images pass nonrepairing e2fsck. Evidence is
in lab `artifacts/ext4-journal/write-held-inode/`. Both profiles still trail Linux.
Wide wall-time ranges and changing CPU rates between boots preclude a precise
old/new timing claim. The preceding ratios were 0.926/0.903; preserve the paired
reports rather than claiming their difference as a proven speedup.

Deterministic core counters establish reduced work relative to the preceding
`write-compare-ordered-source/` run:

| Per 256 MiB | Ordered source | Held inode |
| --- | ---: | ---: |
| Sequential read callbacks | 16,384 | 8,192 |
| Sequential read bytes | 50,593,792 | 33,554,432 |
| Sequential allocations | 33,024 | 24,832 |
| Random read callbacks | 262,144 | 131,072 |
| Random read bytes | 809,500,672 | 536,870,912 |
| Random allocations | 524,544 | 393,472 |
| Peak live bytes, either profile | 153,752 | 145,608 |

The peak comparison includes the intervening compact transaction-entry refinement.
Write bytes remain 271,581,184 and flushes 512 in both profiles. Write callbacks
remain 4,864 sequential and 66,304 random; device write/sector/flush medians are
unchanged. Fewer core callbacks are not fewer physical writes. Earlier source
coalescing reduced sequential callbacks from 66,304 to 4,864 and allocations from
102,656 to 33,024; its baseline is preserved in `write-compare-admitted/`.

Earlier RAM experiments isolate particular core changes, not Linux durability.
They preserve counter/image/fsck evidence, including unfavorable outcomes: the
long ordered-write target-owner comparison was about 1.6% slower despite fewer
allocations, while borrowed journal payloads improved a long journaled-data test
about 7% and its ordered-data control only 0.6%. Do not promote short sequential
or sub-millisecond truncate timings into general throughput claims. Historical
reports remain under `artifacts/checks/write-targets/` and `journal-payload/`.

## Hashing and encryption

Portable SHA-256/SHA-512 consume complete unaligned blocks directly and rotate
working roles through grouped rounds. Verity clones a hash state containing its
padded salt. Independent hashlib vectors cover padding/streaming boundaries,
alignments and allocation ends. No-key names retain Linux's accepted trailing
zero sextet and size bounds; direct alphabet decoding replaces linear scanning.

ARM64 userspace targets guaranteeing SHA-256 instructions use ACLE intrinsics.
Kernel builds and `EXT4_SHA_PORTABLE` retain the scalar transform until native
SIMD ownership is separately accepted. On Apple M4 Pro / Apple Clang 21 O2,
interleaved scalar/instruction runs produced these median MiB/s rates over 18
samples per version, with unaligned inputs and independently matching digests:

| Message | Scalar | Instructions | Ratio |
| --- | ---: | ---: | ---: |
| 64 B | 210.7 | 929.6 | 4.412 |
| 1 KiB | 425.1 | 2,029.2 | 4.773 |
| 4 KiB | 451.2 | 2,117.0 | 4.692 |
| 64 KiB | 459.6 | 2,182.5 | 4.749 |

The unchanged SHA-512 control ratios were 0.989–1.013. Earlier portable-round
changes improved SHA throughput roughly 13–17% and the four measured verified-read
profiles 14.6–20.8% against the preceding core. These are separate microbenchmarks,
not ordinary read, AES, Linux or mounted-adapter speedups. Evidence remains in
`artifacts/checks/core-review-2/` and `sha-instructions/`. A CRC alignment experiment
regressed common unaligned inputs and was rejected; existing hardware CRC32C
selection is retained.

AES, key derivation and key lifetime belong to production adapter providers.
Optimizing the test reference AES would not accelerate them. Core fault tests
cover lookup/derivation, nonce generation and ciphers that partially change output
before failing; private writes remain unpublished, read prefixes remain accurate
and acquired handles balance. Concurrent revocation, trust-root replacement and
native provider failures still need adapter acceptance.

## Verification scope and CI

The latest broad transaction run in `artifacts/checks/ordered-source-final/`
configured 718 tests: **716 passed and two partial-write fault profiles failed**.
It exposed clean pre-I/O memory refusal being treated as a fatal write. The fix
passed **68 focused tests**, including both prior failures, with no explicit skips,
in `ordered-source-rejection-fix/`. Preserve the original full result without
relabeling its failures. That full run also reported 29 explicit applicability
skips in 17 tests, separate from Meson's pass count.

Subsequent bounded checks are recorded separately:

- `source-block-bounds/`: 13 ownership/journal/partial-write tests pass. Before/after
  counter runs have identical deterministic work and output images, both accepted
  by fsck; peak live memory falls 4,096 bytes in each workload.
- `read-run-cache/`: four read/mapping/journal tests pass, including failed misses,
  reverse seeks and logical limits; the Linux read run passes functionally but
  fails its performance target.
- `held-inode-location/`: eight tests pass, including full 1 KiB, 4 KiB and indirect
  removal/fault profiles. Held/unheld writes produce identical durable images
  with two fewer metadata reads and allocations. Current generation/checksum,
  failed refresh, read-only fallback, unlink, release and reuse remain checked.

Each of these batches has optimized freestanding frame checks and unsigned FSKit,
arm64e and x86_64 build evidence. The later focused sets have no explicit skips;
they are not a new full regression. Remote CI runs independently on published
source. Never substitute an older green revision for the current result.

Two CI oracle/harness issues were corrected without weakening core checks:

- Pinned e2fsprogs misclassified malformed UTF-8 encountered after cursor setup as
  ENAMETOOLONG instead of EINVAL, bypassing opaque-name hash fallback. e2fsck then
  reused a stale hash. The recorded one-line tool patch makes the exact preserved
  rejected image pass unchanged; independent verification checks 604 names/flags,
  1,318,086 folds and 24,120 hashes. Evidence is in `ci-latest/`; CI applies the
  patch explicitly, and the corrected Linux format job passes.
- Fragmented verity fault enumeration repeated complete prefixes quadratically.
  Each stateless chunk now receives its own allocation/read faults, preserving
  prefix and guard checks. It still covers 4,200 allocation and 5,384 read failures;
  all five local profiles and the remote format job pass without a larger timeout.

## Remaining work

Close the four read deficits through an accepted data-I/O path without weakening
the eight-profile gate, and continue treating writes separately. The current raw
backend controls expose a limitation that more extent-search tuning alone has not
resolved. Native integration must carry over held ownership, serialization and
native cache contracts; unsigned compilation cannot establish its performance.
Follow [CORE-HANDOFF.md](CORE-HANDOFF.md) for execution constraints and
[DEVELOPMENT.md](DEVELOPMENT.md) for reproducible commands. Source history belongs
in Git; exact inputs, binaries, identities and measurements belong in artifacts.

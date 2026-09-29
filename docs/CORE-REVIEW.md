# Core review and performance

## Scope and conclusions

The functional core queue is accepted with the limitations in `ACCEPTANCE.md`.
This review examined the cryptography boundary, SHA streaming, verity hashing,
no-key names, transaction/compound publication, MMP ownership guards and remembered
directory-index validation. It also checked recent acceptance evidence and local
type organization. It is a bounded review, not an assertion that every path in
the implementation is defect-free or that native integration is accepted.

The shared-core/native-adapter boundary remains appropriate. The core owns disk
formats, transforms, metadata transactions and recovery. FSKit and XNU/LXNU own
key services, locking, native caches, credentials and platform resource lifetime.
The current filesystem instance requires a serialized owner. This review does not
introduce native concurrency by adding locks inside portable algorithms.

No confirmed data-integrity defect was found in the inspected paths. In particular,
a trailing zero base64url sextet initially looked like an invalid no-key filename,
but the pinned [Linux 6.12 decoder](https://github.com/torvalds/linux/blob/v6.12/fs/crypto/fname.c)
accepts it too. Preserve that behavior within the name-size bound; changing it would
be an interoperability change, not a correctness fix.

## Changes

- Private structures precede function definitions in the directory, namespace,
  mapping, write and fast-commit modules. The executable text of those six files
  is unchanged after excluding moved definitions, comments and whitespace.
- The fscrypt context union has one private definition shared by parsing and
  writing. AES implementations and key bytes remain with the adapter.
- SHA-256/SHA-512 consume full, potentially unaligned blocks directly. Partial
  updates retain their existing buffering and padding semantics. Compression
  processes eight rounds per loop, rotating word roles through inline round
  functions instead of copying seven words each round.
- Verity keeps a hash context initialized with its padded salt and clones it for
  each Merkle leaf/node. This removes repeated salt compression without introducing
  a mount-wide data cache or changing the on-disk digest.
- No-key decoding maps alphabet ranges directly rather than scanning 64 symbols
  for every input byte. Its accepted byte strings remain Linux-compatible.
- README and the handoff now describe the current phase rather than an obsolete
  fast-commit implementation queue. Historical acceptance remains in its own report.

## Validation and remaining coverage

`hash-streams-and-nokey-names` adds independently computed hashlib answers at
padding and compression boundaries through 64 KiB. It tests eight alignments,
every initial split through 129 bytes, empty updates, byte-at-a-time updates and
buffers ending at the allocation boundary under ASan. It also covers every
ciphertext filename length, every input byte of the base64url alphabet, nonzero
unused bits, oversized names and Linux's extra-zero-sextet behavior.

The applicable integration checks are the existing crypto vectors, four verity
image profiles, verity enabling, encrypted reads/writes and sustained encrypted and
verity operations. They retain independent image oracles, corruption checks and
interrupted-write coverage. Freestanding and both kernel compilation targets check
the larger verity context against the stack budget. Builds are not native mounts.
Generated results for the two measured batches live in
`artifacts/checks/core-review-1/` and `artifacts/checks/core-review-2/`.
The final focused selection passed all 21 unique tests under ASan/UBSan. Both
unsigned kext targets, arm64e and x86_64, compile, including the 2 KiB frame check;
the bounded quota-table conversion no longer produces a narrowing warning. The
independent SHA/no-key test also passes when linked against the previous core.
Formatting and `git diff --check` pass. No extension was loaded for these checks.
The single end-of-batch full regression then passed **711/711 tests**, with no
failures; its dedicated logs are in `artifacts/checks/core-review-full/`. The
core and test binaries stayed unchanged throughout the run. A separate GitHub
run closed the preceding functional baseline successfully in all nine jobs;
CI for the reviewed core is separate from that baseline result.
No entire test was skipped. Within those tests, 29 existing applicability skips
remain explicit: two inode-body checks on 128-byte-inode fixtures and 27 directory
split transitions assigned to other fixture profiles. These are recorded
separately in the full-run report, not counted as exercised subcases.

The encrypted-write tests now inject errors into key lookup/derivation, nonce
generation and encryption/decryption, including a provider that changes part of
its output before returning an error. Failed policy changes, creates and atomic
writes must issue no device writes and preserve the complete image. A read that
fails decrypting its second block must expose exactly the first block, leave the
remaining caller buffer untouched, and report the completed prefix. Removing the
provider must balance every acquired handle; a successful retry must still work.

Remaining targeted coverage is concentrated at the production crypto boundary:
key eviction/revocation and replacement of trust roots while native requests are
queued, plus each provider's actual error behavior. Existing reference-keyring tests
exercise key lookup, eviction through many files and removal of all cached keys;
they do not establish concurrent native lifetime contracts. Those cases belong to
the adapters that own request serialization and key services. The test AES
implementation is explicitly not a production or constant-time provider.

Historical acceptance does not automatically cover a new revision. The previous
710-case regression initially had one malformed-image expectation failure and a
successful focused correction; the later 120,000-operation unjournaled soak is
separate evidence. Preserve both records rather than calling either an unqualified
full pass at a different source revision.

## Measured changes relative to the previous core

These are old/current core comparisons on an Apple M4 Pro, using the same
Apple Clang 21 toolchain and uninstrumented O2 build. They are not Linux or mounted
FSKit/LXNU comparisons. Versions alternated on an otherwise idle host, before VM
qualification. Raw results, source/binary identities and validation are retained in
`artifacts/checks/core-review-2/review-results.json` and its accompanying logs.

SHA timings below are medians of 45 samples per row/version. Each sample hashes
8 MiB as separate messages of the specified size, including initialization and
finalization, using input offset by three bytes. Output digests were independently
checked with Python hashlib outside timing. The last column is `old / new - 1`.

| Algorithm | Message bytes | Old ms | Current ms | Throughput change |
| --- | ---: | ---: | ---: | ---: |
| SHA-256 | 64 | 43.899 | 38.040 | +15.40% |
| SHA-256 | 1,024 | 21.718 | 18.619 | +16.64% |
| SHA-256 | 4,096 | 20.594 | 17.636 | +16.77% |
| SHA-256 | 65,536 | 20.275 | 17.426 | +16.35% |
| SHA-512 | 64 | 31.522 | 27.294 | +15.49% |
| SHA-512 | 1,024 | 15.798 | 13.864 | +13.95% |
| SHA-512 | 4,096 | 14.287 | 12.599 | +13.40% |
| SHA-512 | 65,536 | 13.774 | 11.976 | +15.01% |

The separate no-key decoder microbenchmark performs 32,768 decodes of one
252-byte name per sample. Its 45-sample median fell from 83.758 ms to 7.743 ms
(10.82 times the throughput). This measures a repeated public-name decode with a
warm branch predictor, not encrypted-directory lookup or AES.

Verified file reads use four runs per version, each with an untimed warmup and
31 timed reads. The figures are means of the four run medians, not pooled medians.
The existing POSIX image adapter supplies cached backing reads; timed work includes
the core's Merkle verification and copying to the caller. It excludes opening,
mounting, resolving the path and the output check. Every version read identical
bytes with identical callback/allocation counts, balanced allocations and no writes.

| Verity profile | File bytes | Old ms | Current ms | Throughput change |
| --- | ---: | ---: | ---: | ---: |
| 4 KiB SHA-256 | 1,572,941 | 4.145 | 3.554 | +16.61% |
| 1 KiB SHA-256, salted | 2,621,445 | 8.892 | 7.362 | +20.78% |
| 4 KiB SHA-512 | 716,800 | 1.426 | 1.244 | +14.64% |
| 4 KiB SHA-256, inline-data volume | 614,400 | 1.613 | 1.389 | +16.11% |

These results support retaining the portable SHA and salt-state changes. They
do not quantify ordinary writes, directory mutation or native AES performance.
No measured SHA or verified-read profile regressed; per-run p95 values remain in
the generated report, including the slower tail in one current 4 KiB read run.

## Linux VM timing qualification

The existing runner uses Apple Virtualization.framework and a `VZLinuxBootLoader`,
with hardware virtualization. A single diskless boot of Linux 6.12.94-0-virt
aarch64 used two virtual CPUs and 512 MiB, with the probe pinned to CPU 0. No image
or network device was attached. `tests/linux_timing.c` runs only as guest PID 1 and
prints the actual kernel, CPU capabilities, clocksource and raw measurements.

The guest selected `arch_sys_counter`. Across a 1,621.550 ms host interval between
receipt of console markers, guest MONOTONIC differed by -0.044582 ms and RAW by
-0.044624 ms. The 50/250/1,000 ms sleeps took 55.106/252.317/1,004.644 ms, satisfying
the specified bounds. Reported clock resolution was 1 ns; this is not a claim of
1 ns measurement accuracy. Console receipt introduces additional uncertainty.

The 10/20/40-million-iteration CPU loops scaled approximately 1:2:4 after the first
repeat. Their maximum/minimum spreads were 28.9%, 14.0% and 7.4%, respectively;
the first repeat was slower. Therefore the VM is suitable for controlled,
interleaved comparisons inside the same guest with warmup and longer workloads.
This short probe does not establish long-run stability or qualify a guest-versus-
host comparison. Both contenders must run inside that Linux guest for the next
portable-core comparison. Native FSKit/LXNU claims still require native adapters.

Preparation, exact invocation, raw console, host marker timestamps and the
derived evaluation are under the lab's
`artifacts/ext4-journal/core-review-timing/`. `LINUX_TIMING_RESULT=PASS` means only
that the probe completed; clock readiness follows from evaluating the saved
measurements, not from that marker alone.

## Performance acceptance

The agreed target is **at least 1.15 times Linux's filesystem throughput on matched
workloads**, with SHA/AES reported separately. A 15% throughput increase is not the
same as a 15% reduction in elapsed time. Report the ratio and both raw measurements.
The target remains open until a matched Linux comparison passes.

Use the same hardware, storage/image geometry, data set and CPU/power conditions.
Run one contender at a time. Match ext4 features, encryption modes, Merkle geometry,
I/O sizes, working-set size, concurrency and warm/cold-cache state. Match durability:
synchronous operations against synchronous operations, or deferred operations with
the same commit/fsync schedule and final durable sync. A no-op RAM-device flush is
not equivalent to Linux fsync on storage.

Measure sequential reads/writes, random 4 KiB reads/overwrites, large-directory
create/lookup/unlink, allocation in fragmented space, truncate/reclamation, encrypted
I/O and verified reads. Record per-workload throughput and latency, median/p95,
CPU time, device reads/writes/barriers, memory peak and write amplification. Use an
untimed warmup for warm-cache runs and independent resets for cold-cache runs;
interleave contenders over multiple samples and validate results outside timing.
Report every workload and regression; do not select only favorable cases.

Portable algorithm comparisons can run the same core harness under Linux, but
must distinguish library-call costs from Linux VFS/page-cache costs. Native product
claims additionally require FSKit and LXNU adapters with equivalent semantics.
The VM qualification above allows that same-guest work to proceed; functional
Linux roundtrips alone do not qualify a performance reference. A warm Linux
fs-verity page can retain its verified state, as described in the
[kernel's page-cache contract](https://docs.kernel.org/filesystems/fsverity.html#pagecache),
while the portable core currently verifies on each read. A library-versus-VFS
comparison must expose that difference;
native cache ownership cannot be inferred from hash throughput.

SHA/AES benchmarks must identify backend, CPU extensions, alignment, message size,
key setup inclusion and encryption mode. Reuse expanded keys across data units.
Use platform-supported accelerated providers and retain the portable hash fallback;
do not enable kernel SIMD just because userspace instructions are available. The
current change deliberately adds no platform-instruction dependency. Optimizing the
reference AES in `tests/crypto.h` would not accelerate a production adapter.

The immediate algorithm candidates after profiling are repeated mapping walks in
encrypted reads and avoidable base-device reads fully covered by pending journal
snapshots. Both require ownership/error-path tests before changing behavior.
Larger journal decomposition should follow these ownership boundaries and have a
separate behavioral acceptance; moving code between files alone is not a speedup.

## Same-guest ordinary reads

The first matched read comparison exposed repeated extent-tree walks as a major
cost on sparse files. A read now retains its checked extent leaf for the duration
of that call and uses binary search for subsequent ranges. Crossing an ancestor's
index boundary restarts validation; no mapping cache survives the call. The wire
decode/encode helpers have their unchanged byte-based bodies inlined, allowing
metadata scans to avoid an external call for each field. The change adds neither
platform instructions nor a data cache and uses the existing single scratch block.

The final probe runs both readers in the qualified Linux guest: two CPUs,
512 MiB, CPU 0 affinity, Linux 6.12.94-0-virt aarch64. Both read the same immutable,
checksummed 4 KiB-block ext4 device. The core uses exact buffered raw-device `pread`;
Linux uses its mounted ext4 file `pread`. Files are 16 MiB: one contiguous extent,
or alternating 4 KiB data and holes with 2,048 extents. There is no userspace image
preload, private data cache, mmap backend or added core readahead. Mount, lookup,
warmup and independent full-file validation are outside timing. This measures the
library/backend boundary against Linux VFS; it does not establish native adapter
performance or other file sizes and fragmentation patterns.

Seven interleaved samples per reader cover sequential 1 MiB and permuted 4 KiB
requests. Warm samples read 8 GiB and 1 GiB, respectively. Guest-cold samples read
2 GiB and 64 MiB, dropping guest caches before each complete 16 MiB pass outside
timing. Host/storage caches remain warm or uncontrolled. All warm profiles recorded
zero virtual-device reads for both contenders. Final throughputs below use median
elapsed time; paired ranges show every Linux/core time ratio rather than a
confidence interval. With seven samples, nearest-rank p95 is the maximum sample.

| File | Cache | Access | Linux MiB/s | Core MiB/s | Core / Linux | Paired range |
| --- | --- | --- | ---: | ---: | ---: | ---: |
| Contiguous | Warm | Sequential | 32,987.3 | 31,705.8 | 0.961 | 0.934–0.974 |
| Contiguous | Warm | Random | 4,469.0 | 4,367.9 | 0.977 | 0.948–1.003 |
| Contiguous | Guest-cold | Sequential | 9,415.0 | 8,719.0 | 0.926 | 0.905–0.974 |
| Contiguous | Guest-cold | Random | 173.5 | 172.4 | 0.994 | 0.971–1.012 |
| Sparse | Warm | Sequential | 24,355.2 | 16,364.3 | 0.672 | 0.666–0.678 |
| Sparse | Warm | Random | 10,646.0 | 1,877.7 | 0.176 | 0.172–0.182 |
| Sparse | Guest-cold | Sequential | 6,669.3 | 8,186.5 | **1.227** | **1.205–1.281** |
| Sparse | Guest-cold | Random | 325.8 | 286.3 | 0.879 | 0.798–1.026 |

Only sparse guest-cold sequential reading clears 1.15 in every pair. **The general
15% target is not achieved.** For sparse warm sequential reading, core throughput
improved 10.40 times over the initial baseline; warm random reading improved 29.3%.
The initial sequential batches were shorter; the final and intermediate cursor
runs use the longer batches described above. These before/after figures normalize
bytes and elapsed time, and must not be confused with the Linux ratios.

Per GiB of sparse sequential logical data, core read callbacks fell from 655,360
to 133,888 and callback bytes from 2.50 GiB to 0.511 GiB. Allocations stayed at
1,024 per GiB, one scratch allocation per 1 MiB request. The 4 KiB random API calls
still re-read and revalidate external mapping nodes and cannot share that scratch.
The remaining warm random gap is substantial; it is not covered by the one passing
profile.

A diagnostic third contender reads the contiguous file's checked physical range
directly through the same backend, bypassing every core filesystem operation.
Its throughput relative to Linux was 0.933 warm sequential, 0.987 warm random,
0.930 guest-cold sequential and 1.004 guest-cold random. This measured backend
limit leaves no demonstrated 15% headroom for contiguous reads through this
interface. Removing more mapping instructions cannot remove its I/O boundary.
This is evidence about the present harness, not a proof about all backends or
native FSKit/LXNU performance.

Further work belongs at explicit ownership boundaries: bounded mapping reuse
across requests needs invalidation for mutation, truncation and deferred journal
state; batching sparse I/O needs an adapter capability or carefully bounded
gathering. FSKit resource I/O and XNU's existing UBC/clustered read path must be
measured separately. A private replacement for the native page cache, an mmap-only
test backend or disabling Linux caching would not establish the requested product
advantage. Encrypted and verity reads remain separate profiles.

The range regression covers checked-leaf reuse, fresh mapping contents on a later
call, corruption outside the requested range, partial I/O errors, allocation
failure, ancestor boundaries and EOF across the supported block sizes. The final
focused ASan/UBSan selection (`image-reader`, `file-read-ranges`, `malformed-images`)
passes all three tests. Both final guest readers validated their output, all 140
sample rows were complete, the guest powered off cleanly and the image hash was
unchanged. The single end-of-batch ASan/UBSan regression then passed **711/711**
tests in 21 minutes 12 seconds, with no failures or whole-test skips. The existing
29 in-test applicability skips remain separate: two inode-body checks on 128-byte
inodes and 27 directory-split transitions assigned to other fixture profiles.
Unsigned arm64e and x86_64 kext compilation, including the 2 KiB frame check, and
the final style/diff checks pass. No extension was installed or booted for this
batch. GitHub CI for this optimized source is still running at report collection;
earlier green CI does not close that run.

Commands, raw console, all samples, CPU/I/O counters, source/binary identities and
checks are in the lab's `artifacts/ext4-journal/read-compare-baseline-2/`,
`read-compare-cursor/` and `read-compare-inline/`. The last directory is the final
implementation; earlier measurements remain preserved. Local test/build logs are
under this repository's `artifacts/checks/read-performance/`. The preparation and
analysis commands are documented in [DEVELOPMENT.md](DEVELOPMENT.md).

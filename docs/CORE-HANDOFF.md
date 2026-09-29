# Core review handoff

The portable core's functional acceptance queue is complete with the documented
limitations in [ACCEPTANCE.md](ACCEPTANCE.md). Do not restart the historical
implementation queue or infer completion from old test totals. The current task is
review, code organization and performance, described in [CORE-REVIEW.md](CORE-REVIEW.md).

Read the workspace and repository `AGENTS.md`, `README.md`, `ARCHITECTURE.md` and
`DEVELOPMENT.md`. Work in `/Users/darekhta/Development/machlin/ext4` on `development`;
inspect Git state before changing it. The main agent owns implementation, review,
test design and acceptance. Delegate prepared builds/tests to GPT-6 Luna and any VM
operations to GPT-6 Sol with exact paths, commands and one owner per VM. Lab and VM
commands use `/Users/darekhta/Development/machlin/lab` as their working directory.

The user has a constrained usage budget. Develop coherent changes, then run their
applicable checks once. Preserve prior evidence, use existing fixtures, and do not
repeat filesystem acceptance for documentation or mechanical formatting alone.
Keep source revisions in Git and measurements, hashes and generated inputs in
ignored artifacts. Verify personal Git identity and signing before publication.

The performance target is 15% higher throughput than Linux on the same hardware
and filesystem workload with equivalent durability. SHA/AES results are separate;
old/new core measurements cannot establish the Linux target. Read the current
review report for evidence, remaining coverage and the comparison protocol.

The same-guest ordinary-read comparison now includes `ext4_read_held`, with an
explicit inode lifetime and bounded metadata cache. Mapping traversal/validation
is separate from byte delivery. Transactions invalidate cached snapshots before
publication; explicit refresh, memory-pressure discard, release and unmount own
cleanup. Stateless `ext4_read` retains its original contract. Native adapters do
not yet use this API; integrate it only with their owning lifetime/serialization.
FSKit byte reads can use the held-read entry point. XNU/LXNU must retain native
UBC/cluster I/O and would need held mapping queries to share the leaf cache with
block-map requests, rather than routing regular I/O around the native page cache.

The read path implements bounded physical I/O coalescing across logical holes
in `core/read_io.c`, with in-place expansion and no extra data allocation. A cursor
advances at sequential extent boundaries and falls back to binary search for other
seeks. It only reuses fully validated metadata; lookahead performs no device I/O.
Sparse warm sequential core throughput is now 31,343.4 MiB/s versus Linux's
23,943.9 MiB/s, a 30.9% advantage and 91.0% above the held-read baseline. Sparse
warm random is 49.4% faster than Linux, and guest-cold sequential is 63.5% faster.
These three profiles clear 15% in every pair; the other five do not. Keep all eight
results and raw-backend diagnostics in the review. Cold passes discard the core
metadata cache and charge reconstruction. Do not claim an overall Linux or native
adapter win, or repeat timing qualification without an invalidating change.
Contiguous warm sequential throughput is already essentially the raw backend's
throughput; further algorithm changes cannot be assumed to deliver 15% there.

The held-read batch passed the full 712-test ASan/UBSan regression and unsigned
FSKit plus arm64e/x86_64 kext compilation. Do not repeat those checks for report or
CI-diagnostic changes. The original CI format job rejected one mutated relaxed
casefold image; its stdout and image were lost by the old checker. Local reruns
and the complete diagnostic CI format job passed. Preserve this as an unexplained
failure: improved logging and a passing rerun do not prove a filesystem fix.
The later batching/cursor batch also passed 712/712 ASan/UBSan tests, with the same
29 explicit applicability skips, unchanged captured source/binary hashes and both
kext plus unsigned FSKit builds. Preserve the separate evidence for the two batches
and compiled revisions when continuing work.

The following SHA-256 instruction batch enables ACLE rounds only in eligible
little-endian ARM64 userspace builds; kernel and other builds retain the portable
transform. Its measured hash speedup is 4.4–4.8 times the scalar core, separate from
the Linux read target. A Meson test explicitly retains scalar-path coverage, and
the shared hash vectors now cover 16 alignments. CRC is unchanged: its alignment
prototype regressed short unaligned userspace inputs and was rejected. See the
review for measurements. The final source passed 18/18 focused ASan/UBSan checks
and both unsigned kext plus FSKit builds, recorded in
`artifacts/checks/sha-instructions/`. The earlier 712-test read regression does not
validate these later edits; track the new full CI matrix separately without holding
up development while it runs.

The current batch gives verity reads a context with independent data/tree mapping
cursors and separates current journal-view delivery from commit/checkpoint code.
Pending or committed snapshots supply their own bytes without rereading obsolete
home data. The new independently authored fragmented verity profile crosses two
external leaves; measured throughput is about 49% higher than the preceding core,
with 807-to-32 allocations per file pass. RAM write profiles show substantially
fewer backend reads with deferred/lazy commits, unchanged writes/flushes and matching
checked images; those timings do not establish the Linux or native-adapter target.
See the current review section and `artifacts/checks/read-write-state/`. Final full
regression and native builds for this batch are pending acceptance; the source
is stable while those checks run. The ordinary Linux +15% target remains open.

Both adapters remain read-only. FSKit integration precedes LXNU policy; signing
is deferred. No host kernel, boot-policy, NVRAM or system-file changes are authorized.
Native cryptography and write/page-cache integration require their own acceptance.

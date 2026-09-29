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

Sparse warm random core throughput is 15,222.5 MiB/s versus Linux's 9,654.0 MiB/s:
57.7% faster, with every pair at least 55.4% faster. It is 8.11 times the prior core
run. Sparse guest-cold sequential reading is 20.8% faster than Linux. The other six
profiles do not clear the 15% target; keep all eight results and raw-backend
diagnostics in the review. Cold passes explicitly drop the core's new metadata
cache and charge reconstruction. Do not claim an overall Linux or native-adapter
win, or rerun timing qualification without an invalidating change. Sparse warm
sequential I/O still makes one backend call per data extent; physical adjacency
across holes is a possible bounded batching opportunity, not an accepted change.

The held-read batch passed the full 712-test ASan/UBSan regression and unsigned
FSKit plus arm64e/x86_64 kext compilation. Do not repeat those checks for report or
CI-diagnostic changes. The original CI format job rejected one mutated relaxed
casefold image; its stdout and image were lost by the old checker. Local reruns
and the complete diagnostic CI format job passed. Preserve this as an unexplained
failure: improved logging and a passing rerun do not prove a filesystem fix.

Both adapters remain read-only. FSKit integration precedes LXNU policy; signing
is deferred. No host kernel, boot-policy, NVRAM or system-file changes are authorized.
Native cryptography and write/page-cache integration require their own acceptance.

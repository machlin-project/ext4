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

The same-guest ordinary-read comparison is now implemented. Checked extent leaves
are reused within each read, and wire accessors are inline. The overall Linux target
remains open: one sparse sequential profile exceeds it, while random sparse reads
still repeat mapping work across API calls. The contiguous raw-backend diagnostic
shows that this harness has no demonstrated 15% headroom even without filesystem
work. Preserve all eight reported profiles and keep the raw diagnostic separate
from core results. Do not rerun VM timing qualification or the initial baseline
without a change that invalidates them. Further cross-request mapping reuse must
have an explicit mutation/journal invalidation contract; native cache and I/O costs
belong to the adapters. See the ordinary-read section of the review for exact data.

Both adapters remain read-only. FSKit integration precedes LXNU policy; signing
is deferred. No host kernel, boot-policy, NVRAM or system-file changes are authorized.
Native cryptography and write/page-cache integration require their own acceptance.

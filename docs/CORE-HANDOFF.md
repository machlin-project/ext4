# Core development handoff

Continue implementing and accepting the portable ext4 core in this repository.
The user wants a finished, robust core before FSKit integration, followed by LXNU.
This is an implementation task: carry the remaining core work through review,
independent interoperability checks and acceptance. Do not stop at a plan or a
passing checkpoint. The user and the previous agent will review the result later.

## Working rules

- Read the workspace and repository `AGENTS.md`, then `README.md`,
  `docs/ARCHITECTURE.md`, `docs/ACCEPTANCE.md` and `docs/DEVELOPMENT.md`.
  The acceptance queue is authoritative; test counts are not completion percentages.
- The local repository is `/Users/darekhta/Development/machlin/ext4`, on
  `development`. Keep that branch. Inspect Git status/history before making changes.
  Do not overwrite another worker's changes or copy a `.git` directory.
- Use Meson/Ninja and the selected Xcode clang-format. Keep declarations at block
  starts, explicit environment allocations, bounded arithmetic and named disk fields.
  The core must remain independent of libc allocation, FSKit, Foundation and XNU types.
- The main agent owns architecture, implementation, test design, diagnosis and review.
  Delegate prepared CLI builds, fixtures, tests and evidence collection to
  `gpt-6-luna`; delegate VM preparation/management to `gpt-6-sol`. Give absolute
  working directories, exact commands and success criteria. One owner per VM.
- Develop coherent batches. Do not restart broad validation after every small edit.
  Freeze binaries and inputs for an active run. Documentation or formatting alone
  does not justify repeating completed filesystem acceptance.
- Standalone commands run from this repository. VM/harness commands run from
  `/Users/darekhta/Development/machlin/lab`. Host boot policy, kernel, NVRAM and
  system files are outside scope. Signing is deferred by the user.
- Verify personal repository-local Git identity/signing and GitHub authentication
  before publication. The remote uses `git@github-personal.com`. Keep generated
  images, credentials, logs and binaries out of commits.

## Current implementation

Ordinary reads/writes, allocation and restartable truncation, open-unlinked
lifetime, namespace mutation, extent/indirect maps, HTree/LARGEDIR/DIR_NLINK,
raw xattrs/EA_INODE, INLINE_DATA, BIGALLOC, internal/external journals and both
orphan representations have substantial portable, independent and Linux evidence.
The last completed full CI regression has 552 unique passing tests across six
jobs, with no JUnit failures/errors/skips. Preserve the explicitly documented
reader-only xattr fsck and fixture-normalization exceptions.

The checkpoint being handed over improves fast-commit recovery:

- Collect referenced inodes, sort/deduplicate once, then use binary lookup.
- Merge logged physical ranges into an exclusion union; use binary overlap lookup.
  This excludes data from metadata allocation, not global cross-inode ownership checking.
- Reuse one journal block per sequential pass while checking every record's saved
  identity. Invalidate the cache after a partially failed read and between passes.
- Combine name lookup and insertion-slot preparation into one full directory scan.
  An existing name must retain its inode identity and link count; conflicting owners reject.
- Validate allocation bitmaps by visiting protected range intersections and counting
  bits by byte. Preserve checksum, metadata protection, partial-cluster/padding and
  free-count checks, including lazy initialization. No validation cache was added.

Semantic record order, durability ordering and the 256-snapshot transaction bound
remain unchanged. Directory name replay still performs a full scan for each
operation; it has not become a linear-time namespace algorithm. Do not introduce
a plain "already validated" flag without accounting for all intervening block changes.

The new protocol fixtures create 256 files at 1 KiB and 1,024 at 4 KiB, revisit
inode/range records in reverse order, overlap legitimate ranges and repeat names.
They use a 1 MiB fast area in an 8 MiB journal, still within 32 MiB images.
Large fault/resource cases sample positions and include all durability barriers
and adjacent events. Existing smaller exhaustive cases remain exhaustive.

## Immediate continuation

First read the handoff commit, current CI and the generated reports below. Finish
acceptance of this checkpoint before building another optimization on top of it.
Do not regenerate unchanged fixtures or repeat already accepted local cases.

Local paths are relative to the ext4 repository unless stated otherwise:

- Final fixtures: `artifacts/fast-commit-large-prefix-final-fixtures/`.
  Helper plus all 15 profiles passed generation and strict reference-image fsck.
- Current sanitized/freestanding build: `artifacts/fast-commit-bitmap-build/`.
- Current evidence: `artifacts/checks/fast-commit-bitmap/`.
  The two-case gate passed: `allocation-and-growth` in 34.19 seconds and
  `fast-commit-sampled-faults-large-prefix-4k` in 431.76 seconds, under its unchanged
  600-second limit. The remaining selection contains 127 different cases; together
  these are 129 unique cases, all passing. The terminal summary is `results.json`;
  source, binary, frozen-copy and fixture-report hashes remained unchanged.
- Two earlier runs timed out on that same large fault case. Their 102/103 and
  104/105 results remain under `artifacts/checks/fast-commit-large-prefix-accepted/`
  and `artifacts/checks/fast-commit-large-prefix-fixed/`. Despite the first path's
  name, neither run is accepted. The latter contains a bounded process sample
  showing repeated directory validation and bitmap inspection inside replay.
- The compiler/source/binary/input hashes are in generated reports. Check them
  before reusing evidence. Keep source revisions in Git, not a custom source ledger.

After local acceptance, the following steps remain. The previous agent deliberately
left them for this handoff; do not infer that preparation means they have run.

1. Independently verify all 15 recovered outputs with strict nonrepairing fsck,
   exact namespace/data/attribute reads and an unchanged second clean recovery:

   ```sh
   python3 tests/check_fast_commit.py \
     --tools-root /Users/darekhta/Development/machlin/lab/vendor/e2fsprogs-ext4/build \
     --fixtures artifacts/fast-commit-large-prefix-final-fixtures \
     --recover artifacts/fast-commit-bitmap-build/ext4-recover \
     --output artifacts/fast-commit-large-prefix-independent
   ```

2. Measure release builds without sanitizers on a quiet host. Compare the parent
   of this handoff checkpoint with the checkpoint, using the same current
   `tests/fast_commit_recovery.c` driver and the same final fixtures. Use ordinary
   `git archive` into an ignored directory, not a copied Git database. Run
   `ext4-fast-commit-recovery-test PENDING EXPECTED --benchmark` for `1k`,
   `indirect-1k`, `large-prefix-1k` and `large-prefix-4k`: one checked warmup and
   31 measured recoveries each. Alternate version order across profiles. Report
   median/p95, reads, allocations, peak core allocation and durability events.
   Check full recovered-image SHA equality on separate copies using both versions.
   Older recovery executables are preserved in
   `artifacts/checks/fast-commit-large-prefix-baseline/`; they do not contain the
   new benchmark driver. Timing excludes resetting/comparing/hashing the image
   and measures a memory-backed device, not mounted or real-device throughput.

3. Have Sol recheck and explicitly hand over the idle Linux reference VM to Luna.
   The existing readiness report is
   `artifacts/checks/fast-commit-large-prefix-native-prep/readiness.json`.
   No large-prefix native boot has run. Execute this from the absolute lab directory:

   ```sh
   python3 /Users/darekhta/Development/machlin/ext4/tests/run_linux_fast_commit.py \
     --lab /Users/darekhta/Development/machlin/lab \
     --prepared /Users/darekhta/Development/machlin/lab/artifacts/ext4-journal/external-journal-native-retry2 \
     --runner /Users/darekhta/Development/machlin/lab/.cache/linux-reference/linux-vm-external \
     --output /Users/darekhta/Development/machlin/lab/artifacts/ext4-journal/fast-commit-large-prefix-readback \
     --recover /Users/darekhta/Development/machlin/ext4/artifacts/fast-commit-bitmap-build/ext4-recover \
     --large-prefix-fixtures /Users/darekhta/Development/machlin/ext4/artifacts/fast-commit-large-prefix-final-fixtures
   ```

   Require two actual Linux readback boots, every created file's contents/metadata,
   sparse data and hardlink identity, clean unmount and strict fsck. This establishes
   Linux consumption of core output, not native generation or direct Linux replay
   of these independently serialized pending logs.

4. Collect the new push-triggered GitHub CI to terminal state, including independent
   reports and upload/cleanup. Expected inventory: 563 unique tests, core 289,
   indexed 141, namespace 63, rename 38, removal 27, orphan-file 5. Do not manually
   dispatch a duplicate run or cancel an older one just to make a status look green.
   Fix real failures, preserving their evidence. Update acceptance docs only from
   completed results.

Use fresh output paths if a report directory already exists. The local toolchain
uses `PATH=/opt/homebrew/bin:/usr/bin:/bin:/usr/sbin:/sbin` and `CC=/usr/bin/clang`.

## Finish the core

Continue the actual remaining queue in `docs/ACCEPTANCE.md`:

- Fast-commit BIGALLOC, casefold, quota and 64 KiB profiles, ownership-corruption
  rejection, interrupted e2fsck replay of ordinary logs and interrupted Linux replay
  of two native captures are accepted (`tests/run_linux_log_writes.py`). Torn states
  of Linux's range-heavy fast-commit replay fail closed, by product decision (see
  `docs/ARCHITECTURE.md`). Preserve unresolved
  native-reference failures; a repaired diagnostic image is not an accepted oracle.
- Growth, fragmentation, allocator and recovery cost, memory and write
  amplification are measured ("Scale measurement evidence"). Remaining: whole-tree
  index classification on each indexed operation. Deferred group commit, ordered
  data and lazy checkpointing are in place (`ext4_write_options`). Back performance
  claims with equivalent measured workloads.
- Expand required geometry/format compatibility. MMP, quota/project accounting and
  limit enforcement, casefold, keyless encryption and verity reading, enabling and
  measurement, with built-in signatures checked by an adapter callback, and reading,
  writing and encrypting objects with keys from adapter callbacks are implemented.
  Linux's no-key names remain; do not silently remove them.
- Sustained mixed-operation/crash sequences, operations fuzzing and
  checksum-repairing journal fuzzing are in place; keep extending them. Fix
  discovered contracts at their owning layer, not in the test harness.

The core currently has a serialized resource owner. Native locking, page-cache
coordination, authorization and adapter lifetime acceptance come later. FSKit and
LXNU do not take priority over the remaining portable-core queue. Neither unsigned
adapter compilation nor a passing core test proves a stock macOS mount or custom
kernel runtime contract.

Keep focused signed commits on `development`, publish concrete checkpoints, and
report completed behavior, measured evidence and remaining risks. Do not claim
the core is finished merely because the current tests pass or unsupported inputs
are rejected safely.

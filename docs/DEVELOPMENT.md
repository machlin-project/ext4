# Development

## FSKit app

`make fskit` uses XcodeGen and the selected Xcode toolchain to build an unsigned
app under `artifacts/fskit/DerivedData/Build/Products/Debug/Machlin ext4.app`.
XcodeGen generates its project from `adapters/fskit/project.yml`; the generated
project and build outputs stay ignored. macOS 26.4 is the deployment target.

For a signed build, run `python3 scripts/build_fskit.py --team PERSONAL_TEAM_ID`.
Add `--provision` to let Xcode retrieve profiles using its configured account.
The extension needs a profile with the FSKit Module capability. An unsigned build
does not establish that macOS will load the extension.

Install and enable the app only in the dedicated disposable guest for development
acceptance. Use the platform's File System Extensions controls, then mount the
identified fixture device read-only with `mount -F -t machlin_ext4 -o rdonly`.
The exact guest device must be verified before mounting. Run `ext4-mounted-test`
against that mount and verify clean unmount and unchanged image bytes afterward.
These installed steps have not yet passed; signing is deferred.

## Kernel extension

Complete portable-core acceptance before resuming adapter development. FSKit is
the first platform priority after the core; LXNU policy follows it.

`make kext` builds the same C core plus `adapters/xnu/` using the selected Xcode
kernel headers. Outputs live in `artifacts/kext/arm64e/`, including the unsigned
`MachlinExt4.kext` and a read-only `mount_machlin_ext4 DEVICE MOUNTPOINT` helper.
Use `python3 scripts/build_kext.py --arch x86_64` for the Intel compilation check.
Neither command installs or loads the extension.

Loading and mounting must use a separate disposable kernel-development guest,
with its actual kernel and device identified in the report. The stock FSKit
guest remains separate. The lab's ordinary collection builder validates the
stock kext inventory; do not alter that invariant or silently inject this kext
into existing acceptance artifacts. The read-only arm64e profile has separate
custom-kernel evidence in the acceptance document.

After verifying the loaded module and exact disposable device, use the built
`mount_machlin_ext4 DEVICE MOUNTPOINT`, then run `ext4-mounted-test MOUNTPOINT`
as an ordinary user. Unmount normally and compare the raw image bytes. The
privileged `ext4-mounted-lifetime-test MOUNTPOINT` additionally checks busy
unmount with a retained descriptor or mapping; it unmounts the volume on success.
Run these tools only against the dedicated fixture mounts. Device identifiers
can change on every guest boot and must not be inferred from an earlier report.

## Metadata fuzzing

`EXT4_BUILD_FUZZER=ON` builds `ext4-image-fuzzer` using Clang's libFuzzer runtime,
ASan and UBSan. Use an LLVM installation that includes libFuzzer; the selected
Xcode installation currently lacks `libclang_rt.fuzzer_osx.a`. A separate build
directory under `artifacts/` keeps this compiler isolated from the driver builds.

Pass `--image=ABSOLUTE_PATH` to a generated `ext4-4k.img` and a corpus directory.
The fuzzer maps the reference image read-only. Its small inputs describe in-memory
metadata mutations and optional checksum repair, allowing malformed structure
tests to reach checks after CRC validation. Each iteration limits resource reads
and allocations. Start with a single worker, `-max_total_time=60 -timeout=5
-max_len=512 -rss_limit_mb=512`, and preserve findings under ignored artifacts.
This is a bounded mutation test, not complete disk-format or concurrency coverage.

## Journal tests and offline recovery

`make test` also runs the journal durability suites for the selected 1 KiB and
4 KiB images. See [TESTING.md](TESTING.md) for the fault and interoperability
matrix. To retain pending journal images for an independent replayer, create a
new empty export directory and pass it as the second argument:

```sh
mkdir artifacts/journal-writer-exports
.build/ext4-journal-test artifacts/fixtures/ext4-4k.img artifacts/journal-writer-exports
.build/ext4-journal-test artifacts/fixtures/ext4-1k.img artifacts/journal-writer-exports
```

The test opens its input read-only and exports ten independent images and their
block/feature metadata. Export files must not already exist. The offline utility
`.build/ext4-recover --write IMAGE` explicitly opens an unmounted regular image for
recovery, obtains an advisory exclusive lock, and uses `F_FULLFSYNC` on macOS or
`fsync` on Linux for durability. Use independent copies for destructive tests;
the caller must exclude mounted or other noncooperating users of the image.

Generate and check independent logs using the same prepared e2fsprogs build:

The modern orphan-file journal-only oracle requires e2fsprogs 1.47.2 or newer.
Earlier versions leave `orphan_present` set after successful journal replay and
cleanup; the following strict read-only check correctly reports that unfinished
state. See the [upstream fix](https://github.com/tytso/e2fsprogs/blob/v1.47.2/doc/RelNotes/v1.47.2.txt).
CI builds the pinned 1.47.3 release, matching the lab, rather than using the
runner's older system package. Oracle checks still require clean metadata and
do not repair consistency errors after journal replay.

```sh
python3 tests/generate_journal_fixtures.py --tools-root /path/to/e2fsprogs/build \
  --source artifacts/fixtures/ext4-4k.img --output artifacts/journal-fixtures
python3 tests/check_journal_recovery.py --fixtures artifacts/journal-fixtures \
  --recover .build/ext4-recover --e2fsck /path/to/e2fsprogs/build/e2fsck/e2fsck \
  --output artifacts/journal-recovery
```

The Linux roundtrip harness is an optional Machlin lab acceptance command. Run
`tests/run_linux_journal.py` by absolute path with the lab as the working directory.
It takes `--lab`, `--exports`, `--module-report` and a new `--output` directory.
It uses the prepared musl compiler/linker, pinned Linux kernel, signed ephemeral
VZ runner, and matching modules from the verified module report. The report needs
`kernel_release` and a `modules` object containing each module's lab-relative
`path` and `sha256`, including the dynamically selected `crc32c_generic` provider.
Sol prepares that environment; Luna executes the bounded sequential VM runs.
The harness creates a separate writable copy for every case, with no network.
It requires guest kernel and success markers, actual pending Linux-authored
transactions, independent content/metadata comparison, and e2fsck.

The filesystem adapters do not enable writes or invoke offline recovery yet.
In particular, FSKit metadata-flush completion has not been established as a
durable device-cache barrier; it cannot satisfy the write capability by assumption.

## Orphan recovery tests

`ext4-orphan-test IMAGE...` constructs linked-truncate intents in RAM copies and
checks cleanup, malformed lists, stale summaries, interrupted recovery and retries.
It is included in CTest for the ordinary writable profiles. Use `--smoke --export
NEW_EMPTY_DIRECTORY` to export successful pending/clean states without the fault
loops. The indirect profile additionally exports a 257-leaf map that cannot be
removed by the bounded atomic truncate API. Check these exports with:

```sh
python3 tests/check_orphans.py --linked-exports artifacts/orphan-linked-exports \
  --recover .build/ext4-recover --tools-root /path/to/e2fsprogs/build \
  --output artifacts/orphan-linked-independent
```

From the explicit lab working directory, `run_linux_journal.py --orphans` takes
the same prepared environment and independently checked clean truncate exports
as `--truncate`. Linux creates and keeps open six objects, removes their final
names, commits the orphan list and powers off. Its report marks them generated,
not recovered. Pass that report to `check_orphans.py --fixtures REPORT.json`
with `--recover`, `--tools-root` and a new `--output` directory. It operates on
copies and retains every source image. `ext4-orphan-test --pending IMAGE...`
also runs the modeled fault matrix against those untouched Linux images.
Only one prepared Linux runner may be operated at a time.

Modern orphan-file profiles additionally need tune2fs in the same e2fsprogs build
or on PATH. Generate the ordinary and mapped fixtures, then enable their tests:

```sh
python3 tests/generate_orphan_file_fixtures.py --fixtures artifacts/fixtures \
  --tools-root /path/to/e2fsprogs/build --output artifacts/orphan-file-fixtures
python3 tests/generate_orphan_file_fixtures.py --fixtures artifacts/fixtures \
  --tools-root /path/to/e2fsprogs/build --case ext4-indirect-1k.img --blocks 17 \
  --output artifacts/orphan-file-fixtures/mapped
python3 tests/generate_orphan_file_fixtures.py --fixtures artifacts/fixtures \
  --tools-root /path/to/e2fsprogs/build --case ext4-indirect-1k.img --blocks 512 \
  --output artifacts/orphan-file-fixtures/maximum
cmake -S . -B .build -DEXT4_ORPHAN_FILE_TESTS=ON
```

`EXT4_ORPHAN_FILE_FIXTURES` selects another fixture directory. The generator's
default ten profiles require the base fixture generator's `--extended --inode128`.
Use `ext4-orphan-test --orphan-file` or `--mixed` on these images; `--pending`
accepts Linux-authored modern or legacy recovery states. The same independent
checker handles both formats and verifies the modern file is emptied without
changing its fixed inode or map. Keep each export mode in its own new directory.

`ext4-orphan-test --live IMAGE...` exercises live multi-transaction shrink and
all operation faults; add `--large` for a sparse indirect tree exceeding atomic
capacity. For independent evidence use `--smoke --export NEW_EMPTY_DIRECTORY`,
then run:

```sh
python3 tests/check_resize.py --exports artifacts/live-exports \
  --recover .build/ext4-recover --tools-root /path/to/e2fsprogs/build \
  --output artifacts/live-independent
```

Keep large-profile exports separate and pass `--large` to the checker. Its
successful report can be selected for `run_linux_journal.py --live-truncate`
from the lab directory. That mode verifies the completed file or recovers a
selected pending live intent, checks size/blocks/permissions/timestamps and all
retained bytes, then grows and writes the file in Linux for reverse replay.
Pending inputs require a generated selection report pinning their actual hashes
and the independently verified new outcome. Use the same Sol/Luna handoff.

## Extended-attribute reader tests

Generate and inspect xattr reader fixtures with the same independent tools:

```sh
python3 tests/generate_xattr_fixtures.py --tools-root /path/to/e2fsprogs/build \
  --output artifacts/xattr-fixtures
cmake -S . -B .build -DEXT4_XATTR_FIXTURES="$PWD/artifacts/xattr-fixtures"
cmake --build .build --target ext4-xattr-test
ctest --test-dir .build -R '^xattr-reader-' --output-on-failure
mkdir artifacts/xattr-exports
for image in artifacts/xattr-fixtures/*.img; do
  .build/ext4-xattr-test --export artifacts/xattr-exports "$image" "${image%.img}.expected"
done
python3 tests/check_xattrs.py --fixtures artifacts/xattr-fixtures/report.json \
  --exports artifacts/xattr-exports --tools-root /path/to/e2fsprogs/build \
  --output artifacts/xattr-independent
```

The checker distinguishes clean unknown-namespace exports from synthetic shared-value
reader compatibility images. The latter deliberately fail e2fsck and are never
reported as clean filesystem acceptance. No input image is repaired or modified.

## Inode and file-write tests

`ext4-write-test` opens source fixtures read-only and mutates separate modeled
volatile/persistent images. It checks selective inode updates, bounded allocated
range overwrites, validation failures, resource faults and every commit/finish
interruption. Optional `--export NEW_EMPTY_DIRECTORY` writes clean finished
images for independent inspection. Pass the desired image paths after the option;
the tool refuses to overwrite export files. It also accepts 128-byte inode
fixtures, where extended timestamp fields are absent. The 64 KiB reader fixture
has no journal and is not a writable test image.
Use `tests/generate_fixtures.py --extended --inode128` to reproduce the complete
reader/write profile, and configure `-DEXT4_EXTENDED_TESTS=ON -DEXT4_INODE128_TEST=ON`.
The generator verifies the legacy inode size and omits unrepresentable timestamp
fixture fields; tests verify that such updates reject instead of truncating.

`tests/check_writes.py --exports DIRECTORY --tools-root E2FSPROGS_BUILD --output NEW_DIRECTORY`
checks the exports with debugfs, dumpe2fs and nonrepairing e2fsck. It verifies exact
file bytes, hardlinks, full-width ownership, mode and raw timestamp encodings,
and requires unchanged source hashes. Its generated `report.json` can be passed
as `--exports` to `tests/run_linux_journal.py --file-writes`. Repeat `--case NAME.img`
to select an explicit Linux acceptance profile. As with journal tests, run that
harness from the lab working directory after Sol prepares the isolated runner.
Linux independently mounts and checks the written files and attributes, then
authors a new committed transaction for the portable replayer to recover.

`ext4-write-test --allocation IMAGE...` runs the allocation, sparse growth,
extent/indirect mapping, full-disk and fault suites. Combine it with
`--export NEW_EMPTY_DIRECTORY` to retain clean grown images. The independent checker
is `tests/check_allocation.py --exports DIRECTORY --tools-root E2FSPROGS_BUILD
--output NEW_DIRECTORY`.

Generate deliberately nonzero unwritten backing data with
`tests/generate_allocation_fixtures.py --fixtures EXTENDED_FIXTURE_DIRECTORY
--tools-root E2FSPROGS_BUILD --output NEW_DIRECTORY`. Enable its CTest suite with
`-DEXT4_UNWRITTEN_TESTS=ON -DEXT4_ALLOCATION_FIXTURES=ABSOLUTE_NEW_DIRECTORY`.
The generator preserves its source images and records each independent mapping,
the exact modified data blocks, tool output and nonrepairing e2fsck result.

`tests/run_linux_journal.py --allocation` accepts the allocation checker's
`report.json` as `--exports`. It verifies the full grown file in Linux, extends
it with a new allocation, commits and powers off without unmount. The returned
image must contain a pending Linux journal that the portable core can recover,
with exact file bytes/attributes and e2fsck verified afterward. The same lab
working directory, explicit profile selection and Sol/Luna VM handoff apply.

The `Portable filesystem` GitHub Actions workflow runs on development/main pushes
and pull requests. Separate Ubuntu jobs cover the base core, orphan files and
namespace mutations, each generating fresh fixtures and building with Clang and
ASan/UBSan. They run disjoint CTest suites, inspect mutation exports independently
and recover debugfs-authored journals. Reports and logs are retained; successfully
verified image exports are released between stages, and namespace exports are
checked one source profile at a time to bound disk use.
CI uses `RelWithDebInfo` with both sanitizers enabled and two concurrent CTest
workers; fixture and fault coverage is identical to Debug. Use the same build
type locally when reproducing CI timing.
This portable CI does not replace selected-Xcode formatting, unsigned platform
builds, actual macOS mounts, or the separately identified kernel/LXNU VM tests.

Namespace tests use the ordinary writable and orphan-file profiles. Add six small
multi-group fixtures and their exhaustion/group-transition suites with:

```sh
python3 tests/generate_namespace_fixtures.py --tools-root /path/to/e2fsprogs/build \
  --output artifacts/namespace-fixtures
cmake -S . -B .build -DEXT4_NAMESPACE_TESTS=ON
cmake --build .build --parallel 4
ctest --test-dir .build -R '^namespace' --output-on-failure
```

`EXT4_NAMESPACE_FIXTURES` selects another fixture directory. The ordinary namespace
and basic indexed-directory tests are always enabled; the modern suite follows
`EXT4_ORPHAN_FILE_TESTS`. To independently inspect one profile after its fault suite:

```sh
mkdir artifacts/namespace-exports
.build/ext4-namespace-test --smoke --export artifacts/namespace-exports \
  artifacts/fixtures/ext4-4k.img
python3 tests/check_namespace.py --fixtures artifacts/fixtures \
  --exports artifacts/namespace-exports --recover .build/ext4-recover \
  --tools-root /path/to/e2fsprogs/build --output artifacts/namespace-independent
```

For the small images, also export `--smoke --groups` and `--exhaust` into that
source profile's new export directory before checking it. Exports contain both
uncommitted and durable-commit interruptions. The independent checker requires
the old and new states respectively, not merely an arbitrary consistent outcome.

From the lab directory, `run_linux_journal.py --namespace` takes that checker's
`report.json`. Pass an explicit prepared `--recover` executable, `--case` names,
the pinned `--module-report`, `--lab` and a new `--output`. Select 1/2/4 KiB images
for the current reference kernel. Add `--pending` for atomic entries with a checked
committed journal; basic/exhaustion exports are clean cases. Sol prepares the
reference environment, then Luna runs the CLI batches. The guest verifies an
independent namespace oracle and authors new committed changes for reverse replay.

Add `--symlinks` to `ext4-namespace-test` for short, long and maximum-length
target creation, validation and recovery. Use a separate empty export directory
from the create/mkdir/link run because both modes preserve a parent-append input.
Combine `--symlinks --groups` for the small multi-group fixtures. The same
independent checker and Linux `--namespace` runner consume these outputs;
`symlinks-` images are clean multi-target cases, while atomic entries have the
usual committed and uncommitted exports.

`ext4-removal-test IMAGE...` runs unlink/rmdir, held-inode lifetime, malformed
inputs and fault recovery. Its ordinary and indexed-validation suites are always
configured; small and orphan-file suites follow the corresponding fixture options.
Use `--smoke --export NEW_EMPTY_DIRECTORY` after the full suite to retain removal
images, then pass them to `check_namespace.py` with the matching source directory.
The same Linux `--namespace` runner accepts the checked report: `removed-` cases
are clean operation sequences, and `remove-atomic-` cases also support `--pending`.

After read-only mounting an independently checked `symlinks-` image in a dedicated
native guest, run `ext4-mounted-symlink-test MOUNTPOINT BLOCK_SIZE`. The explicit
block size is part of the known fixture profile, not the host VM page size. Run
as an ordinary user, then unmount, detach and require unchanged source bytes.
This probes the native readlink path; it does not enable adapter writes.
`run_macos_symlinks.py` runs these checked exports through an already prepared
lab VM. It requires explicit lab/VM paths, packaged products, read-only share,
guest directory, expected boot session and loaded-module UUID, independent
reports and a new output directory inside the shared products. It verifies the
identified kernel/module and probe hashes, attaches each raw image read-only,
checks its exact device identity, runs the probe without privilege, then unmounts,
detaches and verifies unchanged device/source bytes. It does not load a module,
replace a kernel or change boot state; those remain the separate VM preparation.

`ext4-write-test --truncate IMAGE...` checks bounded resize, freeing, corruption
and recovery. Add `--export-only --export NEW_DIRECTORY` after a successful CTest
run to create independent inspection images without duplicating its fault loops.
For truncate this emits three `cut400-`, `cut40-`, `cut4-` intermediate images and
the final reused-block image for each source. Verify them with:

```sh
python3 tests/check_allocation.py --truncate --exports artifacts/truncate-exports \
  --output artifacts/truncate-independent
```

Use `--tools-root` when e2fsprogs is outside PATH. The Linux roundtrip runner's
`--truncate` mode takes this independent report, verifies a final export under
Linux, and then truncates, grows and reallocates before leaving a committed journal
for portable recovery. Select final image names explicitly with `--case`; the
intermediate `cut*` exports use different byte oracles and are e2fsprogs checks.
Run VM commands from the lab directory using its identified reference kernel.

Use the selected Xcode C compiler and formatter on macOS. The portable core and
image tests must also compile with Clang on Linux. FSKit builds target a declared
macOS baseline; do not use newer SDK APIs without availability handling.

`make format` applies `.clang-format` to owned C headers, C sources and Objective-C
adapters. `make check-style` runs the same formatter without modifying files and
fails on formatting differences. Both select `clang-format` with `xcrun --find`
so `DEVELOPER_DIR` and the selected Xcode determine the toolchain. `CLANG_FORMAT`
is an explicit override; Linux checks require a compatible version 21 formatter.
The profile matches LXNU: tabs, eight-column indentation, a 100-column limit,
braced control flow and one statement per line. C declarations still belong at
block starts; formatting alone does not enforce declaration order.

External e2fsprogs commands create and independently inspect ext4 images. In the
shared workspace these tools are prepared under the lab's ignored `vendor/`
directory. Standalone users can point the fixture generator at their installed
tools. Images, dependency downloads and generated reports are ignored.

Generate the initial image profile with e2fsprogs tools on PATH:

```sh
python3 tests/generate_fixtures.py
make test
make check-style
```

For an out-of-tree e2fsprogs build, use
`--tools-root /absolute/path/to/e2fsprogs/build`. To retain previous evidence,
choose a new `--output` directory; the generator refuses to overwrite images.
`cmake -S . -B .build -DEXT4_FIXTURES=/absolute/path/to/fixtures` selects that
directory for tests. ASan/UBSan are enabled by default and can be disabled for
an adapter build with `-DEXT4_SANITIZERS=OFF`.

For the extended read profile, add `--extended` to the fixture generator and
configure `-DEXT4_EXTENDED_TESTS=ON` along with the generated directory in
`EXT4_FIXTURES`. This requires all eleven images rather than skipping absent
variations. It covers block sizes from 1 through 64 KiB, 32-bit group descriptors
with indirect mapping, metadata without checksums and an explicit checksum seed.
The 64 KiB image omits the journal to keep its total size at 64 MiB.

The selected Xcode clang compiles a second, optimized freestanding object target
with the same source and a 2048-byte frame-size check. This is a portability
check, not a linked or boot-tested kernel artifact. Kernel stack-protector symbols
remain enabled; XNU supplies them. Core code has no libc I/O or allocation imports.

Development proceeds through a portable reader and image tests, a stock FSKit
read-only mount, an early read-only kernel adapter, then transactional writes,
recovery and complete platform integration. Keep cheap userspace tests in the
iteration loop and periodically compile the same core for the kernel. Kernel
acceptance runs only in a coordinated disposable VM through Machlin lab.

The two platform builds share the same implementation. FSKit acceptance does
not establish kernel-stack safety, vnode/UBC ownership or LXNU correctness.

## Full-block namespace capacity

Generate small filesystems whose block bitmaps are actually full, then exercise
namespace failure rollback and reuse of existing space:

```sh
python3 tests/generate_space_fixtures.py --tools-root ../lab/vendor/e2fsprogs-ext4/build \
  --output artifacts/space-fixtures
cmake -S . -B .build-space -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DEXT4_SPACE_FIXTURES="$PWD/artifacts/space-fixtures"
cmake --build .build-space --parallel 2
ctest --test-dir .build-space -R '^namespace-space-' --output-on-failure
mkdir artifacts/space-exports
.build-space/ext4-space-test --export artifacts/space-exports artifacts/space-fixtures/*.img
python3 tests/check_space.py --fixtures artifacts/space-fixtures/report.json \
  --exports artifacts/space-exports --tools-root ../lab/vendor/e2fsprogs-ext4/build \
  --output artifacts/space-independent
```

The independent checker produces `linux-inputs.json` for the existing
`run_linux_journal.py --namespace` harness. Run that harness from the explicit lab
directory, with absolute paths to this input, the pinned module report and the
prepared recovery executable. Its full-block mode verifies Linux ENOSPC and
existing-block overwrite, truncates the filler to release capacity, then performs
the indexed namespace roundtrip. Core and independent reverse replay compare exact
retained objects and released/allocated block accounting. Use a new output directory
for every fixture/check/VM run so failures remain reviewable.

## Indexed namespace tests

Generate the indexed and full-root capacity fixtures in new directories, then
build the probes before generating independently verified collision pairs:

```sh
python3 tests/generate_index_fixtures.py --output artifacts/index-fixtures
python3 tests/generate_index_fixtures.py --capacity --output artifacts/index-capacity-fixture
cmake -S . -B .build-indexed -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DEXT4_INDEX_FIXTURES="$PWD/artifacts/index-fixtures" \
  -DEXT4_INDEX_COLLISIONS="$PWD/artifacts/index-collisions/vectors.txt" \
  -DEXT4_INDEX_CAPACITY="$PWD/artifacts/index-capacity-fixture/index-capacity.img"
cmake --build .build-indexed --parallel 4
python3 tests/check_directory_hash.py --probe .build-indexed/ext4-directory-hash-test \
  --output artifacts/index-hash-independent
python3 tests/generate_index_collisions.py --probe .build-indexed/ext4-directory-hash-test \
  --output artifacts/index-collisions
ctest --test-dir .build-indexed -R '^(directory-|indexed-)' --parallel 2 --output-on-failure
```

Generators and independent checkers accept `--tools-root` for a prepared e2fsprogs
build. The twenty normal profiles require all their images; missing profiles fail
the configured suite. The separate capacity fixture exercises full-root rejection
and successful record reuse, without enabling LARGEDIR.

`ext4-index-write-test --export DIR IMAGE` exports the functional result.
`--fault-smoke --export DIR IMAGE` exports old, complete, committed and uncommitted
states for each applicable split. `--edges VECTORS --export DIR IMAGE` exports three
collision/compaction states for 1 KiB profiles. Use `check_index_write.py`,
`check_index_faults.py` and `check_index_edges.py` respectively; each refuses an
existing output directory. The functional/edge checkers accept a repeatable
`--case SOURCE_FILENAME` to inspect one fixture profile at a time. The CI workflow
demonstrates all commands and removes temporary images only after their checks.

The existing Linux `--namespace` harness consumes these independently checked
reports with the explicit prepared recovery executable. Select 1/2/4 KiB profiles;
use `--pending` only for committed split reports. It verifies byte names and authors
its reverse transaction inside the indexed parent. Full-root capacity is currently
tested through the portable writer and independent tools rather than this Linux
namespace probe.

Use prepared bounded commands for execution workers. Preserve user changes in
the existing lab and XNU trees. The ongoing VFS refactor is an independent scope;
new filesystem work must not absorb or publish its uncommitted changes.

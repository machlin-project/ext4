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

Use prepared bounded commands for execution workers. Preserve user changes in
the existing lab and XNU trees. The ongoing VFS refactor is an independent scope;
new filesystem work must not absorb or publish its uncommitted changes.

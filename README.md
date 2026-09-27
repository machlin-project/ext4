# Machlin ext4

An ext4 filesystem for macOS, developed as a shared C implementation with an
FSKit application extension and an XNU filesystem extension. Machlin integration
adds Linux operation policy through a separate, versioned kernel interface.

This repository owns the filesystem implementation. It does not require LXNU to
build its portable core or FSKit adapter. The XNU fork owns LXNU-specific policy;
the Machlin lab owns combined kernel and VM acceptance.

## Status

The portable read-only core passes eleven image profiles with 1 through 64 KiB
blocks, extent and indirect mapping, checksum variations, indexed directories,
links and sparse files. Malformed-image and mount failure tests pass under ASan/UBSan.
An optimized freestanding compilation checks a 2 KiB stack-frame budget.
The read-only FSKit adapter builds for macOS 26.4. Its installed mount tests
await an Apple signing profile with the FSKit Module capability. The XNU adapter
also builds as an unsigned arm64e or x86_64 kext. The arm64e kext passes mounted
read-only tests for all eleven profiles in a dedicated custom-kernel VM, including
mmap, concurrent reads, repeated mounts and open-file/mapping lifetime.
Portable journal transactions and offline recovery pass interrupted-I/O tests,
independent debugfs journals and roundtrips through a real Linux kernel. General
writable operations remain incomplete: selective inode updates, bounded writes,
block allocation, sparse growth, unwritten conversion and truncate/freeing pass
portable faults, independent e2fsck and Linux roundtrips. Live shrink can span
bounded journal transactions, with orphan recovery completing interrupted operations.
Large writes now have a separate API that spans transactions and reports a durable
prefix on failure. Its focused faults, independent image checks and combined
253-test CI regression pass; native write integration remains pending.
Offline recovery handles legacy lists and bounded modern orphan files, including
Linux-authored open-unlinked objects and mixed representations.
Atomic creation, mkdir, symlinks, hard links, removal and rename now have portable
fault, independent image and Linux tests. Rename includes replacement, NOREPLACE,
EXCHANGE and cross-parent directory moves. The serialized core retains
open-unlinked or replaced inodes until their last hold is released.
Indexed-directory mutation now passes portable functional/fault tests, independent
e2fsprogs checks and Linux roundtrips, including collisions and bounded tree growth.
Name lookup follows the directory hash index, including collision continuations
across internal nodes. Independent name comparisons and targeted fault tests pass;
streamed enumeration, contiguous read ranges and per-read scratch reuse also pass
focused tests and warm-cache benchmarks. Broader performance acceptance remains open.
The portable core reads, lists and transactionally changes inode-body and external
xattrs, including copying shared blocks before modification. Attribute batches can
commit together with inode permissions, ownership and times. Attribute lifetime now
extends through creation, file writes, truncate, namespace changes and final orphan
release. The combined 253-test CI regression passes for the large-write/checksum checkpoint,
alongside targeted fault and independent checks. Bidirectional Linux
attribute/ACL/security roundtrips and direct
Linux replay of core attribute transactions pass on eight format profiles. ACL
enforcement and Linux capability policy are not implemented. Work proceeds through
portable-core acceptance, then FSKit integration on stock macOS, then LXNU policy.
Both platform adapters remain read-only; FSKit has not been mounted. Read
[the acceptance matrix](docs/ACCEPTANCE.md) before using an image with this code.
Native adapters have not yet adopted the streamed enumeration API. Bounded zeroing of written
preallocation now passes focused faults and independent image checks for both
write and truncate growth; its full regression remains pending.
Legacy CRC16 group descriptors also pass portable checks, independent image
inspection and Linux mutation/recovery roundtrips, including lazy inode and block
groups. Full regression of that format package remains pending.
Generated disk images and reports are not source artifacts.

## Layout

| Directory | Responsibility |
| --- | --- |
| `core/`, `include/` | Portable disk structures, inode operations and transactions |
| `adapters/posix/` | Image-backed development and fault-injection adapter |
| `adapters/fskit/` | Stock macOS application extension |
| `adapters/xnu/` | Vnode, UBC and block-device integration for the kext |
| `tests/` | Conformance, malformed images, recovery and concurrency |
| `docs/` | Architecture, build instructions and acceptance |

## Development

Meson and Ninja build the portable core and its tests. `make build` configures
the selected toolchain with ASan/UBSan; `make test` runs the configured image
matrix after its fixtures have been generated. The standalone Meson commands,
fixture options and six CI suites are documented below.

See [development](docs/DEVELOPMENT.md) and [architecture](docs/ARCHITECTURE.md).
The [automated test matrix](docs/TESTING.md) separates format, crash recovery,
platform behavior and remaining concurrency coverage.
Use the ongoing `development` branch. A future `main` baseline must reflect
explicitly verified behavior; a development checkpoint is not full acceptance.

The implementation is BSD-3-Clause licensed. External filesystem utilities are
test tools with their own licenses; their implementation is not linked into the
portable core.

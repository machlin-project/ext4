# Machlin ext4

An ext4 filesystem for macOS, developed as a shared C implementation with an
FSKit application extension and an XNU filesystem extension. Machlin integration
adds Linux operation policy through a separate, versioned kernel interface.

This repository owns the filesystem implementation. It does not require LXNU to
build its portable core or FSKit adapter. The XNU fork owns LXNU-specific policy;
the Machlin lab owns combined kernel and VM acceptance.

## Status

The portable core implements extent and indirect reads, sparse files, transactional
writes and allocation, bounded growth/truncate, and open-unlinked lifetime.
Namespace operations include create/mkdir/symlink/mknod/link/unlink/rmdir/rename,
rename exchange and whiteout, HTree mutation, automatic indexing, LARGEDIR and
DIR_NLINK. Raw inode-body and external xattrs follow metadata, data and namespace
transactions. Internal journal recovery handles legacy and modern orphan records.

The combined 361-test CI regression passes all six jobs, alongside independent
image checks and Linux mutation/recovery roundtrips. Tests cover malformed media,
allocation/read failures and interrupted writes under ASan/UBSan; freestanding
compilation enforces a 2 KiB stack-frame budget. Read profiles span 1–64 KiB blocks,
checksummed and legacy formats. Detailed evidence and known exceptions are in
[the acceptance matrix](docs/ACCEPTANCE.md).

Atomic inode flags, immutable/append-only protection and inheritance pass focused,
independent and six Linux roundtrip checks; their 373-test regression is running.
Preallocation and hole punching pass independent and Linux acceptance. Full-disk
writes within existing EOF also pass. KEEP_SIZE growth reaching a reserved extent's
last block passes focused faults, independent checks and six Linux roundtrips;
its expanded full regression is pending.
Growth ending earlier in a full extent still needs mapping space. Wider format and
journal compatibility, sustained scale and performance remain open core work.

Development proceeds through the core, then FSKit on stock macOS, then LXNU policy.
Both native adapters remain read-only. The FSKit adapter builds for macOS 26.4,
but installation/mount tests await a signing profile with FSKit Module capability.
The arm64e kext passes eleven read-only profiles in a dedicated custom-kernel VM,
including mmap, concurrent reads and lifetime checks; x86_64 has compilation only.
Native writes, ACL enforcement and Linux capability policy remain unimplemented.

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

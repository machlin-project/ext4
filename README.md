# Machlin ext4

An ext4 filesystem for macOS, developed as a shared C implementation with an
FSKit application extension and an XNU filesystem extension. Machlin integration
adds Linux operation policy through a separate, versioned kernel interface.

This repository owns the filesystem implementation. It does not require LXNU to
build its portable core or FSKit adapter. The XNU fork owns LXNU-specific policy;
the Machlin lab owns combined kernel and VM acceptance.

## Status

The portable core's functional acceptance queue is complete, subject to the explicit
format limits and interoperability exceptions in [the acceptance matrix](docs/ACCEPTANCE.md).
It supports ordinary and verified reads, transactional writes, allocation and
preallocation, extent and indirect maps, namespace operations, indexed directories, extended attributes,
inline data, clustered allocation, quotas, multi-mount protection, fscrypt and
fs-verity. Recovery covers internal/external journals, fast commits and orphan
cleanup. Volumes without a journal require an explicit write option.

Tests combine independently authored images, Linux roundtrips, malformed inputs,
resource failures, interrupted writes, sustained mixed operations and fuzzing.
ASan/UBSan builds and freestanding compilation with a 2 KiB stack-frame budget
check the shared implementation. Historical test counts and their exact limitations
belong in the acceptance matrix; they do not establish native adapter readiness.

The current phase is FSKit integration with a separate app control channel,
while retaining the final review and performance criteria.
See [the review and performance criteria](docs/CORE-REVIEW.md). The target is 15%
higher throughput than Linux on matched filesystem workloads; SHA/AES measurements
are separate. Reads require a geometric mean of at least 1.15 across eight fixed
profiles, with every profile at least as fast as Linux. The latest read mean is
1.162, but four contiguous-file profiles remain below Linux. Writes are evaluated
separately. AES and key
management are adapter services, while the core has portable SHA, ARM64 userspace
SHA-256 acceleration and metadata checksums.

The [FSKit adapter](docs/FSKIT.md) requires macOS 26.5 or later. Installed tests on
stock 26.5.2 pass 1 KiB and 4 KiB reads and writes, shared writable mmap, concurrent
I/O, namespace and user-xattr changes, open-unlinked lifetime and journal recovery.
An authenticated device-cache service supplies persistence barriers; the app and
extension retain their sandboxes. App Group control IPC and Keychain-backed
fscrypt v1/v2 reads, writes and key lifetime pass signed native roundtrips, followed
by independent fsck. Device-cache service termination and timeout tests return
I/O errors, retain the failed owner's state and recover through a new mount.
Bounded full-disk tests and native volume-label changes pass
on both block sizes on 26.5.2. Stock 27.0.1 also passes ordinary writes, metadata, namespace,
mmap, concurrent I/O, sparse-region queries, fscrypt v1/v2 key lifecycle and
persistence-service failure recovery.
Live set-ID metadata coherence and `diskutil renameVolume` fail on both OS versions.
The larger 1 KiB full-disk scenario stalls on 26.5.2; on 27.0.1, short capacity
checks now preserve ENOSPC but still fail native prefix accounting and readback.
ACL authorization, device-loss stress, broader OS/hardware
acceptance and production distribution remain unaccepted.
This is not a production release.

The kernel adapter remains read-only. The arm64e kext has read-only acceptance in
a dedicated custom-kernel VM; x86_64 has compilation evidence. Kernel encryption
and LXNU policy remain pending.

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
fixture options and nine CI suites are documented below.

See [development](docs/DEVELOPMENT.md) and [architecture](docs/ARCHITECTURE.md).
The [automated test matrix](docs/TESTING.md) separates format, crash recovery,
platform behavior and remaining concurrency coverage.
Use the ongoing `development` branch. A future `main` baseline must reflect
explicitly verified behavior; a development checkpoint is not full acceptance.

The implementation is BSD-3-Clause licensed. External filesystem utilities are
test tools with their own licenses; their implementation is not linked into the
portable core.

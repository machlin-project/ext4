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

Both native adapters remain read-only. The [FSKit adapter](docs/FSKIT.md) requires
macOS 26.5 or later and
uses held inode state, restricted kernel read mappings, user xattr reads and an
App Group Unix socket for the control app. The app and extension build with Apple
Development signing and profiles authorizing FSKit Module and the shared App Group;
installed mount acceptance remains pending. The arm64e
kext has read-only acceptance in a dedicated custom-kernel VM; x86_64 has compilation
evidence. Native writes, page-cache integration for mutation, ACL enforcement and
LXNU policy follow core review, with FSKit first. FSKit has a CommonCrypto provider
and Keychain-backed mount keys; signed native encryption acceptance and the kernel
encryption provider remain pending.

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

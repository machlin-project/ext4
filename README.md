# Machlin ext4

An ext4 filesystem for macOS, developed as a shared C implementation with an
FSKit application extension and an XNU filesystem extension. Machlin integration
adds Linux operation policy through a separate, versioned kernel interface.

This repository owns the filesystem implementation. It does not require LXNU to
build its portable core or FSKit adapter. The XNU fork owns LXNU-specific policy;
the Machlin lab owns combined kernel and VM acceptance.

## Status

The portable read-only core passes image tests for 1 KiB and 4 KiB filesystems,
including checksummed metadata, extent trees, indexed directories, links and
sparse files. Malformed-image and mount failure tests pass under ASan/UBSan.
An optimized freestanding compilation checks a 2 KiB stack-frame budget.
No mounted filesystem, writable filesystem, kernel extension or Linux capability
contract is accepted yet. Read
[the acceptance matrix](docs/ACCEPTANCE.md) before using an image with this code.
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

See [development](docs/DEVELOPMENT.md) and [architecture](docs/ARCHITECTURE.md).
Use the ongoing `development` branch. A future `main` baseline must reflect
explicitly verified behavior; a development checkpoint is not full acceptance.

The implementation is BSD-3-Clause licensed. External filesystem utilities are
test tools with their own licenses; their implementation is not linked into the
portable core.

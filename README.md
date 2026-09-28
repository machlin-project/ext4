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

The completed 469-test CI regression passes all six suites, alongside independent
image checks and Linux mutation/recovery roundtrips. The namespace job's timeout
was raised after its checks finished at the old limit; its targeted rerun passes,
including artifact upload and job cleanup. Tests cover malformed media,
allocation/read failures and interrupted writes under ASan/UBSan; freestanding
compilation enforces a 2 KiB stack-frame budget. Read profiles span 1–64 KiB blocks,
checksummed and legacy formats. Detailed evidence and known exceptions are in
[the acceptance matrix](docs/ACCEPTANCE.md).

Atomic inode flags, immutable/append-only protection and inheritance pass focused,
independent and six Linux roundtrip checks, including the full regression.
Preallocation and hole punching pass independent and Linux acceptance. Full-disk
writes within existing EOF also pass. KEEP_SIZE growth reaching a reserved extent's
last block passes focused faults, independent checks, six Linux roundtrips and
the full regression. META_BG and SPARSE_SUPER2 pass focused
fault tests, independent mutation/recovery checks, eight Linux roundtrips and
their expanded full regression. JBD2 checksum v1 and async commit
compatibility pass focused faults, independent replay in both directions and
eight Linux roundtrips and their combined full regression; the writer
retains its existing durability barriers on async-format journals.
Reservation now retains mapping capacity for partial KEEP_SIZE growth, including
moving EOF between extents at zero free blocks. Focused faults, 30 independent
image states, six Linux roundtrips and its expanded regression pass.
Imported full trees without spare capacity can still reject a reservation or
short growth safely. Wider format and journal compatibility, sustained scale and
performance remain open core work.

EA_INODE adds values up to 64 KiB, shared value references, transactional updates
and staged reclamation. Six format profiles pass focused mutation/corruption
checks, 96 independent image states and twelve private-orphan recovery states.
Power-cut tests cover create, replace, remove and shared-block copying.
Independent replay accepts 48 transaction states. All six Linux roundtrips pass,
including native orphan recovery and two pending core journals. The expanded
432-test regression passes across the unchanged five suites and the corrected core job.
Large attributes on short symlinks and legacy Lustre
value-inode encodings are explicitly unsupported.

INLINE_DATA adds small files and directories stored in the inode, transactional
growth into ordinary blocks, attribute coexistence and open-unlinked cleanup.
Eight format profiles pass focused checks, including creation at zero free blocks.
Independent checks accept 84 functional/lifetime states and 84 journal states;
2,016 interrupted-write cases recover the old or new transaction, or reject a
torn primary superblock. Eight native Linux roundtrips also pass, including
open-unlinked cleanup and two pending core journals. The expanded 444-test CI
regression passes all six suites with no failures or skips.

BIGALLOC implements cluster bitmap accounting, shared cluster backing within an
inode, whole-cluster allocation and final-reference release. Eight profiles pass
functional checks and 88 malformed cases. Independent verification accepts 32
mutation/lifetime states and 72 transaction/replay states; 1,536 power cuts cover
allocation, reuse, partial release, reservation and attribute reclamation. Two
completely allocated images and private attribute values pass four further
independent states. Eight native Linux/core/Linux roundtrips pass, including orphan
recovery and pending core journals. The expanded 466-test regression passes.

Large logical-file tests cover 2/4 GiB byte boundaries, the signed 32-bit block
boundary and format size ceilings on nine profiles up to 64 KiB blocks. Inline
expansion now uses its destination mapping's limit; legacy sector accounting
also bounds growth when HUGE_FILE is absent. All nine focused tests and 36
independent mutation/reclamation states pass. Six native Linux roundtrips pass
with ordinary ordered mounts; the two BIGALLOC profiles pass with explicit
`nodelalloc`. The pinned Linux reference writes inconsistent allocation counters
with delayed allocation at the maximum clustered-file offset; that failure remains
recorded and the core rejects the corrupt orphan. The expanded regression passes;
this does not establish large-volume scale.

Sparse multi-terabyte volumes now exercise last-group inode tables, directories,
data, extent nodes and xattrs, including physical block numbers above 32 bits.
The writer promotes an empty 32-bit journal to wide tags at its first transaction.
Three profiles pass 21 independent read/mutation/reclamation/recovery states and
native Linux/core/Linux roundtrips, including direct Linux replay of the core's
high-address journal. The expanded 469-test regression and independent checks pass.

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

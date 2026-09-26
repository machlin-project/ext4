# Acceptance

The end goal is a working read/write filesystem in stock macOS through FSKit and
in Machlin through a kernel adapter, with the agreed Linux metadata contracts.
The rows below are requirements, not claims of implementation. A checkpoint does
not complete the project. Format support must expand with tested real images;
safe rejection of a feature is recorded separately from supporting it.

| Contract | Required evidence | Current state |
| --- | --- | --- |
| Geometry, feature negotiation, metadata checksums | Real mke2fs images and malformed-input tests under sanitizers | Eleven read profiles pass; broader format and size coverage pending |
| Inodes, directories, links, extents, sparse data | Independent contents and metadata comparison | Portable reader and mounted arm64e kext profiles pass; FSKit runtime pending |
| Modern format variations | Explicit feature/size matrix including checksums, 64-bit fields, indexed directories and additional enabled features | Not accepted |
| Create/write/truncate, allocation, rename, unlink | Linux roundtrips, full disks, partial I/O and open-file lifetime | Not implemented |
| Journal and recovery | Interrupted transactions, ordering faults, device errors, Linux replay and e2fsck | Not implemented |
| Xattrs, permissions and ACLs | Preserve and mutate metadata across macOS/Linux roundtrips | Not implemented |
| Stock macOS FSKit | Actual mount, ordinary application I/O, concurrency, mmap and unmount on an Apple kernel | Read-only adapter builds; installed tests await signing profile |
| Kernel adapter | Actual loaded kext, vnode/UBC behavior, fault/truncate/writeback and resource balance | Loaded arm64e read-only profile passes; writable paths and full resource accounting pending; x86_64 compilation only |
| LXNU policy | CAP_FSETID and privilege removal, xattrs, mixed-ABI races, inherited descriptions and attachment restrictions | Not implemented |
| Compatibility and regression | Shared Linux/LXNU fixtures, native controls and identified stock/custom boots | Not run |
| Distribution | Reproducible standalone build, packaged FSKit extension, documented installation and supported versions | Unsigned app with embedded extension builds; signing and installation pending |

Each accepted row must identify its test command and generated evidence location.
Raw identities, hashes and logs stay in ignored artifacts; source revisions stay
in Git. Generated images never enter source history. Unsupported advanced ext4
features remain visible requirements or explicit scope decisions; they may not be
silently reclassified to declare the project complete.

## Portable reader evidence

`tests/generate_fixtures.py` creates three 64 MiB images: 4 KiB blocks, 1 KiB
blocks, and an e2fsck-indexed copy. The explicit feature set includes extents,
64-bit descriptors, flex_bg and metadata_csum. Each generated image must pass
e2fsck; the generator rejects extra or missing feature flags.

With `--extended`, it creates eleven profiles covering every power-of-two block
size from 1 to 64 KiB, indirect addressing with 32-bit group descriptors, no
metadata checksums and an explicit checksum seed. The 64 KiB fixture has no
journal. All images in `artifacts/fixtures-extended/` passed e2fsck and the
sanitized reader, with exact expected feature sets. These small images do not
exercise large physical block numbers, triple-indirect addressing or all ext4
features.

The accepted profile has no POSIX ACLs, security xattrs or verity inodes. Their
access and integrity policies are not implemented or accepted by the current
adapters. Supporting such files requires preserving and enforcing their owning
contracts, rather than treating mode bits and ordinary data reads as equivalent.

`make test` runs the image reader and malformed-image suites. The reader verifies
exact bytes for ordinary, empty, short/long symlink and sparse files, hard-link
identity, nested and indexed directory enumeration, unaligned reads, EOF and
allocation/I/O failure cleanup during mount. The malformed-image suite checks
the CRC32C standard vector, geometry, incompatible flags, recovery state and
superblock/group/inode/directory checksum rejection. Current local reports live
under `artifacts/checks/`. Independently regenerated images under
`artifacts/fixtures-reproduced/` passed the same reader suite and e2fsck.

The freestanding compilation is not a loaded kernel driver. Its only non-core
undefined symbols are compiler stack-protector support, which XNU exports through
Libkern. The sanitizer build has a different frame layout and is tested separately.

The metadata fixture also covers 32-bit UID/GID, signed pre-epoch time, dates
beyond 2038 and 2106, birth time and nanosecond precision. A malformed timestamp
with a valid inode checksum is rejected. Current regenerated images are in
`artifacts/fixtures-metadata/`; the CMake `EXT4_FIXTURES` setting selects them.

The bounded metadata fuzzer completed 106,119 executions in 61 seconds with
Clang 22.1.5, ASan and UBSan, without a crash or sanitizer diagnostic. It mutates
the superblock, group descriptor, root inode and root directory, optionally
repairing checksums to exercise structural validation. Reports are in
`artifacts/checks/fuzzer-llvm-build.log` and `artifacts/checks/fuzzer-run.log`.
This run does not cover every disk feature, recovery, writes or concurrency.

## FSKit build evidence

The app and embedded extension compile using the macOS 27 SDK with deployment
target macOS 26.4. The adapter maps resource I/O, inode identity, attributes,
lookup, directory cookies, links and reads to the same C library. Mutations return
`EROFS`; its requested mount options include read-only. Only a quick clean-volume
check is implemented; it is not a full consistency checker or repair utility.

Unsigned build reports are in `artifacts/checks/fskit-reader-final-build.log` and
the earlier FSKit build logs. Automatic signing found no Xcode
account or provisioning profile for `org.machlin.ext4.filesystem`. Signing is
deferred by the user. No extension has been installed or mounted.

`ext4-mounted-test MOUNTPOINT` is built, but its FSKit runtime results remain pending.
It checks ordinary reads, metadata, hard links and symlinks, indexed directory
enumeration, sparse data, mmap, concurrent opens/reads/closes and read-only
enforcement. The disposable stock macOS guest was booted and its actual loaded
Apple kernel identified, then shut down. This proves guest readiness only.

## Kernel build evidence

`make kext` compiles every shared core source with the selected Xcode kernel
headers and emits an unsigned arm64e Mach-O kext bundle. The x86_64 build also
passes. Both builds enforce the C declaration/style checks and an optimized
2 KiB stack-frame limit. Inspection reports live under `artifacts/checks/`.
The listed imports correspond to XNU's BSD and Libkern exports; that comparison
does not establish compatibility with a running kernel.

The bundle declares `OSBundleAllowUserLoad`, allowing XNU to retain the mutable
segment state needed for unloading and reloading a module in the primary kernel
collection. It does not alter the system's kext signing or authorization policy.
Normal load/unload/reload passed, followed by another successful normal unload
after the final mounted suites. The lifecycle report records statuses and loaded
module observations; the dedicated VM was then shut down cleanly.

The adapter implements read-only mounts, inode/vnode identity, lookup, stat,
directory enumeration, symbolic links and native UBC reads/page-in. It currently
creates vnodes for regular files, directories and symbolic links; special-file
operations remain unsupported. All mutating vnode operations reject writes.
The arm64e module was packaged with the preserved native custom XNU kernel and
the matching stock kext inventory, then actually booted and explicitly loaded
in the separate `lxnu-ext4-kext-lab` clone. The loaded module UUID, collection
hash, boot slot and new session were checked. This is custom-kernel execution;
it does not establish signed kext installation or stock FSKit acceptance.

The expanded mounted test passed three mount/test/unmount cycles on the attached
indexed image, with unchanged raw device bytes. It checks exact UID/GID and
timestamps, links, sparse bytes, directory positions, eight concurrent readers,
read-only errors, mmap after descriptor close, EOF padding and private copy-on-write.
The same mounted test passed all eleven read-only image devices from the extended
profile. A separate `ext4-mounted-lifetime-test MOUNTPOINT` checks that both an
open descriptor and an untouched mapping keep unmount busy, and that releasing
the last mapping permits normal unmount. It passed without a kernel/session change.

Lab evidence lives under `logs/ext4-kext/` and `artifacts/ext4-kext/`: collection
identity, `userload-*` loaded-kernel/module reports, `mounted-*`, `matrix-*`,
`lifetime-userload-20260927.log` and `final-userload-lifecycle-20260927.json`.
Earlier failed attempts remain separate. These
tests do not establish writable pageout, truncate races, forced unmount, complete
allocation balance, physical hardware or x86_64 runtime behavior.

The reader suite independently checks block mappings by reading returned device
offsets through the raw POSIX resource and comparing file contents, including
unaligned offsets, holes and the last file block. These checks do not substitute
for actual page-in and buffer strategy execution in XNU.

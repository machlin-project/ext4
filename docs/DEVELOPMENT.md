# Development

## Portable build

The project uses Meson 1.3 or newer and Ninja. A C11 compiler and Python 3 are
required; macOS development uses the selected Xcode Clang. `make build` selects
that compiler, configures `.build`, and compiles the core, image utilities and
test programs. Use a new build directory when changing build systems or compilers;
existing generated test evidence can remain in its original directory.

```sh
CC='xcrun --sdk macosx clang' meson setup .build --buildtype=debugoptimized
meson compile -C .build -j 4
env -i PATH="$PATH" meson test -C .build --no-rebuild -j 2 --print-errorlogs
```

The `xcrun` compiler command selects the active Xcode and macOS SDK on each compile
and link, including later incremental builds. On Linux, use `CC=clang` for setup.
ASan and UBSan are enabled by default through
Meson's `b_sanitize=address,undefined`; `-Db_sanitize=none` selects an uninstrumented
build. Every build also compiles the complete freestanding core at O2 without
instrumentation, enforcing its 2 KiB stack-frame budget. Native mounted-test
executables keep their separate uninstrumented build.

Generate the required images as described below before running image tests.
`meson configure .build` lists options; `meson setup --reconfigure .build -Dname=value`
sets them while retaining other choices. Fixture paths can be absolute or relative
to the source directory. Enabling a profile requires all its images; none are
silently skipped. Test executables and `ext4-recover` remain directly in the build
directory for the independent checkers and VM harnesses.
After adding a new Meson option, first regenerate an existing build with
`meson setup --reconfigure .build`, then set the new option with `-D`.

`meson test -C .build --list` lists the configured cases. The eight disjoint suites
are `core`, `orphan-file`, `namespace`, `removal`, `rename`, `indexed`, `sustained` and
`format`; select one with `--suite NAME`, or use quoted test-name patterns. Complete output, per-test
JSON records and JUnit results are in `.build/meson-logs/testlog.txt`,
`testlog.json` and `testlog.junit.xml`. CI retains all three, including successful
fault-test output and explicit applicability skips.
Make and CI invoke the tests with a minimal environment because Meson records
the inherited environment in its test logs. The direct command above does the same.

The Makefile is a convenience entry point for the same Meson commands.
`BUILD_DIR`, `MESON_OPTIONS`, `BUILD_JOBS` and `TEST_JOBS` customize its build and
test invocation. `make clean` removes compiled outputs through Meson without
removing fixtures or reports. `make format` and `make check-style` work without
configuration; Meson exposes the same formatting targets after setup.

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

## Sustained mixed operations

`ext4-sustained-test IMAGE SEED OPERATIONS` runs a deterministic random sequence of
create, mkdir, symlink, mknod, atomic and partial writes, both truncate forms,
preallocation and hole punching, link, unlink, rmdir, rename with replacement,
NOREPLACE and EXCHANGE, user attributes, permission changes and inode holds.
An in-memory model defines the expected namespace, bytes, link counts, permissions
and attribute values. It checks every result against the model, walks every
directory and file every 50 operations, and remounts cleanly every 200 operations,
including a read-only verification mount. Existing names, populated rmdir targets
and invalid renames must reject without changing state or poisoning the owner.

About one in six eligible atomic operations first runs from a clean snapshot to
produce the new image, then repeats from the same bytes with a power cut at a random
write or barrier, a random cache-survival mode and optional torn write. Recovery
must reproduce the exact old or new image outside the journal; a durable commit
requires the new image. A third run applies the operation to the model and must
produce the same bytes. Partial writes, preallocation and inode holds do not take
part in this byte comparison because their durable prefix or orphan lifetime is
not one of two images. Ballast files fill the volume with seeded data, so writes,
growth and namespace operations also run near and at allocation exhaustion.

`--commit-blocks N` mounts with deferred commit of at most N snapshots; each
power-cut case then ends with `ext4_commit`, so its cuts land in the commit, while the
other operations accumulate between commits and the model's walks read them through
the pending overlay. `ext4-deferred-test [--export DIRECTORY] IMAGE...` runs a fixed
sequence of 24 mutations in one compound: nothing may reach the device before
`ext4_commit`, every write and barrier of that commit is cut under three survival
modes and recovery must yield the image before or after the whole sequence; with a
24-block compound, capacity commits happen mid-sequence and every cut must recover
exactly one commit-point image. Both properties then run again with ordered data, where
blocks the expected image leaves free may hold data of an uncommitted mutation, and
an overwrite of `payload.bin` is cut at every event: blocks outside it must be
unchanged, each overwritten 512-byte sector old or new, and a durable commit must
carry the new data. The sustained test's `--data ordered` compares images only for
operations that write no file data. For lazy checkpointing, `ext4-deferred-test`
repeats the atomic commit with a checkpoint set that holds the whole sequence, where
no home block may change before `ext4_sync`, and the commit-point property with a
48-block set, synchronously and with the compound, where checkpoints happen
mid-sequence and cuts land in them. Native mappings from `ext4_map_read` must hold
the current contents or report BUSY. With `--export DIRECTORY` it also writes the synchronous lazy run's final image,
whose log holds every transaction since the last checkpoint;
`tests/check_pending_logs.py --tools-root E2FSPROGS_BUILD --recover EXT4_RECOVER
--exports DIRECTORY --output NEW` requires debugfs to find at least two
consecutive commits, e2fsck's replay-only mode and `ext4-recover` to produce
volumes that strict fsck accepts, and both to be identical outside the primary
superblock. The sustained test's `--checkpoint-blocks N` and
`ext4-scale --commit-blocks N --checkpoint-blocks N --data ordered` exercise and
measure the workloads in these modes.

`tests/generate_format_fixtures.py --tools-root E2FSPROGS_BUILD --output NEW`
authors one volume per optional format feature or geometry, and the `format_fixtures`
option adds a sustained case for each.

`--encrypt`, `--verity` and `--casefold` also exercise those features and set them
on the volume when it lacks them. `--encrypt` encrypts a directory under the Linux
probe's master key from the test keyring in `tests/keyring.h`. Objects created in it
inherit its policy, except special files, and it admits only its policy's objects
and special files; links and renames of other objects must return
`EXT4_CROSS_POLICY`, and encrypted symlink targets longer than the block size less
three bytes `EXT4_NAME_TOO_LONG`. A power-cut repetition restores the keyring's nonce
counter, so it writes the same ciphertext. Every remount also checks the tree
without the key: each encrypted directory lists as many no-key names as the model
has entries, each looks up its inode, encrypted contents are refused and targets
read as no-key names. `--verity` adds an operation that enables verity on a file
with either algorithm, the filesystem or a 1 KiB Merkle block and an optional salt;
verity files must refuse writes, truncation and preallocation with
`EXT4_PERMISSION_DENIED`, measure the same digest until removed and refuse enabling
again, and encrypted files refuse it with `EXT4_ENCRYPTED`. Enabling spans several
transactions and does not take part in the power-cut byte comparison. `--casefold`
adds a casefolded directory whose new subdirectories inherit the flag; the model
matches names in it without regard to case, reuses names in other spellings, and
requires listings to keep each name's spelling and case variants to look up the same
inode. With any of these, half of the directory choices fall in encrypted or
casefolded directories, and the encrypted and casefolded directories created at the
start may move but are not removed. `--no-ballast` leaves out ballast files for
compact exports such as fuzzing seeds. The `sustained` suite runs each feature on the
4 KiB and 1 KiB base volumes, all three with ordered data, a compound and lazy
checkpoints on those and the indirect 1 KiB volume, and each format fixture with its
feature in use.

`--objects`, `--entries` and `--directories` raise the model's limits; the wide
cases keep up to 2,000 names in three directories to exercise indexed growth and
splits. `--export DIRECTORY` writes the final clean image, a manifest and expected
file/symlink bytes; the manifest also marks encrypted and casefolded objects, gives
each verity file's parameters and digest, and lists each encrypted directory's
no-key names as a mount without the key presents them. `tests/check_sustained.py
--tools-root E2FSPROGS_BUILD --exports DIRECTORY --output NEW_DIRECTORY` then requires
strict nonrepairing e2fsck and compares every directory, inode identity, type, link
count, permission, file byte, symlink target and attribute value with debugfs,
addressing objects by inode number. An encrypted directory's raw ciphertext names
must be those its no-key names carry; encrypted contents and targets are counted
rather than decrypted. Encryption, casefold and verity flags must match, the fscrypt
context must exist exactly on encrypted objects, and each verity file's digest,
Merkle tree and descriptor are recomputed from its expected contents.
`tests/run_linux_sustained.py --lab LAB --prepared PREPARED --runner RUNNER --export
DIRECTORY --output NEW` runs from the lab and mounts an export read-only in the
Linux reference guest. Without the key Linux must list exactly the exported no-key
names of each encrypted directory, reached through no-key names of encrypted
ancestors; with the probe's key it must read every file's expected contents and
symlink target, find every type and measure every verity digest, and the image must
stay unchanged and pass strict fsck. The reference kernel lacks `CONFIG_UNICODE`, so
casefolded exports are checked by e2fsprogs only. The `sustained` suite runs every
writable base profile and two wide profiles; CI additionally exports five runs for
independent verification. Seeds make each failing sequence reproducible.

## Scale measurements

`ext4-scale IMAGE WORKLOAD SIZE [RESULT]` loads an image into memory and runs one
workload against the optimized, unsanitized freestanding core. `sequential MIB`
appends a file in 1 MiB partial writes, overwrites 1,000 random 64 KiB ranges and
truncates it to zero. `directory ENTRIES` creates empty files in one directory,
reporting every 10,000 creations, then looks up 1,000 names. `fragmented FILES`
fills a directory with 4 KiB files, removes every other one and writes 64 MiB into
the resulting holes. `reclamation MIB` unlinks a held file of that size, unmounts
and measures offline orphan reclamation. Each phase prints one JSON object with
elapsed time, device reads, writes, flushes and bytes, core allocation calls, peak
live core allocation, write amplification and the resulting extent count. Device
flushes cost nothing, so times measure the core rather than the medium.
`RESULT` receives the image after the workload.

`tests/run_scale.py --tools-root E2FSPROGS_BUILD --scale EXT4_SCALE --output NEW
--label NAME [--commit-blocks N] [--data ordered]` runs every workload on fresh
1 GiB e2fsprogs volumes with 4 KiB and 1 KiB blocks, requires strict nonrepairing
fsck of each result and writes `measurements.jsonl`. Compare source revisions with the same `tools/scale.c` driver
on a quiet host: add it and its `meson.build` executable to a `git archive` of the
baseline revision, build both with the same options and run them in turn.

## Multi-mount protection tests

`tests/generate_mmp_fixtures.py --output artifacts/mmp-fixtures` authors four MMP
images with e2fsprogs: 4 KiB and 1 KiB with metadata checksums, 1 KiB without them,
and a 40-second update interval. Each passes strict fsck and starts with the clean
sequence. Configure `-Dmmp_fixtures=artifacts/mmp-fixtures` to add the `format` suite's
`ext4-mmp-test` cases. A modeled second host runs on a virtual clock and changes the
MMP block during selected waits; every MMP write is recorded.

The test covers clean acquisition and its exact wait, the immediate post-acquisition
update, stale-sequence refresh before a mutation, explicit heartbeat, release and
subsequent read-only rejection; sequence wrap; the checker value, stale and active
owners, a contested confirmation, interrupted waits and ownership stolen before a
heartbeat or mutation; missing services and malformed blocks; and offline recovery
holding the checker value before releasing it. Passing an export directory writes a
released image and a pending committed journal. `tests/check_mmp.py --tools-root
E2FSPROGS_BUILD --exports DIRECTORY --recover .build/ext4-recover --output NEW` requires
strict fsck, the clean sequence and the core's node and check interval through
debugfs, lets e2fsck acquire a selected released image read-write, and recovers each
pending image with the POSIX utility. These steps use real waits of about 21 to 58
seconds per acquisition. The POSIX adapter supplies wall-clock sleeps, `/dev/urandom`
values and the host name.

`tests/run_linux_mmp.py` runs from the lab with `--lab`, `--prepared`, `--runner`,
`--core-test` (an `ext4-mmp-test` from the core under test), `--exports` with the
released and pending images of the 4 KiB, 1 KiB and unchecksummed profiles, and a
fresh `--output`. Linux mounts each released image read-write, writes a file and
unmounts; strict fsck must pass and the MMP block must hold the clean sequence, the
guest's node name and the check interval. `ext4-mmp-test --continue IMAGE` then
acquires that image through the POSIX adapter with real waits, requires the Linux
file, adds its own and releases it with its host name; Linux must find both files and
release again. Finally Linux takes over each pending image, replaying its journal,
and releases it clean. The guest's monotonic clock does not advance across the
kernel's MMP sleeps in this VM, so the runner records Linux's elapsed mount time but
does not rely on it.

## fs-verity tests

`tests/generate_verity_fixtures.py --output artifacts/verity-fixtures` authors fs-verity
files independently of the core: Python computes the Merkle tree, root hash,
descriptor and file digest from the documented format, and debugfs stores the data,
tree, descriptor and size field before setting i_size and the EXTENTS|VERITY flags.
Four images cover 4 KiB SHA-256, salted 1 KiB SHA-256, salted 4 KiB SHA-512 and 4 KiB
SHA-256 with inline data, with empty, single-block, partial, sparse and multi-level
files. Each image also contains
damaged data, a damaged level-zero hash, a wrong root hash, an unsupported descriptor
version and an impossible descriptor size. Every image passes strict fsck; the
manifest records sizes, SHA-256 contents, file digests and the block that must fail.

Configure `-Dverity_fixtures=artifacts/verity-fixtures` to add `ext4-verity-test IMAGE
MANIFEST` cases to the `format` suite. They check SHA-256/SHA-512 known answers and
split updates, complete and offset reads against the manifest, every allocation and
read failure of the largest valid file, native-mapping refusal and exact rejection of
each damaged file. A writable copy then must refuse writes, truncation, growth,
preallocation and hole punching without device writes, while permission, attribute,
link, rename and unlink changes succeed and deletion frees all metadata blocks. An
optional export directory receives that image and an updated manifest;
`tests/check_verity.py --tools-root E2FSPROGS_BUILD --exports DIRECTORY --output NEW`
requires strict fsck and checks names, flags, permissions and the new attribute.

The same option adds `ext4-verity-enable-test`, which enables verity through the core
on the four images and requires a volume without the feature to refuse. Each image
receives empty, one-byte, one-block, sparse, preallocated, inline and multi-level
files under both algorithms, Merkle blocks of the filesystem block size and 1 KiB,
and salts; each must read back verified, measure, keep its times and refuse another
enable and writes. Unsuitable files and parameters must be refused. A power cut at
every write or barrier of a multi-transaction enable, every allocation and read
failure of another, and a volume that runs out of space after the first tree
transaction must each leave either the original file with its blocks and free space
or a verity file. With `--export DIRECTORY` it writes each final image and a manifest
in the fixtures' format, extended by each file's algorithm, Merkle block size and
salt; `tests/check_verity_enable.py --tools-root E2FSPROGS_BUILD --exports DIRECTORY
--output NEW` requires strict fsck, the verity flag and the dumped contents, compares
the tree, descriptor and size field read through the file's extents past EOF with the
generator's independent layout, rejects other mappings past EOF, compares digests and
writes a fixtures report for the Linux harness. The test also enables files with
signatures of a test scheme, one spanning several blocks, under a verifying
environment: enabling verifies once, reads reuse the accepted digest until the
environment is installed again, read-only mounts verify, forged and oversized
signatures are refused without change, and required signatures refuse unsigned
reads, measurement and enabling without writes.

`tests/generate_verity_signatures.py --tools-root E2FSPROGS_BUILD --enable-test
EXT4_VERITY_ENABLE_TEST --image VERITY_4K_IMAGE --output NEW` creates an RSA
certificate with OpenSSL, signs the formatted digest of each file, computed
independently, as a detached PKCS#7 signature without certificates or attributes,
and has the core store the signatures through `ext4-verity-enable-test --import`.
The image also holds an unsigned file, one signed by another certificate and one
with a damaged signature. `check_verity_enable.py` checks the result, including the
signature bytes after each descriptor, and the output keeps `signer.der`.

`tests/run_linux_verity.py` runs from the lab with `--lab`, `--prepared`, `--runner`,
`--fixtures DIRECTORY` and a fresh `--output`. The directory needs a `report.json`
naming each image and manifest, as written by the generator or `check_verity.py`.
Each image boots once: Linux mounts it read-only, requires FS_IOC_MEASURE_VERITY to
match the manifest digest, reads every valid file and requires EIO or open failure
for each damaged one. `--certificate DER` adds a certificate to the .fs-verity
keyring and `--require-signatures` sets fs.verity.require_signatures; files of the
kinds "unsigned" and "badsig" must then fail to open. Linux records the invalid descriptor location in the superblock
error fields even on a read-only mount; that primary-superblock record is the only
permitted image change.

## Encryption tests

`ext4-encrypt-test --synthetic IMAGE` builds a tree through the core on a base image,
then marks a directory subtree and an empty top-level directory encrypted in memory
and sets the ENCRYPT feature. The `format` suite runs it on the 4 KiB and 1 KiB base
images. Without `--synthetic`, IMAGE must come from the Linux probe. Both modes find
encrypted objects by inode scan, require `EXT4_ENCRYPTED` for their contents and
data changes and for new names in encrypted directories, deny changes to the fscrypt
context attribute, and then
rename the unencrypted tree, move the encrypted directory and back, remove the empty
encrypted directory and change an encrypted file's permissions. An optional export
directory receives the changed image. Synthetic images lack real fscrypt contexts and
are not fsck oracles. Their plaintext names are too short to be ciphertext, so
listing them without the key is corruption; the probe's tree lists no-key names,
each of which looks up its inode, while plaintext names are not found.

`ext4-encrypt-test --key IMAGE`, with the image the Linux probe created, also
installs a test adapter holding the probe's master key, built on the reference
cryptography in `tests/crypto.h`. Another master key must leave the tree
unreadable and list no-key names; the probe's key must list both encrypted directories exactly, read every
file's contents and the symlink target, keep native mappings refused and reuse a
cached derived key, and every key handle must be released by unmount.
`ext4-crypto-vectors-test`, in the `core` suite, checks that reference cryptography:
AES against FIPS-197, and XTS, CBC with ciphertext stealing and HKDF-SHA512 against
answers computed with OpenSSL, including the key identifier Linux stored for the
probe's master key.

`ext4-encrypt-test --write IMAGE [EXPORT_DIRECTORY]` sets the ENCRYPT feature on a
base image if needed and encrypts a directory through the core with the test
adapter. Invalid, unsupported and unknown-key policies, a non-empty directory and a
file must be refused, and the same policy again must change nothing. Empty, small,
block-sized, partial, sparse, large and long-named files, a subdirectory, fast and
block symlinks and a FIFO are then created, overwritten inside blocks, truncated into
a block and grown, written into preallocation and punched, linked and renamed, and
compared with a model at every stage; 24 files with 200-byte names make the
subdirectory indexed on volumes with DIR_INDEX. Unencrypted files must not enter the
encrypted directory and an encrypted file must be able to leave it. No plaintext
name, target or content may reach the device. Without the key the directory must
list a no-key name for every entry, refuse creation and rename, read a symlink
target as a no-key name and remove a long-named file and a directory by their no-key
names, and a read-only mount must read the tree back with the key. A power cut at every write or barrier of an
encrypted overwrite across three blocks and of a truncation into a block must
recover the old or the new contents. The export directory receives
`encrypted-NAME.img`, a manifest of every object and `encrypted-NAME.nokey`: each
encrypted directory by its no-key path, followed by the no-key names and symlink
targets the core presents in it without the key. The `format` suite runs it on
the 4 KiB and 1 KiB base images and the format fixture with ENCRYPT.

`tests/run_linux_encrypt.py --create` runs from the lab with `--lab`, `--prepared`,
`--runner` and a fresh `--output`. Linux adds a raw key, sets a v2 AES-256-XTS/CTS
policy with 32-byte name padding, and creates encrypted files up to 197 KiB, long
names, a subdirectory, a symlink, an empty encrypted top-level directory and plain
files; the image must pass strict fsck. After the core test changes a copy,
`--verify IMAGE` adds the same key and requires every encrypted byte, the symlink
target, the removed directory and the renamed plain file, followed by strict fsck.
`--verify-core IMAGE --manifest MANIFEST` adds the same key to a tree the core
encrypted and requires every manifest object: files by size and SHA-256, symlinks by
target and the types of directories and FIFOs, followed by strict fsck. With
`--nokey NOKEY`, Linux first lists each directory the file names without the key,
looks up every name and reads every symlink target, and the harness requires exactly
the core's names and targets.

## Casefold tests

`scripts/generate_unicode_data.py --ucd DIRECTORY --output core/unicode_data.h`
regenerates the casefold tables from the Unicode 12.1.0 `UnicodeData.txt`,
`CaseFolding.txt`, `DerivedCoreProperties.txt` and `DerivedAge.txt`; it refuses
files whose SHA-256 differs from the pinned release.

`tests/generate_casefold_vectors.py --tools-root E2FSPROGS_BUILD --output NEW` builds
`tests/casefold_oracle.c` against that build's `libext2fs.a`, which implements
Linux's utf8data semantics, and writes `vectors.txt`: the fold of every code point,
including surrogates, malformed sequences, 200,000 random combining sequences of up
to 12 code points, 20,000 casefolded hashes across all six hash versions with random
and zero seeds, and 4,000 near-maximal names of expanding code points whose folds
exceed 255 bytes, each folded and hashed. Configure
`-Dcasefold_vectors=NEW/vectors.txt` to add `casefold-oracle` to the `format` suite;
`ext4-casefold-test` requires identical folds, opaque classification and hashes,
and at least one hash of a fold longer than 255 bytes.
Vectors containing NUL are skipped because libext2fs stops at NUL and no ext4 name
contains one.

`tests/generate_casefold_fixtures.py --tools-root E2FSPROGS_BUILD --output NEW` makes a
4 KiB strict and a 1 KiB relaxed volume. debugfs writes six names with case, NFD,
ignorable and expansion variants and 400 bulk names into `cf`, and the six names into
the linear `small` directory; the relaxed profile adds an opaque non-UTF-8 name to
`small`, because e2fsck cannot rebuild an index holding one. `e2fsck -fyD` then
indexes `cf` with e2fsprogs' casefolded hashes, and strict fsck must pass. The
manifest lists equivalent lookups, absent names and the strict flag.

Configure `-Dcasefold_fixtures=NEW` to add `ext4-casefold-fs-test IMAGE MANIFEST` cases
to the `format` suite. Every manifest lookup must resolve to the stored name's inode,
or be absent, in both directories. A writable copy then must return `EXT4_EXISTS`
for folded duplicates in create and link, `EXT4_INVALID_ARGUMENT` for a malformed
name under the strict encoding and exact-byte matching under the relaxed one, and
treat all names of ignorable code points as one empty name. It adds 600 names that
split indexed leaves, checks a case-only rename, creates an inheriting subdirectory,
unlinks and renames entries through equivalent names, enables casefolding on a new
top-level directory only once it is empty and remounts to look up names again. An optional export directory receives the image;
`tests/check_casefold.py --tools-root E2FSPROGS_BUILD --exports DIRECTORY --output NEW`
requires strict fsck, which recomputes every casefolded hash in the index, and checks
the stored names, removals and inherited and enabled flags with debugfs. The Linux reference
kernel lacks `CONFIG_UNICODE`, so no Linux mount check exists for these images.

## Quota tests

`tests/generate_quota_fixtures.py --tools-root E2FSPROGS_BUILD --output NEW` makes a 4 KiB
volume with user, group and project quotas, a 1 KiB one that also has EA_INODE, and
a 4 KiB BIGALLOC volume with 16 KiB clusters and user and group quotas. debugfs writes
files for three owners, groups and projects; `e2fsck -fy` computes the quota files and
strict fsck must then pass. Configure `-Dquota_fixtures=NEW` to add three
`ext4-quota-test` scenarios and three `--faults` cases to the `format` suite and three
4,000-operation profiles to the `sustained` suite.

The scenario creates files for 40 new owners and three IDs whose index paths diverge
at every level, then changes ownership, truncates, adds an external attribute block
and, with EA_INODE, a value inode, removes a held file and replaces another by rename,
creates a directory and a symlink and removes a distant owner's only file. On PROJECT
volumes it moves a file to another project and checks PROJINHERIT inheritance and
cross-project link and rename refusal. An unlinked file held at unmount must stay
charged until offline recovery reclaims it. A reader in the test walks the quota
trees through ordinary reads and compares each changed ID with the charged inodes.
`--faults` fails every allocation and read and cuts power at every event, with torn
and partially surviving writes, while creating a directory for new IDs and while
moving a file to a new owner; recovery must reach the unchanged or the committed image.
An export directory receives the scenario image and both committed fault images;
`tests/check_quota.py --tools-root E2FSPROGS_BUILD --exports DIRECTORY --output NEW`
requires strict fsck, which recomputes all usage and fails on any difference, and
checks the moved, released and new IDs with debugfs.

`tests/run_linux_quota.py` runs from the lab with `--lab`, `--prepared`, `--runner`,
`--modloop`, `--core-test` (an `ext4-quota-test` from the core under test), one
`--exports` directory per scenario export and a fresh `--output`. The pinned kernel
builds the quota tree and v2 format as modules, so the runner extracts `quota_tree.ko`
and `quota_v2.ko` with `tests/squashfs_extract.py` from the Alpine 3.22.5 netboot
`modloop-virt` whose kernel and initramfs match the pinned inputs; the runner checks
the modloop digest. For each export Linux reports every used ID through quotactl,
which must equal the e2fsprogs reading. Linux then removes ten owners' files, moves a
file to a distant owner and back, creates files for five new owners and changes a
project; strict fsck and the e2fsprogs reading must agree with Linux, and a quota
file must record a freed block or entry. `ext4-quota-test --continue IMAGE EXPORT`
adds 30 owners that reuse those freed entries, restores the distant ID, removes a
Linux-created file and changes a project; strict fsck and Linux must then agree again.

`ext4-quota-test --enforce IMAGE [EXPORT]` writes limits for an existing owner and a
project into their quota entries, as setquota does, installs an enforcement policy
with a test clock and checks hard, partial, soft-grace, exemption, inode and project
refusals; the `format` suite runs it on the user-and-project volumes. Passing the
export directory to `run_linux_quota.py --enforced-exports` boots Linux with quota
mount options: an unprivileged process of that owner must be refused with EDQUOT
exactly where the limits e2fsprogs reads require.

## Metadata fuzzing

`-Dfuzzer=true` builds `ext4-image-fuzzer` using Clang's libFuzzer runtime,
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

The same option builds `ext4-operations-fuzzer`. Each input is a list of mutations
applied to nonzero blocks of the `--image` through a copy-on-write overlay, followed
by offline recovery, a bounded read-only namespace walk without and then with the
Linux probe's fscrypt key, and a fixed sequence of writable operations that also
encrypts, fills and casefolds new directories and enables verity on a new file; results are admissible, while memory errors, unbounded work
and unreleased allocations or key handles are findings. A mutation may instead select a nonzero
block of the internal journal and may request checksum repair. Repair follows the
ordinary log from its start as the recovery scanner does, recomputing each
descriptor's data-tag and tail checksums, revoke tails and commit checksums, and
recomputes every fast-commit tail CRC over the preceding records, so malformed
records reach replay validation instead of stopping at a checksum. Repair stops
where a mutated header, sequence or record length ends the scan and does not repair
metadata checksums. Run one worker per image and corpus, for example
`ext4-operations-fuzzer --image=ABSOLUTE_IMAGE corpus -max_total_time=1500
-timeout=20 -max_len=1024 -rss_limit_mb=3072 -artifact_prefix=crashes/`, and keep
corpora, logs and findings under ignored `artifacts/fuzz-operations/` directories.

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

The fast-commit development probe runs in the lab's prepared disposable Linux
environment. Run `tests/run_linux_fast_commit.py --help` for its required lab,
prepared reference, runner and fresh output paths. By default it asks Linux to
create and directly recover a pending fast journal. `--recover /absolute/path/to/ext4-recover`
instead recovers a copy with the portable utility before Linux verifies it; the
report labels this different path explicitly. `--block-size 1024` or `4096`
limits a capture. The harness freezes its probe source and binary, retains pending
images and journal dumps, and checks the recovered image without repairing it.
`--orphan-file` enables the modern orphan file and keeps an allocated, unlinked
inode open across the initial full checkpoint and final crash. The capture must
identify that held inode and retain `orphan_present` in the pending filesystem.
The normal capture rejects unexpected full-commit fallback, and core recovery
must report a positive fast-commit replay count. `--special-files` adds three
symlinks, character/block devices, a FIFO and a socket after the fast prefix.
The pinned Linux writer makes these operations ineligible for fast commit. This
mode explicitly requires the data-journalling fallback and ordinary core replay
with zero fast commits; it tests supersession of the old prefix, not native
generation of special-inode fast records.
The expanded native direct-replay failure is documented in ACCEPTANCE.md.

After capture, the focused executables accept the protected inputs directly:

```sh
.build/ext4-fast-commit-test CAPTURE/pending.journal EXPECTED_TID EXPECTED_COMMITS
.build/ext4-fast-commit-recovery-test CAPTURE/pending.img VERIFIED_REFERENCE_IMAGE --faults
.build/ext4-fast-commit-recovery-test CAPTURE/pending.img VERIFIED_REFERENCE_IMAGE --resources
```

The second command performs recovery in memory and compares against an independently
verified reference. Its interruption cases vary both partial writes and device-cache
survival. The resource mode injects each observed allocation and read failure,
then restarts recovery from durable bytes. Keep the reference's provenance in the
capture report: native direct
recovery and core recovery followed by native verification are different evidence.
These native-generated fixtures are not yet generated by the ordinary CI workflow.

The ordinary CI instead generates bounded protocol fixtures with e2fsprogs, without
linking the core into the fixture author. `debugfs` independently creates the expected
filesystem, including holes, directory creation and growth, long names, hard links
and rename. The serialized journal contains three fast commits with block padding.
Profiles cover 1/4 KiB blocks, an explicit checksum seed and absent metadata checksums.
Two additional orphan-file profiles combine inode-generation reuse, final unlink,
a checkpointed unlinked inode, an interrupted linked truncate and a legacy orphan
in the same pending filesystem. Their expected filesystems are independently
authored before serializing the orphan state and fast-commit records.
Two special-inode profiles add 7/59/60-byte symlink targets, legacy and extended
device encodings, a FIFO and a socket. Their journals are protocol fixtures;
the pinned native Linux writer instead uses its full-commit fallback for these
inode changes.

Three attribute-reuse profiles cover modern orphan slots at both block sizes and
a legacy orphan chain at 1 KiB. Two old inode generations share a 64 KiB body
value and an external block with a surviving third owner; the first also owns
two private values. Both old inode numbers are reused in the same fast prefix.
The expected image uses e2fsprogs' whole-block detach before removing body keys:
the pinned debugfs per-key removal of a shared external block incorrectly frees
value inodes still owned by that block. Expected images must pass strict fsck
without repair before they become references.

Two indirect profiles omit EXTENTS and 64BIT. Their sparse file crosses the direct,
single-indirect and double-indirect boundaries; a created file needs a new indirect
path. The replaced old generation owns all three indirect levels. Legacy orphans,
mapped symlinks and special inodes share the same committed prefix. Special inodes
are allocated before the expected image releases orphan numbers, so only the
explicit replacement reuses an old inode number. Malformed logs add unwritten
indirect data and an address beyond the indirect logical limit.
The journal-map guards inject a hole, repeated data block, data/node overlap,
reused mapping node and a pointer cycle. Each must reject before any write.
Recovery output also records allocation and read counts before result comparison;
these are operation counts for the fixture, not a throughput benchmark.

Format-combination profiles repeat the basic scenario on a 4 KiB BIGALLOC volume with
16 KiB clusters, in a casefolded directory, and on a volume with user, group and
project quotas; e2fsck computes the quota usage of both reference images. The
large-prefix-casefold profile adds 1,024 long names to a casefolded directory, so
replay builds an index with casefolded hashes. The core's conversion accounts quota
usage, which Linux leaves to fsck after its own fast-commit replay; strict fsck of
the replayed image checks both. The fixture helper cannot yet serialize a 64 KiB
profile.

The huge-prefix profile creates 1,024 long-name files at 1 KiB with a 2 MiB fast area.
Its conversion needs more than the ordinary 256-snapshot transaction bound, so it
exercises the recovery transaction bound; it has no conflicting-owner variant.
`generate_fast_commit_fixtures.py --profile NAME` and `check_fast_commit.py --profile
NAME` restrict generation and checking to explicitly selected profiles.
The large-prefix profiles create 256 files at 1 KiB and 1,024 at 4 KiB, then revisit
their inode and range records in reverse order. Repeated single-block ranges and
containing two-block ranges exercise exclusion-union semantics. They retain three
committed prefixes and use a 1 MiB fast area in an 8 MiB journal; the ordinary
conversion credit bound is unchanged. Their sampled resource tests include early,
late and evenly spaced failure positions. Sampled power cuts additionally include
every durability barrier and its adjacent events. The smaller profiles retain
exhaustive allocation/read failures and write/barrier cuts.
Duplicate CREATE and LINK records revisit existing names at the beginning, middle
and end of the created set; identities and link counts must remain unchanged.
Two valid-CRC malformed logs instead link an existing name to another inode and
must reject without home or journal writes.
The special and indirect profiles add two ownership-corruption logs, whose range for
the renamed file claims an inode-table block or a block owned by `lost+found`, an
inode the log never names; both must reject without writes.

`ext4-fast-commit-recovery-test PENDING EXPECTED --benchmark` performs one checked
warmup and 31 measured recoveries. Each recovery starts from the same image in the
test's memory-backed device, with its modeled volatile cache and durability events.
Timing excludes image restoration, result comparison and hashing; every sample
still compares the recovered filesystem with the independently authored reference.
Output includes elapsed nanoseconds, read/allocation counts, peak core allocation,
durability-event count and an image checksum. Use equivalent optimized unsanitized
builds and the same test driver/fixtures when comparing source revisions. These
measurements exclude real device latency and mounted filesystem operation.

For native consumption of these protocol images, pass
`--xattr-fixtures /absolute/path/to/fast-commit-fixtures` and `--recover` to
`run_linux_fast_commit.py` with the same lab, prepared reference, runner and fresh
output arguments described above. This mode excludes `--orphan-file` and
`--special-files`, which select native capture. It recovers independent image
copies with the core, requires the recorded fast-commit count, boots Linux to read
both replacements and every surviving attribute, and requires clean unmount plus
strict fsck and unchanged namespace/value hashes. It proves Linux consumption of
core output, not Linux generation or direct replay of that pending log.
The mutually exclusive `--indirect-fixtures` option uses the same procedure for the
two indirect profiles. Linux reads the complete sparse file, checks its hardlink
identity, reads created files, the replacement inode, truncated orphan and symlink
targets, and checks the special-device identities before clean unmount and
independent verification.
The `--large-prefix-fixtures` option checks every created file in each large- or
huge-prefix profile present in the fixture report, including its full contents,
permissions and size, plus the sparse source and hardlink identity. Each boot
receives that profile's exact file count. It is mutually exclusive with the other
protocol profiles.

```sh
python3 tests/generate_fast_commit_fixtures.py --tools-root E2FSPROGS_BUILD \
  --output artifacts/fast-commit-fixtures
meson configure .build -Dfast_commit_fixtures=artifacts/fast-commit-fixtures
meson compile -C .build
meson test -C .build --no-rebuild --print-errorlogs 'fast-commit-*'
python3 tests/check_fast_commit.py --tools-root E2FSPROGS_BUILD \
  --fixtures artifacts/fast-commit-fixtures --recover .build/ext4-recover \
  --output artifacts/fast-commit-independent
```

The checker requires nonrepairing e2fsck, independently read namespace, file
hashes, symlink bytes, device identities and exact attribute values, and an
unchanged image after a second clean recovery. It exports regular file data and
attribute bytes; special nodes are inspected
inside the image and never created or opened on the host. In-memory comparisons
also check inode metadata and free-inode counts; any free-block difference must be
charged exactly to a different reconstructed directory/index layout. Generated
inputs remain unchanged. This is independent output verification, not a native
Linux capture. The generator's optional `--direct-replay` additionally requires
e2fsck's direct fast replay and currently reproduces its directory-growth failure;
an unsuccessful oracle replay is never used as the expected filesystem.
The orphan profiles also run `--orphans` guards: duplicate slots, overlap with the
legacy chain, reserved/out-of-range inode numbers and damaged tails must be rejected
before any conversion write. Resource and durability failures cover both the fast
conversion and the subsequent ordinary orphan-cleanup transactions.
Each special-inode profile also supplies five valid-CRC malformed journals:
invalid short-link size, embedded NUL, missing terminator, nonzero special-inode
size and a missing mapped-link range. `--reject` requires corruption with no home
or journal writes and no leaked allocations.

Add `--checksum-v1` before the image to select v1 with 32/64-bit tags. Add
`--export-only` after that option to retain committed logs without repeating the
full fault matrix. `check_journal_recovery.py --writers EXPORT_DIRECTORY` checks
these exports with both portable and e2fsck journal-only replay, followed by exact
block comparisons and nonrepairing consistency checks. Native v1 roundtrips require
the `ext4-no-checksum.img` source: Linux otherwise upgrades journal checksums along
with `metadata_csum`. The Linux harness selects `journal_checksum` for v1 exports,
requires an independently decoded revoke, and compares both reverse replayers.

Add `--async` after `--checksum-v1`, or use it alone for v2/v3 profiles, to exercise
async-format journals with the same ordered writer. The independent checker adds
`--discard-commit --tools-root /path/to/e2fsprogs/build` to corrupt only the commit
checksum on a private copy and require both replayers to discard that transaction.
The Linux harness selects `data=writeback,journal_async_commit`, fsyncs the file
before stopping and requires a decoded directory-block revoke. This mount mode
matters because Linux deliberately omits revokes in `data=journal` mode.

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

For independent v1 input, add `--checksum-v1` to the generator and select
`ext4-no-checksum.img`. It generates committed, uncommitted-tail and later-rewrite
cases; the same checker verifies them. Multi-descriptor transactions remain in
the 1 KiB portable durability suite. Each checker preserves its original input.

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

## Interrupted foreign replay

`tests/check_interrupted_replay.py --fixtures DIRECTORY --image PENDING... --tools-root
E2FSPROGS_BUILD --recover EXT4_RECOVER --output NEW` takes the manifest of
`generate_journal_fixtures.py` and any other pending images, such as those Linux left
in the lab's journal runs. It builds `tests/write_trace.c` as a preload library, with
DYLD interposition on macOS and `LD_PRELOAD` on Linux, and runs `e2fsck -E
journal_only`, which uses e2fsprogs' copy of the kernel's jbd2 recovery code. The
library records every write and flush e2fsck issues to the image. Crash states follow
page-cache semantics: between flushes, writes coalesce per filesystem block and may
reach the device in any order, so each state keeps any subset of the current epoch's
dirty blocks on top of all earlier epochs; epochs of more than ten blocks contribute
their prefixes and single omissions. While the log still holds transactions, the
core must recover each state to exactly the non-journal contents of its own recovery
of the untouched image, and strict fsck must pass. After e2fsck has emptied the log
it continues with unjournaled orphan release and superblock updates; those states
are counted as clean, refused by the core, or left for fsck, since neither the core's
nor Linux's mount-time recovery audits every bitmap. The state after all of e2fsck's
writes must recover and pass strict fsck.

Interrupted replay by Linux itself is recorded below the filesystem.
`tests/run_linux_log_writes.py` runs from the lab with `--lab`, `--prepared`, a
two-disk `--runner`, `--modloop`, `--recover`, one `--image` per pending volume and
a fresh `--output`. The guest probe `tests/linux_log_writes.c` loads `dm-mod` and
`dm-log-writes` from the pinned Alpine module image, places a log-writes target over
the pending volume with its log on the second disk, mounts ext4 through it, which
replays the journal and its fast commits, unmounts and removes the target. The
runner parses the log, requires the complete log to reproduce Linux's result
exactly and that result to pass strict fsck and read like the core's recovery of
the untouched volume. Power-cut states follow the device's volatile cache: a flush
makes every earlier completed write durable, a FUA write is durable when it
completes, and any subset of the other completed writes may have reached the
medium. While the log is still pending, the core must recover each state, strict
fsck must pass and independently read names, data and attributes must equal the
untouched recovery; blocks are not compared, as Linux places replayed names by its
own algorithm. States after Linux emptied the log are counted as for e2fsck. The
report lists every failing state and keeps the first eight failing images.

## Orphan recovery tests

`ext4-orphan-test IMAGE...` constructs linked-truncate intents in RAM copies and
checks cleanup, malformed lists, stale summaries, interrupted recovery and retries.
It is included in Meson for the ordinary writable profiles. Use `--smoke --export
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
meson setup --reconfigure .build -Dorphan_file_tests=true
```

`orphan_file_fixtures` selects another fixture directory. The generator's
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
meson setup --reconfigure .build -Dxattr_fixtures="$PWD/artifacts/xattr-fixtures"
meson compile -C .build ext4-xattr-test
env -i PATH="$PATH" meson test -C .build 'xattr-reader-*' --no-rebuild --print-errorlogs
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

## Large attribute values

EA_INODE fixtures use the pinned e2fsprogs build's static libext2fs and a small
fixture-only helper. This avoids debugfs's one-block input limit for `ea_set -f`.
The helper is not linked into the portable core. The journal is at least 2 MiB
and 1,024 blocks, allowing Linux's 64 KiB replacement credit reservation.
Generate all six profiles and
run their reader, mutation, corruption, fault and private-orphan tests with:

```sh
python3 tests/generate_ea_inode_fixtures.py --tools-root /path/to/e2fsprogs/build \
  --output artifacts/ea-inode-fixtures
meson setup --reconfigure .build -Dea_inode_fixtures="$PWD/artifacts/ea-inode-fixtures"
meson compile -C .build
env -i PATH="$PATH" meson test -C .build 'ea-inode-*' --no-rebuild -j 2 --print-errorlogs
mkdir artifacts/ea-inode-exports
for image in artifacts/ea-inode-fixtures/*.img; do
  .build/ext4-ea-inode-test --export artifacts/ea-inode-exports "$image"
  .build/ext4-ea-inode-test --value-orphans --export artifacts/ea-inode-exports "$image"
done
python3 tests/check_ea_inode.py --fixtures artifacts/ea-inode-fixtures/report.json \
  --exports artifacts/ea-inode-exports --tools-root /path/to/e2fsprogs/build \
  --output artifacts/ea-inode-independent
```

Twenty registered cases cover six reader, six mutation/corruption, six private
orphan and two exhaustive mutation-fault suites. The checker requires eighteen
states per profile and never repairs source/export images. It replays an isolated
copy with `e2fsck -y -E journal_only`, rejects any full-check passes at this stage,
then verifies exact values, namespace and counters before `e2fsck -fn`. Use repeated
`--state` arguments to inspect only named states during a focused development
batch. CI runs the complete 108-state matrix at the feature boundary.

`ext4-ea-inode-test --fault-smoke --export DIRECTORY IMAGE...` emits before,
after, committed-pending and uncommitted states for create/replace/remove and
shared-block copy. It omits the already registered exhaustive failure sweeps.
Check the 1 KiB and 128-byte-inode exports using
`tests/check_ea_inode_faults.py --exports DIRECTORY --recover EXECUTABLE
--tools-root E2FSPROGS_BUILD --output DIRECTORY`. The 48-state oracle comparison
includes exact metadata/value/data transitions, clean nonrepairing fsck and
idempotent recovery. Its `linux-pending.json` selects two verified committed inputs.

Prepare native clean inputs from the functional checker's report using
`tests/prepare_ea_inode_linux.py --report REPORT --recover EXECUTABLE
--tools-root E2FSPROGS_BUILD --output DIRECTORY`. The resulting `selection.json`
and pending selection feed `tests/run_linux_journal.py --xattrs --xattr-reader
EXECUTABLE --recover EXECUTABLE` from the explicit lab directory; add `--pending`
for the latter. The Sol/Luna VM handoff applies. The EA profile compares core and
actual Linux recovery of Linux-authored journals and open-unlinked owners, then
mutates shared values through the core and checks the returned image in Linux.
Retain e2fsprogs journal-only orphan failures separately; they are not clean passes.

CI defaults to all nine suites. Fast-commit recovery, its fixtures and its
independent check form their own `fast-commit` suite so the `core` job keeps headroom
on slower runners. A manual workflow dispatch can select one suite after changes
limited to its tests or fixtures. Keep the completed evidence for
unchanged suites, and run every affected suite when the portable core changes.

Large-volume tests use sparse 4–16 TiB images with less than 18 MB initially stored.
The host must support both the logical file size and SEEK_DATA/SEEK_HOLE. Do not
copy, hash or archive these images by reading their entire logical contents; use
the bounded helpers in `tests/sparse_image.py`.

```sh
python3 tests/generate_large_volume_fixtures.py --tools-root /path/to/e2fsprogs/build \
  --output artifacts/large-volume-fixtures
meson setup --reconfigure .build -Dlarge_volume_fixtures="$PWD/artifacts/large-volume-fixtures"
meson compile -C .build ext4-large-volume-test ext4-recover
python3 tests/check_large_volumes.py --tools-root /path/to/e2fsprogs/build \
  --fixtures artifacts/large-volume-fixtures/report.json \
  --test .build/ext4-large-volume-test --recover .build/ext4-recover \
  --output artifacts/large-volume-independent
```

The checker preserves pending images and compares core recovery with independent
raw journal replay followed by strict nonrepairing fsck. Its output directory must
be new so a failed attempt cannot be overwritten.

`prepare_large_volume_linux.py --fixtures FIXTURE_REPORT --report CHECKED_REPORT
--output NEW_DIRECTORY` selects the checked mutation states and committed journals
using sparse copies. From the explicit lab directory, give its `selection.json`
to `run_linux_journal.py --xattrs --xattr-reader /absolute/path/ext4-large-volume-test`
with the prepared `--recover`, `--module-report`, `--lab` and a new `--output`.
Each profile exercises Linux mutation, core/Linux recovery and orphan cleanup,
direct Linux replay of the core's committed journal, then core mutation and Linux
verification. Image identities use the explicitly recorded sparse digest format;
executable identities remain ordinary SHA-256. Preserve executable permissions
when freezing the two binaries for a run.

## External journal device pairs

The core's `ext4_mount_writable_with_journal` and `ext4_recover_with_journal` APIs
take the explicitly owned second resource. NULL selects the internal journal.
Generate and check independent pairs with:

```sh
python3 tests/generate_external_journal_fixtures.py --tools-root /path/to/e2fsprogs/build \
  --output artifacts/external-journal-fixtures
meson setup --reconfigure .build
meson configure .build -Dexternal_journal_fixtures=artifacts/external-journal-fixtures
meson compile -C .build ext4-external-journal-test ext4-recover
meson test -C .build --no-rebuild 'external-journal-*'
python3 tests/check_external_journals.py --tools-root /path/to/e2fsprogs/build \
  --fixtures artifacts/external-journal-fixtures --test .build/ext4-external-journal-test \
  --recover .build/ext4-recover --output artifacts/external-journal-independent
```

The fixture author links only with e2fsprogs. Its registered filesystem UUID differs
from the journal device UUID. Each journal reserves at least 1,024 usable ring
blocks after its control prefix, as required for a Linux mount. The checker
compares complete transactions and both recovery outcomes, verifies paired-device
idempotence, and uses strict `e2fsck -fn
-j JOURNAL IMAGE`. Its oracle invokes `e2fsck -y -E journal_only` without `-f` and
rejects any filesystem repair pass. Force checking overrides journal-only mode.

From the explicit lab directory, `run_linux_external_journal.py` accepts `--lab`,
`--fixtures`, `--independent`, `--test`, `--recover`, `--module-report`, `--runner`
and a new `--output`, all as absolute paths. Use a signed runner built from the
lab's `scripts/linux-vm.swift`, with both disposable images attached. Four native
boots per 1/4 KiB profile check Linux mutations and open-unlinked recovery, direct
Linux replay of a committed core log, and Linux verification of returned core
mutations. The 64 KiB profile remains portable-only on the pinned 4 KiB-page kernel.
The harness freezes executable copies and checks original input identities.

## Extended-attribute mutation tests

The same ten fixtures drive `xattr-mutation-*` and `xattr-packing-*`. The first
enumerates every post-lookup allocation/read failure and write/flush interruption
for six atomic operations. `--smoke` exports their successful and interrupted states
without repeating fault sweeps; it does not replace the registered Meson suites.

```sh
meson compile -C .build ext4-xattr-write-test ext4-recover
env -i PATH="$PATH" meson test -C .build 'xattr-mutation-*' 'xattr-packing-*' -j 2 --no-rebuild --print-errorlogs
mkdir artifacts/xattr-write-exports artifacts/xattr-edge-exports
.build/ext4-xattr-write-test --smoke --export artifacts/xattr-write-exports artifacts/xattr-fixtures/*.img
.build/ext4-xattr-write-test --edges --export artifacts/xattr-edge-exports artifacts/xattr-fixtures/*.img
python3 tests/check_xattr_writes.py --fixtures artifacts/xattr-fixtures/report.json \
  --exports artifacts/xattr-write-exports --recover .build/ext4-recover \
  --tools-root /path/to/e2fsprogs/build --output artifacts/xattr-write-independent
python3 tests/check_xattr_writes.py --edges --fixtures artifacts/xattr-fixtures/report.json \
  --exports artifacts/xattr-edge-exports --recover .build/ext4-recover \
  --tools-root /path/to/e2fsprogs/build --output artifacts/xattr-edge-independent
python3 tests/generate_xattr_space.py --fixtures artifacts/xattr-fixtures/report.json \
  --tools-root /path/to/e2fsprogs/build --output artifacts/xattr-space-fixtures
meson setup --reconfigure .build -Dxattr_space_fixtures="$PWD/artifacts/xattr-space-fixtures"
env -i PATH="$PATH" meson test -C .build 'xattr-full-space-*' --no-rebuild --print-errorlogs
mkdir artifacts/xattr-full-exports
.build/ext4-xattr-write-test --full --export artifacts/xattr-full-exports artifacts/xattr-space-fixtures/*.img
python3 tests/check_xattr_writes.py --full --fixtures artifacts/xattr-space-fixtures/report.json \
  --exports artifacts/xattr-full-exports --recover .build/ext4-recover \
  --tools-root /path/to/e2fsprogs/build --output artifacts/xattr-full-independent
```

The independent checker preserves source images and mutates only newly created
recovery copies. CI uses `--discard-recovered` to delete each verified copy after
recording its final hash, reducing disk use while retaining reports and commands.

## Attribute lifetime and first-attribute tests

The `xattr-lifetime-*` cases use the ordinary attribute fixtures. They include
creation, admitted data/attribute transitions, held inode mutation and final-release
fault sweeps. `--release` selects just the final-release cases when investigating
that contract. Independent checking uses the same selection:

```sh
meson compile -C .build ext4-xattr-lifetime-test ext4-recover
env -i PATH="$PATH" meson test -C .build 'xattr-lifetime-*' --no-rebuild -j 2 --print-errorlogs
mkdir artifacts/xattr-release-exports
.build/ext4-xattr-lifetime-test --release --smoke --export artifacts/xattr-release-exports \
  artifacts/xattr-fixtures/*.img
python3 tests/check_xattr_lifetime.py --release --fixtures artifacts/xattr-fixtures/report.json \
  --exports artifacts/xattr-release-exports --recover .build/ext4-recover \
  --tools-root /path/to/e2fsprogs/build --output artifacts/xattr-release-independent --discard-recovered
```

First-attribute tests require separately generated clean images whose EXT_ATTR
feature is absent. They cover the feature bit, inode and new attribute in the same
transaction, plus forbidden recovery feature changes:

```sh
python3 tests/generate_xattr_enable_fixtures.py --tools-root /path/to/e2fsprogs/build \
  --output artifacts/xattr-enable-fixtures
meson setup --reconfigure .build -Dxattr_enable_fixtures="$PWD/artifacts/xattr-enable-fixtures"
meson compile -C .build ext4-xattr-lifetime-test ext4-recover
env -i PATH="$PATH" meson test -C .build 'xattr-first-attribute-*' --no-rebuild -j 2 --print-errorlogs
mkdir artifacts/xattr-enable-exports
.build/ext4-xattr-lifetime-test --enable --smoke --export artifacts/xattr-enable-exports \
  artifacts/xattr-enable-fixtures/*.img
python3 tests/check_xattr_lifetime.py --enable --fixtures artifacts/xattr-enable-fixtures/report.json \
  --exports artifacts/xattr-enable-exports --recover .build/ext4-recover \
  --tools-root /path/to/e2fsprogs/build --output artifacts/xattr-enable-independent --discard-recovered
```

Without either selection, the independent lifetime checker inspects the ordinary
creation/write/truncate/namespace exports. The pinned e2fsck linked-truncate defect
described in ACCEPTANCE.md produces an actual failed replay in that mode.
`--keep-going` retains those failures and their images while inspecting other states;
the command still returns nonzero. It must not be used to turn a failed oracle into
acceptance. The separate Linux `--xattr-truncate` probe covers recovery of the affected
pending exports, with the same disposable-VM ownership handoff as other Linux probes.
To reproduce the e2fsck defect directly from an unmodified tool-generated fixture:

```sh
python3 tests/reproduce_e2fsck_xattr_orphan.py --image artifacts/xattr-fixtures/xattr-1k.img \
  --tools-root /path/to/e2fsprogs/build --output artifacts/e2fsck-linked-xattr-reproducer
```

This diagnostic succeeds only when the specified tool defect is reproduced; its
report explicitly marks the oracle recovery as failed. It never executes the core.
The default retains every recovery image. Full-space checks also hash all filler
data/mapping blocks and require them to remain unchanged.

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
reader/write profile, and configure `-Dextended_tests=true -Dinode128_test=true`.
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

`ext4-file-range-test IMAGE...` checks preallocation and hole punching, including
partial progress, xattr transitions, extent splits and indirect path removal.
`--faults` exercises resource failures and interrupted commits; `--export DIRECTORY`
retains functional states plus committed/uncommitted journal images. Check them with
`tests/check_file_ranges.py --fixtures FIXTURE_DIRECTORY --exports DIRECTORY
--recover .build/ext4-recover --tools-root E2FSPROGS_BUILD --output NEW_DIRECTORY`.
Its report feeds `tests/run_linux_journal.py --namespace`, with `--pending` for
atomic records. Linux checks the retained file, creates its own preallocation and
hole, then commits a journal for core/oracle replay. Run
`ext4-file-range-test --linux-read RETURNED_CORE_IMAGE` to verify those Linux-authored
bytes through the portable reader. Use the same isolated VM handoff as other
Linux checks. The indirect profile supports punching and rejects reservation.

With the existing full-space fixtures, `ext4-file-range-test --capacity-full`
checks allocation-free writes into a full inode root, `--capacity` spans zeroing
transactions, and `--capacity-tree` fills an external extent leaf. Each accepts
`--export DIRECTORY`. `tests/check_range_capacity.py --fixtures DIRECTORY
--exports DIRECTORY --output NEW_DIRECTORY` checks the large case; add `--full`
for small roots, or `--full --external` for external leaves. `--tools-root` selects
e2fsprogs as above. `ext4-file-range-test --capacity-faults` injects resource and
storage failures across preparation and the final data commit, including deliberately
nonzero inaccessible backing. KEEP_SIZE variants `--capacity-keep`,
`--capacity-keep-large` and `--capacity-keep-tree` test growth reaching the extent's
last block; add `--keep-size` to the independent checker with the corresponding
geometry options above. They also retain unchanged-media rejection when growth
ends earlier in an imported full tree and still needs a split. `--capacity-keep-faults` covers interrupted
preparation/data/EOF transitions; its `--export DIRECTORY` mode emits before/after
and two journal cuts. Check those using `--keep-size --faults --recover
.build/ext4-recover`, then use its checked after records with the Linux harness's
`--pending` mode. `--capacity-keep-prefix` checks durable progress followed by a
hole that cannot be allocated. No new capacity fixtures need to be generated.
Linux capacity checks change a byte while free blocks
remain zero and require that exact change after independent and core journal replay;
a same-byte write cannot establish that the write survived.

`ext4-file-range-test --reservation` exercises public KEEP_SIZE reservations and
repeated partial EOF growth across three separated extents on a full filesystem.
`--reservation-tree` fills an external leaf while retaining the required mapping
capacity; `--reservation-controls` covers size-only growth, re-reservation, hole
punching, final release and a failed growing reservation's retained capacity.
`--reservation-faults` covers resource failures and every storage cut while moving
the initialized/unwritten boundary between extents. The first, tree and fault
modes accept `--export DIRECTORY`; fault export mode avoids rerunning its matrix.

Check these exports with `tests/check_reservation.py --fixtures SPACE_FIXTURES
--exports EXPORTS --output NEW_DIRECTORY`, adding `--tree` or
`--faults --recover .build/ext4-recover` as appropriate. `--tools-root` selects
e2fsprogs. Each image's extent map is decoded in one independent debugfs command,
with exact data, map flags and physical locations compared across states. The
checked written/after records feed `run_linux_journal.py --namespace`, including
`--pending` for fault exports, and the returned images use `--linux-read` above.
CI runs all twelve registered reservation tests and thirty independent states.

Persistent flag tests reuse the xattr fixtures: `ext4-inode-flags-test IMAGE...`
runs operations/protection/inheritance, `--faults` injects set/clear failures, and
`--export DIRECTORY` writes clean and interrupted states. Check exports using
`tests/check_inode_flags.py --fixtures XATTR_FIXTURES --exports DIRECTORY
--output NEW_DIRECTORY --recover .build/ext4-recover`; `--tools-root` selects
the e2fsprogs build. The namespace Linux harness accepts these checked records,
including `--pending` flag journals, and verifies actual flag ioctls and denied
operations before authoring its reverse journal. `ext4-inode-flags-test --linux-read`
checks the returned core-recovered images.

Generate deliberately nonzero unwritten backing data with
`tests/generate_allocation_fixtures.py --fixtures EXTENDED_FIXTURE_DIRECTORY
--tools-root E2FSPROGS_BUILD --output NEW_DIRECTORY`. Enable its Meson suite with
`-Dunwritten_tests=true -Dallocation_fixtures=ABSOLUTE_NEW_DIRECTORY`.
The generator preserves its source images and records each independent mapping,
the exact modified data blocks, tool output and nonrepairing e2fsck result.

`tests/run_linux_journal.py --allocation` accepts the allocation checker's
`report.json` as `--exports`. It verifies the full grown file in Linux, extends
it with a new allocation, commits and powers off without unmount. The returned
image must contain a pending Linux journal that the portable core can recover,
with exact file bytes/attributes and e2fsck verified afterward. The same lab
working directory, explicit profile selection and Sol/Luna VM handoff apply.

The `Portable filesystem` GitHub Actions workflow runs on development/main pushes
and pull requests. Separate Ubuntu jobs cover the core, orphan files, namespace,
removal, rename and indexed directories, each generating fresh fixtures and building with Clang and
ASan/UBSan. They run disjoint Meson suites, inspect mutation exports independently
and recover debugfs-authored journals. Reports and logs are retained; successfully
verified image exports are released between stages, and namespace exports are
checked one source profile at a time to bound disk use.
Each branch and suite selection retains its active run to completion. New pushes
queue the latest source revision; a newer pending revision can replace an older
pending one, but publishing does not cancel the already running acceptance checks.
CI uses `debugoptimized` with both sanitizers enabled and two concurrent Meson
workers; fixture and fault coverage is identical to `debug`. Use the same build
type locally when reproducing CI timing.
This portable CI does not replace selected-Xcode formatting, unsigned platform
builds, actual macOS mounts, or the separately identified kernel/LXNU VM tests.

Namespace tests use the ordinary writable and orphan-file profiles. Add six small
multi-group fixtures and their exhaustion/group-transition suites with:

```sh
python3 tests/generate_namespace_fixtures.py --tools-root /path/to/e2fsprogs/build \
  --output artifacts/namespace-fixtures
meson setup --reconfigure .build -Dnamespace_tests=true
meson compile -C .build -j 4
env -i PATH="$PATH" meson test -C .build 'namespace-*' -j 2 --no-rebuild --print-errorlogs
```

`namespace_fixtures` selects another fixture directory. The ordinary namespace
and basic indexed-directory tests are always enabled; the modern suite follows
`orphan_file_tests`. To independently inspect one profile after its fault suite:

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

Use `ext4-namespace-test --special` for device, FIFO and socket creation, and
`ext4-rename-test --whiteout` for atomic move/whiteout cases. Both accept the
existing `--smoke --export DIR` options; their numbered exports can share a
directory with the respective ordinary suite. Check them with
`check_namespace.py` or `check_rename.py` and use the resulting report with the
Linux `--namespace --pending` runner. After reverse replay,
`ext4-namespace-test --special-read RETURNED_IMAGE` verifies Linux-created device
identities, FIFOs, sockets and whiteouts through the portable reader.

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
and recovery. Add `--export-only --export NEW_DIRECTORY` after a successful Meson
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
`meson setup --reconfigure .build -Dfixtures=/absolute/path/to/fixtures` selects that
directory for tests. ASan/UBSan are enabled by default and can be disabled for
an adapter build with `-Db_sanitize=none`.

For the extended read profile, add `--extended` to the fixture generator and
configure `-Dextended_tests=true` along with the generated directory in
`fixtures`. This requires all eleven images rather than skipping absent
variations. It covers block sizes from 1 through 64 KiB, 32-bit group descriptors
with indirect mapping, metadata without checksums and an explicit checksum seed.
The 64 KiB image omits the journal to keep its total size at 64 MiB.

Legacy group checksums have a separate optional format package:

```sh
python3 tests/generate_group_checksum_fixtures.py --tools-root E2FSPROGS_BUILD \
  --output artifacts/group-checksum-fixtures
meson setup --reconfigure .build \
  -Dgroup_checksum_fixtures=artifacts/group-checksum-fixtures
meson compile -C .build -j 4
env -i PATH="$PATH" meson test -C .build --no-rebuild -j 2 \
  metadata-checksum 'group-checksum-*' --print-errorlogs
```

The generator requests `uninit_bg` without `metadata_csum`, checks the exact
feature set and runs nonrepairing e2fsck. Four ordinary images cover 32/64-byte
descriptors, 1/4 KiB blocks, extent/indirect maps and 128-byte inodes. Two small
eight-group images retain lazy inode bitmaps, block bitmaps and inode tables.
The package exercises reading, writes, growth, allocation/freeing, orphan cleanup,
namespace mutations, inode exhaustion, group transitions and interrupted commits.
CI also exports writes, allocation, truncate, growth and namespace states for the
existing independent checkers; no new filesystem oracle is shared with the core.

Distributed descriptor and sparse-superblock layouts have their own bounded images:

```sh
python3 tests/generate_geometry_fixtures.py --tools-root E2FSPROGS_BUILD \
  --output artifacts/geometry-fixtures
meson configure .build -Dgeometry_fixtures=artifacts/geometry-fixtures
meson compile -C .build -j 2
env -i PATH="$PATH" meson test -C .build --no-rebuild -j 2 \
  'group-geometry-*' --print-errorlogs
mkdir artifacts/geometry-exports
for image in artifacts/geometry-fixtures/*.img; do
  .build/ext4-geometry-test --export artifacts/geometry-exports "$image" "${image%.img}.geometry"
done
python3 tests/check_geometry.py --tools-root E2FSPROGS_BUILD \
  --fixtures artifacts/geometry-fixtures --exports artifacts/geometry-exports \
  --recover .build/ext4-recover --output artifacts/geometry-independent
```

The generator obtains expected block locations from `dumpe2fs -g`; mke2fs's
`MKE2FS_FIRST_META_BG` input creates the hybrid profile. The independent checker
batches all inode statistics and file dumps in debugfs, compares every created
inode and file, and verifies both journal outcomes with the core and e2fsck.
The `core` CI job runs the eleven registered cases and all 27 exported transitions.

The selected Xcode clang compiles a second, optimized freestanding archive
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
meson setup .build-space --buildtype=debugoptimized \
  -Dspace_fixtures="$PWD/artifacts/space-fixtures"
meson compile -C .build-space -j 2
env -i PATH="$PATH" meson test -C .build-space 'namespace-space-*' -j 2 --no-rebuild --print-errorlogs
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
meson setup .build-indexed --buildtype=debugoptimized \
  -Dindex_fixtures="$PWD/artifacts/index-fixtures" \
  -Dindex_collisions="$PWD/artifacts/index-collisions/vectors.txt" \
  -Dindex_capacity="$PWD/artifacts/index-capacity-fixture/index-capacity.img"
meson compile -C .build-indexed -j 4
python3 tests/check_directory_hash.py --probe .build-indexed/ext4-directory-hash-test \
  --output artifacts/index-hash-independent
python3 tests/generate_index_collisions.py --probe .build-indexed/ext4-directory-hash-test \
  --output artifacts/index-collisions
env -i PATH="$PATH" meson test -C .build-indexed --suite indexed -j 2 --no-rebuild --print-errorlogs
```

Generators and independent checkers accept `--tools-root` for a prepared e2fsprogs
build. The twenty normal profiles require all their images; missing profiles fail
the configured suite. The separate capacity fixture exercises full-root rejection
and successful record reuse, without enabling LARGEDIR.

After growing the indexed directory, `ext4-index-write-test IMAGE` remounts and links
48 more names while counting device reads of index nodes other than the root. The
first link classifies the index and must read every node and then its path; each
later link, including leaf and node splits, may read each index level at most three
times, when scanning, repacking and enrolling. On trees with at least two nodes, a
node off the next name's path is then damaged before a remount, and that link must
fail as corrupt without writes.

The additional large-directory batch has separate fixture paths:

```sh
python3 tests/generate_index_fixtures.py --large-dir --output artifacts/large-directory-fixtures
python3 tests/generate_large_directory_edges.py --fixtures artifacts/large-directory-fixtures \
  --output artifacts/large-directory-edge-fixtures
meson setup --reconfigure .build-indexed
meson setup --reconfigure .build-indexed \
  -Dlarge_directory_fixtures=artifacts/large-directory-fixtures \
  -Dlarge_directory_edges=artifacts/large-directory-edge-fixtures
meson compile -C .build-indexed -j 2
meson test -C .build-indexed --no-rebuild -j 2 --print-errorlogs 'large-dir-*'
```

Use `--fault-smoke --large-split grow` or `--fault-smoke --large-split cascade`
with `ext4-index-write-test --export DIR IMAGE` to export a prepared large-index
transition. `--faults` sweeps resource and storage faults instead. Compact
`*-capacity.img` images use `--capacity`; the independently authored full-root
image uses `--large-split grow`. Existing independent split/functional checkers
accept the deeper graphs. The capacity checker accepts `--case SOURCE_FILENAME`
to select one verified compact fixture. CI records every independent check after
one portable regression of the complete batch.

Automatic index creation and the real directory link-count boundary use:

```sh
python3 tests/generate_directory_links.py --output artifacts/directory-links-fixture
python3 tests/generate_directory_links.py --large-block-only \
  --output artifacts/directory-large-block-fixture
meson setup --reconfigure .build-indexed
meson setup --reconfigure .build-indexed \
  -Ddirectory_links_fixture=artifacts/directory-links-fixture/directory-links.img \
  -Ddirectory_index_64k_fixture=artifacts/directory-large-block-fixture/directory-64k.img
meson compile -C .build-indexed -j 2
meson test -C .build-indexed --no-rebuild -j 2 --print-errorlogs \
  'directory-create-index-*' 'directory-links-*'
```

`ext4-index-write-test --create-index --fault-smoke --export DIR IMAGE` exports
one automatic index conversion; `--directory-links-image --export DIR IMAGE`
uses the real link-count fixture. `check_index_faults.py` validates both export
families, including committed and uncommitted recovery. The `--directory-links`
flag uses a compact counter model for faults and deliberately produces no exports.
The ordinary 64 KiB reader fixture has no journal; use the separately generated
image above for these mutation checks. The real link-count image is 128 MiB and
contains 64,998 child directories; the generator's `--verify-only` mode inspects
it without repeating creation.

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

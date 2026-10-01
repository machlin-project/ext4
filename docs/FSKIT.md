# FSKit adapter and control app

FSKit is the current native integration priority, before LXNU. The deployment
target is macOS 26.5. Unsigned builds and standalone adapter tests do not
establish installed FSKit behavior.

## Ownership and I/O

The filesystem object owns resource loading and maintenance requests.
`Ext4ResourceIO` retains the block resource and supplies exact, bounded direct
reads and writes. Aligned requests use the caller's buffer; unaligned reads use a
checked bounce buffer and unaligned writes preserve both surrounding edges with
read-modify-write. Short transfers are errors. The adapter does not mix direct and
metadata-cache I/O on overlapping ranges.

Resource probing validates the image through the core and returns `usable` for
an admitted format. Pending journals are discovered through superblock-only
inspection, without reading potentially uncheckpointed root metadata. Recovery
runs only during a writable load with an authenticated persistence barrier;
read-only media and explicit `--rdonly` loads never recover. Read-only access is a
separate mount policy, enforced through
`requestedMountOptions` and mutation rejection. The earlier unconditional
`usableButLimited` result prevented Disk Arbitration from recognizing the volume
on macOS 26.5.2, even though direct FSKit mounts could read it. The guest's Disk
Arbitration log explicitly rejected that result. A successful direct mount is
therefore not sufficient evidence for `diskutil` discovery and mounting.

The FSKit short name and reported file-system type are `machlinext4`. Do not add
an underscore: Disk Arbitration appends `_fskit` internally and strips that suffix
at the first underscore, truncating the former `machlin_ext4` name to `machlin`.
Fixture acceptance uses `hdiutil attach -readonly -owners on -mountpoint ...` to
exercise Disk Arbitration discovery and mounting together. `diskutil info` must
report the actual mount point and enabled ownership; VFS must omit `noowners`.
A separate `diskutil mount` after attaching the raw image with `-nomount` failed
in the tested setup. Direct FSKit mounting also did not establish ownership.
The encryption runner uses the same automount path with `-owners off` because its
Linux-owned fixtures test key enforcement separately from ownership preservation.

Each volume serializes core calls, item publication and control commands through
one recursive monitor. Items retain a core inode hold and the volume; the weak
identity index introduces no cycle. The last item keeps callback storage alive
through release and unmount. On macOS 27, conditional reclaim uses the same monitor
as lookup publication. On older systems, a hold survives until FSKit releases its
last strong item reference. Both paths need installed concurrency acceptance.

The open/close protocol rejects write access on read-only mounts before the
kernel admits cached writes or shared writable mappings. Writable mounts use the
FSKit data cache and core I/O callbacks, with kernel block mappings inhibited.
The last-use notification for an open-unlinked item releases its core hold and
reclaims storage. A descriptor close alone does not release it while mappings or
other users remain. Deactivation clears remaining holds without I/O and releases
the block resource, even if the framework retains stale item objects.

Regular, block-aligned files with ordinary extent or indirect data can supply
validated mappings to FSKit's kernel I/O path. Inline, encrypted and verity files
retain core reads. Partial EOF blocks also retain core reads to avoid exposing
stale on-disk padding. These mappings require an immutable read-only block
resource. Writable volumes use normal FSKit cached I/O instead of exporting maps.

The preferred native transfer size is 128 KiB, distinct from ext4's allocation
block size. The legacy write callback preserves both the committed byte count and
the terminal error, including a partial `ENOSPC`. The adapter must not conceal that
error by reporting an incomplete kernel I/O as successful.

## Writable devices and persistence

The app bundles a ServiceManagement launch daemon whose operations are a health
query, binding a validated block-device name and synchronizing that device's cache.
A separate small setup application registers it through `SMAppService`; macOS
requires approval in Login Items & Extensions. The control app's **Enable disk
writing** button opens this utility. macOS 14.2 and later prohibit a sandboxed app
from registering an unsandboxed daemon, as documented by
[Apple DTS](https://developer.apple.com/forums/thread/802443). The utility handles
installation only and does not participate in mounted filesystem I/O. Remount
after enabling the service; a mounted instance never changes its storage contract
in place. This installer arrangement targets direct macOS distribution.

The extension/control app and daemon authenticate each other with code-signing
requirements for exact peer identifiers and their own signing Team ID. The App Group-prefixed
Mach service is `group.org.machlin.ext4.device-barrier`. Each connection pins one
device descriptor after checking its type and geometry. The daemon uses the public
`DKIOCSYNCHRONIZECACHE` ioctl, with no general ioctl, file-data or descriptor transfer
interface. All filesystem operations and journal ownership remain in the extension.
The control app and extension retain their sandboxes. The setup utility runs as
the ordinary user without a sandbox; only the narrow daemon runs as root.

Writable admission requires writable media, a usable service connection and a
successful device barrier. An unavailable service leaves the volume read-only;
`getInfo.writeUnavailableReason` reports the cause. Once admitted, a failed or
timed-out barrier fails the core transaction; it never acknowledges persistence.
Public FSKit metadata-buffer flushing alone is not used as a device barrier.
Signed installed acceptance of this path is separate from component tests.

The mutation engine handles file/directory/symlink creation, links, unlink/rmdir,
rename and replacement, partial writes, sparse growth, truncate, owner/mode/time
changes and user xattrs. Namespace changes advance directory verifiers. Data
changes conservatively remove set-ID bits and Linux file capabilities in the same
transaction because 26.x callbacks lack caller credentials. Immutable/append flags
map to privileged Darwin system flags; `nodump` maps to the user flag.
Extent preallocation supports physical-EOF and persistent requests. Contiguous or
all-or-nothing allocation and combined size/owner changes remain explicitly
unsupported until the core can carry their full atomic contract.
The shared mutation helper can create FIFO/socket inodes, and component tests
exercise that mapping. FSKit's public create callback, however, admits only files
and directories; native macOS 26.5.2 rejects FIFO/socket creation with `ENOTSUP`.
Installed conformance records these as unsupported features,
separately from passed ordinary-file writes. The core's special-file support is
available to adapters whose native interface can express it.

Volume rename commits the primary superblock label and checksum through the
journal before publishing the new FSKit name. ext4 labels are at most 16 bytes.
Attribute updates leave directory/symlink sizes and unavailable creation times
unconsumed, as FSKit requires, while still applying other supported fields in the
same request. Available-space reporting excludes reserved ext4 blocks.

Writable ownership starts during resource loading. MMP maintenance therefore runs
before activation or mounting, and a maintenance-only unload synchronizes and
releases the claim. Keychain resolution precedes writable ownership so it cannot
delay a claimed volume's heartbeat. Unmount closes mutation admission; late item
release and deactivation do not issue further device writes.

Directory enumeration uses the core's streaming visitor, validating and reading
each directory block once per call. A synchronous packing callback supplies names,
checked inode types and optional attributes. FSKit owns the cookie of the last
packed entry; a full packer stops the visit without retaining directory storage.
The volume monitor covers both traversal and packing. Dot entries, verifier
checks and inode-read errors keep their existing contracts.

## Private control protocol

Each new endpoint retains an exclusive advisory lock on its private manifest for
the server's lifetime. Discovery can remove an unlocked stale manifest and its
matching owned socket after a process crash or reboot, without relying on reused
PIDs. It never removes a live lease or substitutes ordinary files for sockets.
Dead endpoints from older builds are filtered after a definitive connection
failure; their files remain intact. Other connection failures still surface to
the caller. Standalone tests kill a separate server process and verify discovery
continues to return the other live volumes.

Both the app and extension declare `group.org.machlin.ext4`. The App Group must
be registered and included in both provisioning profiles. The transport resolves
the container through FileManager; there is no guessed container path, additional
extension point or filesystem data transport. This application control endpoint
is separate from the restricted device-barrier service.

Each mounted instance publishes a randomly named Unix stream socket and discovery
manifest at the App Group root. Short names accommodate the Unix socket path limit;
a path that still does not fit fails explicitly. The manifest identifies both the
volume UUID and a distinct instance UUID, so clones do not share an endpoint.
Disk Arbitration and ordinary macOS tools remain responsible for disk management.

Sockets and manifests use mode 0600. Both peers check the UID, and requests require
a fresh random 256-bit instance capability. Manifest reads reject symlinks,
other owners, unsafe permissions and oversized files. Provisioned App Group
access adds the sandbox boundary. The current contract requires app and extension
to run as the same user; root-mounted/user-app operation is unaccepted.

One connection carries one request and response: a four-byte unsigned big-endian
length, then a UTF-8 JSON dictionary. Frames are limited to 64 KiB and transport
operations have a two-second total deadline. Receiving a frame does not hold the
volume monitor. Requests contain `version: 1`, `instance`, `token`, `command` and
optional `arguments`. Responses contain the version and instance, plus either
`result` or `error` with a POSIX `code` and a `message`.

| Command | Effect |
| --- | --- |
| `ping` | Returns `pong` |
| `getInfo` | UUID, mount state, read-only state, geometry, free space and feature masks |
| `getCapabilities` | Available commands and explicit unsupported capabilities |
| `getSettings` | Current instance's adapter settings |
| `setSettings` | Exactly one boolean `retainReadState`; disabling drops retained mapping metadata |
| `dropReadState` | Drops held core read metadata without changing file data or native mappings |

The app performs RPC off its UI thread. Settings last for the current mounted
instance. Disconnecting the app does not affect mounted I/O. Unmount/invalidation
removes the endpoint. An already-received command still checks active volume state.
After an extension crash, stale manifests may remain; connection failure is
reported and a manifest alone is never evidence of a live mount.

This is a control plane, not a raw disk editor. Unsupported commands return
`ENOTSUP`. Recovery, live key changes and online feature changes are not exposed over IPC.
Each future mutation requires an owning core operation, caller authorization,
cache/lifetime rules and acceptance tests. Keys and capabilities must not enter logs.

## Command-line control

For initial installation, enable the module in System Settings → General → Login
Items & Extensions → **By Category → File System Extensions**. In the 26.5.2 test
VM, the generic By App “FSKit Modules” switch did not enable FSKit admission.
Check the result through `--control modules` before mounting; a PlugInKit `+`
marker alone does not establish that FSKit enabled the module.

The installed app executable also accepts `--control`, without opening a window.
This uses the same signed sandbox, App Group, Keychain and RPC implementation as
the GUI; an unsigned external socket client is not a substitute for this check.
Each command writes JSON and exits nonzero on failure. `modules` queries FSKit's
actual enabled state, independently of PlugInKit's election marker.

```sh
app='/Applications/Machlin ext4.app/Contents/MacOS/Machlin ext4'
"$app" --control modules
"$app" --control device-service
"$app" --control device-service setup
"$app" --control list
"$app" --control request ENDPOINT ping
"$app" --control request ENDPOINT getCapabilities
"$app" --control request ENDPOINT setSettings '{"retainReadState":false}'
"$app" --control request ENDPOINT getSettings
"$app" --control request ENDPOINT setSettings '{"retainReadState":true}'
"$app" --control keys VOLUME_UUID
"$app" --control import-key VOLUME_UUID - < TEST_KEY_FILE
"$app" --control remove-key VOLUME_UUID KEY_IDENTIFIER
```

Use the endpoint name and volume UUID returned by `list`. Endpoint selection is
restricted to discovered manifests; output excludes their capability tokens.
For headless fixture tests, redirect a mode `0600` synthetic key file into stdin.
The importer requires exactly 64 binary bytes followed by EOF, bounds its input
buffer and wipes it after use. Key bytes never enter command arguments or JSON.
This avoids copying test keys into another process's protected app container.
Passing a file path remains supported if the app already has sandbox access;
arbitrary paths do not grant that access. The GUI retains its security-scoped file picker.
An optional final import argument supplies the fscrypt v1 descriptor. Key changes
still require a new mount; neither interface exports saved master keys.

Use immutable filenames for artifacts transferred through a running VM's shared
folder. Copy executables into the guest and compare their SHA-256 with the host
artifact before running them. The test VM has returned old bytes after a host
executable was replaced at the same shared path; an exit code without this check
does not prove execution of the new diagnostic.

The clean test VM runs the guest RPC agent as a user LaunchAgent with RunAtLoad
and KeepAlive. It uses `--run-rpc` only. This permits CLI installation and tests
in the logged-in session; it does not establish unattended startup before login.
Normal updates copy a uniquely named archive into the guest, verify its hash,
unmount task volumes, stop the old app and extension, install and verify both
signed bundles, and register them with LaunchServices and PlugInKit. Check both
bundle versions and `--control modules` after every update. System enablement
persisted across these CLI updates; no repeated Settings interaction was needed.

For service updates, use the setup utility's `--unregister` before replacing the
bundle and `--register` afterwards, with all ext4 volumes unmounted. Registering an
already enabled service is not an update: in native acceptance its status stayed
enabled while the authenticated health query failed after replacement. Normal
unregistration and registration restored health without another approval. The
setup utility also offers `--refresh` and an update button for this public
lifecycle after installation, and refuses removal while an ext4 volume is mounted.
Verify the signed app's `--control device-service` query, not only setup status.
Immediate re-registration awaits the asynchronous unregister completion. On
26.5.2, the documented completion boundary still returned transient `EPERM`,
matching the [ServiceManagement bug acknowledged by Apple DTS](https://developer.apple.com/forums/thread/783539).
Refresh retries only this error while the service remains unregistered, for a
maximum 3.75 seconds of backoff. It preserves approval/signature errors and never
uses private launchd operations. A failed update remains an error; setup status
alone is not evidence of authenticated service health.

After changing extension metadata, verify discovery again after registration has
settled. On 26.5.2, PlugInKit registered the new extension identity while the user's
existing `fskit_agent` stopped exposing the module through the public FSKit API.
In the dedicated VM, with no FSKit mounts or control endpoints, restarting only
that user's agent restored discovery and preserved enablement. `SIGTERM` did not
replace the process; verify an actual PID change and two successful public module
queries before resuming tests. Do not treat the registration command's exit status
or an immediately cached discovery result as installation acceptance.

## Encryption and key lifetime

The native provider uses public CommonCrypto AES and HMAC operations. AES-256-XTS
uses batched ECB operations with IEEE 1619 tweaks; CBC-CTS uses Linux's CS3 ordering,
including the final full-block swap. There is no private XTS SPI or copy of the
reference AES implementation in the driver. Derived keys retain their cipher
contexts across file reads. The core continues to own fscrypt context validation,
nonce construction, name padding, sparse data and its bounded derived-key cache.

Both fscrypt v1 and v2 derivation are implemented for 64-byte raw master keys.
The provider checks v2 identifiers using fscrypt's HKDF-SHA512 identifier context;
v1 descriptors are explicit identifiers supplied by the importer. Raw key storage
uses dedicated locked pages, rejects allocation/locking failures, and wipes those
pages before release. CommonCrypto releases its own cipher contexts. Temporary
Keychain/import copies have separate system-managed lifetimes; this is not a claim
that every system copy is locked or immediately erased.

The app and extension use the existing App Group as an explicit Keychain access
group, with the data-protection Keychain and device-local, nonsynchronizing items.
Records are scoped to the volume UUID and policy identifier. The extension never
requests authentication UI from a filesystem operation. Missing or inaccessible
keys leave encrypted contents unavailable; unrelated unencrypted files remain
readable. `getInfo` reports loaded key count and Keychain availability.

The app imports a raw 64-byte fscrypt key from an explicitly selected regular
file, lists saved identifiers and removes a selected saved key. A blank descriptor
selects v2; an explicit 16-digit hexadecimal descriptor selects v1. It does not accept
fscrypt password-protector files or derive a master key from a password. Keys never
pass through control JSON, discovery manifests or logs. Duplicate import does not
replace an existing key.

The key set is sealed before publishing a volume and retained through its last
item. Adding or removing a saved key affects the next mount. Removing a saved key
**does not revoke access on the current mount**. Unmount/remount is required with
the current adapter; live key replacement needs accepted name and data-cache invalidation.
Signed, same-user Keychain sharing remains a separate installation requirement.

## OS compatibility

Keep macOS 26.5 as the deployment target. A modern SDK can build one binary with
27-only calls guarded by runtime availability; compile guards additionally keep
those calls out when building against an older SDK. Conditional reclaim uses this
boundary. Separate sibling volume classes implement the incompatible legacy and
27 read/write reply signatures. Both call one serialized I/O engine. The 27
handler supplies fresh inode attributes and sequenced free space in a successful
write reply; it cannot publish a stale snapshot after a failed device refresh.
Namespace and other operations retain the compatible older protocols. Native
acceptance of this combination on macOS 27 remains required: the current host and
guest run 26.x. Compiling with SDK 27 does not establish its runtime behavior.
CI also builds against SDK 26.5, excluding the unavailable declarations.

Future 27-only context/cache handlers must delegate to the same volume engine,
not duplicate the filesystem algorithms. Caller UID/GID in `FSContext` is useful
for ownership decisions but does not provide a complete Linux credential/group
set. The old API cannot substitute extension credentials for caller credentials.
Native mutation policy, ACLs and live cache changes still require explicit designs
and installed acceptance on each supported OS version.

An installed 26.5.2 conformance test currently fails after writing a set-ID file:
the core removes the bits on disk, but live and reopened `fstat` still report
them. The tested mount is `nosuid`; that bounds privilege use but does not satisfy
the metadata contract. The failed test remains mandatory. The newer reply API is
a candidate solution on 27, not evidence that either OS's behavior is fixed.

## Completion requirements

| Area | Implemented boundary | Remaining work |
| --- | --- | --- |
| Resource reads | Exact aligned and unaligned reads | Mounted resource failure and removal |
| File reads | Held state and restricted kernel mapping; mounted read/mmap/EOF checks on 26.5.2 | Native cache/reclaim stress, resource failures and removal |
| User xattrs | Native read/list/set/remove roundtrip; macOS names omit the Linux user namespace prefix | Linux ACL/security/trusted namespaces stay hidden |
| IPC and GUI | Signed same-user App Group RPC, live settings and normal unmount cleanup on 26.5.2 | Root-mounted/user-app coordination, crash recovery and GUI workflow acceptance |
| Writes | Approved authenticated device service, native 1/4 KiB writes, shared mmap, concurrent writers, remount and independent fsck | Live set-ID attribute coherence, native ENOSPC acceptance, cache stress and device failures |
| Crypto and ACLs | CommonCrypto fscrypt v1/v2 reads and writes; native key import/removal and remounts on 26.5.2 | Verity trust and ACL authorization; ACL-bearing items currently fail with ENOTSUP |
| Maintenance | Native Disk Arbitration recovery of interrupted transactions; read-only dirty media remain unchanged; component crash cuts | Full check/repair tooling |

The public SDK documents direct writes
and a metadata buffer-cache flush, but does not establish that a barrier persists
all preceding writes through device volatile caches. It also does not expose the
backing descriptor for a device-cache ioctl. The core's flush callback requires
that guarantee between journal commit phases. Callback completion or a cache flush
cannot be substituted without confirming the contract. The bundled device-barrier
service implements a separate public-API path. Signed native tests confirm that
the cache synchronization ioctl succeeds while FSKit owns the writable resource;
real hardware power-loss qualification remains separate.
The published Apple HFS extension is only a probe: its load method returns ENOTSUP,
and its descriptor access imports a private FSKit header. It does not establish a
public device-barrier path for third-party journaled writers.

Items with a Linux access ACL currently return ENOTSUP at item admission,
rather than being authorized by mode bits alone. This is safe rejection, not ACL
support. User xattr names longer than macOS supports are not exported.

## Focused checks

The separate FSKit GitHub Actions workflow builds an unsigned universal app with
the macOS 26.5 SDK on a macOS 26 runner. It runs the sanitizer-instrumented resource,
control, volume, crypto and fake Keychain checks, then repeats volume checks on
1 KiB and indexed-directory fixtures. It uploads platform, build and test logs.
This job needs no signing secrets and does not install an extension or establish
mounted behavior. The portable core retains its separate Linux CI suites.

For automatic signing, Xcode must have the personal Apple Developer account in
Settings > Apple Accounts, and Keychain must contain a usable signing identity.
A local certificate alone does not let Xcode fetch provisioning profiles. Both
targets set `REGISTER_APP_GROUPS=YES` so Xcode provisions their shared group.
From the absolute Machlin lab directory, build with the personal team identifier:

```sh
python3 ../ext4/scripts/build_fskit.py --team YOUR_TEAM_ID --provision \
  --derived-data ../ext4/artifacts/checks/fskit-signed/DerivedData
```

This permits Xcode to manage the app identifiers and provisioning profiles. It
does not install or launch the app. Verify the app and embedded extension signatures
and their profile entitlements before transferring them to the dedicated test VM.
Development profiles must also include the test Mac's provisioning identifier;
host build success does not establish permission to run in a different VM.
For an Apple silicon Mac, use its Provisioning UDID, not its Hardware UUID.
Read it again after a clean restore: the macOS 26.5.2 test VM retained its Tart
configuration and platform UUID but acquired a different Provisioning UDID.
Compare the actual identifier against both embedded profiles before installation;
preserving the virtual machine identifier alone does not preserve that permission.
If automatic profiles omit the registered VM, create a Mac App Development
profile for each bundle ID including the intended test devices, then let Xcode
download and select the two profiles explicitly:

```sh
python3 ../ext4/scripts/build_fskit.py --team YOUR_TEAM_ID --provision --clean \
  --app-profile 'Machlin ext4 development devices' \
  --extension-profile 'Machlin ext4 filesystem development devices' \
  --derived-data ../ext4/artifacts/checks/fskit-signed/DerivedData
```

Both profile arguments are required together. They bind only their respective
targets; the portable core remains independent of signing profiles.
The build defaults to Debug. Use `--configuration Release` for optimized native
acceptance and performance measurements. `--build-number N` assigns the same
positive bundle build number to the app and its extension; increase it when
installing a replacement build so the system can distinguish the versions.
Configuration does not change the signing identity or profile type: a Release
build with development profiles is still a development artifact.
Signed builds finish by verifying the complete bundle, including nested code,
strictly for all architectures. When switching profiles, use `--clean`: Xcode's
incremental build has replaced an extension's embedded profile without rerunning
its signing step, reporting build success with an invalid resource seal. Ordinary
unchanged-profile builds remain incremental.

From the absolute Machlin lab directory:

```sh
python3 ../ext4/scripts/build_fskit.py \
  --derived-data ../ext4/artifacts/checks/fskit-native-control/DerivedData
python3 ../ext4/scripts/test_fskit.py \
  --derived-data ../ext4/artifacts/checks/fskit-native-control/DerivedData \
  --image ../ext4/artifacts/fixtures/ext4-4k.img \
  --output ../ext4/artifacts/checks/fskit-native-control/tests
../ext4/artifacts/checks/fskit-native-control/tests/fskit-volume \
  ../ext4/artifacts/fixtures/ext4-1k.img
```

To include the Linux-verified encrypted fixtures, pass both optional arguments
to `test_fskit.py`:

```sh
  --encrypted-image ../ext4/artifacts/checks/encrypt-nokey-1/encrypted-ext4-4k.img \
  --encrypted-manifest ../ext4/artifacts/checks/encrypt-nokey-1/encrypted-ext4-4k.manifest
```

The resulting `fskit-volume` executable also accepts the encrypted image and its
manifest after the ordinary fixture path. Repeat that invocation with the 1 KiB
fixtures to cover the second block size. Crypto-provider and fake Keychain tests
run even when encrypted image arguments are omitted; the latter replaces Security
storage functions and does not modify the host Keychain.

The harness instruments adapter code with ASan/UBSan and links the core library
from the unsigned build. It tests transfers, protocol failures and a separate IPC
client process without registering an extension or mounting a volume. Core sanitizer
acceptance remains in the portable matrix.

The first signed VM run must prove app-to-mounted-
extension `ping → pong`, settings, unmount cleanup and restart before advanced
commands are accepted. Report actual results per OS version separately from builds
and standalone tests. No host extension installation or system modification is
part of the unsigned checks.

For installed encryption acceptance, import the fixture's test key through the
control app, unmount, and mount again so the extension loads its immutable key set.
Run `ext4-mounted-manifest-test MOUNTPOINT MANIFEST` against the corresponding
Linux-verified encrypted fixture manifest. This ordinary POSIX reader checks every
file's size, SHA-256 and EOF, plus symlink targets and object types, without calling
the core or FSKit callbacks directly. Its digest implementation is the existing
independent Linux guest verifier. Repeat with 1 KiB and 4 KiB images, then remove
the key and prove a new mount denies encrypted reads. A component test or a
successful Keychain import does not establish this mounted behavior.

The repeatable encrypted acceptance runner executes every command through a
specified Tart VM. It never installs or mounts on the host. Stage the two encrypted
images and manifests, `ext4-mounted-manifest-test`, and the synthetic
`fscrypt-fixture-v2.key` in the shared fixture directory. Keep the key mode `0600`.
Generate exports with the current `ext4-encrypt-test --write` and verify them with
`tests/run_linux_encrypt.py --verify-core`. Older exports used mode 0640 for
directories and cannot test ordinary-user traversal; do not repair an old image
in place or interpret that permissions failure as proof of key enforcement.
From the absolute lab directory, with the signed app already installed and enabled:

```sh
python3 ../ext4/scripts/test_fskit_installed_encryption.py \
  --tart scripts/tart.sh --vm ext4-fskit-stock-clean \
  --fixtures artifacts/ext4-fskit/acceptance/searchable \
  --guest-share '/Volumes/My Shared Files/lxnu-artifacts/ext4-fskit/acceptance/searchable' \
  --guest-workdir /Users/admin/ext4-fskit-acceptance/encrypted-RUN \
  --build-number BUILD \
  --output artifacts/ext4-fskit/installed-clean/encrypted-RUN
```

Choose fresh work and report directories and the actual installed build number.
The runner requires no active control endpoints, verifies transferred hashes,
read-only attachments and Disk Arbitration mount/ownership state, and tests both
block sizes. Each new mount detaches and reattaches its own image through `hdiutil`.
It checks missing-key denial,
signed import, immutable current-mount keys, digest/EOF/symlink readback, removal,
remount denial and endpoint cleanup. It removes only the fixture keys and its own
mounts, then verifies unchanged image bytes; failed steps and cleanup errors retain
a failing status with command logs.

## Platform references

- [App Groups entitlement](https://developer.apple.com/documentation/BundleResources/Entitlements/com.apple.security.application-groups): IPC and container requirements.
- [Provisioned macOS App Groups](https://developer.apple.com/documentation/xcode/accessing-app-group-containers): profile authorization and automatic group registration.
- [Registering test devices](https://developer.apple.com/help/account/devices/register-a-single-device/): the Provisioning UDID requirement for Apple silicon Macs.
- [Block device resources](https://developer.apple.com/documentation/fskit/fsblockdeviceresource): direct versus cached I/O.
- [Disk Arbitration FSKit naming](https://github.com/apple-oss-distributions/DiskArbitration/blob/main/diskarbitrationd/DASupport.m): `DSFSKitGetBundleNameWithoutSuffix` splits at the first underscore.
- [Apple HFS extension source](https://github.com/apple-oss-distributions/hfs/blob/main/hfs_appex/HFSFileSystem.m): private descriptor access and unsupported volume loading.
- [Metadata flush](https://developer.apple.com/documentation/fskit/fsblockdeviceresource/metadataflush()): buffer-cache contract.
- [Kernel I/O](https://developer.apple.com/documentation/fskit/fsvolume/kerneloffloadediooperations): mapping lifetime and inhibition.
- [Keychain sharing](https://developer.apple.com/documentation/security/sharing-access-to-keychain-items-among-a-collection-of-apps): App Group access groups and data-protection Keychain.
- [Conditional reclaim](https://developer.apple.com/documentation/fskit/fsitem/tryreclaim(_:)): synchronization with lookup.


## Current standalone evidence

On macOS 26.6.2, the application and extension compile for arm64 and x86_64,
both unsigned and with Apple Development signing. Strict signature verification
passes for both architectures of the app and its embedded extension. The embedded
profiles authorize the shared App Group, and the extension profile also authorizes
FSKit Module. Signing reports are under `artifacts/checks/fskit-signed/`; these are
development builds, not notarized distribution artifacts.

An earlier signed installation in the macOS 26.4 VM with Apple's stock kernel passed extension
discovery, system enablement and an ordinary-user mount. It failed the read-only
mount-flag and write-open assertions, and the control app found no mounted endpoint.
The base VM had SIP disabled and Gatekeeper assessments enabled, so this run does
not establish behavior with ordinary macOS security settings.
Normal unmount and device detach succeeded. The adapter now rejects write opens
explicitly, with focused open/close tests passing; installed verification on the
new minimum is recorded separately below. A deployment-target change alone does
not establish that runtime failures are fixed. These earlier reports are in the
lab's `artifacts/ext4-fskit/installed/`.

The focused adapter tests exercise direct versus bounced resource I/O,
short/failing reads, two distinct instances with one volume UUID, a separate IPC
client process, malformed/oversized/truncated requests, wrong capabilities and
versions, unsafe manifests, timeout and cleanup. The volume tests pass on 1 KiB
and 4 KiB fixtures: hard-link identity, held reads, sparse/EOF contents, paginated
kernel mapping reconstruction against independent fixture bytes, user xattrs,
ACL rejection, concurrent reads/control requests, late lifecycle rejection and
resource retention through the final item.

The streamed enumeration tests cover 400-file directories with and without
attributes, page capacities of 1, 7 and 512, both a packer that stops after accepting
its last entry and one that rejects the next entry, dot-entry policy, stale
verifiers and failures before or during delivery. They pass with ASan/UBSan on
1 KiB and 4 KiB fixtures. A dangling entry with a valid inode checksum but no live
inode mode reports `EIO`; an inode lookup failure cannot become successful EOF.
Compared with the prior adapter under the same tests,
one complete attribute-bearing scan reduces resource reads from 1,205 to 803
on the 4 KiB fixture and from 1,624 to 820 on the 1 KiB fixture. These are resource
callback counts, not mounted throughput. Evidence is in
`artifacts/checks/fskit-directory-stream/`.

The portable `file-read-ranges` and `held-file-reads` checks pass (2/2, no skips),
including guards that prevent crypto/verity/inline data from being offloaded.
Logs are under `artifacts/checks/fskit-native-control/`. These results do not
establish installed mounts, kernel cache/reclaim behavior, signed sandbox access,
durable native writes or the Linux performance target.

The native crypto and key-storage checks pass under ASan/UBSan. They compare v1/v2
outputs to the separately validated reference ciphers over aligned, partial,
unaligned and maximum-size inputs, including a Linux-derived v2 identifier.
The volume tests also read 36 entries from each of the previously Linux-verified
1 KiB and 4 KiB encrypted trees, checking all manifest digests and symlink targets;
a new mount without the keys refuses encrypted contents. The key-store test
replaces the three Security storage entry points, checks exact query scope and
failure behavior, and never accesses the host Keychain. Real signed Keychain access
is not established by this test. Logs are in `artifacts/checks/fskit-crypto/`.

## Installed evidence on macOS 26.5.2

The restored test VM loads Apple's stock kernel with SIP and authenticated-root
enabled. RPC, signed app updates and test execution now work through the CLI in
the logged-in guest session. The installed signed app confirms actual FSKit
enablement and passes same-user App Group IPC: ping, capabilities, settings and
read-state invalidation. Normal unmount removes the endpoint. Key import through
stdin succeeds without opening the GUI; a new extension instance loads the saved
Keychain key while an existing mount keeps its original key set.

Disk Arbitration automount with ownership enabled passes all six mounted groups
on both 1 KiB and 4 KiB fixtures: read-only flags, inode metadata, UID/GID
preservation, signed/extended timestamps, file/directory reads and read-only
admission. The owner is the fixture's 70001:80002, not the mounting user's identity.
Read coverage includes sparse data, links, private mmap/EOF and concurrent
open/read/close. Ownership and timestamp assertions run independently, so either
failure remains visible. Normal detach removes endpoints and leaves both image
hashes unchanged. Reports are in `build10-hdi-owners/` and
`build10-hdi-owners-4k/` under the lab's `artifacts/ext4-fskit/installed-clean/`.

The corrected encrypted exports pass independent Linux readback, no-key name
comparison and `e2fsck`, then the native signed lifecycle runner passes both
block sizes: all 36 manifest entries per image, including digests, EOF and symlink
targets. The ordinary-user runner first proves plain-file access and encrypted
inode lookup. With no saved key the encrypted read fails; import leaves that
mount unchanged, and a fresh mount loads one key and reads all entries. Removing
the saved key leaves the existing mount readable; the next mount has no loaded
keys and denies the encrypted read. Final unmount removes each endpoint, and the
test deletes its synthetic Keychain records and detaches its read-only images.
The same lifecycle passes through Disk Arbitration automount after the probe/name
fixes, with `-owners off` explicitly verified for these encryption fixtures and
unchanged image hashes after final detach.
Evidence is in `build10-encryption-da/` under that installed report tree;
the independent Linux reports are in the lab's
`artifacts/ext4-encrypt/fskit-searchable-{1k,4k}/`. These checks establish fscrypt
v2 on this OS and development-signed installation, not native writes, v1 mounted
acceptance, distribution signing or the Linux throughput target.

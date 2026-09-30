# FSKit adapter and control app

FSKit is the current native integration priority, before LXNU. The deployment
target is macOS 26.5. Unsigned builds and standalone adapter tests do not
establish installed FSKit behavior.

## Ownership and I/O

The filesystem object owns resource loading and maintenance requests.
`Ext4ResourceIO` retains the block resource and supplies exact, bounded direct
reads. Aligned requests use the caller's buffer; unaligned requests use a checked
bounce buffer. Short transfers are errors. The adapter does not mix direct and
metadata-cache I/O on overlapping ranges.

Each volume serializes core calls, item publication and control commands through
one recursive monitor. Items retain a core inode hold and the volume; the weak
identity index introduces no cycle. The last item keeps callback storage alive
through release and unmount. On macOS 27, conditional reclaim uses the same monitor
as lookup publication. On older systems, a hold survives until FSKit releases its
last strong item reference. Both paths need installed concurrency acceptance.

The open/close protocol rejects write and read/write opens with `EROFS` before
the kernel can admit cached writes or shared writable mappings. Item lifetime
continues to own inode holds independently of the open count. This complements
the requested read-only mount flag; acceptance checks both contracts separately.

Regular, block-aligned files with ordinary extent or indirect data can supply
validated mappings to FSKit's kernel I/O path. Inline, encrypted and verity files
retain core reads. Partial EOF blocks also retain core reads to avoid exposing
stale on-disk padding. These mappings require an immutable read-only block
resource. Writable mapping lifetime and cache invalidation are separate work.

Directory enumeration uses the core's streaming visitor, validating and reading
each directory block once per call. A synchronous packing callback supplies names,
checked inode types and optional attributes. FSKit owns the cookie of the last
packed entry; a full packer stops the visit without retaining directory storage.
The volume monitor covers both traversal and packing. Dot entries, verifier
checks and inode-read errors keep their existing contracts.

## Private control protocol

Both the app and extension declare `group.org.machlin.ext4`. The App Group must
be registered and included in both provisioning profiles. The transport resolves
the container through FileManager; there is no guessed container path, additional
extension point, privileged broker or filesystem data transport.

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
boundary. CI passes the unsigned universal build and focused adapter tests with
the macOS 26.5 SDK; installed runtime acceptance is separate.

Future 27-only context/cache handlers must delegate to the same volume engine,
not duplicate the filesystem algorithms. Caller UID/GID in `FSContext` is useful
for ownership decisions but does not provide a complete Linux credential/group
set. The old API cannot substitute extension credentials for caller credentials.
Native mutation policy, ACLs and live cache changes still require explicit designs
and installed acceptance on each supported OS version.

## Completion requirements

| Area | Implemented boundary | Remaining work |
| --- | --- | --- |
| Resource reads | Exact aligned and unaligned reads | Mounted resource failure and removal |
| File reads | Held state and restricted kernel mapping | Installed I/O, mmap, EOF, cache and concurrent reclaim |
| User xattrs | Read/list Linux user namespace; macOS names omit the namespace prefix | Native roundtrip and writable policy; Linux ACL/security/trusted namespaces stay hidden |
| IPC and GUI | Entitlements, discovery, bounded authenticated RPC and live settings | Signed sandbox POC, same-user identity, unmount and extension restart on supported OS versions |
| Writes | Portable core has mutations and durability | Device persistence contract, native mutation/authorization/cache integration |
| Crypto and ACLs | CommonCrypto fscrypt provider, immutable mount key set, app Keychain import/removal; protected files cannot bypass core reads | Signed Keychain sharing, mounted encrypted I/O, verity trust and ACL authorization; ACL-bearing items currently fail with ENOTSUP |
| Maintenance | Clean read-only quick check | Native recovery flow and full check/repair tooling |

**Block-device mounts remain read-only.** The public SDK documents direct writes
and a metadata buffer-cache flush, but does not establish that a barrier persists
all preceding writes through device volatile caches. It also does not expose the
backing descriptor for a device-cache ioctl. The core's flush callback requires
that guarantee between journal commit phases. Callback completion or a cache flush
cannot be substituted without confirming the contract. This is an unresolved
integration contract, not proof that durable FSKit writes are impossible.
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

## Platform references

- [App Groups entitlement](https://developer.apple.com/documentation/BundleResources/Entitlements/com.apple.security.application-groups): IPC and container requirements.
- [Provisioned macOS App Groups](https://developer.apple.com/documentation/xcode/accessing-app-group-containers): profile authorization and automatic group registration.
- [Registering test devices](https://developer.apple.com/help/account/devices/register-a-single-device/): the Provisioning UDID requirement for Apple silicon Macs.
- [Block device resources](https://developer.apple.com/documentation/fskit/fsblockdeviceresource): direct versus cached I/O.
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
new minimum of macOS 26.5 remains pending. A deployment-target change alone does
not establish that those runtime failures are fixed. These reports are in the
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

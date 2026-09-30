# FSKit adapter and control app

FSKit is the current native integration priority, before LXNU. The deployment
target remains macOS 26.4. Unsigned builds and standalone adapter tests do not
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

Regular, block-aligned files with ordinary extent or indirect data can supply
validated mappings to FSKit's kernel I/O path. Inline, encrypted and verity files
retain core reads. Partial EOF blocks also retain core reads to avoid exposing
stale on-disk padding. These mappings require an immutable read-only block
resource. Writable mapping lifetime and cache invalidation are separate work.

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
`ENOTSUP`. Recovery, key installation and online feature changes are not exposed.
Each future mutation requires an owning core operation, caller authorization,
cache/lifetime rules and acceptance tests. Keys and capabilities must not enter logs.

## Completion requirements

| Area | Implemented boundary | Remaining work |
| --- | --- | --- |
| Resource reads | Exact aligned and unaligned reads | Mounted resource failure and removal |
| File reads | Held state and restricted kernel mapping | Installed I/O, mmap, EOF, cache and concurrent reclaim |
| User xattrs | Read/list Linux user namespace; macOS names omit the namespace prefix | Native roundtrip and writable policy; Linux ACL/security/trusted namespaces stay hidden |
| IPC and GUI | Entitlements, discovery, bounded authenticated RPC and live settings | Signed sandbox POC, same-user identity, unmount and extension restart on supported OS versions |
| Writes | Portable core has mutations and durability | Device persistence contract, native mutation/authorization/cache integration |
| Crypto and ACLs | Protected files cannot use unverified mappings | Native keys, key lifetime and ACL authorization; ACL-bearing items currently fail with ENOTSUP |
| Maintenance | Clean read-only quick check | Native recovery flow and full check/repair tooling |

**Block-device mounts remain read-only.** The public SDK documents direct writes
and a metadata buffer-cache flush, but does not establish that a barrier persists
all preceding writes through device volatile caches. It also does not expose the
backing descriptor for a device-cache ioctl. The core's flush callback requires
that guarantee between journal commit phases. Callback completion or a cache flush
cannot be substituted without confirming the contract. This is an unresolved
integration contract, not proof that durable FSKit writes are impossible.

Items with a Linux access ACL currently return ENOTSUP at item admission,
rather than being authorized by mode bits alone. This is safe rejection, not ACL
support. User xattr names longer than macOS supports are not exported.

macOS 26.4 also predates caller-context and conditional-reclaim APIs. Writable
authorization must account for that difference; extension process credentials
are not the requesting application's credentials.

## Focused checks

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

The harness instruments adapter code with ASan/UBSan and links the core library
from the unsigned build. It tests transfers, protocol failures and a separate IPC
client process without registering an extension or mounting a volume. Core sanitizer
acceptance remains in the portable matrix.

Signing remains deferred. The first signed VM run must prove app-to-mounted-
extension `ping → pong`, settings, unmount cleanup and restart before advanced
commands are accepted. Report actual results per OS version separately from builds
and standalone tests. No host extension installation or system modification is
part of the unsigned checks.

## Platform references

- [App Groups entitlement](https://developer.apple.com/documentation/BundleResources/Entitlements/com.apple.security.application-groups): IPC and container requirements.
- [Block device resources](https://developer.apple.com/documentation/fskit/fsblockdeviceresource): direct versus cached I/O.
- [Metadata flush](https://developer.apple.com/documentation/fskit/fsblockdeviceresource/metadataflush()): buffer-cache contract.
- [Kernel I/O](https://developer.apple.com/documentation/fskit/fsvolume/kerneloffloadediooperations): mapping lifetime and inhibition.
- [Conditional reclaim](https://developer.apple.com/documentation/fskit/fsitem/tryreclaim(_:)): synchronization with lookup.


## Current standalone evidence

On macOS 26.6.2, the unsigned application and extension compile for arm64 and
x86_64. The focused adapter tests exercise direct versus bounced resource I/O,
short/failing reads, two distinct instances with one volume UUID, a separate IPC
client process, malformed/oversized/truncated requests, wrong capabilities and
versions, unsafe manifests, timeout and cleanup. The volume tests pass on 1 KiB
and 4 KiB fixtures: hard-link identity, held reads, sparse/EOF contents, paginated
kernel mapping reconstruction against independent fixture bytes, user xattrs,
ACL rejection, concurrent reads/control requests, late lifecycle rejection and
resource retention through the final item.

The portable `file-read-ranges` and `held-file-reads` checks pass (2/2, no skips),
including guards that prevent crypto/verity/inline data from being offloaded.
Logs are under `artifacts/checks/fskit-native-control/`. These results do not
establish installed mounts, kernel cache/reclaim behavior, signed sandbox access,
durable native writes or the Linux performance target.

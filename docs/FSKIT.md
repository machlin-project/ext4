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

The resource owner checks `FSResource.isRevoked` before device I/O and before
serving retained inode state. Revocation permanently fails that volume owner with
EIO, cancels its MMP heartbeat, rejects control commands and prevents orphan
release, final sync or MMP release through the unavailable device. Unloading a
revoked mounted resource may proceed without writing; stale retained items are
invalidated. Recovery and a later mount require a fresh owner. An unaligned
read-modify-write checks revocation again between its device read and write.
Component checks cover read-only cached reads, writable requests, orphan lifetime
and recovery, including no additional device calls after revocation.

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
The ext4 personality explicitly declares subtype zero, matching `FSStatFSResult`.
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
block size. Each write callback now uses `ext4_write_request`: it prepares all
data, allocation changes, inode attributes and quota updates before committing
the request. ENOSPC or EDQUOT therefore cannot leave an unreported request prefix.
This matters on both API generations: native FSKit did not reconcile partial
bytes returned with an error, and a successful short reply produced EIO instead.
The modern result/error ABI explicitly discards results on error.

Request preparation may use the journal ring's bounded capacity, up to 32 MiB of
private snapshots, while ordinary core operations keep their smaller transaction
budget. Oversized requests, including excessive written-preallocation gap zeroing,
return a range error before publishing data. This is an explicit limit, not an
automatic partial-write fallback. On the tested 27 system a 4 MiB user write reaches
the adapter as 1 MiB callbacks. Separate callbacks retain their separate commit
boundaries; the capacity test includes a large write that reaches ENOSPC after
earlier callbacks have succeeded. Device errors still poison the writable owner
and require recovery; they never become successful short replies.

The shared volume owns operations independently of FSKit's protocol generations.
`Ext4LegacyVolume` conforms to the 26.x operation protocols; `Ext4ModernVolume`
conforms to the 27 handler family. The factory chooses one family for the
lifetime of a mount. Read-only instances use `Ext4LegacyMappedVolume` or
`Ext4ModernMappedVolume`, which additionally conform to kernel-offloaded I/O.
Writable instances advertise only core I/O, so the framework never negotiates
raw block mappings for a mutable owner. Per-file inhibition still selects core
reads for ineligible files on a read-only volume. No class advertises both
incompatible I/O reply ABIs; mapping subclasses reuse the same engine and lifetime.
The modern boundary returns fresh item, parent and overwritten-item attributes
and available space within the same volume monitor as the operation. It also
checks fallible result construction. The shared callbacks are explicitly
synchronous; the bridge neither retains completion blocks nor duplicates disk
algorithms. Native 27.0.1 checks pass ordinary I/O, metadata, namespace, mmap,
concurrent writers and open-unlinked lifetime on 1 KiB and 4 KiB images, followed
by ordinary detach, remount and independent fsck. Live set-ID metadata remains
incorrect as detailed below. Native authorization and the existing ACL
rejection policy remain in force on both versions.

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

Installed failure checks on 26.5.2 and 27.0.1 stop or kill the authenticated service after
an ordinary-user checker establishes a durable baseline on each block size.
Writes and fsync return `EIO`; stopping the process exercises the ten-second
barrier deadline, while termination fails promptly. Restoring service health
does not revive the failed mounted owner: subsequent fsync and truncation still
fail. The runner force-detaches that disposable failed mount, then tests writable
recovery, a new durable write, read-only remount verification and ordinary detach.
All four recovered images on each OS pass independent fsck. These are service-failure checks,
not physical device removal or power-loss qualification.

`scripts/test_fskit_installed_barrier.py` uses the owned fixtures and a staged
`ext4-mounted-failure-test` executable, built by Meson on macOS. Its arguments
identify the Tart wrapper, dedicated VM, host/guest fixture paths, unique checker,
fresh work/evidence directories, installed build number and independent e2fsck.
Run it in a terminal for its no-echo guest sudo prompt. Only the signal helper runs
as root; file operations remain ordinary-user operations. Before each signal,
the helper validates the process UID and the kernel-reported executable path.
Passwords never enter arguments or evidence. Cleanup resumes a stopped service,
but a timed-out filesystem operation leaves its devices intact for diagnosis.

Installed resource lifetime checks on 26.5.2 and 27.0.1 pass the four combinations
of 1/4 KiB blocks and read-only/read-write mounts. An ordinary-user checker holds
both a descriptor and a shared read mapping while the controller force-detaches
only its disposable image. Subsequent uncached reads and writable-file mutations
return EIO; accessing the unavailable mapping page in a child produces SIGBUS.
The descriptors close without error. A previously clean descriptor's final fsync
may succeed because it has no pending writes; this is not acknowledgement of a
new write. Read-only remount verifies every previously acknowledged byte, ordinary
detach succeeds, endpoints disappear and each exported image passes fsck. The
read-only images and source fixtures remain byte-identical.

`scripts/test_fskit_installed_removal.py` runs this matrix with
`ext4-mounted-removal-test`, built by Meson on macOS. Its arguments identify the
Tart wrapper, dedicated VM, host fixture/checker files, new absolute guest work
directory, installed build number, independent e2fsck and new host evidence
directory. Inputs are staged and hash-verified through the CLI transport. Forced
detach is the injected fault, never a cleanup fallback after a timeout. This
accepts forced-unmount lifetime and retained file/mapping behavior, separately
from component resource revocation. It does not establish physical device
removal without final sync, volatile-cache power-loss safety or all I/O races.

The original 64 MiB pressure workload passes on 26.5.2 with 4 KiB blocks,
including ENOSPC at 53,215,232 bytes, readback, space reuse, remount and fsck.
The earlier 1 KiB workload stalled. A repeat with an explicit 1,800-second
deadline stopped making progress after 22,020,096 bytes. Before any signal or
detach, the checker waited in `pwrite` through `lifs_vnop_write`, `cluster_write_ext`
and a kernel mutex sleep, while the owning extension was idle and control IPC
remained responsive. This establishes a native I/O stall, but not its cause.
Disassembly of the guest's actual loaded kernel identifies an outstanding
`cluster_write_direct` I/O-completion wait. The retained kernel identity matches
the pre-signal sample; a different host KDK is not used as exact symbol evidence.
After more than thirteen minutes without progress, the experiment was explicitly
canceled early. Ordinary detach could not finish; terminating the exact owning
extension and restarting the disposable guest released the image. Its exported
interrupted state passed read-only fsck; that diagnostic does not accept the
unfinished workload. Evidence is in the lab's
`artifacts/ext4-fskit/installed-clean/build24-large-pressure-1/`.
The runner's `--profiles` and `--pressure-timeout` permit focused, explicitly bounded
reproduction. Evidence records the chosen deadline and streams progress as it arrives.

Writable volumes no longer advertise kernel block mappings. The unchanged
64 MiB/1 KiB pressure workload now finishes on 26.5.2 without its former stall,
passes sync/readback/space reuse, detaches normally, and passes remount and
independent fsck. This accepts that reproduction, not all capacity edge cases.
Its evidence is in the lab's `installed-clean/build27-large-pressure-1` under
`artifacts/ext4-fskit/`.

`ext4-mounted-capacity-test MOUNTPOINT cached|uncached [tail|aligned|large]`
reuses a fresh copy of the owned full export. The short layouts release the final
128 KiB and issue 256 KiB writes, optionally aligning the start to a native page.
The large layout releases 2 MiB plus 128 KiB and issues 4 MiB writes, deliberately
crossing several native I/O requests. Each case checks reported bytes, live size,
readback, ENOSPC and synchronization. Cached writes may instead report a deferred
ENOSPC at synchronization. An additional `apfs` argument prepares a disposable
APFS control volume of at most 256 MiB before the same operation.

`scripts/test_fskit_installed_capacity.py` uses a new guest directory, an explicitly
selected signed build and a fresh copy per layout/cache combination. It detaches
normally and invokes `ext4-mounted-capacity-test MOUNTPOINT verify START EXPECTED_SIZE`
on a read-only remount. It compares live size with that fresh native view and
independent `debugfs` inode inspection, reads the entire stored tail and EOF,
checks the read-only image hash, and runs nonrepairing `e2fsck`. The JSON
`read_amount` is the last `pread` count; `readback` covers the entire checked range.
Timeouts preserve the owning process/device for diagnosis. Neither clean fsck
nor a live check over an empty range can accept hidden committed bytes.

Complete callback admission now fixes the original 256 KiB cases on both 26.5.2
and 27.0.1: aligned/tail, cached/uncached all return ENOSPC without changing the
file's size or data. Live, remounted and independently inspected sizes agree;
all exports pass fsck, and read-only mounts preserve every image byte.

The larger case still fails on both systems. Two complete 1 MiB callbacks commit
before a later callback refuses allocation without changing the file. Native
`pwrite` fails with ENOSPC without reporting the committed bytes; `stat` includes the 2 MiB prefix, but live
`pread` at its beginning returns EOF. An ordinary read-only remount reads all
2 MiB correctly, with matching independent inode size and clean fsck. Complete
callback admission cannot make a syscall spanning several callbacks atomic.
The failed large cases remain required; native capacity acceptance is incomplete.
Evidence is in `installed-clean/build31-capacity-1` and
`installed-27/build31-capacity-2` under the lab's `artifacts/ext4-fskit/`.

Earlier diagnostics returned an allocation-limited prefix as short success or
combined that response with explicit `FSKernelCacheCoherencyTypeNoCache` on 27.
They produced native EIO and failed live readback; those changes were removed.
A further NoCache diagnostic combined with complete callback admission also fails
both large cases, with cache-open invocation confirmed. It was removed; evidence
is in `installed-27/build32-capacity-large-1` and the standalone
`artifacts/checks/fskit-request-cache32` directory. The earlier evidence remains in
`installed-27/build28-capacity-1`,
`installed-27/build29-capacity-1` and the standalone
`artifacts/checks/fskit-cache29`. They do not establish a supported workaround.
`diskutil verifyVolume` rejects this FSKit volume as unrecognized and supplies
no filesystem-validation evidence. DEBUG builds trace callback sizes/errors
without file contents.

The runner's optional `--diagnose` flag passes an additional `diagnose` argument
to the native checker. It records the original write, sync, stat, readback and
close verdict before reopening the file and attempting a same-size `ftruncate`.
These subsequent operations affect only the disposable diagnostic copy and never
turn the original failure into a pass. JSON `write_amount` is the last native
`pwrite` return value; `reported` accumulates positive syscall returns.

On both tested OS versions and both cache modes, the failed syscall returns
`-1/ENOSPC`, while the core has stored a 2 MiB prefix. Closing every descriptor and
reopening does not make it readable. A successful `ftruncate` to the existing
size, followed by synchronization, makes the entire prefix readable without
changing the size. Remount, independent inode inspection and fsck agree. Evidence
is in the lab's `artifacts/ext4-fskit/installed-clean/build31-native-diagnosis-2`
and `installed-27/build31-native-diagnosis-2` directories.

Read-only static analysis of the exact loaded 27 `lifs` component identifies a
matching error path: `_lifs_vnop_write` saves the old logical file size before
`cluster_write`, then restores both logical and UBC size on an error. Earlier
successful module callbacks have already persisted their bytes. This explains
the observed disagreement between stored size, native stat and live read EOF;
the same-size truncate causes the native size state to be installed again.
The loaded identity, original collection, generated analysis copies and function
notes are recorded in `installed-27/build31-native-component-2`. No modified
kernel was installed or booted. The 26 component was not disassembled.

The ordinary write callback supplies only its own range, not the enclosing
syscall's length or transaction boundary. Complete admission of that callback
therefore cannot prevent the outer native rollback after a later callback fails.
No supported module workaround has been established; issuing synthetic truncates
or rolling back previously acknowledged requests is not an accepted solution.

The mutation engine handles file/directory/symlink creation, links, unlink/rmdir,
rename and replacement, complete write requests, sparse growth, truncate, owner/mode/time
changes and user xattrs. Namespace changes advance directory verifiers. Data
changes conservatively remove set-ID bits and Linux file capabilities in the same
transaction because 26.x callbacks lack caller credentials. Immutable/append flags
map to privileged Darwin system flags; `nodump` maps to the user flag.
Capability removal is an explicit conditional xattr mutation in that transaction.
The adapter prepares the policy without a preliminary getter or a cached claim
that the attribute is absent. Component comparisons on both block sizes reduce
resource reads for a small write from 12 to 9 without a capability, and from 20
to 14 with one. Truncate and owner/group changes fall from 8 to 5 and from 20 to
14 respectively; preallocation falls from 13 to 10 and from 28 to 22. The checks
also preserve unrelated attributes. These are exact in-memory resource-call counts,
not a native throughput result (`ext4/artifacts/checks/fskit-xattr24` in the workspace).
Extent preallocation supports physical-EOF and persistent requests. Allocation
or quota exhaustion may return a successful partial reservation. Device, journal
and format failures remain errors even if earlier allocation transactions committed;
an aborted owner cannot be used again. Contiguous or
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
Native `fsetattrlist` rename, exact-limit labels, oversized rejection and persistence
pass on both block sizes. `diskutil info` reports the new name after remount.
The separate `diskutil renameVolume` invocation still rejects the requested name
on stock 26.5.2 and 27.0.1; it is a remaining system-tool integration failure. A direct
public `DADiskRename` call succeeds for both short and exact-limit labels on the
4 KiB fixture, with the result independently checked after detach.
Focused 27 registration diagnostics confirm that the declared short name and
subtype match the public
[type-name](https://developer.apple.com/documentation/fskit/fsstatfsresult/filesystemtypename)
and [subtype](https://developer.apple.com/documentation/fskit/fsstatfsresult/filesystemsubtype)
contracts. `diskutil info -plist` nevertheless omits filesystem/personality names
for this mounted volume, while reporting its label, UUID, mount point and writable
state. A single fresh-image rename again fails before the module callback.
Comparison with Apple's public exfat/msdos module plists does not establish a
registration defect. PlugInKit's null version display does not mean the bundle
lacks version metadata: its short version and build version are populated.
`diskutil listFilesystems` lists personalities available for
formatting, as its man page specifies; absence there does not prove mount or rename
support is missing. No dummy formatter or alternative filesystem identity is used
to change that observation.
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
to run as the same user. On 27.0.1, mounting with `sudo hdiutil` in the administrator's
login session leaves the extension running as that administrator. The ordinary-user
app then passes control discovery and file operations on both block sizes, followed
by normal detach, read-only verification and independent fsck. The runner's
`--mount-as-root` elevates only image attachment and detachment; test I/O and control
commands retain the ordinary UID. This does not accept a root-owned extension or
cross-user IPC. Evidence is in the lab's
`artifacts/ext4-fskit/installed-27/build24-root-mount-1/`.

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
After an extension crash, discovery removes abandoned leased endpoints and filters
definitively dead legacy endpoints. A manifest alone is never evidence of a live mount.

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

Installed v1 and v2 acceptance passes 1 KiB and 4 KiB images on both 26.5.2 and
27.0.1. On 27, all four cases verify 36 fixture entries, native encrypted writes
and shared mmap, concurrent I/O, key import/removal with next-mount visibility,
read-only remount and independent fsck. The generated reports are
`lab/artifacts/ext4-fskit/installed-27/build24-encryption-{v1,v2}-1/summary.json`.

Actual GUI key import and removal also pass on 27.0.1 for both v1 and v2 using
the 4 KiB encrypted fixtures. The v1 descriptor is entered in the NSOpenPanel
accessory field; invalid hexadecimal input visibly fails without saving a key.
An empty descriptor imports v2. The GUI and CLI saved-key lists agree, while
native remount verifies key loading and the expected plaintext hash. GUI removal
preserves access in the current mount; the next mount has no loaded key and
rejects the same file with permission denied. Both image hashes remain unchanged,
staged synthetic key files are deleted, normal detach succeeds and endpoints are
empty. These checks use GUI actions for import/removal, not CLI substitutes.
Evidence is in `installed-27/build33-gui-keys` and `installed-27/build33-gui-v1`
under the lab's ignored FSKit artifacts. The equivalent GUI path also passes
on 26.5.2 with the optimized Release configuration and the
same v1/v2 fixtures, including invalid v1 input and next-mount key removal. Its
reports are `installed-clean/build34-gui-v1` and `build34-gui-v2`. Signed CLI key
lifecycle on both OS versions is recorded separately.

A bounded GUI check on 27.0.1 also passes Refresh, active-volume geometry and service
status, both values of the read-state retention setting and Release read metadata.
The signed CLI independently verifies the setting changes. The raw-key file picker
opens, but this run could not complete its selection dialog through the VM input
transport. It therefore did not accept GUI key import or removal; later actual
v1/v2 GUI checks on both OS versions are recorded below. The signed CLI key
lifecycle above is separate evidence. After canceling the dialog, normal detach
leaves no endpoint, Refresh shows no volume, and the read-only image is unchanged. Evidence is in the
lab's `artifacts/ext4-fskit/installed-27/build24-gui-1/`.

## OS compatibility

Keep macOS 26.5 as the deployment target. A modern SDK can build one binary with
27-only calls guarded by runtime availability; compile guards additionally keep
those calls out when building against an older SDK. Conditional reclaim uses this
boundary. Separate sibling volume classes implement the incompatible legacy and
27 reply signatures. Each class advertises one complete protocol family, while
both call the same serialized namespace, metadata and I/O engines. The 27 handler
supplies fresh item and parent attributes and sequenced free space after mutations;
it cannot publish a stale snapshot after a failed device refresh. Native 27.0.1
acceptance covers the ordinary mutation and persistence-service failure workloads
above, as well as the v1/v2 encryption lifecycle; broader cache/reclaim stress and
physical device loss remain
separate acceptance requirements.
CI also builds against SDK 26.5, excluding the unavailable declarations.

The 27 seek-region handler delegates to the portable mapping query. It skips
whole sparse runs, treats unwritten extents as holes, clips at logical EOF and
shares the held inode's bounded mapping cache with reads. It never exposes raw
addresses or reads file data. The installed seek check also requires buffered
writes and truncation to be visible; these checks pass both block sizes on 27.0.1.
There is no equivalent public seek handler in the 26.5 API.

Future 27-only context/cache handlers must delegate to the same volume engine,
not duplicate the filesystem algorithms. Caller UID/GID in `FSContext` is useful
for ownership decisions but does not provide a complete Linux credential/group
set. The old API cannot substitute extension credentials for caller credentials.
Native mutation policy, ACLs and live cache changes still require explicit designs
and installed acceptance on each supported OS version.

Installed 26.5.2 and 27.0.1 conformance tests fail after writing a set-ID file:
the core removes the bits on disk, but live and reopened `fstat` still report
them. The tested mount is `nosuid`; that bounds privilege use but does not satisfy
the metadata contract. The failed test remains mandatory. A focused DEBUG trace
on 27 confirms that a non-nil `FSWriteFileResult` returns mode `0740` without an
error, but live/reopened `fstat` retains `06740`. The framework's requested
attribute mask omits mode. This localizes the observed discrepancy to publication
through the native metadata cache; it does not establish a supported workaround.
An ordinary-user APFS control on the same 27 guest clears both bits: mode `06740`
becomes `0740` after a one-byte `pwrite` and `fsync`, including after reopen.
A second control on 26.5.2 also clears both bits with `nosuid` and ownership
checking enabled, matching the flags of the ext4 conformance mount; those flags
do not explain the discrepancy. Thus the expectation is independently reproduced
on macOS. The SDK documents
that result objects cache all populated attributes, including ones not requested;
the adapter already supplies a fresh mode and change time in that result.
There is no public metadata-invalidation operation in the selected SDK. Its
data-cache coherency protocol addresses file data, not an attribute refresh.
Apple's [FSKit cache discussion](https://developer.apple.com/forums/thread/832647)
also distinguishes data-cache management from change notification; that statement
alone does not diagnose this driver's set-ID discrepancy.
The matching 27 kernel analysis adds concrete evidence: I/O completion calls
`_update_lnode_attr_subset_locked`, which updates size, allocation, file ID and
access/modify/change times, but not `FSItemAttributeMode`. The general attribute
updater does handle mode. This restricted completion path is consistent with
the fresh mode in the handler result not replacing the native cached mode;
it is a diagnosis from the loaded binary and mounted traces, not an Apple-confirmed
defect or a supported workaround. The analysis is recorded in the lab's
`artifacts/ext4-fskit/installed-27/build31-native-component-2/native-write-analysis.json`.
The failed conformance check remains required. The APFS control evidence is in
the lab's `artifacts/ext4-fskit/installed-27/build30-setid-control-1/` and
`installed-clean/build31-apfs-nosuid-1/attempt4` directories.

An explicit matching `FSSubType` does not fix `diskutil renameVolume`. A stock 26
control rejects both `ext4` and the valid 16-byte label, while native rename,
read-only remount label verification and independent fsck pass. The failure is
not confined to the maximum label length. Evidence is in the lab's
`artifacts/ext4-fskit/installed-clean/build31-rename-short-1/`.

Static inspection of the test guest's `diskutil` localizes that exact refusal:
`DiskMount` first asks DiskManagement for the disk's filesystem description, then
validates the label through that description. A missing description and a rejected
label both reach the same error message before the actual rename request. Existing
filtered logs do not distinguish those branches. This proves the validation
boundary, not a missing naming utility or a fix through undocumented registration
keys. The selected binary slice was inspected without invoking private methods;
its runtime-selected UUID was not independently observed. Evidence is in the lab's
`artifacts/ext4-fskit/native-issues/rename-gate-investigation`.

Generated evidence lives in the lab's ignored `artifacts/ext4-fskit/` tree:
`installed-27/build22-native-1`, `installed-27/build23-attributes-1-install`,
`installed-27/build23-barrier-1` and `installed-clean/build21-large-pressure-1`.
Keep failed groups and interrupted-run cleanup distinct from passed groups.

## Completion requirements

| Area | Implemented boundary | Remaining work |
| --- | --- | --- |
| Resource reads | Exact aligned/unaligned reads; component revocation and native forced-detach matrix on 26.5.2/27.0.1 | Physical device loss without final sync and additional failure races |
| File reads | Held state and restricted kernel mapping; native read/mmap/EOF checks, retained descriptor/mapping forced detach and bounded mmap/rename lifetime stress on 26.5.2/27.0.1 | Memory-pressure reclamation and physical device loss |
| User xattrs | Native read/list/set/remove roundtrip; macOS names omit the Linux user namespace prefix | Linux ACL/security/trusted namespaces stay hidden |
| IPC and GUI | Signed same-user RPC and key lifecycle and actual GUI v1/v2 key import/removal on 26.5.2/27.0.1; sudo-mounted/admin-app operation and basic controls on 27.0.1; abandoned endpoint recovery on 26.5.2 | Cross-user or root-owned extension coordination |
| Writes | Approved authenticated device service, native 1/4 KiB writes, shared mmap, concurrent writers, bounded cached-write/truncate/rename stress, bounded ENOSPC, extension/service termination and timeout, forced-detach durability, recovery, remount and independent fsck | Live set-ID attribute coherence, larger pressure case, memory-pressure stress and physical device loss |
| Crypto and ACLs | CommonCrypto fscrypt v1/v2 reads and writes; native key import/removal and remounts on 26.5.2 and 27.0.1 | Verity trust and ACL authorization; ACL-bearing items currently fail with ENOTSUP |
| Maintenance | Native Disk Arbitration recovery of interrupted transactions; read-only dirty media remain unchanged; component crash cuts | Full check/repair tooling |
| Distribution | Universal Xcode Release archive/export, Developer ID signatures, hardened runtime, timestamps, matching app/extension profiles, notarization, Gatekeeper and installed native write/remount/policy/fsck on 26.5.2/27.0.1 | Settings-button GUI acceptance and broader OS/hardware acceptance; native failures above remain open |

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
For distribution, use Xcode's archive/export pipeline instead of replacing the
outer app's signature. `--archive-path` creates a fresh archive and defaults to
Release; all nested products are embedded in the main application and excluded
as independent archive products. The app, extension and device-service executables
enable hardened runtime. `--export-path` also requires a signing team and an
explicit `--export-options` plist. Existing archive or export destinations are
rejected so a failed or accepted artifact is retained.

An automatic Developer ID export uses the following `ExportOptions.plist`:

```xml
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>method</key><string>developer-id</string>
    <key>teamID</key><string>YOUR_TEAM_ID</string>
    <key>signingStyle</key><string>automatic</string>
    <key>manageAppVersionAndBuildNumber</key><false/>
</dict>
</plist>
```

From the absolute lab directory:

```sh
python3 ../ext4/scripts/build_fskit.py --team YOUR_TEAM_ID --provision \
  --configuration Release --build-number N \
  --derived-data ../ext4/artifacts/fskit-distribution/DerivedData \
  --archive-path ../ext4/artifacts/fskit-distribution/Machlin-ext4.xcarchive \
  --export-path ../ext4/artifacts/fskit-distribution/export \
  --export-options ../ext4/artifacts/fskit-distribution/ExportOptions.plist
```

Archiving uses the development identity; Xcode export selects the distribution
identity and profiles, including those for restricted extension entitlements.
`--provision` permits Xcode to fetch profiles through the configured personal
account. For manual export, supply the corresponding `signingCertificate` and
per-bundle `provisioningProfiles` in the export plist. Check the selected Xcode's
`xcodebuild -help` for its supported export options. Both the signed archive app
and exported app undergo strict nested signature verification. Distribution also
requires verified Developer ID authorities, secure timestamps, profile entitlement
agreement, notarization and a clean dedicated-VM installation. Export alone does
not establish these remaining gates or functional acceptance. See Apple's
[distribution signing workflow](https://developer.apple.com/documentation/xcode/creating-distribution-signed-code-for-the-mac/)
and [packaging guidance](https://developer.apple.com/documentation/xcode/packaging-mac-software-for-distribution).

The first direct-distribution export passes these signing checks for both
architectures: the app, filesystem extension, installer helper and barrier tool
all carry Developer ID Application signatures for the personal team, hardened
runtime and secure timestamps. The main app and extension embed all-device
direct-distribution profiles that authorize their application identifiers and
shared App Group; only the extension requests the FSKit module entitlement.
Neither requests debugging permission. Profile authorizations may contain
wildcards or broader grants than the signed code uses; compare authorization
coverage rather than requiring identical entitlement arrays. The requested and
Xcode-normalized export options are retained separately. This accepts export and
signing, not a native installation result. Evidence is in
`artifacts/checks/fskit-distribution` in the standalone repository.

Xcode Organizer's direct-distribution upload completes notarization through the
configured personal Apple Account. The successful notarized export passes
`stapler validate`, strict nested signature verification and Gatekeeper assessment.
All four executable targets retain both compiled Mach-O UUIDs from the Release
archive, and the final app/extension profiles authorize their signed entitlements.
No credential reset, new certificate, policy change or second submission is
needed. Organizer imported a registered copy of the source archive; attempting
`xcodebuild -exportNotarizedApp` on the original artifact fails because it lacks
the upload metadata. Both Organizer and CLI export from the registered Ready to
distribute archive succeed and pass the same signature, ticket and Gatekeeper
checks. Locate that archive with Organizer's Show in Finder, then export into a
fresh directory:

```sh
xcodebuild -exportNotarizedApp -archivePath /absolute/registered.xcarchive \
  -exportPath /absolute/new-export-directory
```

Retain the source archive, registered archive location and selected export
separately; an earlier failed CLI export is not a notarization rejection.
See Apple's [Organizer notarization workflow](https://help.apple.com/xcode/mac/current/en.lproj/dev88332a81e.html).

The dedicated 26.5.2 and 27.0.1 installations of that notarized app pass complete
bundle byte/symlink equality, strict signatures and Gatekeeper. The device-cache
service fails to launch after replacement of the development-signed build:
launchd reports a code-signing launch constraint violation, while the public
ServiceManagement status reports enabled. Public unregister/register,
normal app/helper launch and an exact-job kickstart on 26.5.2 have not established
an authenticated running service. On 27.0.1, unregistering the old service before
replacing the bundle and initially registering the new one fails at the same
constraint. Both guests retain the complete notarized app, but neither has a
running service or an enabled filesystem module after this migration. These are
failed installation gates, so no writable distribution test is admitted.
Do not replace the constraint, use an alternate launchd plist or weaken
signature/authorization checks to obtain a pass. Apple's
[service-upgrade discussion](https://developer.apple.com/forums/thread/795022)
describes the same error fields and a possible stale registration; that does not
prove the cause in either guest. Installation and bounded public lifecycle
evidence live in the lab's `installed-clean/build35-distribution-install` and
`installed-27/build35-distribution-install` trees.

A separate scoped cleanup on 27.0.1 preserves every old task-owned app copy in
a verified archive, removes the extracted copies and waits for the app's own
background entry to disappear. Reinstalling the identical notarized bundle into
an empty destination and approving its normal registration then starts the
device service. Its authenticated health, root UID and full installed executable
path are verified. No signing constraint, authorization policy, global database
reset, kickstart or guest reboot is used. The old and new binaries satisfy their
own designated requirements and reject each other's, but public diagnostics do
not expose the enforced spawn requirement; a retained requirement remains an
inference. This accepts that service recovery scenario only: the installed
filesystem module remains off after the By App toggle attempt, so this particular
run is not admitted to mounted distribution tests. Evidence is in
`installed-27/build35-distribution-install/own-copy-cleanup`.

A normal guest reboot preserves the healthy distribution service, but does not
repair the By App aggregate toggle. A fresh call to the documented macOS 27
`FSClient.openFileSystemExtensionsSettings()` API, with System Settings closed,
opens the dedicated File System Extensions sheet. Its ordinary Machlin ext4
toggle enables the installed module, which is independently confirmed through
`FSClient`. An earlier navigation comparison with a preexisting modal sheet is
inconclusive and remains separate. The normal reboot and fresh settings evidence
is in `installed-27/build35-distribution-install/normal-reboot`. The same notarized
app then passes ordinary native write, remount and permission checks plus
independent fsck on both block sizes; evidence is in
`installed-27/build35-distribution-native`. The production app/extension profiles
also pass signed App Group control, real Keychain v1/v2 import and removal,
next-mount key snapshots, encrypted read/write/remount and independent fsck on
both block sizes. Evidence is in `installed-27/build35-distribution-encrypted-v1`
and `installed-27/build35-distribution-encrypted-v2`. This does not close the
retained capacity, set-ID and diskutil naming failures.

The same scoped installation lifecycle on 26.5.2 also preserves and verifies old
app archives, observes disappearance of the own background entry and installs
the identical notarized bundle into an empty destination. Normal service approval
starts the authenticated root daemon. Opening **By Category → File System
Extensions → info** exposes the actual module toggle, which enables the installed
extension; the category's summary names alone are not its complete installed
module list. No reboot or signing/policy change is needed. Ordinary native write,
permission and remount checks pass both block sizes with independent fsck.
Evidence is in `installed-clean/build35-distribution-install/own-copy-cleanup`
and `installed-clean/build35-distribution-native`. The real production-profile
Keychain and control IPC v1/v2 lifecycle, encrypted write/remount and independent
fsck also pass both block sizes; see
`installed-clean/build35-distribution-encrypted-v1` and
`installed-clean/build35-distribution-encrypted-v2`. All these runs finish with
empty endpoints, detached task images and removed fixture keys.

The control app's **Open extension settings** button uses that documented
dedicated route when built with the macOS 27 SDK and run on macOS 27. Earlier
SDKs and systems open Login Items through ServiceManagement; choose **By Category
→ File System Extensions** and enable Machlin ext4 there. The newer call is
guarded at both compilation and runtime boundaries, retaining the 26.5 target.

The optimized Release configuration passes the ordinary native write and permission
checks on both supported test OS versions, with 1 KiB and 4 KiB blocks. It also
passes the four retained-descriptor/mapping removal cases on each OS, followed by
normal remount, independent fsck and empty endpoint/device checks. This validates
the optimized configuration for those workloads; the larger ENOSPC, set-ID cache
and diskutil rename failures remain unaccepted. Reports are under
`installed-{clean,27}/build34-{native,removal}` in the lab's ignored FSKit artifacts.
Signed builds finish by verifying the complete bundle, including nested code,
strictly for all architectures. When switching profiles, use `--clean`: Xcode's
incremental build has replaced an extension's embedded profile without rerunning
its signing step, reporting build success with an invalid resource seal. Ordinary
unchanged-profile builds remain incremental.

Install a complete new app bundle into an empty destination after all test mounts
and extension endpoints have closed. Preserve the previous bundle in a verified
archive for rollback; do not accumulate discoverable app bundles in test staging
directories. Verify the staged bundle before replacement and remove the extracted
staging copy after installation. Copying with `ditto` over the existing bundle
merges directories: switching Debug to Release left obsolete debug and preview
libraries in nested code, invalidating its signature. Check the installed bundle
with strict, deep, all-architecture verification, not just the archive. Verify
app, extension and helper versions and enabled module state after registration.
On the 26.5.2 guest, replacing the app required switching its FSKit module off
and on through System Settings; a discovery-agent refresh alone did not enable
it. With endpoints empty, restart only the registered device-barrier job, then
verify its new PID, root UID, full installed executable and authenticated health.
An old running helper is not evidence that the replacement helper can launch.

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

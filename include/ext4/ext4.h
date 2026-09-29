/* SPDX-License-Identifier: BSD-3-Clause */
#ifndef MACHLIN_EXT4_H
#define MACHLIN_EXT4_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define EXT4_ROOT_INODE 2U
#define EXT4_NAME_MAX 255U
#define EXT4_LINK_MAX 65000U
#define EXT4_UUID_SIZE 16U
#define EXT4_VOLUME_NAME_SIZE 16U
#define EXT4_INODE_BLOCK_BYTES 60U
#define EXT4_DEVICE_MAJOR_MAX 0xfffU
#define EXT4_DEVICE_MINOR_MAX 0xfffffU

enum ext4_result {
	EXT4_OK = 0,
	EXT4_INVALID_ARGUMENT,
	EXT4_NOT_EXT4,
	EXT4_UNSUPPORTED,
	EXT4_CORRUPT,
	EXT4_IO,
	EXT4_NO_MEMORY,
	EXT4_NOT_FOUND,
	EXT4_NOT_DIRECTORY,
	EXT4_NAME_TOO_LONG,
	EXT4_READ_ONLY,
	EXT4_RECOVERY_REQUIRED,
	EXT4_IS_DIRECTORY,
	EXT4_RANGE,
	EXT4_STALE,
	EXT4_NO_SPACE,
	EXT4_EXISTS,
	EXT4_TOO_MANY_LINKS,
	EXT4_NOT_EMPTY,
	EXT4_PERMISSION_DENIED,
	EXT4_BUSY,
	EXT4_ENCRYPTED,
	/* A link or rename would place an object in a project-inheriting directory
	 * of another project; Linux reports EXDEV. */
	EXT4_CROSS_PROJECT,
	/* An enforced quota limit refused the change; Linux reports EDQUOT. */
	EXT4_QUOTA_EXCEEDED
};

enum ext4_file_type {
	EXT4_FT_UNKNOWN = 0,
	EXT4_FT_REGULAR = 1,
	EXT4_FT_DIRECTORY = 2,
	EXT4_FT_CHARACTER = 3,
	EXT4_FT_BLOCK = 4,
	EXT4_FT_FIFO = 5,
	EXT4_FT_SOCKET = 6,
	EXT4_FT_SYMLINK = 7
};

enum ext4_mode {
	EXT4_MODE_TYPE = 0170000,
	EXT4_MODE_REGULAR = 0100000,
	EXT4_MODE_DIRECTORY = 0040000,
	EXT4_MODE_SYMLINK = 0120000,
	EXT4_MODE_CHARACTER = 0020000,
	EXT4_MODE_BLOCK = 0060000,
	EXT4_MODE_FIFO = 0010000,
	EXT4_MODE_SOCKET = 0140000
};

/* Persistent ext4 policy bits. Layout/mapping bits in inode.flags are retained
 * by the core and cannot be changed through ext4_set_inode_flags. */
enum ext4_inode_flag {
	EXT4_INODE_SYNC = 0x00000008U,
	EXT4_INODE_IMMUTABLE = 0x00000010U,
	EXT4_INODE_APPEND = 0x00000020U,
	EXT4_INODE_NODUMP = 0x00000040U,
	EXT4_INODE_NOATIME = 0x00000080U,
	EXT4_INODE_JOURNAL_DATA = 0x00004000U,
	EXT4_INODE_NOTAIL = 0x00008000U,
	EXT4_INODE_DIRSYNC = 0x00010000U,
	EXT4_INODE_TOPDIR = 0x00020000U,
	/* New objects take the directory's project ID. */
	EXT4_INODE_PROJINHERIT = 0x20000000U,
	/* Names in the directory compare and hash through the volume's encoding. */
	EXT4_INODE_CASEFOLD = 0x40000000U
};

#define EXT4_INODE_MODIFIABLE_FLAGS                                                                \
	((uint32_t)(EXT4_INODE_SYNC | EXT4_INODE_IMMUTABLE | EXT4_INODE_APPEND |                   \
	    EXT4_INODE_NODUMP | EXT4_INODE_NOATIME | EXT4_INODE_JOURNAL_DATA | EXT4_INODE_NOTAIL | \
	    EXT4_INODE_DIRSYNC | EXT4_INODE_TOPDIR | EXT4_INODE_PROJINHERIT))

struct ext4_fs;
struct ext4_inode_hold;
struct ext4_xattr_change;

/* read must complete exactly length bytes or return an error. The resource and
 * callbacks remain valid until unmount. All offsets are resource-relative. */
struct ext4_environment {
	void *context;
	uint64_t size_bytes;
	enum ext4_result (*read)(void *context, uint64_t offset, void *buffer, size_t length);
	void *(*allocate)(void *context, size_t size);
	void (*release)(void *context, void *allocation, size_t size);
};

/* Multi-mount protection services supplied by a writable owner of an MMP volume.
 * sleep waits at least the requested seconds, or fails when interrupted. random
 * returns unpredictable values. now returns seconds since the Unix epoch. The
 * names identify this host and device in the MMP block and are truncated to its
 * fields. Environment reads of the MMP block must observe other hosts' writes:
 * the owner may not satisfy them from a cache. */
struct ext4_mmp_environment {
	void *context;
	enum ext4_result (*sleep)(void *context, uint32_t seconds);
	uint32_t (*random)(void *context);
	int64_t (*now)(void *context);
	const char *node_name;
	const char *device_name;
};

/* A separate capability: supplying read callbacks never authorizes writes.
 * write completes exactly length bytes; errors may have changed any part of the
 * requested range. flush must persist every preceding successful write through
 * all volatile caches before returning success. Read-after-write must be coherent.
 * The caller exclusively owns the resource throughout recovery or mutation.
 * A failed write/flush makes the outcome uncertain: stop mutations and reopen
 * through recovery. Checksummed control-block damage requires offline repair. */
struct ext4_write_environment {
	void *context;
	enum ext4_result (*write)(
	    void *context, uint64_t offset, const void *buffer, size_t length);
	enum ext4_result (*flush)(void *context);
	/* Required to write a volume with the MMP feature; otherwise unused. */
	const struct ext4_mmp_environment *mmp;
};

/* Optional external journal device, supplied explicitly by the resource owner.
 * It follows the same exact-I/O, coherent-read and durability contracts above.
 * The owner exclusively retains BOTH devices until unmount/recovery completes;
 * they must not alias. Journal flushes persist this device independently of the
 * filesystem writer. Allocation still belongs to the filesystem environment.
 * The core verifies the on-disk UUID association; it never opens device names. */
struct ext4_journal_environment {
	void *context;
	uint64_t size_bytes;
	enum ext4_result (*read)(void *context, uint64_t offset, void *buffer, size_t length);
	enum ext4_result (*write)(
	    void *context, uint64_t offset, const void *buffer, size_t length);
	enum ext4_result (*flush)(void *context);
};

struct ext4_recovery_report {
	uint32_t transactions;
	uint32_t replayed_blocks;
	uint32_t revoked_blocks;
	uint32_t cleaned_orphans;
	uint32_t orphan_transactions;
	uint32_t orphan_file_transfers;
	bool accounting_updated;
	bool discarded_tail;
	uint32_t fast_commits;
};

struct ext4_info {
	uint64_t blocks;
	uint64_t free_blocks;
	uint32_t inodes;
	uint32_t free_inodes;
	uint32_t block_size;
	uint32_t groups;
	uint32_t feature_compat;
	uint32_t feature_incompat;
	uint32_t feature_ro_compat;
	/* Seconds between required ext4_mmp_update calls; zero without MMP. */
	uint32_t mmp_interval;
	uint8_t uuid[EXT4_UUID_SIZE];
	char volume_name[EXT4_VOLUME_NAME_SIZE + 1];
};

struct ext4_timestamp {
	int64_t seconds;
	uint32_t nanoseconds;
};

enum ext4_attribute_field {
	EXT4_ATTR_PERMISSIONS = 1U << 0,
	EXT4_ATTR_UID = 1U << 1,
	EXT4_ATTR_GID = 1U << 2,
	EXT4_ATTR_ACCESS_TIME = 1U << 3,
	EXT4_ATTR_CHANGE_TIME = 1U << 4,
	EXT4_ATTR_MODIFY_TIME = 1U << 5,
	EXT4_ATTR_BIRTH_TIME = 1U << 6,
	EXT4_ATTR_XATTRS = 1U << 7
};

/* An admitted operation, not credentials or an authorization bypass. The owner
 * authorizes against a fresh inode while holding its serialization lock, and
 * supplies the final permission bits (including any set-ID removal) and times.
 * Unselected fields are preserved. permissions contains no file-type bits. */
struct ext4_inode_update {
	uint32_t fields;
	uint32_t uid;
	uint32_t gid;
	uint16_t permissions;
	struct ext4_timestamp access_time;
	struct ext4_timestamp change_time;
	struct ext4_timestamp modify_time;
	struct ext4_timestamp birth_time;
	const struct ext4_xattr_change *xattrs;
	size_t xattr_count;
};

struct ext4_inode {
	uint64_t size;
	uint64_t blocks_512;
	uint32_t number;
	uint32_t generation;
	uint32_t uid;
	uint32_t gid;
	/* Project ID on PROJECT volumes; zero otherwise or without room in the record. */
	uint32_t project;
	uint32_t flags;
	/* Device identity is meaningful only for character/block special files. */
	uint32_t device_major;
	uint32_t device_minor;
	struct ext4_timestamp access_time;
	struct ext4_timestamp change_time;
	struct ext4_timestamp modify_time;
	struct ext4_timestamp birth_time;
	uint16_t mode;
	uint16_t links;
	uint8_t block_data[EXT4_INODE_BLOCK_BYTES];
	bool fast_symlink;
	bool birth_time_valid;
};

struct ext4_special_file {
	enum ext4_file_type type;
	uint32_t device_major;
	uint32_t device_minor;
};

struct ext4_dir_entry {
	uint32_t inode;
	enum ext4_file_type type;
	uint16_t name_length;
	uint8_t name[EXT4_NAME_MAX + 1];
};

/* On-disk namespaces; the adapter owns their visibility and authorization. */
enum ext4_xattr_namespace {
	EXT4_XATTR_USER = 1,
	EXT4_XATTR_POSIX_ACL_ACCESS = 2,
	EXT4_XATTR_POSIX_ACL_DEFAULT = 3,
	EXT4_XATTR_TRUSTED = 4,
	EXT4_XATTR_SECURITY = 6,
	EXT4_XATTR_SYSTEM = 7,
	EXT4_XATTR_RICHACL = 8
};

struct ext4_xattr_key {
	uint32_t value_size;
	uint8_t name_index;
	uint8_t name_length;
	uint8_t name[EXT4_NAME_MAX];
};

enum ext4_xattr_policy { EXT4_XATTR_SET, EXT4_XATTR_CREATE, EXT4_XATTR_REPLACE, EXT4_XATTR_REMOVE };

#define EXT4_XATTR_MAX_CHANGES 8192U

/* EXT4_ATTR_XATTRS selects an admitted batch of distinct raw keys, changed in one
 * transaction with the other selected inode fields. CHANGE_TIME is required. CREATE/REPLACE
 * enforce existence against the original inode; REMOVE requires an existing key
 * and NULL/zero value. Caller-owned names and values remain valid through return.
 * ACL/security values are opaque; the owner supplies their admitted transition.
 * EA_INODE filesystems admit values up to 64 KiB, subject to transaction capacity.
 * Other formats require values to fit the inode body and external attribute block.
 * On an inode with attributes, permission/owner changes, writes and truncates
 * require this selection. An empty batch explicitly admits preserving the keys. */
struct ext4_xattr_change {
	enum ext4_xattr_policy policy;
	uint8_t name_index;
	const uint8_t *name;
	size_t name_length;
	const void *value;
	size_t value_size;
};

/* Immutable read mapping; an adapter must not reuse it across future mutations. */
struct ext4_mapping {
	uint64_t device_offset;
	size_t length;
	bool hole;
};

struct ext4_rename_entry {
	uint32_t directory;
	uint32_t directory_generation;
	const uint8_t *name;
	size_t name_length;
	uint32_t inode;
	uint32_t generation;
};

enum ext4_rename_flags { EXT4_RENAME_NOREPLACE = 1U << 0, EXT4_RENAME_EXCHANGE = 1U << 1 };

enum ext4_result ext4_mount(const struct ext4_environment *environment, struct ext4_fs **result);
/* The owner must serialize ALL access to a writable instance, including reads,
 * inode snapshots and mapping consumers, through each operation's completion.
 * Refresh affected snapshots after mutation; never retain a mapping across it.
 * Mount requires a clean resource and does not implicitly recover it. */
enum ext4_result ext4_mount_writable(const struct ext4_environment *environment,
    const struct ext4_write_environment *writer, struct ext4_fs **result);
/* NULL journal selects the internal journal. A supplied resource requires an
 * external-journal filesystem with exactly one registered filesystem UUID. */
enum ext4_result ext4_mount_writable_with_journal(const struct ext4_environment *environment,
    const struct ext4_write_environment *writer, const struct ext4_journal_environment *journal,
    struct ext4_fs **result);

/* Commit policy of a writable owner. With commit_blocks zero, each mutation is its
 * own synchronous durable transaction. Otherwise completed mutations join a compound
 * transaction of at most commit_blocks metadata snapshots, as Linux's jbd2 running
 * transaction does, and every later read sees them. The compound becomes durable as
 * one journal transaction on ext4_commit or ext4_sync, or when the next mutation
 * would not fit. A power cut loses the mutations after the last durable commit and
 * never applies one partially. commit_blocks may not exceed the recovery bound of
 * the journal, half its ring and 32 MiB of blocks.
 *
 * EXT4_WRITE_ORDERED_DATA writes regular-file data in place before the commit that
 * references it instead of journaling it, as Linux's data=ordered does: committed
 * metadata never exposes a block whose data is not durable, but a power cut can
 * leave overwritten blocks of an existing file with their new contents while its
 * size and times are old. Data reusing a block freed since the last durable commit
 * stays journaled, so a power cut never shows it through the block's old owner.
 * Without the flag every write, including its data, is atomic.
 *
 * With checkpoint_blocks zero, each commit writes its blocks home and empties the
 * log before the next transaction. Otherwise committed transactions stay in the log,
 * as jbd2's checkpoint list does, and the latest committed version of each of their
 * blocks, at most checkpoint_blocks, stays in memory for reads. A checkpoint writes
 * them home and empties the log when the next commit would not fit in the log or in
 * that bound, and on ext4_sync; a power cut or an unmount without ext4_sync leaves
 * them for recovery. checkpoint_blocks may not exceed the ring of the journal or
 * 32 MiB of blocks. */
#define EXT4_WRITE_ORDERED_DATA 0x1U

struct ext4_write_options {
	uint32_t commit_blocks;
	uint32_t flags;
	uint32_t checkpoint_blocks;
};
enum ext4_result ext4_mount_writable_with_options(const struct ext4_environment *environment,
    const struct ext4_write_environment *writer, const struct ext4_journal_environment *journal,
    const struct ext4_write_options *options, struct ext4_fs **result);
/* Make every completed mutation durable, as fsync does, without checkpointing or
 * clearing the recovery marker. With synchronous commits it writes nothing. */
enum ext4_result ext4_commit(struct ext4_fs *fs);
/* Commit, checkpoint and also clear the recovery marker. unmount only releases memory
 * and discards uncommitted mutations; call sync first for a clean shutdown. An
 * uncertain commit poisons the instance, including reads: unmount and recover. */
enum ext4_result ext4_sync(struct ext4_fs *fs);
/* Inspect fresh, generation-checked xattrs without writing. Names exclude their
 * namespace prefix and have no trailing NUL; values are opaque bytes, including
 * ACL/security values. Both inode-body and external attributes are validated
 * before any output changes. NULL buffer/keys with zero capacity queries size or
 * count. An insufficient non-NULL buffer returns RANGE without changing outputs.
 * List order is namespace, name length, then unsigned name bytes. Unknown namespace
 * indices remain visible to the owning adapter. Mutation still requires its
 * separate storage and admitted security-policy contract. */
enum ext4_result ext4_get_xattr(struct ext4_fs *fs, uint32_t number, uint32_t generation,
    uint8_t name_index, const uint8_t *name, size_t name_length, void *buffer, size_t capacity,
    size_t *size);
enum ext4_result ext4_list_xattrs(struct ext4_fs *fs, uint32_t number, uint32_t generation,
    struct ext4_xattr_key *keys, size_t capacity, size_t *count);
enum ext4_result ext4_set_attributes(struct ext4_fs *fs, uint32_t number, uint32_t generation,
    const struct ext4_inode_update *update, struct ext4_inode *result);

/* Atomically replace selected policy bits and ctime, preserving every other
 * field. mask must be nonzero and contain only MODIFIABLE_FLAGS; flags must be
 * a subset of mask. DIRSYNC/TOPDIR/PROJINHERIT require a directory; other
 * nonregular types admit only NODUMP/NOATIME. The exclusive owner authorizes the flag transition
 * (including protected-bit and journal-mode privileges), drains pending writes
 * and revokes incompatible mappings before calling. This operation can clear
 * IMMUTABLE/APPEND; changing other flags on an immutable inode must also clear
 * IMMUTABLE. Ordinary mutation APIs cannot bypass their restrictions.
 * JOURNAL_DATA is recorded but does not select a mode: the mount's write options
 * decide how data is committed. NOATIME governs automatic
 * platform updates, not an explicitly admitted timestamp change. mask may also
 * select CASEFOLD, which changes only on empty directories of casefold volumes:
 * other volumes return UNSUPPORTED, other types NOT_DIRECTORY and directories
 * with entries NOT_EMPTY. */
enum ext4_result ext4_set_inode_flags(struct ext4_fs *fs, uint32_t number, uint32_t generation,
    uint32_t mask, uint32_t flags, const struct ext4_timestamp *change_time,
    struct ext4_inode *result);
/* Change an inode's project ID and ctime in one transaction, moving its quota
 * usage to the new project, as Linux's FS_IOC_FSSETXATTR does. Volumes without
 * the PROJECT feature admit only project zero as a no-op; records without room for
 * the field return UNSUPPORTED, and immutable inodes PERMISSION_DENIED. An
 * unchanged ID changes nothing. The owner authorizes the transition. */
enum ext4_result ext4_set_project(struct ext4_fs *fs, uint32_t number, uint32_t generation,
    uint32_t project, const struct ext4_timestamp *change_time, struct ext4_inode *result);

/* Quota enforcement is the adapter's policy, as Linux's quota mount options are.
 * Without one, a writable owner only accounts usage. With one, a transaction that
 * would raise an enforced ID's usage above its hard limit, or above its soft limit
 * once that limit's grace period has expired, is refused before any write with
 * QUOTA_EXCEEDED and the owner remains usable; partial writes keep their durable
 * prefix. Crossing a soft limit starts its grace period from the quota file's
 * grace time. Decreases are never refused and, as in Linux, clear the grace period
 * once usage is back within the soft limit. Block limits are 1 KiB quota blocks. */
#define EXT4_QUOTA_ENFORCE_USER 0x1U
#define EXT4_QUOTA_ENFORCE_GROUP 0x2U
#define EXT4_QUOTA_ENFORCE_PROJECT 0x4U

struct ext4_quota_policy {
	void *context;
	/* EXT4_QUOTA_ENFORCE_* types; each must be tracked by the volume. */
	uint32_t types;
	/* Seconds since the Unix epoch, for grace periods. */
	int64_t (*now)(void *context);
};
/* Install or replace the policy of a writable owner; NULL removes it. The policy
 * is copied. Types the volume does not track return UNSUPPORTED. */
enum ext4_result ext4_quota_policy_set(struct ext4_fs *fs, const struct ext4_quota_policy *policy);
/* Exempt this serialized owner's subsequent operations from limits, as Linux does
 * for CAP_SYS_RESOURCE, until cleared. The adapter decides who is privileged;
 * usage is still accounted. */
void ext4_quota_exempt(struct ext4_fs *fs, bool exempt);
/* Writes regular files, allocating holes, converting unwritten extents and
 * extending EOF in a bounded atomic transaction. Newly exposed bytes are zeroed.
 * Requests exceeding transaction capacity reject without writes. Ordinary
 * allocation preserves the filesystem's reserved-block pool. Data, allocation
 * metadata, size, permission bits, mtime and ctime share the transaction.
 * Those three attribute fields are required; XATTRS may also select an admitted
 * batch in the same transaction and is required when the inode has attributes.
 * completed is length only on success, otherwise zero; an I/O error can have a
 * committed outcome that must be resolved by recovery. Zero length is a no-op. */
enum ext4_result ext4_write(struct ext4_fs *fs, uint32_t number, uint32_t generation,
    uint64_t offset, const void *buffer, size_t length, const struct ext4_inode_update *update,
    size_t *completed);
/* Write a large request in bounded transactions under one exclusive owner.
 * The buffer and admitted update stay immutable for the complete call. OK means
 * all bytes were written; an error retains the number of bytes from successfully
 * checkpointed transactions. No partially prepared transaction contributes to
 * completed. An uncertain commit poisons the instance and may add another whole
 * transaction during recovery; it never removes the reported durable prefix.
 * Permissions and captured times accompany each data transaction. The xattr
 * batch commits only with the first successful prefix, then is preserved.
 * A caller retrying after partial progress must refresh policy and must not
 * replay that already applied xattr batch. Whole-request overflow rejects before
 * writes. Zero length follows ext4_write. Written preallocation in an EOF gap
 * can be zeroed in preparatory transactions without changing size or attributes.
 * If unwritten conversion needs unavailable mapping space, a required existing
 * prefix can first be zeroed without allocation. Inside current EOF it can also
 * be initialized during preparation. Growth keeps its prepared prefix unwritten
 * until initialization, data, EOF and attributes commit together. Reservation
 * retains leaf capacity for a partial prefix; an older full tree may still need
 * mapping allocation. Moving EOF can also prepare the old boundary's suffix to
 * recover that capacity. Visible bytes, size and attributes remain unchanged
 * until the data transaction commits.
 * An error may retain either preparation even when completed is zero. */
enum ext4_result ext4_write_partial(struct ext4_fs *fs, uint32_t number, uint32_t generation,
    uint64_t offset, const void *buffer, size_t length, const struct ext4_inode_update *update,
    size_t *completed);
/* Resize a regular file with the same admitted attribute fields as write.
 * Shrink releases data and unused mapping nodes, including allocations beyond
 * EOF, and zeroes the retained partial block. Growth exposes zero bytes without
 * allocating holes. Size, block accounting and attributes commit atomically.
 * Changed mapping nodes and other metadata must fit the transaction bound;
 * credit exhaustion rejects without device writes. result changes on success. */
enum ext4_result ext4_truncate_atomic(struct ext4_fs *fs, uint32_t number, uint32_t generation,
    uint64_t size, const struct ext4_inode_update *update, struct ext4_inode *result);
/* Resize under the same exclusive owner and admitted attribute contract.
 * Shrink uses bounded transactions; the first atomically records the target
 * size, attributes and legacy orphan intent. Recovery completes a committed
 * intent, so an interrupted request may finish after an error. Any error after
 * that commit poisons the instance, including reads. Successful completion has
 * released all suffix blocks and removed the intent. Before the first commit,
 * private failures leave the resource unchanged. Journals too small to reserve
 * cleanup paths retain the atomic shrink limit. Growth can first zero written
 * preallocation in bounded transactions, retaining the old size and attributes
 * until the final transaction. On error those invisible zeros can persist; an
 * uncertain final commit can expose the complete requested size after recovery.
 * result changes only on complete success. */
enum ext4_result ext4_truncate(struct ext4_fs *fs, uint32_t number, uint32_t generation,
    uint64_t size, const struct ext4_inode_update *update, struct ext4_inode *result);

enum ext4_fallocate_flags { EXT4_FALLOC_KEEP_SIZE = 1U << 0, EXT4_FALLOC_PUNCH_HOLE = 1U << 1 };

/* Reserve a nonempty byte range, preserving existing data and allocating holes.
 * Reservation requires extent mapping and leaves new extents unwritten. It also
 * retains leaf capacity for later partial EOF growth through write_partial;
 * checking an already backed range can therefore need a mapping allocation.
 * Without KEEP_SIZE,
 * each committed prefix can grow EOF. PUNCH_HOLE requires KEEP_SIZE: whole blocks
 * are freed, partial written blocks are zeroed, and size remains unchanged.
 * Punching supports extent and indirect mappings.
 * Like write_partial, this uses bounded transactions under one exclusive owner.
 * completed records the byte prefix in successful checkpoints, including holes
 * already satisfying the operation; a failed private step contributes nothing.
 * An uncertain commit poisons the instance and recovery can add one whole step.
 * The admitted write attributes accompany each step; its xattr batch applies
 * only with the first successful prefix. Whole-range validation precedes writes.
 * Growth can first zero hidden written preallocation without changing EOF or
 * attributes, as with truncate. Refresh policy before retrying a partial call. */
enum ext4_result ext4_fallocate(struct ext4_fs *fs, uint32_t number, uint32_t generation,
    uint64_t offset, uint64_t length, uint32_t flags, const struct ext4_inode_update *update,
    uint64_t *completed);
/* Namespace mutations share the writable instance's exclusive owner. The caller
 * authorizes against fresh objects and supplies admitted creation attributes and
 * one captured namespace time; this interface does not confer policy authority.
 * Create/mkdir require permissions, UID/GID and atime/mtime/ctime; birth time is
 * optional and must be representable. XATTRS may provide admitted child attributes;
 * it is required, even for an empty batch, when the parent has attributes. The
 * owner decides ACL/security inheritance. They create an empty regular file or a
 * directory containing dot/dotdot. Parent ctime/mtime change to directory_time.
 * Existing names and dot/dotdot are rejected before writes. Linear and bounded
 * indexed directories use the same transaction and admitted attribute contract.
 * Allocation, directory records, link counts and timestamps commit atomically.
 * Outputs change only on success; uncertain commits require explicit recovery.
 * Immutable parents reject additions; append-only parents accept additions but
 * reject removal/replacement. Protected targets reject linking or removal. */
enum ext4_result ext4_create(struct ext4_fs *fs, uint32_t directory, uint32_t generation,
    const uint8_t *name, size_t name_length, const struct ext4_inode_update *attributes,
    const struct ext4_timestamp *directory_time, struct ext4_inode *result);
enum ext4_result ext4_mkdir(struct ext4_fs *fs, uint32_t directory, uint32_t generation,
    const uint8_t *name, size_t name_length, const struct ext4_inode_update *attributes,
    const struct ext4_timestamp *directory_time, struct ext4_inode *result);
/* Create a character/block device, FIFO or socket inode with the same admitted
 * attributes and atomic namespace contract. Device numbers use ext4's 12/20-bit
 * major/minor format and must be zero for FIFO/socket. This stores metadata only;
 * opening a device, pipe or socket is an adapter-owned operation. */
enum ext4_result ext4_mknod(struct ext4_fs *fs, uint32_t directory, uint32_t generation,
    const uint8_t *name, size_t name_length, const struct ext4_special_file *special,
    const struct ext4_inode_update *attributes, const struct ext4_timestamp *directory_time,
    struct ext4_inode *result);
/* Create a symbolic link in the same atomic namespace transaction. Target bytes
 * are opaque except that NUL and empty targets are rejected. The target and its
 * terminating NUL must fit one filesystem block. The terminator is not in size.
 * Creation attributes and parent timestamps follow the create/mkdir contract. */
enum ext4_result ext4_symlink(struct ext4_fs *fs, uint32_t directory, uint32_t generation,
    const uint8_t *name, size_t name_length, const uint8_t *target, size_t target_length,
    const struct ext4_inode_update *attributes, const struct ext4_timestamp *directory_time,
    struct ext4_inode *result);
/* Add another name for an allocated non-directory inode. Update its ctime and
 * the destination directory's ctime/mtime to time, preserving other attributes. */
enum ext4_result ext4_link(struct ext4_fs *fs, uint32_t directory, uint32_t directory_generation,
    const uint8_t *name, size_t name_length, uint32_t target, uint32_t target_generation,
    const struct ext4_timestamp *time, struct ext4_inode *result);
/* Remove the named, generation-checked target. Unlink rejects directories;
 * rmdir requires an empty directory with matching dot/dotdot. Name removal,
 * link counts, parent mtime/ctime and target ctime share one transaction. The
 * last link enrolls crash recovery before any block can be reused. Held objects
 * survive until their last release; unheld objects are reclaimed before return.
 * An error after the namespace commit poisons the instance and recovery may
 * complete the deletion. result changes only on complete success and describes
 * the removed inode before reclamation; it is not itself a lifetime reference. */
enum ext4_result ext4_unlink(struct ext4_fs *fs, uint32_t directory, uint32_t directory_generation,
    const uint8_t *name, size_t name_length, uint32_t target, uint32_t target_generation,
    const struct ext4_timestamp *time, struct ext4_inode *result);
enum ext4_result ext4_rmdir(struct ext4_fs *fs, uint32_t directory, uint32_t directory_generation,
    const uint8_t *name, size_t name_length, uint32_t target, uint32_t target_generation,
    const struct ext4_timestamp *time, struct ext4_inode *result);

/* Rename generation-checked entries atomically under the exclusive owner.
 * Destination inode/generation zero requires absence; a nonzero inode with its
 * generation identifies the expected destination; zero generation permits the
 * current generation as in other mutation APIs. NOREPLACE and EXCHANGE are mutually exclusive.
 * Parent times, child ctimes, directory dotdot/link counts and replacement orphan
 * ownership share one transaction. An overwritten held inode remains accessible
 * through its hold. The result is the source inode snapshot and changes only on
 * success; a post-commit reclamation error poisons the instance, as with unlink.
 * Renaming two names for the same inode is a no-op except with NOREPLACE. */
enum ext4_result ext4_rename(struct ext4_fs *fs, const struct ext4_rename_entry *source,
    const struct ext4_rename_entry *destination, uint32_t flags, const struct ext4_timestamp *time,
    struct ext4_inode *result);
/* Atomically rename and replace the old name with a new character device 0:0.
 * whiteout_attributes supplies admitted creation attributes, including explicit
 * ownership, zero permission bits and any parent ACL/security inheritance.
 * Flags may be zero or NOREPLACE; EXCHANGE is invalid. Existing aliases of one
 * inode remain a no-op as with rename. The caller authorizes device creation;
 * no credentials or overlay policy are inferred inside the filesystem. */
enum ext4_result ext4_rename_whiteout(struct ext4_fs *fs, const struct ext4_rename_entry *source,
    const struct ext4_rename_entry *destination, uint32_t flags,
    const struct ext4_inode_update *whiteout_attributes, const struct ext4_timestamp *time,
    struct ext4_inode *result);

/* A writable owner of an MMP volume refreshes its sequence at least once per
 * info.mmp_interval seconds. Mutations also refresh a stale sequence before their
 * first write. A sequence or node written by another host poisons the instance and
 * returns BUSY. Volumes without MMP return OK without I/O. */
enum ext4_result ext4_mmp_update(struct ext4_fs *fs);
/* After a successful ext4_sync, publish the clean MMP sequence. The instance then
 * rejects further mutation; unmount it. Volumes without MMP return OK. */
enum ext4_result ext4_mmp_release(struct ext4_fs *fs);

/* Offline recovery replays the journal, reconstructs allocation summaries and
 * completes legacy-list and modern orphan-file cleanup in bounded transactions.
 * A read-only mount never invokes this operation.
 * On error the resource remains unmounted and must not be used for mutations. */
enum ext4_result ext4_recover(const struct ext4_environment *environment,
    const struct ext4_write_environment *writer, struct ext4_recovery_report *report);
enum ext4_result ext4_recover_with_journal(const struct ext4_environment *environment,
    const struct ext4_write_environment *writer, const struct ext4_journal_environment *journal,
    struct ext4_recovery_report *report);
void ext4_unmount(struct ext4_fs *fs);
void ext4_get_info(const struct ext4_fs *fs, struct ext4_info *info);
enum ext4_result ext4_get_inode(struct ext4_fs *fs, uint32_t number, struct ext4_inode *inode);
/* Hold an allocated inode under the filesystem owner's serialization. Platform
 * owners retain a hold while descriptors, mappings or other native references
 * can access the object. Repeated holds share identity and each needs release.
 * Refresh returns a current snapshot, including an inode unlinked while held;
 * ordinary lookup/get_inode still require a linked inode. The final release
 * reclaims an unlinked inode in restartable transactions and consumes the hold
 * even on failure. Such failure poisons the filesystem until offline recovery.
 * Unmount invalidates all holds without writing, as with other core objects. */
enum ext4_result ext4_hold_inode(
    struct ext4_fs *fs, uint32_t number, uint32_t generation, struct ext4_inode_hold **result);
enum ext4_result ext4_refresh_inode(struct ext4_inode_hold *hold, struct ext4_inode *result);
enum ext4_result ext4_release_inode(struct ext4_inode_hold *hold);
enum ext4_result ext4_read(struct ext4_fs *fs, const struct ext4_inode *inode, uint64_t offset,
    void *buffer, size_t length, size_t *completed);
/* Return a contiguous physical or zero-filled range from a fresh regular-file
 * snapshot. The range can include padding in the block containing EOF, but no
 * later blocks. The owner zeroes EOF padding before exposing it through a native
 * page cache. A writable owner's journal can hold the current contents of journaled
 * data in memory until a commit or checkpoint writes them home; the range ends
 * before such a block, and BUSY reports a range that starts at one: read it with
 * ext4_read, or map it after ext4_sync. Failure leaves mapping unchanged. */
enum ext4_result ext4_map_read(struct ext4_fs *fs, const struct ext4_inode *inode, uint64_t offset,
    size_t length, struct ext4_mapping *mapping);

enum ext4_dir_action { EXT4_DIR_ACCEPT, EXT4_DIR_ACCEPT_STOP, EXT4_DIR_STOP };

/* Stream entries using one temporary directory block. Every block's checksum,
 * records and resume boundary are validated before any of its entries are
 * delivered. The visitor receives the cookie following the entry: ACCEPT
 * consumes it and continues, ACCEPT_STOP consumes it and returns, and STOP
 * returns without consuming it. A stopped visit returns OK; NOT_FOUND means
 * the directory ended, including when entries were delivered in this call.
 * Errors retain progress through earlier accepted entries/validated blocks.
 * Entry storage expires when the visitor returns. Read-only core queries are
 * allowed in the visitor; mutation, unmount or changes to directory/cookie are
 * not. The writable owner's serialization covers the complete call and visitor.
 * No directory data is retained between calls or across a mutation. */
enum ext4_result ext4_iterate_dir(struct ext4_fs *fs, const struct ext4_inode *directory,
    uint64_t *cookie,
    enum ext4_dir_action (*visit)(
	void *context, const struct ext4_dir_entry *entry, uint64_t next_cookie),
    void *context);
/* EXT4_NOT_FOUND means end-of-directory; cookie is an opaque resumable offset. */
enum ext4_result ext4_next_dir(struct ext4_fs *fs, const struct ext4_inode *directory,
    uint64_t *cookie, struct ext4_dir_entry *entry);
enum ext4_result ext4_lookup(struct ext4_fs *fs, const struct ext4_inode *directory,
    const uint8_t *name, size_t name_length, struct ext4_inode *inode);
const char *ext4_result_string(enum ext4_result result);

#endif

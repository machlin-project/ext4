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
	EXT4_NOT_EMPTY
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
	uint32_t flags;
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
 * ACL/security values are opaque; the owner supplies their admitted transition. */
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
/* Mutations are synchronous durable transactions. sync also clears the recovery
 * marker. unmount only releases memory; call sync first for a clean shutdown.
 * An uncertain commit poisons the instance, including reads: unmount and recover. */
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
/* Writes regular files, allocating holes, converting unwritten extents and
 * extending EOF in a bounded atomic transaction. Newly exposed bytes are zeroed.
 * Requests exceeding transaction capacity reject without writes. Ordinary
 * allocation preserves the filesystem's reserved-block pool. Data, allocation
 * metadata, size, permission bits, mtime and ctime share the transaction.
 * Those three attribute fields are required; no other fields may be selected.
 * completed is length only on success, otherwise zero; an I/O error can have a
 * committed outcome that must be resolved by recovery. Zero length is a no-op. */
enum ext4_result ext4_write(struct ext4_fs *fs, uint32_t number, uint32_t generation,
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
 * cleanup paths retain the atomic limit, as does growth. result changes only
 * on complete success. */
enum ext4_result ext4_truncate(struct ext4_fs *fs, uint32_t number, uint32_t generation,
    uint64_t size, const struct ext4_inode_update *update, struct ext4_inode *result);
/* Namespace mutations share the writable instance's exclusive owner. The caller
 * authorizes against fresh objects and supplies admitted creation attributes and
 * one captured namespace time; this interface does not confer policy authority.
 * Create/mkdir require permissions, UID/GID and atime/mtime/ctime; birth time is
 * optional and must be representable. They create an empty regular file or a
 * directory containing dot/dotdot. Parent ctime/mtime change to directory_time.
 * Existing names and dot/dotdot are rejected before writes. Linear and bounded
 * indexed directories use the same transaction and admitted attribute contract.
 * Allocation, directory records, link counts and timestamps commit atomically.
 * Outputs change only on success; uncertain commits require explicit recovery. */
enum ext4_result ext4_create(struct ext4_fs *fs, uint32_t directory, uint32_t generation,
    const uint8_t *name, size_t name_length, const struct ext4_inode_update *attributes,
    const struct ext4_timestamp *directory_time, struct ext4_inode *result);
enum ext4_result ext4_mkdir(struct ext4_fs *fs, uint32_t directory, uint32_t generation,
    const uint8_t *name, size_t name_length, const struct ext4_inode_update *attributes,
    const struct ext4_timestamp *directory_time, struct ext4_inode *result);
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

/* Offline recovery replays the journal, reconstructs allocation summaries and
 * completes legacy-list and modern orphan-file cleanup in bounded transactions.
 * A read-only mount never invokes this operation.
 * On error the resource remains unmounted and must not be used for mutations. */
enum ext4_result ext4_recover(const struct ext4_environment *environment,
    const struct ext4_write_environment *writer, struct ext4_recovery_report *report);
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
enum ext4_result ext4_map_read(struct ext4_fs *fs, const struct ext4_inode *inode, uint64_t offset,
    size_t length, struct ext4_mapping *mapping);
/* EXT4_NOT_FOUND means end-of-directory; cookie is an opaque resumable offset. */
enum ext4_result ext4_next_dir(struct ext4_fs *fs, const struct ext4_inode *directory,
    uint64_t *cookie, struct ext4_dir_entry *entry);
enum ext4_result ext4_lookup(struct ext4_fs *fs, const struct ext4_inode *directory,
    const uint8_t *name, size_t name_length, struct ext4_inode *inode);
const char *ext4_result_string(enum ext4_result result);

#endif

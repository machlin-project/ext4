/* SPDX-License-Identifier: BSD-3-Clause */
#include "fscrypt.h"
#include "allocate.h"
#include "inline.h"
#include "quota.h"
#include "xattr.h"

#define EXT4_ATTRIBUTE_FIELDS                                                                      \
	((uint32_t)(EXT4_ATTR_PERMISSIONS | EXT4_ATTR_UID | EXT4_ATTR_GID |                        \
	    EXT4_ATTR_ACCESS_TIME | EXT4_ATTR_CHANGE_TIME | EXT4_ATTR_MODIFY_TIME |                \
	    EXT4_ATTR_BIRTH_TIME | EXT4_ATTR_XATTRS))
#define EXT4_WRITE_FIELDS (EXT4_ATTR_PERMISSIONS | EXT4_ATTR_CHANGE_TIME | EXT4_ATTR_MODIFY_TIME)

enum ext4_result
ext4_mount_writable(const struct ext4_environment *environment,
    const struct ext4_write_environment *writer, struct ext4_fs **result)
{
	return ext4_mount_writable_with_journal(environment, writer, NULL, result);
}

enum ext4_result
ext4_mount_writable_with_journal(const struct ext4_environment *environment,
    const struct ext4_write_environment *writer, const struct ext4_journal_environment *journal,
    struct ext4_fs **result)
{
	return ext4_mount_writable_with_options(environment, writer, journal, NULL, result);
}

enum ext4_result
ext4_mount_writable_with_options(const struct ext4_environment *environment,
    const struct ext4_write_environment *writer, const struct ext4_journal_environment *journal,
    const struct ext4_write_options *options, struct ext4_fs **result)
{
	struct ext4_fs *fs;
	enum ext4_result error;

	if (result == NULL) {
		return EXT4_INVALID_ARGUMENT;
	}
	*result = NULL;
	if (writer == NULL || writer->write == NULL || writer->flush == NULL) {
		return EXT4_INVALID_ARGUMENT;
	}
	error = ext4_mount(environment, &fs);
	if (error != EXT4_OK) {
		return error;
	}
	if (fs->first_inode < EXT4_FIRST_NON_RESERVED_INODE || fs->first_inode > fs->info.inodes ||
	    fs->clusters_per_group % EXT4_BITS_PER_BYTE != 0 ||
	    fs->inodes_per_group % EXT4_BITS_PER_BYTE != 0) {
		error = EXT4_CORRUPT;
	} else {
		/* Multi-mount protection precedes every other write. */
		error = ext4_mmp_start(fs, writer, false);
	}
	if (error == EXT4_OK) {
		error = ext4_journal_open_external(fs, writer, journal, &fs->journal);
	}
	if (error == EXT4_OK) {
		error = ext4_system_ranges_build(fs);
	}
	if (error == EXT4_OK) {
		error = ext4_quota_open(fs);
	}
	if (error == EXT4_OK && options != NULL &&
	    (options->flags & ~(uint32_t)EXT4_WRITE_ORDERED_DATA) != 0) {
		error = EXT4_INVALID_ARGUMENT;
	}
	if (error == EXT4_OK && options != NULL && options->commit_blocks != 0) {
		if (options->commit_blocks > ext4_journal_compound_limit(fs->journal)) {
			error = EXT4_RANGE;
		} else {
			fs->journal->compound_blocks = options->commit_blocks;
		}
	}
	if (error == EXT4_OK && options != NULL && options->checkpoint_blocks != 0) {
		if (options->checkpoint_blocks > ext4_journal_checkpoint_limit(fs->journal)) {
			error = EXT4_RANGE;
		} else {
			fs->journal->checkpoint_blocks = options->checkpoint_blocks;
		}
	}
	if (error == EXT4_OK && options != NULL) {
		fs->journal->ordered_data = (options->flags & EXT4_WRITE_ORDERED_DATA) != 0;
	}
	if (error != EXT4_OK) {
		ext4_unmount(fs);
		return error;
	}
	fs->validated_maps_enabled = true;
	*result = fs;
	return EXT4_OK;
}

enum ext4_result
ext4_commit(struct ext4_fs *fs)
{
	enum ext4_result error;

	if (fs == NULL) {
		return EXT4_INVALID_ARGUMENT;
	}
	if (fs->aborted) {
		return EXT4_RECOVERY_REQUIRED;
	}
	if (fs->journal == NULL) {
		return EXT4_OK;
	}
	error = ext4_mmp_guard(fs);
	if (error == EXT4_OK) {
		error = ext4_journal_commit(fs->journal);
	}
	if (error != EXT4_OK) {
		fs->aborted = true;
	}
	return error;
}

enum ext4_result
ext4_sync(struct ext4_fs *fs)
{
	enum ext4_result error;

	if (fs == NULL) {
		return EXT4_INVALID_ARGUMENT;
	}
	if (fs->aborted) {
		return EXT4_RECOVERY_REQUIRED;
	}
	if (fs->journal == NULL) {
		return EXT4_OK;
	}
	error = ext4_commit(fs);
	if (error != EXT4_OK) {
		return error;
	}
	/* A failed checkpoint leaves the committed transactions for recovery. */
	error = ext4_journal_checkpoint(fs->journal);
	if (error == EXT4_OK && fs->last_orphan != 0) {
		error = ext4_orphan_validate_live(fs);
		if (error == EXT4_OK &&
		    (fs->journal->transaction_active || fs->journal->start != 0)) {
			error = EXT4_RECOVERY_REQUIRED;
		}
		if (error == EXT4_OK) {
			error = ext4_journal_flush(fs->journal);
		}
	} else if (error == EXT4_OK) {
		error = ext4_journal_finish(fs->journal);
	}
	if (error != EXT4_OK) {
		fs->aborted = true;
	}
	return error;
}

static enum ext4_result
ext4_update_validate(struct ext4_fs *fs, const struct ext4_inode_update *update)
{
	if (fs == NULL || update == NULL || (update->fields & ~EXT4_ATTRIBUTE_FIELDS) ||
	    ((update->fields & EXT4_ATTR_PERMISSIONS) &&
		(update->permissions & ~EXT4_MODE_PERMISSIONS))) {
		return EXT4_INVALID_ARGUMENT;
	}
	if (fs->aborted) {
		return EXT4_RECOVERY_REQUIRED;
	}
	if (fs->journal == NULL) {
		return EXT4_READ_ONLY;
	}
	/* Ownership changes must carry the admitted set-ID transition. Changing
	 * metadata other than atime also needs the operation's captured ctime. */
	if (((update->fields & (EXT4_ATTR_UID | EXT4_ATTR_GID)) &&
		!(update->fields & EXT4_ATTR_PERMISSIONS)) ||
	    ((update->fields & ~(uint32_t)(EXT4_ATTR_ACCESS_TIME | EXT4_ATTR_CHANGE_TIME)) &&
		!(update->fields & EXT4_ATTR_CHANGE_TIME))) {
		return EXT4_INVALID_ARGUMENT;
	}
	return update->fields & EXT4_ATTR_XATTRS
	    ? ext4_xattr_changes_validate(fs, update->xattrs, update->xattr_count)
	    : EXT4_OK;
}

static enum ext4_result
ext4_edit_inode_record(struct ext4_fs *fs, struct ext4_transaction *transaction, uint32_t number,
    uint32_t generation, struct ext4_inode_disk **disk, struct ext4_inode *inode)
{
	void *buffer;
	uint64_t offset;
	struct ext4_inode_hold *hold;
	enum ext4_result error;

	if (number == 0 || number > fs->info.inodes) {
		return EXT4_INVALID_ARGUMENT;
	}
	if ((number < fs->first_inode && number != EXT4_ROOT_INODE) ||
	    number == fs->journal_inode || number == fs->orphan_file_inode ||
	    ext4_quota_system_inode(fs, number)) {
		return EXT4_UNSUPPORTED;
	}
	error = ext4_inode_allocated(fs, number);
	if (error != EXT4_OK) {
		return error;
	}
	error = ext4_inode_location(fs, number, &offset);
	if (error != EXT4_OK) {
		return error;
	}
	error = ext4_transaction_buffer(transaction, offset / fs->info.block_size, &buffer);
	if (error != EXT4_OK) {
		return error;
	}
	*disk = (struct ext4_inode_disk *)((uint8_t *)buffer + offset % fs->info.block_size);
	error = ext4_inode_decode_live(fs, number, *disk, inode);
	if (error != EXT4_OK) {
		return error;
	}
	if (inode->generation != generation) {
		return EXT4_STALE;
	}
	hold = ext4_inode_find_hold(fs, number);
	if (ext4_le32(&(*disk)->deletion_time) != 0 && (hold == NULL || !hold->unlinked)) {
		return EXT4_CORRUPT;
	}
	return ext4_inode_flags_writable(fs, inode);
}

enum ext4_result
ext4_edit_inode(struct ext4_fs *fs, struct ext4_transaction *transaction, uint32_t number,
    uint32_t generation, struct ext4_inode_disk **disk, struct ext4_inode *inode)
{
	enum ext4_result error;

	error = ext4_edit_inode_record(fs, transaction, number, generation, disk, inode);
	return error == EXT4_OK ? ext4_inode_writable(fs, *disk, inode) : error;
}

static enum ext4_result
ext4_edit_commit(struct ext4_fs *fs, struct ext4_transaction *transaction)
{
	enum ext4_result error;

	error = ext4_transaction_commit(transaction);
	if (error != EXT4_OK) {
		if (!ext4_commit_rejected(error)) {
			fs->aborted = true;
		}
	}
	return error;
}

static enum ext4_result
ext4_update_admitted(
    struct ext4_fs *fs, const struct ext4_inode_disk *disk, const struct ext4_inode_update *update)
{
	/* An explicit empty batch admits preservation. Otherwise a mode/owner or
	 * data change must not silently preserve an unreviewed ACL or capability. */
	if ((update->fields & (EXT4_ATTR_PERMISSIONS | EXT4_ATTR_UID | EXT4_ATTR_GID)) &&
	    !(update->fields & EXT4_ATTR_XATTRS) && ext4_inode_has_xattrs(fs, disk)) {
		return EXT4_INVALID_ARGUMENT;
	}
	return EXT4_OK;
}

enum ext4_result
ext4_set_attributes(struct ext4_fs *fs, uint32_t number, uint32_t generation,
    const struct ext4_inode_update *update, struct ext4_inode *result)
{
	struct ext4_transaction *transaction;
	struct ext4_inode_disk *disk;
	struct ext4_inode inode;
	struct ext4_allocation allocation;
	uint32_t feature_compat;
	uint32_t credits;
	bool allocation_ready = false;
	enum ext4_result error;

	if (result == NULL) {
		return EXT4_INVALID_ARGUMENT;
	}
	error = ext4_update_validate(fs, update);
	if (error != EXT4_OK) {
		return error;
	}
	feature_compat = fs->info.feature_compat;
	credits = update->fields & EXT4_ATTR_XATTRS ? ext4_journal_credits(fs->journal) : 1;
	error = ext4_transaction_begin(fs->journal, credits, &transaction);
	if (error != EXT4_OK) {
		return error;
	}
	error = update->fields & EXT4_ATTR_XATTRS
	    ? ext4_edit_inode_record(fs, transaction, number, generation, &disk, &inode)
	    : ext4_edit_inode(fs, transaction, number, generation, &disk, &inode);
	/* The owner distinguishes admitted automatic/touch times from explicit
	 * timestamp-setting policy. Append writes may still update captured times. */
	if (error == EXT4_OK && update->fields != 0 &&
	    ((inode.flags & EXT4_INODE_IMMUTABLE) ||
		((inode.flags & EXT4_INODE_APPEND) &&
		    (update->fields &
			~(uint32_t)(EXT4_ATTR_ACCESS_TIME | EXT4_ATTR_MODIFY_TIME |
			    EXT4_ATTR_CHANGE_TIME))))) {
		error = EXT4_PERMISSION_DENIED;
	}
	if (error == EXT4_OK) {
		error = ext4_update_admitted(fs, disk, update);
	}
	if (error == EXT4_OK && (update->fields & EXT4_ATTR_XATTRS)) {
		error = ext4_allocation_init(&allocation, fs, transaction, &inode);
		allocation_ready = error == EXT4_OK;
		if (error == EXT4_OK) {
			error = ext4_write_map_validate(&allocation, &inode, disk);
		}
		if (error == EXT4_OK) {
			error = ext4_xattr_apply(
			    &allocation, &inode, disk, update->xattrs, update->xattr_count);
		}
		if (error == EXT4_OK) {
			error = ext4_inode_account(&allocation, &inode, disk, inode.size);
		}
	}
	if (error == EXT4_OK) {
		error = ext4_inode_apply(fs, disk, update);
	}
	if (error == EXT4_OK && inode.links == 0) {
		error = ext4_orphan_reserve(fs, &inode, disk);
	}
	if (error == EXT4_OK) {
		ext4_inode_checksum_set(fs, number, disk);
		error = ext4_inode_decode_live(fs, number, disk, &inode);
	}
	if (allocation_ready) {
		if (allocation.super != NULL) {
			feature_compat = ext4_le32(&allocation.super->feature_compat);
		}
		ext4_allocation_destroy(&allocation);
	}
	if (error != EXT4_OK || update->fields == 0) {
		ext4_transaction_cancel(transaction);
	} else {
		error = ext4_edit_commit(fs, transaction);
	}
	if (error == EXT4_OK) {
		fs->info.feature_compat = feature_compat;
		*result = inode;
	}
	return error;
}

struct ext4_write_target {
	uint64_t physical;
	uint32_t logical;
};

/* Only an exclusive operation can retain this preparation across transactions.
 * No position or validation result survives the public call. */
struct ext4_growth {
	uint64_t zeroed;
	/* The old EOF's unwritten suffix can release the slot needed by the
	 * new EOF's prefix. Both conversions publish in the data transaction. */
	struct ext4_unwritten_extent deferred[2];
	uint32_t initialized[2];
	uint32_t deferred_count;
	uint32_t allocation_logical;
	bool capacity_failed;
	bool zeroing;
	bool mapping_no_space;
};

static uint64_t
ext4_indirect_overhead(uint64_t blocks, uint64_t pointers)
{
	uint64_t metadata;
	uint64_t span = pointers * pointers;
	uint64_t covered;

	if (blocks <= EXT4_DIRECT_BLOCKS) {
		return 0;
	}
	blocks -= EXT4_DIRECT_BLOCKS;
	metadata = 1;
	if (blocks <= pointers) {
		return metadata;
	}
	blocks -= pointers;
	covered = blocks < span ? blocks : span;
	metadata += 1U + (covered + pointers - 1U) / pointers;
	if (blocks <= span) {
		return metadata;
	}
	blocks -= span;
	return metadata + 1U + (blocks + span - 1U) / span + (blocks + pointers - 1U) / pointers;
}

static enum ext4_result
ext4_file_size_valid(struct ext4_fs *fs, const struct ext4_inode *inode, uint64_t size)
{
	uint64_t per_block = fs->info.block_size / sizeof(struct ext4_le32);
	uint64_t limit = UINT32_MAX;
	uint64_t capacity;
	bool extents = (inode->flags & EXT4_INODE_EXTENTS) ||
	    ((inode->flags & EXT4_INODE_INLINE_DATA) &&
		(fs->info.feature_incompat & EXT4_FEATURE_INCOMPAT_EXTENTS));

	if (!extents) {
		limit = EXT4_DIRECT_BLOCKS + per_block + per_block * per_block +
		    per_block * per_block * per_block;
		if (limit > UINT32_MAX) {
			limit = UINT32_MAX;
		}
	}
	if (!(fs->info.feature_ro_compat & EXT4_FEATURE_RO_HUGE_FILE)) {
		capacity = UINT32_MAX / (fs->info.block_size / EXT4_SECTOR_SIZE);
		if (limit > capacity) {
			limit = capacity;
		}
		if (!extents && limit + ext4_indirect_overhead(limit, per_block) > capacity) {
			/* Match Linux's dense-map ceiling: reserve the indirect
			 * nodes for the full legacy block budget before data. */
			limit = capacity - ext4_indirect_overhead(capacity, per_block);
		}
	}
	if (size > limit * fs->info.block_size) {
		return EXT4_RANGE;
	}
	return size > INT32_MAX && !(fs->info.feature_ro_compat & EXT4_FEATURE_RO_LARGE_FILE)
	    ? EXT4_UNSUPPORTED
	    : EXT4_OK;
}

/* Change [within, within + length) of a file data block's snapshot to source bytes, or
 * to zeros when source is NULL. A fresh block has no earlier contents. An encrypted
 * file's snapshot holds ciphertext: it is decrypted unless fresh, changed and
 * encrypted again with the file's key and the block's logical number. */
static enum ext4_result
ext4_data_change(struct ext4_fs *fs, const struct ext4_inode *inode, uint32_t logical,
    uint8_t *snapshot, bool fresh, size_t within, const void *source, size_t length)
{
	struct ext4_fscrypt_key key;
	uint8_t *plain = NULL;
	uint8_t *target = snapshot;
	enum ext4_result error = EXT4_OK;

	if (inode->flags & EXT4_INODE_ENCRYPT) {
		error = ext4_fscrypt_key(fs, inode, &key);
		if (error != EXT4_OK) {
			return error;
		}
		plain = fs->environment.allocate(fs->environment.context, fs->info.block_size);
		if (plain == NULL) {
			return EXT4_NO_MEMORY;
		}
		target = plain;
		if (!fresh) {
			error = ext4_fscrypt_block(fs, &key, logical, false, snapshot, plain);
		}
	}
	if (error == EXT4_OK && fresh) {
		ext4_zero(target, fs->info.block_size);
	}
	if (error == EXT4_OK && source != NULL) {
		ext4_copy(target + within, source, length);
	} else if (error == EXT4_OK) {
		ext4_zero(target + within, length);
	}
	if (target != snapshot) {
		if (error == EXT4_OK) {
			error = ext4_fscrypt_block(fs, &key, logical, true, plain, snapshot);
		}
		fs->environment.release(fs->environment.context, plain, fs->info.block_size);
	}
	return error;
}

static enum ext4_result
ext4_write_snapshot(struct ext4_allocation *allocation, struct ext4_write_target *targets,
    uint32_t *count, uint32_t capacity, uint32_t logical, uint64_t physical, bool blank,
    void **snapshot)
{
	uint32_t index;
	enum ext4_result error;

	for (index = 0; index < *count; index++) {
		if (targets[index].physical == physical || targets[index].logical == logical) {
			if (targets[index].physical != physical ||
			    targets[index].logical != logical) {
				return EXT4_CORRUPT;
			}
			break;
		}
	}
	if (index == *count) {
		if (*count == capacity) {
			return EXT4_RANGE;
		}
		targets[index].physical = physical;
		targets[index].logical = logical;
		(*count)++;
	}
	error = ext4_allocation_valid(allocation, physical);
	if (error == EXT4_OK) {
		error = ext4_transaction_data(allocation->transaction, physical, blank, snapshot);
	}
	return error;
}

static enum ext4_result
ext4_write_gap(struct ext4_allocation *allocation, const struct ext4_inode *inode,
    const struct ext4_inode_disk *disk, uint64_t end, struct ext4_write_target *targets,
    uint32_t *count, uint32_t capacity, uint64_t *next)
{
	struct ext4_fs *fs = allocation->fs;
	struct ext4_map_run run;
	void *snapshot;
	uint64_t position = inode->size;
	uint64_t chunk;
	uint32_t logical;
	size_t within;
	enum ext4_result error;

	while (position < end) {
		/* A preparation step may stop after a bounded number of data blocks.
		 * Atomic callers omit next and retain the all-or-nothing limit. */
		if (next != NULL && *count == capacity) {
			break;
		}
		logical = (uint32_t)(position / fs->info.block_size);
		within = (size_t)(position % fs->info.block_size);
		error = ext4_write_map_lookup(allocation, inode, disk, logical, &run);
		if (error != EXT4_OK) {
			return error;
		}
		if (run.length == 0) {
			return EXT4_CORRUPT;
		}
		if (run.physical == 0 || run.unwritten) {
			chunk = run.length * fs->info.block_size - within;
		} else {
			chunk = fs->info.block_size - within;
		}
		if (chunk > end - position) {
			chunk = end - position;
		}
		if (run.physical != 0 && !run.unwritten) {
			error = ext4_write_snapshot(allocation, targets, count, capacity, logical,
			    run.physical, false, &snapshot);
			if (error == EXT4_OK) {
				error = ext4_data_change(fs, inode, logical, snapshot, false,
				    within, NULL, (size_t)chunk);
			}
			if (error != EXT4_OK) {
				return error;
			}
		}
		position += chunk;
	}
	if (next != NULL) {
		*next = position;
	}
	return EXT4_OK;
}

static enum ext4_result
ext4_growth_check(struct ext4_fs *fs, uint32_t number, uint32_t generation, uint64_t end,
    const struct ext4_inode_update *update, struct ext4_inode *result)
{
	struct ext4_fscrypt_key key;
	struct ext4_transaction *transaction;
	struct ext4_inode_disk *disk;
	struct ext4_inode inode;
	struct ext4_inode original;
	struct ext4_allocation allocation;
	bool allocation_ready = false;
	enum ext4_result error;

	/* Validate the complete ownership map and admitted transition before any
	 * hidden data changes. Attribute allocation and encoding remain private;
	 * the final size/data transaction applies the transition exactly once. */
	error =
	    ext4_transaction_begin(fs->journal, ext4_journal_credits(fs->journal), &transaction);
	if (error != EXT4_OK) {
		return error;
	}
	error = update->fields & EXT4_ATTR_XATTRS
	    ? ext4_edit_inode_record(fs, transaction, number, generation, &disk, &inode)
	    : ext4_edit_inode(fs, transaction, number, generation, &disk, &inode);
	if (error != EXT4_OK) {
		ext4_transaction_cancel(transaction);
		return error;
	}
	original = inode;
	if (error == EXT4_OK && (inode.mode & EXT4_MODE_TYPE) != EXT4_MODE_REGULAR) {
		error = (inode.mode & EXT4_MODE_TYPE) == EXT4_MODE_DIRECTORY ? EXT4_IS_DIRECTORY
									     : EXT4_UNSUPPORTED;
	}
	/* Growth preparation would zero Merkle metadata stored beyond EOF. Encrypted
	 * zeros need the file's key. */
	if (error == EXT4_OK && (inode.flags & EXT4_INODE_ENCRYPT)) {
		error = ext4_fscrypt_key(fs, &inode, &key);
	}
	if (error == EXT4_OK && (inode.flags & EXT4_INODE_VERITY)) {
		error = EXT4_PERMISSION_DENIED;
	}
	if (error == EXT4_OK) {
		error = ext4_file_size_valid(fs, &inode, end);
	}
	if (error == EXT4_OK) {
		error = ext4_update_admitted(fs, disk, update);
	}
	if (error == EXT4_OK) {
		error = ext4_inode_apply(fs, disk, update);
	}
	if (error == EXT4_OK) {
		error = ext4_allocation_init(&allocation, fs, transaction, &inode);
		allocation_ready = error == EXT4_OK;
	}
	if (error == EXT4_OK) {
		error = ext4_write_map_validate(&allocation, &inode, disk);
	}
	if (error == EXT4_OK && (update->fields & EXT4_ATTR_XATTRS)) {
		error = ext4_xattr_apply(
		    &allocation, &inode, disk, update->xattrs, update->xattr_count);
	}
	if (error == EXT4_OK) {
		error = ext4_inode_account(&allocation, &inode, disk, end);
	}
	if (error == EXT4_OK && inode.links == 0) {
		error = ext4_orphan_reserve(fs, &inode, disk);
	}
	if (allocation_ready) {
		ext4_allocation_destroy(&allocation);
	}
	ext4_transaction_cancel(transaction);
	if (error == EXT4_OK) {
		*result = original;
	}
	return error;
}

static enum ext4_result
ext4_growth_zero(struct ext4_fs *fs, uint32_t number, uint32_t generation, uint64_t old_size,
    uint64_t position, uint64_t end, struct ext4_write_target *targets, uint32_t credits,
    uint64_t *next)
{
	struct ext4_transaction *transaction;
	struct ext4_inode_disk *disk;
	struct ext4_inode inode;
	struct ext4_allocation allocation;
	uint32_t count = 0;
	bool allocation_ready = false;
	enum ext4_result error;

	error = ext4_transaction_begin(fs->journal, credits, &transaction);
	if (error != EXT4_OK) {
		return error;
	}
	error = ext4_edit_inode_record(fs, transaction, number, generation, &disk, &inode);
	if (error == EXT4_OK && inode.size != old_size) {
		error = EXT4_CORRUPT;
	}
	if (error == EXT4_OK) {
		error = ext4_allocation_init(&allocation, fs, transaction, &inode);
		allocation_ready = error == EXT4_OK;
	}
	if (error == EXT4_OK) {
		inode.size = position;
		error = ext4_write_gap(
		    &allocation, &inode, disk, end, targets, &count, credits - 1U, next);
	}
	if (allocation_ready) {
		ext4_allocation_destroy(&allocation);
	}
	if (error != EXT4_OK || count == 0) {
		ext4_transaction_cancel(transaction);
	} else {
		/* The inode snapshot is unchanged. Only bytes outside its visible size
		 * are zeroed; no allocation, attribute or orphan-list transition occurs. */
		error = ext4_edit_commit(fs, transaction);
	}
	return error;
}

static enum ext4_result
ext4_growth_clear(struct ext4_fs *fs, const struct ext4_inode *inode, uint64_t end)
{
	struct ext4_write_target *targets;
	uint32_t credits = ext4_journal_credits(fs->journal);
	uint64_t position;
	uint64_t next;
	enum ext4_result error = EXT4_OK;

	if (credits < 2) {
		return EXT4_RANGE;
	}
	if (end <= inode->size || (inode->flags & EXT4_INODE_INLINE_DATA)) {
		return EXT4_OK;
	}
	targets = fs->environment.allocate(
	    fs->environment.context, (size_t)(credits - 1U) * sizeof(*targets));
	if (targets == NULL) {
		return EXT4_NO_MEMORY;
	}
	position = inode->size;
	/* The exclusive owner retains a validated, unchanged mapping throughout.
	 * Preparatory commits can survive an error without exposing new bytes or
	 * applying the requested attributes. A later call can safely zero them again. */
	while (position < end) {
		error = ext4_growth_zero(fs, inode->number, inode->generation, inode->size,
		    position, end, targets, credits, &next);
		if (error != EXT4_OK) {
			break;
		}
		position = next;
	}
	fs->environment.release(
	    fs->environment.context, targets, (size_t)(credits - 1U) * sizeof(*targets));
	return error;
}

static enum ext4_result
ext4_growth_prepare(struct ext4_fs *fs, uint32_t number, uint32_t generation, uint64_t end,
    const struct ext4_inode_update *update)
{
	struct ext4_inode inode;
	enum ext4_result error;

	error = ext4_growth_check(fs, number, generation, end, update, &inode);
	return error == EXT4_OK ? ext4_growth_clear(fs, &inode, end) : error;
}

enum ext4_result
ext4_inode_account(struct ext4_allocation *allocation, const struct ext4_inode *inode,
    struct ext4_inode_disk *disk, uint64_t end)
{
	struct ext4_fs *fs = allocation->fs;
	uint64_t units = ext4_le32(&disk->blocks_lo);
	uint64_t increment = allocation->allocated + allocation->attribute_blocks_added;
	uint64_t decrement = allocation->freed + allocation->detached_shared_blocks +
	    allocation->attribute_blocks_removed;
	uint64_t maximum = UINT32_MAX;
	uint32_t flags = inode->flags;

	if (fs->info.feature_ro_compat & EXT4_FEATURE_RO_HUGE_FILE) {
		units |= (uint64_t)ext4_le16(&disk->blocks_hi) << 32;
		maximum = EXT4_PHYSICAL_BLOCK_MAX;
	}
	if (!(flags & EXT4_INODE_HUGE_FILE)) {
		increment *= fs->info.block_size / EXT4_SECTOR_SIZE;
		if (decrement > units / (fs->info.block_size / EXT4_SECTOR_SIZE)) {
			return EXT4_CORRUPT;
		}
		decrement *= fs->info.block_size / EXT4_SECTOR_SIZE;
	}
	if (decrement > units) {
		return EXT4_CORRUPT;
	}
	units -= decrement;
	if (increment > maximum - units) {
		if (!(fs->info.feature_ro_compat & EXT4_FEATURE_RO_HUGE_FILE) ||
		    (flags & EXT4_INODE_HUGE_FILE) ||
		    units % (fs->info.block_size / EXT4_SECTOR_SIZE) != 0) {
			return EXT4_RANGE;
		}
		units /= fs->info.block_size / EXT4_SECTOR_SIZE;
		increment = allocation->allocated + allocation->attribute_blocks_added;
		flags |= EXT4_INODE_HUGE_FILE;
	}
	units += increment;
	ext4_encode32(&disk->blocks_lo, (uint32_t)units);
	if (fs->info.feature_ro_compat & EXT4_FEATURE_RO_HUGE_FILE) {
		ext4_encode16(&disk->blocks_hi, (uint16_t)(units >> 32));
	}
	ext4_encode32(&disk->flags, flags);
	if (end > INT32_MAX && !(fs->info.feature_ro_compat & EXT4_FEATURE_RO_LARGE_FILE)) {
		return EXT4_UNSUPPORTED;
	}
	ext4_encode32(&disk->size_lo, (uint32_t)end);
	ext4_encode32(&disk->size_hi, (uint32_t)(end >> 32));
	return EXT4_OK;
}

static enum ext4_result
ext4_write_validate(struct ext4_fs *fs, uint64_t offset, const void *buffer, size_t length,
    const struct ext4_inode_update *update)
{
	enum ext4_result error;

	if (length != 0 && buffer == NULL) {
		return EXT4_INVALID_ARGUMENT;
	}
	error = ext4_update_validate(fs, update);
	if (error != EXT4_OK) {
		return error;
	}
	if ((update->fields & ~(uint32_t)EXT4_ATTR_XATTRS) != EXT4_WRITE_FIELDS) {
		return EXT4_INVALID_ARGUMENT;
	}
	if (offset > UINT64_MAX - length ||
	    (length != 0 && offset + length > (uint64_t)UINT32_MAX * fs->info.block_size)) {
		return EXT4_RANGE;
	}
	return EXT4_OK;
}

static enum ext4_result
ext4_unwritten_zero(struct ext4_fs *fs, uint32_t number, uint32_t generation,
    const struct ext4_unwritten_extent *range, uint32_t length, uint32_t position, uint32_t budget,
    bool initialize, uint32_t *next, bool *capacity_failed)
{
	struct ext4_transaction *transaction;
	struct ext4_inode_disk *disk;
	struct ext4_inode inode;
	struct ext4_allocation allocation;
	struct ext4_unwritten_extent observed;
	void *buffer;
	uint32_t end = length - position < budget ? length : position + budget;
	uint32_t index;
	bool allocation_ready = false;
	enum ext4_result error;

	*capacity_failed = false;
	error =
	    ext4_transaction_begin(fs->journal, ext4_journal_credits(fs->journal), &transaction);
	if (error != EXT4_OK) {
		return error;
	}
	error = ext4_edit_inode_record(fs, transaction, number, generation, &disk, &inode);
	if (error == EXT4_OK) {
		error = ext4_allocation_init(&allocation, fs, transaction, &inode);
		allocation_ready = error == EXT4_OK;
	}
	if (error == EXT4_OK) {
		error =
		    ext4_write_map_unwritten(&allocation, &inode, disk, range->logical, &observed);
	}
	if (error == EXT4_OK &&
	    (observed.logical != range->logical || observed.physical != range->physical ||
		observed.length != range->length)) {
		error = EXT4_CORRUPT;
	}
	for (index = position; error == EXT4_OK && index < end; index++) {
		error = ext4_transaction_data(transaction, range->physical + index, true, &buffer);
		if (error == EXT4_OK) {
			error = ext4_data_change(fs, &inode, range->logical + index, buffer, true,
			    0, NULL, fs->info.block_size);
		}
	}
	if (error == EXT4_OK && initialize && end == length) {
		error = ext4_write_map_initialize(&allocation, &inode, disk, range, length);
		if (error == EXT4_OK) {
			ext4_inode_checksum_set(fs, number, disk);
		}
	}
	*capacity_failed = ext4_transaction_capacity_failed(transaction);
	if (allocation_ready) {
		ext4_allocation_destroy(&allocation);
	}
	if (error != EXT4_OK) {
		ext4_transaction_cancel(transaction);
		return error;
	}
	/* A range crossing EOF stays unwritten even after its final zeroing commit.
	 * Only the subsequent data/size transaction can publish that conversion. */
	error = ext4_edit_commit(fs, transaction);
	if (error == EXT4_OK) {
		*next = end;
	}
	return error;
}

static enum ext4_result
ext4_unwritten_prepare(struct ext4_fs *fs, uint32_t number, uint32_t generation, uint32_t logical,
    uint64_t end, const struct ext4_inode_update *update, struct ext4_growth *growth,
    bool *prepared)
{
	struct ext4_transaction *transaction;
	struct ext4_inode_disk *disk;
	struct ext4_inode inode;
	struct ext4_allocation allocation;
	struct ext4_unwritten_extent ranges[2];
	uint64_t last;
	uint64_t blocks;
	uint32_t lengths[2] = { 0 };
	uint32_t count = 0;
	uint32_t index;
	uint32_t position;
	uint32_t budget = EXT4_ORPHAN_BATCH_BLOCKS;
	bool allocation_ready = false;
	bool capacity_failed;
	bool initialize = false;
	enum ext4_result error;

	*prepared = false;
	growth->deferred_count = 0;
	error =
	    ext4_transaction_begin(fs->journal, ext4_journal_credits(fs->journal), &transaction);
	if (error != EXT4_OK) {
		return error;
	}
	error = ext4_edit_inode_record(fs, transaction, number, generation, &disk, &inode);
	if (error == EXT4_OK) {
		error = ext4_allocation_init(&allocation, fs, transaction, &inode);
		allocation_ready = error == EXT4_OK;
	}
	if (error == EXT4_OK) {
		error = ext4_write_map_unwritten(&allocation, &inode, disk, logical, &ranges[0]);
	}
	if (error == EXT4_OK && ranges[0].length != 0) {
		count = 1;
		blocks = ((end > inode.size ? end : inode.size) + fs->info.block_size - 1U) /
			fs->info.block_size -
		    ranges[0].logical;
		lengths[0] = blocks < ranges[0].length ? (uint32_t)blocks : ranges[0].length;
		last = (uint64_t)ranges[0].logical + lengths[0] - 1U;
		initialize = last * fs->info.block_size < inode.size;
		/* Check existing leaf room before even hidden zeroing. A full imported
		 * tree without a spare record may still need a new mapping block. */
		error =
		    ext4_write_map_initialize(&allocation, &inode, disk, &ranges[0], lengths[0]);
		if (error == EXT4_NO_SPACE) {
			/* Advancing EOF into another extent must first merge away the old
			 * initialized/unwritten boundary, which may own the spare slot. */
			ranges[1] = ranges[0];
			lengths[1] = lengths[0];
			error = ext4_write_map_mergeable(&allocation, &inode, disk, logical,
			    end > inode.size ? end : inode.size, &ranges[0]);
			if (error == EXT4_OK && ranges[0].length == 0) {
				error = EXT4_NO_SPACE;
			}
			if (error == EXT4_OK) {
				lengths[0] = ranges[0].length;
				error = ext4_write_map_initialize(
				    &allocation, &inode, disk, &ranges[0], lengths[0]);
			}
			if (error == EXT4_OK) {
				error = ext4_write_map_initialize(
				    &allocation, &inode, disk, &ranges[1], lengths[1]);
			}
			count = 2;
			initialize = false;
		}
	}
	if (allocation_ready) {
		ext4_allocation_destroy(&allocation);
	}
	ext4_transaction_cancel(transaction);
	if (error == EXT4_NO_SPACE) {
		return EXT4_OK;
	}
	if (error != EXT4_OK || count == 0) {
		return error;
	}
	/* Validate all physical ownership and the admitted attribute transition
	 * before writing even inaccessible backing bytes. No other owner can change
	 * the extent while bounded zeroing checkpoints retain this local range. */
	error = ext4_growth_check(fs, number, generation, end, update, &inode);
	if (error != EXT4_OK) {
		return error;
	}
	for (index = 0; index < count; index++) {
		position = 0;
		while (position < lengths[index]) {
			error = ext4_unwritten_zero(fs, number, generation, &ranges[index],
			    lengths[index], position, budget, initialize, &position,
			    &capacity_failed);
			if (error == EXT4_RANGE && capacity_failed && !fs->aborted && budget > 1) {
				budget /= 2;
				continue;
			}
			if (error != EXT4_OK) {
				return error;
			}
		}
	}
	if (!initialize) {
		growth->deferred_count = count;
		for (index = 0; index < count; index++) {
			growth->deferred[index] = ranges[index];
			growth->initialized[index] = lengths[index];
		}
	}
	*prepared = true;
	return EXT4_OK;
}

static enum ext4_result
ext4_write_atomic(struct ext4_fs *fs, uint32_t number, uint32_t generation, uint64_t offset,
    const void *buffer, size_t length, const struct ext4_inode_update *update, size_t *completed,
    struct ext4_growth *growth)
{
	struct ext4_fscrypt_key key;
	struct ext4_transaction *transaction;
	struct ext4_inode_disk *disk;
	struct ext4_inode inode;
	struct ext4_inode zero_from;
	struct ext4_allocation allocation;
	struct ext4_write_target *targets = NULL;
	void *snapshot;
	uint64_t logical;
	uint64_t block_count;
	uint64_t initialized;
	uint64_t physical;
	uint32_t feature_compat;
	uint32_t credits;
	uint32_t index;
	uint32_t target_count = 0;
	size_t within;
	size_t chunk;
	size_t consumed = 0;
	bool allocation_ready = false;
	bool zero;
	bool inline_done;
	enum ext4_result error;

	if (completed == NULL) {
		return EXT4_INVALID_ARGUMENT;
	}
	*completed = 0;
	if (growth != NULL) {
		growth->capacity_failed = false;
		growth->zeroing = false;
		growth->mapping_no_space = false;
	}
	error = ext4_write_validate(fs, offset, buffer, length, update);
	if (error != EXT4_OK) {
		return error;
	}
	within = (size_t)(offset % fs->info.block_size);
	block_count = length == 0 ? 0
				  : length / fs->info.block_size +
		(within + length % fs->info.block_size + fs->info.block_size - 1) /
		    fs->info.block_size;
	if (block_count >= EXT4_TRANSACTION_MAX_BLOCKS) {
		return EXT4_RANGE;
	}
	credits = ext4_journal_credits(fs->journal);
	if (credits == 0 || block_count >= credits) {
		return EXT4_RANGE;
	}
	error = ext4_transaction_begin(fs->journal, credits, &transaction);
	if (error != EXT4_OK) {
		return error;
	}
	error = update->fields & EXT4_ATTR_XATTRS
	    ? ext4_edit_inode_record(fs, transaction, number, generation, &disk, &inode)
	    : ext4_edit_inode(fs, transaction, number, generation, &disk, &inode);
	if (error != EXT4_OK) {
		goto cancel;
	}
	if ((inode.mode & EXT4_MODE_TYPE) != EXT4_MODE_REGULAR) {
		error = (inode.mode & EXT4_MODE_TYPE) == EXT4_MODE_DIRECTORY ? EXT4_IS_DIRECTORY
									     : EXT4_UNSUPPORTED;
		goto cancel;
	}
	/* Encrypted contents need the file's key; they never stay in the inode. */
	if (inode.flags & EXT4_INODE_ENCRYPT) {
		error = inode.flags & EXT4_INODE_INLINE_DATA ? EXT4_CORRUPT
							     : ext4_fscrypt_key(fs, &inode, &key);
		if (error != EXT4_OK) {
			goto cancel;
		}
	}
	if ((inode.flags & EXT4_INODE_DATA_PROTECTED) ||
	    ((inode.flags & EXT4_INODE_APPEND) && offset != inode.size)) {
		error = EXT4_PERMISSION_DENIED;
		goto cancel;
	}
	if (length == 0) {
		goto cancel;
	}
	error = ext4_file_size_valid(fs, &inode, offset + length);
	if (error == EXT4_OK) {
		error = ext4_update_admitted(fs, disk, update);
	}
	if (error == EXT4_OK) {
		error = ext4_inode_apply(fs, disk, update);
	}
	if (error != EXT4_OK) {
		goto cancel;
	}
	targets =
	    fs->environment.allocate(fs->environment.context, (size_t)credits * sizeof(*targets));
	if (targets == NULL) {
		error = EXT4_NO_MEMORY;
		goto cancel;
	}
	error = ext4_allocation_init(&allocation, fs, transaction, &inode);
	if (error != EXT4_OK) {
		goto cancel;
	}
	allocation_ready = true;
	error = ext4_inline_edit(&allocation, &inode, disk,
	    offset + length > inode.size ? offset + length : inode.size, offset, buffer, length,
	    &inline_done);
	if (error != EXT4_OK) {
		goto cancel;
	}
	if (inline_done) {
		goto attributes;
	}
	allocation.mapping_size = offset + length;
	if ((update->fields & EXT4_ATTR_XATTRS) || ext4_inode_has_xattrs(fs, disk)) {
		error = ext4_write_map_validate(&allocation, &inode, disk);
		if (error != EXT4_OK) {
			goto cancel;
		}
	}
	if (offset > inode.size) {
		zero_from = inode;
		if (growth != NULL && growth->zeroed > zero_from.size) {
			zero_from.size = growth->zeroed;
		}
		error = ext4_write_gap(
		    &allocation, &zero_from, disk, offset, targets, &target_count, credits, NULL);
		if (growth != NULL) {
			growth->zeroing = target_count != 0;
		}
		if (error != EXT4_OK) {
			goto cancel;
		}
	}
	for (index = 0; growth != NULL && index < growth->deferred_count; index++) {
		/* Zeroing was durable while the map still returned zeros. Initialize the
		 * prepared prefix privately, after gap handling and before copying data;
		 * its new EOF and admitted attributes must commit in this transaction. */
		initialized = ((offset + length > inode.size ? offset + length : inode.size) +
				  fs->info.block_size - 1U) /
		    fs->info.block_size;
		if (initialized <= growth->deferred[index].logical) {
			break;
		}
		initialized -= growth->deferred[index].logical;
		if (initialized > growth->initialized[index]) {
			initialized = growth->initialized[index];
		}
		error = ext4_write_map_initialize(
		    &allocation, &inode, disk, &growth->deferred[index], (uint32_t)initialized);
		if (error != EXT4_OK) {
			goto cancel;
		}
	}
	logical = offset / fs->info.block_size;
	for (index = 0; index < block_count; index++) {
		allocation.run_clusters =
		    (uint32_t)((block_count - index + fs->cluster_blocks - 1U) /
			fs->cluster_blocks);
		error = ext4_write_map_allocate(
		    &allocation, &inode, disk, (uint32_t)(logical + index), &physical, &zero);
		allocation.run_clusters = 0;
		if (error != EXT4_OK) {
			if (growth != NULL && error == EXT4_NO_SPACE) {
				growth->mapping_no_space = true;
				growth->allocation_logical = (uint32_t)(logical + index);
			}
			goto cancel;
		}
		chunk = fs->info.block_size - within;
		if (chunk > length - consumed) {
			chunk = length - consumed;
		}
		/* A new or completely overwritten block needs no previous contents. */
		error = ext4_write_snapshot(&allocation, targets, &target_count, credits,
		    (uint32_t)(logical + index), physical, zero || chunk == fs->info.block_size,
		    &snapshot);
		if (error != EXT4_OK) {
			goto cancel;
		}
		error = ext4_data_change(fs, &inode, (uint32_t)(logical + index), snapshot,
		    zero || chunk == fs->info.block_size, within,
		    (const uint8_t *)buffer + consumed, chunk);
		if (error != EXT4_OK) {
			goto cancel;
		}
		consumed += chunk;
		within = 0;
	}
attributes:
	if (update->fields & EXT4_ATTR_XATTRS) {
		error = ext4_xattr_apply(
		    &allocation, &inode, disk, update->xattrs, update->xattr_count);
	}
	if (error == EXT4_OK) {
		error = ext4_inode_account(&allocation, &inode, disk,
		    offset + length > inode.size ? offset + length : inode.size);
	}
	if (error == EXT4_OK && inode.links == 0) {
		error = ext4_orphan_reserve(fs, &inode, disk);
	}
	if (error != EXT4_OK) {
		goto cancel;
	}
	ext4_inode_checksum_set(fs, number, disk);
	feature_compat = allocation.super == NULL ? fs->info.feature_compat
						  : ext4_le32(&allocation.super->feature_compat);
	ext4_allocation_destroy(&allocation);
	fs->environment.release(
	    fs->environment.context, targets, (size_t)credits * sizeof(*targets));
	error = ext4_edit_commit(fs, transaction);
	if (error == EXT4_OK) {
		fs->info.feature_compat = feature_compat;
		*completed = length;
	}
	return error;
cancel:
	if (growth != NULL) {
		growth->capacity_failed = ext4_transaction_capacity_failed(transaction);
	}
	if (allocation_ready) {
		ext4_allocation_destroy(&allocation);
	}
	if (targets != NULL) {
		fs->environment.release(
		    fs->environment.context, targets, (size_t)credits * sizeof(*targets));
	}
	ext4_transaction_cancel(transaction);
	return error;
}

enum ext4_result
ext4_write(struct ext4_fs *fs, uint32_t number, uint32_t generation, uint64_t offset,
    const void *buffer, size_t length, const struct ext4_inode_update *update, size_t *completed)
{
	return ext4_write_atomic(
	    fs, number, generation, offset, buffer, length, update, completed, NULL);
}

enum ext4_result
ext4_write_partial(struct ext4_fs *fs, uint32_t number, uint32_t generation, uint64_t offset,
    const void *buffer, size_t length, const struct ext4_inode_update *update, size_t *completed)
{
	struct ext4_inode_update remaining;
	struct ext4_growth growth = { 0 };
	const struct ext4_unwritten_extent *last;
	uint64_t available;
	uint32_t credits;
	uint32_t blocks;
	size_t limit;
	size_t within;
	size_t chunk;
	size_t written;
	bool prepared;
	enum ext4_result error;

	if (completed == NULL) {
		return EXT4_INVALID_ARGUMENT;
	}
	*completed = 0;
	/* Validate the whole request before committing any prefix. Each transaction
	 * subsequently resolves and validates the live inode under the same owner. */
	error = ext4_write_validate(fs, offset, buffer, length, update);
	if (error != EXT4_OK) {
		return error;
	}
	if (length == 0) {
		return ext4_write(
		    fs, number, generation, offset, buffer, length, update, completed);
	}
	credits = ext4_journal_credits(fs->journal);
	if (credits < 2) {
		return EXT4_RANGE;
	}
	remaining = *update;
	limit = (size_t)(credits - 1U) * fs->info.block_size;
	while (*completed < length) {
		within = (size_t)((offset + *completed) % fs->info.block_size);
		chunk = limit - within;
		if (chunk > length - *completed) {
			chunk = length - *completed;
		}
		error = ext4_write_atomic(fs, number, generation, offset + *completed,
		    (const uint8_t *)buffer + *completed, chunk, &remaining, &written, &growth);
		if (error == EXT4_OK) {
			*completed += written;
			growth.deferred_count = 0;
			/* The first durable data prefix owns the admitted attribute change.
			 * Later transactions preserve that result, including non-idempotent
			 * CREATE/REMOVE operations and security attribute removal. */
			remaining.xattrs = NULL;
			remaining.xattr_count = 0;
			continue;
		}
		if (fs->aborted ||
		    !((error == EXT4_RANGE && growth.capacity_failed) || error == EXT4_NO_SPACE ||
			error == EXT4_QUOTA_EXCEEDED)) {
			return error;
		}
		if (error == EXT4_RANGE && growth.zeroing) {
			error = ext4_growth_prepare(fs, number, generation, offset, &remaining);
			if (error != EXT4_OK) {
				return error;
			}
			growth.zeroed = offset;
			continue;
		}
		if (error == EXT4_NO_SPACE && growth.mapping_no_space) {
			error = ext4_unwritten_prepare(fs, number, generation,
			    growth.allocation_logical, offset + *completed + chunk, &remaining,
			    &growth, &prepared);
			if (error != EXT4_OK) {
				return error;
			}
			if (prepared) {
				if (growth.deferred_count != 0) {
					/* Stop at the prepared extent so a later allocation
					 * shortage cannot discard this checkpoint's conversion. */
					last = &growth.deferred[growth.deferred_count - 1U];
					available =
					    ((uint64_t)last->logical + last->length -
						(offset + *completed) / fs->info.block_size) *
					    fs->info.block_size;
					if (available < limit) {
						limit = (size_t)available;
					}
				}
				continue;
			}
			error = EXT4_NO_SPACE;
		}
		blocks =
		    (uint32_t)((within + chunk + fs->info.block_size - 1U) / fs->info.block_size);
		if (blocks <= 1) {
			return error;
		}
		/* Cancelled private snapshots wrote nothing. Reduce only a proven
		 * credit, allocation or quota shortage, never an I/O/format error.
		 * Keep the smaller bound for subsequent batches of this request. */
		limit = (size_t)(blocks / 2U) * fs->info.block_size;
	}
	return EXT4_OK;
}

/* One data block can release two extent paths, with a bitmap and descriptor
 * per released block, plus the inode, superblock and retained data tail. Small
 * journals keep the atomic contract rather than admitting an intent whose
 * later path might not fit even the minimum cleanup batch. */
#define EXT4_TRUNCATE_RECOVERY_CREDITS (3U * (2U * EXT4_EXTENT_MAX_DEPTH + 1U) + 4U)

static enum ext4_result
ext4_truncate_start(struct ext4_fs *fs, uint32_t number, uint32_t generation, uint64_t size,
    const struct ext4_inode_update *update, struct ext4_inode *result, uint32_t batch, bool *retry,
    bool *pending, struct ext4_growth *growth)
{
	struct ext4_fscrypt_key key;
	struct ext4_transaction *transaction;
	struct ext4_inode_disk *disk;
	struct ext4_inode inode;
	struct ext4_inode zero_from;
	struct ext4_allocation allocation;
	struct ext4_write_target *targets = NULL;
	uint64_t end;
	uint32_t first;
	uint32_t target_count = 0;
	uint32_t credits;
	uint32_t feature_compat;
	bool allocation_ready = false;
	bool done = true;
	bool inline_done;
	enum ext4_result error;

	*retry = false;
	*pending = false;
	if (growth != NULL) {
		growth->capacity_failed = false;
		growth->zeroing = false;
	}
	if (result == NULL) {
		return EXT4_INVALID_ARGUMENT;
	}
	error = ext4_update_validate(fs, update);
	if (error != EXT4_OK) {
		return error;
	}
	if ((update->fields & ~(uint32_t)EXT4_ATTR_XATTRS) != EXT4_WRITE_FIELDS) {
		return EXT4_INVALID_ARGUMENT;
	}
	if (size > (uint64_t)UINT32_MAX * fs->info.block_size) {
		return EXT4_RANGE;
	}
	credits = ext4_journal_credits(fs->journal);
	if (credits < EXT4_TRUNCATE_RECOVERY_CREDITS) {
		batch = 0;
	}
	error = ext4_transaction_begin(fs->journal, credits, &transaction);
	if (error != EXT4_OK) {
		return error;
	}
	error = update->fields & EXT4_ATTR_XATTRS
	    ? ext4_edit_inode_record(fs, transaction, number, generation, &disk, &inode)
	    : ext4_edit_inode(fs, transaction, number, generation, &disk, &inode);
	if (error != EXT4_OK) {
		goto cancel;
	}
	if ((inode.mode & EXT4_MODE_TYPE) != EXT4_MODE_REGULAR) {
		error = (inode.mode & EXT4_MODE_TYPE) == EXT4_MODE_DIRECTORY ? EXT4_IS_DIRECTORY
									     : EXT4_UNSUPPORTED;
		goto cancel;
	}
	if (inode.flags & EXT4_INODE_ENCRYPT) {
		error = inode.flags & EXT4_INODE_INLINE_DATA ? EXT4_CORRUPT
							     : ext4_fscrypt_key(fs, &inode, &key);
		if (error != EXT4_OK) {
			goto cancel;
		}
	}
	if (inode.flags & (EXT4_INODE_RESTRICTED_FLAGS | EXT4_INODE_VERITY)) {
		error = EXT4_PERMISSION_DENIED;
		goto cancel;
	}
	error = ext4_file_size_valid(fs, &inode, size);
	if (error == EXT4_OK) {
		error = ext4_update_admitted(fs, disk, update);
	}
	if (error == EXT4_OK) {
		error = ext4_inode_apply(fs, disk, update);
	}
	if (error != EXT4_OK) {
		goto cancel;
	}
	error = ext4_allocation_init(&allocation, fs, transaction, &inode);
	if (error != EXT4_OK) {
		goto cancel;
	}
	allocation_ready = true;
	error = ext4_inline_edit(&allocation, &inode, disk, size, 0, NULL, 0, &inline_done);
	if (error != EXT4_OK) {
		goto cancel;
	}
	if (inline_done) {
		goto attributes;
	}
	if (size > inode.size &&
	    ((update->fields & EXT4_ATTR_XATTRS) || ext4_inode_has_xattrs(fs, disk))) {
		error = ext4_write_map_validate(&allocation, &inode, disk);
		if (error != EXT4_OK) {
			goto cancel;
		}
	}
	first = (uint32_t)((size + fs->info.block_size - 1) / fs->info.block_size);
	if (size <= inode.size) {
		if (batch == 0) {
			error = ext4_write_map_truncate(&allocation, &inode, disk, first);
		} else {
			error = ext4_allocation_super(&allocation);
			if (error == EXT4_OK &&
			    ext4_le32(&allocation.super->last_orphan) != fs->last_orphan) {
				error = EXT4_CORRUPT;
			}
			if (error == EXT4_OK) {
				error = ext4_write_map_validate(&allocation, &inode, disk);
			}
			if (error == EXT4_OK) {
				*retry = batch > 1;
				error = ext4_write_map_trim(
				    &allocation, &inode, disk, first, batch, &done);
			}
			if (error == EXT4_OK && !done) {
				if (allocation.freed == 0) {
					error = EXT4_CORRUPT;
				} else if (inode.links != 0) {
					ext4_encode32(&disk->deletion_time, fs->last_orphan);
					ext4_encode32(&allocation.super->last_orphan, number);
				}
			}
		}
		if (error != EXT4_OK) {
			goto cancel;
		}
	}
	targets =
	    fs->environment.allocate(fs->environment.context, (size_t)credits * sizeof(*targets));
	if (targets == NULL) {
		error = EXT4_NO_MEMORY;
		goto cancel;
	}
	zero_from = inode;
	if (size <= inode.size) {
		zero_from.size = size;
		end = (uint64_t)first * fs->info.block_size;
	} else {
		end = size;
		if (growth != NULL && growth->zeroed > zero_from.size) {
			zero_from.size = growth->zeroed;
		}
	}
	error = ext4_write_gap(
	    &allocation, &zero_from, disk, end, targets, &target_count, credits, NULL);
	if (growth != NULL && size > inode.size) {
		growth->zeroing = target_count != 0;
	}
attributes:
	if (error == EXT4_OK && (update->fields & EXT4_ATTR_XATTRS)) {
		error = ext4_xattr_apply(
		    &allocation, &inode, disk, update->xattrs, update->xattr_count);
	}
	if (error == EXT4_OK) {
		error = ext4_inode_account(&allocation, &inode, disk, size);
	}
	if (error == EXT4_OK && inode.links == 0) {
		error = ext4_orphan_reserve(fs, &inode, disk);
	}
	if (error == EXT4_OK) {
		ext4_inode_checksum_set(fs, number, disk);
		error = ext4_inode_decode_live(fs, number, disk, &inode);
	}
	if (error != EXT4_OK) {
		goto cancel;
	}
	feature_compat = allocation.super == NULL ? fs->info.feature_compat
						  : ext4_le32(&allocation.super->feature_compat);
	ext4_allocation_destroy(&allocation);
	if (targets != NULL) {
		fs->environment.release(
		    fs->environment.context, targets, (size_t)credits * sizeof(*targets));
	}
	error = ext4_edit_commit(fs, transaction);
	if (error == EXT4_OK) {
		fs->info.feature_compat = feature_compat;
		if (!done && inode.links != 0) {
			fs->last_orphan = number;
		}
		*pending = !done;
		*result = inode;
	}
	return error;
cancel:
	if (growth != NULL) {
		growth->capacity_failed = ext4_transaction_capacity_failed(transaction);
	}
	if (allocation_ready) {
		ext4_allocation_destroy(&allocation);
	}
	if (targets != NULL) {
		fs->environment.release(
		    fs->environment.context, targets, (size_t)credits * sizeof(*targets));
	}
	ext4_transaction_cancel(transaction);
	return error;
}

enum ext4_result
ext4_truncate_atomic(struct ext4_fs *fs, uint32_t number, uint32_t generation, uint64_t size,
    const struct ext4_inode_update *update, struct ext4_inode *result)
{
	bool retry;
	bool pending;

	return ext4_truncate_start(
	    fs, number, generation, size, update, result, 0, &retry, &pending, NULL);
}

enum ext4_result
ext4_truncate(struct ext4_fs *fs, uint32_t number, uint32_t generation, uint64_t size,
    const struct ext4_inode_update *update, struct ext4_inode *result)
{
	struct ext4_inode inode;
	struct ext4_inode_hold *hold;
	struct ext4_growth growth = { 0 };
	uint32_t batch = EXT4_ORPHAN_BATCH_BLOCKS;
	bool retry;
	bool pending;
	enum ext4_result error;

	if (result == NULL) {
		return EXT4_INVALID_ARGUMENT;
	}
	for (;;) {
		error = ext4_truncate_start(
		    fs, number, generation, size, update, &inode, batch, &retry, &pending, &growth);
		if (error == EXT4_RANGE && growth.capacity_failed && growth.zeroing &&
		    !fs->aborted) {
			error = ext4_growth_prepare(fs, number, generation, size, update);
			if (error != EXT4_OK) {
				return error;
			}
			growth.zeroed = size;
			continue;
		}
		if (error != EXT4_RANGE || !retry || fs->aborted) {
			break;
		}
		batch /= 2;
	}
	if (error != EXT4_OK) {
		return error;
	}
	if (pending) {
		error = ext4_orphan_finish_inode(fs, number, generation, inode.links == 0);
		if (error == EXT4_OK) {
			hold = ext4_inode_find_hold(fs, number);
			error = hold == NULL ? ext4_get_inode(fs, number, &inode)
					     : ext4_refresh_inode(hold, &inode);
		}
		if (error != EXT4_OK) {
			/* Even a private allocation/read failure now follows a committed
			 * intent. Prevent every further access until offline recovery. */
			fs->aborted = true;
			return error;
		}
	}
	*result = inode;
	return EXT4_OK;
}

static enum ext4_result
ext4_file_range_step(struct ext4_fs *fs, uint32_t number, uint32_t generation, uint64_t offset,
    uint64_t end, uint32_t flags, const struct ext4_inode_update *update, uint32_t budget,
    uint64_t *next, bool *capacity_failed)
{
	struct ext4_transaction *transaction;
	struct ext4_allocation allocation;
	struct ext4_inode_disk *disk;
	struct ext4_inode inode;
	struct ext4_map_run run;
	void *buffer;
	uint64_t position = offset;
	uint64_t run_end;
	uint64_t amount;
	uint64_t size;
	uint32_t feature_compat;
	uint32_t logical;
	uint32_t within;
	uint32_t blocks;
	uint32_t work = 0;
	bool allocation_ready = false;
	bool inline_done;
	bool punch = (flags & EXT4_FALLOC_PUNCH_HOLE) != 0;
	enum ext4_result error;

	*capacity_failed = false;
	error =
	    ext4_transaction_begin(fs->journal, ext4_journal_credits(fs->journal), &transaction);
	if (error != EXT4_OK) {
		return error;
	}
	error = update->fields & EXT4_ATTR_XATTRS
	    ? ext4_edit_inode_record(fs, transaction, number, generation, &disk, &inode)
	    : ext4_edit_inode(fs, transaction, number, generation, &disk, &inode);
	if (error == EXT4_OK) {
		error = ext4_update_admitted(fs, disk, update);
	}
	if (error == EXT4_OK) {
		error = ext4_inode_apply(fs, disk, update);
	}
	if (error == EXT4_OK) {
		error = ext4_allocation_init(&allocation, fs, transaction, &inode);
		allocation_ready = error == EXT4_OK;
	}
	if (error != EXT4_OK) {
		goto cancel;
	}
	if (inode.flags & EXT4_INODE_INLINE_DATA) {
		if (punch) {
			amount = position >= inode.size ? 0 : inode.size - position;
			if (amount > end - position) {
				amount = end - position;
			}
			error = ext4_inline_edit(&allocation, &inode, disk, inode.size,
			    amount == 0 ? 0 : position, NULL, (size_t)amount, &inline_done);
			position = end;
		} else {
			error = ext4_inline_expand(&allocation, &inode, disk);
		}
		if (error != EXT4_OK) {
			goto cancel;
		}
	}
	/* The public operation validated the complete map before its first step.
	 * Its exclusive owner prevents any other mutation between checkpoints. */
	while (position < end && work < budget) {
		logical = (uint32_t)(position / fs->info.block_size);
		within = (uint32_t)(position % fs->info.block_size);
		amount = fs->info.block_size - within;
		if (amount > end - position) {
			amount = end - position;
		}
		error = ext4_write_map_lookup(&allocation, &inode, disk, logical, &run);
		if (error != EXT4_OK) {
			goto cancel;
		}
		if ((punch && run.physical == 0) || (!punch && run.physical != 0)) {
			run_end = ((uint64_t)logical + run.length) * fs->info.block_size;
			if (!(flags & EXT4_FALLOC_KEEP_SIZE)) {
				allocation.mapping_size = run_end < end ? run_end : end;
			}
			if (!punch && (inode.flags & EXT4_INODE_EXTENTS)) {
				error = ext4_write_map_reserve_capacity(
				    &allocation, &inode, disk, logical);
				if (error != EXT4_OK) {
					goto cancel;
				}
				work++;
			}
			position = run_end < end ? run_end : end;
			continue;
		}
		if (punch && within == 0 && amount == fs->info.block_size) {
			amount = (end - position) / fs->info.block_size;
			if (amount > run.length) {
				amount = run.length;
			}
			if (amount > budget - work) {
				amount = budget - work;
			}
			blocks = (uint32_t)amount;
			error = ext4_write_map_punch(&allocation, &inode, disk, logical, blocks);
			work += blocks;
			amount *= fs->info.block_size;
		} else if (punch) {
			if (!run.unwritten) {
				error = ext4_transaction_data(
				    transaction, run.physical, false, &buffer);
				if (error == EXT4_OK) {
					error = ext4_data_change(fs, &inode, logical, buffer, false,
					    within, NULL, (size_t)amount);
				}
				work++;
			}
		} else {
			/* Only this checkpoint's durable prefix may release reserved
			 * mapping capacity; later work can still run out of space. */
			if (!(flags & EXT4_FALLOC_KEEP_SIZE)) {
				allocation.mapping_size = position + amount;
			}
			error = ext4_write_map_reserve(&allocation, &inode, disk, logical);
			work++;
		}
		if (error != EXT4_OK) {
			goto cancel;
		}
		position += amount;
	}
	if (update->fields & EXT4_ATTR_XATTRS) {
		error = ext4_xattr_apply(
		    &allocation, &inode, disk, update->xattrs, update->xattr_count);
	}
	size = inode.size;
	if (!(flags & EXT4_FALLOC_KEEP_SIZE) && position > size) {
		size = position;
	}
	if (error == EXT4_OK) {
		error = ext4_inode_account(&allocation, &inode, disk, size);
	}
	if (error == EXT4_OK && inode.links == 0) {
		error = ext4_orphan_reserve(fs, &inode, disk);
	}
	if (error != EXT4_OK) {
		goto cancel;
	}
	ext4_inode_checksum_set(fs, number, disk);
	feature_compat = allocation.super == NULL ? fs->info.feature_compat
						  : ext4_le32(&allocation.super->feature_compat);
	ext4_allocation_destroy(&allocation);
	error = ext4_edit_commit(fs, transaction);
	if (error == EXT4_OK) {
		fs->info.feature_compat = feature_compat;
		*next = position;
	}
	return error;
cancel:
	*capacity_failed = ext4_transaction_capacity_failed(transaction);
	if (allocation_ready) {
		ext4_allocation_destroy(&allocation);
	}
	ext4_transaction_cancel(transaction);
	return error;
}

enum ext4_result
ext4_fallocate(struct ext4_fs *fs, uint32_t number, uint32_t generation, uint64_t offset,
    uint64_t length, uint32_t flags, const struct ext4_inode_update *update, uint64_t *completed)
{
	struct ext4_inode inode;
	struct ext4_inode_update remaining;
	uint64_t end;
	uint64_t next;
	uint32_t budget = EXT4_ORPHAN_BATCH_BLOCKS;
	bool capacity_failed;
	enum ext4_result error;

	if (completed == NULL) {
		return EXT4_INVALID_ARGUMENT;
	}
	*completed = 0;
	if (length == 0 || (flags & ~(uint32_t)(EXT4_FALLOC_KEEP_SIZE | EXT4_FALLOC_PUNCH_HOLE)) ||
	    ((flags & EXT4_FALLOC_PUNCH_HOLE) && !(flags & EXT4_FALLOC_KEEP_SIZE))) {
		return EXT4_INVALID_ARGUMENT;
	}
	error = ext4_update_validate(fs, update);
	if (error != EXT4_OK) {
		return error;
	}
	if ((update->fields & ~(uint32_t)EXT4_ATTR_XATTRS) != EXT4_WRITE_FIELDS) {
		return EXT4_INVALID_ARGUMENT;
	}
	if (offset > UINT64_MAX - length) {
		return EXT4_RANGE;
	}
	end = offset + length;
	error = ext4_growth_check(fs, number, generation, end, update, &inode);
	if (error == EXT4_OK &&
	    ((inode.flags & EXT4_INODE_DATA_PROTECTED) ||
		((inode.flags & EXT4_INODE_APPEND) && (flags & EXT4_FALLOC_PUNCH_HOLE)))) {
		error = EXT4_PERMISSION_DENIED;
	}
	if (error == EXT4_OK && !(flags & EXT4_FALLOC_PUNCH_HOLE) &&
	    !(inode.flags & EXT4_INODE_EXTENTS) &&
	    !((inode.flags & EXT4_INODE_INLINE_DATA) &&
		(fs->info.feature_incompat & EXT4_FEATURE_INCOMPAT_EXTENTS))) {
		error = EXT4_UNSUPPORTED;
	}
	if (error == EXT4_OK && !(flags & EXT4_FALLOC_KEEP_SIZE)) {
		error = ext4_growth_clear(fs, &inode, end);
	}
	if (error != EXT4_OK) {
		return error;
	}
	remaining = *update;
	while (*completed < length) {
		error = ext4_file_range_step(fs, number, generation, offset + *completed, end,
		    flags, &remaining, budget, &next, &capacity_failed);
		if (error == EXT4_OK) {
			*completed = next - offset;
			remaining.xattrs = NULL;
			remaining.xattr_count = 0;
			continue;
		}
		if (fs->aborted || budget == 1 ||
		    !((error == EXT4_RANGE && capacity_failed) || error == EXT4_NO_SPACE)) {
			return error;
		}
		budget /= 2;
	}
	return EXT4_OK;
}

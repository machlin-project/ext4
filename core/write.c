/* SPDX-License-Identifier: BSD-3-Clause */
#include "allocate.h"

#define EXT4_ATTRIBUTE_FIELDS                                                                      \
	((uint32_t)(EXT4_ATTR_PERMISSIONS | EXT4_ATTR_UID | EXT4_ATTR_GID |                        \
	    EXT4_ATTR_ACCESS_TIME | EXT4_ATTR_CHANGE_TIME | EXT4_ATTR_MODIFY_TIME |                \
	    EXT4_ATTR_BIRTH_TIME))
#define EXT4_WRITE_FIELDS (EXT4_ATTR_PERMISSIONS | EXT4_ATTR_CHANGE_TIME | EXT4_ATTR_MODIFY_TIME)

enum ext4_result
ext4_mount_writable(const struct ext4_environment *environment,
    const struct ext4_write_environment *writer, struct ext4_fs **result)
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
	    fs->blocks_per_group % EXT4_BITS_PER_BYTE != 0 ||
	    fs->inodes_per_group % EXT4_BITS_PER_BYTE != 0) {
		error = EXT4_CORRUPT;
	} else {
		error = ext4_journal_open(fs, writer, &fs->journal);
	}
	if (error == EXT4_OK) {
		error = ext4_system_ranges_build(fs);
	}
	if (error != EXT4_OK) {
		ext4_unmount(fs);
		return error;
	}
	*result = fs;
	return EXT4_OK;
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
	error = ext4_journal_finish(fs->journal);
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
	return EXT4_OK;
}

static enum ext4_result
ext4_edit_inode(struct ext4_fs *fs, struct ext4_transaction *transaction, uint32_t number,
    uint32_t generation, struct ext4_inode_disk **disk, struct ext4_inode *inode)
{
	void *buffer;
	uint64_t offset;
	enum ext4_result error;

	if (number == 0 || number > fs->info.inodes) {
		return EXT4_INVALID_ARGUMENT;
	}
	if ((number < fs->first_inode && number != EXT4_ROOT_INODE) ||
	    number == fs->journal_inode) {
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
	error = ext4_inode_decode(fs, number, *disk, inode);
	if (error != EXT4_OK) {
		return error;
	}
	if (inode->generation != generation) {
		return EXT4_STALE;
	}
	if (ext4_le32(&(*disk)->deletion_time) != 0) {
		return EXT4_CORRUPT;
	}
	return ext4_inode_writable(fs, *disk, inode);
}

static enum ext4_result
ext4_edit_commit(struct ext4_fs *fs, struct ext4_transaction *transaction)
{
	enum ext4_result error;

	error = ext4_transaction_commit(transaction);
	if (error != EXT4_OK) {
		fs->aborted = true;
	}
	return error;
}

enum ext4_result
ext4_set_attributes(struct ext4_fs *fs, uint32_t number, uint32_t generation,
    const struct ext4_inode_update *update, struct ext4_inode *result)
{
	struct ext4_transaction *transaction;
	struct ext4_inode_disk *disk;
	struct ext4_inode inode;
	enum ext4_result error;

	if (result == NULL) {
		return EXT4_INVALID_ARGUMENT;
	}
	error = ext4_update_validate(fs, update);
	if (error != EXT4_OK) {
		return error;
	}
	error = ext4_transaction_begin(fs->journal, 1, &transaction);
	if (error != EXT4_OK) {
		return error;
	}
	error = ext4_edit_inode(fs, transaction, number, generation, &disk, &inode);
	if (error == EXT4_OK) {
		error = ext4_inode_apply(fs, disk, update);
	}
	if (error == EXT4_OK) {
		ext4_inode_checksum_set(fs, number, disk);
		error = ext4_inode_decode(fs, number, disk, &inode);
	}
	if (error != EXT4_OK || update->fields == 0) {
		ext4_transaction_cancel(transaction);
	} else {
		error = ext4_edit_commit(fs, transaction);
	}
	if (error == EXT4_OK) {
		*result = inode;
	}
	return error;
}

struct ext4_write_target {
	uint64_t physical;
	uint32_t logical;
};

static enum ext4_result
ext4_write_snapshot(struct ext4_allocation *allocation, struct ext4_write_target *targets,
    uint32_t *count, uint32_t capacity, uint32_t logical, uint64_t physical, void **snapshot)
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
		error = ext4_transaction_buffer(allocation->transaction, physical, snapshot);
	}
	return error;
}

static enum ext4_result
ext4_write_gap(struct ext4_allocation *allocation, const struct ext4_inode *inode,
    const struct ext4_inode_disk *disk, uint64_t end, struct ext4_write_target *targets,
    uint32_t *count, uint32_t capacity)
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
			error = ext4_write_snapshot(
			    allocation, targets, count, capacity, logical, run.physical, &snapshot);
			if (error != EXT4_OK) {
				return error;
			}
			ext4_zero((uint8_t *)snapshot + within, (size_t)chunk);
		}
		position += chunk;
	}
	return EXT4_OK;
}

enum ext4_result
ext4_inode_account(struct ext4_allocation *allocation, const struct ext4_inode *inode,
    struct ext4_inode_disk *disk, uint64_t end)
{
	struct ext4_fs *fs = allocation->fs;
	uint64_t units = ext4_le32(&disk->blocks_lo);
	uint64_t increment = allocation->allocated;
	uint64_t decrement = allocation->freed;
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
		increment = allocation->allocated;
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

enum ext4_result
ext4_write(struct ext4_fs *fs, uint32_t number, uint32_t generation, uint64_t offset,
    const void *buffer, size_t length, const struct ext4_inode_update *update, size_t *completed)
{
	struct ext4_transaction *transaction;
	struct ext4_inode_disk *disk;
	struct ext4_inode inode;
	struct ext4_allocation allocation;
	struct ext4_write_target *targets = NULL;
	void *snapshot;
	uint64_t logical;
	uint64_t block_count;
	uint64_t physical;
	uint64_t free_blocks;
	uint32_t credits;
	uint32_t index;
	uint32_t target_count = 0;
	size_t within;
	size_t chunk;
	size_t consumed = 0;
	bool allocation_ready = false;
	bool zero;
	enum ext4_result error;

	if (completed == NULL) {
		return EXT4_INVALID_ARGUMENT;
	}
	*completed = 0;
	if (length != 0 && buffer == NULL) {
		return EXT4_INVALID_ARGUMENT;
	}
	error = ext4_update_validate(fs, update);
	if (error != EXT4_OK) {
		return error;
	}
	if (update->fields != EXT4_WRITE_FIELDS) {
		return EXT4_INVALID_ARGUMENT;
	}
	if (offset > UINT64_MAX - length ||
	    (length != 0 && offset + length > (uint64_t)UINT32_MAX * fs->info.block_size)) {
		return EXT4_RANGE;
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
	error = ext4_edit_inode(fs, transaction, number, generation, &disk, &inode);
	if (error != EXT4_OK) {
		goto cancel;
	}
	if ((inode.mode & EXT4_MODE_TYPE) != EXT4_MODE_REGULAR) {
		error = (inode.mode & EXT4_MODE_TYPE) == EXT4_MODE_DIRECTORY ? EXT4_IS_DIRECTORY
									     : EXT4_UNSUPPORTED;
		goto cancel;
	}
	if (length == 0) {
		goto cancel;
	}
	error = ext4_inode_apply(fs, disk, update);
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
	if (offset > inode.size) {
		error = ext4_write_gap(
		    &allocation, &inode, disk, offset, targets, &target_count, credits);
		if (error != EXT4_OK) {
			goto cancel;
		}
	}
	logical = offset / fs->info.block_size;
	for (index = 0; index < block_count; index++) {
		error = ext4_write_map_allocate(
		    &allocation, &inode, disk, (uint32_t)(logical + index), &physical, &zero);
		if (error != EXT4_OK) {
			goto cancel;
		}
		error = ext4_write_snapshot(&allocation, targets, &target_count, credits,
		    (uint32_t)(logical + index), physical, &snapshot);
		if (error != EXT4_OK) {
			goto cancel;
		}
		if (zero) {
			ext4_zero(snapshot, fs->info.block_size);
		}
		chunk = fs->info.block_size - within;
		if (chunk > length - consumed) {
			chunk = length - consumed;
		}
		ext4_copy((uint8_t *)snapshot + within, (const uint8_t *)buffer + consumed, chunk);
		consumed += chunk;
		within = 0;
	}
	error = ext4_inode_account(
	    &allocation, &inode, disk, offset + length > inode.size ? offset + length : inode.size);
	if (error != EXT4_OK) {
		goto cancel;
	}
	ext4_inode_checksum_set(fs, number, disk);
	free_blocks = allocation.free_blocks;
	ext4_allocation_destroy(&allocation);
	fs->environment.release(
	    fs->environment.context, targets, (size_t)credits * sizeof(*targets));
	error = ext4_edit_commit(fs, transaction);
	if (error == EXT4_OK) {
		fs->info.free_blocks = free_blocks;
		*completed = length;
	}
	return error;
cancel:
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

/* One data block can release two extent paths, with a bitmap and descriptor
 * per released block, plus the inode, superblock and retained data tail. Small
 * journals keep the atomic contract rather than admitting an intent whose
 * later path might not fit even the minimum cleanup batch. */
#define EXT4_TRUNCATE_RECOVERY_CREDITS (3U * (2U * EXT4_EXTENT_MAX_DEPTH + 1U) + 3U)

static enum ext4_result
ext4_truncate_start(struct ext4_fs *fs, uint32_t number, uint32_t generation, uint64_t size,
    const struct ext4_inode_update *update, struct ext4_inode *result, uint32_t batch, bool *retry)
{
	struct ext4_transaction *transaction;
	struct ext4_inode_disk *disk;
	struct ext4_inode inode;
	struct ext4_inode zero_from;
	struct ext4_allocation allocation;
	struct ext4_write_target *targets = NULL;
	uint64_t end;
	uint64_t free_blocks;
	uint64_t per_block;
	uint64_t limit;
	uint32_t first;
	uint32_t target_count = 0;
	uint32_t credits;
	bool allocation_ready = false;
	bool done = true;
	enum ext4_result error;

	*retry = false;
	if (result == NULL) {
		return EXT4_INVALID_ARGUMENT;
	}
	error = ext4_update_validate(fs, update);
	if (error != EXT4_OK) {
		return error;
	}
	if (update->fields != EXT4_WRITE_FIELDS) {
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
	error = ext4_edit_inode(fs, transaction, number, generation, &disk, &inode);
	if (error != EXT4_OK) {
		goto cancel;
	}
	if ((inode.mode & EXT4_MODE_TYPE) != EXT4_MODE_REGULAR) {
		error = (inode.mode & EXT4_MODE_TYPE) == EXT4_MODE_DIRECTORY ? EXT4_IS_DIRECTORY
									     : EXT4_UNSUPPORTED;
		goto cancel;
	}
	if (!(inode.flags & EXT4_INODE_EXTENTS)) {
		per_block = fs->info.block_size / sizeof(struct ext4_le32);
		limit = EXT4_DIRECT_BLOCKS + per_block + per_block * per_block +
		    per_block * per_block * per_block;
		if (size > limit * fs->info.block_size) {
			error = EXT4_RANGE;
			goto cancel;
		}
	}
	error = ext4_inode_apply(fs, disk, update);
	if (error != EXT4_OK) {
		goto cancel;
	}
	error = ext4_allocation_init(&allocation, fs, transaction, &inode);
	if (error != EXT4_OK) {
		goto cancel;
	}
	allocation_ready = true;
	first = (uint32_t)((size + fs->info.block_size - 1) / fs->info.block_size);
	if (size <= inode.size) {
		if (batch == 0) {
			error = ext4_write_map_truncate(&allocation, &inode, disk, first);
		} else {
			error = ext4_allocation_super(&allocation);
			if (error == EXT4_OK &&
			    (fs->last_orphan != 0 ||
				ext4_le32(&allocation.super->last_orphan) != 0)) {
				error = EXT4_RECOVERY_REQUIRED;
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
				} else {
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
	}
	error = ext4_write_gap(&allocation, &zero_from, disk, end, targets, &target_count, credits);
	if (error == EXT4_OK) {
		error = ext4_inode_account(&allocation, &inode, disk, size);
	}
	if (error == EXT4_OK) {
		ext4_inode_checksum_set(fs, number, disk);
		error = ext4_inode_decode(fs, number, disk, &inode);
	}
	if (error != EXT4_OK) {
		goto cancel;
	}
	free_blocks = allocation.free_blocks;
	ext4_allocation_destroy(&allocation);
	fs->environment.release(
	    fs->environment.context, targets, (size_t)credits * sizeof(*targets));
	error = ext4_edit_commit(fs, transaction);
	if (error == EXT4_OK) {
		fs->info.free_blocks = free_blocks;
		if (!done) {
			fs->last_orphan = number;
		}
		*result = inode;
	}
	return error;
cancel:
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

	return ext4_truncate_start(fs, number, generation, size, update, result, 0, &retry);
}

enum ext4_result
ext4_truncate(struct ext4_fs *fs, uint32_t number, uint32_t generation, uint64_t size,
    const struct ext4_inode_update *update, struct ext4_inode *result)
{
	struct ext4_inode inode;
	struct ext4_recovery_report report;
	uint32_t batch = EXT4_ORPHAN_BATCH_BLOCKS;
	bool retry;
	enum ext4_result error;

	if (result == NULL) {
		return EXT4_INVALID_ARGUMENT;
	}
	for (;;) {
		error = ext4_truncate_start(
		    fs, number, generation, size, update, &inode, batch, &retry);
		if (error != EXT4_RANGE || !retry || fs->aborted) {
			break;
		}
		batch /= 2;
	}
	if (error != EXT4_OK) {
		return error;
	}
	if (fs->last_orphan != 0) {
		ext4_zero(&report, sizeof(report));
		error = ext4_orphan_cleanup(fs, &report);
		if (error == EXT4_OK) {
			error = ext4_get_inode(fs, number, &inode);
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

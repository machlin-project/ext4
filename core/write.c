/* SPDX-License-Identifier: BSD-3-Clause */
#include "journal.h"

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
	size_t xattr_offset;
	uint16_t extra_size;
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
	if (inode->flags & ~EXT4_INODE_WRITABLE_FLAGS) {
		return EXT4_UNSUPPORTED;
	}
	/* Until ACL and security-xattr transitions are implemented, do not mutate
	 * an inode with attributes whose policy we cannot preserve atomically. */
	if (ext4_le32(&(*disk)->xattr_block_lo) != 0 || ext4_le16(&(*disk)->xattr_block_hi) != 0) {
		return EXT4_UNSUPPORTED;
	}
	extra_size = fs->inode_size > EXT4_INODE_BASE_SIZE ? ext4_le16(&(*disk)->extra_size) : 0;
	xattr_offset = EXT4_INODE_BASE_SIZE + extra_size;
	if (extra_size != 0 && xattr_offset <= fs->inode_size - sizeof(struct ext4_le32) &&
	    ext4_le32((struct ext4_le32 *)((uint8_t *)*disk + xattr_offset)) == EXT4_XATTR_MAGIC) {
		return EXT4_UNSUPPORTED;
	}
	return EXT4_OK;
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

enum ext4_result
ext4_write(struct ext4_fs *fs, uint32_t number, uint32_t generation, uint64_t offset,
    const void *buffer, size_t length, const struct ext4_inode_update *update, size_t *completed)
{
	struct ext4_transaction *transaction;
	struct ext4_inode_disk *disk;
	struct ext4_inode inode;
	uint64_t *targets = NULL;
	void *snapshot;
	uint64_t logical;
	uint64_t block_count;
	uint32_t index;
	uint32_t previous;
	size_t within;
	size_t chunk;
	size_t consumed = 0;
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
	if (offset > UINT64_MAX - length) {
		return EXT4_RANGE;
	}
	within = (size_t)(offset % fs->info.block_size);
	block_count = length / fs->info.block_size +
	    (within + length % fs->info.block_size + fs->info.block_size - 1) / fs->info.block_size;
	if (block_count >= EXT4_TRANSACTION_MAX_BLOCKS) {
		return EXT4_RANGE;
	}
	error = ext4_transaction_begin(fs->journal, (uint32_t)block_count + 1, &transaction);
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
	if (offset > inode.size || length > inode.size - offset) {
		error = EXT4_UNSUPPORTED;
		goto cancel;
	}
	error = ext4_inode_apply(fs, disk, update);
	if (error != EXT4_OK) {
		goto cancel;
	}
	targets = fs->environment.allocate(
	    fs->environment.context, (size_t)block_count * sizeof(*targets));
	if (targets == NULL) {
		error = EXT4_NO_MEMORY;
		goto cancel;
	}
	logical = offset / fs->info.block_size;
	for (index = 0; index < block_count; index++) {
		error = ext4_map_block(fs, &inode, (uint32_t)(logical + index), &targets[index]);
		if (error != EXT4_OK) {
			goto cancel;
		}
		if (targets[index] == 0) {
			error = EXT4_UNSUPPORTED;
			goto cancel;
		}
		if (!ext4_journal_target(fs->journal, targets[index])) {
			error = EXT4_CORRUPT;
			goto cancel;
		}
		error = ext4_data_block_valid(fs, targets[index]);
		if (error != EXT4_OK) {
			goto cancel;
		}
		for (previous = 0; previous < index; previous++) {
			if (targets[index] == targets[previous]) {
				error = EXT4_CORRUPT;
				goto cancel;
			}
		}
		error = ext4_transaction_buffer(transaction, targets[index], &snapshot);
		if (error != EXT4_OK) {
			goto cancel;
		}
		chunk = fs->info.block_size - within;
		if (chunk > length - consumed) {
			chunk = length - consumed;
		}
		ext4_copy((uint8_t *)snapshot + within, (const uint8_t *)buffer + consumed, chunk);
		consumed += chunk;
		within = 0;
	}
	ext4_inode_checksum_set(fs, number, disk);
	fs->environment.release(
	    fs->environment.context, targets, (size_t)block_count * sizeof(*targets));
	error = ext4_edit_commit(fs, transaction);
	if (error == EXT4_OK) {
		*completed = length;
	}
	return error;
cancel:
	if (targets != NULL) {
		fs->environment.release(
		    fs->environment.context, targets, (size_t)block_count * sizeof(*targets));
	}
	ext4_transaction_cancel(transaction);
	return error;
}

/* SPDX-License-Identifier: BSD-3-Clause */
#include "journal.h"

enum ext4_result
ext4_set_inode_flags(struct ext4_fs *fs, uint32_t number, uint32_t generation, uint32_t mask,
    uint32_t flags, const struct ext4_timestamp *change_time, struct ext4_inode *result)
{
	struct ext4_transaction *transaction;
	struct ext4_inode_disk *disk;
	struct ext4_inode inode;
	struct ext4_inode_update update;
	uint32_t allowed = EXT4_INODE_MODIFIABLE_FLAGS;
	uint32_t desired;
	uint16_t type;
	enum ext4_result error;

	if (fs == NULL || change_time == NULL || result == NULL || mask == 0 || (flags & ~mask)) {
		return EXT4_INVALID_ARGUMENT;
	}
	if (mask & ~EXT4_INODE_MODIFIABLE_FLAGS) {
		return EXT4_UNSUPPORTED;
	}
	if (fs->aborted) {
		return EXT4_RECOVERY_REQUIRED;
	}
	if (fs->journal == NULL) {
		return EXT4_READ_ONLY;
	}
	error = ext4_transaction_begin(fs->journal, 1, &transaction);
	if (error != EXT4_OK) {
		return error;
	}
	error = ext4_edit_inode(fs, transaction, number, generation, &disk, &inode);
	if (error != EXT4_OK) {
		goto cancel;
	}
	type = inode.mode & EXT4_MODE_TYPE;
	if (type == EXT4_MODE_REGULAR) {
		allowed &= ~(uint32_t)(EXT4_INODE_DIRSYNC | EXT4_INODE_TOPDIR);
	} else if (type != EXT4_MODE_DIRECTORY) {
		allowed = EXT4_INODE_NODUMP | EXT4_INODE_NOATIME;
	}
	desired = (inode.flags & ~mask) | flags;
	if ((inode.flags & desired & EXT4_INODE_IMMUTABLE) && desired != inode.flags) {
		error = EXT4_PERMISSION_DENIED;
		goto cancel;
	}
	if (desired & EXT4_INODE_MODIFIABLE_FLAGS & ~allowed) {
		error = EXT4_INVALID_ARGUMENT;
		goto cancel;
	}
	ext4_zero(&update, sizeof(update));
	update.fields = EXT4_ATTR_CHANGE_TIME;
	update.change_time = *change_time;
	error = ext4_inode_apply(fs, disk, &update);
	if (error != EXT4_OK) {
		goto cancel;
	}
	ext4_encode32(&disk->flags, desired);
	ext4_inode_checksum_set(fs, number, disk);
	error = ext4_inode_decode_live(fs, number, disk, &inode);
	if (error != EXT4_OK) {
		goto cancel;
	}
	error = ext4_transaction_commit(transaction);
	if (error != EXT4_OK) {
		fs->aborted = true;
		return error;
	}
	*result = inode;
	return EXT4_OK;
cancel:
	ext4_transaction_cancel(transaction);
	return error;
}

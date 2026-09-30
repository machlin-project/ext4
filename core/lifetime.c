/* SPDX-License-Identifier: BSD-3-Clause */
#include "internal.h"

#define EXT4_INODE_HOLD_LIMIT (1U << 20)

struct ext4_inode_hold *
ext4_inode_find_hold(struct ext4_fs *fs, uint32_t number)
{
	struct ext4_inode_hold *hold;

	for (hold = fs->holds; hold != NULL; hold = hold->next) {
		if (hold->number == number) {
			return hold;
		}
	}
	return NULL;
}

enum ext4_result
ext4_inode_decode_live(struct ext4_fs *fs, uint32_t number, void *buffer, struct ext4_inode *inode)
{
	struct ext4_inode_hold *hold = ext4_inode_find_hold(fs, number);
	struct ext4_inode decoded;
	enum ext4_result error;

	error = hold != NULL && hold->unlinked
	    ? ext4_inode_decode_orphan(fs, number, buffer, &decoded)
	    : ext4_inode_decode(fs, number, buffer, &decoded);
	if (error != EXT4_OK) {
		return error;
	}
	if (decoded.flags & EXT4_INODE_EA_INODE) {
		return EXT4_CORRUPT;
	}
	if (hold != NULL && decoded.generation != hold->generation) {
		return EXT4_STALE;
	}
	if (hold != NULL && (hold->references == 0 || (hold->unlinked && decoded.links != 0))) {
		return EXT4_CORRUPT;
	}
	*inode = decoded;
	return EXT4_OK;
}

enum ext4_result
ext4_refresh_inode(struct ext4_inode_hold *hold, struct ext4_inode *result)
{
	struct ext4_fs *fs;
	struct ext4_inode inode = { 0 };
	void *buffer;
	uint64_t offset = 0;
	enum ext4_result error;

	if (hold == NULL || result == NULL || hold->references == 0) {
		return EXT4_INVALID_ARGUMENT;
	}
	fs = hold->fs;
	/* Explicit refresh also observes changes from a replaced backing view. */
	ext4_drop_read_cache(hold);
	if (fs->aborted) {
		return EXT4_RECOVERY_REQUIRED;
	}
	error = ext4_inode_resolve(fs, hold->number, &offset);
	if (error != EXT4_OK) {
		return error;
	}
	buffer = fs->environment.allocate(fs->environment.context, fs->inode_size);
	if (buffer == NULL) {
		return EXT4_NO_MEMORY;
	}
	error = ext4_device_read(fs, offset, buffer, fs->inode_size);
	if (error == EXT4_OK) {
		error = ext4_inode_decode_live(fs, hold->number, buffer, &inode);
	}
	fs->environment.release(fs->environment.context, buffer, fs->inode_size);
	if (error == EXT4_OK && inode.generation != hold->generation) {
		error = EXT4_STALE;
	}
	if (error == EXT4_OK) {
		*result = inode;
	}
	return error;
}

enum ext4_result
ext4_hold_inode(
    struct ext4_fs *fs, uint32_t number, uint32_t generation, struct ext4_inode_hold **result)
{
	struct ext4_inode_hold *hold;
	struct ext4_inode inode;
	enum ext4_result error;

	if (fs == NULL || result == NULL || number == 0 || number > fs->info.inodes) {
		return EXT4_INVALID_ARGUMENT;
	}
	if (fs->aborted) {
		return EXT4_RECOVERY_REQUIRED;
	}
	if ((number < fs->first_inode && number != EXT4_ROOT_INODE) ||
	    number == fs->journal_inode || number == fs->orphan_file_inode) {
		return EXT4_UNSUPPORTED;
	}
	hold = ext4_inode_find_hold(fs, number);
	if (hold != NULL) {
		if (hold->generation != generation) {
			return EXT4_STALE;
		}
		if (hold->references == UINT32_MAX) {
			return EXT4_RANGE;
		}
		error = ext4_refresh_inode(hold, &inode);
		if (error == EXT4_OK) {
			hold->references++;
			*result = hold;
		}
		return error;
	}
	if (fs->hold_count == EXT4_INODE_HOLD_LIMIT) {
		return EXT4_UNSUPPORTED;
	}
	error = ext4_inode_allocated(fs, number);
	if (error == EXT4_OK) {
		error = ext4_get_inode(fs, number, &inode);
	}
	if (error != EXT4_OK) {
		return error;
	}
	if (inode.generation != generation) {
		return EXT4_STALE;
	}
	hold = fs->environment.allocate(fs->environment.context, sizeof(*hold));
	if (hold == NULL) {
		return EXT4_NO_MEMORY;
	}
	hold->fs = fs;
	hold->reader = NULL;
	hold->number = number;
	hold->generation = generation;
	hold->references = 1;
	hold->unlinked = false;
	hold->next = fs->holds;
	fs->holds = hold;
	fs->hold_count++;
	*result = hold;
	return EXT4_OK;
}

enum ext4_result
ext4_release_inode(struct ext4_inode_hold *hold)
{
	struct ext4_fs *fs;
	struct ext4_inode_hold **link;
	enum ext4_result error = EXT4_OK;

	if (hold == NULL || hold->references == 0) {
		return EXT4_INVALID_ARGUMENT;
	}
	fs = hold->fs;
	if (hold->references > 1) {
		hold->references--;
		return EXT4_OK;
	}
	if (hold->unlinked) {
		error = fs->aborted
		    ? EXT4_RECOVERY_REQUIRED
		    : ext4_orphan_finish_inode(fs, hold->number, hold->generation, false);
		if (error != EXT4_OK) {
			fs->aborted = true;
		}
	}
	for (link = &fs->holds; *link != hold; link = &(*link)->next) {
		/* Only this exclusive owner changes the registry. */
	}
	*link = hold->next;
	fs->hold_count--;
	ext4_drop_read_cache(hold);
	fs->environment.release(fs->environment.context, hold, sizeof(*hold));
	return error;
}

void
ext4_inode_holds_destroy(struct ext4_fs *fs)
{
	struct ext4_inode_hold *hold;

	while (fs->holds != NULL) {
		hold = fs->holds;
		fs->holds = hold->next;
		ext4_drop_read_cache(hold);
		fs->environment.release(fs->environment.context, hold, sizeof(*hold));
	}
	fs->hold_count = 0;
}

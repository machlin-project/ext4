/* SPDX-License-Identifier: BSD-3-Clause */
#include "map_read.h"

void
ext4_drop_read_cache(struct ext4_inode_hold *hold)
{
	struct ext4_fs *fs;

	if (hold == NULL || hold->reader == NULL) {
		return;
	}
	fs = hold->fs;
	ext4_map_reader_close(fs, &hold->reader->mapping);
	fs->environment.release(fs->environment.context, hold->reader, sizeof(*hold->reader));
	hold->reader = NULL;
}

void
ext4_read_cache_invalidate(struct ext4_fs *fs)
{
	struct ext4_inode_hold *hold;

	/* Wrapping must not make an old snapshot current again. Normal invalidation
	 * is constant time regardless of the number of open inodes. */
	if (fs->read_revision == UINT64_MAX) {
		for (hold = fs->holds; hold != NULL; hold = hold->next) {
			ext4_drop_read_cache(hold);
		}
	}
	fs->read_revision++;
}

enum ext4_result
ext4_read_state_get(struct ext4_inode_hold *hold, struct ext4_read_state **result)
{
	struct ext4_fs *fs;
	struct ext4_inode inode;
	struct ext4_read_state *reader;
	enum ext4_result error;

	if (hold == NULL || hold->references == 0) {
		return EXT4_INVALID_ARGUMENT;
	}
	fs = hold->fs;
	if (fs->aborted) {
		return EXT4_RECOVERY_REQUIRED;
	}
	if (hold->reader != NULL && hold->reader->revision != fs->read_revision) {
		ext4_drop_read_cache(hold);
	}
	if (hold->reader == NULL) {
		error = ext4_refresh_inode(hold, &inode);
		if (error != EXT4_OK) {
			return error;
		}
		reader = fs->environment.allocate(fs->environment.context, sizeof(*reader));
		if (reader == NULL) {
			return EXT4_NO_MEMORY;
		}
		ext4_zero(reader, sizeof(*reader));
		reader->inode = inode;
		reader->revision = fs->read_revision;
		reader->mapping.cache = &reader->cache;
		hold->reader = reader;
	}
	*result = hold->reader;
	return EXT4_OK;
}

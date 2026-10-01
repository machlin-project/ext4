/* SPDX-License-Identifier: BSD-3-Clause */
#include "map_read.h"
#include "inline.h"

static enum ext4_result
ext4_seek_with_mapping(struct ext4_fs *fs, const struct ext4_inode *inode,
    struct ext4_map_reader *reader, uint64_t offset, enum ext4_seek_region region, uint64_t *result)
{
	uint64_t logical;
	uint64_t physical;
	uint64_t blocks;
	uint64_t bytes;
	enum ext4_result error;

	if (fs == NULL || inode == NULL || result == NULL ||
	    (region != EXT4_SEEK_DATA && region != EXT4_SEEK_HOLE) ||
	    (inode->mode & EXT4_MODE_TYPE) != EXT4_MODE_REGULAR) {
		return EXT4_INVALID_ARGUMENT;
	}
	if (fs->aborted) {
		return EXT4_RECOVERY_REQUIRED;
	}
	if (offset >= inode->size) {
		return EXT4_NOT_FOUND;
	}
	if (inode->flags & EXT4_INODE_INLINE_DATA) {
		error = ext4_inline_validate(fs, inode, NULL);
		if (error == EXT4_OK) {
			*result = region == EXT4_SEEK_DATA ? offset : inode->size;
		}
		return error;
	}
	while (offset < inode->size) {
		logical = offset / fs->info.block_size;
		if (logical > UINT32_MAX) {
			return EXT4_RANGE;
		}
		error =
		    ext4_map_reader_next(fs, inode, reader, (uint32_t)logical, &physical, &blocks);
		if (error != EXT4_OK) {
			return error;
		}
		if (blocks == 0 || blocks > (uint64_t)UINT32_MAX + 1U - logical) {
			return EXT4_CORRUPT;
		}
		if ((physical == 0) == (region == EXT4_SEEK_HOLE)) {
			*result = offset;
			return EXT4_OK;
		}
		/* Advance across an entire checked run, including large sparse gaps.
		 * Only mapping nodes are read; pending journal data stays untouched. */
		bytes = blocks * fs->info.block_size - offset % fs->info.block_size;
		offset += bytes < inode->size - offset ? bytes : inode->size - offset;
	}
	if (region == EXT4_SEEK_HOLE) {
		*result = inode->size;
		return EXT4_OK;
	}
	return EXT4_NOT_FOUND;
}

enum ext4_result
ext4_seek_region(struct ext4_fs *fs, const struct ext4_inode *inode, uint64_t offset,
    enum ext4_seek_region region, uint64_t *result)
{
	struct ext4_map_reader reader = { 0 };
	enum ext4_result error;

	error = ext4_seek_with_mapping(fs, inode, &reader, offset, region, result);
	if (fs != NULL) {
		ext4_map_reader_close(fs, &reader);
	}
	return error;
}

enum ext4_result
ext4_seek_region_held(
    struct ext4_inode_hold *hold, uint64_t offset, enum ext4_seek_region region, uint64_t *result)
{
	struct ext4_read_state *reader;
	enum ext4_result error;

	if (result == NULL || (region != EXT4_SEEK_DATA && region != EXT4_SEEK_HOLE)) {
		return EXT4_INVALID_ARGUMENT;
	}
	error = ext4_read_state_get(hold, &reader);
	if (error != EXT4_OK) {
		return error;
	}
	return ext4_seek_with_mapping(
	    hold->fs, &reader->inode, &reader->mapping, offset, region, result);
}

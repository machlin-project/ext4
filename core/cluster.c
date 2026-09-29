/* SPDX-License-Identifier: BSD-3-Clause */
#include "allocate.h"

static enum ext4_result
ext4_cluster_backing(struct ext4_allocation *allocation, const struct ext4_inode *inode,
    const struct ext4_inode_disk *disk, uint32_t logical, uint64_t skip_first, uint64_t skip_end,
    uint64_t *physical, bool *found)
{
	struct ext4_fs *fs = allocation->fs;
	struct ext4_map_run run;
	uint64_t first = (uint64_t)logical / fs->cluster_blocks * fs->cluster_blocks;
	uint64_t end = first + fs->cluster_blocks;
	uint64_t position = first;
	uint64_t length;
	uint64_t base;
	enum ext4_result error;

	*found = false;
	while (position < end) {
		if (position >= skip_first && position < skip_end) {
			position = skip_end < end ? skip_end : end;
			continue;
		}
		error = ext4_write_map_lookup(allocation, inode, disk, (uint32_t)position, &run);
		if (error != EXT4_OK) {
			return error;
		}
		length = run.length < end - position ? run.length : end - position;
		if (position < skip_first && length > skip_first - position) {
			length = skip_first - position;
		}
		if (length == 0) {
			return EXT4_CORRUPT;
		}
		if (run.physical != 0) {
			if (run.physical < position - first) {
				return EXT4_CORRUPT;
			}
			base = run.physical - (position - first);
			if (base % fs->cluster_blocks != 0 || (*found && *physical != base)) {
				return EXT4_CORRUPT;
			}
			*physical = base;
			*found = true;
		}
		position += length;
	}
	return *found ? ext4_allocation_valid_range(allocation, *physical, fs->cluster_blocks)
		      : EXT4_OK;
}

enum ext4_result
ext4_cluster_allocate(struct ext4_allocation *allocation, const struct ext4_inode *inode,
    const struct ext4_inode_disk *disk, uint32_t logical, uint64_t *physical)
{
	struct ext4_fs *fs = allocation->fs;
	struct ext4_map_run run;
	uint64_t base = 0;
	uint64_t goal = 0;
	bool found = false;
	enum ext4_result error = EXT4_OK;

	if (fs->cluster_blocks != 1) {
		error = ext4_cluster_backing(allocation, inode, disk, logical, 0, 0, &base, &found);
	}
	/* Like Linux, place new data after the block backing the preceding logical
	 * block; allocating there extends the file's existing extent. */
	if (error == EXT4_OK && !found && logical != 0 && !ext4_allocation_continues(allocation)) {
		error = ext4_write_map_lookup(allocation, inode, disk, logical - 1U, &run);
		if (error == EXT4_OK && run.physical != 0) {
			goal = (run.physical / fs->cluster_blocks + 1U) * fs->cluster_blocks;
		}
	}
	if (error == EXT4_OK && !found) {
		error = ext4_allocate_data(allocation, goal, &base);
	}
	if (error == EXT4_OK) {
		*physical = fs->cluster_blocks == 1 ? base : base + logical % fs->cluster_blocks;
	}
	return error;
}

enum ext4_result
ext4_cluster_release(struct ext4_allocation *allocation, const struct ext4_inode *inode,
    const struct ext4_inode_disk *disk, uint32_t logical, uint64_t physical, uint32_t length,
    uint64_t removed_end)
{
	struct ext4_fs *fs = allocation->fs;
	uint64_t first;
	uint64_t end;
	uint64_t position;
	uint64_t backing = 0;
	uint64_t base;
	bool found;
	enum ext4_result error;

	if (length == 0 || (uint64_t)logical + length > removed_end ||
	    removed_end > (uint64_t)UINT32_MAX + 1U) {
		return EXT4_CORRUPT;
	}
	if (fs->cluster_blocks == 1) {
		error = ext4_free_blocks(allocation, physical, length);
	} else {
		first = (uint64_t)logical / fs->cluster_blocks * fs->cluster_blocks;
		end = ((uint64_t)logical + length + fs->cluster_blocks - 1U) / fs->cluster_blocks *
		    fs->cluster_blocks;
		if (physical < logical - first ||
		    physical % fs->cluster_blocks != logical % fs->cluster_blocks) {
			return EXT4_CORRUPT;
		}
		base = physical - (logical - first);
		for (position = first; position < end; position += fs->cluster_blocks) {
			error = ext4_cluster_backing(allocation, inode, disk, (uint32_t)position,
			    logical, removed_end, &backing, &found);
			if (error != EXT4_OK) {
				return error;
			}
			if (found) {
				if (backing != base + position - first) {
					return EXT4_CORRUPT;
				}
			} else {
				error = ext4_free_blocks(
				    allocation, base + position - first, fs->cluster_blocks);
				if (error != EXT4_OK) {
					return error;
				}
			}
		}
		error = EXT4_OK;
	}
	if (error == EXT4_OK) {
		allocation->unmapped += length;
	}
	return error;
}

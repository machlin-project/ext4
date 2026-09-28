/* SPDX-License-Identifier: BSD-3-Clause */
#include "journal.h"

#define EXT4_SYSTEM_MAX_RANGES (1U << 20)

static enum ext4_result
ext4_system_add(struct ext4_fs *fs, uint64_t first, uint64_t length)
{
	struct ext4_block_range *range;

	if (first < fs->first_data_block || first >= fs->info.blocks || length == 0 ||
	    length > fs->info.blocks - first ||
	    fs->system_range_count == fs->system_range_capacity) {
		return EXT4_CORRUPT;
	}
	range = &fs->system_ranges[fs->system_range_count++];
	range->first = first;
	range->length = length;
	return EXT4_OK;
}

static void
ext4_range_sift(struct ext4_block_range *ranges, size_t root, size_t count)
{
	struct ext4_block_range saved = ranges[root];
	size_t child;

	while (root < count / 2) {
		child = root * 2 + 1;
		if (child + 1 < count && ranges[child + 1].first > ranges[child].first) {
			child++;
		}
		if (saved.first >= ranges[child].first) {
			break;
		}
		ranges[root] = ranges[child];
		root = child;
	}
	ranges[root] = saved;
}

static void
ext4_ranges_order(struct ext4_block_range *ranges, size_t count)
{
	struct ext4_block_range temporary;
	size_t index;

	for (index = count / 2; index > 0; index--) {
		ext4_range_sift(ranges, index - 1, count);
	}
	for (index = count; index > 1; index--) {
		temporary = ranges[0];
		ranges[0] = ranges[index - 1];
		ranges[index - 1] = temporary;
		ext4_range_sift(ranges, 0, index - 1);
	}
}

enum ext4_result
ext4_ranges_sort(struct ext4_block_range *ranges, size_t *range_count)
{
	size_t count = *range_count;
	size_t index;
	size_t used = 0;

	ext4_ranges_order(ranges, count);
	for (index = 0; index < count; index++) {
		if (used != 0) {
			if (ranges[index].first <
			    ranges[used - 1].first + ranges[used - 1].length) {
				return EXT4_CORRUPT;
			}
			if (ranges[index].first ==
			    ranges[used - 1].first + ranges[used - 1].length) {
				ranges[used - 1].length += ranges[index].length;
				continue;
			}
		}
		ranges[used++] = ranges[index];
	}
	*range_count = used;
	return EXT4_OK;
}

void
ext4_ranges_union(struct ext4_block_range *ranges, size_t *range_count)
{
	struct ext4_block_range *previous;
	uint64_t end;
	size_t index;
	size_t used = 0;

	ext4_ranges_order(ranges, *range_count);
	for (index = 0; index < *range_count; index++) {
		if (used != 0) {
			previous = &ranges[used - 1];
			if (ranges[index].first <= previous->first + previous->length) {
				end = ranges[index].first + ranges[index].length;
				if (end > previous->first + previous->length) {
					previous->length = end - previous->first;
				}
				continue;
			}
		}
		ranges[used++] = ranges[index];
	}
	*range_count = used;
}

enum ext4_result
ext4_system_ranges_build(struct ext4_fs *fs)
{
	struct ext4_group group;
	struct ext4_block_range fixed;
	struct ext4_journal *journal = fs->journal;
	uint64_t capacity;
	uint64_t free_blocks = 0;
	uint64_t free_inodes = 0;
	uint32_t index;
	enum ext4_result error;

	if (journal == NULL || fs->system_ranges != NULL) {
		return EXT4_INVALID_ARGUMENT;
	}
	capacity = (uint64_t)fs->info.groups * 4 + journal->run_count + journal->mapping_count + 1U;
	if (fs->orphan_file_inode != 0) {
		capacity += EXT4_ORPHAN_FILE_MAX_BLOCKS * (EXT4_EXTENT_MAX_DEPTH + 1U);
	}
	if (capacity > EXT4_SYSTEM_MAX_RANGES) {
		return EXT4_UNSUPPORTED;
	}
	fs->system_range_capacity = (size_t)capacity;
	fs->system_ranges = fs->environment.allocate(
	    fs->environment.context, fs->system_range_capacity * sizeof(*fs->system_ranges));
	if (fs->system_ranges == NULL) {
		return EXT4_NO_MEMORY;
	}
	for (index = 0; index < fs->info.groups; index++) {
		error = ext4_group_reserved(fs, index, &fixed);
		if (error == EXT4_OK && fixed.length != 0) {
			error = ext4_system_add(fs, fixed.first, fixed.length);
		}
		if (error != EXT4_OK) {
			return error;
		}
		error = ext4_group_get(fs, index, &group);
		if (error != EXT4_OK) {
			return error;
		}
		free_blocks += group.free_blocks;
		free_inodes += group.free_inodes;
		error = ext4_system_add(fs, group.block_bitmap, 1);
		if (error == EXT4_OK) {
			error = ext4_system_add(fs, group.inode_bitmap, 1);
		}
		if (error == EXT4_OK) {
			error = ext4_system_add(fs, group.inode_table, group.table_blocks);
		}
		if (error != EXT4_OK) {
			return error;
		}
	}
	if (free_blocks != fs->info.free_blocks || free_inodes != fs->info.free_inodes) {
		return EXT4_CORRUPT;
	}
	for (index = 0; index < journal->run_count; index++) {
		error =
		    ext4_system_add(fs, journal->runs[index].physical, journal->runs[index].length);
		if (error != EXT4_OK) {
			return error;
		}
	}
	for (index = 0; index < journal->mapping_count; index++) {
		error = ext4_system_add(fs, journal->mapping_blocks[index], 1);
		if (error != EXT4_OK) {
			return error;
		}
	}
	if (fs->mmp_block != 0) {
		error = ext4_system_add(fs, fs->mmp_block, 1);
		if (error != EXT4_OK) {
			return error;
		}
	}
	error = ext4_ranges_sort(fs->system_ranges, &fs->system_range_count);
	if (error == EXT4_OK) {
		error = ext4_orphan_file_prepare(fs);
	}
	if (error != EXT4_OK || fs->orphan_file == NULL) {
		return error;
	}
	for (index = 0; index < fs->orphan_file->block_count + fs->orphan_file->mapping_count;
	    index++) {
		error = ext4_system_add(fs, fs->orphan_file->blocks[index], 1);
		if (error != EXT4_OK) {
			return error;
		}
	}
	return ext4_ranges_sort(fs->system_ranges, &fs->system_range_count);
}

bool
ext4_system_block(const struct ext4_fs *fs, uint64_t block)
{
	return ext4_system_overlaps(fs, block, 1);
}

bool
ext4_system_overlaps(const struct ext4_fs *fs, uint64_t block, uint64_t length)
{
	return ext4_ranges_overlap(fs->system_ranges, fs->system_range_count, block, length);
}

bool
ext4_ranges_overlap(
    const struct ext4_block_range *ranges, size_t count, uint64_t block, uint64_t length)
{
	const struct ext4_block_range *range;
	size_t low = 0;
	size_t high = count;
	size_t middle;

	while (low < high) {
		middle = low + (high - low) / 2;
		range = &ranges[middle];
		if (block < range->first) {
			if (range->first - block < length) {
				return true;
			}
			high = middle;
		} else if (block - range->first < range->length) {
			return true;
		} else {
			low = middle + 1;
		}
	}
	return false;
}

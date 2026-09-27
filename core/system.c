/* SPDX-License-Identifier: BSD-3-Clause */
#include "journal.h"

#define EXT4_SYSTEM_MAX_RANGES (1U << 20)

static bool
ext4_power_of(uint32_t value, uint32_t base)
{
	while (value > 1 && value % base == 0) {
		value /= base;
	}
	return value == 1;
}

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

enum ext4_result
ext4_ranges_sort(struct ext4_block_range *ranges, size_t *range_count)
{
	struct ext4_block_range temporary;
	size_t count = *range_count;
	size_t index;
	size_t used = 0;

	for (index = count / 2; index > 0; index--) {
		ext4_range_sift(ranges, index - 1, count);
	}
	for (index = count; index > 1; index--) {
		temporary = ranges[0];
		ranges[0] = ranges[index - 1];
		ranges[index - 1] = temporary;
		ext4_range_sift(ranges, 0, index - 1);
	}
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

enum ext4_result
ext4_system_ranges_build(struct ext4_fs *fs)
{
	struct ext4_group group;
	struct ext4_journal *journal = fs->journal;
	uint64_t capacity;
	uint64_t reserved;
	uint64_t first;
	uint64_t free_blocks = 0;
	uint64_t free_inodes = 0;
	uint32_t index;
	bool super;
	enum ext4_result error;

	if (journal == NULL || fs->system_ranges != NULL) {
		return EXT4_INVALID_ARGUMENT;
	}
	capacity = (uint64_t)fs->info.groups * 4 + journal->run_count + journal->mapping_count;
	if (capacity > EXT4_SYSTEM_MAX_RANGES) {
		return EXT4_UNSUPPORTED;
	}
	fs->system_range_capacity = (size_t)capacity;
	fs->system_ranges = fs->environment.allocate(
	    fs->environment.context, fs->system_range_capacity * sizeof(*fs->system_ranges));
	if (fs->system_ranges == NULL) {
		return EXT4_NO_MEMORY;
	}
	reserved = 1 +
	    ((uint64_t)fs->info.groups * fs->descriptor_size + fs->info.block_size - 1) /
		fs->info.block_size +
	    fs->reserved_gdt_blocks;
	for (index = 0; index < fs->info.groups; index++) {
		first = fs->first_data_block + (uint64_t)index * fs->blocks_per_group;
		super = !(fs->info.feature_ro_compat & EXT4_FEATURE_RO_SPARSE_SUPER) ||
		    index == 0 || ext4_power_of(index, 3) || ext4_power_of(index, 5) ||
		    ext4_power_of(index, 7);
		if (super) {
			error = ext4_system_add(fs, first, reserved);
			if (error != EXT4_OK) {
				return error;
			}
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
	const struct ext4_block_range *range;
	size_t low = 0;
	size_t high = fs->system_range_count;
	size_t middle;

	while (low < high) {
		middle = low + (high - low) / 2;
		range = &fs->system_ranges[middle];
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

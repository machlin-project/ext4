/* SPDX-License-Identifier: BSD-3-Clause */
#include "allocate.h"

static bool
ext4_bitmap_test(const uint8_t *bitmap, uint32_t bit)
{
	return (bitmap[bit / EXT4_BITS_PER_BYTE] & (1U << (bit % EXT4_BITS_PER_BYTE))) != 0;
}

static void
ext4_bitmap_set(uint8_t *bitmap, uint32_t bit)
{
	bitmap[bit / EXT4_BITS_PER_BYTE] |= (uint8_t)(1U << (bit % EXT4_BITS_PER_BYTE));
}

static enum ext4_result
ext4_allocation_group(
    struct ext4_allocation *allocation, uint32_t index, struct ext4_group *group, uint64_t *offset)
{
	struct ext4_fs *fs = allocation->fs;
	enum ext4_result error;

	error = ext4_group_descriptor_offset(fs, index, offset);
	if (error == EXT4_OK) {
		error = ext4_transaction_read(
		    allocation->transaction, *offset / fs->info.block_size, allocation->scratch);
	}
	if (error != EXT4_OK) {
		return error;
	}
	return ext4_group_decode(fs, index,
	    (struct ext4_group_disk *)(allocation->scratch + *offset % fs->info.block_size), group);
}

static enum ext4_result
ext4_allocation_checksum(
    struct ext4_allocation *allocation, const struct ext4_group *group, const uint8_t *bitmap)
{
	struct ext4_fs *fs = allocation->fs;
	uint32_t checksum;

	if (fs->metadata_checksum) {
		checksum = ext4_crc32c(
		    fs->checksum_seed, bitmap, fs->clusters_per_group / EXT4_BITS_PER_BYTE);
		if (fs->descriptor_size < EXT4_GROUP_64_SIZE) {
			checksum &= UINT16_MAX;
		}
		if (checksum != group->block_bitmap_checksum) {
			return EXT4_CORRUPT;
		}
	}
	return EXT4_OK;
}

enum ext4_result
ext4_allocation_init(struct ext4_allocation *allocation, struct ext4_fs *fs,
    struct ext4_transaction *transaction, const struct ext4_inode *inode)
{
	const struct ext4_super_disk *super;
	const uint8_t *buffer;

	ext4_zero(allocation, sizeof(*allocation));
	allocation->fs = fs;
	allocation->transaction = transaction;
	/* Orphan cleanup and multi-step verity construction use validated records
	 * directly instead of the ordinary live-inode edit entry point. */
	ext4_transaction_inode_policy(transaction, inode);
	allocation->free_blocks = fs->info.free_blocks;
	allocation->free_inodes = fs->info.free_inodes;
	/* A later context in the same transaction continues from its counters. */
	buffer = ext4_transaction_peek(transaction, EXT4_SUPER_OFFSET / fs->info.block_size);
	if (buffer != NULL) {
		super = (const struct ext4_super_disk *)(buffer +
		    EXT4_SUPER_OFFSET % fs->info.block_size);
		allocation->free_blocks = ext4_le32(&super->free_blocks_lo);
		if (fs->info.feature_incompat & EXT4_FEATURE_INCOMPAT_64BIT) {
			allocation->free_blocks |= (uint64_t)ext4_le32(&super->free_blocks_hi)
			    << 32;
		}
		allocation->free_inodes = ext4_le32(&super->free_inodes);
	}
	allocation->maximum_block =
	    (inode->flags & EXT4_INODE_EXTENTS) ? EXT4_PHYSICAL_BLOCK_MAX : UINT32_MAX;
	if (!(fs->journal->features & EXT4_JBD_64BIT)) {
		allocation->maximum_block = UINT32_MAX;
	}
	allocation->group_index = (inode->number - 1) / fs->inodes_per_group;
	allocation->validated_group = UINT32_MAX;
	allocation->scratch =
	    fs->environment.allocate(fs->environment.context, fs->info.block_size);
	return allocation->scratch == NULL ? EXT4_NO_MEMORY : EXT4_OK;
}

void
ext4_allocation_destroy(struct ext4_allocation *allocation)
{
	struct ext4_fs *fs = allocation->fs;

	if (allocation->scratch != NULL) {
		fs->environment.release(
		    fs->environment.context, allocation->scratch, fs->info.block_size);
		allocation->scratch = NULL;
	}
	if (allocation->validated != NULL) {
		fs->environment.release(
		    fs->environment.context, allocation->validated, fs->info.block_size);
		allocation->validated = NULL;
	}
}

enum ext4_result
ext4_allocation_super(struct ext4_allocation *allocation)
{
	struct ext4_fs *fs = allocation->fs;
	struct ext4_super_disk *super;
	uint64_t free_blocks;
	enum ext4_result error;

	if (allocation->super != NULL) {
		return EXT4_OK;
	}
	error = ext4_transaction_super(allocation->transaction, &super);
	if (error != EXT4_OK) {
		return error;
	}
	free_blocks = ext4_le32(&super->free_blocks_lo);
	allocation->reserved_blocks = ext4_le32(&super->reserved_blocks_lo);
	if (fs->info.feature_incompat & EXT4_FEATURE_INCOMPAT_64BIT) {
		free_blocks |= (uint64_t)ext4_le32(&super->free_blocks_hi) << 32;
		allocation->reserved_blocks |= (uint64_t)ext4_le32(&super->reserved_blocks_hi)
		    << 32;
	}
	if (free_blocks != allocation->free_blocks ||
	    allocation->reserved_blocks > fs->info.blocks) {
		return EXT4_CORRUPT;
	}
	allocation->super = super;
	return EXT4_OK;
}

static enum ext4_result
ext4_bitmap_protect(uint8_t *bitmap, uint32_t first, uint32_t end, bool initialize)
{
	uint32_t byte;
	uint8_t mask;

	while (first < end && first % EXT4_BITS_PER_BYTE != 0) {
		if (!initialize && !ext4_bitmap_test(bitmap, first)) {
			return EXT4_CORRUPT;
		}
		ext4_bitmap_set(bitmap, first++);
	}
	while (end - first >= EXT4_BITS_PER_BYTE) {
		byte = first / EXT4_BITS_PER_BYTE;
		if (!initialize && bitmap[byte] != UINT8_MAX) {
			return EXT4_CORRUPT;
		}
		bitmap[byte] = UINT8_MAX;
		first += EXT4_BITS_PER_BYTE;
	}
	if (first < end) {
		byte = first / EXT4_BITS_PER_BYTE;
		mask = (uint8_t)((1U << (end - first)) - 1U);
		if (!initialize && (bitmap[byte] & mask) != mask) {
			return EXT4_CORRUPT;
		}
		bitmap[byte] |= mask;
	}
	return EXT4_OK;
}

static enum ext4_result
ext4_allocation_protect(
    struct ext4_allocation *allocation, uint64_t first, uint32_t available, bool initialize)
{
	struct ext4_fs *fs = allocation->fs;
	const struct ext4_block_range *range;
	uint64_t end = first + (uint64_t)available * fs->cluster_blocks;
	uint64_t lower;
	uint64_t upper;
	size_t low = 0;
	size_t high = fs->system_range_count;
	size_t middle;
	enum ext4_result error;

	/* Exclude the incomplete final cluster and all bitmap padding. */
	error = ext4_bitmap_protect(
	    allocation->bitmap, available, fs->info.block_size * EXT4_BITS_PER_BYTE, initialize);
	if (error != EXT4_OK) {
		return error;
	}
	/* Find the first protected range touching this group, then visit each
	 * intersection once instead of searching the range index for every bit. */
	while (low < high) {
		middle = low + (high - low) / 2U;
		range = &fs->system_ranges[middle];
		if (range->first + range->length <= first) {
			low = middle + 1U;
		} else {
			high = middle;
		}
	}
	for (; low < fs->system_range_count; low++) {
		range = &fs->system_ranges[low];
		if (range->first >= end) {
			break;
		}
		lower = range->first < first ? 0 : range->first - first;
		upper = range->first + range->length;
		upper = (upper > end ? end : upper) - first;
		error = ext4_bitmap_protect(allocation->bitmap,
		    (uint32_t)(lower / fs->cluster_blocks),
		    (uint32_t)((upper + fs->cluster_blocks - 1U) / fs->cluster_blocks), initialize);
		if (error != EXT4_OK) {
			return error;
		}
	}
	return EXT4_OK;
}

static enum ext4_result
ext4_allocation_bitmap(struct ext4_allocation *allocation)
{
	static const uint8_t population[] = { 0, 1, 1, 2, 1, 2, 2, 3, 1, 2, 2, 3, 2, 3, 3, 4 };
	struct ext4_fs *fs = allocation->fs;
	struct ext4_group *group = &allocation->group;
	uint64_t first =
	    fs->first_data_block + (uint64_t)allocation->group_index * fs->blocks_per_group;
	uint64_t available = fs->info.blocks - first;
	uint32_t free_blocks = 0;
	uint32_t byte;
	uint8_t value;
	bool uninitialized = (group->flags & EXT4_GROUP_BLOCK_UNINIT) != 0;
	enum ext4_result error;

	if (uninitialized) {
		if (!(fs->info.feature_ro_compat & EXT4_GROUP_CHECKSUM_FEATURES)) {
			return EXT4_CORRUPT;
		}
		ext4_zero(allocation->bitmap, fs->info.block_size);
	} else {
		error = ext4_allocation_checksum(allocation, group, allocation->bitmap);
		if (error != EXT4_OK) {
			return error;
		}
	}
	if (available > fs->blocks_per_group) {
		available = fs->blocks_per_group;
	}
	available /= fs->cluster_blocks;
	error = ext4_allocation_protect(allocation, first, (uint32_t)available, uninitialized);
	if (error != EXT4_OK) {
		return error;
	}
	for (byte = 0; byte < fs->info.block_size; byte++) {
		value = allocation->bitmap[byte];
		free_blocks +=
		    (EXT4_BITS_PER_BYTE - population[value & 0x0fU] - population[value >> 4U]) *
		    fs->cluster_blocks;
	}
	if (free_blocks != group->free_blocks) {
		return EXT4_CORRUPT;
	}
	group->flags &= (uint16_t)~EXT4_GROUP_BLOCK_UNINIT;
	return EXT4_OK;
}

static void
ext4_allocation_account(struct ext4_allocation *allocation)
{
	struct ext4_fs *fs = allocation->fs;
	struct ext4_group_disk *disk = allocation->descriptor;
	struct ext4_group *group = &allocation->group;
	uint32_t checksum;

	ext4_encode16(&disk->free_blocks_lo, (uint16_t)(group->free_blocks / fs->cluster_blocks));
	ext4_encode16(&disk->flags, group->flags);
	ext4_encode32(&allocation->super->free_blocks_lo, (uint32_t)allocation->free_blocks);
	if (fs->info.feature_incompat & EXT4_FEATURE_INCOMPAT_64BIT) {
		ext4_encode16(&disk->free_blocks_hi,
		    (uint16_t)((group->free_blocks / fs->cluster_blocks) >> 16));
		ext4_encode32(
		    &allocation->super->free_blocks_hi, (uint32_t)(allocation->free_blocks >> 32));
	}
	if (fs->metadata_checksum) {
		checksum = ext4_crc32c(fs->checksum_seed, allocation->bitmap,
		    fs->clusters_per_group / EXT4_BITS_PER_BYTE);
		ext4_encode16(&disk->block_bitmap_checksum_lo, (uint16_t)checksum);
		if (fs->descriptor_size >= EXT4_GROUP_64_SIZE) {
			ext4_encode16(&disk->block_bitmap_checksum_hi, (uint16_t)(checksum >> 16));
		}
	}
	ext4_group_checksum_set(fs, allocation->group_index, disk);
}

static bool
ext4_allocation_excluded(const struct ext4_allocation *allocation, uint64_t block)
{
	return ext4_ranges_overlap(allocation->excluded, allocation->excluded_count, block,
	    allocation->fs->cluster_blocks);
}

enum ext4_result
ext4_allocate_block(struct ext4_allocation *allocation, uint64_t *block)
{
	struct ext4_fs *fs = allocation->fs;
	void *buffer;
	uint64_t offset;
	uint64_t first;
	uint32_t visited;
	uint32_t bit;
	enum ext4_result error;

	error = ext4_allocation_super(allocation);
	if (error != EXT4_OK) {
		return error;
	}
	/* Reserved space needs a separate admitted policy; ordinary writes cannot
	 * consume it merely because the adapter runs with elevated credentials. */
	if (allocation->free_blocks <= allocation->reserved_blocks ||
	    allocation->free_blocks - allocation->reserved_blocks < fs->cluster_blocks) {
		return EXT4_NO_SPACE;
	}
	for (visited = 0; visited < fs->info.groups; visited++) {
		first =
		    fs->first_data_block + (uint64_t)allocation->group_index * fs->blocks_per_group;
		if (allocation->bitmap == NULL && first <= allocation->maximum_block) {
			error = ext4_allocation_group(
			    allocation, allocation->group_index, &allocation->group, &offset);
			if (error != EXT4_OK) {
				return error;
			}
			if (allocation->group.free_blocks != 0) {
				error = ext4_transaction_buffer(
				    allocation->transaction, offset / fs->info.block_size, &buffer);
				if (error != EXT4_OK) {
					return error;
				}
				allocation->descriptor =
				    (struct ext4_group_disk *)((uint8_t *)buffer +
					offset % fs->info.block_size);
				error = ext4_transaction_buffer(allocation->transaction,
				    allocation->group.block_bitmap, &buffer);
				if (error != EXT4_OK) {
					return error;
				}
				allocation->bitmap = buffer;
				allocation->next_bit = allocation->seek_bit;
				allocation->seek_bit = 0;
				error = ext4_allocation_bitmap(allocation);
				if (error != EXT4_OK) {
					return error;
				}
			}
		}
		if (allocation->bitmap != NULL && allocation->group.free_blocks != 0) {
			for (bit = allocation->next_bit; bit < fs->clusters_per_group &&
			    first + (uint64_t)(bit + 1U) * fs->cluster_blocks - 1U <=
				allocation->maximum_block;
			    bit++) {
				if (!ext4_bitmap_test(allocation->bitmap, bit) &&
				    !ext4_allocation_excluded(
					allocation, first + (uint64_t)bit * fs->cluster_blocks)) {
					ext4_bitmap_set(allocation->bitmap, bit);
					allocation->next_bit = bit + 1;
					allocation->group.free_blocks -= fs->cluster_blocks;
					allocation->free_blocks -= fs->cluster_blocks;
					allocation->allocated += fs->cluster_blocks;
					ext4_allocation_account(allocation);
					*block = first + (uint64_t)bit * fs->cluster_blocks;
					return EXT4_OK;
				}
			}
		}
		allocation->bitmap = NULL;
		allocation->group_index++;
		if (allocation->group_index == fs->info.groups) {
			allocation->group_index = 0;
		}
	}
	return EXT4_NO_SPACE;
}

/* Find the first run of at least length clear bits in [start, end). */
static bool
ext4_bitmap_run(
    const uint8_t *bitmap, uint32_t start, uint32_t end, uint32_t length, uint32_t *found)
{
	uint32_t bit;
	uint32_t run = 0;

	for (bit = start; bit < end; bit++) {
		if (bit % EXT4_BITS_PER_BYTE == 0 && end - bit >= EXT4_BITS_PER_BYTE &&
		    bitmap[bit / EXT4_BITS_PER_BYTE] == UINT8_MAX) {
			run = 0;
			bit += EXT4_BITS_PER_BYTE - 1U;
			continue;
		}
		if (ext4_bitmap_test(bitmap, bit)) {
			run = 0;
			continue;
		}
		run++;
		if (run == length) {
			*found = bit + 1U - length;
			return true;
		}
	}
	return false;
}

bool
ext4_allocation_continues(const struct ext4_allocation *allocation)
{
	return allocation->bitmap != NULL &&
	    allocation->next_bit < allocation->fs->clusters_per_group &&
	    !ext4_bitmap_test(allocation->bitmap, allocation->next_bit);
}

/* Read a group's current block bitmap without enrolling it in the transaction. */
static enum ext4_result
ext4_allocation_peek_bitmap(struct ext4_allocation *allocation, uint32_t index,
    const struct ext4_group *group, const uint8_t **bitmap)
{
	enum ext4_result error;

	if (index == allocation->group_index && allocation->bitmap != NULL) {
		*bitmap = allocation->bitmap;
		return EXT4_OK;
	}
	*bitmap = ext4_transaction_peek(allocation->transaction, group->block_bitmap);
	if (*bitmap != NULL) {
		return EXT4_OK;
	}
	error = ext4_transaction_read(
	    allocation->transaction, group->block_bitmap, allocation->scratch);
	*bitmap = error == EXT4_OK ? allocation->scratch : NULL;
	return error;
}

/* The next ext4_allocate_block examines this cluster first. */
static void
ext4_allocation_position(struct ext4_allocation *allocation, uint32_t index, uint32_t bit)
{
	if (index == allocation->group_index && allocation->bitmap != NULL) {
		allocation->next_bit = bit;
	} else {
		allocation->bitmap = NULL;
		allocation->group_index = index;
		allocation->seek_bit = bit;
	}
}

/* Position at a free goal cluster. A group whose bitmap is not yet initialized is
 * left to the ordinary search, which initializes it. */
static enum ext4_result
ext4_allocation_goal(struct ext4_allocation *allocation, uint64_t goal, bool *positioned)
{
	struct ext4_fs *fs = allocation->fs;
	struct ext4_group group;
	const uint8_t *bitmap;
	uint64_t offset;
	uint32_t index;
	uint32_t bit;
	enum ext4_result error;

	*positioned = false;
	if (goal < fs->first_data_block || goal >= fs->info.blocks ||
	    goal > allocation->maximum_block) {
		return EXT4_OK;
	}
	index = (uint32_t)((goal - fs->first_data_block) / fs->blocks_per_group);
	bit = (uint32_t)((goal - fs->first_data_block) % fs->blocks_per_group / fs->cluster_blocks);
	if (index == allocation->group_index && allocation->bitmap != NULL) {
		group = allocation->group;
	} else {
		error = ext4_allocation_group(allocation, index, &group, &offset);
		if (error != EXT4_OK || (group.flags & EXT4_GROUP_BLOCK_UNINIT)) {
			return error;
		}
	}
	if (group.free_blocks == 0) {
		return EXT4_OK;
	}
	error = ext4_allocation_peek_bitmap(allocation, index, &group, &bitmap);
	if (error == EXT4_OK && !ext4_bitmap_test(bitmap, bit)) {
		ext4_allocation_position(allocation, index, bit);
		*positioned = true;
	}
	return error;
}

/* Position at the first free run long enough for the request, from the current
 * group onward. The chosen group is loaded and fully validated by
 * ext4_allocate_block like any other. */
static enum ext4_result
ext4_allocation_seek(struct ext4_allocation *allocation)
{
	struct ext4_fs *fs = allocation->fs;
	struct ext4_group group;
	const uint8_t *bitmap;
	uint64_t offset;
	uint64_t first;
	uint64_t clusters;
	uint32_t wanted = allocation->run_clusters;
	uint32_t index = allocation->group_index;
	uint32_t visited;
	uint32_t start;
	uint32_t end;
	uint32_t found;
	bool current;
	enum ext4_result error;

	if (wanted > EXT4_ALLOCATION_RUN_CLUSTERS) {
		wanted = EXT4_ALLOCATION_RUN_CLUSTERS;
	}
	if (wanted <= 1U) {
		return EXT4_OK;
	}
	for (visited = 0; visited < fs->info.groups; visited++) {
		first = fs->first_data_block + (uint64_t)index * fs->blocks_per_group;
		current = index == allocation->group_index && allocation->bitmap != NULL;
		if (current) {
			start = allocation->next_bit;
			group = allocation->group;
		} else {
			error = ext4_allocation_group(allocation, index, &group, &offset);
			if (error != EXT4_OK) {
				return error;
			}
			start = 0;
		}
		if (first <= allocation->maximum_block &&
		    group.free_blocks >= (uint64_t)wanted * fs->cluster_blocks) {
			if (!current && (group.flags & EXT4_GROUP_BLOCK_UNINIT)) {
				/* An untouched group is free beyond its own metadata. */
				ext4_allocation_position(allocation, index, 0);
				return EXT4_OK;
			}
			error = ext4_allocation_peek_bitmap(allocation, index, &group, &bitmap);
			if (error != EXT4_OK) {
				return error;
			}
			clusters = (fs->info.blocks - first) / fs->cluster_blocks;
			end = clusters < fs->clusters_per_group ? (uint32_t)clusters
								: fs->clusters_per_group;
			if (ext4_bitmap_run(bitmap, start, end, wanted, &found)) {
				ext4_allocation_position(allocation, index, found);
				return EXT4_OK;
			}
		}
		index++;
		if (index == fs->info.groups) {
			index = 0;
		}
	}
	return EXT4_OK;
}

enum ext4_result
ext4_allocate_data(struct ext4_allocation *allocation, uint64_t goal, uint64_t *block)
{
	bool positioned = false;
	enum ext4_result error = EXT4_OK;

	if (!ext4_allocation_continues(allocation)) {
		if (goal != 0) {
			error = ext4_allocation_goal(allocation, goal, &positioned);
		}
		if (error == EXT4_OK && !positioned) {
			error = ext4_allocation_seek(allocation);
		}
	}
	if (error != EXT4_OK) {
		return error;
	}
	return ext4_allocate_block(allocation, block);
}

enum ext4_result
ext4_allocation_claim(struct ext4_allocation *allocation, uint64_t block, uint64_t length)
{
	struct ext4_fs *fs = allocation->fs;
	void *buffer;
	uint64_t relative;
	uint64_t offset;
	uint64_t chunk;
	uint32_t index;
	uint32_t bit;
	uint32_t group_index;
	enum ext4_result error;

	if (block < fs->first_data_block || block >= fs->info.blocks || length == 0 ||
	    length > fs->info.blocks - block) {
		return EXT4_CORRUPT;
	}
	relative = (block - fs->first_data_block) % fs->cluster_blocks;
	block -= relative;
	length =
	    (length + relative + fs->cluster_blocks - 1U) / fs->cluster_blocks * fs->cluster_blocks;
	if (length > fs->info.blocks - block || ext4_system_overlaps(fs, block, length)) {
		return EXT4_CORRUPT;
	}
	error = ext4_allocation_super(allocation);
	if (error != EXT4_OK) {
		return error;
	}
	while (length != 0) {
		relative = block - fs->first_data_block;
		group_index = (uint32_t)(relative / fs->blocks_per_group);
		bit = (uint32_t)(relative % fs->blocks_per_group);
		chunk = fs->blocks_per_group - bit;
		if (chunk > length) {
			chunk = length;
		}
		/* Reload after a different inode or bitmap operation shared the same
		 * descriptor snapshot. No cached group counts cross this boundary. */
		allocation->bitmap = NULL;
		allocation->group_index = group_index;
		error = ext4_allocation_group(allocation, group_index, &allocation->group, &offset);
		if (error != EXT4_OK) {
			return error;
		}
		error = ext4_transaction_buffer(
		    allocation->transaction, offset / fs->info.block_size, &buffer);
		if (error != EXT4_OK) {
			return error;
		}
		allocation->descriptor =
		    (struct ext4_group_disk *)((uint8_t *)buffer + offset % fs->info.block_size);
		error = ext4_transaction_buffer(
		    allocation->transaction, allocation->group.block_bitmap, &buffer);
		if (error != EXT4_OK) {
			return error;
		}
		allocation->bitmap = buffer;
		error = ext4_allocation_bitmap(allocation);
		if (error != EXT4_OK) {
			return error;
		}
		for (index = bit / fs->cluster_blocks; index < (bit + chunk) / fs->cluster_blocks;
		    index++) {
			if (!ext4_bitmap_test(allocation->bitmap, index)) {
				if (allocation->group.free_blocks < fs->cluster_blocks ||
				    allocation->free_blocks < fs->cluster_blocks) {
					return EXT4_CORRUPT;
				}
				ext4_bitmap_set(allocation->bitmap, index);
				allocation->group.free_blocks -= fs->cluster_blocks;
				allocation->free_blocks -= fs->cluster_blocks;
			}
		}
		ext4_allocation_account(allocation);
		allocation->bitmap = NULL;
		block += chunk;
		length -= chunk;
	}
	return EXT4_OK;
}

enum ext4_result
ext4_allocation_valid_range(struct ext4_allocation *allocation, uint64_t block, uint64_t length)
{
	struct ext4_fs *fs = allocation->fs;
	struct ext4_group group;
	const uint8_t *bitmap;
	uint64_t offset;
	uint64_t relative;
	uint64_t chunk;
	uint32_t group_index;
	uint32_t bit;
	uint32_t index;
	enum ext4_result error;

	if (block < fs->first_data_block || block >= fs->info.blocks || length == 0 ||
	    length > fs->info.blocks - block || ext4_system_overlaps(fs, block, length)) {
		return EXT4_CORRUPT;
	}
	while (length != 0) {
		relative = block - fs->first_data_block;
		group_index = (uint32_t)(relative / fs->blocks_per_group);
		bit = (uint32_t)(relative % fs->blocks_per_group);
		chunk = fs->blocks_per_group - bit;
		if (chunk > length) {
			chunk = length;
		}
		/* Transaction snapshots were verified when enrolled and keep their
		 * checksum current; a committed bitmap is verified once per group. A
		 * validated group's bitmap location and initialization cannot change. */
		if (group_index != allocation->validated_group) {
			error = ext4_allocation_group(allocation, group_index, &group, &offset);
			if (error != EXT4_OK) {
				return error;
			}
			if (group.flags & EXT4_GROUP_BLOCK_UNINIT) {
				return EXT4_CORRUPT;
			}
			allocation->validated_group = UINT32_MAX;
			if (ext4_transaction_peek(allocation->transaction, group.block_bitmap) ==
			    NULL) {
				if (allocation->validated == NULL) {
					allocation->validated = fs->environment.allocate(
					    fs->environment.context, fs->info.block_size);
					if (allocation->validated == NULL) {
						return EXT4_NO_MEMORY;
					}
				}
				error =
				    ext4_block_read(fs, group.block_bitmap, allocation->validated);
				if (error == EXT4_OK) {
					error = ext4_allocation_checksum(
					    allocation, &group, allocation->validated);
				}
				if (error != EXT4_OK) {
					return error;
				}
			}
			allocation->validated_bitmap = group.block_bitmap;
			allocation->validated_group = group_index;
		}
		/* A bitmap enrolled after the committed copy was read supersedes it. */
		bitmap =
		    ext4_transaction_peek(allocation->transaction, allocation->validated_bitmap);
		if (bitmap == NULL) {
			bitmap = allocation->validated;
		}
		for (index = bit / fs->cluster_blocks;
		    index <= (bit + chunk - 1U) / fs->cluster_blocks; index++) {
			if (!ext4_bitmap_test(bitmap, index)) {
				return EXT4_CORRUPT;
			}
		}
		block += chunk;
		length -= chunk;
	}
	return EXT4_OK;
}

enum ext4_result
ext4_allocation_valid(struct ext4_allocation *allocation, uint64_t block)
{
	return ext4_allocation_valid_range(allocation, block, 1);
}

enum ext4_result
ext4_free_blocks(struct ext4_allocation *allocation, uint64_t block, uint64_t length)
{
	struct ext4_fs *fs = allocation->fs;
	void *buffer;
	uint64_t offset;
	uint64_t relative;
	uint64_t chunk;
	uint32_t group_index;
	uint32_t bit;
	uint32_t index;
	enum ext4_result error;

	if (block < fs->first_data_block || block >= fs->info.blocks || length == 0 ||
	    length > fs->info.blocks - block || ext4_system_overlaps(fs, block, length)) {
		return EXT4_CORRUPT;
	}
	/* Metadata owns complete clusters. Data callers first prove that no
	 * surviving extent still references either boundary cluster. */
	relative = (block - fs->first_data_block) % fs->cluster_blocks;
	block -= relative;
	length =
	    (length + relative + fs->cluster_blocks - 1U) / fs->cluster_blocks * fs->cluster_blocks;
	if (length > fs->info.blocks - block || ext4_system_overlaps(fs, block, length)) {
		return EXT4_CORRUPT;
	}
	error = ext4_allocation_super(allocation);
	if (error != EXT4_OK) {
		return error;
	}
	ext4_transaction_freed(allocation->transaction, block, length);
	while (length != 0) {
		relative = block - fs->first_data_block;
		group_index = (uint32_t)(relative / fs->blocks_per_group);
		bit = (uint32_t)(relative % fs->blocks_per_group);
		chunk = fs->blocks_per_group - bit;
		if (chunk > length) {
			chunk = length;
		}
		if (allocation->bitmap == NULL || allocation->group_index != group_index) {
			allocation->bitmap = NULL;
			allocation->group_index = group_index;
			error = ext4_allocation_group(
			    allocation, group_index, &allocation->group, &offset);
			if (error != EXT4_OK) {
				return error;
			}
			if (allocation->group.flags & EXT4_GROUP_BLOCK_UNINIT) {
				return EXT4_CORRUPT;
			}
			error = ext4_transaction_buffer(
			    allocation->transaction, offset / fs->info.block_size, &buffer);
			if (error != EXT4_OK) {
				return error;
			}
			allocation->descriptor = (struct ext4_group_disk *)((uint8_t *)buffer +
			    offset % fs->info.block_size);
			error = ext4_transaction_buffer(
			    allocation->transaction, allocation->group.block_bitmap, &buffer);
			if (error != EXT4_OK) {
				return error;
			}
			allocation->bitmap = buffer;
			error = ext4_allocation_bitmap(allocation);
			if (error != EXT4_OK) {
				return error;
			}
		}
		for (index = bit / fs->cluster_blocks; index < (bit + chunk) / fs->cluster_blocks;
		    index++) {
			if (!ext4_bitmap_test(allocation->bitmap, index)) {
				return EXT4_CORRUPT;
			}
			allocation->bitmap[index / EXT4_BITS_PER_BYTE] &=
			    (uint8_t)~(1U << (index % EXT4_BITS_PER_BYTE));
		}
		if (chunk > fs->blocks_per_group - allocation->group.free_blocks ||
		    chunk > fs->info.blocks - allocation->free_blocks) {
			return EXT4_CORRUPT;
		}
		allocation->group.free_blocks += (uint32_t)chunk;
		allocation->free_blocks += chunk;
		allocation->freed += chunk;
		allocation->next_bit = 0;
		ext4_allocation_account(allocation);
		block += chunk;
		length -= chunk;
	}
	return EXT4_OK;
}

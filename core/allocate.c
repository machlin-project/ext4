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
		    fs->checksum_seed, bitmap, fs->blocks_per_group / EXT4_BITS_PER_BYTE);
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
	ext4_zero(allocation, sizeof(*allocation));
	allocation->fs = fs;
	allocation->transaction = transaction;
	allocation->free_blocks = fs->info.free_blocks;
	allocation->free_inodes = fs->info.free_inodes;
	allocation->maximum_block =
	    (inode->flags & EXT4_INODE_EXTENTS) ? EXT4_PHYSICAL_BLOCK_MAX : UINT32_MAX;
	if (!(fs->journal->features & EXT4_JBD_64BIT)) {
		allocation->maximum_block = UINT32_MAX;
	}
	allocation->group_index = (inode->number - 1) / fs->inodes_per_group;
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
ext4_allocation_bitmap(struct ext4_allocation *allocation)
{
	struct ext4_fs *fs = allocation->fs;
	struct ext4_group *group = &allocation->group;
	uint64_t first =
	    fs->first_data_block + (uint64_t)allocation->group_index * fs->blocks_per_group;
	uint64_t available = fs->info.blocks - first;
	uint32_t free_blocks = 0;
	uint32_t bit;
	bool uninitialized = (group->flags & EXT4_GROUP_BLOCK_UNINIT) != 0;
	bool system;
	bool used;
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
	for (bit = 0; bit < fs->info.block_size * EXT4_BITS_PER_BYTE; bit++) {
		system = bit >= available || ext4_system_block(fs, first + bit);
		if (uninitialized && system) {
			ext4_bitmap_set(allocation->bitmap, bit);
		}
		used = ext4_bitmap_test(allocation->bitmap, bit);
		if (system && !used) {
			return EXT4_CORRUPT;
		}
		if (!used) {
			free_blocks++;
		}
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

	ext4_encode16(&disk->free_blocks_lo, (uint16_t)group->free_blocks);
	ext4_encode16(&disk->flags, group->flags);
	ext4_encode32(&allocation->super->free_blocks_lo, (uint32_t)allocation->free_blocks);
	if (fs->info.feature_incompat & EXT4_FEATURE_INCOMPAT_64BIT) {
		ext4_encode16(&disk->free_blocks_hi, (uint16_t)(group->free_blocks >> 16));
		ext4_encode32(
		    &allocation->super->free_blocks_hi, (uint32_t)(allocation->free_blocks >> 32));
	}
	if (fs->metadata_checksum) {
		checksum = ext4_crc32c(fs->checksum_seed, allocation->bitmap,
		    fs->blocks_per_group / EXT4_BITS_PER_BYTE);
		ext4_encode16(&disk->block_bitmap_checksum_lo, (uint16_t)checksum);
		if (fs->descriptor_size >= EXT4_GROUP_64_SIZE) {
			ext4_encode16(&disk->block_bitmap_checksum_hi, (uint16_t)(checksum >> 16));
		}
	}
	ext4_group_checksum_set(fs, allocation->group_index, disk);
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
	if (allocation->free_blocks <= allocation->reserved_blocks) {
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
				allocation->next_bit = 0;
				error = ext4_allocation_bitmap(allocation);
				if (error != EXT4_OK) {
					return error;
				}
			}
		}
		if (allocation->bitmap != NULL && allocation->group.free_blocks != 0) {
			for (bit = allocation->next_bit;
			    bit < fs->blocks_per_group && first + bit <= allocation->maximum_block;
			    bit++) {
				if (!ext4_bitmap_test(allocation->bitmap, bit)) {
					ext4_bitmap_set(allocation->bitmap, bit);
					allocation->next_bit = bit + 1;
					allocation->group.free_blocks--;
					allocation->free_blocks--;
					allocation->allocated++;
					ext4_allocation_account(allocation);
					*block = first + bit;
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

enum ext4_result
ext4_allocation_valid_range(struct ext4_allocation *allocation, uint64_t block, uint64_t length)
{
	struct ext4_fs *fs = allocation->fs;
	struct ext4_group group;
	uint64_t offset;
	uint64_t relative;
	uint64_t chunk;
	uint32_t bit;
	uint32_t index;
	enum ext4_result error;

	if (block < fs->first_data_block || block >= fs->info.blocks || length == 0 ||
	    length > fs->info.blocks - block || ext4_system_overlaps(fs, block, length)) {
		return EXT4_CORRUPT;
	}
	while (length != 0) {
		relative = block - fs->first_data_block;
		bit = (uint32_t)(relative % fs->blocks_per_group);
		chunk = fs->blocks_per_group - bit;
		if (chunk > length) {
			chunk = length;
		}
		error = ext4_allocation_group(
		    allocation, (uint32_t)(relative / fs->blocks_per_group), &group, &offset);
		if (error != EXT4_OK) {
			return error;
		}
		if (group.flags & EXT4_GROUP_BLOCK_UNINIT) {
			return EXT4_CORRUPT;
		}
		error = ext4_transaction_read(
		    allocation->transaction, group.block_bitmap, allocation->scratch);
		if (error == EXT4_OK) {
			error = ext4_allocation_checksum(allocation, &group, allocation->scratch);
		}
		if (error != EXT4_OK) {
			return error;
		}
		for (index = 0; index < chunk; index++) {
			if (!ext4_bitmap_test(allocation->scratch, bit + index)) {
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
		for (index = 0; index < chunk; index++) {
			if (!ext4_bitmap_test(allocation->bitmap, bit + index)) {
				return EXT4_CORRUPT;
			}
			allocation->bitmap[(bit + index) / EXT4_BITS_PER_BYTE] &=
			    (uint8_t)~(1U << ((bit + index) % EXT4_BITS_PER_BYTE));
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

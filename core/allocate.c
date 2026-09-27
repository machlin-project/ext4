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

	*offset = (uint64_t)(fs->first_data_block + 1) * fs->info.block_size +
	    (uint64_t)index * fs->descriptor_size;
	error = ext4_transaction_read(
	    allocation->transaction, *offset / fs->info.block_size, allocation->scratch);
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

static enum ext4_result
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
		if (!fs->metadata_checksum) {
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

	group->free_blocks--;
	allocation->free_blocks--;
	allocation->allocated++;
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
		ext4_group_checksum_set(fs, allocation->group_index, disk);
	}
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
ext4_allocation_valid(struct ext4_allocation *allocation, uint64_t block)
{
	struct ext4_fs *fs = allocation->fs;
	struct ext4_group group;
	uint64_t offset;
	uint64_t relative;
	uint32_t bit;
	enum ext4_result error;

	if (block < fs->first_data_block || block >= fs->info.blocks ||
	    ext4_system_block(fs, block)) {
		return EXT4_CORRUPT;
	}
	relative = block - fs->first_data_block;
	bit = (uint32_t)(relative % fs->blocks_per_group);
	error = ext4_allocation_group(
	    allocation, (uint32_t)(relative / fs->blocks_per_group), &group, &offset);
	if (error != EXT4_OK) {
		return error;
	}
	if (group.flags & EXT4_GROUP_BLOCK_UNINIT) {
		return EXT4_CORRUPT;
	}
	error =
	    ext4_transaction_read(allocation->transaction, group.block_bitmap, allocation->scratch);
	if (error == EXT4_OK) {
		error = ext4_allocation_checksum(allocation, &group, allocation->scratch);
	}
	if (error == EXT4_OK && !ext4_bitmap_test(allocation->scratch, bit)) {
		error = EXT4_CORRUPT;
	}
	return error;
}

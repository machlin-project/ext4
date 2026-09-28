/* SPDX-License-Identifier: BSD-3-Clause */
#include "allocate.h"

static enum ext4_result
ext4_inode_bitmap_prepare(struct ext4_fs *fs, uint32_t index, const struct ext4_group *group,
    uint32_t unused, uint8_t *bitmap, uint32_t *selected)
{
	uint32_t first = index * fs->inodes_per_group;
	uint32_t available = fs->info.inodes - first;
	uint32_t free_inodes = 0;
	uint32_t checksum;
	uint32_t bit;
	uint32_t number;
	bool uninitialized = (group->flags & EXT4_GROUP_INODE_UNINIT) != 0;
	bool reserved;
	bool used;

	if (available > fs->inodes_per_group) {
		available = fs->inodes_per_group;
	}
	if (unused > fs->inodes_per_group ||
	    (uninitialized &&
		(!(fs->info.feature_ro_compat & EXT4_GROUP_CHECKSUM_FEATURES) || index == 0))) {
		return EXT4_CORRUPT;
	}
	if (uninitialized) {
		ext4_zero(bitmap, fs->info.block_size);
	} else if (fs->metadata_checksum) {
		checksum = ext4_crc32c(
		    fs->checksum_seed, bitmap, fs->inodes_per_group / EXT4_BITS_PER_BYTE);
		if (fs->descriptor_size < EXT4_GROUP_64_SIZE) {
			checksum &= UINT16_MAX;
		}
		if (checksum != group->inode_bitmap_checksum) {
			return EXT4_CORRUPT;
		}
	}
	*selected = UINT32_MAX;
	for (bit = 0; bit < fs->info.block_size * EXT4_BITS_PER_BYTE; bit++) {
		number = bit < available ? first + bit + 1 : 0;
		reserved = bit >= available || number < fs->first_inode ||
		    number == fs->journal_inode || number == fs->orphan_file_inode;
		if (uninitialized && reserved) {
			/* A private allocated inode cannot belong to an uninitialized group. */
			if (bit < available) {
				return EXT4_CORRUPT;
			}
			bitmap[bit / EXT4_BITS_PER_BYTE] |=
			    (uint8_t)(1U << (bit % EXT4_BITS_PER_BYTE));
		}
		used = (bitmap[bit / EXT4_BITS_PER_BYTE] & (1U << (bit % EXT4_BITS_PER_BYTE))) != 0;
		if ((!used && reserved) ||
		    (used && bit < available && bit >= fs->inodes_per_group - unused)) {
			return EXT4_CORRUPT;
		}
		if (!used) {
			free_inodes++;
			if (*selected == UINT32_MAX) {
				*selected = bit;
			}
		}
	}
	if (free_inodes != group->free_inodes || *selected == UINT32_MAX) {
		return EXT4_CORRUPT;
	}
	return EXT4_OK;
}

enum ext4_result
ext4_allocate_inode(struct ext4_allocation *allocation, uint16_t mode,
    struct ext4_inode_disk **result, struct ext4_inode *inode)
{
	struct ext4_fs *fs = allocation->fs;
	struct ext4_group group;
	struct ext4_group_disk *descriptor;
	struct ext4_inode_disk *disk;
	struct ext4_extent_header_disk *extent;
	uint8_t *bitmap;
	void *buffer;
	uint64_t offset = 0;
	uint32_t index = allocation->group_index;
	uint32_t visited;
	uint32_t within;
	uint32_t number;
	uint32_t generation;
	uint32_t directories;
	uint32_t available;
	uint32_t unused;
	uint32_t checksum;
	uint16_t extra = 0;
	uint16_t desired;
	enum ext4_result error;

	if (mode != EXT4_MODE_REGULAR && mode != EXT4_MODE_DIRECTORY && mode != EXT4_MODE_SYMLINK &&
	    mode != EXT4_MODE_CHARACTER && mode != EXT4_MODE_BLOCK && mode != EXT4_MODE_FIFO &&
	    mode != EXT4_MODE_SOCKET) {
		return EXT4_INVALID_ARGUMENT;
	}
	error = ext4_allocation_super(allocation);
	if (error != EXT4_OK) {
		return error;
	}
	if (ext4_le32(&allocation->super->free_inodes) != fs->info.free_inodes) {
		return EXT4_CORRUPT;
	}
	if (fs->info.free_inodes == 0) {
		return EXT4_NO_SPACE;
	}
	if (fs->inode_size > EXT4_INODE_BASE_SIZE) {
		extra = sizeof(struct ext4_inode_disk) - EXT4_INODE_BASE_SIZE;
		desired = ext4_le16(&allocation->super->min_extra_inode_size);
		if (desired > extra) {
			extra = desired;
		}
		desired = ext4_le16(&allocation->super->want_extra_inode_size);
		if (desired > extra) {
			extra = desired;
		}
		if ((extra & 3U) || extra > fs->inode_size - EXT4_INODE_BASE_SIZE) {
			return EXT4_UNSUPPORTED;
		}
	}
	for (visited = 0; visited < fs->info.groups; visited++) {
		error = ext4_group_descriptor_offset(fs, index, &offset);
		if (error == EXT4_OK) {
			error = ext4_transaction_read(allocation->transaction,
			    offset / fs->info.block_size, allocation->scratch);
		}
		if (error != EXT4_OK) {
			return error;
		}
		descriptor =
		    (struct ext4_group_disk *)(allocation->scratch + offset % fs->info.block_size);
		error = ext4_group_decode(fs, index, descriptor, &group);
		if (error != EXT4_OK) {
			return error;
		}
		if (group.free_inodes != 0) {
			break;
		}
		index++;
		if (index == fs->info.groups) {
			index = 0;
		}
	}
	if (visited == fs->info.groups) {
		/* A positive primary summary must have an available group. */
		return EXT4_CORRUPT;
	}
	error =
	    ext4_transaction_buffer(allocation->transaction, offset / fs->info.block_size, &buffer);
	if (error != EXT4_OK) {
		return error;
	}
	/* The descriptor snapshot is shared with block allocation in this transaction. */
	descriptor = (struct ext4_group_disk *)((uint8_t *)buffer + offset % fs->info.block_size);
	unused = ext4_le16(&descriptor->unused_inodes_lo);
	directories = ext4_le16(&descriptor->used_directories_lo);
	if (fs->descriptor_size >= EXT4_GROUP_64_SIZE) {
		unused |= (uint32_t)ext4_le16(&descriptor->unused_inodes_hi) << 16;
		directories |= (uint32_t)ext4_le16(&descriptor->used_directories_hi) << 16;
	}
	available = fs->info.inodes - index * fs->inodes_per_group;
	if (available > fs->inodes_per_group) {
		available = fs->inodes_per_group;
	}
	if (group.free_inodes > available || directories > available - group.free_inodes) {
		return EXT4_CORRUPT;
	}
	error = ext4_transaction_buffer(allocation->transaction, group.inode_bitmap, &buffer);
	if (error != EXT4_OK) {
		return error;
	}
	bitmap = buffer;
	error = ext4_inode_bitmap_prepare(fs, index, &group, unused, bitmap, &within);
	if (error != EXT4_OK) {
		return error;
	}
	number = index * fs->inodes_per_group + within + 1;
	offset = group.inode_table * fs->info.block_size + (uint64_t)within * fs->inode_size;
	error =
	    ext4_transaction_buffer(allocation->transaction, offset / fs->info.block_size, &buffer);
	if (error != EXT4_OK) {
		return error;
	}
	disk = (struct ext4_inode_disk *)((uint8_t *)buffer + offset % fs->info.block_size);
	generation = ext4_le32(&disk->generation) + 1;
	if (generation == 0) {
		generation = 1;
	}
	ext4_zero(disk, fs->inode_size);
	ext4_encode16(&disk->mode, mode);
	ext4_encode16(&disk->links, mode == EXT4_MODE_DIRECTORY ? 2 : 1);
	ext4_encode32(&disk->generation, generation);
	if (extra != 0) {
		ext4_encode16(&disk->extra_size, extra);
	}
	if ((fs->info.feature_incompat & EXT4_FEATURE_INCOMPAT_EXTENTS) &&
	    (mode == EXT4_MODE_REGULAR || mode == EXT4_MODE_DIRECTORY ||
		mode == EXT4_MODE_SYMLINK)) {
		ext4_encode32(&disk->flags, EXT4_INODE_EXTENTS);
		extent = (struct ext4_extent_header_disk *)disk->block_data;
		ext4_encode16(&extent->magic, EXT4_EXTENT_MAGIC);
		ext4_encode16(&extent->maximum,
		    (sizeof(disk->block_data) - sizeof(*extent)) / sizeof(struct ext4_extent_disk));
	}
	bitmap[within / EXT4_BITS_PER_BYTE] |= (uint8_t)(1U << (within % EXT4_BITS_PER_BYTE));
	group.free_inodes--;
	group.flags &= (uint16_t)~EXT4_GROUP_INODE_UNINIT;
	if (mode == EXT4_MODE_DIRECTORY) {
		directories++;
	}
	if (unused > fs->inodes_per_group - within - 1) {
		unused = fs->inodes_per_group - within - 1;
	}
	/* Advancing the initialized high-water mark prevents Linux's lazy table
	 * zeroing from overwriting the new inode. Preserve INODE_ZEROED itself. */
	ext4_encode16(&descriptor->free_inodes_lo, (uint16_t)group.free_inodes);
	ext4_encode16(&descriptor->used_directories_lo, (uint16_t)directories);
	ext4_encode16(&descriptor->unused_inodes_lo, (uint16_t)unused);
	ext4_encode16(&descriptor->flags, group.flags);
	if (fs->descriptor_size >= EXT4_GROUP_64_SIZE) {
		ext4_encode16(&descriptor->free_inodes_hi, (uint16_t)(group.free_inodes >> 16));
		ext4_encode16(&descriptor->used_directories_hi, (uint16_t)(directories >> 16));
		ext4_encode16(&descriptor->unused_inodes_hi, (uint16_t)(unused >> 16));
	}
	if (fs->metadata_checksum) {
		checksum = ext4_crc32c(
		    fs->checksum_seed, bitmap, fs->inodes_per_group / EXT4_BITS_PER_BYTE);
		ext4_encode16(&descriptor->inode_bitmap_checksum_lo, (uint16_t)checksum);
		if (fs->descriptor_size >= EXT4_GROUP_64_SIZE) {
			ext4_encode16(
			    &descriptor->inode_bitmap_checksum_hi, (uint16_t)(checksum >> 16));
		}
	}
	ext4_group_checksum_set(fs, index, descriptor);
	ext4_encode32(&allocation->super->free_inodes, fs->info.free_inodes - 1);
	ext4_inode_checksum_set(fs, number, disk);
	error = ext4_inode_decode(fs, number, disk, inode);
	if (error == EXT4_OK) {
		*result = disk;
	}
	return error;
}

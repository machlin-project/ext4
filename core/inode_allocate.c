/* SPDX-License-Identifier: BSD-3-Clause */
#include "allocate.h"

static const uint8_t ext4_nibble_population[] = { 0, 1, 1, 2, 1, 2, 2, 3, 1, 2, 2, 3, 2, 3, 3, 4 };

/* Count clear bits in [first, end) byte by byte, reporting the lowest one. */
static uint32_t
ext4_inode_bits_clear(const uint8_t *bitmap, uint32_t first, uint32_t end, uint32_t *lowest)
{
	uint32_t count = 0;
	uint32_t base;
	uint32_t bit;
	uint8_t mask;
	uint8_t clear;

	*lowest = UINT32_MAX;
	for (bit = first; bit < end; bit = base + EXT4_BITS_PER_BYTE) {
		base = bit - bit % EXT4_BITS_PER_BYTE;
		mask = (uint8_t)(UINT8_MAX << (bit - base));
		if (end - base < EXT4_BITS_PER_BYTE) {
			mask &= (uint8_t)(UINT8_MAX >> (EXT4_BITS_PER_BYTE - (end - base)));
		}
		clear = (uint8_t)(~bitmap[base / EXT4_BITS_PER_BYTE] & mask);
		if (clear == 0) {
			continue;
		}
		if (*lowest == UINT32_MAX) {
			*lowest = base;
			while (!(clear & (1U << (*lowest - base)))) {
				(*lowest)++;
			}
		}
		count += ext4_nibble_population[clear & 0x0fU];
		count += ext4_nibble_population[clear >> 4U];
	}
	return count;
}

static bool
ext4_inode_bits_used(const uint8_t *bitmap, uint32_t first, uint32_t end)
{
	uint32_t lowest;

	return first >= end || ext4_inode_bits_clear(bitmap, first, end, &lowest) == 0;
}

static enum ext4_result
ext4_inode_bitmap_prepare(struct ext4_fs *fs, uint32_t index, const struct ext4_group *group,
    uint32_t unused, uint8_t *bitmap, uint32_t *selected)
{
	uint32_t first = index * fs->inodes_per_group;
	uint32_t available = fs->info.inodes - first;
	uint32_t capacity = fs->info.block_size * EXT4_BITS_PER_BYTE;
	uint32_t special[] = { fs->journal_inode, fs->orphan_file_inode };
	uint32_t reserved = 0;
	uint32_t initialized;
	uint32_t checksum;
	uint32_t lowest;
	uint32_t bit;
	unsigned int item;
	bool uninitialized = (group->flags & EXT4_GROUP_INODE_UNINIT) != 0;

	if (available > fs->inodes_per_group) {
		available = fs->inodes_per_group;
	}
	if (unused > fs->inodes_per_group ||
	    (uninitialized &&
		(!(fs->info.feature_ro_compat & EXT4_GROUP_CHECKSUM_FEATURES) || index == 0))) {
		return EXT4_CORRUPT;
	}
	/* Numbers below first_inode are reserved system records. */
	if (fs->first_inode > first + 1U) {
		reserved = fs->first_inode - first - 1U;
		if (reserved > available) {
			reserved = available;
		}
	}
	if (uninitialized) {
		/* A private allocated inode cannot belong to an uninitialized group. */
		if (reserved != 0) {
			return EXT4_CORRUPT;
		}
		for (item = 0; item < sizeof(special) / sizeof(special[0]); item++) {
			if (special[item] > first && special[item] - first <= available) {
				return EXT4_CORRUPT;
			}
		}
		ext4_zero(bitmap, fs->info.block_size);
		for (bit = available; bit < capacity; bit++) {
			bitmap[bit / EXT4_BITS_PER_BYTE] |=
			    (uint8_t)(1U << (bit % EXT4_BITS_PER_BYTE));
		}
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
	/* Padding and reserved records must be allocated. */
	if (!ext4_inode_bits_used(bitmap, available, capacity) ||
	    !ext4_inode_bits_used(bitmap, 0, reserved)) {
		return EXT4_CORRUPT;
	}
	for (item = 0; item < sizeof(special) / sizeof(special[0]); item++) {
		if (special[item] > first && special[item] - first <= available &&
		    !ext4_inode_bits_used(
			bitmap, special[item] - first - 1U, special[item] - first)) {
			return EXT4_CORRUPT;
		}
	}
	/* Records beyond the initialized high-water mark cannot be allocated. */
	initialized = fs->inodes_per_group - unused;
	if (initialized < available &&
	    ext4_inode_bits_clear(bitmap, initialized, available, &lowest) !=
		available - initialized) {
		return EXT4_CORRUPT;
	}
	if (ext4_inode_bits_clear(bitmap, 0, available, selected) != group->free_inodes) {
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
	uint64_t loaded = UINT64_MAX;
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
	if (ext4_le32(&allocation->super->free_inodes) != allocation->free_inodes) {
		return EXT4_CORRUPT;
	}
	if (allocation->free_inodes == 0) {
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
		/* Consecutive full groups share descriptor blocks; read each block once. */
		if (error == EXT4_OK && offset / fs->info.block_size != loaded) {
			loaded = UINT64_MAX;
			error = ext4_transaction_read(allocation->transaction,
			    offset / fs->info.block_size, allocation->scratch);
		}
		if (error != EXT4_OK) {
			return error;
		}
		loaded = offset / fs->info.block_size;
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
	if (error != EXT4_OK || within == UINT32_MAX) {
		return error == EXT4_OK ? EXT4_CORRUPT : error;
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
	allocation->free_inodes--;
	ext4_encode32(&allocation->super->free_inodes, allocation->free_inodes);
	ext4_inode_checksum_set(fs, number, disk);
	error = ext4_inode_decode(fs, number, disk, inode);
	if (error == EXT4_OK) {
		*result = disk;
	}
	return error;
}

enum ext4_result
ext4_inode_claim(struct ext4_allocation *allocation, uint32_t number, uint16_t mode, bool *created)
{
	struct ext4_fs *fs = allocation->fs;
	struct ext4_group group;
	struct ext4_group_disk *descriptor;
	uint8_t *bitmap;
	void *buffer = NULL;
	uint64_t offset;
	uint32_t index;
	uint32_t within;
	uint32_t selected;
	uint32_t unused;
	uint32_t directories;
	uint32_t available;
	uint32_t checksum;
	enum ext4_result error;

	*created = false;
	if (number == 0 || number > fs->info.inodes ||
	    (number < fs->first_inode && number != EXT4_ROOT_INODE) ||
	    number == fs->journal_inode || number == fs->orphan_file_inode) {
		return EXT4_CORRUPT;
	}
	index = (number - 1U) / fs->inodes_per_group;
	within = (number - 1U) % fs->inodes_per_group;
	error = ext4_allocation_super(allocation);
	if (error != EXT4_OK) {
		return error;
	}
	error = ext4_group_descriptor_offset(fs, index, &offset);
	if (error == EXT4_OK) {
		error = ext4_transaction_buffer(
		    allocation->transaction, offset / fs->info.block_size, &buffer);
	}
	if (error != EXT4_OK) {
		return error;
	}
	descriptor = (struct ext4_group_disk *)((uint8_t *)buffer + offset % fs->info.block_size);
	error = ext4_group_decode(fs, index, descriptor, &group);
	if (error != EXT4_OK) {
		return error;
	}
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
	if (group.free_inodes > available || directories > available - group.free_inodes ||
	    ext4_le32(&allocation->super->free_inodes) != allocation->free_inodes) {
		return EXT4_CORRUPT;
	}
	error = ext4_transaction_buffer(allocation->transaction, group.inode_bitmap, &buffer);
	if (error != EXT4_OK) {
		return error;
	}
	bitmap = buffer;
	error = ext4_inode_bitmap_prepare(fs, index, &group, unused, bitmap, &selected);
	if (error != EXT4_OK) {
		return error;
	}
	if (bitmap[within / EXT4_BITS_PER_BYTE] & (1U << (within % EXT4_BITS_PER_BYTE))) {
		return EXT4_OK;
	}
	if (group.free_inodes == 0 || allocation->free_inodes == 0) {
		return EXT4_CORRUPT;
	}
	bitmap[within / EXT4_BITS_PER_BYTE] |= (uint8_t)(1U << (within % EXT4_BITS_PER_BYTE));
	group.free_inodes--;
	group.flags &= (uint16_t)~EXT4_GROUP_INODE_UNINIT;
	if ((mode & EXT4_MODE_TYPE) == EXT4_MODE_DIRECTORY) {
		directories++;
	}
	if (unused > fs->inodes_per_group - within - 1U) {
		unused = fs->inodes_per_group - within - 1U;
	}
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
	allocation->free_inodes--;
	ext4_encode32(&allocation->super->free_inodes, allocation->free_inodes);
	/* Block and inode claims can share a descriptor; discard its cached copy. */
	allocation->bitmap = NULL;
	*created = true;
	return EXT4_OK;
}

enum ext4_result
ext4_free_inode(struct ext4_allocation *allocation, struct ext4_inode_disk *disk,
    const struct ext4_inode *inode)
{
	struct ext4_fs *fs = allocation->fs;
	struct ext4_group group;
	struct ext4_group_disk *descriptor;
	uint8_t *bitmap;
	void *buffer;
	uint64_t offset;
	uint32_t index = (inode->number - 1) / fs->inodes_per_group;
	uint32_t within = (inode->number - 1) % fs->inodes_per_group;
	uint32_t available = fs->info.inodes - index * fs->inodes_per_group;
	uint32_t capacity = fs->info.block_size * EXT4_BITS_PER_BYTE;
	uint32_t free_inodes;
	uint32_t reserved;
	uint32_t lowest;
	uint32_t directories;
	uint32_t checksum;
	enum ext4_result error;

	error = ext4_allocation_super(allocation);
	if (error != EXT4_OK) {
		return error;
	}
	if (ext4_le32(&allocation->super->free_inodes) != allocation->free_inodes ||
	    allocation->free_inodes == fs->info.inodes) {
		return EXT4_CORRUPT;
	}
	error = ext4_group_descriptor_offset(fs, index, &offset);
	if (error != EXT4_OK) {
		return error;
	}
	error =
	    ext4_transaction_buffer(allocation->transaction, offset / fs->info.block_size, &buffer);
	if (error != EXT4_OK) {
		return error;
	}
	descriptor = (struct ext4_group_disk *)((uint8_t *)buffer + offset % fs->info.block_size);
	error = ext4_group_decode(fs, index, descriptor, &group);
	if (error != EXT4_OK) {
		return error;
	}
	if (group.flags & EXT4_GROUP_INODE_UNINIT) {
		return EXT4_CORRUPT;
	}
	error = ext4_transaction_buffer(allocation->transaction, group.inode_bitmap, &buffer);
	if (error != EXT4_OK) {
		return error;
	}
	bitmap = buffer;
	if (fs->metadata_checksum) {
		checksum = ext4_crc32c(
		    fs->checksum_seed, bitmap, fs->inodes_per_group / EXT4_BITS_PER_BYTE);
		if (fs->descriptor_size < EXT4_GROUP_64_SIZE) {
			checksum &= UINT16_MAX;
		}
		if (checksum != group.inode_bitmap_checksum) {
			return EXT4_CORRUPT;
		}
	}
	if (available > fs->inodes_per_group) {
		available = fs->inodes_per_group;
	}
	reserved = index == 0 && fs->first_inode > 1U ? fs->first_inode - 1U : 0;
	if (reserved > capacity) {
		reserved = capacity;
	}
	if (!ext4_inode_bits_used(bitmap, available, capacity) ||
	    !ext4_inode_bits_used(bitmap, 0, reserved) ||
	    !ext4_inode_bits_used(bitmap, within, within + 1U)) {
		return EXT4_CORRUPT;
	}
	free_inodes = ext4_inode_bits_clear(bitmap, 0, available, &lowest);
	directories = ext4_le16(&descriptor->used_directories_lo);
	if (fs->descriptor_size >= EXT4_GROUP_64_SIZE) {
		directories |= (uint32_t)ext4_le16(&descriptor->used_directories_hi) << 16;
	}
	if (free_inodes != group.free_inodes || free_inodes >= available ||
	    directories > available - free_inodes) {
		return EXT4_CORRUPT;
	}
	if ((inode->mode & EXT4_MODE_TYPE) == EXT4_MODE_DIRECTORY) {
		if (directories == 0) {
			return EXT4_CORRUPT;
		}
		directories--;
	}
	bitmap[within / EXT4_BITS_PER_BYTE] &= (uint8_t)~(1U << (within % EXT4_BITS_PER_BYTE));
	free_inodes++;
	ext4_encode16(&descriptor->free_inodes_lo, (uint16_t)free_inodes);
	ext4_encode16(&descriptor->used_directories_lo, (uint16_t)directories);
	if (fs->descriptor_size >= EXT4_GROUP_64_SIZE) {
		ext4_encode16(&descriptor->free_inodes_hi, (uint16_t)(free_inodes >> 16));
		ext4_encode16(&descriptor->used_directories_hi, (uint16_t)(directories >> 16));
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
	allocation->free_inodes++;
	ext4_encode32(&allocation->super->free_inodes, allocation->free_inodes);
	/* Preserve the generation for the future allocator. A freed record has no
	 * live type, data pointers or invented deletion timestamp. */
	ext4_zero(disk, fs->inode_size);
	ext4_encode32(&disk->generation, inode->generation);
	ext4_inode_checksum_set(fs, inode->number, disk);
	return EXT4_OK;
}

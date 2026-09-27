/* SPDX-License-Identifier: BSD-3-Clause */
#include "internal.h"

static uint16_t
ext4_group_checksum(struct ext4_fs *fs, uint32_t group, struct ext4_group_disk *disk)
{
	struct ext4_le32 group_wire;
	uint32_t checksum;
	uint16_t saved;
	uint16_t legacy;
	size_t checksum_offset = offsetof(struct ext4_group_disk, checksum);
	size_t suffix = checksum_offset + sizeof(disk->checksum);

	ext4_encode32(&group_wire, group);
	if (fs->metadata_checksum) {
		saved = ext4_le16(&disk->checksum);
		ext4_encode16(&disk->checksum, 0);
		checksum = ext4_crc32c(fs->checksum_seed, &group_wire, sizeof(group_wire));
		checksum = ext4_crc32c(checksum, disk, fs->descriptor_size);
		ext4_encode16(&disk->checksum, saved);
		return (uint16_t)checksum;
	}
	/* CRC16 omits the checksum field rather than including two zero bytes.
	 * Extended descriptors include every byte after that skipped field. */
	legacy = ext4_crc16(UINT16_MAX, fs->info.uuid, sizeof(fs->info.uuid));
	legacy = ext4_crc16(legacy, &group_wire, sizeof(group_wire));
	legacy = ext4_crc16(legacy, disk, checksum_offset);
	return ext4_crc16(legacy, (const uint8_t *)disk + suffix, fs->descriptor_size - suffix);
}

enum ext4_result
ext4_group_decode(
    struct ext4_fs *fs, uint32_t group, struct ext4_group_disk *disk, struct ext4_group *result)
{
	struct ext4_group decoded;

	if (group >= fs->info.groups) {
		return EXT4_CORRUPT;
	}
	if ((fs->info.feature_ro_compat & EXT4_GROUP_CHECKSUM_FEATURES) &&
	    ext4_group_checksum(fs, group, disk) != ext4_le16(&disk->checksum)) {
		return EXT4_CORRUPT;
	}
	ext4_zero(&decoded, sizeof(decoded));
	decoded.block_bitmap = ext4_le32(&disk->block_bitmap_lo);
	decoded.inode_bitmap = ext4_le32(&disk->inode_bitmap_lo);
	decoded.inode_table = ext4_le32(&disk->inode_table_lo);
	decoded.block_bitmap_checksum = ext4_le16(&disk->block_bitmap_checksum_lo);
	decoded.inode_bitmap_checksum = ext4_le16(&disk->inode_bitmap_checksum_lo);
	decoded.free_blocks = ext4_le16(&disk->free_blocks_lo);
	decoded.free_inodes = ext4_le16(&disk->free_inodes_lo);
	decoded.flags = ext4_le16(&disk->flags);
	if (fs->info.feature_incompat & EXT4_FEATURE_INCOMPAT_64BIT) {
		decoded.block_bitmap |= (uint64_t)ext4_le32(&disk->block_bitmap_hi) << 32;
		decoded.inode_bitmap |= (uint64_t)ext4_le32(&disk->inode_bitmap_hi) << 32;
		decoded.inode_table |= (uint64_t)ext4_le32(&disk->inode_table_hi) << 32;
		decoded.block_bitmap_checksum |=
		    (uint32_t)ext4_le16(&disk->block_bitmap_checksum_hi) << 16;
		decoded.inode_bitmap_checksum |=
		    (uint32_t)ext4_le16(&disk->inode_bitmap_checksum_hi) << 16;
		decoded.free_blocks |= (uint32_t)ext4_le16(&disk->free_blocks_hi) << 16;
		decoded.free_inodes |= (uint32_t)ext4_le16(&disk->free_inodes_hi) << 16;
	}
	decoded.table_blocks =
	    ((uint64_t)fs->inodes_per_group * fs->inode_size + fs->info.block_size - 1) /
	    fs->info.block_size;
	if (decoded.inode_table < fs->first_data_block || decoded.inode_table >= fs->info.blocks ||
	    decoded.table_blocks > fs->info.blocks - decoded.inode_table ||
	    decoded.block_bitmap < fs->first_data_block ||
	    decoded.block_bitmap >= fs->info.blocks ||
	    decoded.inode_bitmap < fs->first_data_block ||
	    decoded.inode_bitmap >= fs->info.blocks || decoded.free_blocks > fs->blocks_per_group ||
	    decoded.free_inodes > fs->inodes_per_group) {
		return EXT4_CORRUPT;
	}
	*result = decoded;
	return EXT4_OK;
}

void
ext4_group_checksum_set(struct ext4_fs *fs, uint32_t group, struct ext4_group_disk *disk)
{
	if (!(fs->info.feature_ro_compat & EXT4_GROUP_CHECKSUM_FEATURES)) {
		return;
	}
	ext4_encode16(&disk->checksum, ext4_group_checksum(fs, group, disk));
}

enum ext4_result
ext4_group_get(struct ext4_fs *fs, uint32_t group, struct ext4_group *result)
{
	void *buffer;
	uint64_t offset;
	enum ext4_result error;

	if (group >= fs->info.groups) {
		return EXT4_CORRUPT;
	}
	buffer = fs->environment.allocate(fs->environment.context, fs->descriptor_size);
	if (buffer == NULL) {
		return EXT4_NO_MEMORY;
	}
	offset = (uint64_t)(fs->first_data_block + 1) * fs->info.block_size +
	    (uint64_t)group * fs->descriptor_size;
	error = ext4_device_read(fs, offset, buffer, fs->descriptor_size);
	if (error == EXT4_OK) {
		error = ext4_group_decode(fs, group, buffer, result);
	}
	fs->environment.release(fs->environment.context, buffer, fs->descriptor_size);
	return error;
}

enum ext4_result
ext4_inode_location(struct ext4_fs *fs, uint32_t number, uint64_t *offset)
{
	struct ext4_group group;
	enum ext4_result error;

	if (number == 0 || number > fs->info.inodes) {
		return EXT4_INVALID_ARGUMENT;
	}
	error = ext4_group_get(fs, (number - 1) / fs->inodes_per_group, &group);
	if (error == EXT4_OK) {
		*offset = group.inode_table * fs->info.block_size +
		    (uint64_t)((number - 1) % fs->inodes_per_group) * fs->inode_size;
	}
	return error;
}

static enum ext4_result
ext4_bitmap_allocated(
    struct ext4_fs *fs, uint64_t block, uint32_t bits, uint32_t expected, uint32_t index)
{
	uint8_t *buffer;
	uint32_t checksum;
	enum ext4_result error;

	if (bits % EXT4_BITS_PER_BYTE != 0 || index >= bits) {
		return EXT4_CORRUPT;
	}
	buffer = fs->environment.allocate(fs->environment.context, fs->info.block_size);
	if (buffer == NULL) {
		return EXT4_NO_MEMORY;
	}
	error = ext4_block_read(fs, block, buffer);
	if (error != EXT4_OK) {
		goto out;
	}
	if (fs->metadata_checksum) {
		checksum = ext4_crc32c(fs->checksum_seed, buffer, bits / EXT4_BITS_PER_BYTE);
		if (fs->descriptor_size < EXT4_GROUP_64_SIZE) {
			checksum &= UINT16_MAX;
		}
		if (checksum != expected) {
			error = EXT4_CORRUPT;
			goto out;
		}
	}
	if (!(buffer[index / EXT4_BITS_PER_BYTE] & (1U << (index % EXT4_BITS_PER_BYTE)))) {
		error = EXT4_CORRUPT;
	}
out:
	fs->environment.release(fs->environment.context, buffer, fs->info.block_size);
	return error;
}

enum ext4_result
ext4_inode_allocated(struct ext4_fs *fs, uint32_t number)
{
	struct ext4_group group;
	enum ext4_result error;

	error = ext4_group_get(fs, (number - 1) / fs->inodes_per_group, &group);
	if (error != EXT4_OK) {
		return error;
	}
	if (group.flags & EXT4_GROUP_INODE_UNINIT) {
		return EXT4_CORRUPT;
	}
	return ext4_bitmap_allocated(fs, group.inode_bitmap, fs->inodes_per_group,
	    group.inode_bitmap_checksum, (number - 1) % fs->inodes_per_group);
}

enum ext4_result
ext4_block_allocated(struct ext4_fs *fs, uint64_t block)
{
	struct ext4_group group;
	uint64_t relative;
	enum ext4_result error;

	if (block < fs->first_data_block || block >= fs->info.blocks) {
		return EXT4_CORRUPT;
	}
	relative = block - fs->first_data_block;
	error = ext4_group_get(fs, (uint32_t)(relative / fs->blocks_per_group), &group);
	if (error != EXT4_OK) {
		return error;
	}
	if (group.flags & EXT4_GROUP_BLOCK_UNINIT) {
		return EXT4_CORRUPT;
	}
	return ext4_bitmap_allocated(fs, group.block_bitmap, fs->blocks_per_group,
	    group.block_bitmap_checksum, (uint32_t)(relative % fs->blocks_per_group));
}

enum ext4_result
ext4_data_block_valid(struct ext4_fs *fs, uint64_t block)
{

	if (fs->system_ranges == NULL || ext4_system_block(fs, block)) {
		return EXT4_CORRUPT;
	}
	return ext4_block_allocated(fs, block);
}

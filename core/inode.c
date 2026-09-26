/* SPDX-License-Identifier: BSD-3-Clause */
#include "internal.h"

#define EXT4_TIME_EPOCH_MASK 3U
#define EXT4_TIME_NANOSECOND_SHIFT 2U
#define EXT4_NANOSECONDS_PER_SECOND 1000000000U

#define EXT4_INODE_HAS_FIELD(extra_size, field)                                                    \
	(EXT4_INODE_BASE_SIZE + (size_t)(extra_size) >= offsetof(struct ext4_inode_disk, field) +  \
		sizeof(((struct ext4_inode_disk *)0)->field))

static enum ext4_result
ext4_decode_time(uint32_t seconds, uint32_t extra, struct ext4_timestamp *time)
{
	time->seconds = seconds;
	if (seconds > INT32_MAX) {
		time->seconds -= INT64_C(1) << 32;
	}
	time->seconds += (int64_t)(extra & EXT4_TIME_EPOCH_MASK) << 32;
	time->nanoseconds = extra >> EXT4_TIME_NANOSECOND_SHIFT;
	return time->nanoseconds < EXT4_NANOSECONDS_PER_SECOND ? EXT4_OK : EXT4_CORRUPT;
}

static enum ext4_result
ext4_inode_times(const struct ext4_inode_disk *disk, uint16_t extra_size, struct ext4_inode *inode)
{
	uint32_t access_extra = 0;
	uint32_t change_extra = 0;
	uint32_t modify_extra = 0;
	uint32_t birth_extra = 0;
	enum ext4_result error;

	if (EXT4_INODE_HAS_FIELD(extra_size, access_time_extra)) {
		access_extra = ext4_le32(&disk->access_time_extra);
	}
	if (EXT4_INODE_HAS_FIELD(extra_size, change_time_extra)) {
		change_extra = ext4_le32(&disk->change_time_extra);
	}
	if (EXT4_INODE_HAS_FIELD(extra_size, modify_time_extra)) {
		modify_extra = ext4_le32(&disk->modify_time_extra);
	}
	error = ext4_decode_time(ext4_le32(&disk->access_time), access_extra, &inode->access_time);
	if (error != EXT4_OK) {
		return error;
	}
	error = ext4_decode_time(ext4_le32(&disk->change_time), change_extra, &inode->change_time);
	if (error != EXT4_OK) {
		return error;
	}
	error = ext4_decode_time(ext4_le32(&disk->modify_time), modify_extra, &inode->modify_time);
	if (error != EXT4_OK) {
		return error;
	}
	inode->birth_time_valid = EXT4_INODE_HAS_FIELD(extra_size, birth_time);
	if (inode->birth_time_valid) {
		if (EXT4_INODE_HAS_FIELD(extra_size, birth_time_extra)) {
			birth_extra = ext4_le32(&disk->birth_time_extra);
		}
		error =
		    ext4_decode_time(ext4_le32(&disk->birth_time), birth_extra, &inode->birth_time);
	}
	return error;
}

static enum ext4_result
ext4_inode_table(struct ext4_fs *fs, uint32_t group, uint64_t *table)
{
	struct ext4_group_disk *descriptor;
	struct ext4_le32 group_wire;
	uint8_t *buffer;
	uint64_t offset;
	uint64_t table_blocks;
	uint32_t checksum;
	uint16_t expected;
	enum ext4_result error;

	buffer = fs->environment.allocate(fs->environment.context, fs->descriptor_size);
	if (buffer == NULL) {
		return EXT4_NO_MEMORY;
	}
	offset = (uint64_t)(fs->first_data_block + 1) * fs->info.block_size +
	    (uint64_t)group * fs->descriptor_size;
	error = ext4_device_read(fs, offset, buffer, fs->descriptor_size);
	if (error != EXT4_OK) {
		goto out;
	}
	descriptor = (struct ext4_group_disk *)buffer;
	if (fs->metadata_checksum) {
		expected = ext4_le16(&descriptor->checksum);
		ext4_zero(&descriptor->checksum, sizeof(descriptor->checksum));
		ext4_encode32(&group_wire, group);
		checksum = ext4_crc32c(fs->checksum_seed, &group_wire, sizeof(group_wire));
		checksum = ext4_crc32c(checksum, buffer, fs->descriptor_size);
		if ((uint16_t)checksum != expected) {
			error = EXT4_CORRUPT;
			goto out;
		}
	}
	*table = ext4_le32(&descriptor->inode_table_lo);
	if (fs->info.feature_incompat & EXT4_FEATURE_INCOMPAT_64BIT) {
		*table |= (uint64_t)ext4_le32(&descriptor->inode_table_hi) << 32;
	}
	table_blocks = ((uint64_t)fs->inodes_per_group * fs->inode_size + fs->info.block_size - 1) /
	    fs->info.block_size;
	if (*table < fs->first_data_block || *table >= fs->info.blocks ||
	    table_blocks > fs->info.blocks - *table) {
		error = EXT4_CORRUPT;
	}
out:
	fs->environment.release(fs->environment.context, buffer, fs->descriptor_size);
	return error;
}

enum ext4_result
ext4_get_inode(struct ext4_fs *fs, uint32_t number, struct ext4_inode *inode)
{
	struct ext4_inode_disk *disk;
	struct ext4_inode decoded;
	uint8_t *buffer;
	uint64_t table;
	uint64_t offset;
	uint64_t xattr_block;
	uint32_t group;
	uint32_t checksum;
	uint32_t expected;
	uint16_t extra_size = 0;
	bool checksum_hi;
	enum ext4_result error;

	if (fs == NULL || inode == NULL || number == 0 || number > fs->info.inodes) {
		return EXT4_INVALID_ARGUMENT;
	}
	group = (number - 1) / fs->inodes_per_group;
	error = ext4_inode_table(fs, group, &table);
	if (error != EXT4_OK) {
		return error;
	}
	buffer = fs->environment.allocate(fs->environment.context, fs->inode_size);
	if (buffer == NULL) {
		return EXT4_NO_MEMORY;
	}
	offset = table * fs->info.block_size +
	    (uint64_t)((number - 1) % fs->inodes_per_group) * fs->inode_size;
	error = ext4_device_read(fs, offset, buffer, fs->inode_size);
	if (error != EXT4_OK) {
		goto out;
	}
	disk = (struct ext4_inode_disk *)buffer;
	ext4_zero(&decoded, sizeof(decoded));
	decoded.number = number;
	decoded.generation = ext4_le32(&disk->generation);
	if (fs->inode_size > EXT4_INODE_BASE_SIZE) {
		extra_size = ext4_le16(&disk->extra_size);
		if (extra_size > fs->inode_size - EXT4_INODE_BASE_SIZE || (extra_size & 3U)) {
			error = EXT4_CORRUPT;
			goto out;
		}
	}
	checksum_hi = extra_size >= sizeof(disk->extra_size) + sizeof(disk->checksum_hi);
	if (fs->metadata_checksum) {
		expected = ext4_le16(&disk->checksum_lo);
		ext4_zero(&disk->checksum_lo, sizeof(disk->checksum_lo));
		if (checksum_hi) {
			expected |= (uint32_t)ext4_le16(&disk->checksum_hi) << 16;
			ext4_zero(&disk->checksum_hi, sizeof(disk->checksum_hi));
		}
		checksum = ext4_crc32c(ext4_inode_seed(fs, &decoded), buffer, fs->inode_size);
		if (!checksum_hi) {
			checksum &= UINT16_MAX;
		}
		if (checksum != expected) {
			error = EXT4_CORRUPT;
			goto out;
		}
	}
	decoded.mode = ext4_le16(&disk->mode);
	decoded.links = ext4_le16(&disk->links);
	decoded.uid = ext4_le16(&disk->uid_lo) | ((uint32_t)ext4_le16(&disk->uid_hi) << 16);
	decoded.gid = ext4_le16(&disk->gid_lo) | ((uint32_t)ext4_le16(&disk->gid_hi) << 16);
	decoded.flags = ext4_le32(&disk->flags);
	decoded.size = ext4_le32(&disk->size_lo) | ((uint64_t)ext4_le32(&disk->size_hi) << 32);
	decoded.blocks_512 = ext4_le32(&disk->blocks_lo);
	if (fs->info.feature_ro_compat & EXT4_FEATURE_RO_HUGE_FILE) {
		decoded.blocks_512 |= (uint64_t)ext4_le16(&disk->blocks_hi) << 32;
		if (decoded.flags & EXT4_INODE_HUGE_FILE) {
			decoded.blocks_512 *= fs->info.block_size / 512U;
		}
	}
	error = ext4_inode_times(disk, extra_size, &decoded);
	if (error != EXT4_OK) {
		goto out;
	}
	ext4_copy(decoded.block_data, disk->block_data, sizeof(decoded.block_data));
	xattr_block =
	    ext4_le32(&disk->xattr_block_lo) | ((uint64_t)ext4_le16(&disk->xattr_block_hi) << 32);
	decoded.fast_symlink = (decoded.mode & EXT4_MODE_TYPE) == EXT4_MODE_SYMLINK &&
	    decoded.blocks_512 == (xattr_block == 0 ? 0 : fs->info.block_size / 512U);
	if (decoded.mode == 0 || decoded.links == 0) {
		error = EXT4_NOT_FOUND;
	} else if (decoded.flags & EXT4_INODE_INLINE_DATA) {
		error = EXT4_UNSUPPORTED;
	} else if ((decoded.fast_symlink && decoded.size > sizeof(decoded.block_data)) ||
	    decoded.size > (uint64_t)UINT32_MAX * fs->info.block_size) {
		error = EXT4_CORRUPT;
	} else {
		*inode = decoded;
	}
out:
	fs->environment.release(fs->environment.context, buffer, fs->inode_size);
	return error;
}

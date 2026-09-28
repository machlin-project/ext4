/* SPDX-License-Identifier: BSD-3-Clause */
#include "internal.h"
#include "xattr.h"
#include "inline.h"

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
ext4_inode_decode_record(
    struct ext4_fs *fs, uint32_t number, void *buffer, bool orphan, struct ext4_inode *inode)
{
	struct ext4_inode_disk *disk;
	const struct ext4_device_disk *device;
	struct ext4_inode decoded;
	uint64_t xattr_block;
	uint32_t checksum;
	uint32_t expected;
	uint32_t encoded_device;
	uint16_t extra_size = 0;
	bool checksum_hi;
	enum ext4_result error;

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
		ext4_encode16(&disk->checksum_lo, (uint16_t)expected);
		if (checksum_hi) {
			ext4_encode16(&disk->checksum_hi, (uint16_t)(expected >> 16));
		}
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
	if ((decoded.mode & EXT4_MODE_TYPE) == EXT4_MODE_CHARACTER ||
	    (decoded.mode & EXT4_MODE_TYPE) == EXT4_MODE_BLOCK) {
		device = (const struct ext4_device_disk *)disk->block_data;
		encoded_device = ext4_le32(&device->legacy);
		if (encoded_device != 0) {
			decoded.device_major =
			    (encoded_device >> EXT4_DEVICE_MAJOR_SHIFT) & EXT4_DEVICE_LEGACY_MASK;
			decoded.device_minor = encoded_device & EXT4_DEVICE_LEGACY_MASK;
		} else {
			encoded_device = ext4_le32(&device->extended);
			decoded.device_major =
			    (encoded_device >> EXT4_DEVICE_MAJOR_SHIFT) & EXT4_DEVICE_MAJOR_MAX;
			decoded.device_minor = (encoded_device & EXT4_DEVICE_LEGACY_MASK) |
			    ((encoded_device >> EXT4_DEVICE_MINOR_HIGH_SHIFT) &
				(EXT4_DEVICE_MINOR_MAX & ~EXT4_DEVICE_LEGACY_MASK));
		}
	}
	xattr_block =
	    ext4_le32(&disk->xattr_block_lo) | ((uint64_t)ext4_le16(&disk->xattr_block_hi) << 32);
	if ((decoded.flags & EXT4_INODE_EA_INODE) &&
	    (decoded.mode & EXT4_MODE_TYPE) != EXT4_MODE_REGULAR) {
		/* Validate before following any attributes to bound private-inode reads. */
		error = EXT4_CORRUPT;
		goto out;
	}
	decoded.fast_symlink = (decoded.mode & EXT4_MODE_TYPE) == EXT4_MODE_SYMLINK &&
	    decoded.blocks_512 ==
		(xattr_block == 0
			? 0
			: (uint64_t)fs->cluster_blocks * (fs->info.block_size / EXT4_SECTOR_SIZE));
	if (decoded.mode == 0 || (!orphan && decoded.links == 0)) {
		error = EXT4_NOT_FOUND;
	} else if ((decoded.flags & EXT4_INODE_CASEFOLD) &&
	    !(fs->info.feature_incompat & EXT4_FEATURE_INCOMPAT_CASEFOLD)) {
		/* Linux rejects a casefold flag without the volume encoding. */
		error = EXT4_CORRUPT;
	} else if ((decoded.flags & EXT4_INODE_INLINE_DATA) &&
	    (!(fs->info.feature_incompat & EXT4_FEATURE_INCOMPAT_INLINE_DATA) ||
		(decoded.flags & (EXT4_INODE_EXTENTS | EXT4_INODE_INDEX | EXT4_INODE_EA_INODE)) ||
		((decoded.mode & EXT4_MODE_TYPE) != EXT4_MODE_REGULAR &&
		    (decoded.mode & EXT4_MODE_TYPE) != EXT4_MODE_DIRECTORY))) {
		error = EXT4_CORRUPT;
	} else if ((decoded.fast_symlink && decoded.size >= sizeof(decoded.block_data)) ||
	    decoded.size > (uint64_t)UINT32_MAX * fs->info.block_size) {
		error = EXT4_CORRUPT;
	} else {
		*inode = decoded;
	}
out:
	return error;
}

enum ext4_result
ext4_inode_decode(struct ext4_fs *fs, uint32_t number, void *buffer, struct ext4_inode *inode)
{
	return ext4_inode_decode_record(fs, number, buffer, false, inode);
}

enum ext4_result
ext4_inode_decode_orphan(
    struct ext4_fs *fs, uint32_t number, void *buffer, struct ext4_inode *inode)
{
	return ext4_inode_decode_record(fs, number, buffer, true, inode);
}

enum ext4_result
ext4_inode_flags_writable(struct ext4_fs *fs, const struct ext4_inode *inode)
{
	switch (inode->mode & EXT4_MODE_TYPE) {
	case EXT4_MODE_REGULAR:
	case EXT4_MODE_DIRECTORY:
	case EXT4_MODE_SYMLINK:
	case EXT4_MODE_CHARACTER:
	case EXT4_MODE_BLOCK:
	case EXT4_MODE_FIFO:
	case EXT4_MODE_SOCKET:
		break;
	default:
		return EXT4_CORRUPT;
	}
	if (inode->flags & ~EXT4_INODE_WRITABLE_FLAGS) {
		return EXT4_UNSUPPORTED;
	}
	if (fs->cluster_blocks > 1 &&
	    !(inode->flags & (EXT4_INODE_EXTENTS | EXT4_INODE_INLINE_DATA)) &&
	    ((inode->mode & EXT4_MODE_TYPE) == EXT4_MODE_REGULAR ||
		(inode->mode & EXT4_MODE_TYPE) == EXT4_MODE_DIRECTORY ||
		((inode->mode & EXT4_MODE_TYPE) == EXT4_MODE_SYMLINK && !inode->fast_symlink))) {
		return EXT4_UNSUPPORTED;
	}
	if (((inode->flags & EXT4_INODE_EXTENTS) &&
		!(fs->info.feature_incompat & EXT4_FEATURE_INCOMPAT_EXTENTS)) ||
	    ((inode->flags & EXT4_INODE_HUGE_FILE) &&
		!(fs->info.feature_ro_compat & EXT4_FEATURE_RO_HUGE_FILE))) {
		return EXT4_CORRUPT;
	}
	return EXT4_OK;
}

bool
ext4_inode_has_xattrs(const struct ext4_fs *fs, const struct ext4_inode_disk *disk)
{
	size_t xattr_offset;
	uint16_t extra_size;

	if (ext4_le32(&disk->xattr_block_lo) != 0 || ext4_le16(&disk->xattr_block_hi) != 0) {
		return true;
	}
	extra_size = fs->inode_size > EXT4_INODE_BASE_SIZE ? ext4_le16(&disk->extra_size) : 0;
	xattr_offset = EXT4_INODE_BASE_SIZE + extra_size;
	return extra_size != 0 && xattr_offset <= fs->inode_size - sizeof(struct ext4_le32) &&
	    ext4_le32((const struct ext4_le32 *)((const uint8_t *)disk + xattr_offset)) ==
	    EXT4_XATTR_MAGIC;
}

enum ext4_result
ext4_inode_writable(
    struct ext4_fs *fs, const struct ext4_inode_disk *disk, const struct ext4_inode *inode)
{
	struct ext4_xattr_snapshot snapshot;
	enum ext4_result error = ext4_inode_flags_writable(fs, inode);

	if (error == EXT4_OK && (inode->flags & EXT4_INODE_INLINE_DATA)) {
		return ext4_inline_validate(fs, inode, disk);
	}
	if (error != EXT4_OK || !ext4_inode_has_xattrs(fs, disk)) {
		return error;
	}
	error = ext4_xattr_open_inode(fs, inode, disk, &snapshot);
	ext4_xattr_close(&snapshot);
	return error;
}

void
ext4_inode_checksum_set(struct ext4_fs *fs, uint32_t number, struct ext4_inode_disk *disk)
{
	struct ext4_inode inode;
	uint32_t checksum;
	uint16_t extra_size;
	bool checksum_hi;

	if (!fs->metadata_checksum) {
		return;
	}
	extra_size = fs->inode_size > EXT4_INODE_BASE_SIZE ? ext4_le16(&disk->extra_size) : 0;
	checksum_hi = EXT4_INODE_HAS_FIELD(extra_size, checksum_hi);
	ext4_zero(&inode, sizeof(inode));
	inode.number = number;
	inode.generation = ext4_le32(&disk->generation);
	ext4_encode16(&disk->checksum_lo, 0);
	if (checksum_hi) {
		ext4_encode16(&disk->checksum_hi, 0);
	}
	checksum = ext4_crc32c(ext4_inode_seed(fs, &inode), disk, fs->inode_size);
	ext4_encode16(&disk->checksum_lo, (uint16_t)checksum);
	if (checksum_hi) {
		ext4_encode16(&disk->checksum_hi, (uint16_t)(checksum >> 16));
	}
}

enum ext4_result
ext4_get_inode(struct ext4_fs *fs, uint32_t number, struct ext4_inode *inode)
{
	struct ext4_inode decoded = { 0 };
	void *buffer;
	uint64_t offset;
	enum ext4_result error;

	if (fs == NULL || inode == NULL) {
		return EXT4_INVALID_ARGUMENT;
	}
	if (fs->aborted) {
		return EXT4_RECOVERY_REQUIRED;
	}
	error = ext4_inode_location(fs, number, &offset);
	if (error != EXT4_OK) {
		return error;
	}
	buffer = fs->environment.allocate(fs->environment.context, fs->inode_size);
	if (buffer == NULL) {
		return EXT4_NO_MEMORY;
	}
	error = ext4_device_read(fs, offset, buffer, fs->inode_size);
	if (error == EXT4_OK) {
		error = ext4_inode_decode(fs, number, buffer, &decoded);
		if (error == EXT4_OK && (decoded.flags & EXT4_INODE_EA_INODE)) {
			/* These objects are reachable only through validated xattr entries. */
			error = EXT4_CORRUPT;
		}
	}
	fs->environment.release(fs->environment.context, buffer, fs->inode_size);
	if (error == EXT4_OK) {
		*inode = decoded;
	}
	return error;
}

static enum ext4_result
ext4_encode_time(
    const struct ext4_timestamp *time, struct ext4_le32 *seconds, struct ext4_le32 *extra)
{
	uint32_t low;
	uint32_t epoch;
	int64_t signed_low;
	int64_t maximum =
	    extra == NULL ? INT32_MAX : INT32_MAX + ((int64_t)EXT4_TIME_EPOCH_MASK << 32);

	if (time->nanoseconds >= EXT4_NANOSECONDS_PER_SECOND || time->seconds < INT32_MIN ||
	    time->seconds > maximum || (extra == NULL && time->nanoseconds != 0)) {
		return EXT4_RANGE;
	}
	low = (uint32_t)(uint64_t)time->seconds;
	signed_low = low <= INT32_MAX ? (int64_t)low : (int64_t)low - (INT64_C(1) << 32);
	epoch = (uint32_t)((time->seconds - signed_low) >> 32);
	ext4_encode32(seconds, low);
	if (extra != NULL) {
		ext4_encode32(extra, (time->nanoseconds << EXT4_TIME_NANOSECOND_SHIFT) | epoch);
	}
	return EXT4_OK;
}

enum ext4_result
ext4_inode_apply(
    struct ext4_fs *fs, struct ext4_inode_disk *disk, const struct ext4_inode_update *update)
{
	uint16_t extra_size =
	    fs->inode_size > EXT4_INODE_BASE_SIZE ? ext4_le16(&disk->extra_size) : 0;
	enum ext4_result error = EXT4_OK;

	/* The caller owns a private transaction snapshot. Any failure discards all
	 * these changes, including fields encoded before an unrepresentable time. */
	if (update->fields & EXT4_ATTR_PERMISSIONS) {
		ext4_encode16(
		    &disk->mode, (ext4_le16(&disk->mode) & EXT4_MODE_TYPE) | update->permissions);
	}
	if (update->fields & EXT4_ATTR_UID) {
		ext4_encode16(&disk->uid_lo, (uint16_t)update->uid);
		ext4_encode16(&disk->uid_hi, (uint16_t)(update->uid >> 16));
	}
	if (update->fields & EXT4_ATTR_GID) {
		ext4_encode16(&disk->gid_lo, (uint16_t)update->gid);
		ext4_encode16(&disk->gid_hi, (uint16_t)(update->gid >> 16));
	}
	if (update->fields & EXT4_ATTR_ACCESS_TIME) {
		error = ext4_encode_time(&update->access_time, &disk->access_time,
		    EXT4_INODE_HAS_FIELD(extra_size, access_time_extra) ? &disk->access_time_extra
									: NULL);
	}
	if (error == EXT4_OK && (update->fields & EXT4_ATTR_CHANGE_TIME)) {
		error = ext4_encode_time(&update->change_time, &disk->change_time,
		    EXT4_INODE_HAS_FIELD(extra_size, change_time_extra) ? &disk->change_time_extra
									: NULL);
	}
	if (error == EXT4_OK && (update->fields & EXT4_ATTR_MODIFY_TIME)) {
		error = ext4_encode_time(&update->modify_time, &disk->modify_time,
		    EXT4_INODE_HAS_FIELD(extra_size, modify_time_extra) ? &disk->modify_time_extra
									: NULL);
	}
	if (error == EXT4_OK && (update->fields & EXT4_ATTR_BIRTH_TIME)) {
		if (!EXT4_INODE_HAS_FIELD(extra_size, birth_time)) {
			return EXT4_UNSUPPORTED;
		}
		error = ext4_encode_time(&update->birth_time, &disk->birth_time,
		    EXT4_INODE_HAS_FIELD(extra_size, birth_time_extra) ? &disk->birth_time_extra
								       : NULL);
	}
	return error;
}

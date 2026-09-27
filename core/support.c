/* SPDX-License-Identifier: BSD-3-Clause */
#include "internal.h"

uint16_t
ext4_le16(const struct ext4_le16 *value)
{
	return (uint16_t)value->bytes[0] | (uint16_t)((uint16_t)value->bytes[1] << 8);
}

uint32_t
ext4_le32(const struct ext4_le32 *value)
{
	return (uint32_t)value->bytes[0] | ((uint32_t)value->bytes[1] << 8) |
	    ((uint32_t)value->bytes[2] << 16) | ((uint32_t)value->bytes[3] << 24);
}

void
ext4_encode16(struct ext4_le16 *output, uint16_t value)
{
	output->bytes[0] = (uint8_t)value;
	output->bytes[1] = (uint8_t)(value >> 8);
}

void
ext4_encode32(struct ext4_le32 *output, uint32_t value)
{
	unsigned int index;

	for (index = 0; index < sizeof(output->bytes); index++) {
		output->bytes[index] = (uint8_t)(value >> (index * 8));
	}
}

void
ext4_copy(void *destination, const void *source, size_t length)
{
	uint8_t *output = destination;
	const uint8_t *input = source;
	size_t index;

	for (index = 0; index < length; index++) {
		output[index] = input[index];
	}
}

void
ext4_zero(void *destination, size_t length)
{
	uint8_t *output = destination;
	size_t index;

	for (index = 0; index < length; index++) {
		output[index] = 0;
	}
}

bool
ext4_equal(const void *left, const void *right, size_t length)
{
	const uint8_t *a = left;
	const uint8_t *b = right;
	size_t index;

	for (index = 0; index < length; index++) {
		if (a[index] != b[index]) {
			return false;
		}
	}
	return true;
}

uint32_t
ext4_crc32c(uint32_t checksum, const void *buffer, size_t length)
{
	const uint8_t *bytes = buffer;
	size_t index;
	unsigned int bit;

	for (index = 0; index < length; index++) {
		checksum ^= bytes[index];
		for (bit = 0; bit < 8; bit++) {
			checksum =
			    (checksum >> 1) ^ (EXT4_CRC32C_POLYNOMIAL & (0U - (checksum & 1U)));
		}
	}
	return checksum;
}

uint32_t
ext4_inode_seed(const struct ext4_fs *fs, const struct ext4_inode *inode)
{
	struct ext4_le32 number;
	struct ext4_le32 generation;
	uint32_t checksum;

	ext4_encode32(&number, inode->number);
	ext4_encode32(&generation, inode->generation);
	checksum = ext4_crc32c(fs->checksum_seed, &number, sizeof(number));
	return ext4_crc32c(checksum, &generation, sizeof(generation));
}

enum ext4_result
ext4_device_read(struct ext4_fs *fs, uint64_t offset, void *buffer, size_t length)
{
	if (fs->aborted) {
		return EXT4_RECOVERY_REQUIRED;
	}
	if (offset > fs->environment.size_bytes || length > fs->environment.size_bytes - offset) {
		return EXT4_CORRUPT;
	}
	return fs->environment.read(fs->environment.context, offset, buffer, length);
}

enum ext4_result
ext4_block_read(struct ext4_fs *fs, uint64_t block, void *buffer)
{
	if (block < fs->first_data_block || block >= fs->info.blocks) {
		return EXT4_CORRUPT;
	}
	return ext4_device_read(fs, block * fs->info.block_size, buffer, fs->info.block_size);
}

const char *
ext4_result_string(enum ext4_result result)
{
	switch (result) {
	case EXT4_OK:
		return "success";
	case EXT4_INVALID_ARGUMENT:
		return "invalid argument";
	case EXT4_NOT_EXT4:
		return "not an ext4 filesystem";
	case EXT4_UNSUPPORTED:
		return "unsupported filesystem feature";
	case EXT4_CORRUPT:
		return "corrupt filesystem metadata";
	case EXT4_IO:
		return "resource I/O failed";
	case EXT4_NO_MEMORY:
		return "allocation failed";
	case EXT4_NOT_FOUND:
		return "entry not found";
	case EXT4_NOT_DIRECTORY:
		return "not a directory";
	case EXT4_NAME_TOO_LONG:
		return "name too long";
	case EXT4_READ_ONLY:
		return "read-only filesystem";
	case EXT4_RECOVERY_REQUIRED:
		return "filesystem requires recovery";
	case EXT4_IS_DIRECTORY:
		return "operation requires a nondirectory";
	case EXT4_RANGE:
		return "value out of range";
	case EXT4_STALE:
		return "inode generation changed";
	}
	return "unknown filesystem error";
}

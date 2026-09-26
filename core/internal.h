/* SPDX-License-Identifier: BSD-3-Clause */
#ifndef MACHLIN_EXT4_INTERNAL_H
#define MACHLIN_EXT4_INTERNAL_H

#include "disk.h"

struct ext4_fs {
	struct ext4_environment environment;
	struct ext4_info info;
	uint32_t first_data_block;
	uint32_t blocks_per_group;
	uint32_t inodes_per_group;
	uint32_t checksum_seed;
	uint16_t inode_size;
	uint16_t descriptor_size;
	bool metadata_checksum;
};

uint16_t ext4_le16(const struct ext4_le16 *value);
uint32_t ext4_le32(const struct ext4_le32 *value);
void ext4_encode32(struct ext4_le32 *output, uint32_t value);
void ext4_copy(void *destination, const void *source, size_t length);
void ext4_zero(void *destination, size_t length);
bool ext4_equal(const void *left, const void *right, size_t length);
uint32_t ext4_crc32c(uint32_t checksum, const void *buffer, size_t length);
uint32_t ext4_inode_seed(const struct ext4_fs *fs, const struct ext4_inode *inode);
enum ext4_result ext4_device_read(struct ext4_fs *fs, uint64_t offset, void *buffer, size_t length);
enum ext4_result ext4_block_read(struct ext4_fs *fs, uint64_t block, void *buffer);
enum ext4_result ext4_map_block(
    struct ext4_fs *fs, const struct ext4_inode *inode, uint32_t logical, uint64_t *physical);

#endif

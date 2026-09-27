/* SPDX-License-Identifier: BSD-3-Clause */
#ifndef MACHLIN_EXT4_INTERNAL_H
#define MACHLIN_EXT4_INTERNAL_H

#include "disk.h"

struct ext4_journal;
struct ext4_transaction;

struct ext4_block_range {
	uint64_t first;
	uint64_t length;
};

struct ext4_block_path {
	uint64_t blocks[EXT4_EXTENT_MAX_DEPTH];
	uint16_t count;
};

struct ext4_fs {
	struct ext4_environment environment;
	struct ext4_info info;
	uint32_t first_data_block;
	uint32_t blocks_per_group;
	uint32_t inodes_per_group;
	uint32_t checksum_seed;
	uint32_t first_inode;
	uint32_t journal_inode;
	uint16_t reserved_gdt_blocks;
	uint16_t inode_size;
	uint16_t descriptor_size;
	bool metadata_checksum;
	bool writer_attached;
	bool aborted;
	struct ext4_journal *journal;
	struct ext4_block_range *system_ranges;
	size_t system_range_capacity;
	size_t system_range_count;
};

struct ext4_group {
	uint64_t block_bitmap;
	uint64_t inode_bitmap;
	uint64_t inode_table;
	uint64_t table_blocks;
	uint32_t block_bitmap_checksum;
	uint32_t inode_bitmap_checksum;
	uint32_t free_blocks;
	uint32_t free_inodes;
	uint16_t flags;
};

/* Sort, reject overlaps and merge adjacent already-bounded physical ranges. */
enum ext4_result ext4_ranges_sort(struct ext4_block_range *ranges, size_t *count);
enum ext4_result ext4_group_get(struct ext4_fs *fs, uint32_t group, struct ext4_group *result);
enum ext4_result ext4_group_decode(
    struct ext4_fs *fs, uint32_t group, struct ext4_group_disk *disk, struct ext4_group *result);
void ext4_group_checksum_set(struct ext4_fs *fs, uint32_t group, struct ext4_group_disk *disk);
enum ext4_result ext4_system_ranges_build(struct ext4_fs *fs);
bool ext4_system_block(const struct ext4_fs *fs, uint64_t block);
bool ext4_system_overlaps(const struct ext4_fs *fs, uint64_t block, uint64_t length);
enum ext4_result ext4_inode_location(struct ext4_fs *fs, uint32_t number, uint64_t *offset);
enum ext4_result ext4_inode_decode(
    struct ext4_fs *fs, uint32_t number, void *buffer, struct ext4_inode *inode);
void ext4_inode_checksum_set(struct ext4_fs *fs, uint32_t number, struct ext4_inode_disk *disk);
enum ext4_result ext4_inode_apply(
    struct ext4_fs *fs, struct ext4_inode_disk *disk, const struct ext4_inode_update *update);
enum ext4_result ext4_inode_allocated(struct ext4_fs *fs, uint32_t number);
enum ext4_result ext4_data_block_valid(struct ext4_fs *fs, uint64_t block);

enum ext4_result ext4_load(
    const struct ext4_environment *environment, bool recovery, struct ext4_fs **result);

uint16_t ext4_le16(const struct ext4_le16 *value);
uint32_t ext4_le32(const struct ext4_le32 *value);
void ext4_encode16(struct ext4_le16 *output, uint16_t value);
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
enum ext4_result ext4_map_block_path(struct ext4_fs *fs, const struct ext4_inode *inode,
    uint32_t logical, uint64_t *physical, struct ext4_block_path *path);

#endif

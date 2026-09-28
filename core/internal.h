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

struct ext4_name_hash {
	uint32_t major;
	uint32_t minor;
};

#define EXT4_ORPHAN_FILE_MAX_BLOCKS 512U
#define EXT4_ORPHAN_FILE_MAX_ENTRIES (1U << 20)

struct ext4_orphan_file {
	struct ext4_inode inode;
	/* Data block addresses followed by unique mapping-node addresses. */
	uint64_t *blocks;
	size_t capacity;
	uint32_t block_count;
	uint32_t mapping_count;
	uint32_t pending;
};

struct ext4_inode_hold {
	struct ext4_fs *fs;
	struct ext4_inode_hold *next;
	uint32_t number;
	uint32_t generation;
	uint32_t references;
	bool unlinked;
};

struct ext4_fs {
	struct ext4_environment environment;
	struct ext4_info info;
	uint32_t first_data_block;
	uint32_t blocks_per_group;
	uint32_t cluster_blocks;
	uint32_t clusters_per_group;
	uint32_t inodes_per_group;
	uint32_t checksum_seed;
	uint32_t directory_hash_seed[4];
	uint32_t directory_hash_flags;
	uint32_t first_inode;
	uint32_t journal_inode;
	uint32_t journal_device;
	uint8_t journal_uuid[EXT4_UUID_SIZE];
	uint32_t last_orphan;
	uint32_t orphan_file_inode;
	uint32_t first_meta_group;
	uint32_t backup_groups[2];
	uint16_t reserved_gdt_blocks;
	uint16_t inode_size;
	uint16_t descriptor_size;
	bool metadata_checksum;
	bool writer_attached;
	bool aborted;
	struct ext4_journal *journal;
	struct ext4_orphan_file *orphan_file;
	struct ext4_inode_hold *holds;
	uint32_t hold_count;
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
/* Geometry is shared by reads, transactional mutation and metadata exclusion.
 * A group's fixed prefix can be empty; bitmap/table locations remain in its
 * descriptor and can belong to another group when FLEX_BG is enabled. */
bool ext4_group_has_super(const struct ext4_fs *fs, uint32_t group);
enum ext4_result ext4_group_reserved(
    const struct ext4_fs *fs, uint32_t group, struct ext4_block_range *range);
enum ext4_result ext4_group_descriptor_offset(
    const struct ext4_fs *fs, uint32_t group, uint64_t *offset);
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
/* Offline orphan ownership may inspect allocated inodes with no links. */
enum ext4_result ext4_inode_decode_orphan(
    struct ext4_fs *fs, uint32_t number, void *buffer, struct ext4_inode *inode);
enum ext4_result ext4_inode_decode_live(
    struct ext4_fs *fs, uint32_t number, void *buffer, struct ext4_inode *inode);
struct ext4_inode_hold *ext4_inode_find_hold(struct ext4_fs *fs, uint32_t number);
void ext4_inode_holds_destroy(struct ext4_fs *fs);
/* Validate mutation-compatible formats. Public operation boundaries separately
 * enforce immutable/append policy; orphan cleanup must finish accepted deletion. */
enum ext4_result ext4_inode_writable(
    struct ext4_fs *fs, const struct ext4_inode_disk *disk, const struct ext4_inode *inode);
enum ext4_result ext4_inode_flags_writable(struct ext4_fs *fs, const struct ext4_inode *inode);
bool ext4_inode_has_xattrs(const struct ext4_fs *fs, const struct ext4_inode_disk *disk);
void ext4_inode_checksum_set(struct ext4_fs *fs, uint32_t number, struct ext4_inode_disk *disk);
enum ext4_result ext4_inode_apply(
    struct ext4_fs *fs, struct ext4_inode_disk *disk, const struct ext4_inode_update *update);
enum ext4_result ext4_inode_allocated(struct ext4_fs *fs, uint32_t number);
enum ext4_result ext4_edit_inode(struct ext4_fs *fs, struct ext4_transaction *transaction,
    uint32_t number, uint32_t generation, struct ext4_inode_disk **disk, struct ext4_inode *inode);
uint32_t ext4_directory_record_length(
    struct ext4_fs *fs, const struct ext4_dir_header_disk *header);
enum ext4_result ext4_directory_entry_decode(struct ext4_fs *fs, const uint8_t *buffer,
    uint32_t offset, struct ext4_dir_entry *entry, uint32_t *record_length);
/* The caller resolves the on-disk signedness policy into a hash version.
 * Seed words are host-order; NULL/all-zero uses the specified default seed.
 * Failure leaves result unchanged. This hashes bytes without name normalization. */
enum ext4_result ext4_directory_hash(uint8_t version, const uint32_t seed[4], const uint8_t *name,
    size_t length, struct ext4_name_hash *result);
enum ext4_result ext4_directory_checksum(
    struct ext4_fs *fs, const struct ext4_inode *inode, uint32_t logical, uint8_t *buffer);
enum ext4_result ext4_orphan_cleanup(struct ext4_fs *fs, struct ext4_recovery_report *report);
enum ext4_result ext4_orphan_finish_inode(
    struct ext4_fs *fs, uint32_t number, uint32_t generation, bool retained);
enum ext4_result ext4_orphan_validate_live(struct ext4_fs *fs);
enum ext4_result ext4_orphan_validate(struct ext4_fs *fs);
enum ext4_result ext4_orphan_file_prepare(struct ext4_fs *fs);
void ext4_orphan_file_close(struct ext4_fs *fs);
enum ext4_result ext4_data_block_valid(struct ext4_fs *fs, uint64_t block);
enum ext4_result ext4_block_allocated(struct ext4_fs *fs, uint64_t block);

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
uint32_t ext4_crc32_be(uint32_t checksum, const void *buffer, size_t length);
uint16_t ext4_crc16(uint16_t checksum, const void *buffer, size_t length);
uint32_t ext4_inode_seed(const struct ext4_fs *fs, const struct ext4_inode *inode);
enum ext4_result ext4_device_read(struct ext4_fs *fs, uint64_t offset, void *buffer, size_t length);
enum ext4_result ext4_block_read(struct ext4_fs *fs, uint64_t block, void *buffer);
enum ext4_result ext4_map_block(
    struct ext4_fs *fs, const struct ext4_inode *inode, uint32_t logical, uint64_t *physical);
enum ext4_result ext4_map_block_path(struct ext4_fs *fs, const struct ext4_inode *inode,
    uint32_t logical, uint64_t *physical, struct ext4_block_path *path);

#endif

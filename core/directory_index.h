/* SPDX-License-Identifier: BSD-3-Clause */
#ifndef MACHLIN_EXT4_DIRECTORY_INDEX_H
#define MACHLIN_EXT4_DIRECTORY_INDEX_H

#include "allocate.h"

#define EXT4_DIRECTORY_MAX_BLOCKS (1U << 20)
/* An additional level requires the separately negotiated LARGEDIR format. */
#define EXT4_DX_MAX_INDIRECT_LEVELS 1U
#define EXT4_DX_HASH_END (UINT64_C(1) << 32)

enum ext4_index_kind { EXT4_INDEX_UNKNOWN, EXT4_INDEX_ROOT, EXT4_INDEX_NODE, EXT4_INDEX_LEAF };

struct ext4_index_range {
	uint64_t upper;
	uint32_t lower;
	uint32_t parent;
	uint16_t entry;
	uint16_t count;
	uint8_t level;
	uint8_t kind;
};

/* The tree belongs to the caller's private transaction view. Its range array is
 * bounded by directory size and covers every mapped logical directory block. */
struct ext4_directory_index {
	struct ext4_allocation *allocation;
	const struct ext4_inode *inode;
	struct ext4_inode_disk *disk;
	struct ext4_index_range *ranges;
	uint32_t seed[4];
	uint32_t blocks;
	uint32_t parent_number;
	uint8_t levels;
	uint8_t version;
};

enum ext4_result ext4_index_open(struct ext4_allocation *allocation, const struct ext4_inode *inode,
    struct ext4_inode_disk *disk, struct ext4_directory_index *index);
void ext4_index_close(struct ext4_directory_index *index);
enum ext4_result ext4_index_read(
    struct ext4_directory_index *index, uint32_t logical, uint64_t *physical);
bool ext4_index_contains(const struct ext4_index_range *range, uint32_t hash);
/* These helpers consume an already validated tree under the exclusive owner.
 * next starts at blocks; account all appended blocks once at operation end. */
enum ext4_result ext4_index_append(
    struct ext4_directory_index *index, uint32_t *next, uint32_t *logical, uint8_t **buffer);
enum ext4_result ext4_index_add(struct ext4_directory_index *index, uint32_t leaf, uint32_t hash,
    uint32_t block, uint32_t *next);
void ext4_index_checksum_set(
    struct ext4_fs *fs, const struct ext4_inode *inode, uint32_t logical, uint8_t *buffer);

#endif

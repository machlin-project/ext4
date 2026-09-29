/* SPDX-License-Identifier: BSD-3-Clause */
#ifndef MACHLIN_EXT4_DIRECTORY_INDEX_H
#define MACHLIN_EXT4_DIRECTORY_INDEX_H

#include "allocate.h"

#define EXT4_DIRECTORY_MAX_BLOCKS (1U << 20)
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

struct ext4_index_metadata {
	uint32_t parent;
	uint16_t count;
	uint8_t levels;
	uint8_t version;
};

/* One node of a probed root-to-leaf path: its block, its range, the entry the path
 * follows and a CRC32C of its count and entries as the probe checked them. */
struct ext4_index_frame {
	struct ext4_index_range range;
	uint32_t logical;
	uint32_t checksum;
	uint16_t count;
	uint16_t position;
};

/* The tree belongs to the caller's private transaction view. A classified tree's
 * range array is bounded by directory size and covers every mapped logical
 * directory block; a probe fills the frames of one path and its leaf instead. */
struct ext4_directory_index {
	struct ext4_allocation *allocation;
	const struct ext4_inode *inode;
	struct ext4_inode_disk *disk;
	struct ext4_index_range *ranges;
	struct ext4_index_frame frames[EXT4_DX_MAX_INDIRECT_LEVELS + 1U];
	struct ext4_index_range leaf;
	uint32_t seed[4];
	uint32_t blocks;
	uint32_t parent_number;
	uint32_t leaf_logical;
	uint8_t levels;
	uint8_t version;
};

uint8_t ext4_index_max_levels(const struct ext4_fs *fs);
/* Decode one index header without interpreting its child pointers. The buffer
 * and result remain unchanged on failure; checksum verification is read-only. */
enum ext4_result ext4_index_decode(struct ext4_fs *fs, const struct ext4_inode *inode,
    uint32_t logical, uint8_t *buffer, struct ext4_index_metadata *result);
/* Open a directory's index for a change. Every block is read and classified when
 * classify is set, or when the mount does not remember the index as its map record
 * now stands; ranges then covers every block, and probes check each node they read
 * against it. */
enum ext4_result ext4_index_open(struct ext4_allocation *allocation, const struct ext4_inode *inode,
    struct ext4_inode_disk *disk, struct ext4_directory_index *index, bool classify);
/* Remember an index the mount's own operation produced or changed, as its map record
 * now stands. */
void ext4_index_remember(struct ext4_allocation *allocation, const struct ext4_inode *inode,
    const struct ext4_inode_disk *disk);
/* Follow one root-to-leaf path to the first leaf whose range contains hash. */
enum ext4_result ext4_index_probe(struct ext4_directory_index *index, uint32_t hash);
/* Advance the path to the next leaf when it continues hash across a boundary whose
 * odd hash marks a collision; more reports whether it did. */
enum ext4_result ext4_index_next(struct ext4_directory_index *index, uint32_t hash, bool *more);
void ext4_index_close(struct ext4_directory_index *index);
enum ext4_result ext4_index_read(
    struct ext4_directory_index *index, uint32_t logical, uint64_t *physical);
bool ext4_index_contains(const struct ext4_index_range *range, uint32_t hash);
/* These helpers consume an already validated tree under the exclusive owner.
 * next starts at blocks; account all appended blocks once at operation end. */
enum ext4_result ext4_index_append(
    struct ext4_directory_index *index, uint32_t *next, uint32_t *logical, uint8_t **buffer);
/* Insert a new leaf after the probed one, splitting full nodes up the path. */
enum ext4_result ext4_index_add(
    struct ext4_directory_index *index, uint32_t hash, uint32_t block, uint32_t *next);
void ext4_index_checksum_set(
    struct ext4_fs *fs, const struct ext4_inode *inode, uint32_t logical, uint8_t *buffer);

#endif

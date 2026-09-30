/* SPDX-License-Identifier: BSD-3-Clause */
#ifndef MACHLIN_EXT4_MAP_READ_H
#define MACHLIN_EXT4_MAP_READ_H

#include "internal.h"

#define EXT4_READ_CACHE_LEAVES 8U
#define EXT4_READ_CACHE_BYTES (64U * 1024U)

/* A leaf is published only after its checksum and every record are validated.
 * Ancestor boundaries limit reuse even when an extent crosses into a sibling. */
struct ext4_extent_cursor {
	const uint8_t *leaf;
	uint64_t first;
	uint64_t limit;
	uint64_t next;
	uint16_t entries;
	uint16_t position;
};

struct ext4_cached_leaf {
	struct ext4_extent_cursor cursor;
	uint8_t *buffer;
};

/* Only mapping metadata, never file data. Allocate leaves on demand, bounded by
 * both a slot count and a byte budget. Replacement reuses the victim's buffer. */
struct ext4_extent_cache {
	struct ext4_cached_leaf leaves[EXT4_READ_CACHE_LEAVES];
	uint32_t next;
};

/* A decoded run has no pointer into replaceable traversal storage. Both extent
 * and indirect readers can reuse it within their immutable inode/device view. */
struct ext4_read_run {
	uint64_t first;
	uint64_t limit;
	uint64_t physical;
};

struct ext4_map_reader {
	struct ext4_read_run run;
	struct ext4_extent_cursor cursor;
	uint8_t *scratch;
	struct ext4_extent_cache *cache;
};

/* One held inode owns the checked snapshot and mapping cache for both copied
 * reads and native block mappings. Neither consumer retains file data. */
struct ext4_read_state {
	struct ext4_inode inode;
	struct ext4_map_reader mapping;
	struct ext4_extent_cache cache;
	uint64_t revision;
};

enum ext4_result ext4_read_state_get(struct ext4_inode_hold *hold, struct ext4_read_state **result);

/* A zero-initialized reader uses one temporary block. An optional cache retains
 * leaves across calls for the same immutable inode snapshot and device view.
 * The owner must close the reader before changing either. */
enum ext4_result ext4_map_reader_next(struct ext4_fs *fs, const struct ext4_inode *inode,
    struct ext4_map_reader *reader, uint32_t logical, uint64_t *physical, uint64_t *blocks);
/* Look ahead only through a checked run or validated leaves. Never allocate or
 * read the device: planning cannot move a metadata failure ahead of data. */
bool ext4_map_reader_cached(
    struct ext4_map_reader *reader, uint32_t logical, uint64_t *physical, uint64_t *blocks);
/* Deliver a byte range, batching adjacent physical data across logical holes.
 * require_data rejects holes, including unwritten extents, at their boundary. */
enum ext4_result ext4_map_reader_read(struct ext4_fs *fs, const struct ext4_inode *inode,
    struct ext4_map_reader *reader, uint64_t offset, void *buffer, size_t length, bool require_data,
    size_t *completed);
void ext4_map_reader_close(struct ext4_fs *fs, struct ext4_map_reader *reader);

#endif

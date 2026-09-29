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
	uint16_t entries;
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

struct ext4_map_reader {
	struct ext4_extent_cursor cursor;
	uint8_t *scratch;
	struct ext4_extent_cache *cache;
};

/* A zero-initialized reader uses one temporary block. An optional cache retains
 * leaves across calls for the same immutable inode snapshot and device view.
 * The owner must close the reader before changing either. */
enum ext4_result ext4_map_reader_next(struct ext4_fs *fs, const struct ext4_inode *inode,
    struct ext4_map_reader *reader, uint32_t logical, uint64_t *physical, uint64_t *blocks);
void ext4_map_reader_close(struct ext4_fs *fs, struct ext4_map_reader *reader);

#endif

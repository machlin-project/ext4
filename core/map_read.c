/* SPDX-License-Identifier: BSD-3-Clause */
#include "map_read.h"
#include "journal.h"

static enum ext4_result ext4_map_blocks(struct ext4_fs *fs, const struct ext4_inode *inode,
    uint32_t logical, uint8_t **scratch, uint64_t *physical, uint64_t *blocks,
    struct ext4_block_path *path);

bool
ext4_inode_can_map_read(const struct ext4_inode *inode)
{
	return inode != NULL && (inode->mode & EXT4_MODE_TYPE) == EXT4_MODE_REGULAR &&
	    (inode->flags & (EXT4_INODE_ENCRYPT | EXT4_INODE_VERITY | EXT4_INODE_INLINE_DATA)) == 0;
}

static enum ext4_result
ext4_mapping_node(struct ext4_fs *fs, uint64_t block, uint8_t **scratch)
{
	if (*scratch == NULL) {
		*scratch = fs->environment.allocate(fs->environment.context, fs->info.block_size);
		if (*scratch == NULL) {
			return EXT4_NO_MEMORY;
		}
	}
	return ext4_block_read(fs, block, *scratch);
}

static enum ext4_result
ext4_map_reader_range(struct ext4_fs *fs, const struct ext4_inode *inode,
    struct ext4_map_reader *reader, uint64_t offset, size_t length, struct ext4_mapping *mapping)
{
	uint64_t block;
	uint64_t logical;
	uint64_t blocks;
	uint64_t file_blocks;
	uint64_t bytes;
	size_t within;
	enum ext4_result error;

	if (fs == NULL || inode == NULL || mapping == NULL || length == 0 ||
	    (inode->mode & EXT4_MODE_TYPE) != EXT4_MODE_REGULAR) {
		return EXT4_INVALID_ARGUMENT;
	}
	if (fs->aborted) {
		return EXT4_RECOVERY_REQUIRED;
	}
	if (inode->flags & EXT4_INODE_ENCRYPT) {
		return EXT4_ENCRYPTED;
	}
	/* Native mappings would bypass Merkle verification; use ext4_read. */
	if (inode->flags & EXT4_INODE_VERITY) {
		return EXT4_UNSUPPORTED;
	}
	if (offset >= inode->size) {
		return EXT4_NOT_FOUND;
	}
	logical = offset / fs->info.block_size;
	if (logical > UINT32_MAX) {
		return EXT4_RANGE;
	}
	error = ext4_map_reader_next(fs, inode, reader, (uint32_t)logical, &block, &blocks);
	if (error != EXT4_OK) {
		return error;
	}
	file_blocks = inode->size / fs->info.block_size;
	if (inode->size % fs->info.block_size != 0) {
		file_blocks++;
	}
	if (blocks > file_blocks - logical) {
		blocks = file_blocks - logical;
	}
	/* Native reads see only home blocks; the journal holds newer contents of some. */
	if (block != 0 && fs->journal != NULL) {
		blocks = ext4_journal_home_prefix(fs->journal, block, blocks);
		if (blocks == 0) {
			return EXT4_BUSY;
		}
	}
	within = (size_t)(offset % fs->info.block_size);
	bytes = blocks * fs->info.block_size - within;
	mapping->hole = block == 0;
	mapping->device_offset = block == 0 ? 0 : block * fs->info.block_size + within;
	mapping->length = bytes < length ? (size_t)bytes : length;
	/* Native I/O may include padding in the final filesystem block only. */
	return EXT4_OK;
}

enum ext4_result
ext4_map_read(struct ext4_fs *fs, const struct ext4_inode *inode, uint64_t offset, size_t length,
    struct ext4_mapping *mapping)
{
	struct ext4_map_reader reader = { 0 };
	enum ext4_result error;

	error = ext4_map_reader_range(fs, inode, &reader, offset, length, mapping);
	if (fs != NULL) {
		ext4_map_reader_close(fs, &reader);
	}
	return error;
}

enum ext4_result
ext4_map_read_held(
    struct ext4_inode_hold *hold, uint64_t offset, size_t length, struct ext4_mapping *mapping)
{
	struct ext4_read_state *reader;
	enum ext4_result error;

	if (mapping == NULL || length == 0) {
		return EXT4_INVALID_ARGUMENT;
	}
	error = ext4_read_state_get(hold, &reader);
	if (error != EXT4_OK) {
		return error;
	}
	return ext4_map_reader_range(
	    hold->fs, &reader->inode, &reader->mapping, offset, length, mapping);
}

/* leaf_end, when requested, receives the end of the selected leaf's last extent. */
static enum ext4_result
ext4_extent_map(struct ext4_fs *fs, const struct ext4_inode *inode, uint32_t logical,
    uint8_t **scratch, uint64_t *physical, uint64_t *blocks, struct ext4_block_path *path,
    uint64_t *leaf_end, struct ext4_extent_cursor *cursor)
{
	const uint8_t *node = inode->block_data;
	const struct ext4_extent_header_disk *header;
	const struct ext4_extent_disk *extent;
	const struct ext4_extent_index_disk *index;
	const struct ext4_le32 *tail;
	size_t node_size = sizeof(inode->block_data);
	size_t capacity;
	size_t tail_offset;
	uint64_t start;
	uint64_t end = 0;
	uint64_t disk_block;
	uint64_t child;
	uint64_t boundary = (uint64_t)UINT32_MAX + 1U;
	uint64_t first = 0;
	uint64_t leaf_limit;
	uint32_t length;
	uint16_t entries;
	uint16_t maximum;
	uint16_t depth;
	uint16_t expected_depth = UINT16_MAX;
	uint16_t position;
	bool external = false;
	bool unwritten;
	enum ext4_result error;

	*physical = 0;
	if (cursor != NULL) {
		cursor->leaf = NULL;
	}
	for (;;) {
		header = (const struct ext4_extent_header_disk *)node;
		entries = ext4_le16(&header->entries);
		maximum = ext4_le16(&header->maximum);
		depth = ext4_le16(&header->depth);
		capacity = (node_size - sizeof(*header)) / sizeof(*extent);
		if (ext4_le16(&header->magic) != EXT4_EXTENT_MAGIC || maximum == 0 ||
		    maximum > capacity || entries > maximum || depth > EXT4_EXTENT_MAX_DEPTH ||
		    (expected_depth != UINT16_MAX && depth != expected_depth) ||
		    (depth != 0 && entries == 0)) {
			return EXT4_CORRUPT;
		}
		if (external && fs->metadata_checksum) {
			tail_offset = sizeof(*header) + maximum * sizeof(*extent);
			if (tail_offset > node_size - sizeof(*tail)) {
				return EXT4_CORRUPT;
			}
			tail = (const struct ext4_le32 *)(node + tail_offset);
			if (ext4_crc32c(ext4_inode_seed(fs, inode), node, tail_offset) !=
			    ext4_le32(tail)) {
				return EXT4_CORRUPT;
			}
		}
		child = 0;
		end = 0;
		leaf_limit = boundary;
		for (position = 0; position < entries; position++) {
			if (depth == 0) {
				extent = (const struct ext4_extent_disk *)(node + sizeof(*header) +
				    position * sizeof(*extent));
				start = ext4_le32(&extent->logical);
				length = ext4_le16(&extent->length);
				unwritten = length > EXT4_EXTENT_UNWRITTEN_LIMIT;
				if (unwritten) {
					length -= EXT4_EXTENT_UNWRITTEN_LIMIT;
				}
				disk_block = ext4_le32(&extent->physical_lo) |
				    ((uint64_t)ext4_le16(&extent->physical_hi) << 32);
				if (length == 0 || (position != 0 && start < end) ||
				    start + length > (uint64_t)UINT32_MAX + 1 || disk_block == 0 ||
				    disk_block >= fs->info.blocks ||
				    length > fs->info.blocks - disk_block ||
				    disk_block % fs->cluster_blocks != start % fs->cluster_blocks) {
					return EXT4_CORRUPT;
				}
				end = start + length;
				if (logical >= start && logical < end) {
					if (!unwritten) {
						*physical = disk_block + (logical - start);
					}
					if (end < boundary) {
						boundary = end;
					}
				} else if (start > logical && start < boundary) {
					boundary = start;
				}
			} else {
				index = (const struct ext4_extent_index_disk *)(node +
				    sizeof(*header) + position * sizeof(*index));
				start = ext4_le32(&index->logical);
				disk_block = ext4_le32(&index->child_lo) |
				    ((uint64_t)ext4_le16(&index->child_hi) << 32);
				if ((position != 0 && start <= end) || disk_block == 0 ||
				    disk_block >= fs->info.blocks) {
					return EXT4_CORRUPT;
				}
				end = start;
				if (logical >= start) {
					child = disk_block;
					if (start > first) {
						first = start;
					}
				} else if (start < boundary) {
					boundary = start;
				}
			}
		}
		if (depth == 0 || child == 0) {
			if (depth == 0 && cursor != NULL) {
				cursor->leaf = node;
				cursor->first = first;
				cursor->limit = leaf_limit;
				cursor->entries = entries;
				cursor->next = UINT64_MAX;
				cursor->position = 0;
			}
			if (blocks != NULL) {
				*blocks = boundary - logical;
			}
			if (leaf_end != NULL) {
				*leaf_end = depth == 0 ? end : 0;
			}
			return EXT4_OK;
		}
		if (path != NULL) {
			path->blocks[path->count++] = child;
		}
		error = ext4_mapping_node(fs, child, scratch);
		if (error != EXT4_OK) {
			return error;
		}
		expected_depth = depth - 1;
		node = *scratch;
		node_size = fs->info.block_size;
		external = true;
	}
}

/* All records, physical ranges and the checksum were checked before publishing
 * the leaf. A sequential boundary crosses at most one validated record; other
 * requests use binary search. Neither path repeats validation. */
static void
ext4_extent_cursor_map(
    struct ext4_extent_cursor *cursor, uint32_t logical, uint64_t *physical, uint64_t *blocks)
{
	const struct ext4_extent_disk *extents = (const struct ext4_extent_disk *)(cursor->leaf +
	    sizeof(struct ext4_extent_header_disk));
	const struct ext4_extent_disk *extent;
	uint64_t boundary = cursor->limit;
	uint64_t end;
	uint32_t start;
	uint32_t length;
	uint16_t low = 0;
	uint16_t high = cursor->entries;
	uint16_t middle;
	bool unwritten;

	if (logical == cursor->next) {
		low = cursor->position;
		if (low < cursor->entries && ext4_le32(&extents[low].logical) <= logical) {
			low++;
		}
	} else {
		while (low < high) {
			middle = low + (high - low) / 2U;
			if (ext4_le32(&extents[middle].logical) <= logical) {
				low = middle + 1U;
			} else {
				high = middle;
			}
		}
	}
	*physical = 0;
	if (low < cursor->entries) {
		start = ext4_le32(&extents[low].logical);
		if (start < boundary) {
			boundary = start;
		}
	}
	if (low != 0) {
		extent = &extents[low - 1U];
		start = ext4_le32(&extent->logical);
		length = ext4_le16(&extent->length);
		unwritten = length > EXT4_EXTENT_UNWRITTEN_LIMIT;
		if (unwritten) {
			length -= EXT4_EXTENT_UNWRITTEN_LIMIT;
		}
		end = (uint64_t)start + length;
		if (logical < end) {
			if (!unwritten) {
				*physical = ext4_le32(&extent->physical_lo) |
				    ((uint64_t)ext4_le16(&extent->physical_hi) << 32);
				*physical += logical - start;
			}
			if (end < boundary) {
				boundary = end;
			}
		}
	}
	*blocks = boundary - logical;
	cursor->position = low;
	cursor->next = boundary;
}

static enum ext4_result
ext4_indirect_map(struct ext4_fs *fs, const struct ext4_inode *inode, uint32_t logical,
    uint8_t **scratch, uint64_t *physical, uint64_t *blocks, struct ext4_block_path *path)
{
	const struct ext4_le32 *pointers = (const struct ext4_le32 *)inode->block_data;
	uint64_t remaining;
	uint64_t span = 1;
	uint64_t block;
	uint64_t next;
	uint64_t run;
	uint64_t limit = (uint64_t)UINT32_MAX + 1U - logical;
	uint32_t per_block = fs->info.block_size / sizeof(*pointers);
	uint32_t position = 0;
	uint32_t available;
	unsigned int depth;
	unsigned int level;
	enum ext4_result error;

	if (logical < EXT4_DIRECT_BLOCKS) {
		position = logical;
		available = EXT4_DIRECT_BLOCKS - position;
		block = ext4_le32(&pointers[position]);
	} else {
		remaining = logical - EXT4_DIRECT_BLOCKS;
		for (depth = 1; depth <= EXT4_INDIRECT_LEVELS; depth++) {
			span *= per_block;
			if (remaining < span) {
				break;
			}
			remaining -= span;
		}
		if (depth > EXT4_INDIRECT_LEVELS) {
			return EXT4_CORRUPT;
		}
		block = ext4_le32(&pointers[EXT4_DIRECT_BLOCKS + depth - 1]);
		for (level = depth; level != 0 && block != 0; level--) {
			if (path != NULL) {
				path->blocks[path->count++] = block;
			}
			error = ext4_mapping_node(fs, block, scratch);
			if (error != EXT4_OK) {
				return error;
			}
			span /= per_block;
			pointers = (const struct ext4_le32 *)*scratch;
			position = (uint32_t)(remaining / span);
			block = ext4_le32(&pointers[position]);
			remaining %= span;
		}
		if (level != 0) {
			/* An absent ancestor covers the rest of its logical subtree. */
			*physical = 0;
			if (blocks != NULL) {
				run = span - remaining;
				*blocks = run < limit ? run : limit;
			}
			return EXT4_OK;
		}
		available = per_block - position;
	}
	if (block >= fs->info.blocks) {
		return EXT4_CORRUPT;
	}
	*physical = block;
	if (blocks != NULL) {
		for (run = 1; run < available && run < limit; run++) {
			next = ext4_le32(&pointers[position + (uint32_t)run]);
			if (next >= fs->info.blocks ||
			    (block == 0 ? next != 0 : next != block + run)) {
				break;
			}
		}
		*blocks = run;
	}
	return EXT4_OK;
}

static enum ext4_result
ext4_map_blocks(struct ext4_fs *fs, const struct ext4_inode *inode, uint32_t logical,
    uint8_t **scratch, uint64_t *physical, uint64_t *blocks, struct ext4_block_path *path)
{
	if (fs->aborted) {
		return EXT4_RECOVERY_REQUIRED;
	}
	if (path != NULL) {
		path->count = 0;
	}
	if (inode->flags & EXT4_INODE_INLINE_DATA) {
		return EXT4_UNSUPPORTED;
	}
	if (inode->flags & EXT4_INODE_EXTENTS) {
		return ext4_extent_map(
		    fs, inode, logical, scratch, physical, blocks, path, NULL, NULL);
	}
	return ext4_indirect_map(fs, inode, logical, scratch, physical, blocks, path);
}

enum ext4_result
ext4_extent_last_end(struct ext4_fs *fs, const struct ext4_inode *inode, uint64_t *end)
{
	uint8_t *scratch = NULL;
	uint64_t physical;
	enum ext4_result error;

	*end = 0;
	if (fs->aborted) {
		return EXT4_RECOVERY_REQUIRED;
	}
	if ((inode->flags & (EXT4_INODE_EXTENTS | EXT4_INODE_INLINE_DATA)) != EXT4_INODE_EXTENTS) {
		return EXT4_UNSUPPORTED;
	}
	/* The largest logical address descends the rightmost path to the last leaf. */
	error = ext4_extent_map(fs, inode, UINT32_MAX, &scratch, &physical, NULL, NULL, end, NULL);
	if (scratch != NULL) {
		fs->environment.release(fs->environment.context, scratch, fs->info.block_size);
	}
	return error;
}

enum ext4_result
ext4_map_block(
    struct ext4_fs *fs, const struct ext4_inode *inode, uint32_t logical, uint64_t *physical)
{
	return ext4_map_block_path(fs, inode, logical, physical, NULL);
}

enum ext4_result
ext4_map_block_path(struct ext4_fs *fs, const struct ext4_inode *inode, uint32_t logical,
    uint64_t *physical, struct ext4_block_path *path)
{
	return ext4_map_blocks_path(fs, inode, logical, physical, NULL, path);
}

enum ext4_result
ext4_map_blocks_path(struct ext4_fs *fs, const struct ext4_inode *inode, uint32_t logical,
    uint64_t *physical, uint64_t *blocks, struct ext4_block_path *path)
{
	uint8_t *scratch = NULL;
	enum ext4_result error;

	error = ext4_map_blocks(fs, inode, logical, &scratch, physical, blocks, path);
	if (scratch != NULL) {
		fs->environment.release(fs->environment.context, scratch, fs->info.block_size);
	}
	return error;
}

static bool
ext4_extent_cursor_contains(const struct ext4_extent_cursor *cursor, uint32_t logical)
{
	return cursor->leaf != NULL && logical >= cursor->first && logical < cursor->limit;
}

static uint32_t
ext4_extent_cache_capacity(const struct ext4_fs *fs)
{
	uint32_t capacity = EXT4_READ_CACHE_BYTES / fs->info.block_size;

	return capacity < EXT4_READ_CACHE_LEAVES ? capacity : EXT4_READ_CACHE_LEAVES;
}

static void
ext4_read_run_store(
    struct ext4_map_reader *reader, uint32_t logical, uint64_t physical, uint64_t blocks)
{
	reader->run = (struct ext4_read_run){ logical, (uint64_t)logical + blocks, physical };
}

bool
ext4_map_reader_cached(
    struct ext4_map_reader *reader, uint32_t logical, uint64_t *physical, uint64_t *blocks)
{
	struct ext4_extent_cache *cache = reader->cache;
	uint32_t index;

	if (logical >= reader->run.first && logical < reader->run.limit) {
		*physical = reader->run.physical == 0
		    ? 0
		    : reader->run.physical + (logical - reader->run.first);
		*blocks = reader->run.limit - logical;
		return true;
	}
	if (ext4_extent_cursor_contains(&reader->cursor, logical)) {
		ext4_extent_cursor_map(&reader->cursor, logical, physical, blocks);
		ext4_read_run_store(reader, logical, *physical, *blocks);
		return true;
	}
	for (index = 0; cache != NULL && index < EXT4_READ_CACHE_LEAVES; index++) {
		if (ext4_extent_cursor_contains(&cache->leaves[index].cursor, logical)) {
			reader->cursor = cache->leaves[index].cursor;
			ext4_extent_cursor_map(&reader->cursor, logical, physical, blocks);
			ext4_read_run_store(reader, logical, *physical, *blocks);
			return true;
		}
	}
	return false;
}

enum ext4_result
ext4_map_reader_next(struct ext4_fs *fs, const struct ext4_inode *inode,
    struct ext4_map_reader *reader, uint32_t logical, uint64_t *physical, uint64_t *blocks)
{
	struct ext4_extent_cache *cache = reader->cache;
	struct ext4_cached_leaf *slot;
	uint8_t *spare;
	uint32_t capacity;
	enum ext4_result error;

	if (fs->aborted) {
		return EXT4_RECOVERY_REQUIRED;
	}
	if (ext4_map_reader_cached(reader, logical, physical, blocks)) {
		return EXT4_OK;
	}
	if ((inode->flags & (EXT4_INODE_EXTENTS | EXT4_INODE_INLINE_DATA)) != EXT4_INODE_EXTENTS) {
		error =
		    ext4_map_blocks(fs, inode, logical, &reader->scratch, physical, blocks, NULL);
		if (error == EXT4_OK) {
			ext4_read_run_store(reader, logical, *physical, *blocks);
		}
		return error;
	}
	capacity = ext4_extent_cache_capacity(fs);
	error = ext4_extent_map(
	    fs, inode, logical, &reader->scratch, physical, blocks, NULL, NULL, &reader->cursor);
	if (error != EXT4_OK) {
		return error;
	}
	ext4_read_run_store(reader, logical, *physical, *blocks);
	if (cache == NULL || capacity == 0 || reader->cursor.leaf != reader->scratch ||
	    reader->scratch == NULL) {
		return error;
	}
	/* Transfer the verified leaf without copying it. The evicted buffer becomes
	 * the next traversal's scratch; no cached cursor can refer to that buffer. */
	slot = &cache->leaves[cache->next];
	spare = slot->buffer;
	slot->buffer = reader->scratch;
	slot->cursor = reader->cursor;
	reader->scratch = spare;
	cache->next = (cache->next + 1U) % capacity;
	return EXT4_OK;
}

void
ext4_map_reader_close(struct ext4_fs *fs, struct ext4_map_reader *reader)
{
	uint32_t index;

	if (reader->scratch != NULL) {
		fs->environment.release(
		    fs->environment.context, reader->scratch, fs->info.block_size);
	}
	if (reader->cache != NULL) {
		for (index = 0; index < EXT4_READ_CACHE_LEAVES; index++) {
			if (reader->cache->leaves[index].buffer != NULL) {
				fs->environment.release(fs->environment.context,
				    reader->cache->leaves[index].buffer, fs->info.block_size);
			}
		}
		ext4_zero(reader->cache, sizeof(*reader->cache));
	}
	reader->scratch = NULL;
	reader->cursor.leaf = NULL;
	reader->run.limit = 0;
}

/* SPDX-License-Identifier: BSD-3-Clause */
#include "internal.h"

enum ext4_result
ext4_map_read(struct ext4_fs *fs, const struct ext4_inode *inode, uint64_t offset, size_t length,
    struct ext4_mapping *mapping)
{
	uint64_t block;
	uint64_t logical;
	size_t within;
	enum ext4_result error;

	if (fs == NULL || inode == NULL || mapping == NULL || length == 0 ||
	    (inode->mode & EXT4_MODE_TYPE) != EXT4_MODE_REGULAR) {
		return EXT4_INVALID_ARGUMENT;
	}
	if (fs->aborted) {
		return EXT4_RECOVERY_REQUIRED;
	}
	if (offset >= inode->size) {
		return EXT4_NOT_FOUND;
	}
	logical = offset / fs->info.block_size;
	if (logical > UINT32_MAX) {
		return EXT4_RANGE;
	}
	error = ext4_map_block(fs, inode, (uint32_t)logical, &block);
	if (error != EXT4_OK) {
		return error;
	}
	within = (size_t)(offset % fs->info.block_size);
	mapping->hole = block == 0;
	mapping->device_offset = block == 0 ? 0 : block * fs->info.block_size + within;
	mapping->length = fs->info.block_size - within;
	if (mapping->length > length) {
		mapping->length = length;
	}
	/* The mapped block includes EOF padding for native page-cache I/O. */
	return EXT4_OK;
}

static enum ext4_result
ext4_extent_map(struct ext4_fs *fs, const struct ext4_inode *inode, uint32_t logical,
    uint8_t *scratch, uint64_t *physical)
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
				    length > fs->info.blocks - disk_block) {
					return EXT4_CORRUPT;
				}
				end = start + length;
				if (logical >= start && logical < end && !unwritten) {
					*physical = disk_block + (logical - start);
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
				}
			}
		}
		if (depth == 0 || child == 0) {
			return EXT4_OK;
		}
		error = ext4_block_read(fs, child, scratch);
		if (error != EXT4_OK) {
			return error;
		}
		expected_depth = depth - 1;
		node = scratch;
		node_size = fs->info.block_size;
		external = true;
	}
}

static enum ext4_result
ext4_indirect_map(struct ext4_fs *fs, const struct ext4_inode *inode, uint32_t logical,
    uint8_t *scratch, uint64_t *physical)
{
	const struct ext4_le32 *pointers = (const struct ext4_le32 *)inode->block_data;
	uint64_t remaining;
	uint64_t span = 1;
	uint64_t block;
	uint32_t per_block = fs->info.block_size / sizeof(*pointers);
	unsigned int depth;
	unsigned int level;
	enum ext4_result error;

	if (logical < EXT4_DIRECT_BLOCKS) {
		block = ext4_le32(&pointers[logical]);
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
			error = ext4_block_read(fs, block, scratch);
			if (error != EXT4_OK) {
				return error;
			}
			span /= per_block;
			pointers = (const struct ext4_le32 *)scratch;
			block = ext4_le32(&pointers[remaining / span]);
			remaining %= span;
		}
	}
	if (block >= fs->info.blocks) {
		return EXT4_CORRUPT;
	}
	*physical = block;
	return EXT4_OK;
}

enum ext4_result
ext4_map_block(
    struct ext4_fs *fs, const struct ext4_inode *inode, uint32_t logical, uint64_t *physical)
{
	uint8_t *scratch;
	enum ext4_result error;

	if (fs->aborted) {
		return EXT4_RECOVERY_REQUIRED;
	}
	scratch = fs->environment.allocate(fs->environment.context, fs->info.block_size);
	if (scratch == NULL) {
		return EXT4_NO_MEMORY;
	}
	if (inode->flags & EXT4_INODE_EXTENTS) {
		error = ext4_extent_map(fs, inode, logical, scratch, physical);
	} else {
		error = ext4_indirect_map(fs, inode, logical, scratch, physical);
	}
	fs->environment.release(fs->environment.context, scratch, fs->info.block_size);
	return error;
}

enum ext4_result
ext4_read(struct ext4_fs *fs, const struct ext4_inode *inode, uint64_t offset, void *buffer,
    size_t length, size_t *completed)
{
	uint8_t *output = buffer;
	uint64_t physical;
	uint64_t logical;
	size_t chunk;
	size_t in_block;
	enum ext4_result error;

	if (completed == NULL) {
		return EXT4_INVALID_ARGUMENT;
	}
	*completed = 0;
	if (fs == NULL || inode == NULL || (length != 0 && buffer == NULL)) {
		return EXT4_INVALID_ARGUMENT;
	}
	if (fs->aborted) {
		return EXT4_RECOVERY_REQUIRED;
	}
	if (offset >= inode->size) {
		return EXT4_OK;
	}
	if (length > inode->size - offset) {
		length = (size_t)(inode->size - offset);
	}
	if (inode->fast_symlink) {
		if (inode->size > sizeof(inode->block_data)) {
			return EXT4_CORRUPT;
		}
		ext4_copy(buffer, inode->block_data + offset, length);
		*completed = length;
		return EXT4_OK;
	}
	while (*completed < length) {
		logical = offset / fs->info.block_size;
		if (logical > UINT32_MAX) {
			return EXT4_RANGE;
		}
		in_block = (size_t)(offset % fs->info.block_size);
		chunk = fs->info.block_size - in_block;
		if (chunk > length - *completed) {
			chunk = length - *completed;
		}
		error = ext4_map_block(fs, inode, (uint32_t)logical, &physical);
		if (error != EXT4_OK) {
			return error;
		}
		if (physical == 0) {
			ext4_zero(output + *completed, chunk);
		} else {
			error = ext4_device_read(fs, physical * fs->info.block_size + in_block,
			    output + *completed, chunk);
			if (error != EXT4_OK) {
				return error;
			}
		}
		*completed += chunk;
		offset += chunk;
	}
	return EXT4_OK;
}

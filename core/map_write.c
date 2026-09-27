/* SPDX-License-Identifier: BSD-3-Clause */
#include "allocate.h"

struct ext4_extent_path {
	uint8_t *nodes[EXT4_EXTENT_MAX_DEPTH + 1];
	uint64_t blocks[EXT4_EXTENT_MAX_DEPTH + 1];
	uint16_t positions[EXT4_EXTENT_MAX_DEPTH + 1];
	uint16_t levels;
};

static uint16_t
ext4_extent_length(const struct ext4_extent_disk *extent)
{
	uint16_t length = ext4_le16(&extent->length);

	return length > EXT4_EXTENT_UNWRITTEN_LIMIT
	    ? (uint16_t)(length - EXT4_EXTENT_UNWRITTEN_LIMIT)
	    : length;
}

static uint64_t
ext4_extent_physical(const struct ext4_extent_disk *extent)
{
	return ext4_le32(&extent->physical_lo) | (uint64_t)ext4_le16(&extent->physical_hi) << 32;
}

static uint64_t
ext4_extent_child(const struct ext4_extent_index_disk *index)
{
	return ext4_le32(&index->child_lo) | (uint64_t)ext4_le16(&index->child_hi) << 32;
}

static enum ext4_result
ext4_extent_check(struct ext4_allocation *allocation, const struct ext4_inode *inode,
    const uint8_t *node, bool external, uint16_t expected_depth, uint64_t low, uint64_t high)
{
	struct ext4_fs *fs = allocation->fs;
	const struct ext4_extent_header_disk *header = (const struct ext4_extent_header_disk *)node;
	const struct ext4_extent_disk *extents = (const struct ext4_extent_disk *)(header + 1);
	const struct ext4_extent_index_disk *indices =
	    (const struct ext4_extent_index_disk *)(header + 1);
	const struct ext4_le32 *tail;
	size_t size = external ? fs->info.block_size : EXT4_INODE_BLOCK_BYTES;
	size_t tail_offset;
	uint64_t start;
	uint64_t end = low;
	uint64_t block;
	uint32_t length;
	uint16_t count = ext4_le16(&header->entries);
	uint16_t maximum = ext4_le16(&header->maximum);
	uint16_t depth = ext4_le16(&header->depth);
	uint16_t index;
	uint16_t previous;

	if (ext4_le16(&header->magic) != EXT4_EXTENT_MAGIC || maximum == 0 ||
	    maximum > (size - sizeof(*header)) / sizeof(*extents) || count > maximum ||
	    depth > EXT4_EXTENT_MAX_DEPTH ||
	    (expected_depth != UINT16_MAX && depth != expected_depth) ||
	    ((external || depth != 0) && count == 0)) {
		return EXT4_CORRUPT;
	}
	if (external && fs->metadata_checksum) {
		tail_offset = sizeof(*header) + maximum * sizeof(*extents);
		if (tail_offset > size - sizeof(*tail)) {
			return EXT4_CORRUPT;
		}
		tail = (const struct ext4_le32 *)(node + tail_offset);
		if (ext4_le32(tail) != ext4_crc32c(ext4_inode_seed(fs, inode), node, tail_offset)) {
			return EXT4_CORRUPT;
		}
	}
	for (index = 0; index < count; index++) {
		start = ext4_le32(&extents[index].logical);
		if (start < end || start >= high) {
			return EXT4_CORRUPT;
		}
		if (depth == 0) {
			length = ext4_extent_length(&extents[index]);
			block = ext4_extent_physical(&extents[index]);
			end = start + length;
			if (length == 0 || end > high || block == 0 || block >= fs->info.blocks ||
			    length > fs->info.blocks - block ||
			    ext4_system_overlaps(fs, block, length)) {
				return EXT4_CORRUPT;
			}
		} else {
			block = ext4_extent_child(&indices[index]);
			end = start + 1;
			if (block == 0 || block >= fs->info.blocks ||
			    ext4_system_block(fs, block)) {
				return EXT4_CORRUPT;
			}
			for (previous = 0; previous < index; previous++) {
				if (block == ext4_extent_child(&indices[previous])) {
					return EXT4_CORRUPT;
				}
			}
		}
	}
	return EXT4_OK;
}

static enum ext4_result
ext4_extent_lookup(struct ext4_allocation *allocation, const struct ext4_inode *inode,
    const struct ext4_inode_disk *disk, uint32_t logical, uint8_t *scratch,
    struct ext4_map_run *run)
{
	const uint8_t *node = disk->block_data;
	const struct ext4_extent_header_disk *header;
	const struct ext4_extent_disk *extents;
	const struct ext4_extent_index_disk *indices;
	uint64_t ancestors[EXT4_EXTENT_MAX_DEPTH];
	uint64_t low = 0;
	uint64_t high = (uint64_t)UINT32_MAX + 1;
	uint64_t start;
	uint64_t end;
	uint64_t child;
	uint16_t depth = UINT16_MAX;
	uint16_t count;
	uint16_t index;
	uint16_t previous;
	uint16_t level = 0;
	enum ext4_result error;

	for (;;) {
		error = ext4_extent_check(allocation, inode, node, level != 0, depth, low, high);
		if (error != EXT4_OK) {
			return error;
		}
		header = (const struct ext4_extent_header_disk *)node;
		depth = ext4_le16(&header->depth);
		count = ext4_le16(&header->entries);
		extents = (const struct ext4_extent_disk *)(header + 1);
		indices = (const struct ext4_extent_index_disk *)(header + 1);
		if (depth == 0) {
			for (index = 0; index < count; index++) {
				start = ext4_le32(&extents[index].logical);
				end = start + ext4_extent_length(&extents[index]);
				if (logical < start) {
					high = start;
					break;
				}
				if (logical < end) {
					run->physical =
					    ext4_extent_physical(&extents[index]) + logical - start;
					run->length = end - logical;
					run->unwritten = ext4_le16(&extents[index].length) >
					    EXT4_EXTENT_UNWRITTEN_LIMIT;
					for (previous = 0; previous < level; previous++) {
						if (ancestors[previous] >= run->physical &&
						    ancestors[previous] - run->physical <
							run->length) {
							return EXT4_CORRUPT;
						}
					}
					return EXT4_OK;
				}
			}
			run->length = high - logical;
			return EXT4_OK;
		}
		if (logical < ext4_le32(&indices[0].logical)) {
			run->length = ext4_le32(&indices[0].logical) - logical;
			return EXT4_OK;
		}
		for (index = 0; index + 1 < count; index++) {
			if (logical < ext4_le32(&indices[index + 1].logical)) {
				break;
			}
		}
		low = ext4_le32(&indices[index].logical);
		if (index + 1 < count) {
			high = ext4_le32(&indices[index + 1].logical);
		}
		child = ext4_extent_child(&indices[index]);
		for (previous = 0; previous < level; previous++) {
			if (child == ancestors[previous]) {
				return EXT4_CORRUPT;
			}
		}
		ancestors[level++] = child;
		error = ext4_allocation_valid(allocation, child);
		if (error == EXT4_OK) {
			error = ext4_transaction_read(allocation->transaction, child, scratch);
		}
		if (error != EXT4_OK) {
			return error;
		}
		node = scratch;
		depth--;
	}
}

static enum ext4_result
ext4_indirect_position(
    struct ext4_fs *fs, uint32_t logical, uint16_t *depth, uint64_t *remaining, uint64_t *span)
{
	uint32_t per_block = fs->info.block_size / sizeof(struct ext4_le32);

	*remaining = logical;
	*span = 1;
	*depth = 0;
	if (logical < EXT4_DIRECT_BLOCKS) {
		return EXT4_OK;
	}
	*remaining -= EXT4_DIRECT_BLOCKS;
	for (*depth = 1; *depth <= EXT4_INDIRECT_LEVELS; (*depth)++) {
		*span *= per_block;
		if (*remaining < *span) {
			return EXT4_OK;
		}
		*remaining -= *span;
	}
	return EXT4_RANGE;
}

static enum ext4_result
ext4_indirect_lookup(struct ext4_allocation *allocation, const struct ext4_inode_disk *disk,
    uint32_t logical, uint8_t *scratch, struct ext4_map_run *run)
{
	struct ext4_fs *fs = allocation->fs;
	const struct ext4_le32 *pointers = (const struct ext4_le32 *)disk->block_data;
	uint64_t ancestors[EXT4_INDIRECT_LEVELS];
	uint64_t remaining;
	uint64_t span;
	uint64_t block;
	uint16_t depth;
	uint16_t level;
	uint16_t previous;
	enum ext4_result error;

	error = ext4_indirect_position(fs, logical, &depth, &remaining, &span);
	if (error != EXT4_OK) {
		return error;
	}
	block = ext4_le32(&pointers[depth == 0 ? logical : EXT4_DIRECT_BLOCKS + depth - 1]);
	for (level = 0; level < depth && block != 0; level++) {
		for (previous = 0; previous < level; previous++) {
			if (block == ancestors[previous]) {
				return EXT4_CORRUPT;
			}
		}
		ancestors[level] = block;
		error = ext4_allocation_valid(allocation, block);
		if (error == EXT4_OK) {
			error = ext4_transaction_read(allocation->transaction, block, scratch);
		}
		if (error != EXT4_OK) {
			return error;
		}
		span /= fs->info.block_size / sizeof(*pointers);
		pointers = (const struct ext4_le32 *)scratch;
		block = ext4_le32(&pointers[remaining / span]);
		remaining %= span;
	}
	if (block != 0) {
		if (block >= fs->info.blocks || ext4_system_block(fs, block)) {
			return EXT4_CORRUPT;
		}
		for (previous = 0; previous < level; previous++) {
			if (block == ancestors[previous]) {
				return EXT4_CORRUPT;
			}
		}
	}
	run->physical = block;
	run->length = depth == 0 ? 1 : span - remaining;
	return EXT4_OK;
}

enum ext4_result
ext4_write_map_lookup(struct ext4_allocation *allocation, const struct ext4_inode *inode,
    const struct ext4_inode_disk *disk, uint32_t logical, struct ext4_map_run *run)
{
	struct ext4_fs *fs = allocation->fs;
	uint8_t *scratch;
	enum ext4_result error;

	ext4_zero(run, sizeof(*run));
	scratch = fs->environment.allocate(fs->environment.context, fs->info.block_size);
	if (scratch == NULL) {
		return EXT4_NO_MEMORY;
	}
	if (inode->flags & EXT4_INODE_EXTENTS) {
		error = ext4_extent_lookup(allocation, inode, disk, logical, scratch, run);
	} else {
		error = ext4_indirect_lookup(allocation, disk, logical, scratch, run);
	}
	fs->environment.release(fs->environment.context, scratch, fs->info.block_size);
	return error;
}

static void
ext4_extent_make(struct ext4_extent_disk *extent, uint32_t logical, uint64_t physical,
    uint16_t length, bool unwritten)
{
	ext4_encode32(&extent->logical, logical);
	ext4_encode16(
	    &extent->length, (uint16_t)(length + (unwritten ? EXT4_EXTENT_UNWRITTEN_LIMIT : 0)));
	ext4_encode16(&extent->physical_hi, (uint16_t)(physical >> 32));
	ext4_encode32(&extent->physical_lo, (uint32_t)physical);
}

static void
ext4_extent_index_make(struct ext4_extent_index_disk *index, uint32_t logical, uint64_t child)
{
	ext4_encode32(&index->logical, logical);
	ext4_encode32(&index->child_lo, (uint32_t)child);
	ext4_encode16(&index->child_hi, (uint16_t)(child >> 32));
	ext4_encode16(&index->unused, 0);
}

static enum ext4_result
ext4_extent_path_get(struct ext4_allocation *allocation, const struct ext4_inode *inode,
    struct ext4_inode_disk *disk, uint32_t logical, struct ext4_extent_path *path)
{
	struct ext4_extent_header_disk *header;
	struct ext4_extent_index_disk *indices;
	void *buffer = NULL;
	uint64_t low = 0;
	uint64_t high = (uint64_t)UINT32_MAX + 1;
	uint64_t child;
	uint16_t depth = UINT16_MAX;
	uint16_t count;
	uint16_t index;
	uint16_t previous;
	uint16_t level = 0;
	enum ext4_result error;

	ext4_zero(path, sizeof(*path));
	path->nodes[0] = disk->block_data;
	for (;;) {
		error = ext4_extent_check(
		    allocation, inode, path->nodes[level], level != 0, depth, low, high);
		if (error != EXT4_OK) {
			return error;
		}
		header = (struct ext4_extent_header_disk *)path->nodes[level];
		depth = ext4_le16(&header->depth);
		count = ext4_le16(&header->entries);
		indices = (struct ext4_extent_index_disk *)(header + 1);
		if (depth == 0) {
			path->levels = level + 1;
			return EXT4_OK;
		}
		for (index = 0; index + 1 < count; index++) {
			if (logical < ext4_le32(&indices[index + 1].logical)) {
				break;
			}
		}
		path->positions[level] = index;
		low = ext4_le32(&indices[index].logical);
		if (index + 1 < count) {
			high = ext4_le32(&indices[index + 1].logical);
		}
		child = ext4_extent_child(&indices[index]);
		for (previous = 1; previous <= level; previous++) {
			if (child == path->blocks[previous]) {
				return EXT4_CORRUPT;
			}
		}
		error = ext4_allocation_valid(allocation, child);
		if (error == EXT4_OK) {
			error = ext4_transaction_buffer(allocation->transaction, child, &buffer);
		}
		if (error != EXT4_OK) {
			return error;
		}
		level++;
		path->nodes[level] = buffer;
		path->blocks[level] = child;
		depth--;
	}
}

static void
ext4_extent_install(struct ext4_allocation *allocation, const struct ext4_inode *inode,
    uint8_t *node, bool external, const void *records, uint16_t count)
{
	struct ext4_fs *fs = allocation->fs;
	struct ext4_extent_header_disk *header = (struct ext4_extent_header_disk *)node;
	struct ext4_le32 *tail;
	size_t tail_offset =
	    sizeof(*header) + ext4_le16(&header->maximum) * sizeof(struct ext4_extent_disk);

	ext4_zero(header + 1, tail_offset - sizeof(*header));
	ext4_copy(header + 1, records, count * sizeof(struct ext4_extent_disk));
	ext4_encode16(&header->entries, count);
	if (external && fs->metadata_checksum) {
		tail = (struct ext4_le32 *)(node + tail_offset);
		ext4_encode32(tail, ext4_crc32c(ext4_inode_seed(fs, inode), node, tail_offset));
	}
}

static enum ext4_result
ext4_extent_new(struct ext4_allocation *allocation, uint16_t depth, uint64_t *block, uint8_t **node)
{
	struct ext4_fs *fs = allocation->fs;
	struct ext4_extent_header_disk *header;
	void *buffer = NULL;
	enum ext4_result error;

	error = ext4_allocate_block(allocation, block);
	if (error == EXT4_OK) {
		error = ext4_transaction_buffer(allocation->transaction, *block, &buffer);
	}
	if (error != EXT4_OK) {
		return error;
	}
	*node = buffer;
	ext4_zero(buffer, fs->info.block_size);
	header = buffer;
	ext4_encode16(&header->magic, EXT4_EXTENT_MAGIC);
	ext4_encode16(&header->depth, depth);
	ext4_encode16(&header->maximum,
	    (uint16_t)((fs->info.block_size - sizeof(*header)) / sizeof(struct ext4_extent_disk)));
	return EXT4_OK;
}

/* Replace a node's entries. On overflow an external node splits; an inode root
 * moves into one larger external block. The caller propagates keys iteratively. */
static enum ext4_result
ext4_extent_replace(struct ext4_allocation *allocation, const struct ext4_inode *inode,
    struct ext4_extent_path *path, uint16_t level, const uint8_t *records, uint16_t count,
    struct ext4_extent_index_disk *left, struct ext4_extent_index_disk *right, bool *split)
{
	uint8_t *node = path->nodes[level];
	uint8_t *created;
	struct ext4_extent_header_disk *header = (struct ext4_extent_header_disk *)node;
	const struct ext4_extent_disk *entries = (const struct ext4_extent_disk *)records;
	uint64_t block;
	uint16_t maximum = ext4_le16(&header->maximum);
	uint16_t depth = ext4_le16(&header->depth);
	uint16_t half;
	enum ext4_result error;

	*split = false;
	if (count <= maximum) {
		ext4_extent_install(allocation, inode, node, level != 0, records, count);
		ext4_extent_index_make(left, ext4_le32(&entries[0].logical), path->blocks[level]);
		return EXT4_OK;
	}
	if (level == 0 && depth == EXT4_EXTENT_MAX_DEPTH) {
		return EXT4_RANGE;
	}
	error = ext4_extent_new(allocation, depth, &block, &created);
	if (error != EXT4_OK) {
		return error;
	}
	if (level == 0) {
		ext4_extent_install(allocation, inode, created, true, records, count);
		ext4_extent_index_make(left, ext4_le32(&entries[0].logical), block);
		ext4_encode16(&header->depth, depth + 1);
		ext4_encode16(&header->maximum,
		    (uint16_t)((EXT4_INODE_BLOCK_BYTES - sizeof(*header)) / sizeof(*entries)));
		ext4_extent_install(allocation, inode, node, false, left, 1);
		return EXT4_OK;
	}
	half = count / 2;
	ext4_extent_install(allocation, inode, node, true, records, half);
	ext4_extent_install(allocation, inode, created, true, &entries[half], count - half);
	ext4_extent_index_make(left, ext4_le32(&entries[0].logical), path->blocks[level]);
	ext4_extent_index_make(right, ext4_le32(&entries[half].logical), block);
	*split = true;
	return EXT4_OK;
}

static uint16_t
ext4_extent_merge(struct ext4_extent_disk *entries, uint16_t count)
{
	struct ext4_extent_disk *previous;
	uint16_t used = 0;
	uint16_t index;
	uint32_t total;
	bool unwritten;
	bool previous_unwritten;

	for (index = 0; index < count; index++) {
		if (used != 0) {
			previous = &entries[used - 1];
			unwritten = ext4_le16(&entries[index].length) > EXT4_EXTENT_UNWRITTEN_LIMIT;
			previous_unwritten =
			    ext4_le16(&previous->length) > EXT4_EXTENT_UNWRITTEN_LIMIT;
			total = (uint32_t)ext4_extent_length(previous) +
			    ext4_extent_length(&entries[index]);
			if (unwritten == previous_unwritten &&
			    total <= EXT4_EXTENT_UNWRITTEN_LIMIT - (unwritten ? 1U : 0U) &&
			    (uint64_t)ext4_le32(&previous->logical) +
				    ext4_extent_length(previous) ==
				ext4_le32(&entries[index].logical) &&
			    ext4_extent_physical(previous) + ext4_extent_length(previous) ==
				ext4_extent_physical(&entries[index])) {
				ext4_encode16(&previous->length,
				    (uint16_t)(total +
					(unwritten ? EXT4_EXTENT_UNWRITTEN_LIMIT : 0)));
				continue;
			}
		}
		ext4_copy(&entries[used++], &entries[index], sizeof(*entries));
	}
	return used;
}

static enum ext4_result
ext4_extent_insert(struct ext4_allocation *allocation, const struct ext4_inode *inode,
    struct ext4_inode_disk *disk, uint32_t logical, uint64_t physical)
{
	struct ext4_fs *fs = allocation->fs;
	struct ext4_extent_path path;
	struct ext4_extent_header_disk *header;
	struct ext4_extent_disk *entries;
	struct ext4_extent_disk *output;
	struct ext4_extent_index_disk *indices;
	struct ext4_extent_index_disk left;
	struct ext4_extent_index_disk right;
	uint8_t *records;
	size_t record_bytes = fs->info.block_size + 2 * sizeof(*entries);
	uint64_t start;
	uint64_t end;
	uint16_t count;
	uint16_t used = 0;
	uint16_t index;
	uint16_t position;
	uint16_t level;
	bool inserted = false;
	bool split;
	enum ext4_result error;

	error = ext4_extent_path_get(allocation, inode, disk, logical, &path);
	if (error != EXT4_OK) {
		return error;
	}
	for (level = 1; level < path.levels; level++) {
		if (path.blocks[level] == physical) {
			return EXT4_CORRUPT;
		}
	}
	records = fs->environment.allocate(fs->environment.context, record_bytes);
	if (records == NULL) {
		return EXT4_NO_MEMORY;
	}
	level = path.levels - 1;
	header = (struct ext4_extent_header_disk *)path.nodes[level];
	entries = (struct ext4_extent_disk *)(header + 1);
	output = (struct ext4_extent_disk *)records;
	count = ext4_le16(&header->entries);
	for (index = 0; index < count; index++) {
		start = ext4_le32(&entries[index].logical);
		end = start + ext4_extent_length(&entries[index]);
		if (!inserted && logical < start) {
			ext4_extent_make(&output[used++], logical, physical, 1, false);
			inserted = true;
		}
		if (logical >= start && logical < end) {
			if (ext4_le16(&entries[index].length) <= EXT4_EXTENT_UNWRITTEN_LIMIT ||
			    physical != ext4_extent_physical(&entries[index]) + logical - start) {
				error = EXT4_CORRUPT;
				goto out;
			}
			if (logical != start) {
				ext4_extent_make(&output[used++], (uint32_t)start,
				    ext4_extent_physical(&entries[index]),
				    (uint16_t)(logical - start), true);
			}
			ext4_extent_make(&output[used++], logical, physical, 1, false);
			if ((uint64_t)logical + 1 != end) {
				ext4_extent_make(&output[used++], logical + 1, physical + 1,
				    (uint16_t)(end - logical - 1), true);
			}
			inserted = true;
		} else {
			ext4_copy(&output[used++], &entries[index], sizeof(*entries));
		}
	}
	if (!inserted) {
		ext4_extent_make(&output[used++], logical, physical, 1, false);
	}
	used = ext4_extent_merge(output, used);
	for (;;) {
		error = ext4_extent_replace(
		    allocation, inode, &path, level, records, used, &left, &right, &split);
		if (error != EXT4_OK || level == 0) {
			break;
		}
		level--;
		header = (struct ext4_extent_header_disk *)path.nodes[level];
		indices = (struct ext4_extent_index_disk *)(header + 1);
		count = ext4_le16(&header->entries);
		position = path.positions[level];
		ext4_copy(records, indices, position * sizeof(*indices));
		ext4_copy(records + position * sizeof(*indices), &left, sizeof(left));
		used = position + 1;
		if (split) {
			ext4_copy(records + used++ * sizeof(*indices), &right, sizeof(right));
		}
		ext4_copy(records + used * sizeof(*indices), &indices[position + 1],
		    (count - position - 1U) * sizeof(*indices));
		used = count + (split ? 1U : 0U);
	}
out:
	fs->environment.release(fs->environment.context, records, record_bytes);
	return error;
}

static enum ext4_result
ext4_indirect_allocate(struct ext4_allocation *allocation, struct ext4_inode_disk *disk,
    uint32_t logical, uint64_t *physical)
{
	struct ext4_fs *fs = allocation->fs;
	struct ext4_le32 *pointers = (struct ext4_le32 *)disk->block_data;
	struct ext4_le32 *slot;
	void *buffer;
	uint64_t ancestors[EXT4_INDIRECT_LEVELS];
	uint64_t remaining;
	uint64_t span;
	uint64_t block;
	uint16_t depth;
	uint16_t level;
	uint16_t previous;
	bool created;
	enum ext4_result error;

	error = ext4_indirect_position(fs, logical, &depth, &remaining, &span);
	if (error != EXT4_OK) {
		return error;
	}
	slot = &pointers[depth == 0 ? logical : EXT4_DIRECT_BLOCKS + depth - 1];
	for (level = 0; level <= depth; level++) {
		block = ext4_le32(slot);
		created = block == 0;
		if (created) {
			error = ext4_allocate_block(allocation, &block);
			if (error != EXT4_OK) {
				return error;
			}
			ext4_encode32(slot, (uint32_t)block);
		} else {
			error = ext4_allocation_valid(allocation, block);
			if (error != EXT4_OK) {
				return error;
			}
		}
		for (previous = 0; previous < level; previous++) {
			if (block == ancestors[previous]) {
				return EXT4_CORRUPT;
			}
		}
		if (level == depth) {
			if (!created) {
				return EXT4_CORRUPT;
			}
			*physical = block;
			return EXT4_OK;
		}
		ancestors[level] = block;
		error = ext4_transaction_buffer(allocation->transaction, block, &buffer);
		if (error != EXT4_OK) {
			return error;
		}
		if (created) {
			ext4_zero(buffer, fs->info.block_size);
		}
		span /= fs->info.block_size / sizeof(*pointers);
		pointers = buffer;
		slot = &pointers[remaining / span];
		remaining %= span;
	}
	return EXT4_CORRUPT;
}

enum ext4_result
ext4_write_map_allocate(struct ext4_allocation *allocation, const struct ext4_inode *inode,
    struct ext4_inode_disk *disk, uint32_t logical, uint64_t *physical, bool *zero)
{
	struct ext4_map_run run;
	enum ext4_result error;

	error = ext4_write_map_lookup(allocation, inode, disk, logical, &run);
	if (error != EXT4_OK) {
		return error;
	}
	*zero = run.physical == 0 || run.unwritten;
	*physical = run.physical;
	if (run.physical != 0) {
		error = ext4_allocation_valid(allocation, run.physical);
		if (error != EXT4_OK || !run.unwritten) {
			return error;
		}
	}
	if (!(inode->flags & EXT4_INODE_EXTENTS)) {
		return ext4_indirect_allocate(allocation, disk, logical, physical);
	}
	if (run.physical == 0) {
		error = ext4_allocate_block(allocation, physical);
		if (error != EXT4_OK) {
			return error;
		}
	}
	return ext4_extent_insert(allocation, inode, disk, logical, *physical);
}

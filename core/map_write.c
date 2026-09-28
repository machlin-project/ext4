/* SPDX-License-Identifier: BSD-3-Clause */
#include "allocate.h"
#include "xattr.h"

struct ext4_extent_path {
	uint8_t *nodes[EXT4_EXTENT_MAX_DEPTH + 1];
	uint64_t blocks[EXT4_EXTENT_MAX_DEPTH + 1];
	uint16_t positions[EXT4_EXTENT_MAX_DEPTH + 1];
	uint16_t levels;
};

static uint16_t ext4_extent_merge(struct ext4_extent_disk *entries, uint16_t count);

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
ext4_unwritten_entry(struct ext4_allocation *allocation, const struct ext4_inode *inode,
    struct ext4_inode_disk *disk, uint32_t logical, struct ext4_extent_path *path,
    struct ext4_extent_disk **result)
{
	struct ext4_extent_header_disk *header;
	struct ext4_extent_disk *entries;
	uint64_t start;
	uint16_t index;
	enum ext4_result error;

	*result = NULL;
	if (!(inode->flags & EXT4_INODE_EXTENTS)) {
		return EXT4_OK;
	}
	error = ext4_extent_path_get(allocation, inode, disk, logical, path);
	if (error != EXT4_OK) {
		return error;
	}
	header = (struct ext4_extent_header_disk *)path->nodes[path->levels - 1];
	entries = (struct ext4_extent_disk *)(header + 1);
	for (index = 0; index < ext4_le16(&header->entries); index++) {
		start = ext4_le32(&entries[index].logical);
		if (logical >= start && logical - start < ext4_extent_length(&entries[index]) &&
		    ext4_le16(&entries[index].length) > EXT4_EXTENT_UNWRITTEN_LIMIT) {
			*result = &entries[index];
			break;
		}
	}
	return EXT4_OK;
}

enum ext4_result
ext4_write_map_unwritten(struct ext4_allocation *allocation, const struct ext4_inode *inode,
    struct ext4_inode_disk *disk, uint32_t logical, struct ext4_unwritten_extent *range)
{
	struct ext4_extent_path path;
	struct ext4_extent_disk *entry;
	enum ext4_result error;

	ext4_zero(range, sizeof(*range));
	error = ext4_unwritten_entry(allocation, inode, disk, logical, &path, &entry);
	if (error == EXT4_OK && entry != NULL) {
		range->logical = ext4_le32(&entry->logical);
		range->physical = ext4_extent_physical(entry);
		range->length = ext4_extent_length(entry);
	}
	return error;
}

enum ext4_result
ext4_write_map_initialize(struct ext4_allocation *allocation, const struct ext4_inode *inode,
    struct ext4_inode_disk *disk, const struct ext4_unwritten_extent *range, uint32_t length)
{
	struct ext4_fs *fs = allocation->fs;
	struct ext4_extent_path path;
	struct ext4_extent_disk *entry;
	struct ext4_extent_header_disk *header;
	struct ext4_extent_disk *entries;
	struct ext4_extent_disk *output;
	uint8_t *node;
	size_t bytes = fs->info.block_size + sizeof(*entry);
	uint16_t count;
	uint16_t index;
	uint16_t used = 0;
	enum ext4_result error;

	if (length == 0 || length > range->length) {
		return EXT4_CORRUPT;
	}
	error = ext4_unwritten_entry(allocation, inode, disk, range->logical, &path, &entry);
	if (error != EXT4_OK) {
		return error;
	}
	if (entry == NULL || ext4_le32(&entry->logical) != range->logical ||
	    ext4_extent_physical(entry) != range->physical ||
	    ext4_extent_length(entry) != range->length) {
		return EXT4_CORRUPT;
	}
	node = path.nodes[path.levels - 1];
	header = (struct ext4_extent_header_disk *)node;
	entries = (struct ext4_extent_disk *)(header + 1);
	count = ext4_le16(&header->entries);
	output = fs->environment.allocate(fs->environment.context, bytes);
	if (output == NULL) {
		return EXT4_NO_MEMORY;
	}
	for (index = 0; index < count; index++) {
		if (&entries[index] != entry) {
			ext4_copy(&output[used++], &entries[index], sizeof(*entry));
			continue;
		}
		ext4_extent_make(
		    &output[used++], range->logical, range->physical, (uint16_t)length, false);
		if (length < range->length) {
			ext4_extent_make(&output[used++], range->logical + length,
			    range->physical + length, (uint16_t)(range->length - length), true);
		}
	}
	used = ext4_extent_merge(output, used);
	if (used <= ext4_le16(&header->maximum)) {
		/* A growing prefix can temporarily consume the reserved leaf slot.
		 * Further growth merges into this contiguous initialized prefix;
		 * reaching its end releases the slot for the next reservation. */
		ext4_extent_install(allocation, inode, node, path.levels > 1, output, used);
		error = EXT4_OK;
	} else {
		error = EXT4_NO_SPACE;
	}
	fs->environment.release(fs->environment.context, output, bytes);
	return error;
}

static bool
ext4_extent_zero_mergeable(
    const struct ext4_extent_disk *left, const struct ext4_extent_disk *right)
{
	uint32_t left_length = ext4_extent_length(left);
	uint32_t right_length = ext4_extent_length(right);

	return ext4_le16(&left->length) <= EXT4_EXTENT_UNWRITTEN_LIMIT &&
	    ext4_le16(&right->length) > EXT4_EXTENT_UNWRITTEN_LIMIT &&
	    left_length + right_length <= EXT4_EXTENT_UNWRITTEN_LIMIT &&
	    (uint64_t)ext4_le32(&left->logical) + left_length == ext4_le32(&right->logical) &&
	    ext4_extent_physical(left) + left_length == ext4_extent_physical(right);
}

enum ext4_result
ext4_write_map_mergeable(struct ext4_allocation *allocation, const struct ext4_inode *inode,
    struct ext4_inode_disk *disk, uint32_t logical, uint64_t end,
    struct ext4_unwritten_extent *range)
{
	struct ext4_extent_path path;
	struct ext4_extent_header_disk *header;
	struct ext4_extent_disk *entries;
	uint64_t last;
	uint16_t index;
	enum ext4_result error;

	ext4_zero(range, sizeof(*range));
	error = ext4_extent_path_get(allocation, inode, disk, logical, &path);
	if (error != EXT4_OK) {
		return error;
	}
	header = (struct ext4_extent_header_disk *)path.nodes[path.levels - 1];
	entries = (struct ext4_extent_disk *)(header + 1);
	for (index = 1; index < ext4_le16(&header->entries); index++) {
		last = (uint64_t)ext4_le32(&entries[index].logical) +
		    ext4_extent_length(&entries[index]) - 1U;
		if (last * allocation->fs->info.block_size < end &&
		    ext4_extent_zero_mergeable(&entries[index - 1U], &entries[index])) {
			range->logical = ext4_le32(&entries[index].logical);
			range->physical = ext4_extent_physical(&entries[index]);
			range->length = ext4_extent_length(&entries[index]);
			break;
		}
	}
	return EXT4_OK;
}

static bool
ext4_extent_needs_capacity(const struct ext4_allocation *allocation, const struct ext4_inode *inode,
    const struct ext4_extent_disk *entries, uint16_t count)
{
	uint64_t size =
	    inode->size > allocation->mapping_size ? inode->size : allocation->mapping_size;
	uint64_t last;
	uint16_t index;
	uint16_t length;
	bool needed = false;

	for (index = 0; index < count; index++) {
		length = ext4_extent_length(&entries[index]);
		last = (uint64_t)ext4_le32(&entries[index].logical) + length - 1U;
		if (index != 0 &&
		    ext4_extent_zero_mergeable(&entries[index - 1U], &entries[index]) &&
		    ((uint64_t)ext4_le32(&entries[index].logical) - 1U) *
			    allocation->fs->info.block_size <
			size) {
			/* An existing EOF boundary already owns the spare record. */
			return false;
		}
		if (ext4_le16(&entries[index].length) > EXT4_EXTENT_UNWRITTEN_LIMIT && length > 1 &&
		    last * allocation->fs->info.block_size >= size) {
			needed = true;
		}
	}
	return needed;
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
	uint16_t capacity;
	uint16_t half;
	enum ext4_result error;

	*split = false;
	/* One spare record represents the only partial initialized/unwritten
	 * boundary after zeroing a prefix. Reserve it before promising KEEP_SIZE
	 * backing, and preserve it through unrelated insertions and hole punches. */
	if (depth == 0 && ext4_extent_needs_capacity(allocation, inode, entries, count)) {
		/* Imported leaves can advertise less than their physical capacity,
		 * even one record. Expand that bound before considering a split;
		 * splitting a singleton would otherwise create an empty child. */
		capacity = (uint16_t)(((level == 0 ? EXT4_INODE_BLOCK_BYTES
						   : allocation->fs->info.block_size) -
					  sizeof(*header)) /
		    sizeof(*entries));
		if (maximum < capacity) {
			maximum = capacity;
			ext4_encode16(&header->maximum, maximum);
		}
		maximum--;
	}
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
ext4_extent_update(struct ext4_allocation *allocation, const struct ext4_inode *inode,
    struct ext4_extent_path *path, uint8_t *records, uint16_t used)
{
	struct ext4_extent_header_disk *header;
	struct ext4_extent_index_disk *indices;
	struct ext4_extent_index_disk left;
	struct ext4_extent_index_disk right;
	uint16_t level = path->levels - 1;
	uint16_t count;
	uint16_t position;
	uint16_t next;
	bool removed;
	bool split;
	enum ext4_result error;

	for (;;) {
		removed = used == 0;
		split = false;
		if (removed) {
			if (level == 0) {
				header = (struct ext4_extent_header_disk *)path->nodes[0];
				ext4_encode16(&header->depth, 0);
				ext4_extent_install(
				    allocation, inode, path->nodes[0], false, records, 0);
				return EXT4_OK;
			}
			error = ext4_free_blocks(allocation, path->blocks[level], 1);
		} else {
			error = ext4_extent_replace(
			    allocation, inode, path, level, records, used, &left, &right, &split);
		}
		if (error != EXT4_OK || level == 0) {
			return error;
		}
		level--;
		header = (struct ext4_extent_header_disk *)path->nodes[level];
		indices = (struct ext4_extent_index_disk *)(header + 1);
		count = ext4_le16(&header->entries);
		position = path->positions[level];
		ext4_copy(records, indices, position * sizeof(*indices));
		next = position;
		if (!removed) {
			ext4_copy(records + next++ * sizeof(*indices), &left, sizeof(left));
			if (split) {
				ext4_copy(
				    records + next++ * sizeof(*indices), &right, sizeof(right));
			}
		}
		ext4_copy(records + next * sizeof(*indices), &indices[position + 1],
		    (count - position - 1U) * sizeof(*indices));
		used = count + (split ? 1U : 0U) - (removed ? 1U : 0U);
	}
}

static enum ext4_result
ext4_extent_insert(struct ext4_allocation *allocation, const struct ext4_inode *inode,
    struct ext4_inode_disk *disk, uint32_t logical, uint64_t physical, bool unwritten)
{
	struct ext4_fs *fs = allocation->fs;
	struct ext4_extent_path path;
	struct ext4_extent_header_disk *header;
	struct ext4_extent_disk *entries;
	struct ext4_extent_disk *output;
	uint8_t *records;
	size_t record_bytes = fs->info.block_size + 2 * sizeof(*entries);
	uint64_t start;
	uint64_t end;
	uint16_t count;
	uint16_t used = 0;
	uint16_t index;
	uint16_t level;
	bool inserted = false;
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
			ext4_extent_make(&output[used++], logical, physical, 1, unwritten);
			inserted = true;
		}
		if (logical >= start && logical < end) {
			if (unwritten ||
			    ext4_le16(&entries[index].length) <= EXT4_EXTENT_UNWRITTEN_LIMIT ||
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
		ext4_extent_make(&output[used++], logical, physical, 1, unwritten);
	}
	used = ext4_extent_merge(output, used);
	error = ext4_extent_update(allocation, inode, &path, records, used);
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
	return ext4_extent_insert(allocation, inode, disk, logical, *physical, false);
}

enum ext4_result
ext4_write_map_reserve_capacity(struct ext4_allocation *allocation, const struct ext4_inode *inode,
    struct ext4_inode_disk *disk, uint32_t logical)
{
	struct ext4_fs *fs = allocation->fs;
	struct ext4_extent_path path;
	struct ext4_extent_header_disk *header;
	uint8_t *records;
	size_t bytes = fs->info.block_size + sizeof(struct ext4_extent_disk);
	uint16_t count;
	enum ext4_result error;

	error = ext4_extent_path_get(allocation, inode, disk, logical, &path);
	if (error != EXT4_OK) {
		return error;
	}
	header = (struct ext4_extent_header_disk *)path.nodes[path.levels - 1];
	count = ext4_le16(&header->entries);
	if (count < ext4_le16(&header->maximum) ||
	    !ext4_extent_needs_capacity(
		allocation, inode, (const struct ext4_extent_disk *)(header + 1), count)) {
		return EXT4_OK;
	}
	records = fs->environment.allocate(fs->environment.context, bytes);
	if (records == NULL) {
		return EXT4_NO_MEMORY;
	}
	ext4_copy(records, header + 1, count * sizeof(struct ext4_extent_disk));
	error = ext4_extent_update(allocation, inode, &path, records, count);
	fs->environment.release(fs->environment.context, records, bytes);
	return error;
}

enum ext4_result
ext4_write_map_reserve(struct ext4_allocation *allocation, const struct ext4_inode *inode,
    struct ext4_inode_disk *disk, uint32_t logical)
{
	struct ext4_map_run run;
	uint64_t physical;
	enum ext4_result error;

	if (!(inode->flags & EXT4_INODE_EXTENTS)) {
		return EXT4_UNSUPPORTED;
	}
	error = ext4_write_map_lookup(allocation, inode, disk, logical, &run);
	if (error != EXT4_OK) {
		return error;
	}
	if (run.physical != 0) {
		return EXT4_OK;
	}
	error = ext4_allocate_block(allocation, &physical);
	return error == EXT4_OK
	    ? ext4_extent_insert(allocation, inode, disk, logical, physical, true)
	    : error;
}

#define EXT4_INODE_MAX_RANGES (1U << 20)

struct ext4_map_owners {
	struct ext4_block_range *ranges;
	size_t count;
	size_t capacity;
	uint64_t blocks;
};

static enum ext4_result
ext4_map_owner_add(struct ext4_allocation *allocation, struct ext4_map_owners *owners,
    uint64_t block, uint64_t length)
{
	struct ext4_fs *fs = allocation->fs;
	struct ext4_block_range *ranges;
	struct ext4_block_range *last;
	size_t capacity;

	if (block < fs->first_data_block || block >= fs->info.blocks || length == 0 ||
	    length > fs->info.blocks - block || length > fs->info.blocks - owners->blocks ||
	    ext4_system_overlaps(fs, block, length)) {
		return EXT4_CORRUPT;
	}
	owners->blocks += length;
	if (owners->count != 0) {
		last = &owners->ranges[owners->count - 1];
		if (last->first + last->length == block) {
			last->length += length;
			return EXT4_OK;
		}
	}
	if (owners->count == owners->capacity) {
		capacity = owners->capacity == 0 ? 32 : owners->capacity * 2;
		if (capacity > EXT4_INODE_MAX_RANGES) {
			return EXT4_RANGE;
		}
		ranges =
		    fs->environment.allocate(fs->environment.context, capacity * sizeof(*ranges));
		if (ranges == NULL) {
			return EXT4_NO_MEMORY;
		}
		if (owners->ranges != NULL) {
			ext4_copy(ranges, owners->ranges, owners->count * sizeof(*ranges));
			fs->environment.release(fs->environment.context, owners->ranges,
			    owners->capacity * sizeof(*ranges));
		}
		owners->ranges = ranges;
		owners->capacity = capacity;
	}
	owners->ranges[owners->count].first = block;
	owners->ranges[owners->count++].length = length;
	return EXT4_OK;
}

struct ext4_extent_walk {
	uint8_t *node;
	uint64_t low;
	uint64_t high;
	uint16_t next;
	uint16_t depth;
};

static enum ext4_result
ext4_extent_owners(struct ext4_allocation *allocation, const struct ext4_inode *inode,
    struct ext4_inode_disk *disk, struct ext4_map_owners *owners, uint8_t *scratch)
{
	struct ext4_extent_walk frames[EXT4_EXTENT_MAX_DEPTH + 1];
	struct ext4_extent_walk *frame;
	struct ext4_extent_header_disk *header;
	struct ext4_extent_disk *entries;
	struct ext4_extent_index_disk *indices;
	uint8_t *buffer;
	uint64_t child;
	uint16_t count;
	uint16_t position;
	uint16_t level = 0;
	enum ext4_result error;

	ext4_zero(frames, sizeof(frames));
	frames[0].node = disk->block_data;
	frames[0].high = (uint64_t)UINT32_MAX + 1;
	frames[0].depth = UINT16_MAX;
	for (;;) {
		frame = &frames[level];
		header = (struct ext4_extent_header_disk *)frame->node;
		if (frame->next == 0) {
			error = ext4_extent_check(allocation, inode, frame->node, level != 0,
			    frame->depth, frame->low, frame->high);
			if (error != EXT4_OK) {
				return error;
			}
			frame->depth = ext4_le16(&header->depth);
		}
		count = ext4_le16(&header->entries);
		if (frame->next == count) {
			if (level == 0) {
				return EXT4_OK;
			}
			level--;
			continue;
		}
		position = frame->next++;
		entries = (struct ext4_extent_disk *)(header + 1);
		indices = (struct ext4_extent_index_disk *)(header + 1);
		if (frame->depth == 0) {
			error = ext4_map_owner_add(allocation, owners,
			    ext4_extent_physical(&entries[position]),
			    ext4_extent_length(&entries[position]));
			if (error != EXT4_OK) {
				return error;
			}
			continue;
		}
		child = ext4_extent_child(&indices[position]);
		buffer = scratch + (size_t)level * allocation->fs->info.block_size;
		error = ext4_map_owner_add(allocation, owners, child, 1);
		if (error == EXT4_OK) {
			error = ext4_transaction_read(allocation->transaction, child, buffer);
		}
		if (error != EXT4_OK) {
			return error;
		}
		level++;
		frames[level].node = buffer;
		frames[level].next = 0;
		frames[level].depth = frame->depth - 1;
		frames[level].low = ext4_le32(&indices[position].logical);
		frames[level].high =
		    position + 1 < count ? ext4_le32(&indices[position + 1].logical) : frame->high;
	}
}

struct ext4_indirect_walk {
	struct ext4_le32 *pointers;
	struct ext4_le32 *parent;
	uint64_t block;
	uint64_t logical;
	uint64_t span;
	uint32_t next;
};

static enum ext4_result
ext4_indirect_owners(struct ext4_allocation *allocation, struct ext4_inode_disk *disk,
    struct ext4_map_owners *owners, uint8_t *scratch)
{
	struct ext4_le32 *root = (struct ext4_le32 *)disk->block_data;
	struct ext4_indirect_walk frames[EXT4_INDIRECT_LEVELS];
	struct ext4_indirect_walk *frame;
	uint8_t *buffer;
	uint64_t block;
	uint32_t per_block = allocation->fs->info.block_size / sizeof(*root);
	uint32_t index;
	uint16_t depth;
	uint16_t level;
	enum ext4_result error;

	for (index = 0; index < EXT4_DIRECT_BLOCKS; index++) {
		block = ext4_le32(&root[index]);
		if (block != 0) {
			error = ext4_map_owner_add(allocation, owners, block, 1);
			if (error != EXT4_OK) {
				return error;
			}
		}
	}
	for (depth = 1; depth <= EXT4_INDIRECT_LEVELS; depth++) {
		block = ext4_le32(&root[EXT4_DIRECT_BLOCKS + depth - 1]);
		if (block == 0) {
			continue;
		}
		level = 0;
		for (;;) {
			buffer = scratch + (size_t)level * allocation->fs->info.block_size;
			error = ext4_map_owner_add(allocation, owners, block, 1);
			if (error == EXT4_OK) {
				error =
				    ext4_transaction_read(allocation->transaction, block, buffer);
			}
			if (error != EXT4_OK) {
				return error;
			}
			frames[level].pointers = (struct ext4_le32 *)buffer;
			frames[level].next = 0;
			for (;;) {
				frame = &frames[level];
				if (frame->next == per_block) {
					if (level == 0) {
						break;
					}
					level--;
					continue;
				}
				block = ext4_le32(&frame->pointers[frame->next++]);
				if (block == 0) {
					continue;
				}
				if (level + 1 < depth) {
					level++;
					break;
				}
				error = ext4_map_owner_add(allocation, owners, block, 1);
				if (error != EXT4_OK) {
					return error;
				}
			}
			if (level == 0 && frames[0].next == per_block) {
				break;
			}
		}
	}
	return EXT4_OK;
}

enum ext4_result
ext4_write_map_validate(struct ext4_allocation *allocation, const struct ext4_inode *inode,
    struct ext4_inode_disk *disk)
{
	struct ext4_fs *fs = allocation->fs;
	struct ext4_map_owners owners;
	struct ext4_xattr_snapshot attributes;
	uint8_t *scratch;
	uint64_t attribute_block;
	uint64_t value_blocks = 0;
	uint16_t type = inode->mode & EXT4_MODE_TYPE;
	size_t scratch_size = (size_t)fs->info.block_size * EXT4_EXTENT_MAX_DEPTH;
	size_t index;
	enum ext4_result error;

	ext4_zero(&owners, sizeof(owners));
	scratch = fs->environment.allocate(fs->environment.context, scratch_size);
	if (scratch == NULL) {
		return EXT4_NO_MEMORY;
	}
	if (type == EXT4_MODE_REGULAR || type == EXT4_MODE_DIRECTORY ||
	    (type == EXT4_MODE_SYMLINK && !inode->fast_symlink)) {
		error = inode->flags & EXT4_INODE_EXTENTS
		    ? ext4_extent_owners(allocation, inode, disk, &owners, scratch)
		    : ext4_indirect_owners(allocation, disk, &owners, scratch);
	} else {
		error = inode->flags & EXT4_INODE_EXTENTS ? EXT4_CORRUPT : EXT4_OK;
	}
	attribute_block =
	    ext4_le32(&disk->xattr_block_lo) | ((uint64_t)ext4_le16(&disk->xattr_block_hi) << 32);
	if (error == EXT4_OK && attribute_block != 0) {
		error = ext4_map_owner_add(allocation, &owners, attribute_block, 1);
	}
	if (error == EXT4_OK && (fs->info.feature_incompat & EXT4_FEATURE_INCOMPAT_EA_INODE) &&
	    ext4_inode_has_xattrs(fs, disk)) {
		error = ext4_xattr_open_inode(fs, inode, disk, &attributes);
		if (error == EXT4_OK) {
			value_blocks = ext4_xattr_value_blocks(&attributes);
		}
		ext4_xattr_close(&attributes);
	}
	if (error == EXT4_OK &&
	    (inode->blocks_512 % (fs->info.block_size / EXT4_SECTOR_SIZE) != 0 ||
		owners.blocks + value_blocks !=
		    inode->blocks_512 / (fs->info.block_size / EXT4_SECTOR_SIZE))) {
		error = EXT4_CORRUPT;
	}
	if (error == EXT4_OK) {
		error = ext4_ranges_sort(owners.ranges, &owners.count);
	}
	for (index = 0; error == EXT4_OK && index < owners.count; index++) {
		error = ext4_allocation_valid_range(
		    allocation, owners.ranges[index].first, owners.ranges[index].length);
	}
	if (owners.ranges != NULL) {
		fs->environment.release(fs->environment.context, owners.ranges,
		    owners.capacity * sizeof(*owners.ranges));
	}
	fs->environment.release(fs->environment.context, scratch, scratch_size);
	return error;
}

static void
ext4_extent_shorten(struct ext4_allocation *allocation, const struct ext4_inode *inode,
    uint8_t *node, bool external, uint16_t count)
{
	struct ext4_extent_header_disk *header = (struct ext4_extent_header_disk *)node;
	struct ext4_extent_disk *entries = (struct ext4_extent_disk *)(header + 1);
	size_t tail_offset = sizeof(*header) + ext4_le16(&header->maximum) * sizeof(*entries);
	struct ext4_le32 *tail = (struct ext4_le32 *)(node + tail_offset);

	ext4_zero(&entries[count], (ext4_le16(&header->maximum) - count) * sizeof(*entries));
	ext4_encode16(&header->entries, count);
	if (external && allocation->fs->metadata_checksum) {
		ext4_encode32(
		    tail, ext4_crc32c(ext4_inode_seed(allocation->fs, inode), node, tail_offset));
	}
}

static enum ext4_result
ext4_extent_collapse(struct ext4_allocation *allocation, const struct ext4_inode *inode,
    struct ext4_inode_disk *disk)
{
	struct ext4_extent_header_disk *root = (struct ext4_extent_header_disk *)disk->block_data;
	struct ext4_extent_header_disk *header;
	struct ext4_extent_index_disk *indices;
	void *buffer;
	uint64_t child;
	uint16_t count;
	uint16_t root_max = (EXT4_INODE_BLOCK_BYTES - sizeof(*root)) / sizeof(*indices);
	enum ext4_result error;

	/* A sole small child fits back into the inode, including repeated collapse
	 * through an internal level. No block allocation is needed to shrink. */
	while (ext4_le16(&root->depth) != 0 && ext4_le16(&root->entries) == 1) {
		indices = (struct ext4_extent_index_disk *)(root + 1);
		child = ext4_extent_child(&indices[0]);
		error = ext4_transaction_buffer(allocation->transaction, child, &buffer);
		if (error != EXT4_OK) {
			return error;
		}
		header = buffer;
		count = ext4_le16(&header->entries);
		if (count > root_max ||
		    (count == root_max && ext4_le16(&header->depth) == 0 &&
			ext4_extent_needs_capacity(allocation, inode,
			    (const struct ext4_extent_disk *)(header + 1), count))) {
			break;
		}
		ext4_encode16(&root->depth, ext4_le16(&header->depth));
		ext4_encode16(&root->maximum, root_max);
		ext4_extent_install(allocation, inode, disk->block_data, false, header + 1, count);
		error = ext4_free_blocks(allocation, child, 1);
		if (error != EXT4_OK) {
			return error;
		}
	}
	return EXT4_OK;
}

static enum ext4_result
ext4_extent_truncate(struct ext4_allocation *allocation, const struct ext4_inode *inode,
    struct ext4_inode_disk *disk, uint32_t first, uint64_t limit, bool *done)
{
	struct ext4_extent_path path;
	struct ext4_extent_header_disk *header;
	struct ext4_extent_header_disk *root = (struct ext4_extent_header_disk *)disk->block_data;
	struct ext4_extent_disk *entries;
	struct ext4_extent_disk *entry;
	uint64_t start;
	uint16_t count;
	uint16_t length;
	uint16_t keep;
	uint16_t level;
	bool finished = false;
	bool stopped = false;
	bool unwritten;
	enum ext4_result error;

	while (!finished && !stopped) {
		error = ext4_extent_path_get(allocation, inode, disk, UINT32_MAX, &path);
		if (error != EXT4_OK) {
			return error;
		}
		level = path.levels - 1;
		header = (struct ext4_extent_header_disk *)path.nodes[level];
		entries = (struct ext4_extent_disk *)(header + 1);
		count = ext4_le16(&header->entries);
		while (count != 0) {
			entry = &entries[count - 1];
			start = ext4_le32(&entry->logical);
			length = ext4_extent_length(entry);
			if (start + length <= first) {
				finished = true;
				break;
			}
			keep = first > start ? (uint16_t)(first - start) : 0;
			if (allocation->freed >= limit) {
				stopped = true;
				break;
			}
			if ((uint64_t)(length - keep) > limit - allocation->freed) {
				keep = (uint16_t)(length - (limit - allocation->freed));
			}
			error = ext4_free_blocks(
			    allocation, ext4_extent_physical(entry) + keep, length - keep);
			if (error != EXT4_OK) {
				return error;
			}
			if (keep != 0) {
				unwritten = ext4_le16(&entry->length) > EXT4_EXTENT_UNWRITTEN_LIMIT;
				ext4_encode16(&entry->length,
				    keep + (unwritten ? EXT4_EXTENT_UNWRITTEN_LIMIT : 0));
				finished = start + keep <= first;
				stopped = !finished;
				break;
			}
			count--;
		}
		for (;;) {
			ext4_extent_shorten(
			    allocation, inode, path.nodes[level], level != 0, count);
			if (count != 0 || level == 0) {
				if (count == 0) {
					ext4_encode16(&root->depth, 0);
					finished = true;
				}
				break;
			}
			error = ext4_free_blocks(allocation, path.blocks[level], 1);
			if (error != EXT4_OK) {
				return error;
			}
			level--;
			header = (struct ext4_extent_header_disk *)path.nodes[level];
			count = ext4_le16(&header->entries) - 1;
		}
	}
	*done = finished;
	return ext4_extent_collapse(allocation, inode, disk);
}

static enum ext4_result
ext4_extent_punch(struct ext4_allocation *allocation, const struct ext4_inode *inode,
    struct ext4_inode_disk *disk, uint32_t logical, uint32_t length)
{
	struct ext4_fs *fs = allocation->fs;
	struct ext4_extent_path path;
	struct ext4_extent_header_disk *header;
	struct ext4_extent_disk *entries;
	struct ext4_extent_disk *output;
	uint8_t *records;
	size_t record_bytes = fs->info.block_size + sizeof(*entries);
	uint64_t start;
	uint64_t end;
	uint64_t removed_end = (uint64_t)logical + length;
	uint64_t physical;
	uint16_t count;
	uint16_t used = 0;
	uint16_t index;
	bool removed = false;
	bool unwritten;
	enum ext4_result error;

	error = ext4_extent_path_get(allocation, inode, disk, logical, &path);
	if (error != EXT4_OK) {
		return error;
	}
	records = fs->environment.allocate(fs->environment.context, record_bytes);
	if (records == NULL) {
		return EXT4_NO_MEMORY;
	}
	header = (struct ext4_extent_header_disk *)path.nodes[path.levels - 1];
	entries = (struct ext4_extent_disk *)(header + 1);
	output = (struct ext4_extent_disk *)records;
	count = ext4_le16(&header->entries);
	for (index = 0; index < count; index++) {
		start = ext4_le32(&entries[index].logical);
		end = start + ext4_extent_length(&entries[index]);
		if (logical < start || logical >= end) {
			ext4_copy(&output[used++], &entries[index], sizeof(*entries));
			continue;
		}
		if (removed_end > end) {
			error = EXT4_CORRUPT;
			goto out;
		}
		physical = ext4_extent_physical(&entries[index]);
		unwritten = ext4_le16(&entries[index].length) > EXT4_EXTENT_UNWRITTEN_LIMIT;
		error = ext4_free_blocks(allocation, physical + logical - start, length);
		if (error != EXT4_OK) {
			goto out;
		}
		if (logical > start) {
			ext4_extent_make(&output[used++], (uint32_t)start, physical,
			    (uint16_t)(logical - start), unwritten);
		}
		if (removed_end < end) {
			ext4_extent_make(&output[used++], (uint32_t)removed_end,
			    physical + removed_end - start, (uint16_t)(end - removed_end),
			    unwritten);
		}
		removed = true;
	}
	if (!removed) {
		error = EXT4_CORRUPT;
		goto out;
	}
	error = ext4_extent_update(allocation, inode, &path, records, used);
	if (error == EXT4_OK) {
		error = ext4_extent_collapse(allocation, inode, disk);
	}
out:
	fs->environment.release(fs->environment.context, records, record_bytes);
	return error;
}

static enum ext4_result
ext4_indirect_punch(
    struct ext4_allocation *allocation, struct ext4_inode_disk *disk, uint32_t logical)
{
	struct ext4_fs *fs = allocation->fs;
	struct ext4_le32 *pointers = (struct ext4_le32 *)disk->block_data;
	struct ext4_le32 *slots[EXT4_INDIRECT_LEVELS + 1];
	struct ext4_le32 *nodes[EXT4_INDIRECT_LEVELS];
	void *buffer;
	uint64_t blocks[EXT4_INDIRECT_LEVELS + 1];
	uint64_t remaining;
	uint64_t span;
	uint32_t per_block = fs->info.block_size / sizeof(*pointers);
	uint32_t index;
	uint16_t depth;
	uint16_t level;
	enum ext4_result error;

	error = ext4_indirect_position(fs, logical, &depth, &remaining, &span);
	if (error != EXT4_OK) {
		return error;
	}
	slots[0] = &pointers[depth == 0 ? logical : EXT4_DIRECT_BLOCKS + depth - 1];
	for (level = 0; level <= depth; level++) {
		blocks[level] = ext4_le32(slots[level]);
		if (blocks[level] == 0) {
			return EXT4_CORRUPT;
		}
		if (level == depth) {
			break;
		}
		error = ext4_transaction_buffer(allocation->transaction, blocks[level], &buffer);
		if (error != EXT4_OK) {
			return error;
		}
		nodes[level] = buffer;
		span /= per_block;
		slots[level + 1] = &nodes[level][remaining / span];
		remaining %= span;
	}
	for (;;) {
		error = ext4_free_blocks(allocation, blocks[level], 1);
		if (error != EXT4_OK) {
			return error;
		}
		ext4_encode32(slots[level], 0);
		if (level == 0) {
			return EXT4_OK;
		}
		level--;
		for (index = 0; index < per_block; index++) {
			if (ext4_le32(&nodes[level][index]) != 0) {
				return EXT4_OK;
			}
		}
	}
}

enum ext4_result
ext4_write_map_punch(struct ext4_allocation *allocation, const struct ext4_inode *inode,
    struct ext4_inode_disk *disk, uint32_t logical, uint32_t length)
{
	if (length == 0 || (uint64_t)logical + length > (uint64_t)UINT32_MAX + 1) {
		return EXT4_INVALID_ARGUMENT;
	}
	if (inode->flags & EXT4_INODE_EXTENTS) {
		return ext4_extent_punch(allocation, inode, disk, logical, length);
	}
	return length == 1 ? ext4_indirect_punch(allocation, disk, logical) : EXT4_INVALID_ARGUMENT;
}

static enum ext4_result
ext4_indirect_truncate(struct ext4_allocation *allocation, struct ext4_inode_disk *disk,
    uint32_t first, uint64_t limit, bool *done)
{
	struct ext4_le32 *root = (struct ext4_le32 *)disk->block_data;
	struct ext4_indirect_walk frames[EXT4_INDIRECT_LEVELS];
	struct ext4_indirect_walk *frame;
	struct ext4_le32 *slot;
	void *buffer;
	uint64_t block;
	uint64_t logical;
	uint64_t base = EXT4_DIRECT_BLOCKS;
	uint64_t span = 1;
	uint32_t per_block = allocation->fs->info.block_size / sizeof(*root);
	uint32_t index;
	uint16_t depth;
	uint16_t level;
	bool empty;
	enum ext4_result error;

	for (index = first; index < EXT4_DIRECT_BLOCKS; index++) {
		block = ext4_le32(&root[index]);
		if (block != 0) {
			if (allocation->freed >= limit) {
				return EXT4_OK;
			}
			error = ext4_free_blocks(allocation, block, 1);
			if (error != EXT4_OK) {
				return error;
			}
			ext4_encode32(&root[index], 0);
		}
	}
	for (depth = 1; depth <= EXT4_INDIRECT_LEVELS; depth++) {
		slot = &root[EXT4_DIRECT_BLOCKS + depth - 1];
		block = ext4_le32(slot);
		if (block != 0 && base + span * per_block > first) {
			level = 0;
			frames[0].parent = slot;
			frames[0].block = block;
			frames[0].logical = base;
			frames[0].span = span;
			for (;;) {
				error = ext4_transaction_buffer(
				    allocation->transaction, frames[level].block, &buffer);
				if (error != EXT4_OK) {
					return error;
				}
				frames[level].pointers = buffer;
				frames[level].next = 0;
				for (;;) {
					frame = &frames[level];
					if (frame->next == per_block) {
						empty = true;
						for (index = 0; index < per_block; index++) {
							if (ext4_le32(&frame->pointers[index]) !=
							    0) {
								empty = false;
								break;
							}
						}
						if (empty) {
							if (allocation->freed >= limit) {
								return EXT4_OK;
							}
							error = ext4_free_blocks(
							    allocation, frame->block, 1);
							if (error != EXT4_OK) {
								return error;
							}
							ext4_encode32(frame->parent, 0);
						}
						if (level == 0) {
							break;
						}
						level--;
						continue;
					}
					index = frame->next++;
					slot = &frame->pointers[index];
					block = ext4_le32(slot);
					logical = frame->logical + index * frame->span;
					if (block == 0 || logical + frame->span <= first) {
						continue;
					}
					if (level + 1 < depth) {
						level++;
						frames[level].parent = slot;
						frames[level].block = block;
						frames[level].logical = logical;
						frames[level].span = frame->span / per_block;
						break;
					}
					if (allocation->freed >= limit) {
						return EXT4_OK;
					}
					error = ext4_free_blocks(allocation, block, 1);
					if (error != EXT4_OK) {
						return error;
					}
					ext4_encode32(slot, 0);
				}
				if (level == 0 && frames[0].next == per_block) {
					break;
				}
			}
		}
		base += span * per_block;
		span *= per_block;
	}
	*done = true;
	return EXT4_OK;
}

enum ext4_result
ext4_write_map_trim(struct ext4_allocation *allocation, const struct ext4_inode *inode,
    struct ext4_inode_disk *disk, uint32_t first, uint32_t limit, bool *done)
{
	*done = false;
	if (limit == 0 || allocation->freed != 0) {
		return EXT4_INVALID_ARGUMENT;
	}
	return inode->flags & EXT4_INODE_EXTENTS
	    ? ext4_extent_truncate(allocation, inode, disk, first, limit, done)
	    : ext4_indirect_truncate(allocation, disk, first, limit, done);
}

enum ext4_result
ext4_write_map_truncate(struct ext4_allocation *allocation, const struct ext4_inode *inode,
    struct ext4_inode_disk *disk, uint32_t first)
{
	bool done = false;
	enum ext4_result error;

	error = ext4_write_map_validate(allocation, inode, disk);
	if (error != EXT4_OK) {
		return error;
	}
	return inode->flags & EXT4_INODE_EXTENTS
	    ? ext4_extent_truncate(allocation, inode, disk, first, UINT64_MAX, &done)
	    : ext4_indirect_truncate(allocation, disk, first, UINT64_MAX, &done);
}

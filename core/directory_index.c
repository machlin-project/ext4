/* SPDX-License-Identifier: BSD-3-Clause */
#include "directory_index.h"

uint8_t
ext4_index_max_levels(const struct ext4_fs *fs)
{
	return (fs->info.feature_incompat & EXT4_FEATURE_INCOMPAT_LARGEDIR)
	    ? EXT4_DX_MAX_INDIRECT_LEVELS
	    : EXT4_DX_LEGACY_INDIRECT_LEVELS;
}

bool
ext4_index_contains(const struct ext4_index_range *range, uint32_t hash)
{
	uint64_t upper = range->upper & ~UINT64_C(1);

	return hash >= (range->lower & ~1U) &&
	    (hash < upper || ((range->upper & 1U) && hash == upper));
}

enum ext4_result
ext4_index_read(struct ext4_directory_index *index, uint32_t logical, uint64_t *physical)
{
	struct ext4_allocation *allocation = index->allocation;
	struct ext4_map_run run;
	enum ext4_result error;

	if (logical >= index->blocks) {
		return EXT4_CORRUPT;
	}
	error = ext4_write_map_lookup(allocation, index->inode, index->disk, logical, &run);
	if (error != EXT4_OK) {
		return error;
	}
	if (run.physical == 0 || run.unwritten) {
		return EXT4_CORRUPT;
	}
	*physical = run.physical;
	return ext4_transaction_read(allocation->transaction, run.physical, allocation->scratch);
}

enum ext4_result
ext4_index_decode(struct ext4_fs *fs, const struct ext4_inode *inode, uint32_t logical,
    uint8_t *buffer, struct ext4_index_metadata *result)
{
	const struct ext4_dx_root_prefix_disk *root;
	const struct ext4_dir_header_disk *header;
	const struct ext4_dx_count_disk *counts;
	struct ext4_index_metadata decoded = { 0 };
	uint32_t base = logical == 0 ? sizeof(*root) : sizeof(*header);
	uint32_t tail = fs->metadata_checksum ? sizeof(struct ext4_dx_tail_disk) : 0;
	uint16_t limit;
	bool filetype = (fs->info.feature_incompat & EXT4_FEATURE_INCOMPAT_FILETYPE) != 0;
	enum ext4_result error;

	error = ext4_directory_checksum(fs, inode, logical, buffer);
	if (error != EXT4_OK) {
		return error;
	}
	if (logical == 0) {
		root = (const struct ext4_dx_root_prefix_disk *)buffer;
		decoded.parent = ext4_le32(&root->dotdot.inode);
		if (ext4_le32(&root->dot.inode) != inode->number ||
		    ext4_directory_record_length(fs, &root->dot) !=
			offsetof(struct ext4_dx_root_prefix_disk, dotdot) ||
		    root->dot.name_length != 1 || root->dot_name[0] != '.' || decoded.parent == 0 ||
		    decoded.parent > fs->info.inodes ||
		    (inode->number == EXT4_ROOT_INODE && decoded.parent != EXT4_ROOT_INODE) ||
		    ext4_directory_record_length(fs, &root->dotdot) !=
			fs->info.block_size - offsetof(struct ext4_dx_root_prefix_disk, dotdot) ||
		    root->dotdot.name_length != 2 || root->dotdot_name[0] != '.' ||
		    root->dotdot_name[1] != '.' ||
		    (filetype &&
			((root->dot.type != EXT4_FT_DIRECTORY &&
			     root->dot.type != EXT4_FT_UNKNOWN) ||
			    (root->dotdot.type != EXT4_FT_DIRECTORY &&
				root->dotdot.type != EXT4_FT_UNKNOWN))) ||
		    (!filetype && (root->dot.type != 0 || root->dotdot.type != 0)) ||
		    ext4_le32(&root->reserved) != 0 ||
		    root->info_length !=
			sizeof(*root) - offsetof(struct ext4_dx_root_prefix_disk, reserved)) {
			return EXT4_CORRUPT;
		}
		if (root->flags != 0 || root->hash_version > EXT4_HASH_TEA_UNSIGNED ||
		    root->indirect_levels > ext4_index_max_levels(fs)) {
			return EXT4_UNSUPPORTED;
		}
		decoded.levels = root->indirect_levels;
		decoded.version = root->hash_version;
	} else {
		header = (const struct ext4_dir_header_disk *)buffer;
		if (ext4_le32(&header->inode) != 0 || header->name_length != 0 ||
		    header->type != 0 ||
		    ext4_directory_record_length(fs, header) != fs->info.block_size) {
			return EXT4_CORRUPT;
		}
	}
	counts = (const struct ext4_dx_count_disk *)(buffer + base);
	limit = ext4_le16(&counts->limit);
	decoded.count = ext4_le16(&counts->count);
	if (limit != (fs->info.block_size - base - tail) / sizeof(*counts) || decoded.count == 0 ||
	    decoded.count > limit) {
		return EXT4_CORRUPT;
	}
	*result = decoded;
	return EXT4_OK;
}

static enum ext4_result
ext4_index_header(struct ext4_directory_index *index, uint32_t logical)
{
	const struct ext4_super_disk *super = index->allocation->super;
	struct ext4_index_metadata decoded;
	uint32_t flags =
	    ext4_le32(&super->flags) & (EXT4_SIGNED_DIRECTORY_HASH | EXT4_UNSIGNED_DIRECTORY_HASH);
	unsigned int word;
	enum ext4_result error;

	error = ext4_index_decode(
	    index->allocation->fs, index->inode, logical, index->allocation->scratch, &decoded);
	if (error != EXT4_OK || logical != 0) {
		return error;
	}
	if (flags == 0 && decoded.version <= EXT4_HASH_TEA) {
		return EXT4_UNSUPPORTED;
	}
	if (flags == (EXT4_SIGNED_DIRECTORY_HASH | EXT4_UNSIGNED_DIRECTORY_HASH)) {
		return EXT4_CORRUPT;
	}
	index->parent_number = decoded.parent;
	index->levels = decoded.levels;
	index->version = decoded.version;
	if (index->version <= EXT4_HASH_TEA && flags == EXT4_UNSIGNED_DIRECTORY_HASH) {
		index->version += EXT4_HASH_LEGACY_UNSIGNED;
	}
	for (word = 0; word < 4; word++) {
		index->seed[word] = ext4_le32(&super->hash_seed[word]);
	}
	return EXT4_OK;
}

static enum ext4_result
ext4_index_node(struct ext4_directory_index *index, uint32_t logical)
{
	struct ext4_allocation *allocation = index->allocation;
	struct ext4_index_range *range = &index->ranges[logical];
	struct ext4_index_range *child;
	struct ext4_dx_entry_disk *entries;
	struct ext4_dx_count_disk *counts;
	uint64_t physical;
	uint64_t upper;
	uint32_t base = logical == 0 ? sizeof(struct ext4_dx_root_prefix_disk)
				     : sizeof(struct ext4_dir_header_disk);
	uint32_t lower;
	uint32_t previous = range->lower;
	uint32_t number;
	uint16_t count;
	uint16_t entry;
	enum ext4_result error;

	error = ext4_index_read(index, logical, &physical);
	if (error == EXT4_OK) {
		error = ext4_index_header(index, logical);
	}
	if (error != EXT4_OK) {
		return error;
	}
	counts = (struct ext4_dx_count_disk *)(allocation->scratch + base);
	entries = (struct ext4_dx_entry_disk *)(allocation->scratch + base);
	count = ext4_le16(&counts->count);
	range->count = count;
	for (entry = 0; entry < count; entry++) {
		lower = entry == 0 ? range->lower : ext4_le32(&entries[entry].hash);
		upper = entry + 1U == count ? range->upper : ext4_le32(&entries[entry + 1U].hash);
		number = ext4_le32(&entries[entry].block);
		if (lower < previous || lower >= EXT4_HASH_EOF ||
		    (entry != 0 && lower == previous && !(lower & 1U)) || upper > range->upper ||
		    lower > upper || (lower == upper && !(lower & 1U)) || number == 0 ||
		    number >= index->blocks || index->ranges[number].kind != EXT4_INDEX_UNKNOWN) {
			return EXT4_CORRUPT;
		}
		child = &index->ranges[number];
		child->lower = lower;
		child->upper = upper;
		child->parent = logical;
		child->entry = entry;
		child->level = (uint8_t)(range->level + 1U);
		child->kind = range->level == index->levels ? EXT4_INDEX_LEAF : EXT4_INDEX_NODE;
		previous = lower;
	}
	return EXT4_OK;
}

void
ext4_index_close(struct ext4_directory_index *index)
{
	struct ext4_fs *fs = index->allocation->fs;

	if (index->ranges != NULL) {
		fs->environment.release(fs->environment.context, index->ranges,
		    (size_t)index->blocks * sizeof(*index->ranges));
		index->ranges = NULL;
	}
}

/* The mount's record of an index that is classified as its map record stands. */
static struct ext4_validated_map *
ext4_index_remembered(
    struct ext4_fs *fs, const struct ext4_inode *inode, const struct ext4_map_record *record)
{
	struct ext4_validated_map *entry;
	uint32_t index;

	for (index = 0; index < EXT4_VALIDATED_INDEXES; index++) {
		entry = &fs->validated_indexes[index];
		if (entry->number == inode->number && entry->generation == inode->generation) {
			return ext4_equal(&entry->record, record, sizeof(*record)) ? entry : NULL;
		}
	}
	return NULL;
}

void
ext4_index_remember(struct ext4_allocation *allocation, const struct ext4_inode *inode,
    const struct ext4_inode_disk *disk)
{
	struct ext4_fs *fs = allocation->fs;
	struct ext4_validated_map *entry = NULL;
	uint32_t index;

	if (!fs->validated_maps_enabled) {
		return;
	}
	for (index = 0; index < EXT4_VALIDATED_INDEXES && entry == NULL; index++) {
		if (fs->validated_indexes[index].number == inode->number) {
			entry = &fs->validated_indexes[index];
		}
	}
	if (entry == NULL) {
		entry = &fs->validated_indexes[fs->validated_index_next++ % EXT4_VALIDATED_INDEXES];
	}
	entry->number = inode->number;
	entry->generation = inode->generation;
	ext4_map_record_read(disk, &entry->record);
}

static enum ext4_result
ext4_index_classify(struct ext4_directory_index *index)
{
	struct ext4_fs *fs = index->allocation->fs;
	uint32_t logical;
	unsigned int level;
	enum ext4_result error;

	index->ranges = fs->environment.allocate(
	    fs->environment.context, (size_t)index->blocks * sizeof(*index->ranges));
	if (index->ranges == NULL) {
		return EXT4_NO_MEMORY;
	}
	ext4_zero(index->ranges, (size_t)index->blocks * sizeof(*index->ranges));
	index->ranges[0].kind = EXT4_INDEX_ROOT;
	index->ranges[0].upper = EXT4_DX_HASH_END;
	for (level = 0; level <= index->levels; level++) {
		for (logical = 0; logical < index->blocks; logical++) {
			if (index->ranges[logical].level != level ||
			    (index->ranges[logical].kind != EXT4_INDEX_ROOT &&
				index->ranges[logical].kind != EXT4_INDEX_NODE)) {
				continue;
			}
			error = ext4_index_node(index, logical);
			if (error != EXT4_OK) {
				goto fail;
			}
		}
	}
	for (logical = 0; logical < index->blocks; logical++) {
		if (index->ranges[logical].kind == EXT4_INDEX_UNKNOWN) {
			error = EXT4_CORRUPT;
			goto fail;
		}
	}
	return EXT4_OK;
fail:
	ext4_index_close(index);
	return error;
}

enum ext4_result
ext4_index_open(struct ext4_allocation *allocation, const struct ext4_inode *inode,
    struct ext4_inode_disk *disk, struct ext4_directory_index *index, bool classify)
{
	struct ext4_fs *fs = allocation->fs;
	struct ext4_map_record record;
	uint64_t blocks = inode->size / fs->info.block_size;
	uint64_t physical;
	enum ext4_result error;

	ext4_zero(index, sizeof(*index));
	index->allocation = allocation;
	index->inode = inode;
	index->disk = disk;
	if ((inode->mode & EXT4_MODE_TYPE) != EXT4_MODE_DIRECTORY ||
	    !(inode->flags & EXT4_INODE_INDEX) ||
	    !(fs->info.feature_compat & EXT4_FEATURE_COMPAT_DIR_INDEX) || blocks < 2 ||
	    inode->size % fs->info.block_size != 0) {
		return EXT4_CORRUPT;
	}
	if (blocks > EXT4_DIRECTORY_MAX_BLOCKS) {
		return EXT4_UNSUPPORTED;
	}
	index->blocks = (uint32_t)blocks;
	error = ext4_write_map_validate(allocation, inode, disk);
	if (error == EXT4_OK) {
		error = ext4_allocation_super(allocation);
	}
	if (error != EXT4_OK) {
		return error;
	}
	/* A remembered index needs only its root; probes check each node they read. */
	ext4_map_record_read(disk, &record);
	if (!classify && ext4_index_remembered(fs, inode, &record) != NULL) {
		error = ext4_index_read(index, 0, &physical);
		return error == EXT4_OK ? ext4_index_header(index, 0) : error;
	}
	error = ext4_index_classify(index);
	if (error == EXT4_OK) {
		ext4_index_remember(allocation, inode, disk);
	}
	return error;
}

/* Read node frames[level] and check its entries against its range, the path above
 * it and, when this operation classified the tree, the classification. Then follow
 * the last entry at or below hash when search is set, or the entry at the frame's
 * position. The followed child becomes the next frame, or the leaf below the last
 * level. */
static enum ext4_result
ext4_index_follow(struct ext4_directory_index *index, uint8_t level, bool search, uint32_t hash)
{
	struct ext4_index_frame *frame = &index->frames[level];
	struct ext4_index_range child = { 0 };
	const struct ext4_dx_entry_disk *entries;
	const struct ext4_dx_count_disk *counts;
	const struct ext4_index_range *classified;
	uint64_t physical;
	uint64_t upper;
	uint32_t base = frame->logical == 0 ? sizeof(struct ext4_dx_root_prefix_disk)
					    : sizeof(struct ext4_dir_header_disk);
	uint32_t previous = frame->range.lower;
	uint32_t lower;
	uint32_t number;
	uint16_t first = 1;
	uint16_t end;
	uint16_t middle;
	uint16_t count;
	uint16_t entry;
	uint8_t levels = index->levels;
	uint8_t version = index->version;
	uint8_t above;
	enum ext4_result error;

	error = ext4_index_read(index, frame->logical, &physical);
	if (error == EXT4_OK) {
		error = ext4_index_header(index, frame->logical);
	}
	if (error != EXT4_OK) {
		return error;
	}
	if (index->levels != levels || index->version != version) {
		return EXT4_CORRUPT;
	}
	counts = (const struct ext4_dx_count_disk *)(index->allocation->scratch + base);
	entries = (const struct ext4_dx_entry_disk *)counts;
	count = ext4_le16(&counts->count);
	for (entry = 0; entry < count; entry++) {
		lower = entry == 0 ? frame->range.lower : ext4_le32(&entries[entry].hash);
		upper =
		    entry + 1U == count ? frame->range.upper : ext4_le32(&entries[entry + 1U].hash);
		number = ext4_le32(&entries[entry].block);
		if (lower < previous || lower >= EXT4_HASH_EOF ||
		    (entry != 0 && lower == previous && !(lower & 1U)) ||
		    upper > frame->range.upper || lower > upper ||
		    (lower == upper && !(lower & 1U)) || number == 0 || number >= index->blocks) {
			return EXT4_CORRUPT;
		}
		for (above = 0; above <= level; above++) {
			if (index->frames[above].logical == number) {
				return EXT4_CORRUPT;
			}
		}
		classified = index->ranges == NULL ? NULL : &index->ranges[number];
		if (classified != NULL &&
		    (classified->parent != frame->logical || classified->entry != entry ||
			classified->lower != lower || classified->upper != upper ||
			classified->kind !=
			    (level == index->levels ? EXT4_INDEX_LEAF : EXT4_INDEX_NODE))) {
			return EXT4_CORRUPT;
		}
		previous = lower;
	}
	if (search) {
		/* The first entry inherits its lower bound. A continuation's odd boundary
		 * sorts after the even name hash, so the search stops before it. */
		end = count;
		while (first < end) {
			middle = (uint16_t)(first + (end - first) / 2U);
			if (ext4_le32(&entries[middle].hash) <= hash) {
				first = (uint16_t)(middle + 1U);
			} else {
				end = middle;
			}
		}
		frame->position = (uint16_t)(first - 1U);
	} else if (frame->position >= count) {
		return EXT4_CORRUPT;
	}
	frame->count = count;
	frame->checksum = ext4_crc32c(
	    UINT32_MAX, index->allocation->scratch + base, (size_t)count * sizeof(*entries));
	entry = frame->position;
	child.lower = entry == 0 ? frame->range.lower : ext4_le32(&entries[entry].hash);
	child.upper =
	    entry + 1U == count ? frame->range.upper : ext4_le32(&entries[entry + 1U].hash);
	child.level = (uint8_t)(level + 1U);
	number = ext4_le32(&entries[entry].block);
	if (level == index->levels) {
		child.kind = EXT4_INDEX_LEAF;
		index->leaf = child;
		index->leaf_logical = number;
		return EXT4_OK;
	}
	child.kind = EXT4_INDEX_NODE;
	index->frames[level + 1U].range = child;
	index->frames[level + 1U].logical = number;
	index->frames[level + 1U].position = 0;
	return EXT4_OK;
}

enum ext4_result
ext4_index_probe(struct ext4_directory_index *index, uint32_t hash)
{
	uint8_t level;
	enum ext4_result error;

	ext4_zero(index->frames, sizeof(index->frames));
	index->frames[0].range.kind = EXT4_INDEX_ROOT;
	index->frames[0].range.upper = EXT4_DX_HASH_END;
	for (level = 0; level <= index->levels; level++) {
		error = ext4_index_follow(index, level, true, hash);
		if (error != EXT4_OK) {
			return error;
		}
	}
	return EXT4_OK;
}

enum ext4_result
ext4_index_next(struct ext4_directory_index *index, uint32_t hash, bool *more)
{
	uint64_t boundary;
	uint8_t level = (uint8_t)(index->levels + 1U);
	enum ext4_result error;

	*more = false;
	while (level > 0 &&
	    index->frames[level - 1U].position + 1U >= index->frames[level - 1U].count) {
		level--;
	}
	if (level == 0) {
		return EXT4_OK;
	}
	level--;
	/* The followed child's upper bound is the next entry's hash. */
	boundary =
	    level == index->levels ? index->leaf.upper : index->frames[level + 1U].range.upper;
	if (!(boundary & 1U) || (boundary & ~UINT64_C(1)) != hash) {
		return EXT4_OK;
	}
	index->frames[level].position++;
	for (; level <= index->levels; level++) {
		error = ext4_index_follow(index, level, false, 0);
		if (error != EXT4_OK) {
			return error;
		}
	}
	*more = true;
	return EXT4_OK;
}

void
ext4_index_checksum_set(
    struct ext4_fs *fs, const struct ext4_inode *inode, uint32_t logical, uint8_t *buffer)
{
	uint32_t base = logical == 0 ? sizeof(struct ext4_dx_root_prefix_disk)
				     : sizeof(struct ext4_dir_header_disk);
	struct ext4_dx_count_disk *counts = (struct ext4_dx_count_disk *)(buffer + base);
	struct ext4_dx_tail_disk *tail;
	uint32_t checksum;

	if (!fs->metadata_checksum) {
		return;
	}
	tail = (struct ext4_dx_tail_disk *)(buffer + base +
	    (size_t)ext4_le16(&counts->limit) * sizeof(*counts));
	ext4_encode32(&tail->checksum, 0);
	checksum = ext4_crc32c(ext4_inode_seed(fs, inode), buffer,
	    base + (size_t)ext4_le16(&counts->count) * sizeof(*counts));
	checksum = ext4_crc32c(checksum, tail, sizeof(*tail));
	ext4_encode32(&tail->checksum, checksum);
}

enum ext4_result
ext4_index_append(
    struct ext4_directory_index *index, uint32_t *next, uint32_t *logical, uint8_t **buffer)
{
	struct ext4_allocation *allocation = index->allocation;
	void *snapshot = NULL;
	uint64_t physical;
	bool zero;
	enum ext4_result error;

	if (*next >= EXT4_DIRECTORY_MAX_BLOCKS) {
		return EXT4_UNSUPPORTED;
	}
	error =
	    ext4_write_map_allocate(allocation, index->inode, index->disk, *next, &physical, &zero);
	if (error != EXT4_OK) {
		return error;
	}
	if (!zero) {
		return EXT4_CORRUPT;
	}
	error = ext4_transaction_buffer(allocation->transaction, physical, &snapshot);
	if (error != EXT4_OK) {
		return error;
	}
	ext4_zero(snapshot, allocation->fs->info.block_size);
	*logical = (*next)++;
	*buffer = snapshot;
	return EXT4_OK;
}

/* Enroll node frames[level] in the transaction, requiring the count and entries
 * its probe checked. */
static enum ext4_result
ext4_index_buffer(struct ext4_directory_index *index, uint8_t level, uint8_t **buffer)
{
	struct ext4_allocation *allocation = index->allocation;
	struct ext4_index_frame *frame = &index->frames[level];
	const struct ext4_index_range *child =
	    level == index->levels ? &index->leaf : &index->frames[level + 1U].range;
	struct ext4_map_run run;
	struct ext4_dx_entry_disk *entries;
	struct ext4_dx_count_disk *counts;
	uint32_t logical = frame->logical;
	uint32_t base = logical == 0 ? sizeof(struct ext4_dx_root_prefix_disk)
				     : sizeof(struct ext4_dir_header_disk);
	uint32_t parent = index->parent_number;
	uint32_t number =
	    level == index->levels ? index->leaf_logical : index->frames[level + 1U].logical;
	uint8_t levels = index->levels;
	uint8_t version = index->version;
	void *snapshot = NULL;
	enum ext4_result error;

	error = ext4_write_map_lookup(allocation, index->inode, index->disk, logical, &run);
	if (error != EXT4_OK) {
		return error;
	}
	if (run.physical == 0 || run.unwritten) {
		return EXT4_CORRUPT;
	}
	error = ext4_transaction_buffer(allocation->transaction, run.physical, &snapshot);
	if (error != EXT4_OK) {
		return error;
	}
	/* Snapshot enrollment may perform a fresh device read. Validate its own
	 * bounds and require the links the probe followed. */
	ext4_copy(allocation->scratch, snapshot, allocation->fs->info.block_size);
	error = ext4_index_header(index, logical);
	if (error != EXT4_OK) {
		return error;
	}
	if (index->levels != levels || index->version != version ||
	    index->parent_number != parent) {
		return EXT4_CORRUPT;
	}
	entries = (struct ext4_dx_entry_disk *)(allocation->scratch + base);
	counts = (struct ext4_dx_count_disk *)entries;
	if (ext4_le16(&counts->count) != frame->count ||
	    ext4_crc32c(UINT32_MAX, entries, (size_t)frame->count * sizeof(*entries)) !=
		frame->checksum ||
	    ext4_le32(&entries[frame->position].block) != number ||
	    (frame->position != 0 && ext4_le32(&entries[frame->position].hash) != child->lower)) {
		return EXT4_CORRUPT;
	}
	*buffer = snapshot;
	return EXT4_OK;
}

static void
ext4_index_entry_insert(struct ext4_dx_entry_disk *entries, uint16_t count, uint16_t position,
    uint32_t hash, uint32_t block)
{
	uint16_t entry;

	for (entry = count; entry > position; entry--) {
		entries[entry] = entries[entry - 1];
	}
	ext4_encode32(&entries[position].hash, hash);
	ext4_encode32(&entries[position].block, block);
}

static void
ext4_index_node_initialize(struct ext4_fs *fs, uint8_t *buffer, uint16_t count)
{
	struct ext4_dir_header_disk *header = (struct ext4_dir_header_disk *)buffer;
	struct ext4_dx_count_disk *counts = (struct ext4_dx_count_disk *)(header + 1);
	uint32_t tail = fs->metadata_checksum ? sizeof(struct ext4_dx_tail_disk) : 0;

	ext4_encode16(&header->record_length,
	    fs->info.block_size == EXT4_MAX_BLOCK_SIZE ? UINT16_MAX
						       : (uint16_t)fs->info.block_size);
	ext4_encode16(&counts->limit,
	    (uint16_t)((fs->info.block_size - sizeof(*header) - tail) / sizeof(*counts)));
	ext4_encode16(&counts->count, count);
}

enum ext4_result
ext4_index_add(struct ext4_directory_index *index, uint32_t hash, uint32_t block, uint32_t *next)
{
	struct ext4_allocation *allocation = index->allocation;
	struct ext4_fs *fs = allocation->fs;
	const struct ext4_index_range *range;
	struct ext4_dx_count_disk *counts;
	struct ext4_dx_entry_disk *entries;
	struct ext4_dx_entry_disk *right_entries;
	struct ext4_dx_entry_disk *original;
	struct ext4_dx_entry_disk value;
	struct ext4_dx_root_prefix_disk *root;
	uint8_t *buffer = NULL;
	uint8_t *right_buffer = NULL;
	uint32_t child = index->leaf_logical;
	uint32_t parent;
	uint32_t base;
	uint32_t right;
	uint32_t separator;
	uint16_t position;
	uint16_t count;
	uint16_t total;
	uint16_t cut;
	uint16_t entry;
	uint8_t level = (uint8_t)(index->levels + 1U);
	enum ext4_result error;

	/* Propagate a split up the checked probed path. New nodes stay private to the
	 * transaction; the directory owner publishes them only together with its final
	 * inode size and allocation accounting. */
	while (level-- > 0) {
		range = level == index->levels ? &index->leaf : &index->frames[level + 1U].range;
		parent = index->frames[level].logical;
		base = parent == 0 ? sizeof(*root) : sizeof(struct ext4_dir_header_disk);
		position = (uint16_t)(index->frames[level].position + 1U);
		error = ext4_index_buffer(index, level, &buffer);
		if (error != EXT4_OK) {
			return error;
		}
		entries = (struct ext4_dx_entry_disk *)(buffer + base);
		counts = (struct ext4_dx_count_disk *)entries;
		count = ext4_le16(&counts->count);
		if (position > count || ext4_le32(&entries[position - 1].block) != child ||
		    !ext4_index_contains(range, hash & ~1U)) {
			return EXT4_CORRUPT;
		}
		if (count < ext4_le16(&counts->limit)) {
			ext4_index_entry_insert(entries, count, position, hash, block);
			ext4_encode16(&counts->count, count + 1);
			ext4_index_checksum_set(fs, index->inode, parent, buffer);
			return EXT4_OK;
		}
		if (parent == 0) {
			if (index->levels == ext4_index_max_levels(fs)) {
				return EXT4_UNSUPPORTED;
			}
			/* A full root fits, including the new entry, in one node:
			 * the node has a smaller prefix than the root. */
			error = ext4_index_append(index, next, &right, &right_buffer);
			if (error != EXT4_OK) {
				return error;
			}
			right_entries = (struct ext4_dx_entry_disk *)(right_buffer +
			    sizeof(struct ext4_dir_header_disk));
			ext4_copy(right_entries, entries, (size_t)count * sizeof(*entries));
			ext4_index_entry_insert(right_entries, count, position, hash, block);
			ext4_index_node_initialize(fs, right_buffer, count + 1);
			ext4_zero(entries, fs->info.block_size - base);
			ext4_encode16(&counts->limit,
			    (uint16_t)((fs->info.block_size - base -
					   (fs->metadata_checksum ? sizeof(struct ext4_dx_tail_disk)
								  : 0)) /
				sizeof(*entries)));
			ext4_encode16(&counts->count, 1);
			ext4_encode32(&entries[0].block, right);
			root = (struct ext4_dx_root_prefix_disk *)buffer;
			root->indirect_levels = (uint8_t)(index->levels + 1U);
			ext4_index_checksum_set(fs, index->inode, right, right_buffer);
			ext4_index_checksum_set(fs, index->inode, 0, buffer);
			return EXT4_OK;
		}
		error = ext4_index_append(index, next, &right, &right_buffer);
		if (error != EXT4_OK) {
			return error;
		}
		/* All I/O is finished before scratch holds the original entries. Both
		 * output nodes can now be packed without overlapping their input. */
		original = (struct ext4_dx_entry_disk *)allocation->scratch;
		ext4_copy(original, entries, (size_t)count * sizeof(*entries));
		right_entries = (struct ext4_dx_entry_disk *)(right_buffer +
		    sizeof(struct ext4_dir_header_disk));
		total = (uint16_t)(count + 1U);
		cut = total / 2;
		separator = 0;
		ext4_zero(entries, fs->info.block_size - base);
		for (entry = 0; entry < total; entry++) {
			if (entry == position) {
				ext4_encode32(&value.hash, hash);
				ext4_encode32(&value.block, block);
			} else {
				value = original[entry < position ? entry : entry - 1];
			}
			if (entry < cut) {
				entries[entry] = value;
			} else {
				right_entries[entry - cut] = value;
				if (entry == cut) {
					separator = ext4_le32(&value.hash);
				}
			}
		}
		ext4_index_node_initialize(fs, buffer, cut);
		ext4_index_node_initialize(fs, right_buffer, total - cut);
		ext4_index_checksum_set(fs, index->inode, parent, buffer);
		ext4_index_checksum_set(fs, index->inode, right, right_buffer);
		child = parent;
		hash = separator;
		block = right;
	}
	return EXT4_CORRUPT;
}

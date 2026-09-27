/* SPDX-License-Identifier: BSD-3-Clause */
#include "directory_index.h"

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
		    root->indirect_levels > EXT4_DX_MAX_INDIRECT_LEVELS) {
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
	if (flags == 0) {
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

enum ext4_result
ext4_index_open(struct ext4_allocation *allocation, const struct ext4_inode *inode,
    struct ext4_inode_disk *disk, struct ext4_directory_index *index)
{
	struct ext4_fs *fs = allocation->fs;
	uint64_t blocks = inode->size / fs->info.block_size;
	uint32_t logical;
	unsigned int level;
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

static enum ext4_result
ext4_index_buffer(struct ext4_directory_index *index, uint32_t logical, uint8_t **buffer)
{
	struct ext4_allocation *allocation = index->allocation;
	struct ext4_map_run run;
	struct ext4_dx_entry_disk *entries;
	struct ext4_dx_count_disk *counts;
	struct ext4_index_range *child;
	uint32_t base = logical == 0 ? sizeof(struct ext4_dx_root_prefix_disk)
				     : sizeof(struct ext4_dir_header_disk);
	uint32_t parent = index->parent_number;
	uint32_t number;
	uint16_t count;
	uint16_t entry;
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
	 * bounds and require the links observed by the complete graph walk. */
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
	count = ext4_le16(&counts->count);
	if (count != index->ranges[logical].count) {
		return EXT4_CORRUPT;
	}
	for (entry = 0; entry < count; entry++) {
		number = ext4_le32(&entries[entry].block);
		if (number == 0 || number >= index->blocks) {
			return EXT4_CORRUPT;
		}
		child = &index->ranges[number];
		if (child->parent != logical || child->entry != entry ||
		    (entry != 0 && child->lower != ext4_le32(&entries[entry].hash))) {
			return EXT4_CORRUPT;
		}
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
ext4_index_add(struct ext4_directory_index *index, uint32_t leaf, uint32_t hash, uint32_t block,
    uint32_t *next)
{
	struct ext4_allocation *allocation = index->allocation;
	struct ext4_fs *fs = allocation->fs;
	struct ext4_index_range *range = &index->ranges[leaf];
	struct ext4_dx_count_disk *counts;
	struct ext4_dx_count_disk *root_counts;
	struct ext4_dx_entry_disk *entries;
	struct ext4_dx_entry_disk *root_entries;
	struct ext4_dx_entry_disk *right_entries;
	struct ext4_dx_entry_disk *original;
	struct ext4_dx_entry_disk value;
	struct ext4_dx_root_prefix_disk *root;
	uint8_t *buffer = NULL;
	uint8_t *root_buffer = NULL;
	uint8_t *right_buffer = NULL;
	uint32_t parent = range->parent;
	uint32_t base = parent == 0 ? sizeof(*root) : sizeof(struct ext4_dir_header_disk);
	uint32_t right;
	uint32_t separator;
	uint16_t position = (uint16_t)(range->entry + 1U);
	uint16_t count;
	uint16_t total;
	uint16_t cut;
	uint16_t entry;
	uint16_t root_count;
	uint16_t root_position;
	enum ext4_result error;

	error = ext4_index_buffer(index, parent, &buffer);
	if (error != EXT4_OK) {
		return error;
	}
	entries = (struct ext4_dx_entry_disk *)(buffer + base);
	counts = (struct ext4_dx_count_disk *)entries;
	count = ext4_le16(&counts->count);
	if (position > count || ext4_le32(&entries[position - 1].block) != leaf ||
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
		/* A full shallow root fits, including the new entry, in one node:
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
				   (fs->metadata_checksum ? sizeof(struct ext4_dx_tail_disk) : 0)) /
			sizeof(*entries)));
		ext4_encode16(&counts->count, 1);
		ext4_encode32(&entries[0].block, right);
		root = (struct ext4_dx_root_prefix_disk *)buffer;
		root->indirect_levels = 1;
		ext4_index_checksum_set(fs, index->inode, right, right_buffer);
		ext4_index_checksum_set(fs, index->inode, 0, buffer);
		return EXT4_OK;
	}
	error = ext4_index_buffer(index, 0, &root_buffer);
	if (error != EXT4_OK) {
		return error;
	}
	root_entries = (struct ext4_dx_entry_disk *)(root_buffer + sizeof(*root));
	root_counts = (struct ext4_dx_count_disk *)root_entries;
	root_count = ext4_le16(&root_counts->count);
	root_position = (uint16_t)(index->ranges[parent].entry + 1U);
	if (root_count == ext4_le16(&root_counts->limit)) {
		return EXT4_UNSUPPORTED;
	}
	if (root_position > root_count ||
	    ext4_le32(&root_entries[root_position - 1].block) != parent) {
		return EXT4_CORRUPT;
	}
	error = ext4_index_append(index, next, &right, &right_buffer);
	if (error != EXT4_OK) {
		return error;
	}
	/* All I/O is finished before scratch holds the original entries. Both
	 * output nodes can now be packed without overlapping their input. */
	original = (struct ext4_dx_entry_disk *)allocation->scratch;
	ext4_copy(original, entries, (size_t)count * sizeof(*entries));
	right_entries =
	    (struct ext4_dx_entry_disk *)(right_buffer + sizeof(struct ext4_dir_header_disk));
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
	ext4_index_entry_insert(root_entries, root_count, root_position, separator, right);
	ext4_encode16(&root_counts->count, root_count + 1);
	ext4_index_checksum_set(fs, index->inode, parent, buffer);
	ext4_index_checksum_set(fs, index->inode, right, right_buffer);
	ext4_index_checksum_set(fs, index->inode, 0, root_buffer);
	return EXT4_OK;
}

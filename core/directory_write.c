/* SPDX-License-Identifier: BSD-3-Clause */
#include "directory_write.h"
#include "directory_index.h"
#include "inline.h"
#include "unicode.h"

struct ext4_directory_sort_entry {
	uint32_t hash;
	uint32_t offset;
	uint32_t used;
};

static uint32_t
ext4_directory_minimum(size_t length)
{
	return ((uint32_t)sizeof(struct ext4_dir_header_disk) + (uint32_t)length +
		   EXT4_DIRECTORY_ALIGNMENT - 1U) &
	    ~(EXT4_DIRECTORY_ALIGNMENT - 1U);
}

static uint32_t
ext4_directory_usable(struct ext4_fs *fs)
{
	return fs->info.block_size -
	    (fs->metadata_checksum ? sizeof(struct ext4_dir_tail_disk) : 0);
}

static void
ext4_directory_length(struct ext4_dir_header_disk *entry, uint32_t length)
{
	ext4_encode16(
	    &entry->record_length, length == EXT4_MAX_BLOCK_SIZE ? UINT16_MAX : (uint16_t)length);
}

static void
ext4_directory_entry(struct ext4_fs *fs, uint8_t *buffer, uint32_t length, uint32_t number,
    enum ext4_file_type type, const uint8_t *name, size_t name_length)
{
	struct ext4_dir_header_disk *entry = (struct ext4_dir_header_disk *)buffer;

	ext4_zero(buffer, length);
	ext4_encode32(&entry->inode, number);
	ext4_directory_length(entry, length);
	entry->name_length = (uint8_t)name_length;
	entry->type =
	    (fs->info.feature_incompat & EXT4_FEATURE_INCOMPAT_FILETYPE) ? (uint8_t)type : 0;
	ext4_copy(buffer + sizeof(*entry), name, name_length);
}

static void
ext4_directory_checksum_set(struct ext4_fs *fs, const struct ext4_inode *inode, uint8_t *buffer)
{
	struct ext4_dir_tail_disk *tail;
	uint32_t usable = ext4_directory_usable(fs);

	if (!fs->metadata_checksum) {
		return;
	}
	tail = (struct ext4_dir_tail_disk *)(buffer + usable);
	ext4_zero(tail, sizeof(*tail));
	ext4_encode16(&tail->record_length, sizeof(*tail));
	tail->type = EXT4_DIRECTORY_TAIL_TYPE;
	ext4_encode32(&tail->checksum, ext4_crc32c(ext4_inode_seed(fs, inode), buffer, usable));
}

/* The next block a scan visits: every block in turn, or each further leaf a probed
 * index confines the requested hash to. UINT32_MAX ends the scan; error reports why
 * it ended early. */
static uint32_t
ext4_directory_scan_next(struct ext4_directory_index *tree, bool probed, uint32_t hash,
    uint32_t logical, uint64_t blocks, enum ext4_result *error)
{
	bool more = false;

	if (!probed) {
		return logical + 1U < blocks ? logical + 1U : UINT32_MAX;
	}
	*error = ext4_index_next(tree, hash, &more);
	return *error == EXT4_OK && more ? tree->leaf_logical : UINT32_MAX;
}

static enum ext4_result
ext4_directory_scan_blocks(struct ext4_allocation *allocation, const struct ext4_inode *parent,
    struct ext4_inode_disk *disk, const uint8_t *name, size_t name_length,
    enum ext4_directory_action action, uint32_t expected_parent, struct ext4_directory_slot *slot,
    struct ext4_directory_index *tree, struct ext4_directory_name *key)
{
	struct ext4_fs *fs = allocation->fs;
	struct ext4_map_run run;
	struct ext4_dir_header_disk *entry;
	struct ext4_directory_slot repack;
	struct ext4_name_hash requested = { 0, 0 };
	struct ext4_name_hash hash;
	const struct ext4_index_range *range = NULL;
	uint8_t *buffer = allocation->scratch;
	uint8_t *entry_name;
	uint64_t blocks = parent->size / fs->info.block_size;
	uint32_t usable = ext4_directory_usable(fs);
	uint32_t required = ext4_directory_minimum(name_length);
	uint32_t logical;
	uint32_t offset;
	uint32_t length;
	uint32_t used;
	uint32_t number;
	uint32_t previous;
	uint32_t occupied;
	uint32_t best = UINT32_MAX;
	uint32_t inline_used = 0;
	uint32_t inline_tail = 0;
	uint32_t inline_hash = 0;
	uint16_t names;
	size_t index;
	bool exists = false;
	bool populated = false;
	bool dot;
	bool dotdot;
	bool eligible;
	bool inline_data = (parent->flags & EXT4_INODE_INLINE_DATA) != 0;
	bool dots = name_length != 0 && name_length <= 2 && name[0] == '.' &&
	    (name_length == 1 || name[1] == '.');
	bool probed = tree != NULL && action != EXT4_DIRECTORY_EMPTY && !dots;
	enum ext4_result error = EXT4_OK;

	if (inline_data) {
		error = ext4_inline_directory(
		    fs, parent, disk, buffer, false, &inline_used, &inline_tail, &inline_hash);
		if (error != EXT4_OK) {
			return error;
		}
		blocks = 1;
	}
	/* As in Linux, "." and ".." are found only in an index's root block, and any
	 * other name's hash confines it to the leaves one probed path reaches. */
	if (tree != NULL && dots && action != EXT4_DIRECTORY_EMPTY) {
		blocks = 1;
	}
	if (probed) {
		error = ext4_directory_name_hash(
		    key, tree->version, tree->seed, name, name_length, &requested);
		if (error == EXT4_OK) {
			error = ext4_index_probe(tree, requested.major);
		}
		if (error != EXT4_OK) {
			return error;
		}
	}
	ext4_zero(slot, sizeof(*slot));
	ext4_zero(&repack, sizeof(repack));
	slot->logical = UINT32_MAX;
	if (inline_data) {
		slot->inline_disk = disk;
		slot->inline_tail = inline_tail;
		slot->inline_hash = inline_hash;
	}
	for (logical = probed ? tree->leaf_logical : 0; logical != UINT32_MAX;
	    logical =
		ext4_directory_scan_next(tree, probed, requested.major, logical, blocks, &error)) {
		range = probed				   ? &tree->leaf
		    : tree != NULL && tree->ranges != NULL ? &tree->ranges[logical]
							   : NULL;
		if (range != NULL && range->kind == EXT4_INDEX_NODE) {
			continue;
		}
		usable = inline_data		   ? inline_used
		    : tree != NULL && logical == 0 ? fs->info.block_size
						   : ext4_directory_usable(fs);
		eligible =
		    tree == NULL || (logical != 0 && ext4_index_contains(range, requested.major));
		/* A validated index confines every name to leaves containing its hash. */
		if (tree != NULL && logical != 0 && !eligible && action != EXT4_DIRECTORY_EMPTY) {
			continue;
		}
		occupied = 0;
		if (inline_data) {
			run.physical = 0;
		} else {
			error = ext4_write_map_lookup(allocation, parent, disk, logical, &run);
			if (error != EXT4_OK) {
				return error;
			}
			if (run.physical == 0 || run.unwritten) {
				return EXT4_CORRUPT;
			}
			error =
			    ext4_transaction_read(allocation->transaction, run.physical, buffer);
			if (error == EXT4_OK) {
				error = ext4_directory_checksum(fs, parent, logical, buffer);
			}
			if (error != EXT4_OK) {
				return error;
			}
		}
		previous = UINT32_MAX;
		for (offset = 0; offset < usable; previous = offset, offset += length) {
			if (inline_data &&
			    (offset == EXT4_INLINE_DOTS_SIZE || offset == EXT4_INLINE_FIRST_END)) {
				previous = UINT32_MAX;
			}
			if (usable - offset < sizeof(*entry)) {
				return EXT4_CORRUPT;
			}
			entry = (struct ext4_dir_header_disk *)(buffer + offset);
			length = ext4_directory_record_length(fs, entry);
			number = ext4_le32(&entry->inode);
			names = entry->name_length;
			if (!(fs->info.feature_incompat & EXT4_FEATURE_INCOMPAT_FILETYPE)) {
				names |= (uint16_t)((uint16_t)entry->type << 8);
			}
			if ((length & 3U) || length < ext4_directory_minimum(names) ||
			    length > usable - offset || names > EXT4_NAME_MAX ||
			    number > fs->info.inodes) {
				return EXT4_CORRUPT;
			}
			entry_name = buffer + offset + sizeof(*entry);
			dot = names == 1 && entry_name[0] == '.';
			dotdot = names == 2 && entry_name[0] == '.' && entry_name[1] == '.';
			if (logical == 0 && offset == 0) {
				if (!dot || number != parent->number ||
				    length != ext4_directory_minimum(1)) {
					return EXT4_CORRUPT;
				}
			} else if (logical == 0 && offset == ext4_directory_minimum(1)) {
				if (!dotdot || number == 0 ||
				    (expected_parent != 0 && number != expected_parent) ||
				    (parent->number == EXT4_ROOT_INODE &&
					number != EXT4_ROOT_INODE)) {
					return EXT4_CORRUPT;
				}
			} else if (number != 0 && (dot || dotdot)) {
				return EXT4_CORRUPT;
			}
			used = 0;
			if (number != 0) {
				if (names == 0 ||
				    ((fs->info.feature_incompat & EXT4_FEATURE_INCOMPAT_FILETYPE) &&
					(entry->type > EXT4_FT_SYMLINK ||
					    ((dot || dotdot) && entry->type != EXT4_FT_DIRECTORY &&
						entry->type != EXT4_FT_UNKNOWN)))) {
					return EXT4_CORRUPT;
				}
				/* Encrypted names are ciphertext and may contain any byte. */
				for (index = 0;
				    !(parent->flags & EXT4_INODE_ENCRYPT) && index < names;
				    index++) {
					if (entry_name[index] == 0 || entry_name[index] == '/') {
						return EXT4_CORRUPT;
					}
				}
				if (tree != NULL && logical != 0) {
					error = ext4_directory_name_hash(key, tree->version,
					    tree->seed, entry_name, names, &hash);
					if (error != EXT4_OK ||
					    !ext4_index_contains(range, hash.major)) {
						return EXT4_CORRUPT;
					}
				}
				if (name != NULL &&
				    ext4_directory_name_match(key, entry_name, names)) {
					if (exists) {
						return EXT4_CORRUPT;
					}
					exists = true;
					/* INSERT also reports the existing identity, allowing
					 * idempotent replay without a separate full FIND pass. */
					slot->number = number;
					if (action == EXT4_DIRECTORY_FIND) {
						slot->logical = logical;
						slot->physical = run.physical;
						slot->offset = offset;
						slot->length = length;
						slot->previous = previous;
						slot->type = (fs->info.feature_incompat &
								 EXT4_FEATURE_INCOMPAT_FILETYPE)
						    ? (enum ext4_file_type)entry->type
						    : EXT4_FT_UNKNOWN;
					}
				}
				populated |= !dot && !dotdot;
				used = ext4_directory_minimum(names);
				occupied += used;
			}
			if (eligible && action == EXT4_DIRECTORY_INSERT &&
			    slot->logical == UINT32_MAX && length - used >= required) {
				slot->logical = logical;
				slot->physical = run.physical;
				slot->offset = offset;
				slot->used = used;
				slot->length = length;
			}
		}
		if (tree != NULL && eligible && occupied < best) {
			best = occupied;
			repack.logical = logical;
			repack.physical = run.physical;
			repack.repack = true;
		}
	}
	if (error != EXT4_OK) {
		return error;
	}
	if (action == EXT4_DIRECTORY_EMPTY) {
		return populated ? EXT4_NOT_EMPTY : EXT4_OK;
	}
	if (action == EXT4_DIRECTORY_FIND) {
		return exists ? EXT4_OK : EXT4_NOT_FOUND;
	}
	if (exists) {
		return EXT4_EXISTS;
	}
	if (slot->logical == UINT32_MAX) {
		if (tree != NULL) {
			if (best == UINT32_MAX) {
				return EXT4_CORRUPT;
			}
			*slot = repack;
			return EXT4_OK;
		}
		if (blocks == EXT4_DIRECTORY_MAX_BLOCKS) {
			return EXT4_UNSUPPORTED;
		}
		slot->logical = (uint32_t)blocks;
		slot->length = usable;
	}
	return EXT4_OK;
}

static enum ext4_result
ext4_directory_scan_named(struct ext4_allocation *allocation, struct ext4_inode *parent,
    struct ext4_inode_disk *disk, const uint8_t *name, size_t name_length,
    enum ext4_directory_action action, uint32_t expected_parent, struct ext4_directory_slot *slot,
    struct ext4_directory_name *key)
{
	struct ext4_fs *fs = allocation->fs;
	struct ext4_directory_index tree;
	bool grown;
	bool indexed = (parent->flags & EXT4_INODE_INDEX) != 0;
	enum ext4_result error;

	if (parent->flags & EXT4_INODE_INLINE_DATA) {
		error = ext4_write_map_validate(allocation, parent, disk);
		if (error != EXT4_OK) {
			return error;
		}
		error = ext4_directory_scan_blocks(allocation, parent, disk, name, name_length,
		    action, expected_parent, slot, NULL, key);
		if (error != EXT4_OK || action != EXT4_DIRECTORY_INSERT || slot->logical == 0) {
			return error;
		}
		error = ext4_inline_grow(allocation, parent, disk, parent->size + 1U, &grown);
		if (error != EXT4_OK) {
			return error;
		}
		if (grown) {
			error = ext4_directory_scan_blocks(allocation, parent, disk, name,
			    name_length, action, expected_parent, slot, NULL, key);
			if (error != EXT4_OK || slot->logical == 0) {
				return error;
			}
		}
		error = ext4_inline_expand(allocation, parent, disk);
		if (error != EXT4_OK) {
			return error;
		}
	}
	if (parent->size == 0 || parent->size % fs->info.block_size != 0) {
		return EXT4_CORRUPT;
	}
	if (parent->size / fs->info.block_size > EXT4_DIRECTORY_MAX_BLOCKS) {
		return EXT4_UNSUPPORTED;
	}
	error = indexed
	    ? ext4_index_open(allocation, parent, disk, &tree, action == EXT4_DIRECTORY_EMPTY)
	    : ext4_write_map_validate(allocation, parent, disk);
	if (error != EXT4_OK) {
		return error;
	}
	error = ext4_directory_scan_blocks(allocation, parent, disk, name, name_length, action,
	    expected_parent, slot, indexed ? &tree : NULL, key);
	if (indexed) {
		ext4_index_close(&tree);
	}
	return error;
}

enum ext4_result
ext4_directory_scan(struct ext4_allocation *allocation, struct ext4_inode *parent,
    struct ext4_inode_disk *disk, const uint8_t *name, size_t name_length,
    enum ext4_directory_action action, uint32_t expected_parent, struct ext4_directory_slot *slot)
{
	struct ext4_fs *fs = allocation->fs;
	struct ext4_directory_name key;
	enum ext4_result error;

	/* A strict encoding admits only names the directory can fold. */
	if (action == EXT4_DIRECTORY_INSERT && ext4_directory_casefolded(fs, parent) &&
	    fs->casefold_strict && !ext4_utf8_name_valid(name, name_length)) {
		return EXT4_INVALID_ARGUMENT;
	}
	error = ext4_directory_name_open(fs, parent, name, name_length, &key);
	if (error != EXT4_OK) {
		return error;
	}
	error = ext4_directory_scan_named(
	    allocation, parent, disk, name, name_length, action, expected_parent, slot, &key);
	ext4_directory_name_close(fs, &key);
	return error;
}

static uint64_t
ext4_directory_sort_key(const struct ext4_directory_sort_entry *entry)
{
	return ((uint64_t)entry->hash << 32) | entry->offset;
}

static void
ext4_directory_sift(struct ext4_directory_sort_entry *entries, uint32_t root, uint32_t count)
{
	struct ext4_directory_sort_entry value = entries[root];
	uint32_t child;

	for (child = root * 2U + 1U; child < count; child = root * 2U + 1U) {
		if (child + 1U < count &&
		    ext4_directory_sort_key(&entries[child]) <
			ext4_directory_sort_key(&entries[child + 1U])) {
			child++;
		}
		if (ext4_directory_sort_key(&entries[child]) <= ext4_directory_sort_key(&value)) {
			break;
		}
		entries[root] = entries[child];
		root = child;
	}
	entries[root] = value;
}

static void
ext4_directory_sort(struct ext4_directory_sort_entry *entries, uint32_t count)
{
	struct ext4_directory_sort_entry value;
	uint32_t index;

	for (index = count / 2; index > 0; index--) {
		ext4_directory_sift(entries, index - 1, count);
	}
	for (index = count; index > 1; index--) {
		value = entries[index - 1];
		entries[index - 1] = entries[0];
		entries[0] = value;
		ext4_directory_sift(entries, 0, index - 1);
	}
}

static void
ext4_directory_pack(struct ext4_fs *fs, const struct ext4_inode *parent, uint8_t *buffer,
    const uint8_t *original, const struct ext4_directory_sort_entry *entries, uint32_t count,
    uint32_t number, enum ext4_file_type type, const uint8_t *name, size_t name_length)
{
	const struct ext4_dir_header_disk *entry;
	uint32_t offset = 0;
	uint32_t length;
	uint32_t index;

	ext4_zero(buffer, fs->info.block_size);
	for (index = 0; index < count; index++) {
		length =
		    index + 1U == count ? ext4_directory_usable(fs) - offset : entries[index].used;
		if (entries[index].offset == UINT32_MAX) {
			ext4_directory_entry(
			    fs, buffer + offset, length, number, type, name, name_length);
		} else {
			entry =
			    (const struct ext4_dir_header_disk *)(original + entries[index].offset);
			ext4_directory_entry(fs, buffer + offset, length, ext4_le32(&entry->inode),
			    (enum ext4_file_type)entry->type, (const uint8_t *)(entry + 1),
			    entry->name_length);
		}
		offset += length;
	}
	ext4_directory_checksum_set(fs, parent, buffer);
}

static enum ext4_result
ext4_directory_index_start(struct ext4_allocation *allocation, const struct ext4_inode *parent,
    struct ext4_inode_disk *disk, struct ext4_directory_index *tree)
{
	uint32_t flags;
	unsigned int word;
	enum ext4_result error;

	ext4_zero(tree, sizeof(*tree));
	tree->allocation = allocation;
	tree->inode = parent;
	tree->disk = disk;
	tree->blocks = 1;
	error = ext4_allocation_super(allocation);
	if (error != EXT4_OK) {
		return error;
	}
	flags = ext4_le32(&allocation->super->flags) &
	    (EXT4_SIGNED_DIRECTORY_HASH | EXT4_UNSIGNED_DIRECTORY_HASH);
	if (flags == (EXT4_SIGNED_DIRECTORY_HASH | EXT4_UNSIGNED_DIRECTORY_HASH)) {
		return EXT4_CORRUPT;
	}
	tree->version = allocation->super->default_hash_version;
	if (tree->version > EXT4_HASH_TEA_UNSIGNED) {
		return EXT4_UNSUPPORTED;
	}
	/* A newly created index can state its unsigned hash explicitly, even on
	 * older media without a recorded architecture's signedness. */
	if (tree->version <= EXT4_HASH_TEA && flags != EXT4_SIGNED_DIRECTORY_HASH) {
		tree->version += EXT4_HASH_LEGACY_UNSIGNED;
	}
	for (word = 0; word < 4; word++) {
		tree->seed[word] = ext4_le32(&allocation->super->hash_seed[word]);
	}
	return EXT4_OK;
}

static void
ext4_directory_index_root(struct ext4_directory_index *tree, uint8_t *buffer, uint32_t left,
    uint32_t right, uint32_t separator)
{
	struct ext4_fs *fs = tree->allocation->fs;
	struct ext4_dx_root_prefix_disk *root = (struct ext4_dx_root_prefix_disk *)buffer;
	struct ext4_dx_count_disk *counts = (struct ext4_dx_count_disk *)(root + 1);
	struct ext4_dx_entry_disk *entries = (struct ext4_dx_entry_disk *)counts;
	uint32_t dot_length = offsetof(struct ext4_dx_root_prefix_disk, dotdot);
	uint32_t tail = fs->metadata_checksum ? sizeof(struct ext4_dx_tail_disk) : 0;

	ext4_zero(buffer, fs->info.block_size);
	ext4_directory_entry(fs, buffer, dot_length, tree->inode->number, EXT4_FT_DIRECTORY,
	    (const uint8_t *)".", 1);
	ext4_directory_entry(fs, buffer + dot_length, fs->info.block_size - dot_length,
	    tree->parent_number, EXT4_FT_DIRECTORY, (const uint8_t *)"..", 2);
	root->hash_version = tree->version;
	root->info_length = sizeof(*root) - offsetof(struct ext4_dx_root_prefix_disk, reserved);
	ext4_encode16(&counts->limit,
	    (uint16_t)((fs->info.block_size - sizeof(*root) - tail) / sizeof(*entries)));
	ext4_encode16(&counts->count, right == 0 ? 1 : 2);
	ext4_encode32(&entries[0].block, left);
	if (right != 0) {
		ext4_encode32(&entries[1].hash, separator);
		ext4_encode32(&entries[1].block, right);
	}
	ext4_index_checksum_set(fs, tree->inode, 0, buffer);
}

static enum ext4_result
ext4_directory_repack(struct ext4_allocation *allocation, const struct ext4_inode *parent,
    struct ext4_inode_disk *disk, struct ext4_directory_slot *slot, uint32_t number,
    enum ext4_file_type type, const uint8_t *name, size_t name_length, bool convert)
{
	struct ext4_fs *fs = allocation->fs;
	struct ext4_directory_index tree;
	struct ext4_directory_sort_entry *entries = NULL;
	struct ext4_dir_header_disk *entry;
	struct ext4_directory_name key = { 0 };
	struct ext4_name_hash hash;
	uint8_t *original = NULL;
	uint8_t *right_buffer = NULL;
	uint8_t *new_buffer = NULL;
	void *left_buffer = NULL;
	void *root_buffer = NULL;
	uint64_t physical;
	uint64_t root_physical = 0;
	uint32_t usable = ext4_directory_usable(fs);
	uint32_t capacity = usable / ext4_directory_minimum(1) + 1;
	uint32_t next;
	uint32_t right = 0;
	uint32_t left_logical = slot->logical;
	uint32_t offset;
	uint32_t length;
	uint32_t count = 0;
	uint32_t total = 0;
	uint32_t left = 0;
	uint32_t cut = 0;
	uint32_t distance;
	uint32_t best = UINT32_MAX;
	uint32_t index;
	uint32_t separator = 0;
	bool more = true;
	enum ext4_result error;

	error = convert ? ext4_directory_index_start(allocation, parent, disk, &tree)
			: ext4_index_open(allocation, parent, disk, &tree, false);
	if (error != EXT4_OK) {
		return error;
	}
	error = ext4_directory_name_open(fs, parent, name, name_length, &key);
	if (error != EXT4_OK) {
		goto out;
	}
	next = tree.blocks;
	/* The slot's leaf is on the new name's probed path or continues its hash. */
	if (!convert) {
		error = ext4_directory_name_hash(
		    &key, tree.version, tree.seed, name, name_length, &hash);
		if (error == EXT4_OK) {
			error = ext4_index_probe(&tree, hash.major);
		}
		while (error == EXT4_OK && more && tree.leaf_logical != slot->logical) {
			error = ext4_index_next(&tree, hash.major, &more);
		}
		if (error == EXT4_OK && tree.leaf_logical != slot->logical) {
			error = EXT4_CORRUPT;
		}
		if (error != EXT4_OK) {
			goto out;
		}
	}
	error = ext4_index_read(&tree, convert ? 0 : slot->logical, &physical);
	if (error == EXT4_OK) {
		error = ext4_directory_checksum(
		    fs, parent, convert ? 0 : slot->logical, allocation->scratch);
	}
	if (error != EXT4_OK) {
		goto out;
	}
	if (!convert && physical != slot->physical) {
		error = EXT4_CORRUPT;
		goto out;
	}
	root_physical = physical;
	original = fs->environment.allocate(fs->environment.context, fs->info.block_size);
	entries =
	    fs->environment.allocate(fs->environment.context, (size_t)capacity * sizeof(*entries));
	if (original == NULL || entries == NULL) {
		error = EXT4_NO_MEMORY;
		goto out;
	}
	ext4_copy(original, allocation->scratch, fs->info.block_size);
	for (offset = 0; offset < usable; offset += length) {
		if (usable - offset < sizeof(*entry)) {
			error = EXT4_CORRUPT;
			goto out;
		}
		entry = (struct ext4_dir_header_disk *)(original + offset);
		length = ext4_directory_record_length(fs, entry);
		if (length < ext4_directory_minimum(entry->name_length) ||
		    (length & (EXT4_DIRECTORY_ALIGNMENT - 1U)) || length > usable - offset ||
		    ext4_le32(&entry->inode) > fs->info.inodes ||
		    (!(fs->info.feature_incompat & EXT4_FEATURE_INCOMPAT_FILETYPE) &&
			entry->type != 0)) {
			error = EXT4_CORRUPT;
			goto out;
		}
		if (convert &&
		    (offset == 0 || offset == offsetof(struct ext4_dx_root_prefix_disk, dotdot))) {
			if ((offset == 0 &&
				(ext4_le32(&entry->inode) != parent->number ||
				    entry->name_length != 1 ||
				    length != offsetof(struct ext4_dx_root_prefix_disk, dotdot) ||
				    original[offset + sizeof(*entry)] != '.')) ||
			    (offset != 0 &&
				(ext4_le32(&entry->inode) == 0 || entry->name_length != 2 ||
				    original[offset + sizeof(*entry)] != '.' ||
				    original[offset + sizeof(*entry) + 1] != '.' ||
				    (parent->number == EXT4_ROOT_INODE &&
					ext4_le32(&entry->inode) != EXT4_ROOT_INODE))) ||
			    (entry->type != EXT4_FT_UNKNOWN && entry->type != EXT4_FT_DIRECTORY)) {
				error = EXT4_CORRUPT;
				goto out;
			}
			if (offset != 0) {
				tree.parent_number = ext4_le32(&entry->inode);
			}
			continue;
		}
		if (ext4_le32(&entry->inode) == 0) {
			continue;
		}
		if (entry->name_length == 0 || entry->type > EXT4_FT_SYMLINK) {
			error = EXT4_CORRUPT;
			goto out;
		}
		if ((entry->name_length == 1 && original[offset + sizeof(*entry)] == '.') ||
		    (entry->name_length == 2 && original[offset + sizeof(*entry)] == '.' &&
			original[offset + sizeof(*entry) + 1] == '.') ||
		    ext4_directory_name_match(
			&key, original + offset + sizeof(*entry), entry->name_length)) {
			error = EXT4_CORRUPT;
			goto out;
		}
		/* Encrypted names are ciphertext and may contain any byte. */
		for (index = 0; !(parent->flags & EXT4_INODE_ENCRYPT) && index < entry->name_length;
		    index++) {
			if (original[offset + sizeof(*entry) + index] == 0 ||
			    original[offset + sizeof(*entry) + index] == '/') {
				error = EXT4_CORRUPT;
				goto out;
			}
		}
		error = ext4_directory_name_hash(&key, tree.version, tree.seed,
		    (const uint8_t *)(entry + 1), entry->name_length, &hash);
		if (error != EXT4_OK || count + 1U >= capacity ||
		    (!convert && !ext4_index_contains(&tree.leaf, hash.major))) {
			error = EXT4_CORRUPT;
			goto out;
		}
		entries[count].hash = hash.major;
		entries[count].offset = offset;
		entries[count].used = ext4_directory_minimum(entry->name_length);
		total += entries[count++].used;
	}
	error = ext4_directory_name_hash(&key, tree.version, tree.seed, name, name_length, &hash);
	if (error != EXT4_OK) {
		goto out;
	}
	entries[count].hash = hash.major;
	entries[count].offset = UINT32_MAX;
	entries[count].used = ext4_directory_minimum(name_length);
	total += entries[count++].used;
	ext4_directory_sort(entries, count);
	if (convert) {
		error = ext4_index_append(&tree, &next, &left_logical, &new_buffer);
		if (error != EXT4_OK) {
			goto out;
		}
		left_buffer = new_buffer;
	}
	if (total <= usable) {
		cut = count;
	} else {
		for (index = 1; index < count; index++) {
			left += entries[index - 1].used;
			if (left > usable || total - left > usable) {
				continue;
			}
			distance = left > total / 2 ? left - total / 2 : total / 2 - left;
			if (distance < best) {
				best = distance;
				cut = index;
			}
		}
		if (cut == 0) {
			error = EXT4_CORRUPT;
			goto out;
		}
		error = ext4_index_append(&tree, &next, &right, &right_buffer);
		if (error != EXT4_OK) {
			goto out;
		}
	}
	if (!convert) {
		error =
		    ext4_transaction_buffer(allocation->transaction, slot->physical, &left_buffer);
	}
	if (error != EXT4_OK) {
		goto out;
	}
	ext4_directory_pack(
	    fs, parent, left_buffer, original, entries, cut, number, type, name, name_length);
	if (cut != count) {
		ext4_directory_pack(fs, parent, right_buffer, original, entries + cut, count - cut,
		    number, type, name, name_length);
		separator =
		    entries[cut].hash | (entries[cut - 1].hash == entries[cut].hash ? 1U : 0);
		if (!convert) {
			error = ext4_index_add(&tree, separator, right, &next);
		}
	}
	if (error == EXT4_OK && convert) {
		error =
		    ext4_transaction_buffer(allocation->transaction, root_physical, &root_buffer);
		if (error == EXT4_OK) {
			ext4_directory_index_root(
			    &tree, root_buffer, left_logical, right, separator);
		}
	}
	if (error == EXT4_OK) {
		error = ext4_inode_account(
		    allocation, parent, disk, (uint64_t)next * fs->info.block_size);
		if (error == EXT4_OK && convert) {
			ext4_encode32(&disk->flags, ext4_le32(&disk->flags) | EXT4_INODE_INDEX);
		}
	}
	/* The split or conversion preserves a classified index. */
	if (error == EXT4_OK) {
		ext4_index_remember(allocation, parent, disk);
	}
out:
	ext4_directory_name_close(fs, &key);
	if (entries != NULL) {
		fs->environment.release(
		    fs->environment.context, entries, (size_t)capacity * sizeof(*entries));
	}
	if (original != NULL) {
		fs->environment.release(fs->environment.context, original, fs->info.block_size);
	}
	ext4_index_close(&tree);
	return error;
}

static struct ext4_dir_header_disk *
ext4_directory_inline_entry(const struct ext4_directory_slot *slot, uint32_t offset)
{
	uint8_t *base;

	if (offset >= EXT4_INLINE_FIRST_END) {
		base = (uint8_t *)slot->inline_disk + slot->inline_tail;
		return (struct ext4_dir_header_disk *)(base + offset - EXT4_INLINE_FIRST_END);
	}
	base = slot->inline_disk->block_data + EXT4_INLINE_PARENT_SIZE;
	return (struct ext4_dir_header_disk *)(base + offset - EXT4_INLINE_DOTS_SIZE);
}

static void
ext4_directory_inline_changed(const struct ext4_directory_slot *slot)
{
	/* An inode-body value may use the canonical zero hash. The inode checksum
	 * is set by the namespace owner after all its private edits are complete. */
	ext4_zero((uint8_t *)slot->inline_disk + slot->inline_hash, sizeof(struct ext4_le32));
}

enum ext4_result
ext4_directory_insert(struct ext4_allocation *allocation, const struct ext4_inode *parent,
    struct ext4_inode_disk *disk, struct ext4_directory_slot *slot, uint32_t number,
    enum ext4_file_type type, const uint8_t *name, size_t name_length)
{
	struct ext4_fs *fs = allocation->fs;
	struct ext4_dir_header_disk *previous;
	uint8_t *buffer;
	void *snapshot;
	uint64_t size = parent->size;
	bool zero;
	enum ext4_result error;

	if (slot->inline_disk != NULL) {
		buffer = (uint8_t *)ext4_directory_inline_entry(slot, slot->offset);
		if (slot->used != 0) {
			ext4_directory_length((struct ext4_dir_header_disk *)buffer, slot->used);
		}
		ext4_directory_entry(fs, buffer + slot->used, slot->length - slot->used, number,
		    type, name, name_length);
		ext4_directory_inline_changed(slot);
		return ext4_inode_account(allocation, parent, disk, parent->size);
	}
	if (slot->repack) {
		return ext4_directory_repack(
		    allocation, parent, disk, slot, number, type, name, name_length, false);
	}
	if (slot->physical == 0 && parent->size == fs->info.block_size &&
	    !(parent->flags & EXT4_INODE_INDEX) &&
	    (fs->info.feature_compat & EXT4_FEATURE_COMPAT_DIR_INDEX)) {
		return ext4_directory_repack(
		    allocation, parent, disk, slot, number, type, name, name_length, true);
	}
	if (slot->physical == 0) {
		error = ext4_write_map_allocate(
		    allocation, parent, disk, slot->logical, &slot->physical, &zero);
		if (error != EXT4_OK) {
			return error;
		}
		size += fs->info.block_size;
	}
	error = ext4_transaction_buffer(allocation->transaction, slot->physical, &snapshot);
	if (error != EXT4_OK) {
		return error;
	}
	buffer = snapshot;
	if (size != parent->size) {
		ext4_zero(buffer, fs->info.block_size);
	}
	if (slot->used != 0) {
		previous = (struct ext4_dir_header_disk *)(buffer + slot->offset);
		ext4_directory_length(previous, slot->used);
	}
	ext4_directory_entry(fs, buffer + slot->offset + slot->used, slot->length - slot->used,
	    number, type, name, name_length);
	ext4_directory_checksum_set(fs, parent, buffer);
	return ext4_inode_account(allocation, parent, disk, size);
}

enum ext4_result
ext4_directory_initialize(struct ext4_allocation *allocation, struct ext4_inode *inode,
    struct ext4_inode_disk *disk, uint32_t parent)
{
	struct ext4_fs *fs = allocation->fs;
	void *buffer = NULL;
	uint64_t physical;
	uint32_t first = ext4_directory_minimum(1);
	bool zero;
	bool inline_created;
	enum ext4_result error;

	/* Like Linux, encrypted directories never keep their entries in the inode. */
	inline_created = false;
	error = inode->flags & EXT4_INODE_ENCRYPT
	    ? EXT4_OK
	    : ext4_inline_start(allocation, inode, disk, parent, &inline_created);
	if (error != EXT4_OK) {
		return error;
	}
	if (inline_created) {
		return ext4_inode_account(allocation, inode, disk, inode->size);
	}
	error = ext4_write_map_allocate(allocation, inode, disk, 0, &physical, &zero);
	if (error == EXT4_OK) {
		error = ext4_transaction_buffer(allocation->transaction, physical, &buffer);
	}
	if (error != EXT4_OK) {
		return error;
	}
	ext4_zero(buffer, fs->info.block_size);
	ext4_directory_entry(
	    fs, buffer, first, inode->number, EXT4_FT_DIRECTORY, (const uint8_t *)".", 1);
	ext4_directory_entry(fs, (uint8_t *)buffer + first, ext4_directory_usable(fs) - first,
	    parent, EXT4_FT_DIRECTORY, (const uint8_t *)"..", 2);
	ext4_directory_checksum_set(fs, inode, buffer);
	return ext4_inode_account(allocation, inode, disk, fs->info.block_size);
}

enum ext4_result
ext4_directory_remove(struct ext4_allocation *allocation, const struct ext4_inode *parent,
    const struct ext4_directory_slot *slot)
{
	struct ext4_dir_header_disk *entry;
	struct ext4_dir_header_disk *previous;
	void *snapshot;
	uint8_t *buffer;
	uint32_t length;
	enum ext4_result error;

	buffer = NULL;
	if (slot->inline_disk != NULL) {
		entry = ext4_directory_inline_entry(slot, slot->offset);
	} else {
		error = ext4_transaction_buffer(allocation->transaction, slot->physical, &snapshot);
		if (error != EXT4_OK) {
			return error;
		}
		buffer = snapshot;
		entry = (struct ext4_dir_header_disk *)(buffer + slot->offset);
	}
	if (ext4_le32(&entry->inode) != slot->number ||
	    ext4_directory_record_length(allocation->fs, entry) != slot->length) {
		return EXT4_CORRUPT;
	}
	ext4_zero(entry, slot->length);
	if (slot->previous == UINT32_MAX) {
		ext4_directory_length(entry, slot->length);
	} else {
		previous = slot->inline_disk != NULL
		    ? ext4_directory_inline_entry(slot, slot->previous)
		    : (struct ext4_dir_header_disk *)(buffer + slot->previous);
		length = ext4_directory_record_length(allocation->fs, previous);
		if (slot->previous + length != slot->offset) {
			return EXT4_CORRUPT;
		}
		ext4_directory_length(previous, length + slot->length);
	}
	if (slot->inline_disk != NULL) {
		ext4_directory_inline_changed(slot);
	} else {
		ext4_directory_checksum_set(allocation->fs, parent, buffer);
	}
	return EXT4_OK;
}

enum ext4_result
ext4_directory_replace(struct ext4_allocation *allocation, const struct ext4_inode *parent,
    const struct ext4_directory_slot *slot, uint32_t number, enum ext4_file_type type)
{
	struct ext4_dir_header_disk *entry;
	void *buffer;
	enum ext4_result error;

	buffer = NULL;
	if (slot->inline_disk != NULL && slot->offset == EXT4_INLINE_DOT_SIZE) {
		if (type != EXT4_FT_DIRECTORY ||
		    ext4_le32((const struct ext4_le32 *)slot->inline_disk->block_data) !=
			slot->number) {
			return EXT4_CORRUPT;
		}
		ext4_encode32((struct ext4_le32 *)slot->inline_disk->block_data, number);
		return EXT4_OK;
	}
	if (slot->inline_disk != NULL) {
		entry = ext4_directory_inline_entry(slot, slot->offset);
	} else {
		error = ext4_transaction_buffer(allocation->transaction, slot->physical, &buffer);
		if (error != EXT4_OK) {
			return error;
		}
		entry = (struct ext4_dir_header_disk *)((uint8_t *)buffer + slot->offset);
	}
	if (ext4_le32(&entry->inode) != slot->number ||
	    ext4_directory_record_length(allocation->fs, entry) != slot->length) {
		return EXT4_CORRUPT;
	}
	ext4_encode32(&entry->inode, number);
	if (allocation->fs->info.feature_incompat & EXT4_FEATURE_INCOMPAT_FILETYPE) {
		entry->type = (uint8_t)type;
	}
	if ((parent->flags & EXT4_INODE_INDEX) && slot->logical == 0) {
		ext4_index_checksum_set(allocation->fs, parent, 0, buffer);
	} else {
		if (slot->inline_disk != NULL) {
			ext4_directory_inline_changed(slot);
		} else {
			ext4_directory_checksum_set(allocation->fs, parent, buffer);
		}
	}
	return EXT4_OK;
}

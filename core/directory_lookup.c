/* SPDX-License-Identifier: BSD-3-Clause */
#include "fscrypt.h"
#include "directory_index.h"
#include "inline.h"
#include "unicode.h"

struct ext4_lookup_node {
	uint8_t *buffer;
	struct ext4_index_range range;
	uint32_t logical;
	uint16_t count;
	uint16_t position;
};

struct ext4_lookup_state {
	struct ext4_fs *fs;
	const struct ext4_inode *directory;
	const uint8_t *name;
	size_t name_length;
	uint64_t blocks;
	uint64_t leaves;
	uint32_t hash;
	uint8_t version;
	uint8_t levels;
	struct ext4_lookup_node nodes[EXT4_DX_MAX_INDIRECT_LEVELS + 1U];
	uint8_t *leaf;
	struct ext4_directory_name key;
};

static const struct ext4_dx_entry_disk *
ext4_lookup_entries(const struct ext4_lookup_node *node)
{
	size_t base = node->logical == 0 ? sizeof(struct ext4_dx_root_prefix_disk)
					 : sizeof(struct ext4_dir_header_disk);

	return (const struct ext4_dx_entry_disk *)(node->buffer + base);
}

static struct ext4_index_range
ext4_lookup_range(const struct ext4_lookup_node *node, uint16_t position)
{
	const struct ext4_dx_entry_disk *entries = ext4_lookup_entries(node);
	struct ext4_index_range range = { 0 };

	range.lower = position == 0 ? node->range.lower : ext4_le32(&entries[position].hash);
	range.upper = position + 1U == node->count ? node->range.upper
						   : ext4_le32(&entries[position + 1U].hash);
	return range;
}

static enum ext4_result
ext4_lookup_read(struct ext4_lookup_state *state, uint32_t logical, uint8_t *buffer)
{
	size_t completed;
	uint32_t used;
	uint32_t tail;
	uint32_t hash;
	enum ext4_result error;

	if (logical >= state->blocks) {
		return EXT4_CORRUPT;
	}
	if (state->directory->flags & EXT4_INODE_INLINE_DATA) {
		return ext4_inline_directory(
		    state->fs, state->directory, NULL, buffer, true, &used, &tail, &hash);
	}
	error =
	    ext4_read(state->fs, state->directory, (uint64_t)logical * state->fs->info.block_size,
		buffer, state->fs->info.block_size, &completed);
	if (error == EXT4_OK && completed != state->fs->info.block_size) {
		return EXT4_CORRUPT;
	}
	return error;
}

static enum ext4_result
ext4_lookup_node_validate(struct ext4_lookup_state *state, const struct ext4_lookup_node *node)
{
	const struct ext4_dx_entry_disk *entries = ext4_lookup_entries(node);
	struct ext4_index_range range;
	uint32_t previous = node->range.lower;
	uint32_t child;
	uint16_t position;

	for (position = 0; position < node->count; position++) {
		range = ext4_lookup_range(node, position);
		child = ext4_le32(&entries[position].block);
		if (range.lower < previous || range.lower >= EXT4_HASH_EOF ||
		    (position != 0 && range.lower == previous && !(range.lower & 1U)) ||
		    range.upper > node->range.upper || range.lower > range.upper ||
		    (range.lower == range.upper && !(range.lower & 1U)) || child == 0 ||
		    child >= state->blocks || child == node->logical) {
			return EXT4_CORRUPT;
		}
		previous = range.lower;
	}
	return EXT4_OK;
}

static uint16_t
ext4_lookup_position(const struct ext4_lookup_node *node, uint32_t hash)
{
	const struct ext4_dx_entry_disk *entries = ext4_lookup_entries(node);
	uint16_t first = 1;
	uint16_t end = node->count;
	uint16_t middle;

	/* The first entry inherits its lower bound. A continuation's odd boundary
	 * sorts after the even name hash, so search starts before that boundary. */
	while (first < end) {
		middle = (uint16_t)(first + (end - first) / 2U);
		if (ext4_le32(&entries[middle].hash) <= hash) {
			first = (uint16_t)(middle + 1U);
		} else {
			end = middle;
		}
	}
	return (uint16_t)(first - 1U);
}

static enum ext4_result
ext4_lookup_child(const struct ext4_lookup_node *node, uint16_t position, uint32_t *logical)
{
	const struct ext4_dx_entry_disk *entries = ext4_lookup_entries(node);
	uint32_t child = ext4_le32(&entries[position].block);
	uint16_t other;

	/* Reject aliasing of a selected child without allocating a directory-sized
	 * topology map. Unvisited subtrees remain the full validator's concern. */
	for (other = 0; other < node->count; other++) {
		if (other != position && ext4_le32(&entries[other].block) == child) {
			return EXT4_CORRUPT;
		}
	}
	*logical = child;
	return EXT4_OK;
}

static enum ext4_result
ext4_lookup_scan(struct ext4_lookup_state *state, uint32_t logical,
    const struct ext4_index_range *range, uint32_t *number)
{
	struct ext4_dir_entry entry;
	struct ext4_name_hash hash;
	uint32_t found = 0;
	uint32_t offset = 0;
	uint32_t length;
	enum ext4_result error;

	error = ext4_lookup_read(state, logical, state->leaf);
	if (error == EXT4_OK) {
		error = ext4_directory_checksum(state->fs, state->directory, logical, state->leaf);
	}
	if (error != EXT4_OK) {
		return error;
	}
	while (offset < state->fs->info.block_size) {
		error = ext4_directory_entry_decode(state->fs, state->leaf, offset,
		    (state->directory->flags & EXT4_INODE_ENCRYPT) != 0, &entry, &length);
		if (error != EXT4_OK) {
			return error;
		}
		offset += length;
		if (entry.inode == 0) {
			continue;
		}
		if (range != NULL) {
			if ((entry.name_length == 1 && entry.name[0] == '.') ||
			    (entry.name_length == 2 && entry.name[0] == '.' &&
				entry.name[1] == '.')) {
				return EXT4_CORRUPT;
			}
			error = ext4_directory_name_hash(&state->key, state->version,
			    state->fs->directory_hash_seed, entry.name, entry.name_length, &hash);
			if (error != EXT4_OK) {
				return error;
			}
			if (!ext4_index_contains(range, hash.major)) {
				return EXT4_CORRUPT;
			}
		}
		if (ext4_directory_name_match(&state->key, entry.name, entry.name_length)) {
			if (found != 0) {
				return EXT4_CORRUPT;
			}
			found = entry.inode;
		}
	}
	if (found == 0) {
		return EXT4_NOT_FOUND;
	}
	*number = found;
	return EXT4_OK;
}

static enum ext4_result
ext4_lookup_leaf(struct ext4_lookup_state *state, uint32_t logical,
    const struct ext4_index_range *range, uint32_t *number)
{
	if (++state->leaves >= state->blocks) {
		return EXT4_CORRUPT;
	}
	return ext4_lookup_scan(state, logical, range, number);
}

static enum ext4_result
ext4_lookup_ancestors(const struct ext4_lookup_state *state, uint8_t level, uint32_t logical)
{
	const struct ext4_dx_entry_disk *entries;
	const struct ext4_lookup_node *node;
	uint8_t ancestor;
	uint16_t entry;

	for (ancestor = 0; ancestor <= level; ancestor++) {
		node = &state->nodes[ancestor];
		if (logical == node->logical) {
			return EXT4_CORRUPT;
		}
		/* Every child of an earlier ancestor is an index node at a shallower
		 * level. The selected child cannot alias any of those known nodes. */
		if (ancestor < level) {
			entries = ext4_lookup_entries(node);
			for (entry = 0; entry < node->count; entry++) {
				if (ext4_le32(&entries[entry].block) == logical) {
					return EXT4_CORRUPT;
				}
			}
		}
	}
	return EXT4_OK;
}

static enum ext4_result
ext4_lookup_linear(struct ext4_lookup_state *state, uint32_t *number)
{
	uint64_t logical;
	enum ext4_result error;

	for (logical = 0; logical < state->blocks; logical++) {
		error = ext4_lookup_scan(state, (uint32_t)logical, NULL, number);
		if (error != EXT4_NOT_FOUND) {
			return error;
		}
	}
	return EXT4_NOT_FOUND;
}

static enum ext4_result
ext4_lookup_indexed(struct ext4_lookup_state *state, uint32_t *number)
{
	struct ext4_index_metadata metadata;
	struct ext4_index_range range;
	struct ext4_name_hash hash;
	struct ext4_lookup_node *root = &state->nodes[0];
	struct ext4_lookup_node *node;
	struct ext4_lookup_node *child;
	uint32_t flags = state->fs->directory_hash_flags;
	uint32_t logical;
	uint16_t position;
	uint8_t level = 0;
	enum ext4_result error;

	if (state->blocks < 2 ||
	    !(state->fs->info.feature_compat & EXT4_FEATURE_COMPAT_DIR_INDEX)) {
		return EXT4_CORRUPT;
	}
	error = ext4_lookup_read(state, 0, root->buffer);
	if (error == EXT4_OK) {
		error = ext4_index_decode(state->fs, state->directory, 0, root->buffer, &metadata);
	}
	if (error != EXT4_OK) {
		return error;
	}
	root->count = metadata.count;
	root->range.upper = EXT4_DX_HASH_END;
	state->levels = metadata.levels;
	state->version = metadata.version;
	error = ext4_lookup_node_validate(state, root);
	if (error != EXT4_OK) {
		return error;
	}
	if (flags == (EXT4_SIGNED_DIRECTORY_HASH | EXT4_UNSIGNED_DIRECTORY_HASH)) {
		return EXT4_CORRUPT;
	}
	if (state->name[0] == '.' &&
	    (state->name_length == 1 || (state->name_length == 2 && state->name[1] == '.'))) {
		*number = state->name_length == 1 ? state->directory->number : metadata.parent;
		return EXT4_OK;
	}
	if (state->version <= EXT4_HASH_TEA) {
		if (flags == 0) {
			/* Older images can omit hash signedness. Exact linear comparison
			 * preserves read access without inventing an architecture policy. */
			return ext4_lookup_linear(state, number);
		}
		if (flags == EXT4_UNSIGNED_DIRECTORY_HASH) {
			state->version += EXT4_HASH_LEGACY_UNSIGNED;
		}
	}
	error = ext4_directory_name_hash(&state->key, state->version,
	    state->fs->directory_hash_seed, state->name, state->name_length, &hash);
	if (error != EXT4_OK) {
		return error;
	}
	state->hash = hash.major;
	root->position = ext4_lookup_position(root, state->hash);
	for (;;) {
		node = &state->nodes[level];
		position = node->position;
		if (position >= node->count) {
			if (level == 0) {
				return EXT4_NOT_FOUND;
			}
			level--;
			continue;
		}
		range = ext4_lookup_range(node, position);
		if (!ext4_index_contains(&range, state->hash)) {
			node->position = node->count;
			continue;
		}
		node->position++;
		error = ext4_lookup_child(node, position, &logical);
		if (error == EXT4_OK) {
			error = ext4_lookup_ancestors(state, level, logical);
		}
		if (error != EXT4_OK) {
			return error;
		}
		if (level == state->levels) {
			error = ext4_lookup_leaf(state, logical, &range, number);
			if (error != EXT4_NOT_FOUND) {
				return error;
			}
			continue;
		}
		child = &state->nodes[level + 1U];
		child->logical = logical;
		child->range = range;
		error = ext4_lookup_read(state, logical, child->buffer);
		if (error == EXT4_OK) {
			error = ext4_index_decode(
			    state->fs, state->directory, logical, child->buffer, &metadata);
		}
		if (error != EXT4_OK) {
			return error;
		}
		child->count = metadata.count;
		error = ext4_lookup_node_validate(state, child);
		if (error != EXT4_OK) {
			return error;
		}
		child->position = ext4_lookup_position(child, state->hash);
		level++;
	}
}

enum ext4_result
ext4_lookup(struct ext4_fs *fs, const struct ext4_inode *directory, const uint8_t *name,
    size_t name_length, struct ext4_inode *inode)
{
	struct ext4_lookup_state state = { 0 };
	struct ext4_inode found = { 0 };
	struct ext4_fscrypt_key key;
	struct ext4_fscrypt_nokey nokey;
	uint8_t cipher[EXT4_NAME_MAX];
	uint8_t padded[EXT4_NAME_MAX];
	uint8_t *buffer;
	size_t capacity;
	size_t index;
	size_t cipher_length = 0;
	uint32_t number;
	uint8_t levels;
	bool indexed;
	enum ext4_result error;

	if (fs == NULL || directory == NULL || name == NULL || inode == NULL || name_length == 0) {
		return EXT4_INVALID_ARGUMENT;
	}
	if (name_length > EXT4_NAME_MAX) {
		return EXT4_NAME_TOO_LONG;
	}
	for (index = 0; index < name_length; index++) {
		if (name[index] == 0 || name[index] == '/') {
			return EXT4_INVALID_ARGUMENT;
		}
	}
	if (fs->aborted) {
		return EXT4_RECOVERY_REQUIRED;
	}
	if ((directory->mode & EXT4_MODE_TYPE) != EXT4_MODE_DIRECTORY) {
		return EXT4_NOT_DIRECTORY;
	}
	/* Encrypted directories store and hash ciphertext names: the lookup name is
	 * encrypted as it would be stored. Casefolded ones hash plaintext names with a
	 * key derived for the directory, which is not implemented. */
	if ((directory->flags & EXT4_INODE_ENCRYPT) && !ext4_fscrypt_dot(name, name_length)) {
		error = ext4_fscrypt_key(fs, directory, &key);
		if ((error == EXT4_OK || error == EXT4_ENCRYPTED) &&
		    (directory->flags & EXT4_INODE_CASEFOLD)) {
			error = EXT4_UNSUPPORTED;
		}
		/* Without the key, names are Linux's no-key names; others do not exist. */
		if (error == EXT4_ENCRYPTED) {
			if (!ext4_fscrypt_nokey_decode(name, name_length, &nokey)) {
				return EXT4_NOT_FOUND;
			}
			error = ext4_directory_nokey_find(
			    fs, directory, &nokey, cipher, &cipher_length, &number);
			if (error == EXT4_OK) {
				error = ext4_get_inode(fs, number, &found);
			}
			if (error == EXT4_OK) {
				*inode = found;
			}
			return error;
		}
		if (error == EXT4_OK) {
			error = ext4_fscrypt_name_encrypt(fs, &key, name, name_length,
			    EXT4_NAME_MAX, padded, cipher, &cipher_length);
		}
		if (error != EXT4_OK) {
			return error;
		}
		name = cipher;
		name_length = cipher_length;
	}
	if (!(directory->flags & EXT4_INODE_INLINE_DATA) &&
	    directory->size % fs->info.block_size != 0) {
		return EXT4_CORRUPT;
	}
	state.blocks =
	    directory->flags & EXT4_INODE_INLINE_DATA ? 1 : directory->size / fs->info.block_size;
	if (state.blocks > (uint64_t)UINT32_MAX + 1U) {
		return EXT4_RANGE;
	}
	indexed = (directory->flags & EXT4_INODE_INDEX) != 0;
	levels = ext4_index_max_levels(fs);
	capacity = (indexed ? levels + 2U : 1U) * fs->info.block_size;
	buffer = fs->environment.allocate(fs->environment.context, capacity);
	if (buffer == NULL) {
		return EXT4_NO_MEMORY;
	}
	state.fs = fs;
	state.directory = directory;
	state.name = name;
	state.name_length = name_length;
	state.leaf = buffer;
	error = ext4_directory_name_open(fs, directory, name, name_length, &state.key);
	if (error != EXT4_OK) {
		fs->environment.release(fs->environment.context, buffer, capacity);
		return error;
	}
	if (indexed) {
		for (index = 0; index <= levels; index++) {
			state.nodes[index].buffer = buffer + index * fs->info.block_size;
		}
		state.leaf = buffer + (levels + 1U) * fs->info.block_size;
	}
	error =
	    indexed ? ext4_lookup_indexed(&state, &number) : ext4_lookup_linear(&state, &number);
	if (error == EXT4_OK) {
		error = ext4_get_inode(fs, number, &found);
		if (error == EXT4_OK) {
			*inode = found;
		}
	}
	ext4_directory_name_close(fs, &state.key);
	fs->environment.release(fs->environment.context, buffer, capacity);
	return error;
}

/* SPDX-License-Identifier: BSD-3-Clause */
#include "internal.h"
#include "fscrypt.h"
#include "inline.h"
#include "journal.h"
#include "verity.h"

static enum ext4_result ext4_map_blocks(struct ext4_fs *fs, const struct ext4_inode *inode,
    uint32_t logical, uint8_t **scratch, uint64_t *physical, uint64_t *blocks,
    struct ext4_block_path *path);

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

enum ext4_result
ext4_map_read(struct ext4_fs *fs, const struct ext4_inode *inode, uint64_t offset, size_t length,
    struct ext4_mapping *mapping)
{
	uint8_t *scratch = NULL;
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
	error = ext4_map_blocks(fs, inode, (uint32_t)logical, &scratch, &block, &blocks, NULL);
	if (scratch != NULL) {
		fs->environment.release(fs->environment.context, scratch, fs->info.block_size);
	}
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

/* leaf_end, when requested, receives the end of the selected leaf's last extent. */
static enum ext4_result
ext4_extent_map(struct ext4_fs *fs, const struct ext4_inode *inode, uint32_t logical,
    uint8_t **scratch, uint64_t *physical, uint64_t *blocks, struct ext4_block_path *path,
    uint64_t *leaf_end)
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
				} else if (start < boundary) {
					boundary = start;
				}
			}
		}
		if (depth == 0 || child == 0) {
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
		return ext4_extent_map(fs, inode, logical, scratch, physical, blocks, path, NULL);
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
	error = ext4_extent_map(fs, inode, UINT32_MAX, &scratch, &physical, NULL, NULL, end);
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

enum ext4_result
ext4_read_mapped(struct ext4_fs *fs, const struct ext4_inode *inode, uint64_t offset, void *buffer,
    size_t length, bool require_data, size_t *completed)
{
	uint8_t *output = buffer;
	uint8_t *scratch = NULL;
	uint64_t physical;
	uint64_t logical;
	uint64_t blocks;
	uint64_t bytes;
	size_t chunk;
	size_t in_block;
	enum ext4_result error = EXT4_OK;

	*completed = 0;
	while (*completed < length) {
		logical = offset / fs->info.block_size;
		if (logical > UINT32_MAX) {
			error = EXT4_RANGE;
			goto out;
		}
		in_block = (size_t)(offset % fs->info.block_size);
		error = ext4_map_blocks(
		    fs, inode, (uint32_t)logical, &scratch, &physical, &blocks, NULL);
		if (error != EXT4_OK) {
			goto out;
		}
		bytes = blocks * fs->info.block_size - in_block;
		chunk = length - *completed;
		if (bytes < chunk) {
			chunk = (size_t)bytes;
		}
		if (physical == 0) {
			if (require_data) {
				error = EXT4_CORRUPT;
				goto out;
			}
			ext4_zero(output + *completed, chunk);
		} else {
			error = ext4_device_read(fs, physical * fs->info.block_size + in_block,
			    output + *completed, chunk);
			if (error != EXT4_OK) {
				goto out;
			}
		}
		*completed += chunk;
		offset += chunk;
	}
out:
	if (scratch != NULL) {
		fs->environment.release(fs->environment.context, scratch, fs->info.block_size);
	}
	return error;
}

/* Decrypt an encrypted regular file's blocks. Holes and unwritten blocks read as
 * zeros, as in Linux; every other block is decrypted with the file's key and its
 * logical block number as the IV. */
static enum ext4_result
ext4_read_encrypted(struct ext4_fs *fs, const struct ext4_inode *inode, uint64_t offset,
    uint8_t *output, size_t length, size_t *completed)
{
	struct ext4_fscrypt_key key;
	uint8_t *scratch = NULL;
	uint8_t *blocks = NULL;
	uint64_t logical;
	uint64_t physical = 0;
	uint64_t run;
	size_t within;
	size_t chunk;
	enum ext4_result error;

	error = ext4_fscrypt_key(fs, inode, &key);
	if (error != EXT4_OK) {
		return error;
	}
	blocks = fs->environment.allocate(fs->environment.context, 2U * fs->info.block_size);
	if (blocks == NULL) {
		return EXT4_NO_MEMORY;
	}
	while (error == EXT4_OK && *completed < length) {
		logical = (offset + *completed) / fs->info.block_size;
		within = (size_t)((offset + *completed) % fs->info.block_size);
		chunk = fs->info.block_size - within;
		if (chunk > length - *completed) {
			chunk = length - *completed;
		}
		error = logical > UINT32_MAX ? EXT4_RANGE
					     : ext4_map_blocks(fs, inode, (uint32_t)logical,
						   &scratch, &physical, &run, NULL);
		if (error != EXT4_OK) {
			break;
		}
		if (physical == 0) {
			ext4_zero(output + *completed, chunk);
		} else {
			error = ext4_device_read(
			    fs, physical * fs->info.block_size, blocks, fs->info.block_size);
			if (error == EXT4_OK) {
				error = ext4_fscrypt_block(
				    fs, &key, logical, false, blocks, blocks + fs->info.block_size);
			}
			if (error != EXT4_OK) {
				break;
			}
			ext4_copy(
			    output + *completed, blocks + fs->info.block_size + within, chunk);
		}
		*completed += chunk;
	}
	if (scratch != NULL) {
		fs->environment.release(fs->environment.context, scratch, fs->info.block_size);
	}
	fs->environment.release(fs->environment.context, blocks, 2U * fs->info.block_size);
	return error;
}

/* Decrypt an encrypted symlink's target: a little-endian 16-bit ciphertext length,
 * then the ciphertext, stored in the inode or its block. Without the key, the target
 * reads as Linux presents it, the ciphertext's no-key name. */
static enum ext4_result
ext4_read_encrypted_link(struct ext4_fs *fs, const struct ext4_inode *inode, uint64_t offset,
    uint8_t *output, size_t length, size_t *completed)
{
	struct ext4_fscrypt_key key;
	struct ext4_le16 stored_length;
	uint8_t *stored;
	uint8_t *plain;
	size_t cipher_length;
	size_t plain_length = 0;
	size_t read = 0;
	size_t bytes = (size_t)inode->size;
	bool nokey;
	enum ext4_result error;

	error = ext4_fscrypt_key(fs, inode, &key);
	nokey = error == EXT4_ENCRYPTED;
	if (error != EXT4_OK && !nokey) {
		return error;
	}
	if (bytes < EXT4_FSCRYPT_SYMLINK_HEADER || bytes > fs->info.block_size ||
	    (inode->flags & EXT4_INODE_INLINE_DATA)) {
		return EXT4_CORRUPT;
	}
	stored = fs->environment.allocate(fs->environment.context, 2U * fs->info.block_size);
	if (stored == NULL) {
		return EXT4_NO_MEMORY;
	}
	plain = stored + fs->info.block_size;
	if (inode->fast_symlink) {
		error = bytes > sizeof(inode->block_data) ? EXT4_CORRUPT : EXT4_OK;
		if (error == EXT4_OK) {
			ext4_copy(stored, inode->block_data, bytes);
		}
	} else {
		error = ext4_read_mapped(fs, inode, 0, stored, bytes, true, &read);
	}
	if (error == EXT4_OK) {
		ext4_copy(&stored_length, stored, sizeof(stored_length));
		cipher_length = ext4_le16(&stored_length);
		if (cipher_length > bytes - EXT4_FSCRYPT_SYMLINK_HEADER ||
		    (nokey && cipher_length < EXT4_FSCRYPT_NAME_MIN)) {
			error = EXT4_CORRUPT;
		} else if (nokey) {
			plain_length = ext4_fscrypt_nokey_encode(
			    stored + EXT4_FSCRYPT_SYMLINK_HEADER, cipher_length, 0, 0, plain);
		} else {
			error = ext4_fscrypt_name_decrypt(fs, &key,
			    stored + EXT4_FSCRYPT_SYMLINK_HEADER, cipher_length,
			    fs->info.block_size, plain, &plain_length);
		}
	}
	if (error == EXT4_OK && offset < plain_length) {
		*completed =
		    plain_length - (size_t)offset < length ? plain_length - (size_t)offset : length;
		ext4_copy(output, plain + offset, *completed);
	}
	fs->environment.release(fs->environment.context, stored, 2U * fs->info.block_size);
	return error;
}

enum ext4_result
ext4_read(struct ext4_fs *fs, const struct ext4_inode *inode, uint64_t offset, void *buffer,
    size_t length, size_t *completed)
{
	struct ext4_fscrypt_key key;
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
	if ((inode->mode & EXT4_MODE_TYPE) != EXT4_MODE_REGULAR &&
	    (inode->mode & EXT4_MODE_TYPE) != EXT4_MODE_DIRECTORY &&
	    (inode->mode & EXT4_MODE_TYPE) != EXT4_MODE_SYMLINK) {
		return EXT4_UNSUPPORTED;
	}
	/* Encrypted directory blocks hold ciphertext names, but no encrypted bytes. */
	if ((inode->flags & EXT4_INODE_ENCRYPT) &&
	    (inode->mode & EXT4_MODE_TYPE) == EXT4_MODE_SYMLINK) {
		return ext4_read_encrypted_link(fs, inode, offset, buffer, length, completed);
	}
	if ((inode->flags & EXT4_INODE_ENCRYPT) &&
	    (inode->mode & EXT4_MODE_TYPE) == EXT4_MODE_REGULAR) {
		/* The tree of an encrypted verity file covers plaintext and is ciphertext. */
		if (inode->flags & EXT4_INODE_VERITY) {
			error = ext4_fscrypt_key(fs, inode, &key);
			return error == EXT4_OK ? EXT4_UNSUPPORTED : error;
		}
		if (inode->flags & EXT4_INODE_INLINE_DATA) {
			return EXT4_CORRUPT;
		}
		if (offset >= inode->size) {
			return EXT4_OK;
		}
		if (length > inode->size - offset) {
			length = (size_t)(inode->size - offset);
		}
		return ext4_read_encrypted(fs, inode, offset, buffer, length, completed);
	}
	if (inode->flags & EXT4_INODE_VERITY) {
		return ext4_verity_read(fs, inode, offset, buffer, length, completed);
	}
	if (offset >= inode->size) {
		return EXT4_OK;
	}
	if (length > inode->size - offset) {
		length = (size_t)(inode->size - offset);
	}
	if (inode->flags & EXT4_INODE_INLINE_DATA) {
		error = ext4_inline_read(fs, inode, offset, buffer, length);
		if (error == EXT4_OK) {
			*completed = length;
		}
		return error;
	}
	if (inode->fast_symlink) {
		if (inode->size > sizeof(inode->block_data)) {
			return EXT4_CORRUPT;
		}
		ext4_copy(buffer, inode->block_data + offset, length);
		*completed = length;
		return EXT4_OK;
	}
	return ext4_read_mapped(fs, inode, offset, buffer, length, false, completed);
}

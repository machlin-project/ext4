/* SPDX-License-Identifier: BSD-3-Clause */
#include "internal.h"
#include "fscrypt.h"
#include "inline.h"
#include "map_read.h"
#include "verity.h"

enum ext4_result
ext4_read_mapped(struct ext4_fs *fs, const struct ext4_inode *inode, uint64_t offset, void *buffer,
    size_t length, bool require_data, size_t *completed)
{
	struct ext4_map_reader reader = { 0 };
	enum ext4_result error;

	error = ext4_map_reader_read(
	    fs, inode, &reader, offset, buffer, length, require_data, completed);
	ext4_map_reader_close(fs, &reader);
	return error;
}

/* Decrypt an encrypted regular file's blocks. Holes and unwritten blocks read as
 * zeros, as in Linux; every other block is decrypted with the file's key and its
 * logical block number as the IV. */
static enum ext4_result
ext4_read_encrypted(struct ext4_fs *fs, const struct ext4_inode *inode,
    struct ext4_map_reader *reader, uint64_t offset, uint8_t *output, size_t length,
    bool require_data, size_t *completed)
{
	struct ext4_fscrypt_key key;
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
		error = logical > UINT32_MAX
		    ? EXT4_RANGE
		    : ext4_map_reader_next(fs, inode, reader, (uint32_t)logical, &physical, &run);
		if (error != EXT4_OK) {
			break;
		}
		if (physical == 0) {
			if (require_data) {
				error = EXT4_CORRUPT;
				break;
			}
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
	fs->environment.release(fs->environment.context, blocks, 2U * fs->info.block_size);
	return error;
}

/* Internal contents view: unlike ext4_read(), this neither clips at EOF nor
 * verifies verity. Metadata stored beyond EOF uses the same file-relative IVs
 * as ordinary contents, but must not turn absent mappings into plaintext zeros. */
enum ext4_result
ext4_read_plaintext(struct ext4_fs *fs, const struct ext4_inode *inode,
    struct ext4_map_reader *reader, uint64_t offset, void *buffer, size_t length,
    bool require_data, size_t *completed)
{
	*completed = 0;
	if (length > UINT64_MAX - offset) {
		return EXT4_RANGE;
	}
	if (inode->flags & EXT4_INODE_ENCRYPT) {
		if ((inode->mode & EXT4_MODE_TYPE) != EXT4_MODE_REGULAR ||
		    (inode->flags & EXT4_INODE_INLINE_DATA)) {
			return EXT4_CORRUPT;
		}
		return ext4_read_encrypted(
		    fs, inode, reader, offset, buffer, length, require_data, completed);
	}
	return ext4_map_reader_read(
	    fs, inode, reader, offset, buffer, length, require_data, completed);
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

static enum ext4_result
ext4_read_with_mapping(struct ext4_fs *fs, const struct ext4_inode *inode,
    struct ext4_map_reader *reader, uint64_t offset, void *buffer, size_t length, size_t *completed)
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
			return error == EXT4_OK
			    ? ext4_verity_read(fs, inode, offset, buffer, length, completed)
			    : error;
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
		return ext4_read_plaintext(
		    fs, inode, reader, offset, buffer, length, false, completed);
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
	return ext4_map_reader_read(fs, inode, reader, offset, buffer, length, false, completed);
}

enum ext4_result
ext4_read(struct ext4_fs *fs, const struct ext4_inode *inode, uint64_t offset, void *buffer,
    size_t length, size_t *completed)
{
	struct ext4_map_reader reader = { 0 };
	enum ext4_result error;

	error = ext4_read_with_mapping(fs, inode, &reader, offset, buffer, length, completed);
	/* Invalid arguments cannot have allocated mapping state. */
	if (fs != NULL) {
		ext4_map_reader_close(fs, &reader);
	}
	return error;
}

enum ext4_result
ext4_read_held(
    struct ext4_inode_hold *hold, uint64_t offset, void *buffer, size_t length, size_t *completed)
{
	struct ext4_read_state *reader;
	enum ext4_result error;

	if (completed == NULL) {
		return EXT4_INVALID_ARGUMENT;
	}
	*completed = 0;
	if (buffer == NULL && length != 0) {
		return EXT4_INVALID_ARGUMENT;
	}
	error = ext4_read_state_get(hold, &reader);
	if (error != EXT4_OK) {
		return error;
	}
	return ext4_read_with_mapping(
	    hold->fs, &reader->inode, &reader->mapping, offset, buffer, length, completed);
}

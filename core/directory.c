/* SPDX-License-Identifier: BSD-3-Clause */
#include "fscrypt.h"
#include "internal.h"
#include "inline.h"

uint32_t
ext4_directory_record_length(struct ext4_fs *fs, const struct ext4_dir_header_disk *header)
{
	uint32_t length = ext4_le16(&header->record_length);

	if (fs->info.block_size == EXT4_MAX_BLOCK_SIZE && (length == 0 || length == UINT16_MAX)) {
		return EXT4_MAX_BLOCK_SIZE;
	}
	return length;
}

enum ext4_result
ext4_directory_checksum(
    struct ext4_fs *fs, const struct ext4_inode *inode, uint32_t logical, uint8_t *buffer)
{
	struct ext4_dir_tail_disk *tail;
	struct ext4_dir_header_disk *first;
	struct ext4_dx_root_prefix_disk *root;
	struct ext4_dx_count_disk *counts;
	struct ext4_dx_tail_disk *dx_tail;
	size_t base;
	size_t tail_offset;
	uint16_t limit;
	uint16_t count;
	uint32_t checksum;
	uint32_t expected;
	bool index_block = false;

	if (!fs->metadata_checksum) {
		return EXT4_OK;
	}
	first = (struct ext4_dir_header_disk *)buffer;
	base = sizeof(*first);
	if (inode->flags & EXT4_INODE_INDEX) {
		if (logical == 0) {
			root = (struct ext4_dx_root_prefix_disk *)buffer;
			if (ext4_le32(&root->reserved) != 0 || root->info_length != 8 ||
			    root->indirect_levels > EXT4_DX_MAX_INDIRECT_LEVELS) {
				return EXT4_CORRUPT;
			}
			base = sizeof(*root);
			index_block = true;
		} else if (ext4_le32(&first->inode) == 0 && first->name_length == 0 &&
		    ext4_directory_record_length(fs, first) == fs->info.block_size) {
			index_block = true;
		}
	}
	if (!index_block) {
		tail = (struct ext4_dir_tail_disk *)(buffer + fs->info.block_size - sizeof(*tail));
		if (ext4_le32(&tail->reserved) != 0 || tail->zero != 0 ||
		    tail->type != EXT4_DIRECTORY_TAIL_TYPE ||
		    ext4_le16(&tail->record_length) != sizeof(*tail)) {
			return EXT4_CORRUPT;
		}
		checksum = ext4_crc32c(
		    ext4_inode_seed(fs, inode), buffer, fs->info.block_size - sizeof(*tail));
		return checksum == ext4_le32(&tail->checksum) ? EXT4_OK : EXT4_CORRUPT;
	}
	counts = (struct ext4_dx_count_disk *)(buffer + base);
	limit = ext4_le16(&counts->limit);
	count = ext4_le16(&counts->count);
	if (limit == 0 || count == 0 || count > limit ||
	    limit > (fs->info.block_size - base - sizeof(*dx_tail)) / sizeof(*counts)) {
		return EXT4_CORRUPT;
	}
	tail_offset = base + limit * sizeof(*counts);
	dx_tail = (struct ext4_dx_tail_disk *)(buffer + tail_offset);
	expected = ext4_le32(&dx_tail->checksum);
	ext4_zero(&dx_tail->checksum, sizeof(dx_tail->checksum));
	checksum = ext4_crc32c(ext4_inode_seed(fs, inode), buffer, base + count * sizeof(*counts));
	checksum = ext4_crc32c(checksum, dx_tail, sizeof(*dx_tail));
	ext4_encode32(&dx_tail->checksum, expected);
	return checksum == expected ? EXT4_OK : EXT4_CORRUPT;
}

enum ext4_result
ext4_directory_entry_decode(struct ext4_fs *fs, const uint8_t *buffer, uint32_t offset,
    bool ciphertext, struct ext4_dir_entry *entry, uint32_t *record_length)
{
	const struct ext4_dir_header_disk *header;
	uint32_t length;
	uint32_t number;
	uint16_t names;
	size_t index;
	bool checksum_tail;
	bool filetype = (fs->info.feature_incompat & EXT4_FEATURE_INCOMPAT_FILETYPE) != 0;

	if (offset > fs->info.block_size || fs->info.block_size - offset < sizeof(*header)) {
		return EXT4_CORRUPT;
	}
	header = (const struct ext4_dir_header_disk *)(buffer + offset);
	length = ext4_directory_record_length(fs, header);
	number = ext4_le32(&header->inode);
	names = header->name_length;
	checksum_tail = fs->metadata_checksum &&
	    offset == fs->info.block_size - sizeof(struct ext4_dir_tail_disk) && number == 0 &&
	    names == 0 && header->type == EXT4_DIRECTORY_TAIL_TYPE &&
	    length == sizeof(struct ext4_dir_tail_disk);
	/* A checksum tail retains its marker byte without the FILETYPE feature. */
	if (!filetype && !checksum_tail) {
		names |= (uint16_t)((uint16_t)header->type << 8);
	}
	if (length < sizeof(*header) || length % EXT4_DIRECTORY_ALIGNMENT != 0 ||
	    length > fs->info.block_size - offset || names > length - sizeof(*header) ||
	    names > EXT4_NAME_MAX || number > fs->info.inodes) {
		return EXT4_CORRUPT;
	}
	if (number != 0) {
		if (names == 0 || (filetype && header->type > EXT4_FT_SYMLINK)) {
			return EXT4_CORRUPT;
		}
		for (index = 0; !ciphertext && index < names; index++) {
			if (buffer[offset + sizeof(*header) + index] == 0 ||
			    buffer[offset + sizeof(*header) + index] == '/') {
				return EXT4_CORRUPT;
			}
		}
		ext4_copy(entry->name, buffer + offset + sizeof(*header), names);
		entry->name[names] = 0;
	}
	entry->inode = number;
	entry->name_length = names;
	entry->type = filetype ? (enum ext4_file_type)header->type : EXT4_FT_UNKNOWN;
	*record_length = length;
	return EXT4_OK;
}

static enum ext4_result
ext4_directory_block_validate(
    struct ext4_fs *fs, const uint8_t *buffer, uint32_t wanted, bool ciphertext)
{
	struct ext4_dir_entry entry;
	uint32_t offset = 0;
	uint32_t length;
	enum ext4_result error;

	while (offset < fs->info.block_size) {
		error =
		    ext4_directory_entry_decode(fs, buffer, offset, ciphertext, &entry, &length);
		if (error != EXT4_OK) {
			return error;
		}
		if (offset < wanted && offset + length > wanted) {
			return EXT4_CORRUPT;
		}
		offset += length;
	}
	return EXT4_OK;
}

enum ext4_result
ext4_iterate_dir(struct ext4_fs *fs, const struct ext4_inode *directory, uint64_t *cookie,
    enum ext4_dir_action (*visit)(
	void *context, const struct ext4_dir_entry *entry, uint64_t next_cookie),
    void *context)
{
	struct ext4_dir_entry decoded;
	struct ext4_fscrypt_key key;
	uint8_t plain[EXT4_NAME_MAX];
	uint8_t *buffer;
	uint64_t block_offset;
	uint64_t size;
	uint32_t inline_used;
	uint32_t inline_tail;
	uint32_t inline_hash;
	size_t completed;
	uint32_t offset;
	uint32_t wanted;
	uint32_t record_length;
	size_t plain_length;
	enum ext4_result error;
	enum ext4_dir_action action;

	if (fs == NULL || directory == NULL || cookie == NULL || visit == NULL) {
		return EXT4_INVALID_ARGUMENT;
	}
	if (fs->aborted) {
		return EXT4_RECOVERY_REQUIRED;
	}
	if ((directory->mode & EXT4_MODE_TYPE) != EXT4_MODE_DIRECTORY) {
		return EXT4_NOT_DIRECTORY;
	}
	/* Encrypted names can be neither presented nor resumed without a key. */
	if (directory->flags & EXT4_INODE_ENCRYPT) {
		error = ext4_fscrypt_key(fs, directory, &key);
		if (error != EXT4_OK) {
			return error;
		}
	}
	size = directory->flags & EXT4_INODE_INLINE_DATA ? fs->info.block_size : directory->size;
	if (size % fs->info.block_size != 0 || *cookie > size) {
		return EXT4_CORRUPT;
	}
	if (size / fs->info.block_size > (uint64_t)UINT32_MAX + 1U) {
		return EXT4_RANGE;
	}
	if (*cookie == size) {
		return EXT4_NOT_FOUND;
	}
	buffer = fs->environment.allocate(fs->environment.context, fs->info.block_size);
	if (buffer == NULL) {
		return EXT4_NO_MEMORY;
	}
	error = EXT4_NOT_FOUND;
	while (*cookie < size) {
		wanted = (uint32_t)(*cookie % fs->info.block_size);
		block_offset = *cookie - wanted;
		if (directory->flags & EXT4_INODE_INLINE_DATA) {
			error = ext4_inline_directory(fs, directory, NULL, buffer, true,
			    &inline_used, &inline_tail, &inline_hash);
			completed = fs->info.block_size;
		} else {
			error = ext4_read(
			    fs, directory, block_offset, buffer, fs->info.block_size, &completed);
		}
		if (error != EXT4_OK) {
			goto out;
		}
		if (completed != fs->info.block_size) {
			error = EXT4_CORRUPT;
			goto out;
		}
		error = ext4_directory_checksum(
		    fs, directory, (uint32_t)(block_offset / fs->info.block_size), buffer);
		if (error == EXT4_OK) {
			error = ext4_directory_block_validate(
			    fs, buffer, wanted, (directory->flags & EXT4_INODE_ENCRYPT) != 0);
		}
		if (error != EXT4_OK) {
			goto out;
		}
		offset = wanted;
		while (offset < fs->info.block_size) {
			error = ext4_directory_entry_decode(fs, buffer, offset,
			    (directory->flags & EXT4_INODE_ENCRYPT) != 0, &decoded, &record_length);
			if (error != EXT4_OK) {
				goto out;
			}
			offset += record_length;
			/* Dot entries are stored unencrypted; other names are ciphertext. */
			if (decoded.inode != 0 && (directory->flags & EXT4_INODE_ENCRYPT) &&
			    !ext4_fscrypt_dot(decoded.name, decoded.name_length)) {
				error = ext4_fscrypt_name_decrypt(fs, &key, decoded.name,
				    decoded.name_length, EXT4_NAME_MAX, plain, &plain_length);
				if (error != EXT4_OK) {
					goto out;
				}
				ext4_copy(decoded.name, plain, plain_length);
				decoded.name[plain_length] = 0;
				decoded.name_length = (uint16_t)plain_length;
			}
			if (decoded.inode != 0) {
				action = visit(context, &decoded, block_offset + offset);
				if (action == EXT4_DIR_STOP) {
					error = EXT4_OK;
					goto out;
				}
				if (action != EXT4_DIR_ACCEPT && action != EXT4_DIR_ACCEPT_STOP) {
					error = EXT4_INVALID_ARGUMENT;
					goto out;
				}
				*cookie = block_offset + offset;
				if (action == EXT4_DIR_ACCEPT_STOP) {
					error = EXT4_OK;
					goto out;
				}
			} else {
				*cookie = block_offset + offset;
			}
		}
		error = EXT4_NOT_FOUND;
	}
out:
	fs->environment.release(fs->environment.context, buffer, fs->info.block_size);
	return error;
}

static enum ext4_dir_action
ext4_directory_one(void *context, const struct ext4_dir_entry *entry, uint64_t next_cookie)
{
	struct ext4_dir_entry *output = context;

	(void)next_cookie;
	output->inode = entry->inode;
	output->type = entry->type;
	output->name_length = entry->name_length;
	ext4_copy(output->name, entry->name, entry->name_length + 1U);
	return EXT4_DIR_ACCEPT_STOP;
}

enum ext4_result
ext4_next_dir(struct ext4_fs *fs, const struct ext4_inode *directory, uint64_t *cookie,
    struct ext4_dir_entry *entry)
{
	if (entry == NULL) {
		return EXT4_INVALID_ARGUMENT;
	}
	return ext4_iterate_dir(fs, directory, cookie, ext4_directory_one, entry);
}

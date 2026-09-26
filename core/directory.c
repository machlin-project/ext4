/* SPDX-License-Identifier: BSD-3-Clause */
#include "internal.h"

static uint32_t
ext4_directory_record_length(struct ext4_fs *fs, const struct ext4_dir_header_disk *header)
{
	uint32_t length = ext4_le16(&header->record_length);

	if (fs->info.block_size == EXT4_MAX_BLOCK_SIZE && (length == 0 || length == UINT16_MAX)) {
		return EXT4_MAX_BLOCK_SIZE;
	}
	return length;
}

static enum ext4_result
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
			    root->indirect_levels > 2) {
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
	return checksum == expected ? EXT4_OK : EXT4_CORRUPT;
}

enum ext4_result
ext4_next_dir(struct ext4_fs *fs, const struct ext4_inode *directory, uint64_t *cookie,
    struct ext4_dir_entry *entry)
{
	struct ext4_dir_header_disk *header;
	uint8_t *buffer;
	uint64_t block_offset;
	size_t offset;
	size_t wanted;
	size_t completed;
	size_t index;
	uint32_t record_length;
	uint32_t number;
	uint16_t name_length;
	enum ext4_result error;

	if (fs == NULL || directory == NULL || cookie == NULL || entry == NULL) {
		return EXT4_INVALID_ARGUMENT;
	}
	if ((directory->mode & EXT4_MODE_TYPE) != EXT4_MODE_DIRECTORY) {
		return EXT4_NOT_DIRECTORY;
	}
	if (directory->size % fs->info.block_size != 0 || *cookie > directory->size) {
		return EXT4_CORRUPT;
	}
	buffer = fs->environment.allocate(fs->environment.context, fs->info.block_size);
	if (buffer == NULL) {
		return EXT4_NO_MEMORY;
	}
	error = EXT4_NOT_FOUND;
	while (*cookie < directory->size) {
		wanted = (size_t)(*cookie % fs->info.block_size);
		block_offset = *cookie - wanted;
		error =
		    ext4_read(fs, directory, block_offset, buffer, fs->info.block_size, &completed);
		if (error != EXT4_OK) {
			goto out;
		}
		if (completed != fs->info.block_size) {
			error = EXT4_CORRUPT;
			goto out;
		}
		error = ext4_directory_checksum(
		    fs, directory, (uint32_t)(block_offset / fs->info.block_size), buffer);
		if (error != EXT4_OK) {
			goto out;
		}
		offset = 0;
		while (offset < fs->info.block_size) {
			if (fs->info.block_size - offset < sizeof(*header)) {
				error = EXT4_CORRUPT;
				goto out;
			}
			header = (struct ext4_dir_header_disk *)(buffer + offset);
			record_length = ext4_directory_record_length(fs, header);
			name_length = header->name_length;
			if (!(fs->info.feature_incompat & EXT4_FEATURE_INCOMPAT_FILETYPE)) {
				name_length |= (uint16_t)((uint16_t)header->type << 8);
			}
			number = ext4_le32(&header->inode);
			if (record_length < sizeof(*header) || (record_length & 3U) ||
			    record_length > fs->info.block_size - offset ||
			    name_length > record_length - sizeof(*header) ||
			    name_length > EXT4_NAME_MAX || number > fs->info.inodes ||
			    (offset < wanted && offset + record_length > wanted)) {
				error = EXT4_CORRUPT;
				goto out;
			}
			if (offset >= wanted) {
				*cookie = block_offset + offset + record_length;
				if (number != 0) {
					if (name_length == 0 ||
					    ((fs->info.feature_incompat &
						 EXT4_FEATURE_INCOMPAT_FILETYPE) &&
						header->type > EXT4_FT_SYMLINK)) {
						error = EXT4_CORRUPT;
						goto out;
					}
					for (index = 0; index < name_length; index++) {
						if (buffer[offset + sizeof(*header) + index] == 0 ||
						    buffer[offset + sizeof(*header) + index] ==
							'/') {
							error = EXT4_CORRUPT;
							goto out;
						}
					}
					entry->inode = number;
					entry->type = (fs->info.feature_incompat &
							  EXT4_FEATURE_INCOMPAT_FILETYPE)
					    ? (enum ext4_file_type)header->type
					    : EXT4_FT_UNKNOWN;
					entry->name_length = name_length;
					ext4_copy(entry->name, buffer + offset + sizeof(*header),
					    name_length);
					entry->name[name_length] = 0;
					error = EXT4_OK;
					goto out;
				}
			}
			offset += record_length;
		}
		error = EXT4_NOT_FOUND;
	}
out:
	fs->environment.release(fs->environment.context, buffer, fs->info.block_size);
	return error;
}

enum ext4_result
ext4_lookup(struct ext4_fs *fs, const struct ext4_inode *directory, const uint8_t *name,
    size_t name_length, struct ext4_inode *inode)
{
	struct ext4_dir_entry entry;
	uint64_t cookie = 0;
	size_t index;
	enum ext4_result error;

	if (name == NULL || inode == NULL || name_length == 0) {
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
	for (;;) {
		error = ext4_next_dir(fs, directory, &cookie, &entry);
		if (error != EXT4_OK) {
			return error;
		}
		if (name_length == entry.name_length && ext4_equal(name, entry.name, name_length)) {
			return ext4_get_inode(fs, entry.inode, inode);
		}
	}
}

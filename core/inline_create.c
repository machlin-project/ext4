/* SPDX-License-Identifier: BSD-3-Clause */
#include "inline.h"
#include "allocate.h"

enum ext4_result
ext4_inline_start(struct ext4_allocation *allocation, struct ext4_inode *inode,
    struct ext4_inode_disk *disk, uint32_t parent, bool *created)
{
	struct ext4_fs *fs = allocation->fs;
	struct ext4_xattr_change change = { 0 };
	struct ext4_dir_header_disk *entry;
	uint32_t minimum = EXT4_INODE_BASE_SIZE + 2U * sizeof(struct ext4_le32) +
	    sizeof(struct ext4_xattr_entry_disk) + 4U;
	enum ext4_result error;

	*created = false;
	if (!(fs->info.feature_incompat & EXT4_FEATURE_INCOMPAT_INLINE_DATA) ||
	    fs->inode_size <= EXT4_INODE_BASE_SIZE || ext4_le16(&disk->extra_size) == 0 ||
	    fs->inode_size < minimum + ext4_le16(&disk->extra_size) ||
	    ext4_inode_has_xattrs(fs, disk)) {
		return EXT4_OK;
	}
	change.policy = EXT4_XATTR_CREATE;
	change.name_index = EXT4_XATTR_SYSTEM;
	change.name = (const uint8_t *)"data";
	change.name_length = 4;
	error = ext4_xattr_apply(allocation, inode, disk, &change, 1);
	if (error != EXT4_OK) {
		return error;
	}
	inode->flags &= ~(uint32_t)EXT4_INODE_EXTENTS;
	inode->flags |= EXT4_INODE_INLINE_DATA;
	ext4_encode32(&disk->flags, inode->flags);
	ext4_zero(disk->block_data, sizeof(disk->block_data));
	if (parent != 0) {
		ext4_encode32((struct ext4_le32 *)disk->block_data, parent);
		entry = (struct ext4_dir_header_disk *)(disk->block_data + EXT4_INLINE_PARENT_SIZE);
		ext4_encode16(
		    &entry->record_length, EXT4_INODE_BLOCK_BYTES - EXT4_INLINE_PARENT_SIZE);
		inode->size = EXT4_INODE_BLOCK_BYTES;
		ext4_encode32(&disk->size_lo, EXT4_INODE_BLOCK_BYTES);
	}
	ext4_copy(inode->block_data, disk->block_data, sizeof(inode->block_data));
	*created = true;
	return EXT4_OK;
}

enum ext4_result
ext4_inline_grow(struct ext4_allocation *allocation, struct ext4_inode *inode,
    struct ext4_inode_disk *disk, uint64_t minimum, bool *grown)
{
	struct ext4_fs *fs = allocation->fs;
	struct ext4_inline_view view;
	struct ext4_xattr_change change = { 0 };
	const struct ext4_xattr_record *record;
	struct ext4_dir_header_disk *entry;
	uint8_t *value = NULL;
	uint32_t capacity;
	uint32_t cost;
	uint32_t position;
	uint32_t length;
	size_t index;
	bool directory = (inode->mode & EXT4_MODE_TYPE) == EXT4_MODE_DIRECTORY;
	enum ext4_result error;

	*grown = false;
	error = ext4_inline_open(fs, inode, disk, &view);
	if (error != EXT4_OK) {
		return error;
	}
	capacity = fs->inode_size - EXT4_INODE_BASE_SIZE - ext4_le16(&disk->extra_size) -
	    2U * sizeof(struct ext4_le32);
	for (index = 0; index < view.attributes.count; index++) {
		record = &view.attributes.records[index];
		if (record->external) {
			continue;
		}
		cost = (sizeof(*record->entry) + record->entry->name_length + EXT4_XATTR_ALIGNMENT -
			   1U) &
		    ~(EXT4_XATTR_ALIGNMENT - 1U);
		if (!record->inode_storage &&
		    !ext4_inline_key(record->entry->name_index,
			(const uint8_t *)(record->entry + 1), record->entry->name_length)) {
			cost +=
			    (ext4_le32(&record->entry->value_size) + EXT4_XATTR_ALIGNMENT - 1U) &
			    ~(EXT4_XATTR_ALIGNMENT - 1U);
		}
		if (cost > capacity) {
			error = EXT4_CORRUPT;
			goto out;
		}
		capacity -= cost;
	}
	if (capacity <= view.tail_size || minimum > EXT4_INODE_BLOCK_BYTES + capacity ||
	    (directory && capacity < sizeof(*entry))) {
		goto out;
	}
	value = fs->environment.allocate(fs->environment.context, capacity);
	if (value == NULL) {
		error = EXT4_NO_MEMORY;
		goto out;
	}
	ext4_zero(value, capacity);
	if (view.tail_size != 0) {
		ext4_copy(value, view.tail, view.tail_size);
	}
	if (directory) {
		position = 0;
		if (view.tail_size != 0) {
			for (;;) {
				if (view.tail_size - position < sizeof(*entry)) {
					error = EXT4_CORRUPT;
					goto out;
				}
				entry = (struct ext4_dir_header_disk *)(value + position);
				length = ext4_le16(&entry->record_length);
				if (length < sizeof(*entry) ||
				    length % EXT4_DIRECTORY_ALIGNMENT != 0 ||
				    length > view.tail_size - position) {
					error = EXT4_CORRUPT;
					goto out;
				}
				if (position + length == view.tail_size) {
					break;
				}
				position += length;
			}
		}
		entry = (struct ext4_dir_header_disk *)(value + position);
		ext4_encode16(&entry->record_length, (uint16_t)(capacity - position));
	}
	change.policy = EXT4_XATTR_REPLACE;
	change.name_index = EXT4_XATTR_SYSTEM;
	change.name = (const uint8_t *)"data";
	change.name_length = 4;
	change.value = value;
	change.value_size = capacity;
	error = ext4_xattr_apply(allocation, inode, disk, &change, 1);
	if (error == EXT4_OK) {
		if (directory) {
			inode->size = EXT4_INODE_BLOCK_BYTES + capacity;
			ext4_encode32(&disk->size_lo, (uint32_t)inode->size);
		}
		*grown = true;
	}
out:
	if (value != NULL) {
		fs->environment.release(fs->environment.context, value, capacity);
	}
	ext4_inline_close(&view);
	return error;
}

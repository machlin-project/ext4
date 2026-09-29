/* SPDX-License-Identifier: BSD-3-Clause */
#include "inline.h"
#include "allocate.h"

bool
ext4_inline_key(uint8_t index, const uint8_t *name, size_t length)
{
	return index == EXT4_XATTR_SYSTEM && length == 4 &&
	    ext4_equal(name, (const uint8_t *)"data", 4);
}

void
ext4_inline_close(struct ext4_inline_view *view)
{
	if (view->attributes.fs != NULL) {
		ext4_xattr_close(&view->attributes);
	}
	ext4_zero(view, sizeof(*view));
}

enum ext4_result
ext4_inline_open(struct ext4_fs *fs, const struct ext4_inode *inode,
    const struct ext4_inode_disk *disk, struct ext4_inline_view *view)
{
	const struct ext4_xattr_record *record = NULL;
	const struct ext4_inode_disk *saved;
	uint64_t blocks;
	uint64_t size;
	uint16_t type = inode->mode & EXT4_MODE_TYPE;
	size_t index;
	enum ext4_result error;

	ext4_zero(view, sizeof(*view));
	if (!(fs->info.feature_incompat & EXT4_FEATURE_INCOMPAT_INLINE_DATA) ||
	    !(inode->flags & EXT4_INODE_INLINE_DATA) ||
	    (inode->flags & (EXT4_INODE_EXTENTS | EXT4_INODE_INDEX | EXT4_INODE_EA_INODE)) ||
	    (type != EXT4_MODE_REGULAR && type != EXT4_MODE_DIRECTORY)) {
		return EXT4_CORRUPT;
	}
	error = disk == NULL
	    ? ext4_xattr_open(fs, inode->number, inode->generation, &view->attributes)
	    : ext4_xattr_open_inode(fs, inode, disk, &view->attributes);
	if (error != EXT4_OK) {
		ext4_inline_close(view);
		return error;
	}
	saved = (const struct ext4_inode_disk *)view->attributes.inode;
	size = ext4_le32(&saved->size_lo) | ((uint64_t)ext4_le32(&saved->size_hi) << 32);
	if (!(ext4_le32(&saved->flags) & EXT4_INODE_INLINE_DATA) || size != inode->size) {
		error = EXT4_STALE;
		goto fail;
	}
	for (index = 0; index < view->attributes.count; index++) {
		record = &view->attributes.records[index];
		if (ext4_inline_key(record->entry->name_index, (const uint8_t *)(record->entry + 1),
			record->entry->name_length)) {
			break;
		}
	}
	if (index == view->attributes.count || record->external || record->inode_storage) {
		error = EXT4_CORRUPT;
		goto fail;
	}
	view->hash_offset =
	    (uint32_t)((const uint8_t *)&record->entry->hash - view->attributes.inode);
	view->head = saved->block_data;
	view->tail = record->value;
	view->tail_size = ext4_le32(&record->entry->value_size);
	view->tail_offset =
	    view->tail_size == 0 ? 0 : (uint32_t)(view->tail - view->attributes.inode);
	blocks = ext4_xattr_value_blocks(&view->attributes) +
	    (view->attributes.external_block == 0 ? 0 : fs->cluster_blocks);
	if (inode->blocks_512 != blocks * (fs->info.block_size / EXT4_SECTOR_SIZE) ||
	    size > EXT4_INODE_BLOCK_BYTES + view->tail_size ||
	    (type == EXT4_MODE_DIRECTORY &&
		((size != EXT4_INODE_BLOCK_BYTES + view->tail_size && inode->links != 0) ||
		    (view->tail_size != 0 &&
			(view->tail_size < sizeof(struct ext4_dir_header_disk) ||
			    view->tail_size % EXT4_DIRECTORY_ALIGNMENT != 0))))) {
		error = EXT4_CORRUPT;
		goto fail;
	}
	return EXT4_OK;
fail:
	ext4_inline_close(view);
	return error;
}

enum ext4_result
ext4_inline_validate(
    struct ext4_fs *fs, const struct ext4_inode *inode, const struct ext4_inode_disk *disk)
{
	struct ext4_inline_view view;
	enum ext4_result error;

	error = ext4_inline_open(fs, inode, disk, &view);
	ext4_inline_close(&view);
	return error;
}

static void
ext4_inline_copy(const struct ext4_inline_view *view, uint64_t offset, void *buffer, size_t length)
{
	size_t first =
	    offset < EXT4_INODE_BLOCK_BYTES ? EXT4_INODE_BLOCK_BYTES - (size_t)offset : 0;

	if (first > length) {
		first = length;
	}
	if (first != 0) {
		ext4_copy(buffer, view->head + offset, first);
		offset += first;
	}
	if (length > first) {
		ext4_copy((uint8_t *)buffer + first, view->tail + offset - EXT4_INODE_BLOCK_BYTES,
		    length - first);
	}
}

enum ext4_result
ext4_inline_read(struct ext4_fs *fs, const struct ext4_inode *inode, uint64_t offset, void *buffer,
    size_t length)
{
	struct ext4_inline_view view;
	enum ext4_result error;

	error = ext4_inline_open(fs, inode, NULL, &view);
	if (error == EXT4_OK) {
		if (offset > inode->size || length > inode->size - offset) {
			error = EXT4_RANGE;
		} else {
			ext4_inline_copy(&view, offset, buffer, length);
		}
	}
	ext4_inline_close(&view);
	return error;
}

static void
ext4_inline_dot(struct ext4_fs *fs, uint8_t *buffer, uint32_t number, uint8_t length)
{
	struct ext4_dir_header_disk *entry = (struct ext4_dir_header_disk *)buffer;

	ext4_encode32(&entry->inode, number);
	ext4_encode16(&entry->record_length, EXT4_INLINE_DOT_SIZE);
	entry->name_length = length;
	entry->type =
	    fs->info.feature_incompat & EXT4_FEATURE_INCOMPAT_FILETYPE ? EXT4_FT_DIRECTORY : 0;
	ext4_copy(entry + 1, "..", length);
}

static enum ext4_result
ext4_inline_region(
    struct ext4_fs *fs, const uint8_t *buffer, uint32_t start, uint32_t end, uint32_t *last)
{
	struct ext4_dir_entry entry;
	uint32_t offset = start;
	uint32_t length;
	enum ext4_result error;

	while (offset < end) {
		error = ext4_directory_entry_decode(fs, buffer, offset, false, &entry, &length);
		if (error != EXT4_OK || length > end - offset ||
		    (entry.inode != 0 && entry.name[0] == '.' &&
			(entry.name_length == 1 ||
			    (entry.name_length == 2 && entry.name[1] == '.')))) {
			return EXT4_CORRUPT;
		}
		*last = offset;
		offset += length;
	}
	return EXT4_OK;
}

enum ext4_result
ext4_inline_directory(struct ext4_fs *fs, const struct ext4_inode *inode,
    const struct ext4_inode_disk *disk, uint8_t *buffer, bool pad, uint32_t *used,
    uint32_t *tail_offset, uint32_t *hash_offset)
{
	struct ext4_inline_view view;
	struct ext4_dir_header_disk *last_entry;
	struct ext4_dir_tail_disk *tail;
	uint32_t last = 0;
	uint32_t parent;
	uint32_t end;
	uint32_t usable = fs->info.block_size - (fs->metadata_checksum ? sizeof(*tail) : 0);
	enum ext4_result error;

	error = ext4_inline_open(fs, inode, disk, &view);
	if (error != EXT4_OK) {
		return error;
	}
	parent = ext4_le32((const struct ext4_le32 *)view.head);
	end = EXT4_INLINE_FIRST_END + view.tail_size;
	if ((inode->mode & EXT4_MODE_TYPE) != EXT4_MODE_DIRECTORY || parent == 0 ||
	    parent > fs->info.inodes ||
	    (inode->number == EXT4_ROOT_INODE && parent != inode->number) || end > usable) {
		error = EXT4_CORRUPT;
		goto out;
	}
	ext4_zero(buffer, fs->info.block_size);
	ext4_inline_dot(fs, buffer, inode->number, 1);
	ext4_inline_dot(fs, buffer + EXT4_INLINE_DOT_SIZE, parent, 2);
	ext4_copy(buffer + EXT4_INLINE_DOTS_SIZE, view.head + EXT4_INLINE_PARENT_SIZE,
	    EXT4_INODE_BLOCK_BYTES - EXT4_INLINE_PARENT_SIZE);
	if (view.tail_size != 0) {
		ext4_copy(buffer + EXT4_INLINE_FIRST_END, view.tail, view.tail_size);
	}
	error = ext4_inline_region(fs, buffer, EXT4_INLINE_DOTS_SIZE, EXT4_INLINE_FIRST_END, &last);
	if (error == EXT4_OK) {
		error = ext4_inline_region(fs, buffer, EXT4_INLINE_FIRST_END, end, &last);
	}
	if (error != EXT4_OK) {
		goto out;
	}
	if (pad) {
		last_entry = (struct ext4_dir_header_disk *)(buffer + last);
		ext4_encode16(&last_entry->record_length, (uint16_t)(usable - last));
		if (fs->metadata_checksum) {
			tail = (struct ext4_dir_tail_disk *)(buffer + usable);
			ext4_encode16(&tail->record_length, sizeof(*tail));
			tail->type = EXT4_DIRECTORY_TAIL_TYPE;
			ext4_encode32(&tail->checksum,
			    ext4_crc32c(ext4_inode_seed(fs, inode), buffer, usable));
		}
	}
	*used = end;
	*tail_offset = view.tail_offset;
	*hash_offset = view.hash_offset;
out:
	ext4_inline_close(&view);
	return error;
}

enum ext4_result
ext4_inline_expand(
    struct ext4_allocation *allocation, struct ext4_inode *inode, struct ext4_inode_disk *disk)
{
	struct ext4_fs *fs = allocation->fs;
	struct ext4_allocation accounting;
	struct ext4_inline_view view;
	struct ext4_xattr_change change;
	struct ext4_extent_header_disk *header;
	void *target = NULL;
	uint8_t *buffer;
	uint64_t physical;
	uint64_t size = inode->size;
	uint32_t used;
	uint32_t tail_offset;
	uint32_t hash_offset;
	bool zero;
	bool directory = (inode->mode & EXT4_MODE_TYPE) == EXT4_MODE_DIRECTORY;
	enum ext4_result error;

	if (!(inode->flags & EXT4_INODE_INLINE_DATA)) {
		return EXT4_OK;
	}
	buffer = fs->environment.allocate(fs->environment.context, fs->info.block_size);
	if (buffer == NULL) {
		return EXT4_NO_MEMORY;
	}
	if (directory) {
		error = ext4_inline_directory(
		    fs, inode, disk, buffer, true, &used, &tail_offset, &hash_offset);
		size = fs->info.block_size;
	} else {
		error = ext4_inline_open(fs, inode, disk, &view);
		if (error == EXT4_OK) {
			ext4_zero(buffer, fs->info.block_size);
			ext4_inline_copy(&view, 0, buffer, (size_t)size);
		}
		ext4_inline_close(&view);
	}
	if (error != EXT4_OK) {
		goto out;
	}
	/* Attribute repacking can change allocation too. Isolate all conversion
	 * charges so callers operating on several namespace objects cannot leak
	 * this inode's data or attribute charges into the next object. */
	accounting = *allocation;
	allocation->allocated = 0;
	allocation->freed = 0;
	allocation->unmapped = 0;
	allocation->detached_shared_blocks = 0;
	allocation->attribute_blocks_added = 0;
	allocation->attribute_blocks_removed = 0;
	allocation->mapping_size = size;
	inode->flags &= ~(uint32_t)EXT4_INODE_INLINE_DATA;
	if (fs->info.feature_incompat & EXT4_FEATURE_INCOMPAT_EXTENTS) {
		inode->flags |= EXT4_INODE_EXTENTS;
	}
	ext4_encode32(&disk->flags, inode->flags);
	ext4_zero(&change, sizeof(change));
	change.policy = EXT4_XATTR_REMOVE;
	change.name_index = EXT4_XATTR_SYSTEM;
	change.name = (const uint8_t *)"data";
	change.name_length = 4;
	error = ext4_xattr_apply(allocation, inode, disk, &change, 1);
	if (error == EXT4_OK) {
		ext4_zero(disk->block_data, sizeof(disk->block_data));
		if (inode->flags & EXT4_INODE_EXTENTS) {
			header = (struct ext4_extent_header_disk *)disk->block_data;
			ext4_encode16(&header->magic, EXT4_EXTENT_MAGIC);
			ext4_encode16(&header->maximum,
			    (sizeof(disk->block_data) - sizeof(*header)) /
				sizeof(struct ext4_extent_disk));
		}
		if (size != 0) {
			error =
			    ext4_write_map_allocate(allocation, inode, disk, 0, &physical, &zero);
			if (error == EXT4_OK) {
				error = ext4_transaction_buffer(
				    allocation->transaction, physical, &target);
			}
			if (error == EXT4_OK) {
				ext4_copy(target, buffer, fs->info.block_size);
			}
		}
	}
	if (error == EXT4_OK) {
		error = ext4_inode_account(allocation, inode, disk, size);
	}
	if (error == EXT4_OK) {
		ext4_inode_checksum_set(fs, inode->number, disk);
		error = ext4_inode_decode_orphan(fs, inode->number, disk, inode);
	}
	allocation->allocated = accounting.allocated;
	allocation->freed = accounting.freed;
	allocation->unmapped = accounting.unmapped;
	allocation->detached_shared_blocks = accounting.detached_shared_blocks;
	allocation->attribute_blocks_added = accounting.attribute_blocks_added;
	allocation->attribute_blocks_removed = accounting.attribute_blocks_removed;
	allocation->mapping_size = accounting.mapping_size;
out:
	fs->environment.release(fs->environment.context, buffer, fs->info.block_size);
	return error;
}

enum ext4_result
ext4_inline_edit(struct ext4_allocation *allocation, struct ext4_inode *inode,
    struct ext4_inode_disk *disk, uint64_t size, uint64_t offset, const void *buffer, size_t length,
    bool *handled)
{
	struct ext4_inline_view view;
	uint8_t *destination;
	uint64_t position;
	uint64_t capacity;
	size_t amount;
	size_t copied = 0;
	bool grown;
	enum ext4_result error;

	*handled = false;
	if (!(inode->flags & EXT4_INODE_INLINE_DATA)) {
		return EXT4_OK;
	}
	error = ext4_inline_open(allocation->fs, inode, disk, &view);
	if (error != EXT4_OK) {
		return error;
	}
	capacity = EXT4_INODE_BLOCK_BYTES + view.tail_size;
	if (size > capacity) {
		ext4_inline_close(&view);
		error = ext4_inline_grow(allocation, inode, disk, size, &grown);
		if (error != EXT4_OK) {
			return error;
		}
		if (!grown) {
			return ext4_inline_expand(allocation, inode, disk);
		}
		error = ext4_inline_open(allocation->fs, inode, disk, &view);
		if (error != EXT4_OK) {
			return error;
		}
		capacity = EXT4_INODE_BLOCK_BYTES + view.tail_size;
	}
	if (offset > size || length > size - offset) {
		ext4_inline_close(&view);
		return EXT4_RANGE;
	}
	position = size < inode->size ? size : inode->size;
	while (position < capacity) {
		if (position < EXT4_INODE_BLOCK_BYTES) {
			destination = disk->block_data + position;
			amount = EXT4_INODE_BLOCK_BYTES - (size_t)position;
		} else {
			destination =
			    (uint8_t *)disk + view.tail_offset + position - EXT4_INODE_BLOCK_BYTES;
			amount = (size_t)(capacity - position);
		}
		ext4_zero(destination, amount);
		position += amount;
	}
	position = offset;
	while (copied < length) {
		if (position < EXT4_INODE_BLOCK_BYTES) {
			destination = disk->block_data + position;
			amount = EXT4_INODE_BLOCK_BYTES - (size_t)position;
		} else {
			destination =
			    (uint8_t *)disk + view.tail_offset + position - EXT4_INODE_BLOCK_BYTES;
			amount = (size_t)(capacity - position);
		}
		if (amount > length - copied) {
			amount = length - copied;
		}
		if (buffer == NULL) {
			ext4_zero(destination, amount);
		} else {
			ext4_copy(destination, (const uint8_t *)buffer + copied, amount);
		}
		position += amount;
		copied += amount;
	}
	ext4_zero((uint8_t *)disk + view.hash_offset, sizeof(struct ext4_le32));
	inode->size = size;
	ext4_encode32(&disk->size_lo, (uint32_t)size);
	ext4_encode32(&disk->size_hi, 0);
	*handled = true;
	ext4_inline_close(&view);
	return EXT4_OK;
}

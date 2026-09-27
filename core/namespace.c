/* SPDX-License-Identifier: BSD-3-Clause */
#include "allocate.h"

#define EXT4_DIRECTORY_MAX_BLOCKS (1U << 20)
#define EXT4_CREATE_FIELDS                                                                         \
	(EXT4_ATTR_PERMISSIONS | EXT4_ATTR_UID | EXT4_ATTR_GID | EXT4_ATTR_ACCESS_TIME |           \
	    EXT4_ATTR_MODIFY_TIME | EXT4_ATTR_CHANGE_TIME)
#define EXT4_INHERITED_FILE_FLAGS                                                                  \
	(EXT4_INODE_SYNC | EXT4_INODE_NODUMP | EXT4_INODE_NOATIME | EXT4_INODE_NOTAIL |            \
	    EXT4_INODE_JOURNAL_DATA)

struct ext4_directory_slot {
	uint64_t physical;
	uint32_t logical;
	uint32_t offset;
	uint32_t used;
	uint32_t length;
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

static enum ext4_result
ext4_namespace_name(const uint8_t *name, size_t length)
{
	size_t index;

	if (name == NULL || length == 0 || (length == 1 && name[0] == '.') ||
	    (length == 2 && name[0] == '.' && name[1] == '.')) {
		return EXT4_INVALID_ARGUMENT;
	}
	if (length > EXT4_NAME_MAX) {
		return EXT4_NAME_TOO_LONG;
	}
	for (index = 0; index < length; index++) {
		if (name[index] == 0 || name[index] == '/') {
			return EXT4_INVALID_ARGUMENT;
		}
	}
	return EXT4_OK;
}

static enum ext4_result
ext4_directory_space(struct ext4_allocation *allocation, const struct ext4_inode *parent,
    struct ext4_inode_disk *disk, const uint8_t *name, size_t name_length,
    struct ext4_directory_slot *slot)
{
	struct ext4_fs *fs = allocation->fs;
	struct ext4_map_run run;
	struct ext4_dir_header_disk *entry;
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
	uint16_t names;
	size_t index;
	bool exists = false;
	bool dot;
	bool dotdot;
	enum ext4_result error;

	if (parent->flags & EXT4_INODE_INDEX) {
		return EXT4_UNSUPPORTED;
	}
	if (parent->size == 0 || parent->size % fs->info.block_size != 0) {
		return EXT4_CORRUPT;
	}
	if (blocks > EXT4_DIRECTORY_MAX_BLOCKS) {
		return EXT4_UNSUPPORTED;
	}
	error = ext4_write_map_validate(allocation, parent, disk);
	if (error != EXT4_OK) {
		return error;
	}
	ext4_zero(slot, sizeof(*slot));
	slot->logical = UINT32_MAX;
	for (logical = 0; logical < blocks; logical++) {
		error = ext4_write_map_lookup(allocation, parent, disk, logical, &run);
		if (error != EXT4_OK) {
			return error;
		}
		if (run.physical == 0 || run.unwritten) {
			return EXT4_CORRUPT;
		}
		error = ext4_transaction_read(allocation->transaction, run.physical, buffer);
		if (error == EXT4_OK) {
			error = ext4_directory_checksum(fs, parent, logical, buffer);
		}
		if (error != EXT4_OK) {
			return error;
		}
		for (offset = 0; offset < usable; offset += length) {
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
				for (index = 0; index < names; index++) {
					if (entry_name[index] == 0 || entry_name[index] == '/') {
						return EXT4_CORRUPT;
					}
				}
				if (names == name_length &&
				    ext4_equal(entry_name, name, name_length)) {
					exists = true;
				}
				used = ext4_directory_minimum(names);
			}
			if (slot->logical == UINT32_MAX && length - used >= required) {
				slot->logical = logical;
				slot->physical = run.physical;
				slot->offset = offset;
				slot->used = used;
				slot->length = length;
			}
		}
	}
	if (exists) {
		return EXT4_EXISTS;
	}
	if (slot->logical == UINT32_MAX) {
		if (blocks == EXT4_DIRECTORY_MAX_BLOCKS) {
			return EXT4_UNSUPPORTED;
		}
		slot->logical = (uint32_t)blocks;
		slot->length = usable;
	}
	return EXT4_OK;
}

static enum ext4_result
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

static enum ext4_result
ext4_directory_initialize(struct ext4_allocation *allocation, const struct ext4_inode *inode,
    struct ext4_inode_disk *disk, uint32_t parent)
{
	struct ext4_fs *fs = allocation->fs;
	void *buffer = NULL;
	uint64_t physical;
	uint32_t first = ext4_directory_minimum(1);
	bool zero;
	enum ext4_result error;

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

static enum ext4_file_type
ext4_namespace_type(uint16_t mode)
{
	switch (mode & EXT4_MODE_TYPE) {
	case EXT4_MODE_REGULAR:
		return EXT4_FT_REGULAR;
	case EXT4_MODE_DIRECTORY:
		return EXT4_FT_DIRECTORY;
	case EXT4_MODE_SYMLINK:
		return EXT4_FT_SYMLINK;
	case EXT4_MODE_CHARACTER:
		return EXT4_FT_CHARACTER;
	case EXT4_MODE_BLOCK:
		return EXT4_FT_BLOCK;
	case EXT4_MODE_FIFO:
		return EXT4_FT_FIFO;
	case EXT4_MODE_SOCKET:
		return EXT4_FT_SOCKET;
	default:
		return EXT4_FT_UNKNOWN;
	}
}

static enum ext4_result
ext4_namespace_add(struct ext4_fs *fs, uint32_t directory, uint32_t directory_generation,
    const uint8_t *name, size_t name_length, uint16_t create_mode, uint32_t target,
    uint32_t target_generation, const struct ext4_inode_update *attributes,
    const struct ext4_timestamp *time, struct ext4_inode *result)
{
	struct ext4_transaction *transaction = NULL;
	struct ext4_inode_disk *parent_disk;
	struct ext4_inode_disk *child_disk = NULL;
	struct ext4_inode parent;
	struct ext4_inode child;
	struct ext4_inode_update times;
	struct ext4_directory_slot slot;
	struct ext4_allocation allocation;
	uint64_t free_blocks;
	uint32_t flags;
	bool ready = false;
	enum ext4_file_type type;
	enum ext4_result error;

	if (fs == NULL || time == NULL || result == NULL) {
		return EXT4_INVALID_ARGUMENT;
	}
	if (create_mode != 0 &&
	    (attributes == NULL ||
		(attributes->fields & EXT4_CREATE_FIELDS) != EXT4_CREATE_FIELDS ||
		(attributes->fields & ~(uint32_t)(EXT4_CREATE_FIELDS | EXT4_ATTR_BIRTH_TIME)) ||
		(attributes->permissions & ~EXT4_MODE_PERMISSIONS))) {
		return EXT4_INVALID_ARGUMENT;
	}
	if (fs->aborted) {
		return EXT4_RECOVERY_REQUIRED;
	}
	if (fs->journal == NULL) {
		return EXT4_READ_ONLY;
	}
	error = ext4_namespace_name(name, name_length);
	if (error != EXT4_OK) {
		return error;
	}
	error =
	    ext4_transaction_begin(fs->journal, ext4_journal_credits(fs->journal), &transaction);
	if (error != EXT4_OK) {
		return error;
	}
	error = ext4_edit_inode(
	    fs, transaction, directory, directory_generation, &parent_disk, &parent);
	if (error != EXT4_OK) {
		goto cancel;
	}
	if ((parent.mode & EXT4_MODE_TYPE) != EXT4_MODE_DIRECTORY) {
		error = EXT4_NOT_DIRECTORY;
		goto cancel;
	}
	if (create_mode == EXT4_MODE_DIRECTORY &&
	    (parent.links == 1 || parent.links >= EXT4_LINK_MAX)) {
		error = EXT4_TOO_MANY_LINKS;
		goto cancel;
	}
	ext4_zero(&times, sizeof(times));
	times.fields = EXT4_ATTR_MODIFY_TIME | EXT4_ATTR_CHANGE_TIME;
	times.modify_time = *time;
	times.change_time = *time;
	if (create_mode == 0) {
		error = ext4_edit_inode(
		    fs, transaction, target, target_generation, &child_disk, &child);
		if (error != EXT4_OK) {
			goto cancel;
		}
		if ((child.mode & EXT4_MODE_TYPE) == EXT4_MODE_DIRECTORY) {
			error = EXT4_IS_DIRECTORY;
			goto cancel;
		}
		if (child.links >= EXT4_LINK_MAX) {
			error = EXT4_TOO_MANY_LINKS;
			goto cancel;
		}
		times.fields = EXT4_ATTR_CHANGE_TIME;
		error = ext4_inode_apply(fs, child_disk, &times);
		if (error != EXT4_OK) {
			goto cancel;
		}
		ext4_encode16(&child_disk->links, child.links + 1);
	}
	times.fields = EXT4_ATTR_MODIFY_TIME | EXT4_ATTR_CHANGE_TIME;
	error = ext4_inode_apply(fs, parent_disk, &times);
	if (error != EXT4_OK) {
		goto cancel;
	}
	error = ext4_allocation_init(&allocation, fs, transaction, &parent);
	if (error != EXT4_OK) {
		goto cancel;
	}
	ready = true;
	error = ext4_directory_space(&allocation, &parent, parent_disk, name, name_length, &slot);
	if (error != EXT4_OK) {
		goto cancel;
	}
	if (create_mode != 0) {
		error = ext4_allocate_inode(&allocation, create_mode, &child_disk, &child);
		if (error != EXT4_OK) {
			goto cancel;
		}
		flags = parent.flags & EXT4_INHERITED_FILE_FLAGS;
		if (create_mode == EXT4_MODE_DIRECTORY) {
			flags |= parent.flags & EXT4_INODE_DIRSYNC;
		}
		flags |= child.flags;
		ext4_encode32(&child_disk->flags, flags);
		error = ext4_inode_apply(fs, child_disk, attributes);
		if (error == EXT4_OK) {
			ext4_inode_checksum_set(fs, child.number, child_disk);
			error = ext4_inode_decode(fs, child.number, child_disk, &child);
		}
		if (error == EXT4_OK && create_mode == EXT4_MODE_DIRECTORY) {
			error = ext4_directory_initialize(
			    &allocation, &child, child_disk, parent.number);
			ext4_encode16(&parent_disk->links, parent.links + 1);
		}
		if (error != EXT4_OK) {
			goto cancel;
		}
		/* Block totals apply separately to the child and the parent. The shared
		 * free-space summary continues to include both allocations. */
		allocation.allocated = 0;
		allocation.freed = 0;
	}
	type = ext4_namespace_type(child.mode);
	if (type == EXT4_FT_UNKNOWN) {
		error = EXT4_UNSUPPORTED;
		goto cancel;
	}
	error = ext4_directory_insert(
	    &allocation, &parent, parent_disk, &slot, child.number, type, name, name_length);
	if (error == EXT4_OK) {
		ext4_inode_checksum_set(fs, parent.number, parent_disk);
		ext4_inode_checksum_set(fs, child.number, child_disk);
		error = ext4_inode_decode(fs, child.number, child_disk, &child);
	}
	if (error != EXT4_OK) {
		goto cancel;
	}
	free_blocks = allocation.free_blocks;
	ext4_allocation_destroy(&allocation);
	error = ext4_transaction_commit(transaction);
	if (error != EXT4_OK) {
		fs->aborted = true;
		return error;
	}
	fs->info.free_blocks = free_blocks;
	if (create_mode != 0) {
		fs->info.free_inodes--;
	}
	*result = child;
	return EXT4_OK;
cancel:
	if (ready) {
		ext4_allocation_destroy(&allocation);
	}
	ext4_transaction_cancel(transaction);
	return error;
}

enum ext4_result
ext4_create(struct ext4_fs *fs, uint32_t directory, uint32_t generation, const uint8_t *name,
    size_t name_length, const struct ext4_inode_update *attributes,
    const struct ext4_timestamp *directory_time, struct ext4_inode *result)
{
	return ext4_namespace_add(fs, directory, generation, name, name_length, EXT4_MODE_REGULAR,
	    0, 0, attributes, directory_time, result);
}

enum ext4_result
ext4_mkdir(struct ext4_fs *fs, uint32_t directory, uint32_t generation, const uint8_t *name,
    size_t name_length, const struct ext4_inode_update *attributes,
    const struct ext4_timestamp *directory_time, struct ext4_inode *result)
{
	return ext4_namespace_add(fs, directory, generation, name, name_length, EXT4_MODE_DIRECTORY,
	    0, 0, attributes, directory_time, result);
}

enum ext4_result
ext4_link(struct ext4_fs *fs, uint32_t directory, uint32_t directory_generation,
    const uint8_t *name, size_t name_length, uint32_t target, uint32_t target_generation,
    const struct ext4_timestamp *time, struct ext4_inode *result)
{
	return ext4_namespace_add(fs, directory, directory_generation, name, name_length, 0, target,
	    target_generation, NULL, time, result);
}

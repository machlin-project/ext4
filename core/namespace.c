/* SPDX-License-Identifier: BSD-3-Clause */
#include "directory_write.h"
#include "xattr.h"
#include "inline.h"

#define EXT4_CREATE_FIELDS                                                                         \
	(EXT4_ATTR_PERMISSIONS | EXT4_ATTR_UID | EXT4_ATTR_GID | EXT4_ATTR_ACCESS_TIME |           \
	    EXT4_ATTR_MODIFY_TIME | EXT4_ATTR_CHANGE_TIME)
#define EXT4_INHERITED_FILE_FLAGS                                                                  \
	(EXT4_INODE_SYNC | EXT4_INODE_NODUMP | EXT4_INODE_NOATIME | EXT4_INODE_NOTAIL |            \
	    EXT4_INODE_JOURNAL_DATA)

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
ext4_directory_links(struct ext4_allocation *allocation, struct ext4_inode_disk *disk,
    const struct ext4_inode *inode, int delta, unsigned int children)
{
	struct ext4_fs *fs = allocation->fs;
	uint32_t features;
	unsigned int links;
	enum ext4_result error;

	if (inode->links == 1) {
		return (fs->info.feature_ro_compat & EXT4_FEATURE_RO_DIR_NLINK) ? EXT4_OK
										: EXT4_CORRUPT;
	}
	if (inode->links < 2U + children || (delta < 0 && inode->links < 2 - delta)) {
		return EXT4_CORRUPT;
	}
	links = (unsigned int)((int)inode->links + delta);
	if (delta > 0 && links > EXT4_LINK_MAX) {
		if (!(ext4_le32(&disk->flags) & EXT4_INODE_INDEX)) {
			return EXT4_TOO_MANY_LINKS;
		}
		error = ext4_allocation_super(allocation);
		if (error != EXT4_OK) {
			return error;
		}
		features = ext4_le32(&allocation->super->feature_ro_compat);
		ext4_encode32(
		    &allocation->super->feature_ro_compat, features | EXT4_FEATURE_RO_DIR_NLINK);
		if (fs->metadata_checksum) {
			ext4_encode32(&allocation->super->checksum,
			    ext4_crc32c(UINT32_MAX, allocation->super,
				offsetof(struct ext4_super_disk, checksum)));
		}
		links = 1;
	}
	ext4_encode16(&disk->links, (uint16_t)links);
	return EXT4_OK;
}

static bool
ext4_directory_links_valid(struct ext4_fs *fs, const struct ext4_inode *inode, bool empty)
{
	if (inode->links == 1) {
		return (fs->info.feature_ro_compat & EXT4_FEATURE_RO_DIR_NLINK) != 0;
	}
	return empty ? inode->links == 2 : inode->links >= 2;
}

static enum ext4_result
ext4_symlink_initialize(struct ext4_allocation *allocation, struct ext4_inode *inode,
    struct ext4_inode_disk *disk, const uint8_t *target, size_t length)
{
	struct ext4_fs *fs = allocation->fs;
	void *buffer;
	uint64_t physical;
	bool zero;
	enum ext4_result error;

	if (length < sizeof(disk->block_data)) {
		if (allocation->attribute_blocks_added != 0) {
			/* Short symlinks cannot carry EA value charges in the Linux format. */
			return EXT4_UNSUPPORTED;
		}
		inode->flags &= ~EXT4_INODE_EXTENTS;
		ext4_zero(disk->block_data, sizeof(disk->block_data));
		ext4_copy(disk->block_data, target, length);
	} else {
		error = ext4_write_map_allocate(allocation, inode, disk, 0, &physical, &zero);
		if (error != EXT4_OK) {
			return error;
		}
		error = ext4_transaction_buffer(allocation->transaction, physical, &buffer);
		if (error != EXT4_OK) {
			return error;
		}
		ext4_zero(buffer, fs->info.block_size);
		ext4_copy(buffer, target, length);
	}
	return ext4_inode_account(allocation, inode, disk, length);
}

static enum ext4_result
ext4_creation_attributes(const struct ext4_inode_update *attributes)
{
	if (attributes == NULL || (attributes->fields & EXT4_CREATE_FIELDS) != EXT4_CREATE_FIELDS ||
	    (attributes->fields &
		~(uint32_t)(EXT4_CREATE_FIELDS | EXT4_ATTR_BIRTH_TIME | EXT4_ATTR_XATTRS)) ||
	    (attributes->permissions & ~EXT4_MODE_PERMISSIONS)) {
		return EXT4_INVALID_ARGUMENT;
	}
	return EXT4_OK;
}

static enum ext4_result
ext4_namespace_new(struct ext4_allocation *allocation, const struct ext4_inode *parent,
    const struct ext4_inode_disk *parent_disk, uint16_t mode,
    const struct ext4_inode_update *attributes, struct ext4_inode_disk **disk,
    struct ext4_inode *child)
{
	struct ext4_fs *fs = allocation->fs;
	uint32_t flags;
	bool inline_created;
	enum ext4_result error;

	/* The owner admits ACL/security inheritance, including explicitly none. */
	if (ext4_inode_has_xattrs(fs, parent_disk) && !(attributes->fields & EXT4_ATTR_XATTRS)) {
		return EXT4_INVALID_ARGUMENT;
	}
	error = ext4_allocate_inode(allocation, mode, disk, child);
	if (error != EXT4_OK) {
		return error;
	}
	flags = parent->flags & EXT4_INHERITED_FILE_FLAGS;
	if (mode == EXT4_MODE_DIRECTORY) {
		flags |= parent->flags & EXT4_INODE_DIRSYNC;
	} else if (mode != EXT4_MODE_REGULAR) {
		flags &= EXT4_INODE_NODUMP | EXT4_INODE_NOATIME;
	}
	ext4_encode32(&(*disk)->flags, flags | child->flags);
	error = ext4_inode_apply(fs, *disk, attributes);
	if (error == EXT4_OK) {
		ext4_inode_checksum_set(fs, child->number, *disk);
		error = ext4_inode_decode(fs, child->number, *disk, child);
	}
	if (error == EXT4_OK && (attributes->fields & EXT4_ATTR_XATTRS)) {
		error = ext4_xattr_apply(
		    allocation, child, *disk, attributes->xattrs, attributes->xattr_count);
	}
	if (error == EXT4_OK && mode == EXT4_MODE_REGULAR) {
		error = ext4_inline_start(allocation, child, *disk, 0, &inline_created);
	}
	return error;
}

static void
ext4_namespace_device(struct ext4_inode_disk *disk, const struct ext4_special_file *special)
{
	struct ext4_device_disk *device = (struct ext4_device_disk *)disk->block_data;
	uint32_t major = special->device_major;
	uint32_t minor = special->device_minor;

	if (major <= EXT4_DEVICE_LEGACY_MASK && minor <= EXT4_DEVICE_LEGACY_MASK) {
		ext4_encode32(&device->legacy, (major << EXT4_DEVICE_MAJOR_SHIFT) | minor);
	} else {
		ext4_encode32(&device->extended,
		    (major << EXT4_DEVICE_MAJOR_SHIFT) | (minor & EXT4_DEVICE_LEGACY_MASK) |
			((minor & ~EXT4_DEVICE_LEGACY_MASK) << EXT4_DEVICE_MINOR_HIGH_SHIFT));
	}
}

static enum ext4_result
ext4_namespace_add(struct ext4_fs *fs, uint32_t directory, uint32_t directory_generation,
    const uint8_t *name, size_t name_length, uint16_t create_mode, uint32_t target,
    uint32_t target_generation, const struct ext4_inode_update *attributes,
    const uint8_t *link_target, size_t link_length, const struct ext4_special_file *special,
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
	uint32_t feature_compat;
	uint32_t feature_ro_compat;
	size_t index;
	bool ready = false;
	enum ext4_file_type type;
	enum ext4_result error;

	if (fs == NULL || time == NULL || result == NULL) {
		return EXT4_INVALID_ARGUMENT;
	}
	if (create_mode != 0 && ext4_creation_attributes(attributes) != EXT4_OK) {
		return EXT4_INVALID_ARGUMENT;
	}
	if (fs->aborted) {
		return EXT4_RECOVERY_REQUIRED;
	}
	if (fs->journal == NULL) {
		return EXT4_READ_ONLY;
	}
	if (create_mode != 0 && (attributes->fields & EXT4_ATTR_XATTRS)) {
		error =
		    ext4_xattr_changes_validate(fs, attributes->xattrs, attributes->xattr_count);
		if (error != EXT4_OK) {
			return error;
		}
	}
	if (create_mode == EXT4_MODE_SYMLINK) {
		if (link_target == NULL || link_length == 0) {
			return EXT4_INVALID_ARGUMENT;
		}
		if (link_length >= fs->info.block_size) {
			return EXT4_NAME_TOO_LONG;
		}
		for (index = 0; index < link_length; index++) {
			if (link_target[index] == 0) {
				return EXT4_INVALID_ARGUMENT;
			}
		}
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
	if (parent.links == 0) {
		error = EXT4_NOT_FOUND;
		goto cancel;
	}
	if (parent.flags & EXT4_INODE_IMMUTABLE) {
		error = EXT4_PERMISSION_DENIED;
		goto cancel;
	}
	if (!ext4_directory_links_valid(fs, &parent, false)) {
		error = EXT4_CORRUPT;
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
		if (child.links == 0) {
			error = EXT4_NOT_FOUND;
			goto cancel;
		}
		if (child.flags & EXT4_INODE_RESTRICTED_FLAGS) {
			error = EXT4_PERMISSION_DENIED;
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
	error = ext4_directory_scan(
	    &allocation, &parent, parent_disk, name, name_length, EXT4_DIRECTORY_INSERT, 0, &slot);
	if (error != EXT4_OK) {
		goto cancel;
	}
	if (create_mode != 0) {
		error = ext4_namespace_new(&allocation, &parent, parent_disk, create_mode,
		    attributes, &child_disk, &child);
		if (error == EXT4_OK && create_mode == EXT4_MODE_DIRECTORY) {
			error = ext4_directory_initialize(
			    &allocation, &child, child_disk, parent.number);
		} else if (error == EXT4_OK && create_mode == EXT4_MODE_SYMLINK) {
			error = ext4_symlink_initialize(
			    &allocation, &child, child_disk, link_target, link_length);
		} else if (error == EXT4_OK) {
			error = ext4_inode_account(&allocation, &child, child_disk, 0);
			if (error == EXT4_OK && special != NULL) {
				ext4_namespace_device(child_disk, special);
			}
		}
		if (error != EXT4_OK) {
			goto cancel;
		}
		/* Block totals apply separately to the child and the parent. The shared
		 * free-space summary continues to include both allocations. */
		allocation.allocated = 0;
		allocation.freed = 0;
		allocation.detached_shared_blocks = 0;
		allocation.attribute_blocks_added = 0;
		allocation.attribute_blocks_removed = 0;
	}
	type = ext4_namespace_type(child.mode);
	if (type == EXT4_FT_UNKNOWN) {
		error = EXT4_UNSUPPORTED;
		goto cancel;
	}
	error = ext4_directory_insert(
	    &allocation, &parent, parent_disk, &slot, child.number, type, name, name_length);
	if (error == EXT4_OK && create_mode == EXT4_MODE_DIRECTORY) {
		error = ext4_directory_links(&allocation, parent_disk, &parent, 1, 0);
	}
	if (error == EXT4_OK) {
		ext4_inode_checksum_set(fs, parent.number, parent_disk);
		ext4_inode_checksum_set(fs, child.number, child_disk);
		error = ext4_inode_decode(fs, child.number, child_disk, &child);
	}
	if (error != EXT4_OK) {
		goto cancel;
	}
	free_blocks = allocation.free_blocks;
	feature_compat = allocation.super == NULL ? fs->info.feature_compat
						  : ext4_le32(&allocation.super->feature_compat);
	feature_ro_compat = allocation.super == NULL
	    ? fs->info.feature_ro_compat
	    : ext4_le32(&allocation.super->feature_ro_compat);
	ext4_allocation_destroy(&allocation);
	error = ext4_transaction_commit(transaction);
	if (error != EXT4_OK) {
		fs->aborted = true;
		return error;
	}
	fs->info.free_blocks = free_blocks;
	fs->info.feature_compat = feature_compat;
	fs->info.feature_ro_compat |= feature_ro_compat & EXT4_FEATURE_RO_DIR_NLINK;
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
	    0, 0, attributes, NULL, 0, NULL, directory_time, result);
}

enum ext4_result
ext4_mkdir(struct ext4_fs *fs, uint32_t directory, uint32_t generation, const uint8_t *name,
    size_t name_length, const struct ext4_inode_update *attributes,
    const struct ext4_timestamp *directory_time, struct ext4_inode *result)
{
	return ext4_namespace_add(fs, directory, generation, name, name_length, EXT4_MODE_DIRECTORY,
	    0, 0, attributes, NULL, 0, NULL, directory_time, result);
}

enum ext4_result
ext4_mknod(struct ext4_fs *fs, uint32_t directory, uint32_t generation, const uint8_t *name,
    size_t name_length, const struct ext4_special_file *special,
    const struct ext4_inode_update *attributes, const struct ext4_timestamp *directory_time,
    struct ext4_inode *result)
{
	uint16_t mode;

	if (special == NULL) {
		return EXT4_INVALID_ARGUMENT;
	}
	switch (special->type) {
	case EXT4_FT_CHARACTER:
		mode = EXT4_MODE_CHARACTER;
		break;
	case EXT4_FT_BLOCK:
		mode = EXT4_MODE_BLOCK;
		break;
	case EXT4_FT_FIFO:
		mode = EXT4_MODE_FIFO;
		break;
	case EXT4_FT_SOCKET:
		mode = EXT4_MODE_SOCKET;
		break;
	default:
		return EXT4_INVALID_ARGUMENT;
	}
	if (mode == EXT4_MODE_FIFO || mode == EXT4_MODE_SOCKET) {
		if (special->device_major != 0 || special->device_minor != 0) {
			return EXT4_INVALID_ARGUMENT;
		}
	} else if (special->device_major > EXT4_DEVICE_MAJOR_MAX ||
	    special->device_minor > EXT4_DEVICE_MINOR_MAX) {
		return EXT4_RANGE;
	}
	return ext4_namespace_add(fs, directory, generation, name, name_length, mode, 0, 0,
	    attributes, NULL, 0, special, directory_time, result);
}

enum ext4_result
ext4_link(struct ext4_fs *fs, uint32_t directory, uint32_t directory_generation,
    const uint8_t *name, size_t name_length, uint32_t target, uint32_t target_generation,
    const struct ext4_timestamp *time, struct ext4_inode *result)
{
	return ext4_namespace_add(fs, directory, directory_generation, name, name_length, 0, target,
	    target_generation, NULL, NULL, 0, NULL, time, result);
}

enum ext4_result
ext4_symlink(struct ext4_fs *fs, uint32_t directory, uint32_t generation, const uint8_t *name,
    size_t name_length, const uint8_t *target, size_t target_length,
    const struct ext4_inode_update *attributes, const struct ext4_timestamp *directory_time,
    struct ext4_inode *result)
{
	return ext4_namespace_add(fs, directory, generation, name, name_length, EXT4_MODE_SYMLINK,
	    0, 0, attributes, target, target_length, NULL, directory_time, result);
}

static enum ext4_result
ext4_namespace_orphan_prepare(struct ext4_allocation *allocation, const struct ext4_inode *inode,
    struct ext4_inode_disk *disk, bool validated_directory)
{
	struct ext4_fs *fs = allocation->fs;
	enum ext4_result error;

	error = ext4_orphan_reserve(fs, inode, disk);
	if (error != EXT4_OK) {
		return error;
	}
	if (!validated_directory) {
		error = ext4_write_map_validate(allocation, inode, disk);
	}
	if (error == EXT4_OK) {
		error = ext4_allocation_super(allocation);
	}
	if (error != EXT4_OK) {
		return error;
	}
	if (ext4_le32(&allocation->super->last_orphan) != fs->last_orphan) {
		return EXT4_CORRUPT;
	}
	ext4_encode32(&disk->deletion_time, fs->last_orphan);
	ext4_encode32(&allocation->super->last_orphan, inode->number);
	return EXT4_OK;
}

static enum ext4_result
ext4_namespace_orphan_complete(struct ext4_fs *fs, uint32_t number, uint32_t generation)
{
	struct ext4_inode_hold *hold = ext4_inode_find_hold(fs, number);
	enum ext4_result error;

	fs->last_orphan = number;
	if (hold != NULL) {
		hold->unlinked = true;
		return EXT4_OK;
	}
	error = ext4_orphan_finish_inode(fs, number, generation, false);
	if (error != EXT4_OK) {
		fs->aborted = true;
	}
	return error;
}

static enum ext4_result
ext4_namespace_remove(struct ext4_fs *fs, uint32_t directory, uint32_t directory_generation,
    const uint8_t *name, size_t name_length, uint32_t target, uint32_t target_generation,
    bool remove_directory, const struct ext4_timestamp *time, struct ext4_inode *result)
{
	struct ext4_transaction *transaction;
	struct ext4_inode_disk *parent_disk;
	struct ext4_inode_disk *child_disk;
	struct ext4_inode parent;
	struct ext4_inode child;
	struct ext4_inode_update times;
	struct ext4_allocation allocation;
	struct ext4_directory_slot slot;
	struct ext4_directory_slot empty;
	uint16_t type;
	bool ready = false;
	bool last;
	enum ext4_result error;

	if (fs == NULL || time == NULL || result == NULL) {
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
	if (parent.links == 0) {
		error = EXT4_NOT_FOUND;
		goto cancel;
	}
	if (parent.flags & EXT4_INODE_RESTRICTED_FLAGS) {
		error = EXT4_PERMISSION_DENIED;
		goto cancel;
	}
	if (!ext4_directory_links_valid(fs, &parent, false)) {
		error = EXT4_CORRUPT;
		goto cancel;
	}
	error = ext4_allocation_init(&allocation, fs, transaction, &parent);
	if (error != EXT4_OK) {
		goto cancel;
	}
	ready = true;
	error = ext4_directory_scan(
	    &allocation, &parent, parent_disk, name, name_length, EXT4_DIRECTORY_FIND, 0, &slot);
	if (error != EXT4_OK) {
		goto cancel;
	}
	if (slot.number != target) {
		error = EXT4_STALE;
		goto cancel;
	}
	if (target == directory || target == EXT4_ROOT_INODE) {
		error = EXT4_CORRUPT;
		goto cancel;
	}
	error = ext4_edit_inode(fs, transaction, target, target_generation, &child_disk, &child);
	if (error != EXT4_OK) {
		goto cancel;
	}
	if (child.flags & EXT4_INODE_RESTRICTED_FLAGS) {
		error = EXT4_PERMISSION_DENIED;
		goto cancel;
	}
	type = child.mode & EXT4_MODE_TYPE;
	if (ext4_namespace_type(child.mode) == EXT4_FT_UNKNOWN) {
		error = EXT4_UNSUPPORTED;
		goto cancel;
	}
	if (child.links == 0 ||
	    (slot.type != EXT4_FT_UNKNOWN && slot.type != ext4_namespace_type(child.mode))) {
		error = EXT4_CORRUPT;
		goto cancel;
	}
	if (remove_directory != (type == EXT4_MODE_DIRECTORY)) {
		error = remove_directory ? EXT4_NOT_DIRECTORY : EXT4_IS_DIRECTORY;
		goto cancel;
	}
	if (remove_directory) {
		error = ext4_directory_scan(&allocation, &child, child_disk, NULL, 0,
		    EXT4_DIRECTORY_EMPTY, parent.number, &empty);
		if (error != EXT4_OK) {
			goto cancel;
		}
		if (!ext4_directory_links_valid(fs, &child, true)) {
			error = EXT4_CORRUPT;
			goto cancel;
		}
		error = ext4_directory_links(&allocation, parent_disk, &parent, -1, 1);
		if (error != EXT4_OK) {
			goto cancel;
		}
	}
	last = remove_directory || child.links == 1;
	if (last) {
		error = ext4_namespace_orphan_prepare(
		    &allocation, &child, child_disk, remove_directory);
		if (error != EXT4_OK) {
			goto cancel;
		}
	}
	ext4_zero(&times, sizeof(times));
	times.fields = EXT4_ATTR_MODIFY_TIME | EXT4_ATTR_CHANGE_TIME;
	times.modify_time = *time;
	times.change_time = *time;
	error = ext4_inode_apply(fs, parent_disk, &times);
	if (error == EXT4_OK) {
		times.fields = EXT4_ATTR_CHANGE_TIME;
		error = ext4_inode_apply(fs, child_disk, &times);
	}
	if (error == EXT4_OK) {
		error = ext4_directory_remove(&allocation, &parent, &slot);
	}
	if (error != EXT4_OK) {
		goto cancel;
	}
	ext4_encode16(&child_disk->links, last ? 0 : child.links - 1);
	if (remove_directory) {
		ext4_encode32(&child_disk->size_lo, 0);
		ext4_encode32(&child_disk->size_hi, 0);
	}
	ext4_inode_checksum_set(fs, directory, parent_disk);
	ext4_inode_checksum_set(fs, target, child_disk);
	error = ext4_inode_decode_orphan(fs, target, child_disk, &child);
	if (error != EXT4_OK) {
		goto cancel;
	}
	ext4_allocation_destroy(&allocation);
	error = ext4_transaction_commit(transaction);
	if (error != EXT4_OK) {
		fs->aborted = true;
		return error;
	}
	if (last) {
		error = ext4_namespace_orphan_complete(fs, target, target_generation);
		if (error != EXT4_OK) {
			return error;
		}
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
ext4_unlink(struct ext4_fs *fs, uint32_t directory, uint32_t directory_generation,
    const uint8_t *name, size_t name_length, uint32_t target, uint32_t target_generation,
    const struct ext4_timestamp *time, struct ext4_inode *result)
{
	return ext4_namespace_remove(fs, directory, directory_generation, name, name_length, target,
	    target_generation, false, time, result);
}

enum ext4_result
ext4_rmdir(struct ext4_fs *fs, uint32_t directory, uint32_t directory_generation,
    const uint8_t *name, size_t name_length, uint32_t target, uint32_t target_generation,
    const struct ext4_timestamp *time, struct ext4_inode *result)
{
	return ext4_namespace_remove(fs, directory, directory_generation, name, name_length, target,
	    target_generation, true, time, result);
}

struct ext4_rename_state {
	struct ext4_inode parents[2];
	struct ext4_inode objects[2];
	struct ext4_inode whiteout;
	struct ext4_inode_disk *parent_disks[2];
	struct ext4_inode_disk *object_disks[2];
	struct ext4_inode_disk *whiteout_disk;
	struct ext4_directory_slot entries[2];
	struct ext4_directory_slot dotdot[2];
	struct ext4_allocation allocation;
};

static enum ext4_result
ext4_rename_resolve(
    struct ext4_rename_state *state, const struct ext4_rename_entry *entry, unsigned int index)
{
	struct ext4_allocation *allocation = &state->allocation;
	struct ext4_fs *fs = allocation->fs;
	struct ext4_inode *inode = &state->objects[index];
	struct ext4_directory_slot *slot = &state->entries[index];
	enum ext4_result error;

	error = ext4_directory_scan(allocation, &state->parents[index], state->parent_disks[index],
	    entry->name, entry->name_length, EXT4_DIRECTORY_FIND, 0, slot);
	if (error == EXT4_NOT_FOUND && index == 1) {
		return entry->inode == 0 ? EXT4_OK : EXT4_STALE;
	}
	if (error != EXT4_OK) {
		return error;
	}
	if (slot->number != entry->inode) {
		return EXT4_STALE;
	}
	if (slot->number == entry->directory || slot->number == EXT4_ROOT_INODE) {
		return EXT4_CORRUPT;
	}
	error = ext4_edit_inode(fs, allocation->transaction, entry->inode, entry->generation,
	    &state->object_disks[index], inode);
	if (error != EXT4_OK) {
		return error;
	}
	if (inode->links == 0 ||
	    (slot->type != EXT4_FT_UNKNOWN && slot->type != ext4_namespace_type(inode->mode))) {
		return EXT4_CORRUPT;
	}
	return ext4_namespace_type(inode->mode) == EXT4_FT_UNKNOWN ? EXT4_UNSUPPORTED : EXT4_OK;
}

static enum ext4_result
ext4_rename_ancestry(struct ext4_fs *fs, uint32_t moved, uint32_t parent)
{
	struct ext4_inode inode;
	struct ext4_inode next;
	uint32_t number = parent;
	uint32_t anchor = parent;
	uint32_t visited = 0;
	uint64_t distance = 0;
	uint64_t power = 1;
	enum ext4_result error;

	/* The exclusive owner keeps this pre-mutation ancestry stable. Brent's
	 * cycle check bounds malformed parent chains without a recursive walk. */
	for (;;) {
		if (number == moved) {
			return EXT4_INVALID_ARGUMENT;
		}
		if (number == EXT4_ROOT_INODE) {
			return EXT4_OK;
		}
		if (visited++ == fs->info.inodes) {
			return EXT4_CORRUPT;
		}
		error = ext4_inode_allocated(fs, number);
		if (error == EXT4_OK) {
			error = ext4_get_inode(fs, number, &inode);
		}
		if (error == EXT4_OK && (inode.mode & EXT4_MODE_TYPE) != EXT4_MODE_DIRECTORY) {
			error = EXT4_CORRUPT;
		}
		if (error == EXT4_OK) {
			error = ext4_lookup(fs, &inode, (const uint8_t *)"..", 2, &next);
		}
		if (error != EXT4_OK) {
			return error == EXT4_NOT_FOUND ? EXT4_CORRUPT : error;
		}
		number = next.number;
		if ((next.mode & EXT4_MODE_TYPE) != EXT4_MODE_DIRECTORY || number == anchor) {
			return EXT4_CORRUPT;
		}
		distance++;
		if (distance == power) {
			anchor = number;
			power *= 2;
			distance = 0;
		}
	}
}

static enum ext4_result
ext4_namespace_rename(struct ext4_fs *fs, const struct ext4_rename_entry *source,
    const struct ext4_rename_entry *destination, uint32_t flags,
    const struct ext4_inode_update *whiteout_attributes, const struct ext4_timestamp *time,
    struct ext4_inode *result)
{
	const struct ext4_rename_entry *names[2];
	struct ext4_rename_state *state;
	struct ext4_transaction *transaction;
	struct ext4_inode_update times;
	struct ext4_directory_slot space;
	uint64_t free_blocks;
	uint32_t feature_compat;
	uint32_t feature_ro_compat;
	unsigned int index;
	int delta[2] = { 0, 0 };
	bool directory[2];
	bool ready = false;
	bool exchange = (flags & EXT4_RENAME_EXCHANGE) != 0;
	bool same_parent;
	bool exists;
	bool last = false;
	enum ext4_result error;

	if (fs == NULL || source == NULL || destination == NULL || time == NULL || result == NULL ||
	    source->inode == 0 || (destination->inode == 0 && destination->generation != 0) ||
	    (flags & ~(uint32_t)(EXT4_RENAME_NOREPLACE | EXT4_RENAME_EXCHANGE)) != 0 ||
	    flags == (EXT4_RENAME_NOREPLACE | EXT4_RENAME_EXCHANGE)) {
		return EXT4_INVALID_ARGUMENT;
	}
	if (whiteout_attributes != NULL &&
	    (exchange || ext4_creation_attributes(whiteout_attributes) != EXT4_OK ||
		whiteout_attributes->permissions != 0)) {
		return EXT4_INVALID_ARGUMENT;
	}
	if (fs->aborted) {
		return EXT4_RECOVERY_REQUIRED;
	}
	if (fs->journal == NULL) {
		return EXT4_READ_ONLY;
	}
	if (whiteout_attributes != NULL && (whiteout_attributes->fields & EXT4_ATTR_XATTRS)) {
		error = ext4_xattr_changes_validate(
		    fs, whiteout_attributes->xattrs, whiteout_attributes->xattr_count);
		if (error != EXT4_OK) {
			return error;
		}
	}
	names[0] = source;
	names[1] = destination;
	for (index = 0; index < 2; index++) {
		error = ext4_namespace_name(names[index]->name, names[index]->name_length);
		if (error != EXT4_OK) {
			return error;
		}
	}
	state = fs->environment.allocate(fs->environment.context, sizeof(*state));
	if (state == NULL) {
		return EXT4_NO_MEMORY;
	}
	ext4_zero(state, sizeof(*state));
	error =
	    ext4_transaction_begin(fs->journal, ext4_journal_credits(fs->journal), &transaction);
	if (error != EXT4_OK) {
		goto out;
	}
	for (index = 0; index < 2; index++) {
		error = ext4_edit_inode(fs, transaction, names[index]->directory,
		    names[index]->directory_generation, &state->parent_disks[index],
		    &state->parents[index]);
		if (error != EXT4_OK) {
			goto cancel;
		}
		if ((state->parents[index].mode & EXT4_MODE_TYPE) != EXT4_MODE_DIRECTORY) {
			error = EXT4_NOT_DIRECTORY;
			goto cancel;
		}
		if (state->parents[index].links == 0) {
			error = EXT4_NOT_FOUND;
			goto cancel;
		}
		if (!ext4_directory_links_valid(fs, &state->parents[index], false)) {
			error = EXT4_CORRUPT;
			goto cancel;
		}
	}
	error = ext4_allocation_init(&state->allocation, fs, transaction, &state->parents[1]);
	if (error != EXT4_OK) {
		goto cancel;
	}
	ready = true;
	for (index = 0; index < 2; index++) {
		error = ext4_rename_resolve(state, names[index], index);
		if (error != EXT4_OK) {
			goto cancel;
		}
		directory[index] =
		    (state->objects[index].mode & EXT4_MODE_TYPE) == EXT4_MODE_DIRECTORY;
	}
	exists = state->objects[1].number != 0;
	if (exists && (flags & EXT4_RENAME_NOREPLACE)) {
		error = EXT4_EXISTS;
		goto cancel;
	}
	if (!exists && exchange) {
		error = EXT4_NOT_FOUND;
		goto cancel;
	}
	if (state->objects[0].number == state->objects[1].number) {
		*result = state->objects[0];
		goto cancel;
	}
	/* Moving out removes a name; an absent destination only adds one. An
	 * append-only destination can accept that addition but not replacement. */
	if ((state->parents[0].flags & EXT4_INODE_RESTRICTED_FLAGS) ||
	    (state->parents[1].flags & EXT4_INODE_IMMUTABLE) ||
	    (exists && (state->parents[1].flags & EXT4_INODE_APPEND)) ||
	    (state->objects[0].flags & EXT4_INODE_RESTRICTED_FLAGS) ||
	    (exists && (state->objects[1].flags & EXT4_INODE_RESTRICTED_FLAGS))) {
		error = EXT4_PERMISSION_DENIED;
		goto cancel;
	}
	if (exists && !exchange && directory[0] != directory[1]) {
		error = directory[0] ? EXT4_NOT_DIRECTORY : EXT4_IS_DIRECTORY;
		goto cancel;
	}
	same_parent = source->directory == destination->directory;
	for (index = 0; index < 2; index++) {
		if (!directory[index]) {
			continue;
		}
		if (!ext4_directory_links_valid(fs, &state->objects[index], false)) {
			error = EXT4_CORRUPT;
			goto cancel;
		}
		error = ext4_directory_scan(&state->allocation, &state->objects[index],
		    state->object_disks[index], (const uint8_t *)"..", 2, EXT4_DIRECTORY_FIND,
		    names[index]->directory, &state->dotdot[index]);
		if (error != EXT4_OK) {
			goto cancel;
		}
		if (!same_parent && (index == 0 || exchange)) {
			error = ext4_rename_ancestry(
			    fs, state->objects[index].number, names[1U - index]->directory);
			if (error != EXT4_OK) {
				goto cancel;
			}
			delta[index]--;
			delta[1U - index]++;
		}
	}
	if (whiteout_attributes != NULL) {
		error = ext4_namespace_new(&state->allocation, &state->parents[0],
		    state->parent_disks[0], EXT4_MODE_CHARACTER, whiteout_attributes,
		    &state->whiteout_disk, &state->whiteout);
		if (error == EXT4_OK) {
			error = ext4_inode_account(
			    &state->allocation, &state->whiteout, state->whiteout_disk, 0);
		}
		if (error != EXT4_OK) {
			goto cancel;
		}
		ext4_inode_checksum_set(fs, state->whiteout.number, state->whiteout_disk);
		/* The zeroed device identity represents the whiteout. Subsequent
		 * allocations belong to the destination directory, not this inode. */
		state->allocation.allocated = 0;
		state->allocation.freed = 0;
		state->allocation.detached_shared_blocks = 0;
		state->allocation.attribute_blocks_added = 0;
		state->allocation.attribute_blocks_removed = 0;
	}
	if (exists && !exchange) {
		if (directory[1]) {
			error = ext4_directory_scan(&state->allocation, &state->objects[1],
			    state->object_disks[1], NULL, 0, EXT4_DIRECTORY_EMPTY,
			    destination->directory, &space);
			if (error != EXT4_OK) {
				goto cancel;
			}
			if (!ext4_directory_links_valid(fs, &state->objects[1], true)) {
				error = EXT4_CORRUPT;
				goto cancel;
			}
			delta[1]--;
		}
		last = directory[1] || state->objects[1].links == 1;
		if (last) {
			error = ext4_namespace_orphan_prepare(&state->allocation,
			    &state->objects[1], state->object_disks[1], directory[1]);
			if (error != EXT4_OK) {
				goto cancel;
			}
		}
		ext4_encode16(
		    &state->object_disks[1]->links, last ? 0 : state->objects[1].links - 1);
		if (directory[1]) {
			ext4_encode32(&state->object_disks[1]->size_lo, 0);
			ext4_encode32(&state->object_disks[1]->size_hi, 0);
		}
	}
	if (exists) {
		error = ext4_directory_replace(&state->allocation, &state->parents[1],
		    &state->entries[1], state->objects[0].number,
		    ext4_namespace_type(state->objects[0].mode));
		if (error != EXT4_OK) {
			goto cancel;
		}
	}
	if (exchange) {
		error = ext4_directory_replace(&state->allocation, &state->parents[0],
		    &state->entries[0], state->objects[1].number,
		    ext4_namespace_type(state->objects[1].mode));
	} else if (whiteout_attributes != NULL) {
		error = ext4_directory_replace(&state->allocation, &state->parents[0],
		    &state->entries[0], state->whiteout.number, EXT4_FT_CHARACTER);
	} else {
		error = ext4_directory_remove(
		    &state->allocation, &state->parents[0], &state->entries[0]);
	}
	if (error != EXT4_OK) {
		goto cancel;
	}
	if (!exists) {
		error = ext4_directory_scan(&state->allocation, &state->parents[1],
		    state->parent_disks[1], destination->name, destination->name_length,
		    EXT4_DIRECTORY_INSERT, 0, &space);
		if (error == EXT4_OK) {
			error = ext4_directory_insert(&state->allocation, &state->parents[1],
			    state->parent_disks[1], &space, state->objects[0].number,
			    ext4_namespace_type(state->objects[0].mode), destination->name,
			    destination->name_length);
		}
	}
	if (error != EXT4_OK) {
		goto cancel;
	}
	if (same_parent) {
		error = ext4_directory_links(&state->allocation, state->parent_disks[0],
		    &state->parents[0], delta[0] + delta[1],
		    (unsigned int)directory[0] + (unsigned int)directory[1]);
	} else {
		error = ext4_directory_links(&state->allocation, state->parent_disks[0],
		    &state->parents[0], delta[0], directory[0]);
		if (error == EXT4_OK) {
			error = ext4_directory_links(&state->allocation, state->parent_disks[1],
			    &state->parents[1], delta[1], directory[1]);
		}
	}
	if (error != EXT4_OK) {
		goto cancel;
	}
	ext4_zero(&times, sizeof(times));
	times.change_time = *time;
	times.modify_time = *time;
	for (index = 0; index < 2; index++) {
		if (directory[index] && !same_parent && (index == 0 || exchange)) {
			error = ext4_directory_replace(&state->allocation, &state->objects[index],
			    &state->dotdot[index], names[1U - index]->directory, EXT4_FT_DIRECTORY);
			if (error != EXT4_OK) {
				goto cancel;
			}
		}
		times.fields = EXT4_ATTR_MODIFY_TIME | EXT4_ATTR_CHANGE_TIME;
		error = ext4_inode_apply(fs, state->parent_disks[index], &times);
		if (error == EXT4_OK && state->objects[index].number != 0) {
			times.fields = EXT4_ATTR_CHANGE_TIME;
			error = ext4_inode_apply(fs, state->object_disks[index], &times);
			ext4_inode_checksum_set(
			    fs, state->objects[index].number, state->object_disks[index]);
		}
		if (error != EXT4_OK) {
			goto cancel;
		}
		ext4_inode_checksum_set(
		    fs, state->parents[index].number, state->parent_disks[index]);
	}
	error = ext4_inode_decode_live(
	    fs, state->objects[0].number, state->object_disks[0], &state->objects[0]);
	if (error != EXT4_OK) {
		goto cancel;
	}
	free_blocks = state->allocation.free_blocks;
	feature_compat = state->allocation.super == NULL
	    ? fs->info.feature_compat
	    : ext4_le32(&state->allocation.super->feature_compat);
	feature_ro_compat = state->allocation.super == NULL
	    ? fs->info.feature_ro_compat
	    : ext4_le32(&state->allocation.super->feature_ro_compat);
	ext4_allocation_destroy(&state->allocation);
	ready = false;
	error = ext4_transaction_commit(transaction);
	if (error != EXT4_OK) {
		fs->aborted = true;
		goto out;
	}
	fs->info.free_blocks = free_blocks;
	fs->info.feature_compat = feature_compat;
	fs->info.feature_ro_compat |= feature_ro_compat & EXT4_FEATURE_RO_DIR_NLINK;
	if (last) {
		error = ext4_namespace_orphan_complete(
		    fs, state->objects[1].number, state->objects[1].generation);
	}
	if (error == EXT4_OK) {
		*result = state->objects[0];
	}
	goto out;
cancel:
	if (ready) {
		ext4_allocation_destroy(&state->allocation);
	}
	ext4_transaction_cancel(transaction);
out:
	fs->environment.release(fs->environment.context, state, sizeof(*state));
	return error;
}

enum ext4_result
ext4_rename(struct ext4_fs *fs, const struct ext4_rename_entry *source,
    const struct ext4_rename_entry *destination, uint32_t flags, const struct ext4_timestamp *time,
    struct ext4_inode *result)
{
	return ext4_namespace_rename(fs, source, destination, flags, NULL, time, result);
}

enum ext4_result
ext4_rename_whiteout(struct ext4_fs *fs, const struct ext4_rename_entry *source,
    const struct ext4_rename_entry *destination, uint32_t flags,
    const struct ext4_inode_update *whiteout_attributes, const struct ext4_timestamp *time,
    struct ext4_inode *result)
{
	if (whiteout_attributes == NULL) {
		return EXT4_INVALID_ARGUMENT;
	}
	return ext4_namespace_rename(
	    fs, source, destination, flags, whiteout_attributes, time, result);
}

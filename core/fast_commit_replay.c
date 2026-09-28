/* SPDX-License-Identifier: BSD-3-Clause */
#include "fast_commit.h"
#include "directory_write.h"
#include "xattr.h"

struct ext4_fc_inode_state {
	uint32_t number;
	bool created;
	bool directory_initialized;
	bool unlinked;
};

struct ext4_fc_replay {
	struct ext4_fast_commit *log;
	struct ext4_allocation allocation;
	struct ext4_block_range *excluded;
	struct ext4_fc_inode_state *inodes;
	uint8_t *buffer;
	uint32_t inode_count;
	uint32_t inode_capacity;
	uint32_t orphan_slots_removed;
};

static bool
ext4_fc_inode_valid(const struct ext4_fs *fs, uint32_t number)
{
	return number != 0 && number <= fs->info.inodes &&
	    (number >= fs->first_inode || number == EXT4_ROOT_INODE) &&
	    number != fs->journal_inode && number != fs->orphan_file_inode;
}

static struct ext4_fc_inode_state *
ext4_fc_inode_state(struct ext4_fc_replay *replay, uint32_t number)
{
	uint32_t low = 0;
	uint32_t high = replay->inode_count;
	uint32_t middle;

	while (low < high) {
		middle = low + (high - low) / 2U;
		if (replay->inodes[middle].number == number) {
			return &replay->inodes[middle];
		}
		if (replay->inodes[middle].number < number) {
			low = middle + 1U;
		} else {
			high = middle;
		}
	}
	return NULL;
}

static void
ext4_fc_inode_sift(struct ext4_fc_inode_state *inodes, uint32_t root, uint32_t count)
{
	struct ext4_fc_inode_state saved = inodes[root];
	uint32_t child;

	while (root < count / 2U) {
		child = root * 2U + 1U;
		if (child + 1U < count && inodes[child + 1U].number > inodes[child].number) {
			child++;
		}
		if (saved.number >= inodes[child].number) {
			break;
		}
		inodes[root] = inodes[child];
		root = child;
	}
	inodes[root] = saved;
}

static void
ext4_fc_inodes_sort(struct ext4_fc_replay *replay)
{
	struct ext4_fc_inode_state *inodes = replay->inodes;
	struct ext4_fc_inode_state saved;
	uint32_t count = replay->inode_count;
	uint32_t index;
	uint32_t used = 0;

	for (index = count / 2U; index > 0; index--) {
		ext4_fc_inode_sift(inodes, index - 1U, count);
	}
	for (index = count; index > 1; index--) {
		saved = inodes[0];
		inodes[0] = inodes[index - 1U];
		inodes[index - 1U] = saved;
		ext4_fc_inode_sift(inodes, 0, index - 1U);
	}
	for (index = 0; index < count; index++) {
		if (used == 0 || inodes[index].number != inodes[used - 1U].number) {
			inodes[used++] = inodes[index];
		}
	}
	replay->inode_count = used;
}

static enum ext4_result
ext4_fc_inode_collect(struct ext4_fc_replay *replay, uint32_t number)
{
	if (!ext4_fc_inode_valid(replay->allocation.fs, number)) {
		return EXT4_CORRUPT;
	}
	if (replay->inode_count == replay->inode_capacity) {
		return EXT4_UNSUPPORTED;
	}
	replay->inodes[replay->inode_count++].number = number;
	return EXT4_OK;
}

static enum ext4_result
ext4_fc_inode_buffer(struct ext4_fc_replay *replay, uint32_t number, struct ext4_inode_disk **disk)
{
	struct ext4_allocation *allocation = &replay->allocation;
	struct ext4_fs *fs = allocation->fs;
	void *buffer = NULL;
	uint64_t offset;
	enum ext4_result error;

	if (!ext4_fc_inode_valid(fs, number)) {
		return EXT4_CORRUPT;
	}
	error = ext4_inode_location(fs, number, &offset);
	if (error == EXT4_OK) {
		error = ext4_transaction_buffer(
		    allocation->transaction, offset / fs->info.block_size, &buffer);
	}
	if (error == EXT4_OK) {
		*disk =
		    (struct ext4_inode_disk *)((uint8_t *)buffer + offset % fs->info.block_size);
	}
	return error;
}

static enum ext4_result
ext4_fc_inode_get(struct ext4_fc_replay *replay, uint32_t number, struct ext4_inode_disk **disk,
    struct ext4_inode *inode)
{
	struct ext4_fc_inode_state *state;
	enum ext4_result error;

	error = ext4_fc_inode_buffer(replay, number, disk);
	if (error == EXT4_OK) {
		error = ext4_inode_decode_orphan(replay->allocation.fs, number, *disk, inode);
	}
	if (error == EXT4_OK) {
		state = ext4_fc_inode_state(replay, number);
		if (state == NULL) {
			return EXT4_UNSUPPORTED;
		}
		error = ext4_inode_writable(replay->allocation.fs, *disk, inode);
	}
	return error;
}

static enum ext4_result
ext4_fc_inode_finish(
    struct ext4_fc_replay *replay, struct ext4_inode_disk *disk, struct ext4_inode *inode)
{
	enum ext4_result error;

	error = ext4_write_map_recount(&replay->allocation, inode, disk);
	if (error == EXT4_OK) {
		ext4_inode_checksum_set(replay->allocation.fs, inode->number, disk);
	}
	return error;
}

static enum ext4_result
ext4_fc_forget_orphan(struct ext4_fc_replay *replay, uint32_t number)
{
	struct ext4_fs *fs = replay->allocation.fs;
	struct ext4_inode_disk *disk;
	struct ext4_inode_disk *previous = NULL;
	struct ext4_inode inode;
	uint32_t previous_number = 0;
	uint32_t cursor;
	uint32_t visited = 0;
	bool removed;
	enum ext4_result error;

	error = ext4_orphan_file_remove(&replay->allocation, number, &removed);
	if (error != EXT4_OK) {
		return error;
	}
	if (removed) {
		if (replay->orphan_slots_removed == fs->orphan_file->pending) {
			return EXT4_CORRUPT;
		}
		replay->orphan_slots_removed++;
		return EXT4_OK;
	}
	error = ext4_allocation_super(&replay->allocation);
	if (error != EXT4_OK) {
		return error;
	}
	cursor = ext4_le32(&replay->allocation.super->last_orphan);
	while (cursor != 0) {
		if (++visited > fs->info.inodes) {
			return EXT4_CORRUPT;
		}
		if (visited > EXT4_RECOVERY_MAX_RECORDS) {
			return EXT4_UNSUPPORTED;
		}
		error = ext4_fc_inode_buffer(replay, cursor, &disk);
		if (error == EXT4_OK) {
			error = ext4_inode_decode_orphan(fs, cursor, disk, &inode);
		}
		if (error != EXT4_OK) {
			return error;
		}
		if (cursor == number) {
			if (previous == NULL) {
				ext4_encode32(&replay->allocation.super->last_orphan,
				    ext4_le32(&disk->deletion_time));
			} else {
				ext4_encode32(
				    &previous->deletion_time, ext4_le32(&disk->deletion_time));
				ext4_inode_checksum_set(fs, previous_number, previous);
			}
			return EXT4_OK;
		}
		previous = disk;
		previous_number = cursor;
		cursor = ext4_le32(&disk->deletion_time);
	}
	return EXT4_OK;
}

static enum ext4_result
ext4_fc_inode_reuse(struct ext4_fc_replay *replay, struct ext4_fc_inode_state *state,
    struct ext4_inode_disk *disk, struct ext4_inode *inode, uint16_t mode)
{
	uint16_t type = inode->mode & EXT4_MODE_TYPE;
	bool created;
	enum ext4_result error;

	/* A new generation needs a preceding last unlink, not just a different
	 * number in an otherwise well-checksummed inode record. */
	if (!state->unlinked || inode->links != 0) {
		return EXT4_CORRUPT;
	}
	/* Remove the old orphan linkage while its inode checksum still describes
	 * the checkpointed generation. All later reclamation remains private. */
	error = ext4_fc_forget_orphan(replay, inode->number);
	if (error == EXT4_OK) {
		error = ext4_write_map_validate(&replay->allocation, inode, disk);
	}
	if (error == EXT4_OK && !(inode->flags & EXT4_INODE_INLINE_DATA) &&
	    (type == EXT4_MODE_REGULAR || type == EXT4_MODE_DIRECTORY ||
		(type == EXT4_MODE_SYMLINK && !inode->fast_symlink))) {
		error = ext4_write_map_truncate(&replay->allocation, inode, disk, 0);
	}
	if (error == EXT4_OK && ext4_inode_has_xattrs(replay->allocation.fs, disk)) {
		/* The old generation cannot retain attribute cleanup across this
		 * replacement. Release its references in the same private transaction. */
		error = ext4_xattr_drop_all(&replay->allocation, inode, disk);
	}
	if (error == EXT4_OK) {
		error = ext4_free_inode(&replay->allocation, disk, inode);
	}
	if (error == EXT4_OK) {
		error = ext4_inode_claim(&replay->allocation, inode->number, mode, &created);
		if (error == EXT4_OK && !created) {
			error = EXT4_CORRUPT;
		}
	}
	if (error == EXT4_OK) {
		state->created = true;
		state->directory_initialized = false;
		state->unlinked = false;
	}
	return error;
}

static enum ext4_result
ext4_fc_apply_inode(
    struct ext4_fc_replay *replay, const struct ext4_fc_inode_disk *record, uint32_t length)
{
	struct ext4_fs *fs = replay->allocation.fs;
	const struct ext4_inode_disk *logged = (const struct ext4_inode_disk *)(record + 1);
	struct ext4_inode_disk *disk;
	struct ext4_extent_header_disk *header;
	struct ext4_inode inode;
	struct ext4_fc_inode_state *state;
	uint32_t number = ext4_le32(&record->inode);
	uint32_t flags = ext4_le32(&logged->flags);
	uint32_t orphan_next = 0;
	uint32_t directory_flags = 0;
	uint32_t index;
	uint64_t directory_size = 0;
	uint16_t extra = 0;
	uint16_t mode = ext4_le16(&logged->mode);
	uint16_t type = mode & EXT4_MODE_TYPE;
	bool created;
	bool extent_root;
	size_t after_map = offsetof(struct ext4_inode_disk, generation);
	enum ext4_result error;

	length -= sizeof(*record);
	if (fs->inode_size > EXT4_INODE_BASE_SIZE) {
		if (length <
		    offsetof(struct ext4_inode_disk, extra_size) + sizeof(logged->extra_size)) {
			return EXT4_CORRUPT;
		}
		extra = ext4_le16(&logged->extra_size);
	}
	if ((extra & 3U) || extra > fs->inode_size - EXT4_INODE_BASE_SIZE ||
	    length < EXT4_INODE_BASE_SIZE + extra || !ext4_fc_inode_valid(fs, number) ||
	    mode == 0) {
		return EXT4_CORRUPT;
	}
	error = ext4_fc_inode_buffer(replay, number, &disk);
	if (error != EXT4_OK) {
		return error;
	}
	error = ext4_inode_claim(&replay->allocation, number, mode, &created);
	if (error != EXT4_OK) {
		return error;
	}
	state = ext4_fc_inode_state(replay, number);
	if (state == NULL) {
		return EXT4_UNSUPPORTED;
	}
	state->created |= created;
	if (!created) {
		error = ext4_inode_decode_orphan(fs, number, disk, &inode);
		if (error != EXT4_OK) {
			return error;
		}
		if (inode.generation != ext4_le32(&logged->generation)) {
			error = ext4_fc_inode_reuse(replay, state, disk, &inode, mode);
			if (error != EXT4_OK) {
				return error;
			}
			created = true;
		} else if ((inode.mode & EXT4_MODE_TYPE) != (mode & EXT4_MODE_TYPE)) {
			return EXT4_CORRUPT;
		} else if (((inode.flags ^ flags) & EXT4_INODE_EXTENTS) &&
		    !((inode.flags | flags) & EXT4_INODE_INLINE_DATA)) {
			/* A format transition needs its own conversion; neither root can
			 * be interpreted as the other mapping representation. */
			return EXT4_UNSUPPORTED;
		} else if ((mode & EXT4_MODE_TYPE) == EXT4_MODE_DIRECTORY) {
			directory_size = inode.size;
			directory_flags = inode.flags & EXT4_INODE_INDEX;
		}
		orphan_next = created ? 0 : ext4_le32(&disk->deletion_time);
	}
	if (created) {
		ext4_zero(disk, fs->inode_size);
	}
	extent_root = !created && (ext4_le32(&disk->flags) & EXT4_INODE_EXTENTS);
	/* Fast records describe metadata and logical ranges. The logged mapping
	 * root belongs to a different allocation history and is never installed. */
	ext4_copy(disk, logged, offsetof(struct ext4_inode_disk, block_data));
	ext4_copy(
	    (uint8_t *)disk + after_map, (const uint8_t *)logged + after_map, length - after_map);
	/* The ordinary journal owns any existing orphan chain. Retain its
	 * linkage until explicit cleanup completes after semantic replay. */
	ext4_encode32(&disk->deletion_time, orphan_next);
	if ((mode & EXT4_MODE_TYPE) == EXT4_MODE_DIRECTORY && !(flags & EXT4_INODE_INLINE_DATA)) {
		/* Directory blocks are reconstructed from names. A logged size or
		 * index flag describes Linux's old layout, not our current tree. */
		flags = (flags & ~(uint32_t)EXT4_INODE_INDEX) | directory_flags;
		ext4_encode32(&disk->flags, flags);
		ext4_encode32(&disk->size_lo, (uint32_t)directory_size);
		ext4_encode32(&disk->size_hi, (uint32_t)(directory_size >> 32));
	}
	if (flags & EXT4_INODE_EXTENTS) {
		if (!extent_root) {
			ext4_zero(disk->block_data, sizeof(disk->block_data));
			header = (struct ext4_extent_header_disk *)disk->block_data;
			ext4_encode16(&header->magic, EXT4_EXTENT_MAGIC);
			ext4_encode16(&header->maximum,
			    (sizeof(disk->block_data) - sizeof(*header)) /
				sizeof(struct ext4_extent_disk));
		}
	} else if (flags & EXT4_INODE_INLINE_DATA) {
		ext4_copy(disk->block_data, logged->block_data, sizeof(disk->block_data));
	} else {
		switch (type) {
		case EXT4_MODE_REGULAR:
		case EXT4_MODE_DIRECTORY:
			/* Retain the checkpointed indirect root, or the empty new root. */
			break;
		case EXT4_MODE_SYMLINK:
			/* The logged charge distinguishes embedded targets from mapped
			 * links, including mapped targets shorter than the inode payload. */
			ext4_inode_checksum_set(fs, number, disk);
			error = ext4_inode_decode_orphan(fs, number, disk, &inode);
			if (error != EXT4_OK) {
				return error;
			}
			if (inode.fast_symlink) {
				ext4_copy(
				    disk->block_data, logged->block_data, sizeof(disk->block_data));
			}
			break;
		case EXT4_MODE_CHARACTER:
		case EXT4_MODE_BLOCK:
		case EXT4_MODE_FIFO:
		case EXT4_MODE_SOCKET:
			/* These bytes hold device identity, not
			 * allocation pointers belonging to the pre-crash mapping tree. */
			ext4_copy(disk->block_data, logged->block_data, sizeof(disk->block_data));
			break;
		default:
			return EXT4_UNSUPPORTED;
		}
	}
	ext4_inode_checksum_set(fs, number, disk);
	error = ext4_inode_decode_orphan(fs, number, disk, &inode);
	if (error != EXT4_OK) {
		return error;
	}
	if (type == EXT4_MODE_SYMLINK) {
		if (inode.size == 0 || inode.size >= fs->info.block_size) {
			return EXT4_CORRUPT;
		}
		if (inode.fast_symlink) {
			if (disk->block_data[inode.size] != 0) {
				return EXT4_CORRUPT;
			}
			for (index = 0; index < inode.size; index++) {
				if (disk->block_data[index] == 0) {
					return EXT4_CORRUPT;
				}
			}
		}
	} else if (type != EXT4_MODE_REGULAR && type != EXT4_MODE_DIRECTORY && inode.size != 0) {
		return EXT4_CORRUPT;
	}
	error = ext4_inode_writable(fs, disk, &inode);
	/* A newly logged long symlink has no local range until ADD_RANGE follows.
	 * Keep its logged block count through that private intermediate state;
	 * zero would misclassify it as a short symlink before its data is claimed.
	 * Range replay and the final inode pass still recount actual ownership. */
	if (error == EXT4_OK && type == EXT4_MODE_SYMLINK && !inode.fast_symlink) {
		return EXT4_OK;
	}
	return error == EXT4_OK ? ext4_fc_inode_finish(replay, disk, &inode) : error;
}

static enum ext4_result
ext4_fc_symlink_validate(struct ext4_fc_replay *replay, const struct ext4_inode_disk *disk,
    const struct ext4_inode *inode)
{
	struct ext4_map_run run;
	const uint8_t *target = disk->block_data;
	uint32_t index;
	enum ext4_result error;

	if ((inode->mode & EXT4_MODE_TYPE) != EXT4_MODE_SYMLINK) {
		return EXT4_OK;
	}
	if (inode->size == 0 || inode->size >= replay->allocation.fs->info.block_size) {
		return EXT4_CORRUPT;
	}
	if (!inode->fast_symlink) {
		error = ext4_write_map_lookup(&replay->allocation, inode, disk, 0, &run);
		if (error != EXT4_OK) {
			return error;
		}
		if (run.physical == 0 || run.unwritten) {
			return EXT4_CORRUPT;
		}
		error = ext4_transaction_read(
		    replay->allocation.transaction, run.physical, replay->buffer);
		if (error != EXT4_OK) {
			return error;
		}
		target = replay->buffer;
	}
	if (target[inode->size] != 0) {
		return EXT4_CORRUPT;
	}
	for (index = 0; index < inode->size; index++) {
		if (target[index] == 0) {
			return EXT4_CORRUPT;
		}
	}
	return EXT4_OK;
}

static enum ext4_result
ext4_fc_range(const struct ext4_fc_add_disk *record, struct ext4_block_range *range,
    uint32_t *logical, uint32_t *length, bool *unwritten)
{
	uint16_t encoded = ext4_le16(&record->extent.length);

	*logical = ext4_le32(&record->extent.logical);
	*unwritten = encoded > EXT4_EXTENT_UNWRITTEN_LIMIT;
	*length = *unwritten ? encoded - EXT4_EXTENT_UNWRITTEN_LIMIT : encoded;
	range->first = ext4_le32(&record->extent.physical_lo) |
	    (uint64_t)ext4_le16(&record->extent.physical_hi) << 32;
	range->length = *length;
	return *length == 0 || (uint64_t)*logical + *length > (uint64_t)UINT32_MAX + 1U
	    ? EXT4_CORRUPT
	    : EXT4_OK;
}

static enum ext4_result
ext4_fc_apply_range(struct ext4_fc_replay *replay, uint16_t type, const void *value)
{
	const struct ext4_fc_delete_disk *deleted = value;
	const struct ext4_fc_add_disk *added = value;
	struct ext4_block_range range = { 0, 0 };
	struct ext4_inode_disk *disk;
	struct ext4_inode inode;
	uint32_t number = ext4_le32(&deleted->inode);
	uint32_t logical;
	uint32_t length;
	bool unwritten = false;
	enum ext4_result error;

	if (type == EXT4_FC_ADD_RANGE) {
		error = ext4_fc_range(added, &range, &logical, &length, &unwritten);
		if (error != EXT4_OK) {
			return error;
		}
	} else {
		logical = ext4_le32(&deleted->logical);
		length = ext4_le32(&deleted->blocks);
	}
	error = ext4_fc_inode_get(replay, number, &disk, &inode);
	if (error == EXT4_OK) {
		error = ext4_write_map_replay(
		    &replay->allocation, &inode, disk, logical, range.first, length, unwritten);
	}
	return error == EXT4_OK ? ext4_fc_inode_finish(replay, disk, &inode) : error;
}

static enum ext4_file_type
ext4_fc_type(uint16_t mode)
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
ext4_fc_apply_name(struct ext4_fc_replay *replay, uint16_t type,
    const struct ext4_fc_name_disk *record, uint32_t length)
{
	struct ext4_fs *fs = replay->allocation.fs;
	struct ext4_inode_disk *parent_disk;
	struct ext4_inode_disk *child_disk;
	struct ext4_inode parent;
	struct ext4_inode child;
	struct ext4_directory_slot slot;
	struct ext4_fc_inode_state *state;
	const uint8_t *name = (const uint8_t *)(record + 1);
	uint32_t index;
	bool directory;
	bool change_parent_links = false;
	enum ext4_result error;

	length -= sizeof(*record);
	if ((length == 1 && name[0] == '.') || (length == 2 && name[0] == '.' && name[1] == '.')) {
		return EXT4_CORRUPT;
	}
	for (index = 0; index < length; index++) {
		if (name[index] == 0 || name[index] == '/') {
			return EXT4_CORRUPT;
		}
	}
	error = ext4_fc_inode_get(replay, ext4_le32(&record->parent), &parent_disk, &parent);
	if (error != EXT4_OK) {
		return error;
	}
	if ((parent.mode & EXT4_MODE_TYPE) != EXT4_MODE_DIRECTORY) {
		return EXT4_CORRUPT;
	}
	error = ext4_fc_inode_get(replay, ext4_le32(&record->inode), &child_disk, &child);
	if (error != EXT4_OK) {
		return error;
	}
	directory = (child.mode & EXT4_MODE_TYPE) == EXT4_MODE_DIRECTORY;
	state = ext4_fc_inode_state(replay, child.number);
	if (state == NULL) {
		return EXT4_UNSUPPORTED;
	}
	if (type == EXT4_FC_CREATE && directory && state->created &&
	    !state->directory_initialized) {
		/* Newly logged directories carry their namespace as semantic records. */
		error = ext4_directory_initialize(
		    &replay->allocation, &child, child_disk, parent.number);
		if (error != EXT4_OK) {
			return error;
		}
		error = ext4_fc_inode_finish(replay, child_disk, &child);
		if (error != EXT4_OK) {
			return error;
		}
		state->directory_initialized = true;
		replay->allocation.allocated = 0;
		replay->allocation.freed = 0;
	}
	error = ext4_directory_scan(&replay->allocation, &parent, parent_disk, name, length,
	    type == EXT4_FC_UNLINK ? EXT4_DIRECTORY_FIND : EXT4_DIRECTORY_INSERT, 0, &slot);
	if (type == EXT4_FC_UNLINK) {
		if (error == EXT4_NOT_FOUND) {
			return EXT4_OK;
		}
		if (error != EXT4_OK || slot.number != child.number) {
			return error == EXT4_OK ? EXT4_OK : error;
		}
		error = ext4_directory_remove(&replay->allocation, &parent, &slot);
		if (error == EXT4_OK && child.links != 0) {
			ext4_encode16(&child_disk->links, directory ? 0 : child.links - 1U);
			state->unlinked = directory || child.links == 1;
			change_parent_links = directory;
		}
	} else if (error == EXT4_EXISTS) {
		/* A previous interrupted replay may already have installed this name. */
		if (slot.number != child.number) {
			return EXT4_CORRUPT;
		}
		error = EXT4_OK;
	} else if (error == EXT4_OK) {
		error = ext4_directory_insert(&replay->allocation, &parent, parent_disk, &slot,
		    child.number, ext4_fc_type(child.mode), name, length);
		change_parent_links = directory;
		if (error == EXT4_OK && type == EXT4_FC_LINK) {
			if (child.links == EXT4_LINK_MAX) {
				return EXT4_CORRUPT;
			}
			ext4_encode16(&child_disk->links, child.links + 1U);
		}
	}
	if (error == EXT4_OK) {
		if (change_parent_links) {
			if (parent.links == 1 && (parent.flags & EXT4_INODE_INDEX) &&
			    (fs->info.feature_ro_compat & EXT4_FEATURE_RO_DIR_NLINK)) {
				/* Indexed directories may already have an unknown link count. */
			} else if (parent.links < 2) {
				return EXT4_CORRUPT;
			} else if (type == EXT4_FC_UNLINK) {
				ext4_encode16(&parent_disk->links, parent.links - 1U);
			} else if (parent.links == EXT4_LINK_MAX) {
				if (!(ext4_le32(&parent_disk->flags) & EXT4_INODE_INDEX) ||
				    !(fs->info.feature_ro_compat & EXT4_FEATURE_RO_DIR_NLINK)) {
					return EXT4_CORRUPT;
				}
				ext4_encode16(&parent_disk->links, 1);
			} else {
				ext4_encode16(&parent_disk->links, parent.links + 1U);
			}
		}
		ext4_inode_checksum_set(fs, parent.number, parent_disk);
		ext4_inode_checksum_set(fs, child.number, child_disk);
	}
	return error;
}

static enum ext4_result
ext4_fc_prepare(struct ext4_fc_replay *replay)
{
	struct ext4_fs *fs = replay->allocation.fs;
	const struct ext4_fc_add_disk *record;
	const struct ext4_fc_name_disk *name;
	const struct ext4_le32 *number;
	const void *value;
	struct ext4_block_range *range;
	uint32_t index;
	uint32_t logical;
	uint32_t length;
	bool unwritten;
	uint16_t type;
	uint32_t cached_block = UINT32_MAX;
	enum ext4_result error;

	for (index = 0; index < replay->log->count; index++) {
		type = replay->log->records[index].type;
		if (type == EXT4_FC_HEAD || type == EXT4_FC_TAIL || type == EXT4_FC_PAD) {
			continue;
		}
		error = ext4_fast_commit_read_cached(
		    replay->log, index, replay->buffer, &cached_block, &value);
		if (error != EXT4_OK) {
			return error;
		}
		if (type == EXT4_FC_CREATE || type == EXT4_FC_LINK || type == EXT4_FC_UNLINK) {
			name = value;
			error = ext4_fc_inode_collect(replay, ext4_le32(&name->parent));
			number = &name->inode;
		} else {
			/* INODE, ADD_RANGE and DEL_RANGE all begin with the inode number. */
			number = value;
		}
		if (error == EXT4_OK) {
			error = ext4_fc_inode_collect(replay, ext4_le32(number));
		}
		if (error != EXT4_OK) {
			return error;
		}
		if (type != EXT4_FC_ADD_RANGE) {
			continue;
		}
		record = value;
		range = &replay->excluded[replay->allocation.excluded_count];
		error = ext4_fc_range(record, range, &logical, &length, &unwritten);
		if (error != EXT4_OK || !ext4_fc_inode_valid(fs, ext4_le32(&record->inode)) ||
		    range->first == 0 || range->first >= fs->info.blocks ||
		    range->length > fs->info.blocks - range->first ||
		    range->first % fs->cluster_blocks != logical % fs->cluster_blocks ||
		    ext4_system_overlaps(fs, range->first, range->length)) {
			return EXT4_CORRUPT;
		}
		replay->allocation.excluded_count++;
	}
	/* Only the lookup indexes are reordered. Semantic records still execute
	 * in their committed order, including generation reuse and repeated ranges. */
	ext4_fc_inodes_sort(replay);
	ext4_ranges_union(replay->excluded, &replay->allocation.excluded_count);
	replay->allocation.excluded = replay->excluded;
	return EXT4_OK;
}

static enum ext4_result
ext4_fc_orphan_enroll(
    struct ext4_fc_replay *replay, const struct ext4_inode *inode, struct ext4_inode_disk *disk)
{
	struct ext4_fs *fs = replay->allocation.fs;
	struct ext4_inode_disk *chain_disk;
	struct ext4_inode chain_inode;
	const struct ext4_le32 *entries;
	uint32_t cursor;
	uint32_t visited = 0;
	uint32_t block;
	uint32_t slot;
	uint32_t slots =
	    (fs->info.block_size - sizeof(struct ext4_orphan_tail_disk)) / sizeof(struct ext4_le32);
	enum ext4_result error;

	error = ext4_orphan_reserve(fs, inode, disk);
	if (error == EXT4_OK) {
		error = ext4_allocation_super(&replay->allocation);
	}
	if (error != EXT4_OK) {
		return error;
	}
	cursor = ext4_le32(&replay->allocation.super->last_orphan);
	while (cursor != 0) {
		if (++visited > fs->info.inodes) {
			return EXT4_CORRUPT;
		}
		if (visited > EXT4_RECOVERY_MAX_RECORDS) {
			return EXT4_UNSUPPORTED;
		}
		if (cursor == inode->number) {
			return EXT4_OK;
		}
		error = ext4_fc_inode_buffer(replay, cursor, &chain_disk);
		if (error == EXT4_OK) {
			error = ext4_inode_decode_orphan(fs, cursor, chain_disk, &chain_inode);
		}
		if (error != EXT4_OK) {
			return error;
		}
		cursor = ext4_le32(&chain_disk->deletion_time);
	}
	if (fs->orphan_file != NULL) {
		for (block = 0; block < fs->orphan_file->block_count; block++) {
			error = ext4_transaction_read(replay->allocation.transaction,
			    fs->orphan_file->blocks[block], replay->buffer);
			if (error != EXT4_OK) {
				return error;
			}
			entries = (const struct ext4_le32 *)replay->buffer;
			for (slot = 0; slot < slots; slot++) {
				if (ext4_le32(&entries[slot]) == inode->number) {
					return EXT4_OK;
				}
			}
		}
	}
	ext4_encode32(&disk->deletion_time, ext4_le32(&replay->allocation.super->last_orphan));
	ext4_encode32(&replay->allocation.super->last_orphan, inode->number);
	ext4_inode_checksum_set(fs, inode->number, disk);
	return EXT4_OK;
}

enum ext4_result
ext4_fast_commit_replay(struct ext4_fast_commit *log)
{
	struct ext4_fs *fs = log->journal->fs;
	struct ext4_fc_replay *replay;
	struct ext4_transaction *transaction = NULL;
	struct ext4_super_disk *super = NULL;
	struct ext4_group group;
	struct ext4_inode_disk *disk;
	struct ext4_inode inode;
	struct ext4_inode root;
	const void *value;
	uint64_t free_blocks = 0;
	uint64_t free_inodes = 0;
	uint32_t index;
	uint32_t orphan_head;
	uint32_t cached_block = UINT32_MAX;
	uint16_t type;
	bool allocation_ready = false;
	enum ext4_result error;

	if (log->count == 0) {
		return EXT4_OK;
	}
	/* Linux may leave the primary summaries behind the ordinary committed
	 * prefix. Derive the baseline in memory; persist it in the same full
	 * conversion transaction as every fast-commit effect. */
	for (index = 0; index < fs->info.groups; index++) {
		error = ext4_group_get(fs, index, &group);
		if (error != EXT4_OK) {
			return error;
		}
		free_blocks += group.free_blocks;
		free_inodes += group.free_inodes;
	}
	if (free_blocks > fs->info.blocks || free_inodes > fs->info.inodes) {
		return EXT4_CORRUPT;
	}
	fs->info.free_blocks = free_blocks;
	fs->info.free_inodes = (uint32_t)free_inodes;
	error = ext4_system_ranges_build(fs);
	if (error == EXT4_OK) {
		error = ext4_orphan_validate(fs);
	}
	if (error != EXT4_OK) {
		return error;
	}
	error = ext4_get_inode(fs, EXT4_ROOT_INODE, &root);
	if (error != EXT4_OK) {
		return error;
	}
	replay = fs->environment.allocate(fs->environment.context, sizeof(*replay));
	if (replay == NULL) {
		return EXT4_NO_MEMORY;
	}
	ext4_zero(replay, sizeof(*replay));
	replay->log = log;
	replay->inode_capacity = log->count * 2U;
	replay->inodes = fs->environment.allocate(
	    fs->environment.context, (size_t)replay->inode_capacity * sizeof(*replay->inodes));
	replay->excluded = fs->environment.allocate(
	    fs->environment.context, (size_t)log->count * sizeof(*replay->excluded));
	replay->buffer = fs->environment.allocate(fs->environment.context, fs->info.block_size);
	if (replay->inodes == NULL || replay->excluded == NULL || replay->buffer == NULL) {
		error = EXT4_NO_MEMORY;
		goto out;
	}
	ext4_zero(replay->inodes, (size_t)replay->inode_capacity * sizeof(*replay->inodes));
	error = ext4_transaction_begin_recovery(
	    log->journal, log->sequence, ext4_journal_credits(log->journal), &transaction);
	if (error == EXT4_OK) {
		error = ext4_transaction_super(transaction, &super);
	}
	if (error != EXT4_OK) {
		goto out;
	}
	ext4_encode32(&super->free_blocks_lo, (uint32_t)free_blocks);
	if (fs->info.feature_incompat & EXT4_FEATURE_INCOMPAT_64BIT) {
		ext4_encode32(&super->free_blocks_hi, (uint32_t)(free_blocks >> 32));
	}
	ext4_encode32(&super->free_inodes, (uint32_t)free_inodes);
	if (fs->metadata_checksum) {
		ext4_encode32(&super->checksum,
		    ext4_crc32c(UINT32_MAX, super, offsetof(struct ext4_super_disk, checksum)));
	}
	error = ext4_allocation_init(&replay->allocation, fs, transaction, &root);
	if (error != EXT4_OK) {
		goto out;
	}
	allocation_ready = true;
	error = ext4_fc_prepare(replay);
	for (index = 0; error == EXT4_OK && index < log->count; index++) {
		error =
		    ext4_fast_commit_read_cached(log, index, replay->buffer, &cached_block, &value);
		if (error != EXT4_OK) {
			break;
		}
		replay->allocation.allocated = 0;
		replay->allocation.freed = 0;
		replay->allocation.unmapped = 0;
		replay->allocation.detached_shared_blocks = 0;
		replay->allocation.attribute_blocks_added = 0;
		replay->allocation.attribute_blocks_removed = 0;
		replay->allocation.mapping_size = 0;
		replay->allocation.bitmap = NULL;
		type = log->records[index].type;
		switch (type) {
		case EXT4_FC_INODE:
			error = ext4_fc_apply_inode(replay, value, log->records[index].length);
			break;
		case EXT4_FC_ADD_RANGE:
		case EXT4_FC_DEL_RANGE:
			error = ext4_fc_apply_range(replay, type, value);
			break;
		case EXT4_FC_CREATE:
		case EXT4_FC_LINK:
		case EXT4_FC_UNLINK:
			error = ext4_fc_apply_name(replay, type, value, log->records[index].length);
			break;
		default:
			break;
		}
	}
	for (index = 0; error == EXT4_OK && index < replay->inode_count; index++) {
		error = ext4_fc_inode_get(replay, replay->inodes[index].number, &disk, &inode);
		if (error == EXT4_OK) {
			error = ext4_fc_symlink_validate(replay, disk, &inode);
		}
		if (error == EXT4_OK) {
			error = ext4_fc_inode_finish(replay, disk, &inode);
		}
		if (error == EXT4_OK) {
			error = ext4_inode_decode_orphan(fs, inode.number, disk, &inode);
		}
		if (error == EXT4_OK && inode.links == 0) {
			error = ext4_fc_orphan_enroll(replay, &inode, disk);
		}
	}
	if (error == EXT4_OK) {
		orphan_head = ext4_le32(&super->last_orphan);
		error = ext4_transaction_commit(transaction);
		transaction = NULL;
		if (error == EXT4_OK) {
			fs->info.free_blocks = replay->allocation.free_blocks;
			fs->info.free_inodes = replay->allocation.free_inodes;
			fs->last_orphan = orphan_head;
			if (fs->orphan_file != NULL) {
				fs->orphan_file->pending -= replay->orphan_slots_removed;
			}
		}
	}
out:
	if (transaction != NULL) {
		if (error == EXT4_RANGE && ext4_transaction_capacity_failed(transaction)) {
			error = EXT4_UNSUPPORTED;
		}
		ext4_transaction_cancel(transaction);
	}
	if (allocation_ready) {
		ext4_allocation_destroy(&replay->allocation);
	}
	if (replay->inodes != NULL) {
		fs->environment.release(fs->environment.context, replay->inodes,
		    (size_t)replay->inode_capacity * sizeof(*replay->inodes));
	}
	if (replay->excluded != NULL) {
		fs->environment.release(fs->environment.context, replay->excluded,
		    (size_t)log->count * sizeof(*replay->excluded));
	}
	if (replay->buffer != NULL) {
		fs->environment.release(
		    fs->environment.context, replay->buffer, fs->info.block_size);
	}
	fs->environment.release(fs->environment.context, replay, sizeof(*replay));
	return error;
}

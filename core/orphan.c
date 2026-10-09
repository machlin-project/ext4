/* SPDX-License-Identifier: BSD-3-Clause */
#include "allocate.h"
#include "xattr.h"
#include "inline.h"
#include "transaction.h"

#define EXT4_ORPHAN_MAPPED_CREDITS (3U * (2U * EXT4_EXTENT_MAX_DEPTH + 1U) + 6U)
#define EXT4_ORPHAN_UNMAPPED_CREDITS 5U
#define EXT4_ORPHAN_XATTR_CREDITS 3U
/* A reclamation step releases up to half the inode's remaining blocks, as a power
 * of two from EXT4_ORPHAN_BATCH_BLOCKS to this bound. A step exceeding its credits
 * halves and retries, down to the single block the reserve guarantees. */
#define EXT4_ORPHAN_STEP_MAX_BLOCKS 65536U

enum ext4_result
ext4_orphan_reserve(
    struct ext4_fs *fs, const struct ext4_inode *inode, const struct ext4_inode_disk *disk)
{
	uint64_t units = ext4_le32(&disk->blocks_lo);
	uint64_t attribute_units;
	uint16_t type = inode->mode & EXT4_MODE_TYPE;
	uint32_t credits;
	bool external =
	    ext4_le32(&disk->xattr_block_lo) != 0 || ext4_le16(&disk->xattr_block_hi) != 0;
	bool mapped = type == EXT4_MODE_REGULAR || type == EXT4_MODE_DIRECTORY ||
	    (type == EXT4_MODE_SYMLINK && !inode->fast_symlink);

	if (fs->info.feature_ro_compat & EXT4_FEATURE_RO_HUGE_FILE) {
		units |= (uint64_t)ext4_le16(&disk->blocks_hi) << 32;
	}
	attribute_units = !external ? 0
	    : ext4_le32(&disk->flags) & EXT4_INODE_HUGE_FILE
	    ? 1
	    : fs->info.block_size / EXT4_SECTOR_SIZE;
	if (units < attribute_units) {
		return EXT4_CORRUPT;
	}
	credits = mapped && units > attribute_units ? EXT4_ORPHAN_MAPPED_CREDITS
						    : EXT4_ORPHAN_UNMAPPED_CREDITS;
	if (external) {
		credits += EXT4_ORPHAN_XATTR_CREDITS;
	}
	return ext4_journal_credits(fs->journal) < credits ? EXT4_RANGE : EXT4_OK;
}

static uint32_t
ext4_orphan_slots(const struct ext4_fs *fs)
{
	return (fs->info.block_size - sizeof(struct ext4_orphan_tail_disk)) /
	    sizeof(struct ext4_le32);
}

static uint32_t
ext4_orphan_file_checksum(struct ext4_fs *fs, uint32_t logical, const void *buffer)
{
	struct ext4_orphan_file *file = fs->orphan_file;
	struct ext4_block_number_disk address;
	uint32_t checksum;

	ext4_encode32(&address.low, (uint32_t)file->blocks[logical]);
	ext4_encode32(&address.high, (uint32_t)(file->blocks[logical] >> 32));
	checksum = ext4_crc32c(ext4_inode_seed(fs, &file->inode), &address, sizeof(address));
	return ext4_crc32c(
	    checksum, buffer, fs->info.block_size - sizeof(struct ext4_orphan_tail_disk));
}

static enum ext4_result
ext4_orphan_file_block(struct ext4_fs *fs, uint32_t logical, const void *buffer)
{
	const struct ext4_orphan_tail_disk *tail =
	    (const struct ext4_orphan_tail_disk *)((const uint8_t *)buffer + fs->info.block_size -
		sizeof(*tail));

	if (ext4_le32(&tail->magic) != EXT4_ORPHAN_MAGIC ||
	    (fs->metadata_checksum &&
		(ext4_le32(&tail->checksum) != ext4_orphan_file_checksum(fs, logical, buffer)))) {
		return EXT4_CORRUPT;
	}
	return EXT4_OK;
}

/* Inode numbers are unique across the orphan file. Physical slots remain stable
 * for this preallocated-file writer; growth has a separate ownership contract. */
static uint32_t
ext4_orphan_find(const struct ext4_orphan_file *file, uint32_t number)
{
	uint32_t low = 0;
	uint32_t high = file->pending;
	uint32_t middle;

	while (low < high) {
		middle = low + (high - low) / 2U;
		if (file->slots[middle].number < number) {
			low = middle + 1U;
		} else {
			high = middle;
		}
	}
	return low;
}

static void
ext4_orphan_sift(struct ext4_orphan_slot *slots, uint32_t count, uint32_t root)
{
	struct ext4_orphan_slot value = slots[root];
	uint32_t child;

	while (root < count / 2U) {
		child = root * 2U + 1U;
		if (child + 1U < count && slots[child].number < slots[child + 1U].number) {
			child++;
		}
		if (value.number >= slots[child].number) {
			break;
		}
		slots[root] = slots[child];
		root = child;
	}
	slots[root] = value;
}

static void
ext4_orphan_sort(struct ext4_orphan_slot *slots, uint32_t count)
{
	struct ext4_orphan_slot value;
	uint32_t index;

	for (index = count / 2U; index != 0; index--) {
		ext4_orphan_sift(slots, count, index - 1U);
	}
	for (index = count; index > 1U; index--) {
		value = slots[0];
		slots[0] = slots[index - 1U];
		slots[index - 1U] = value;
		ext4_orphan_sift(slots, index - 1U, 0);
	}
}

static enum ext4_result
ext4_orphan_index_reserve(struct ext4_fs *fs, uint32_t count)
{
	struct ext4_orphan_file *file = fs->orphan_file;
	struct ext4_orphan_slot *slots;
	uint32_t capacity = file->slot_capacity == 0 ? 16U : file->slot_capacity;

	if (count > EXT4_ORPHAN_FILE_MAX_ENTRIES) {
		return EXT4_UNSUPPORTED;
	}
	if (count <= file->slot_capacity) {
		return EXT4_OK;
	}
	while (capacity < count) {
		capacity *= 2U;
	}
	slots = fs->environment.allocate(fs->environment.context, capacity * sizeof(*slots));
	if (slots == NULL) {
		return EXT4_NO_MEMORY;
	}
	if (file->slots != NULL) {
		ext4_copy(slots, file->slots, file->pending * sizeof(*slots));
		fs->environment.release(fs->environment.context, file->slots,
		    file->slot_capacity * sizeof(*slots));
	}
	file->slots = slots;
	file->slot_capacity = capacity;
	return EXT4_OK;
}

/* Build an index delta from private snapshots. Reserving capacity can change only
 * storage, never membership; refusal leaves the live orphan set unchanged. */
enum ext4_result
ext4_orphan_transaction_prepare(struct ext4_transaction *transaction)
{
	struct ext4_fs *fs = transaction->journal->fs;
	struct ext4_orphan_file *file = fs->orphan_file;
	struct ext4_orphan_slot *changes;
	const struct ext4_le32 *entries;
	const struct ext4_le32 *previous;
	const struct ext4_orphan_slot *reference;
	uint32_t removed = 0;
	uint32_t added = 0;
	uint32_t logical;
	uint32_t slot;
	uint32_t number;
	uint32_t position;
	uint32_t index;
	uint32_t pass;
	enum ext4_result error;

	if (file == NULL || !transaction->orphan_touched) {
		return EXT4_OK;
	}
	/* Count once, allocate exactly once, then record the same immutable delta. */
	for (pass = 0; pass < 2; pass++) {
		removed = 0;
		added = 0;
		for (index = 0; index < file->pending; index++) {
			reference = &file->slots[index];
			entries = ext4_transaction_peek(
			    transaction, file->blocks[reference->logical]);
			if (entries != NULL &&
			    ext4_le32(&entries[reference->slot]) != reference->number) {
				if (pass != 0) {
					transaction->orphan_changes[removed] = *reference;
				}
				removed++;
			}
		}
		for (logical = 0; logical < file->block_count; logical++) {
			entries = ext4_transaction_peek(transaction, file->blocks[logical]);
			if (entries == NULL) {
				continue;
			}
			error = ext4_orphan_file_block(fs, logical, entries);
			if (error != EXT4_OK) {
				return error;
			}
			for (slot = 0; slot < ext4_orphan_slots(fs); slot++) {
				number = ext4_le32(&entries[slot]);
				if (number == 0) {
					continue;
				}
				if (number < fs->first_inode || number > fs->info.inodes ||
				    number == fs->journal_inode ||
				    number == fs->orphan_file_inode) {
					return EXT4_CORRUPT;
				}
				position = ext4_orphan_find(file, number);
				if (position < file->pending &&
				    file->slots[position].number == number) {
					reference = &file->slots[position];
					if (reference->logical == logical &&
					    reference->slot == slot) {
						continue;
					}
					previous = ext4_transaction_peek(transaction,
					    file->blocks[reference->logical]);
					if (previous == NULL ||
					    ext4_le32(&previous[reference->slot]) == number) {
						return EXT4_CORRUPT;
					}
				}
				if (added == EXT4_ORPHAN_FILE_MAX_ENTRIES) {
					return EXT4_UNSUPPORTED;
				}
				if (pass != 0) {
					transaction->orphan_changes[removed + added] =
					    (struct ext4_orphan_slot){ number, logical, slot };
				}
				added++;
			}
		}
		if (pass == 0) {
			if (removed == 0 && added == 0) {
				return EXT4_OK;
			}
			error = ext4_orphan_index_reserve(fs, file->pending - removed + added);
			if (error != EXT4_OK) {
				return error;
			}
			changes = fs->environment.allocate(fs->environment.context,
			    (size_t)(removed + added) * sizeof(*changes));
			if (changes == NULL) {
				return EXT4_NO_MEMORY;
			}
			transaction->orphan_changes = changes;
			transaction->orphan_removed = removed;
			transaction->orphan_added = added;
		}
	}
	ext4_orphan_sort(transaction->orphan_changes + removed, added);
	for (index = 1; index < added; index++) {
		if (transaction->orphan_changes[removed + index - 1U].number ==
		    transaction->orphan_changes[removed + index].number) {
			return EXT4_CORRUPT;
		}
	}
	return EXT4_OK;
}

void
ext4_orphan_transaction_cancel(struct ext4_transaction *transaction)
{
	struct ext4_fs *fs = transaction->journal->fs;

	if (transaction->orphan_changes != NULL) {
		fs->environment.release(fs->environment.context, transaction->orphan_changes,
		    (size_t)(transaction->orphan_removed + transaction->orphan_added) *
			sizeof(*transaction->orphan_changes));
		transaction->orphan_changes = NULL;
	}
}

/* No fallible work remains. A retained compound already published its index when
 * it accepted the operation, so later durable commits carry no index delta. */
void
ext4_orphan_transaction_publish(struct ext4_transaction *transaction)
{
	struct ext4_orphan_file *file = transaction->journal->fs->orphan_file;
	const struct ext4_orphan_slot *change;
	uint32_t position;
	uint32_t index;
	uint32_t move;

	if (transaction->orphan_changes == NULL) {
		return;
	}
	for (index = 0; index < transaction->orphan_removed; index++) {
		change = &transaction->orphan_changes[index];
		position = ext4_orphan_find(file, change->number);
		for (move = position + 1U; move < file->pending; move++) {
			file->slots[move - 1U] = file->slots[move];
		}
		file->pending--;
		file->state[change->logical].free++;
		file->state[change->logical].cursor = change->slot;
	}
	for (; index < transaction->orphan_removed + transaction->orphan_added; index++) {
		change = &transaction->orphan_changes[index];
		position = ext4_orphan_find(file, change->number);
		for (move = file->pending; move > position; move--) {
			file->slots[move] = file->slots[move - 1U];
		}
		file->slots[position] = *change;
		file->pending++;
		file->state[change->logical].free--;
		file->state[change->logical].cursor = change->slot + 1U;
		file->cursor = change->logical;
	}
	ext4_orphan_transaction_cancel(transaction);
}

void
ext4_orphan_file_close(struct ext4_fs *fs)
{
	struct ext4_orphan_file *file = fs->orphan_file;

	if (file == NULL) {
		return;
	}
	if (file->blocks != NULL) {
		fs->environment.release(
		    fs->environment.context, file->blocks, file->capacity * sizeof(*file->blocks));
	}
	if (file->slots != NULL) {
		fs->environment.release(fs->environment.context, file->slots,
		    file->slot_capacity * sizeof(*file->slots));
	}
	if (file->state != NULL) {
		fs->environment.release(fs->environment.context, file->state,
		    file->block_count * sizeof(*file->state));
	}
	fs->environment.release(fs->environment.context, file, sizeof(*file));
	fs->orphan_file = NULL;
}

enum ext4_result
ext4_orphan_file_prepare(struct ext4_fs *fs)
{
	struct ext4_orphan_file *file;
	struct ext4_inode_disk *disk = NULL;
	struct ext4_transaction *transaction = NULL;
	struct ext4_allocation allocation;
	struct ext4_block_path path;
	struct ext4_le32 *entries = NULL;
	uint64_t offset = 0;
	uint64_t blocks;
	uint32_t logical;
	uint32_t slot;
	uint32_t index;
	uint32_t node;
	bool ready = false;
	enum ext4_result error;

	if (fs->orphan_file_inode == 0) {
		return EXT4_OK;
	}
	if (fs->orphan_file != NULL) {
		return EXT4_INVALID_ARGUMENT;
	}
	file = fs->environment.allocate(fs->environment.context, sizeof(*file));
	if (file == NULL) {
		return EXT4_NO_MEMORY;
	}
	ext4_zero(file, sizeof(*file));
	fs->orphan_file = file;
	disk = fs->environment.allocate(fs->environment.context, fs->inode_size);
	entries = fs->environment.allocate(fs->environment.context, fs->info.block_size);
	if (disk == NULL || entries == NULL) {
		error = EXT4_NO_MEMORY;
		goto out;
	}
	error = ext4_inode_allocated(fs, fs->orphan_file_inode);
	if (error == EXT4_OK) {
		error = ext4_inode_location(fs, fs->orphan_file_inode, &offset);
	}
	if (error == EXT4_OK) {
		error = ext4_device_read(fs, offset, disk, fs->inode_size);
	}
	if (error == EXT4_OK) {
		error = ext4_inode_decode(fs, fs->orphan_file_inode, disk, &file->inode);
	}
	if (error != EXT4_OK) {
		error = error == EXT4_NOT_FOUND ? EXT4_CORRUPT : error;
		goto out;
	}
	if ((file->inode.mode & EXT4_MODE_TYPE) != EXT4_MODE_REGULAR || file->inode.links != 1 ||
	    file->inode.size == 0 || file->inode.size % fs->info.block_size != 0 ||
	    ext4_le32(&disk->deletion_time) != 0) {
		error = EXT4_CORRUPT;
		goto out;
	}
	blocks = file->inode.size / fs->info.block_size;
	if (blocks > EXT4_ORPHAN_FILE_MAX_BLOCKS) {
		error = EXT4_UNSUPPORTED;
		goto out;
	}
	file->block_count = (uint32_t)blocks;
	file->capacity = file->block_count * (EXT4_EXTENT_MAX_DEPTH + 1U);
	file->blocks = fs->environment.allocate(
	    fs->environment.context, file->capacity * sizeof(*file->blocks));
	file->state = fs->environment.allocate(
	    fs->environment.context, file->block_count * sizeof(*file->state));
	if (file->blocks == NULL || file->state == NULL) {
		error = EXT4_NO_MEMORY;
		goto out;
	}
	ext4_zero(file->state, file->block_count * sizeof(*file->state));
	error = ext4_inode_has_xattrs(fs, disk) || (file->inode.flags & EXT4_INODE_RESTRICTED_FLAGS)
	    ? EXT4_UNSUPPORTED
	    : ext4_inode_writable(fs, disk, &file->inode);
	if (error == EXT4_OK) {
		/* This context only reads allocation maps and is always cancelled.
		 * Recovery may have checkpointed the ordinary prefix while keeping
		 * its journal pointer until fast-commit conversion becomes durable. */
		error = ext4_transaction_begin_recovery(
		    fs->journal, fs->journal->sequence, 1, &transaction);
	}
	if (error != EXT4_OK) {
		goto out;
	}
	error = ext4_allocation_init(&allocation, fs, transaction, &file->inode);
	if (error != EXT4_OK) {
		goto out;
	}
	ready = true;
	/* The base system index is already sorted. Verify all allocated ownership
	 * before adding this file's immutable data and mapping nodes to that index. */
	error = ext4_write_map_validate(&allocation, &file->inode, disk);
	if (error != EXT4_OK) {
		goto out;
	}
	for (logical = 0; logical < file->block_count; logical++) {
		error =
		    ext4_map_block_path(fs, &file->inode, logical, &file->blocks[logical], &path);
		if (error != EXT4_OK) {
			goto out;
		}
		if (file->blocks[logical] == 0) {
			error = EXT4_CORRUPT;
			goto out;
		}
		for (node = 0; node < path.count; node++) {
			for (index = 0; index < file->mapping_count; index++) {
				if (file->blocks[file->block_count + index] == path.blocks[node]) {
					break;
				}
			}
			if (index == file->mapping_count) {
				if (file->block_count + file->mapping_count == file->capacity) {
					error = EXT4_CORRUPT;
					goto out;
				}
				file->blocks[file->block_count + file->mapping_count++] =
				    path.blocks[node];
			}
		}
		error = ext4_block_read(fs, file->blocks[logical], entries);
		if (error == EXT4_OK) {
			error = ext4_orphan_file_block(fs, logical, entries);
		}
		if (error != EXT4_OK) {
			goto out;
		}
		for (slot = 0; slot < ext4_orphan_slots(fs); slot++) {
			if (ext4_le32(&entries[slot]) != 0) {
				error = ext4_orphan_index_reserve(fs, file->pending + 1U);
				if (error != EXT4_OK) {
					goto out;
				}
				file->slots[file->pending++] = (struct ext4_orphan_slot){
					ext4_le32(&entries[slot]), logical, slot };
			} else {
				file->state[logical].free++;
			}
		}
	}
	ext4_orphan_sort(file->slots, file->pending);
	for (index = 1; index < file->pending; index++) {
		if (file->slots[index - 1U].number == file->slots[index].number) {
			error = EXT4_CORRUPT;
			goto out;
		}
	}
	if (file->inode.blocks_512 !=
		((file->block_count + (uint64_t)fs->cluster_blocks - 1U) / fs->cluster_blocks +
		    file->mapping_count) *
		    fs->cluster_blocks * (fs->info.block_size / EXT4_SECTOR_SIZE) ||
	    (file->pending != 0 &&
		!(fs->info.feature_ro_compat & EXT4_FEATURE_RO_ORPHAN_PRESENT))) {
		error = EXT4_CORRUPT;
	} else if (file->pending > EXT4_ORPHAN_FILE_MAX_ENTRIES) {
		error = EXT4_UNSUPPORTED;
	}
out:
	if (ready) {
		ext4_allocation_destroy(&allocation);
	}
	if (transaction != NULL) {
		ext4_transaction_cancel(transaction);
	}
	if (entries != NULL) {
		fs->environment.release(fs->environment.context, entries, fs->info.block_size);
	}
	if (disk != NULL) {
		fs->environment.release(fs->environment.context, disk, fs->inode_size);
	}
	if (error != EXT4_OK) {
		ext4_orphan_file_close(fs);
	}
	return error;
}

enum ext4_result
ext4_orphan_record(struct ext4_fs *fs, uint32_t number, struct ext4_inode_disk *disk,
    struct ext4_inode *inode, bool *mapped)
{
	struct ext4_xattr_snapshot attributes;
	struct ext4_inode value;
	uint16_t type;
	uint64_t attribute_sectors;
	enum ext4_result error;

	if (number < fs->first_inode || number > fs->info.inodes || number == fs->journal_inode ||
	    number == fs->orphan_file_inode) {
		return EXT4_CORRUPT;
	}
	error = ext4_inode_decode_orphan(fs, number, disk, inode);
	if (error != EXT4_OK) {
		return error == EXT4_NOT_FOUND ? EXT4_CORRUPT : error;
	}
	if (inode->flags & EXT4_INODE_EA_INODE) {
		if (!(fs->info.feature_incompat & EXT4_FEATURE_INCOMPAT_EA_INODE) ||
		    inode->links != 0 || (inode->mode & EXT4_MODE_TYPE) != EXT4_MODE_REGULAR ||
		    ext4_le32(&disk->change_time) != 0 || ext4_le32(&disk->version_lo) != 0 ||
		    ext4_inode_has_xattrs(fs, disk) || inode->size > EXT4_XATTR_VALUE_MAX) {
			return EXT4_CORRUPT;
		}
		value = *inode;
		value.flags &= ~(uint32_t)EXT4_INODE_EA_INODE;
		error = ext4_inode_flags_writable(fs, &value);
	} else {
		error = ext4_inode_writable(fs, disk, inode);
	}
	if (error != EXT4_OK) {
		return error;
	}
	type = inode->mode & EXT4_MODE_TYPE;
	/* Linked cleanup truncates beyond EOF, where verity metadata lives. */
	if (inode->links != 0 &&
	    (type != EXT4_MODE_REGULAR || (inode->flags & EXT4_INODE_VERITY))) {
		return EXT4_UNSUPPORTED;
	}
	switch (type) {
	case EXT4_MODE_REGULAR:
	case EXT4_MODE_DIRECTORY:
		*mapped = !(inode->flags & EXT4_INODE_INLINE_DATA);
		break;
	case EXT4_MODE_SYMLINK:
		*mapped = !inode->fast_symlink;
		break;
	case EXT4_MODE_CHARACTER:
	case EXT4_MODE_BLOCK:
	case EXT4_MODE_FIFO:
	case EXT4_MODE_SOCKET:
		*mapped = false;
		break;
	default:
		return EXT4_CORRUPT;
	}
	attribute_sectors =
	    ext4_le32(&disk->xattr_block_lo) != 0 || ext4_le16(&disk->xattr_block_hi) != 0
	    ? (uint64_t)fs->cluster_blocks * (fs->info.block_size / EXT4_SECTOR_SIZE)
	    : 0;
	if (!*mapped && (fs->info.feature_incompat & EXT4_FEATURE_INCOMPAT_EA_INODE) &&
	    ext4_inode_has_xattrs(fs, disk)) {
		error = ext4_xattr_open_inode(fs, inode, disk, &attributes);
		if (error == EXT4_OK) {
			attribute_sectors += ext4_xattr_value_blocks(&attributes) *
			    (fs->info.block_size / EXT4_SECTOR_SIZE);
		}
		ext4_xattr_close(&attributes);
		if (error != EXT4_OK) {
			return error;
		}
	}
	if (!*mapped &&
	    (inode->blocks_512 != attribute_sectors || (inode->flags & EXT4_INODE_EXTENTS))) {
		return EXT4_CORRUPT;
	}
	return EXT4_OK;
}

static enum ext4_result
ext4_orphan_chain(
    struct ext4_fs *fs, const struct ext4_block_range *entries, size_t count, bool live)
{
	struct ext4_inode_disk *disk;
	struct ext4_inode inode;
	struct ext4_inode_hold *hold;
	uint64_t offset = 0;
	uint64_t power = 1;
	uint64_t distance = 0;
	uint32_t number = fs->last_orphan;
	uint32_t anchor = number;
	uint32_t visited = 0;
	uint32_t held = 0;
	size_t low;
	size_t high;
	size_t middle;
	bool mapped;
	enum ext4_result error = EXT4_OK;

	disk = fs->environment.allocate(fs->environment.context, fs->inode_size);
	if (disk == NULL) {
		return EXT4_NO_MEMORY;
	}
	/* Brent's cycle check uses constant space, including on a corrupt volume
	 * advertising billions of inodes. Validate all links before cleanup. */
	while (number != 0) {
		if (number < fs->first_inode || number > fs->info.inodes ||
		    number == fs->journal_inode || number == fs->orphan_file_inode ||
		    visited == fs->info.inodes) {
			error = EXT4_CORRUPT;
			break;
		}
		if (fs->orphan_file != NULL) {
			middle = ext4_orphan_find(fs->orphan_file, number);
			if (middle < fs->orphan_file->pending &&
			    fs->orphan_file->slots[middle].number == number) {
				error = EXT4_CORRUPT;
				break;
			}
		}
		low = 0;
		high = count;
		while (low < high) {
			middle = low + (high - low) / 2;
			if (number < entries[middle].first) {
				high = middle;
			} else if (number - entries[middle].first < entries[middle].length) {
				error = EXT4_CORRUPT;
				break;
			} else {
				low = middle + 1;
			}
		}
		if (error != EXT4_OK) {
			break;
		}
		visited++;
		error = ext4_inode_allocated(fs, number);
		if (error == EXT4_OK) {
			error = ext4_inode_location(fs, number, &offset);
		}
		if (error == EXT4_OK) {
			error = ext4_device_read(fs, offset, disk, fs->inode_size);
		}
		if (error == EXT4_OK) {
			error = ext4_orphan_record(fs, number, disk, &inode, &mapped);
		}
		if (error != EXT4_OK) {
			break;
		}
		if (live) {
			hold = ext4_inode_find_hold(fs, number);
			if (hold == NULL || !hold->unlinked || hold->references == 0 ||
			    hold->generation != inode.generation || inode.links != 0) {
				error = EXT4_CORRUPT;
				break;
			}
		}
		number = ext4_le32(&disk->deletion_time);
		distance++;
		if (number != 0 && number == anchor) {
			error = EXT4_CORRUPT;
			break;
		}
		if (distance == power) {
			anchor = number;
			power *= 2;
			distance = 0;
		}
	}
	if (error == EXT4_OK && live) {
		for (hold = fs->holds; hold != NULL; hold = hold->next) {
			held += hold->unlinked ? 1U : 0U;
		}
		if ((uint64_t)held != (uint64_t)visited +
			(fs->orphan_file == NULL ? 0U : fs->orphan_file->pending)) {
			error = EXT4_CORRUPT;
		}
	}
	fs->environment.release(fs->environment.context, disk, fs->inode_size);
	return error;
}

static enum ext4_result
ext4_orphan_validate_all(struct ext4_fs *fs, bool live)
{
	struct ext4_orphan_file *file = fs->orphan_file;
	struct ext4_block_range *numbers;
	struct ext4_inode_disk *disk;
	struct ext4_inode inode;
	struct ext4_inode_hold *hold;
	struct ext4_le32 *entries;
	uint64_t offset = 0;
	uint32_t logical;
	uint32_t slot;
	uint32_t number;
	size_t count = 0;
	size_t bytes;
	bool mapped;
	enum ext4_result error = EXT4_OK;

	if (file == NULL || file->pending == 0) {
		return ext4_orphan_chain(fs, NULL, 0, live);
	}
	bytes = (size_t)file->pending * sizeof(*numbers);
	numbers = fs->environment.allocate(fs->environment.context, bytes);
	disk = fs->environment.allocate(fs->environment.context, fs->inode_size);
	entries = fs->environment.allocate(fs->environment.context, fs->info.block_size);
	if (numbers == NULL || disk == NULL || entries == NULL) {
		error = EXT4_NO_MEMORY;
		goto out;
	}
	for (logical = 0; logical < file->block_count; logical++) {
		error = ext4_block_read(fs, file->blocks[logical], entries);
		if (error == EXT4_OK) {
			error = ext4_orphan_file_block(fs, logical, entries);
		}
		if (error != EXT4_OK) {
			goto out;
		}
		for (slot = 0; slot < ext4_orphan_slots(fs); slot++) {
			number = ext4_le32(&entries[slot]);
			if (number == 0) {
				continue;
			}
			if (count == file->pending) {
				error = EXT4_CORRUPT;
				goto out;
			}
			error = ext4_inode_allocated(fs, number);
			if (error == EXT4_OK) {
				error = ext4_inode_location(fs, number, &offset);
			}
			if (error == EXT4_OK) {
				error = ext4_device_read(fs, offset, disk, fs->inode_size);
			}
			if (error == EXT4_OK) {
				error = ext4_orphan_record(fs, number, disk, &inode, &mapped);
			}
			if (error != EXT4_OK) {
				goto out;
			}
			if (ext4_le32(&disk->deletion_time) != 0) {
				error = EXT4_CORRUPT;
				goto out;
			}
			if (live) {
				hold = ext4_inode_find_hold(fs, number);
				if (hold == NULL || !hold->unlinked || hold->references == 0 ||
				    hold->generation != inode.generation || inode.links != 0) {
					error = EXT4_CORRUPT;
					goto out;
				}
			}
			numbers[count].first = number;
			numbers[count++].length = 1;
		}
	}
	if (count != file->pending) {
		error = EXT4_CORRUPT;
		goto out;
	}
	/* Sorting detects duplicates within the file. The complete legacy chain
	 * is then checked against this set before either representation is edited. */
	error = ext4_ranges_sort(numbers, &count);
	if (error == EXT4_OK) {
		error = ext4_orphan_chain(fs, numbers, count, live);
	}
out:
	if (entries != NULL) {
		fs->environment.release(fs->environment.context, entries, fs->info.block_size);
	}
	if (disk != NULL) {
		fs->environment.release(fs->environment.context, disk, fs->inode_size);
	}
	if (numbers != NULL) {
		fs->environment.release(fs->environment.context, numbers, bytes);
	}
	return error;
}

enum ext4_result
ext4_orphan_validate_live(struct ext4_fs *fs)
{
	return ext4_orphan_validate_all(fs, true);
}

enum ext4_result
ext4_orphan_validate(struct ext4_fs *fs)
{
	return ext4_orphan_validate_all(fs, false);
}

static void
ext4_orphan_file_seal(struct ext4_fs *fs, uint32_t logical, void *buffer)
{
	struct ext4_orphan_tail_disk *tail = (struct ext4_orphan_tail_disk *)((uint8_t *)buffer +
	    fs->info.block_size - sizeof(*tail));

	if (fs->metadata_checksum) {
		ext4_encode32(&tail->checksum, ext4_orphan_file_checksum(fs, logical, buffer));
	}
}

enum ext4_result
ext4_orphan_file_remove(struct ext4_allocation *allocation, uint32_t number, bool *removed)
{
	struct ext4_fs *fs = allocation->fs;
	struct ext4_orphan_file *file = fs->orphan_file;
	struct ext4_orphan_slot reference;
	struct ext4_le32 *entries;
	void *buffer;
	uint32_t position;
	enum ext4_result error;

	*removed = false;
	if (file == NULL) {
		return EXT4_OK;
	}
	position = ext4_orphan_find(file, number);
	if (position == file->pending || file->slots[position].number != number) {
		return EXT4_OK;
	}
	reference = file->slots[position];
	error = ext4_transaction_buffer(allocation->transaction, file->blocks[reference.logical],
	    &buffer);
	if (error != EXT4_OK) {
		return error;
	}
	error = ext4_orphan_file_block(fs, reference.logical, buffer);
	if (error != EXT4_OK) {
		return error;
	}
	entries = buffer;
	/* Fast-commit inode reuse can remove the same original slot twice in one
	 * private conversion. Membership publishes only when that conversion commits. */
	if (ext4_le32(&entries[reference.slot]) == 0) {
		return EXT4_OK;
	}
	if (ext4_le32(&entries[reference.slot]) != number) {
		return EXT4_CORRUPT;
	}
	ext4_encode32(&entries[reference.slot], 0);
	ext4_orphan_file_seal(fs, reference.logical, buffer);
	allocation->transaction->orphan_touched = true;
	*removed = true;
	return EXT4_OK;
}

static enum ext4_result
ext4_orphan_file_add(struct ext4_allocation *allocation, uint32_t number, bool *added)
{
	struct ext4_fs *fs = allocation->fs;
	struct ext4_orphan_file *file = fs->orphan_file;
	struct ext4_le32 *entries;
	void *buffer;
	uint32_t logical;
	uint32_t slot;
	uint32_t block;
	uint32_t step;
	uint32_t position;
	uint32_t slots = ext4_orphan_slots(fs);
	enum ext4_result error;

	*added = false;
	/* Direct writers preserve their legacy dirty-superblock contract. */
	if (file == NULL || fs->journal->direct || file->pending == EXT4_ORPHAN_FILE_MAX_ENTRIES) {
		return EXT4_OK;
	}
	position = ext4_orphan_find(file, number);
	if (position < file->pending && file->slots[position].number == number) {
		return EXT4_CORRUPT;
	}
	for (block = 0; block < file->block_count; block++) {
		logical = (file->cursor + block) % file->block_count;
		if (file->state[logical].free == 0) {
			continue;
		}
		error = ext4_transaction_buffer(
		    allocation->transaction, file->blocks[logical], &buffer);
		if (error != EXT4_OK) {
			return error;
		}
		error = ext4_orphan_file_block(fs, logical, buffer);
		if (error != EXT4_OK) {
			return error;
		}
		entries = buffer;
		for (step = 0; step < slots; step++) {
			slot = (file->state[logical].cursor + step) % slots;
			if (ext4_le32(&entries[slot]) != 0) {
				continue;
			}
			ext4_encode32(&entries[slot], number);
			ext4_orphan_file_seal(fs, logical, buffer);
			allocation->transaction->orphan_touched = true;
			*added = true;
			return EXT4_OK;
		}
	}
	return EXT4_OK;
}

static enum ext4_result
ext4_orphan_file_transfer(struct ext4_fs *fs, uint32_t *cursor, struct ext4_recovery_report *report)
{
	struct ext4_orphan_file *file = fs->orphan_file;
	struct ext4_transaction *transaction = NULL;
	struct ext4_super_disk *super;
	struct ext4_inode_disk *disk;
	struct ext4_inode inode;
	struct ext4_orphan_tail_disk *tail;
	struct ext4_le32 *entries;
	void *buffer;
	uint64_t offset;
	uint32_t slots = ext4_orphan_slots(fs);
	uint32_t logical = *cursor / slots;
	uint32_t slot = *cursor % slots;
	uint32_t number = 0;
	bool mapped;
	enum ext4_result error = EXT4_OK;

	if (file == NULL || file->pending == 0) {
		return EXT4_NOT_FOUND;
	}
	if (fs->last_orphan != 0) {
		return EXT4_CORRUPT;
	}
	entries = fs->environment.allocate(fs->environment.context, fs->info.block_size);
	if (entries == NULL) {
		return EXT4_NO_MEMORY;
	}
	for (; logical < file->block_count; logical++) {
		error = ext4_block_read(fs, file->blocks[logical], entries);
		if (error == EXT4_OK) {
			error = ext4_orphan_file_block(fs, logical, entries);
		}
		if (error != EXT4_OK) {
			break;
		}
		for (; slot < slots; slot++) {
			number = ext4_le32(&entries[slot]);
			if (number != 0) {
				break;
			}
		}
		if (number != 0) {
			break;
		}
		slot = 0;
	}
	fs->environment.release(fs->environment.context, entries, fs->info.block_size);
	if (error != EXT4_OK || number == 0) {
		return error == EXT4_OK ? EXT4_CORRUPT : error;
	}
	error =
	    ext4_transaction_begin(fs->journal, ext4_journal_credits(fs->journal), &transaction);
	if (error != EXT4_OK) {
		return error;
	}
	error = ext4_transaction_buffer(transaction, file->blocks[logical], &buffer);
	if (error != EXT4_OK) {
		goto cancel;
	}
	entries = buffer;
	error = ext4_orphan_file_block(fs, logical, entries);
	if (error != EXT4_OK || ext4_le32(&entries[slot]) != number) {
		error = EXT4_CORRUPT;
		goto cancel;
	}
	error = ext4_inode_location(fs, number, &offset);
	if (error == EXT4_OK) {
		error = ext4_transaction_buffer(transaction, offset / fs->info.block_size, &buffer);
	}
	if (error != EXT4_OK) {
		goto cancel;
	}
	disk = (struct ext4_inode_disk *)((uint8_t *)buffer + offset % fs->info.block_size);
	error = ext4_orphan_record(fs, number, disk, &inode, &mapped);
	if (error != EXT4_OK || ext4_le32(&disk->deletion_time) != 0) {
		error = error == EXT4_OK ? EXT4_CORRUPT : error;
		goto cancel;
	}
	error = ext4_transaction_super(transaction, &super);
	if (error != EXT4_OK) {
		goto cancel;
	}
	if (ext4_le32(&super->last_orphan) != 0) {
		error = EXT4_CORRUPT;
		goto cancel;
	}
	/* The file slot and legacy head move in one committed transaction. A crash
	 * leaves this inode in exactly one format; the existing bounded cleaner
	 * can then resume from either side of that transfer. */
	ext4_encode32(&entries[slot], 0);
	transaction->orphan_touched = true;
	tail = (struct ext4_orphan_tail_disk *)((uint8_t *)entries + fs->info.block_size -
	    sizeof(*tail));
	if (fs->metadata_checksum) {
		ext4_encode32(&tail->checksum, ext4_orphan_file_checksum(fs, logical, entries));
	}
	ext4_encode32(&super->last_orphan, number);
	error = ext4_transaction_commit(transaction);
	if (error != EXT4_OK) {
		fs->aborted = true;
		return error;
	}
	fs->last_orphan = number;
	*cursor = logical * slots + slot + 1;
	report->orphan_transactions++;
	report->orphan_file_transfers++;
	return EXT4_OK;
cancel:
	ext4_transaction_cancel(transaction);
	return error;
}

static enum ext4_result
ext4_orphan_tail(struct ext4_allocation *allocation, const struct ext4_inode *inode,
    const struct ext4_inode_disk *disk)
{
	struct ext4_map_run run;
	void *buffer;
	uint32_t block_size = allocation->fs->info.block_size;
	size_t within = (size_t)(inode->size % block_size);
	enum ext4_result error;

	if (within == 0) {
		return EXT4_OK;
	}
	error = ext4_write_map_lookup(
	    allocation, inode, disk, (uint32_t)(inode->size / block_size), &run);
	if (error != EXT4_OK || run.physical == 0 || run.unwritten) {
		return error;
	}
	error = ext4_transaction_data(allocation->transaction, run.physical, false, &buffer);
	if (error == EXT4_OK) {
		ext4_zero((uint8_t *)buffer + within, block_size - within);
	}
	return error;
}

/* The step size depends only on the inode's current allocation and the caller's
 * cap, so recovery after an interruption repeats exactly the remaining steps of an
 * uninterrupted reclamation and reaches the same bytes. */
static uint32_t
ext4_orphan_step_blocks(const struct ext4_fs *fs, const struct ext4_inode *inode, uint32_t cap)
{
	uint64_t half = inode->blocks_512 / (fs->info.block_size / EXT4_SECTOR_SIZE) / 2U;
	uint32_t limit = EXT4_ORPHAN_BATCH_BLOCKS;

	while (limit < EXT4_ORPHAN_STEP_MAX_BLOCKS && (uint64_t)limit * 2U <= half) {
		limit *= 2U;
	}
	return limit < cap ? limit : cap;
}

static enum ext4_result
ext4_orphan_step(struct ext4_fs *fs, uint32_t number, uint32_t previous, bool retained,
    bool file_owned, uint32_t *limit, bool validate, bool *completed,
    struct ext4_recovery_report *report)
{
	struct ext4_transaction *transaction;
	struct ext4_allocation allocation;
	struct ext4_inode inode;
	struct ext4_inode_disk *disk;
	struct ext4_inode_disk *previous_disk;
	struct ext4_inode previous_inode;
	void *buffer = NULL;
	uint64_t offset;
	uint32_t next = 0;
	uint32_t first;
	bool ready = false;
	bool mapped;
	bool previous_mapped;
	bool done = true;
	bool removed;
	enum ext4_result error;

	error =
	    ext4_transaction_begin(fs->journal, ext4_journal_credits(fs->journal), &transaction);
	if (error != EXT4_OK) {
		return error;
	}
	error = ext4_inode_location(fs, number, &offset);
	if (error == EXT4_OK) {
		error = ext4_transaction_buffer(transaction, offset / fs->info.block_size, &buffer);
	}
	if (error != EXT4_OK) {
		goto cancel;
	}
	disk = (struct ext4_inode_disk *)((uint8_t *)buffer + offset % fs->info.block_size);
	error = ext4_orphan_record(fs, number, disk, &inode, &mapped);
	if (error != EXT4_OK) {
		goto cancel;
	}
	next = ext4_le32(&disk->deletion_time);
	*limit = ext4_orphan_step_blocks(fs, &inode, *limit);
	error = ext4_allocation_init(&allocation, fs, transaction, &inode);
	if (error != EXT4_OK) {
		goto cancel;
	}
	ready = true;
	error = ext4_allocation_super(&allocation);
	if (error == EXT4_OK && ext4_le32(&allocation.super->last_orphan) != fs->last_orphan) {
		error = EXT4_CORRUPT;
	}
	if (error == EXT4_OK && validate) {
		error = ext4_write_map_validate(&allocation, &inode, disk);
	}
	first = inode.links == 0 && !retained
	    ? 0
	    : (uint32_t)((inode.size + fs->info.block_size - 1) / fs->info.block_size);
	if (error == EXT4_OK && mapped) {
		error = ext4_write_map_trim(&allocation, &inode, disk, first, *limit, &done);
	}
	/* An encrypted file's partial block is ciphertext that cleanup cannot zero
	 * without its key; as in Linux, those bytes past EOF stay and are never read. */
	if (error == EXT4_OK && done && mapped && (inode.links != 0 || retained) &&
	    !(inode.flags & EXT4_INODE_ENCRYPT)) {
		error = ext4_orphan_tail(&allocation, &inode, disk);
	}
	if (error == EXT4_OK && done && mapped && !retained && inode.links == 0 &&
	    (inode.mode & EXT4_MODE_TYPE) == EXT4_MODE_SYMLINK) {
		/* Attribute reclamation can need later transactions. Once the target
		 * is freed, leave an empty representation rather than a long symlink
		 * whose final value charge would make its empty map look like a fast link. */
		inode.size = 0;
		inode.flags &= ~(uint32_t)EXT4_INODE_EXTENTS;
		ext4_zero(disk->block_data, sizeof(disk->block_data));
		ext4_encode32(&disk->flags, inode.flags);
	}
	if (error == EXT4_OK && done && !retained && inode.links == 0 &&
	    ext4_inode_has_xattrs(fs, disk)) {
		error = ext4_xattr_drop(&allocation, &inode, disk, &done);
	}
	if (error == EXT4_OK) {
		error = ext4_inode_account(&allocation, &inode, disk, inode.size);
	}
	if (error == EXT4_OK && done && !retained) {
		if (inode.links == 0) {
			error = ext4_free_inode(&allocation, disk, &inode);
		} else {
			ext4_encode32(&disk->deletion_time, 0);
		}
		if (error == EXT4_OK && file_owned) {
			error = ext4_orphan_file_remove(&allocation, number, &removed);
			if (error == EXT4_OK && !removed) {
				error = EXT4_CORRUPT;
			}
		} else if (error == EXT4_OK && previous == 0) {
			if (fs->last_orphan != number) {
				error = EXT4_CORRUPT;
			} else {
				ext4_encode32(&allocation.super->last_orphan, next);
			}
		} else if (error == EXT4_OK) {
			error = ext4_inode_location(fs, previous, &offset);
			if (error == EXT4_OK) {
				error = ext4_transaction_buffer(
				    transaction, offset / fs->info.block_size, &buffer);
			}
			if (error == EXT4_OK) {
				previous_disk = (struct ext4_inode_disk *)((uint8_t *)buffer +
				    offset % fs->info.block_size);
				error = ext4_orphan_record(
				    fs, previous, previous_disk, &previous_inode, &previous_mapped);
				if (error == EXT4_OK &&
				    ext4_le32(&previous_disk->deletion_time) != number) {
					error = EXT4_CORRUPT;
				}
				if (error == EXT4_OK) {
					ext4_transaction_inode_policy(transaction, &previous_inode);
					ext4_encode32(&previous_disk->deletion_time, next);
					ext4_inode_checksum_set(fs, previous, previous_disk);
				}
			}
		}
	}
	if (error != EXT4_OK) {
		goto cancel;
	}
	if (!done && allocation.freed == 0 && allocation.unmapped == 0 &&
	    allocation.attribute_blocks_removed == 0) {
		error = EXT4_CORRUPT;
		goto cancel;
	}
	ext4_inode_checksum_set(fs, number, disk);
	ext4_allocation_destroy(&allocation);
	error = ext4_transaction_commit(transaction);
	if (error != EXT4_OK) {
		fs->aborted = true;
		return error;
	}
	report->orphan_transactions++;
	if (done && !retained) {
		if (!file_owned && previous == 0) {
			fs->last_orphan = next;
		}
		report->cleaned_orphans++;
	}
	*completed = done;
	return EXT4_OK;
cancel:
	if (ready) {
		ext4_allocation_destroy(&allocation);
	}
	ext4_transaction_cancel(transaction);
	return error;
}

enum ext4_result
ext4_orphan_link(struct ext4_allocation *allocation, uint32_t number, struct ext4_inode_disk *disk)
{
	struct ext4_fs *fs = allocation->fs;
	bool added = false;
	enum ext4_result error;

	error = ext4_allocation_super(allocation);
	if (error == EXT4_OK && ext4_le32(&allocation->super->last_orphan) != fs->last_orphan) {
		error = EXT4_CORRUPT;
	}
	if (error == EXT4_OK) {
		error = ext4_orphan_file_add(allocation, number, &added);
	}
	if (error == EXT4_OK && added) {
		ext4_encode32(&disk->deletion_time, 0);
		return EXT4_OK;
	}
	if (error == EXT4_OK) {
		ext4_encode32(&disk->deletion_time, fs->last_orphan);
		ext4_encode32(&allocation->super->last_orphan, number);
	}
	return error;
}

enum ext4_result
ext4_orphan_unlink(struct ext4_allocation *allocation, uint32_t number,
    struct ext4_inode_disk *disk, uint32_t *last_orphan)
{
	struct ext4_fs *fs = allocation->fs;
	struct ext4_inode_disk *record;
	struct ext4_inode_disk *previous_disk;
	struct ext4_inode inode;
	void *buffer = NULL;
	uint64_t offset = 0;
	uint32_t cursor = fs->last_orphan;
	uint32_t previous = 0;
	uint32_t next = ext4_le32(&disk->deletion_time);
	bool mapped;
	bool removed;
	enum ext4_result error;

	error = ext4_orphan_file_remove(allocation, number, &removed);
	if (error != EXT4_OK) {
		return error;
	}
	if (removed) {
		if (ext4_le32(&disk->deletion_time) != 0) {
			return EXT4_CORRUPT;
		}
		error = ext4_inode_allocated(fs, number);
		if (error != EXT4_OK) {
			return error;
		}
		*last_orphan = fs->last_orphan;
		ext4_encode32(&disk->deletion_time, 0);
		return EXT4_OK;
	}
	error = ext4_orphan_chain(fs, NULL, 0, false);
	if (error == EXT4_OK) {
		error = ext4_allocation_super(allocation);
	}
	if (error == EXT4_OK && ext4_le32(&allocation->super->last_orphan) != fs->last_orphan) {
		error = EXT4_CORRUPT;
	}
	if (error != EXT4_OK) {
		return error;
	}
	record = fs->environment.allocate(fs->environment.context, fs->inode_size);
	if (record == NULL) {
		return EXT4_NO_MEMORY;
	}
	/* The validated list is finite; find the entry that links to number. */
	while (error == EXT4_OK && cursor != 0 && cursor != number) {
		previous = cursor;
		error = ext4_inode_location(fs, cursor, &offset);
		if (error == EXT4_OK) {
			error = ext4_device_read(fs, offset, record, fs->inode_size);
		}
		if (error == EXT4_OK) {
			cursor = ext4_le32(&record->deletion_time);
		}
	}
	fs->environment.release(fs->environment.context, record, fs->inode_size);
	if (error == EXT4_OK && cursor == 0) {
		error = EXT4_CORRUPT;
	}
	if (error != EXT4_OK) {
		return error;
	}
	*last_orphan = fs->last_orphan;
	if (previous == 0) {
		ext4_encode32(&allocation->super->last_orphan, next);
		*last_orphan = next;
	} else {
		error = ext4_inode_location(fs, previous, &offset);
		if (error == EXT4_OK) {
			error = ext4_transaction_buffer(
			    allocation->transaction, offset / fs->info.block_size, &buffer);
		}
		if (error != EXT4_OK) {
			return error;
		}
		previous_disk =
		    (struct ext4_inode_disk *)((uint8_t *)buffer + offset % fs->info.block_size);
		error = ext4_orphan_record(fs, previous, previous_disk, &inode, &mapped);
		if (error == EXT4_OK && ext4_le32(&previous_disk->deletion_time) != number) {
			error = EXT4_CORRUPT;
		}
		if (error != EXT4_OK) {
			return error;
		}
		ext4_transaction_inode_policy(allocation->transaction, &inode);
		ext4_encode32(&previous_disk->deletion_time, next);
		ext4_inode_checksum_set(fs, previous, previous_disk);
	}
	ext4_encode32(&disk->deletion_time, 0);
	return EXT4_OK;
}

/* The index locates intent but does not replace validation of its stored bytes.
 * Recheck the selected slot before any bounded reclamation batch can write. */
static enum ext4_result
ext4_orphan_file_verify(struct ext4_fs *fs, const struct ext4_orphan_slot *reference)
{
	struct ext4_le32 *entries;
	enum ext4_result error;

	entries = fs->environment.allocate(fs->environment.context, fs->info.block_size);
	if (entries == NULL) {
		return EXT4_NO_MEMORY;
	}
	error = ext4_block_read(fs, fs->orphan_file->blocks[reference->logical], entries);
	if (error == EXT4_OK) {
		error = ext4_orphan_file_block(fs, reference->logical, entries);
	}
	if (error == EXT4_OK && ext4_le32(&entries[reference->slot]) != reference->number) {
		error = EXT4_CORRUPT;
	}
	fs->environment.release(fs->environment.context, entries, fs->info.block_size);
	if (error == EXT4_OK) {
		error = ext4_inode_allocated(fs, reference->number);
	}
	return error;
}

enum ext4_result
ext4_orphan_finish_inode(struct ext4_fs *fs, uint32_t number, uint32_t generation, bool retained)
{
	struct ext4_inode_disk *disk;
	struct ext4_inode inode;
	struct ext4_recovery_report report;
	uint64_t offset = 0;
	uint32_t cursor = fs->last_orphan;
	uint32_t previous = 0;
	uint32_t limit = EXT4_ORPHAN_STEP_MAX_BLOCKS;
	bool mapped;
	bool completed = false;
	bool validate = true;
	bool file_owned = false;
	uint32_t position = 0;
	enum ext4_result error;

	if (fs->orphan_file != NULL) {
		position = ext4_orphan_find(fs->orphan_file, number);
		file_owned = position < fs->orphan_file->pending &&
		    fs->orphan_file->slots[position].number == number;
		if (file_owned) {
			cursor = number;
		}
	}
	error = file_owned ? ext4_orphan_file_verify(fs, &fs->orphan_file->slots[position])
			   : ext4_orphan_chain(fs, NULL, 0, false);
	if (error != EXT4_OK) {
		return error;
	}
	disk = fs->environment.allocate(fs->environment.context, fs->inode_size);
	if (disk == NULL) {
		return EXT4_NO_MEMORY;
	}
	while (cursor != 0) {
		error = ext4_inode_location(fs, cursor, &offset);
		if (error == EXT4_OK) {
			error = ext4_device_read(fs, offset, disk, fs->inode_size);
		}
		if (error == EXT4_OK) {
			error = ext4_orphan_record(fs, cursor, disk, &inode, &mapped);
		}
		if (error == EXT4_OK && file_owned && ext4_le32(&disk->deletion_time) != 0) {
			error = EXT4_CORRUPT;
		}
		if (error != EXT4_OK || cursor == number) {
			break;
		}
		previous = cursor;
		cursor = ext4_le32(&disk->deletion_time);
	}
	fs->environment.release(fs->environment.context, disk, fs->inode_size);
	if (error != EXT4_OK) {
		return error;
	}
	if (cursor == 0) {
		return EXT4_NOT_FOUND;
	}
	if (inode.generation != generation) {
		return EXT4_STALE;
	}
	if (retained && inode.links != 0) {
		return EXT4_INVALID_ARGUMENT;
	}
	ext4_zero(&report, sizeof(report));
	while (!completed) {
		error = ext4_orphan_step(fs, number, previous, retained, file_owned, &limit,
		    validate, &completed, &report);
		if (error == EXT4_RANGE && !fs->aborted && limit > 1) {
			limit /= 2;
			continue;
		}
		if (error != EXT4_OK) {
			return error;
		}
		validate = false;
		limit = EXT4_ORPHAN_STEP_MAX_BLOCKS;
	}
	return EXT4_OK;
}

enum ext4_result
ext4_orphan_cleanup(struct ext4_fs *fs, struct ext4_recovery_report *report)
{
	uint32_t number;
	uint32_t limit;
	uint32_t cursor = 0;
	bool validate;
	bool completed;
	enum ext4_result error;

	if (fs->last_orphan == 0 && (fs->orphan_file == NULL || fs->orphan_file->pending == 0)) {
		return EXT4_OK;
	}
	if (fs->first_inode < EXT4_FIRST_NON_RESERVED_INODE || fs->first_inode > fs->info.inodes ||
	    fs->clusters_per_group % EXT4_BITS_PER_BYTE != 0 ||
	    fs->inodes_per_group % EXT4_BITS_PER_BYTE != 0) {
		return EXT4_CORRUPT;
	}
	error = ext4_orphan_validate(fs);
	if (error != EXT4_OK) {
		return error;
	}
	for (;;) {
		if (fs->last_orphan == 0) {
			error = ext4_orphan_file_transfer(fs, &cursor, report);
			if (error == EXT4_NOT_FOUND) {
				break;
			}
			if (error != EXT4_OK) {
				return error;
			}
		}
		number = fs->last_orphan;
		limit = EXT4_ORPHAN_STEP_MAX_BLOCKS;
		validate = true;
		do {
			error = ext4_orphan_step(
			    fs, number, 0, false, false, &limit, validate, &completed, report);
			if (error == EXT4_RANGE && !fs->aborted && limit > 1) {
				limit /= 2;
				continue;
			}
			if (error != EXT4_OK) {
				return error;
			}
			validate = false;
			limit = EXT4_ORPHAN_STEP_MAX_BLOCKS;
		} while (fs->last_orphan == number);
	}
	return EXT4_OK;
}

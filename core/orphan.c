/* SPDX-License-Identifier: BSD-3-Clause */
#include "allocate.h"

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
	if (file->blocks == NULL) {
		error = EXT4_NO_MEMORY;
		goto out;
	}
	error = ext4_inode_writable(fs, disk, &file->inode);
	if (error == EXT4_OK) {
		error = ext4_transaction_begin(fs->journal, 1, &transaction);
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
				file->pending++;
			}
		}
	}
	if (file->inode.blocks_512 !=
		(uint64_t)(file->block_count + file->mapping_count) *
		    (fs->info.block_size / EXT4_SECTOR_SIZE) ||
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

static enum ext4_result
ext4_orphan_record(struct ext4_fs *fs, uint32_t number, struct ext4_inode_disk *disk,
    struct ext4_inode *inode, bool *mapped)
{
	uint16_t type;
	enum ext4_result error;

	if (number < fs->first_inode || number > fs->info.inodes || number == fs->journal_inode ||
	    number == fs->orphan_file_inode) {
		return EXT4_CORRUPT;
	}
	error = ext4_inode_decode_orphan(fs, number, disk, inode);
	if (error != EXT4_OK) {
		return error == EXT4_NOT_FOUND ? EXT4_CORRUPT : error;
	}
	error = ext4_inode_writable(fs, disk, inode);
	if (error != EXT4_OK) {
		return error;
	}
	type = inode->mode & EXT4_MODE_TYPE;
	if (inode->links != 0 && type != EXT4_MODE_REGULAR) {
		return EXT4_UNSUPPORTED;
	}
	switch (type) {
	case EXT4_MODE_REGULAR:
	case EXT4_MODE_DIRECTORY:
		*mapped = true;
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
	if (!*mapped && (inode->blocks_512 != 0 || (inode->flags & EXT4_INODE_EXTENTS))) {
		return EXT4_CORRUPT;
	}
	return EXT4_OK;
}

static enum ext4_result
ext4_orphan_chain(struct ext4_fs *fs, const struct ext4_block_range *entries, size_t count)
{
	struct ext4_inode_disk *disk;
	struct ext4_inode inode;
	uint64_t offset = 0;
	uint64_t power = 1;
	uint64_t distance = 0;
	uint32_t number = fs->last_orphan;
	uint32_t anchor = number;
	uint32_t visited = 0;
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
	fs->environment.release(fs->environment.context, disk, fs->inode_size);
	return error;
}

static enum ext4_result
ext4_orphan_validate(struct ext4_fs *fs)
{
	struct ext4_orphan_file *file = fs->orphan_file;
	struct ext4_block_range *numbers;
	struct ext4_inode_disk *disk;
	struct ext4_inode inode;
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
		return ext4_orphan_chain(fs, NULL, 0);
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
		error = ext4_orphan_chain(fs, numbers, count);
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
	file->pending--;
	*cursor = logical * slots + slot + 1;
	report->orphan_transactions++;
	report->orphan_file_transfers++;
	return EXT4_OK;
cancel:
	ext4_transaction_cancel(transaction);
	return error;
}

static enum ext4_result
ext4_orphan_free_inode(struct ext4_allocation *allocation, struct ext4_inode_disk *disk,
    const struct ext4_inode *inode)
{
	struct ext4_fs *fs = allocation->fs;
	struct ext4_group group;
	struct ext4_group_disk *descriptor;
	uint8_t *bitmap;
	void *buffer;
	uint64_t offset;
	uint32_t index = (inode->number - 1) / fs->inodes_per_group;
	uint32_t within = (inode->number - 1) % fs->inodes_per_group;
	uint32_t available = fs->info.inodes - index * fs->inodes_per_group;
	uint32_t free_inodes = 0;
	uint32_t directories;
	uint32_t bit;
	uint32_t checksum;
	bool used;
	enum ext4_result error;

	error = ext4_allocation_super(allocation);
	if (error != EXT4_OK) {
		return error;
	}
	if (ext4_le32(&allocation->super->free_inodes) != fs->info.free_inodes ||
	    fs->info.free_inodes == fs->info.inodes) {
		return EXT4_CORRUPT;
	}
	offset = (uint64_t)(fs->first_data_block + 1) * fs->info.block_size +
	    (uint64_t)index * fs->descriptor_size;
	error =
	    ext4_transaction_buffer(allocation->transaction, offset / fs->info.block_size, &buffer);
	if (error != EXT4_OK) {
		return error;
	}
	descriptor = (struct ext4_group_disk *)((uint8_t *)buffer + offset % fs->info.block_size);
	error = ext4_group_decode(fs, index, descriptor, &group);
	if (error != EXT4_OK) {
		return error;
	}
	if (group.flags & EXT4_GROUP_INODE_UNINIT) {
		return EXT4_CORRUPT;
	}
	error = ext4_transaction_buffer(allocation->transaction, group.inode_bitmap, &buffer);
	if (error != EXT4_OK) {
		return error;
	}
	bitmap = buffer;
	if (fs->metadata_checksum) {
		checksum = ext4_crc32c(
		    fs->checksum_seed, bitmap, fs->inodes_per_group / EXT4_BITS_PER_BYTE);
		if (fs->descriptor_size < EXT4_GROUP_64_SIZE) {
			checksum &= UINT16_MAX;
		}
		if (checksum != group.inode_bitmap_checksum) {
			return EXT4_CORRUPT;
		}
	}
	if (available > fs->inodes_per_group) {
		available = fs->inodes_per_group;
	}
	for (bit = 0; bit < fs->info.block_size * EXT4_BITS_PER_BYTE; bit++) {
		used = (bitmap[bit / EXT4_BITS_PER_BYTE] & (1U << (bit % EXT4_BITS_PER_BYTE))) != 0;
		if (!used &&
		    (bit >= available || bit == within ||
			(index == 0 && bit + 1 < fs->first_inode))) {
			return EXT4_CORRUPT;
		}
		if (!used) {
			free_inodes++;
		}
	}
	directories = ext4_le16(&descriptor->used_directories_lo);
	if (fs->descriptor_size >= EXT4_GROUP_64_SIZE) {
		directories |= (uint32_t)ext4_le16(&descriptor->used_directories_hi) << 16;
	}
	if (free_inodes != group.free_inodes || free_inodes >= available ||
	    directories > available - free_inodes) {
		return EXT4_CORRUPT;
	}
	if ((inode->mode & EXT4_MODE_TYPE) == EXT4_MODE_DIRECTORY) {
		if (directories == 0) {
			return EXT4_CORRUPT;
		}
		directories--;
	}
	bitmap[within / EXT4_BITS_PER_BYTE] &= (uint8_t)~(1U << (within % EXT4_BITS_PER_BYTE));
	free_inodes++;
	ext4_encode16(&descriptor->free_inodes_lo, (uint16_t)free_inodes);
	ext4_encode16(&descriptor->used_directories_lo, (uint16_t)directories);
	if (fs->descriptor_size >= EXT4_GROUP_64_SIZE) {
		ext4_encode16(&descriptor->free_inodes_hi, (uint16_t)(free_inodes >> 16));
		ext4_encode16(&descriptor->used_directories_hi, (uint16_t)(directories >> 16));
	}
	if (fs->metadata_checksum) {
		checksum = ext4_crc32c(
		    fs->checksum_seed, bitmap, fs->inodes_per_group / EXT4_BITS_PER_BYTE);
		ext4_encode16(&descriptor->inode_bitmap_checksum_lo, (uint16_t)checksum);
		if (fs->descriptor_size >= EXT4_GROUP_64_SIZE) {
			ext4_encode16(
			    &descriptor->inode_bitmap_checksum_hi, (uint16_t)(checksum >> 16));
		}
	}
	ext4_group_checksum_set(fs, index, descriptor);
	ext4_encode32(&allocation->super->free_inodes, fs->info.free_inodes + 1);
	/* Preserve the generation for the future allocator. A freed record has no
	 * live type, data pointers or invented deletion timestamp. */
	ext4_zero(disk, fs->inode_size);
	ext4_encode32(&disk->generation, inode->generation);
	return EXT4_OK;
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
	error = ext4_transaction_buffer(allocation->transaction, run.physical, &buffer);
	if (error == EXT4_OK) {
		ext4_zero((uint8_t *)buffer + within, block_size - within);
	}
	return error;
}

static enum ext4_result
ext4_orphan_step(
    struct ext4_fs *fs, uint32_t limit, bool validate, struct ext4_recovery_report *report)
{
	struct ext4_transaction *transaction;
	struct ext4_allocation allocation;
	struct ext4_inode inode;
	struct ext4_inode_disk *disk;
	void *buffer = NULL;
	uint64_t offset;
	uint64_t free_blocks;
	uint32_t number = fs->last_orphan;
	uint32_t next = 0;
	uint32_t first;
	bool ready = false;
	bool mapped;
	bool done = true;
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
	error = ext4_allocation_init(&allocation, fs, transaction, &inode);
	if (error != EXT4_OK) {
		goto cancel;
	}
	ready = true;
	error = ext4_allocation_super(&allocation);
	if (error == EXT4_OK && ext4_le32(&allocation.super->last_orphan) != number) {
		error = EXT4_CORRUPT;
	}
	if (error == EXT4_OK && validate && mapped) {
		error = ext4_write_map_validate(&allocation, &inode, disk);
	}
	first = inode.links == 0
	    ? 0
	    : (uint32_t)((inode.size + fs->info.block_size - 1) / fs->info.block_size);
	if (error == EXT4_OK && mapped) {
		error = ext4_write_map_trim(&allocation, &inode, disk, first, limit, &done);
	}
	if (error == EXT4_OK && done && inode.links != 0) {
		error = ext4_orphan_tail(&allocation, &inode, disk);
	}
	if (error == EXT4_OK) {
		error = ext4_inode_account(&allocation, &inode, disk, inode.size);
	}
	if (error == EXT4_OK && done) {
		if (inode.links == 0) {
			error = ext4_orphan_free_inode(&allocation, disk, &inode);
		} else {
			ext4_encode32(&disk->deletion_time, 0);
		}
		ext4_encode32(&allocation.super->last_orphan, next);
	}
	if (error != EXT4_OK) {
		goto cancel;
	}
	if (!done && allocation.freed == 0) {
		error = EXT4_CORRUPT;
		goto cancel;
	}
	ext4_inode_checksum_set(fs, number, disk);
	free_blocks = allocation.free_blocks;
	ext4_allocation_destroy(&allocation);
	error = ext4_transaction_commit(transaction);
	if (error != EXT4_OK) {
		fs->aborted = true;
		return error;
	}
	fs->info.free_blocks = free_blocks;
	report->orphan_transactions++;
	if (done) {
		fs->last_orphan = next;
		fs->info.free_inodes += inode.links == 0 ? 1U : 0U;
		report->cleaned_orphans++;
	}
	return EXT4_OK;
cancel:
	if (ready) {
		ext4_allocation_destroy(&allocation);
	}
	ext4_transaction_cancel(transaction);
	return error;
}

enum ext4_result
ext4_orphan_cleanup(struct ext4_fs *fs, struct ext4_recovery_report *report)
{
	uint32_t number;
	uint32_t limit;
	uint32_t cursor = 0;
	bool validate;
	enum ext4_result error;

	if (fs->last_orphan == 0 && (fs->orphan_file == NULL || fs->orphan_file->pending == 0)) {
		return EXT4_OK;
	}
	if (fs->first_inode < EXT4_FIRST_NON_RESERVED_INODE || fs->first_inode > fs->info.inodes ||
	    fs->blocks_per_group % EXT4_BITS_PER_BYTE != 0 ||
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
		limit = EXT4_ORPHAN_BATCH_BLOCKS;
		validate = true;
		do {
			error = ext4_orphan_step(fs, limit, validate, report);
			if (error == EXT4_RANGE && !fs->aborted && limit > 1) {
				limit /= 2;
				continue;
			}
			if (error != EXT4_OK) {
				return error;
			}
			validate = false;
		} while (fs->last_orphan == number);
	}
	return EXT4_OK;
}

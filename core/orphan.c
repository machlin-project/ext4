/* SPDX-License-Identifier: BSD-3-Clause */
#include "allocate.h"

static enum ext4_result
ext4_orphan_record(struct ext4_fs *fs, uint32_t number, struct ext4_inode_disk *disk,
    struct ext4_inode *inode, bool *mapped)
{
	uint16_t type;
	enum ext4_result error;

	if (number < fs->first_inode || number > fs->info.inodes || number == fs->journal_inode) {
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
ext4_orphan_chain(struct ext4_fs *fs)
{
	struct ext4_inode_disk *disk;
	struct ext4_inode inode;
	uint64_t offset = 0;
	uint64_t power = 1;
	uint64_t distance = 0;
	uint32_t number = fs->last_orphan;
	uint32_t anchor = number;
	uint32_t visited = 0;
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
		    number == fs->journal_inode || visited == fs->info.inodes) {
			error = EXT4_CORRUPT;
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
	bool validate;
	enum ext4_result error;

	if (fs->last_orphan == 0) {
		return EXT4_OK;
	}
	if (fs->first_inode < EXT4_FIRST_NON_RESERVED_INODE || fs->first_inode > fs->info.inodes ||
	    fs->blocks_per_group % EXT4_BITS_PER_BYTE != 0 ||
	    fs->inodes_per_group % EXT4_BITS_PER_BYTE != 0) {
		return EXT4_CORRUPT;
	}
	error = ext4_orphan_chain(fs);
	if (error != EXT4_OK) {
		return error;
	}
	while (fs->last_orphan != 0) {
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

/* SPDX-License-Identifier: BSD-3-Clause */
#include "xattr.h"
#include "allocate.h"

#define EXT4_XATTR_NAME_ROTATION 5U
#define EXT4_XATTR_VALUE_ROTATION 16U
#define EXT4_XATTR_HASH_WIDTH 32U

struct ext4_value_allocation {
	uint32_t allocated;
	uint64_t freed;
	uint64_t detached;
	uint64_t added;
	uint64_t removed;
	uint64_t mapping_size;
};

static void
ext4_value_allocation_begin(struct ext4_allocation *allocation, struct ext4_value_allocation *saved)
{
	saved->allocated = allocation->allocated;
	saved->freed = allocation->freed;
	saved->detached = allocation->detached_shared_blocks;
	saved->added = allocation->attribute_blocks_added;
	saved->removed = allocation->attribute_blocks_removed;
	saved->mapping_size = allocation->mapping_size;
	allocation->allocated = 0;
	allocation->freed = 0;
	allocation->detached_shared_blocks = 0;
	allocation->attribute_blocks_added = 0;
	allocation->attribute_blocks_removed = 0;
	allocation->mapping_size = 0;
}

static void
ext4_value_allocation_end(
    struct ext4_allocation *allocation, const struct ext4_value_allocation *saved)
{
	allocation->allocated = saved->allocated;
	allocation->freed = saved->freed;
	allocation->detached_shared_blocks = saved->detached;
	allocation->attribute_blocks_added = saved->added;
	allocation->attribute_blocks_removed = saved->removed;
	allocation->mapping_size = saved->mapping_size;
}

uint64_t
ext4_xattr_value_blocks(const struct ext4_xattr_snapshot *snapshot)
{
	uint64_t blocks = 0;
	size_t index;

	for (index = 0; index < snapshot->count; index++) {
		if (snapshot->records[index].inode_storage) {
			blocks +=
			    ((uint64_t)ext4_le32(&snapshot->records[index].entry->value_size) +
				snapshot->fs->info.block_size - 1U) /
			    snapshot->fs->info.block_size;
		}
	}
	return blocks;
}

uint32_t
ext4_xattr_inode_entry_hash(
    const struct ext4_xattr_entry_disk *entry, uint32_t value_hash, bool signed_names)
{
	const uint8_t *name = (const uint8_t *)(entry + 1);
	uint32_t hash = 0;
	uint32_t word;
	size_t index;

	for (index = 0; index < entry->name_length; index++) {
		word = name[index];
		if (signed_names && word > INT8_MAX) {
			word |= UINT32_MAX << 8;
		}
		hash = ((hash << EXT4_XATTR_NAME_ROTATION) |
			   (hash >> (EXT4_XATTR_HASH_WIDTH - EXT4_XATTR_NAME_ROTATION))) ^
		    word;
	}
	return ((hash << EXT4_XATTR_VALUE_ROTATION) |
		   (hash >> (EXT4_XATTR_HASH_WIDTH - EXT4_XATTR_VALUE_ROTATION))) ^
	    value_hash;
}

enum ext4_result
ext4_xattr_inode_read(struct ext4_fs *fs, uint32_t parent, uint32_t generation,
    const struct ext4_xattr_entry_disk *entry, void *output, uint32_t *value_hash)
{
	struct ext4_inode inode;
	struct ext4_inode_disk *disk;
	uint8_t *value = NULL;
	uint64_t location = 0;
	uint64_t physical;
	uint64_t references;
	uint32_t number = ext4_le32(&entry->value_inode);
	uint32_t size = ext4_le32(&entry->value_size);
	uint32_t hash = 0;
	uint32_t position;
	uint32_t amount;
	enum ext4_result error;

	if (!(fs->info.feature_incompat & EXT4_FEATURE_INCOMPAT_EA_INODE) ||
	    number < fs->first_inode || number > fs->info.inodes || number == parent ||
	    number == fs->journal_inode || number == fs->orphan_file_inode ||
	    ext4_le16(&entry->value_offset) != 0 || size == 0 || size > EXT4_XATTR_VALUE_MAX) {
		return EXT4_CORRUPT;
	}
	error = ext4_inode_allocated(fs, number);
	if (error == EXT4_OK) {
		error = ext4_inode_location(fs, number, &location);
	}
	if (error != EXT4_OK) {
		return error;
	}
	disk = fs->environment.allocate(fs->environment.context, fs->inode_size);
	if (disk == NULL) {
		return EXT4_NO_MEMORY;
	}
	error = ext4_device_read(fs, location, disk, fs->inode_size);
	if (error == EXT4_OK) {
		error = ext4_inode_decode(fs, number, disk, &inode);
	}
	if (error != EXT4_OK) {
		error = error == EXT4_NOT_FOUND ? EXT4_CORRUPT : error;
		goto out;
	}
	references = (uint64_t)ext4_le32(&disk->change_time) << 32 | ext4_le32(&disk->version_lo);
	hash = ext4_le32(&disk->access_time);
	if ((inode.mode & EXT4_MODE_TYPE) != EXT4_MODE_REGULAR ||
	    !(inode.flags & EXT4_INODE_EA_INODE) || inode.links != 1 || inode.size != size ||
	    ext4_le32(&disk->deletion_time) != 0 || ext4_inode_has_xattrs(fs, disk)) {
		error = EXT4_CORRUPT;
		goto out;
	}
	if (ext4_le32(&entry->hash) != ext4_xattr_inode_entry_hash(entry, hash, false) &&
	    ext4_le32(&entry->hash) != ext4_xattr_inode_entry_hash(entry, hash, true)) {
		/* Old Lustre values use an owning-inode backpointer instead of modern
		 * checksums/reference counts. Keep that variant explicit. */
		error = ext4_le32(&disk->modify_time) == parent && inode.generation == generation
		    ? EXT4_UNSUPPORTED
		    : EXT4_CORRUPT;
		goto out;
	}
	if (references == 0 || references > INT64_MAX) {
		error = EXT4_CORRUPT;
		goto out;
	}
	if (output == NULL) {
		*value_hash = hash;
		goto out;
	}
	value = fs->environment.allocate(fs->environment.context, size);
	if (value == NULL) {
		error = EXT4_NO_MEMORY;
		goto out;
	}
	for (position = 0; position < size; position += amount) {
		amount = size - position;
		if (amount > fs->info.block_size) {
			amount = fs->info.block_size;
		}
		error = ext4_map_block(fs, &inode, position / fs->info.block_size, &physical);
		if (error == EXT4_OK && physical == 0) {
			error = EXT4_CORRUPT;
		}
		if (error == EXT4_OK) {
			error = fs->journal == NULL ? ext4_block_allocated(fs, physical)
						    : ext4_data_block_valid(fs, physical);
		}
		if (error == EXT4_OK) {
			error = ext4_device_read(
			    fs, physical * fs->info.block_size, value + position, amount);
		}
		if (error != EXT4_OK) {
			goto out;
		}
	}
	if (ext4_crc32c(fs->checksum_seed, value, size) != hash) {
		error = EXT4_CORRUPT;
		goto out;
	}
	ext4_copy(output, value, size);
	*value_hash = hash;
out:
	if (value != NULL) {
		fs->environment.release(fs->environment.context, value, size);
	}
	fs->environment.release(fs->environment.context, disk, fs->inode_size);
	return error;
}

enum ext4_result
ext4_xattr_inode_create(struct ext4_allocation *allocation, const struct ext4_inode *parent,
    struct ext4_xattr_record *record)
{
	struct ext4_fs *fs = allocation->fs;
	struct ext4_value_allocation saved;
	struct ext4_inode inode;
	struct ext4_inode_disk *disk;
	uint32_t size = ext4_le32(&record->entry->value_size);
	uint32_t position;
	uint32_t amount;
	uint64_t physical;
	void *buffer = NULL;
	bool allocated;
	enum ext4_result error;

	ext4_value_allocation_begin(allocation, &saved);
	error = ext4_allocate_inode(allocation, EXT4_MODE_REGULAR, &disk, &inode);
	if (error != EXT4_OK) {
		goto out;
	}
	ext4_encode16(&disk->mode, EXT4_MODE_REGULAR | 0600U);
	ext4_encode16(&disk->uid_lo, (uint16_t)parent->uid);
	ext4_encode16(&disk->uid_hi, (uint16_t)(parent->uid >> 16));
	ext4_encode16(&disk->gid_lo, (uint16_t)parent->gid);
	ext4_encode16(&disk->gid_hi, (uint16_t)(parent->gid >> 16));
	inode.flags |= EXT4_INODE_EA_INODE;
	ext4_encode32(&disk->flags, inode.flags);
	record->value_hash = ext4_crc32c(fs->checksum_seed, record->value, size);
	ext4_encode32(&disk->access_time, record->value_hash);
	ext4_encode32(&disk->version_lo, 1);
	for (position = 0; position < size; position += amount) {
		amount = size - position;
		if (amount > fs->info.block_size) {
			amount = fs->info.block_size;
		}
		error = ext4_write_map_allocate(allocation, &inode, disk,
		    position / fs->info.block_size, &physical, &allocated);
		if (error == EXT4_OK && !allocated) {
			error = EXT4_CORRUPT;
		}
		if (error == EXT4_OK) {
			error = ext4_transaction_buffer(allocation->transaction, physical, &buffer);
		}
		if (error != EXT4_OK) {
			goto out;
		}
		ext4_zero(buffer, fs->info.block_size);
		ext4_copy(buffer, record->value + position, amount);
	}
	error = ext4_inode_account(allocation, &inode, disk, size);
	if (error == EXT4_OK) {
		ext4_inode_checksum_set(fs, inode.number, disk);
		record->value_inode = inode.number;
		record->new_inode = true;
	}
out:
	ext4_value_allocation_end(allocation, &saved);
	return error;
}

enum ext4_result
ext4_xattr_inode_adjust(
    struct ext4_allocation *allocation, const struct ext4_xattr_record *record, int change)
{
	struct ext4_fs *fs = allocation->fs;
	struct ext4_inode inode;
	struct ext4_inode_disk *disk;
	struct ext4_value_allocation saved;
	uint64_t location;
	uint64_t references;
	void *buffer = NULL;
	bool done = false;
	enum ext4_result error;

	error = ext4_inode_location(fs, record->value_inode, &location);
	if (error == EXT4_OK) {
		error = ext4_transaction_buffer(
		    allocation->transaction, location / fs->info.block_size, &buffer);
	}
	if (error != EXT4_OK) {
		return error;
	}
	disk = (struct ext4_inode_disk *)((uint8_t *)buffer + location % fs->info.block_size);
	error = ext4_inode_decode(fs, record->value_inode, disk, &inode);
	if (error != EXT4_OK) {
		return error;
	}
	references = (uint64_t)ext4_le32(&disk->change_time) << 32 | ext4_le32(&disk->version_lo);
	if ((change != -1 && change != 1) || references == 0 || references > INT64_MAX ||
	    inode.links != 1 || !(inode.flags & EXT4_INODE_EA_INODE) ||
	    inode.size != ext4_le32(&record->entry->value_size) ||
	    ext4_le32(&disk->access_time) != record->value_hash) {
		return EXT4_CORRUPT;
	}
	if (change > 0 && references == INT64_MAX) {
		return EXT4_RANGE;
	}
	references = change > 0 ? references + 1U : references - 1U;
	if (references != 0) {
		ext4_encode32(&disk->change_time, (uint32_t)(references >> 32));
		ext4_encode32(&disk->version_lo, (uint32_t)references);
		ext4_inode_checksum_set(fs, inode.number, disk);
		return EXT4_OK;
	}
	/* The value remains reachable until this private transaction commits.
	 * Cancelled credit exhaustion cannot publish a dangling reference. */
	ext4_value_allocation_begin(allocation, &saved);
	error = ext4_write_map_validate(allocation, &inode, disk);
	if (error == EXT4_OK) {
		error = ext4_write_map_trim(allocation, &inode, disk, 0, UINT32_MAX, &done);
	}
	if (error == EXT4_OK && !done) {
		error = EXT4_CORRUPT;
	}
	if (error == EXT4_OK) {
		error = ext4_free_inode(allocation, disk, &inode);
	}
	ext4_value_allocation_end(allocation, &saved);
	return error;
}

/* SPDX-License-Identifier: BSD-3-Clause */
#include "xattr.h"
#include "allocate.h"

#define EXT4_XATTR_NAME_HASH_SHIFT 5U
#define EXT4_XATTR_VALUE_HASH_SHIFT 16U
#define EXT4_XATTR_HASH_BITS 32U

static size_t
ext4_xattr_aligned(size_t size)
{

	return (size + EXT4_XATTR_ALIGNMENT - 1) & ~(size_t)(EXT4_XATTR_ALIGNMENT - 1);
}

int
ext4_xattr_compare(
    const struct ext4_xattr_entry_disk *left, const struct ext4_xattr_entry_disk *right)
{
	const uint8_t *left_name = (const uint8_t *)(left + 1);
	const uint8_t *right_name = (const uint8_t *)(right + 1);
	size_t index;

	if (left->name_index != right->name_index) {
		return left->name_index < right->name_index ? -1 : 1;
	}
	if (left->name_length != right->name_length) {
		return left->name_length < right->name_length ? -1 : 1;
	}
	for (index = 0; index < left->name_length; index++) {
		if (left_name[index] != right_name[index]) {
			return left_name[index] < right_name[index] ? -1 : 1;
		}
	}
	return 0;
}

static void
ext4_xattr_sift(struct ext4_xattr_record *records, size_t root, size_t count)
{
	struct ext4_xattr_record saved = records[root];
	size_t child;

	while (root < count / 2) {
		child = root * 2 + 1;
		if (child + 1 < count &&
		    ext4_xattr_compare(records[child].entry, records[child + 1].entry) < 0) {
			child++;
		}
		if (ext4_xattr_compare(saved.entry, records[child].entry) >= 0) {
			break;
		}
		records[root] = records[child];
		root = child;
	}
	records[root] = saved;
}

static enum ext4_result
ext4_xattr_sort(struct ext4_xattr_snapshot *snapshot)
{
	struct ext4_xattr_record saved;
	size_t index;

	for (index = snapshot->count / 2; index > 0; index--) {
		ext4_xattr_sift(snapshot->records, index - 1, snapshot->count);
	}
	for (index = snapshot->count; index > 1; index--) {
		saved = snapshot->records[0];
		snapshot->records[0] = snapshot->records[index - 1];
		snapshot->records[index - 1] = saved;
		ext4_xattr_sift(snapshot->records, 0, index - 1);
	}
	for (index = 1; index < snapshot->count; index++) {
		if (ext4_xattr_compare(
			snapshot->records[index - 1].entry, snapshot->records[index].entry) == 0) {
			return EXT4_CORRUPT;
		}
	}
	return EXT4_OK;
}

static uint32_t
ext4_xattr_hash_step(uint32_t hash, uint32_t value, unsigned int shift)
{

	return ((hash << shift) | (hash >> (EXT4_XATTR_HASH_BITS - shift))) ^ value;
}

uint32_t
ext4_xattr_hash(const struct ext4_xattr_entry_disk *entry, const uint8_t *value, bool signed_names)
{
	const uint8_t *name = (const uint8_t *)(entry + 1);
	uint32_t hash = 0;
	uint32_t word;
	size_t index;
	size_t value_size = ext4_le32(&entry->value_size);

	for (index = 0; index < entry->name_length; index++) {
		word = name[index];
		if (signed_names && word > INT8_MAX) {
			word |= UINT32_MAX << 8;
		}
		hash = ext4_xattr_hash_step(hash, word, EXT4_XATTR_NAME_HASH_SHIFT);
	}
	for (index = 0; index < ext4_xattr_aligned(value_size); index += sizeof(struct ext4_le32)) {
		word = ext4_le32((const struct ext4_le32 *)(value + index));
		hash = ext4_xattr_hash_step(hash, word, EXT4_XATTR_VALUE_HASH_SHIFT);
	}
	return hash;
}

static enum ext4_result
ext4_xattr_region(struct ext4_xattr_snapshot *snapshot, const uint8_t *buffer, size_t size,
    size_t entries_offset, size_t value_base, bool sorted)
{
	const struct ext4_xattr_entry_disk *entry;
	struct ext4_xattr_record *record;
	size_t offset = entries_offset;
	size_t first = snapshot->count;
	size_t length;
	size_t index;
	size_t name_index;
	size_t value_offset;
	size_t value_size;
	uint32_t hash = 0;
	bool hashable = true;
	enum ext4_result error;

	for (;;) {
		if (offset > size || sizeof(struct ext4_le32) > size - offset) {
			return EXT4_CORRUPT;
		}
		if (ext4_le32((const struct ext4_le32 *)(buffer + offset)) == 0) {
			offset += sizeof(struct ext4_le32);
			break;
		}
		if (sizeof(*entry) > size - offset || snapshot->count == snapshot->capacity) {
			return EXT4_CORRUPT;
		}
		entry = (const struct ext4_xattr_entry_disk *)(buffer + offset);
		length = ext4_xattr_aligned(sizeof(*entry) + entry->name_length);
		if (length > size - offset || (entry->name_index == 0 && entry->name_length == 0)) {
			return EXT4_CORRUPT;
		}
		for (name_index = 0; name_index < entry->name_length; name_index++) {
			if (((const uint8_t *)(entry + 1))[name_index] == 0) {
				return EXT4_CORRUPT;
			}
		}
		if (sorted && snapshot->count != first &&
		    ext4_xattr_compare(snapshot->records[snapshot->count - 1].entry, entry) >= 0) {
			return EXT4_CORRUPT;
		}
		ext4_zero(&snapshot->records[snapshot->count], sizeof(*record));
		snapshot->records[snapshot->count].external = sorted;
		snapshot->records[snapshot->count++].entry = entry;
		offset += length;
	}
	for (index = first; index < snapshot->count; index++) {
		record = &snapshot->records[index];
		entry = record->entry;
		value_offset = (size_t)ext4_le16(&entry->value_offset) + value_base;
		value_size = ext4_le32(&entry->value_size);
		record->value_inode = ext4_le32(&entry->value_inode);
		record->inode_storage = record->value_inode != 0;
		if (record->inode_storage) {
			error = ext4_xattr_inode_read(snapshot->fs, snapshot->number,
			    snapshot->generation, entry, NULL, &record->value_hash);
			if (error != EXT4_OK) {
				return error;
			}
		} else if (value_offset > size || value_size > size - value_offset ||
		    ext4_xattr_aligned(value_size) > size - value_offset ||
		    (value_size != 0 &&
			(value_offset < offset ||
			    (value_offset & (EXT4_XATTR_ALIGNMENT - 1)) != 0))) {
			return EXT4_CORRUPT;
		}
		if (!record->inode_storage) {
			record->value = buffer + value_offset;
		}
		if (!record->inode_storage && ext4_le32(&entry->hash) != 0 &&
		    ext4_le32(&entry->hash) != ext4_xattr_hash(entry, record->value, false) &&
		    ext4_le32(&entry->hash) != ext4_xattr_hash(entry, record->value, true)) {
			return EXT4_CORRUPT;
		}
		if (ext4_le32(&entry->hash) == 0) {
			hashable = false;
		}
		hash = ext4_xattr_hash_step(
		    hash, ext4_le32(&entry->hash), EXT4_XATTR_VALUE_HASH_SHIFT);
	}
	if (sorted) {
		/* A zero block hash is valid for an unshared or legacy attribute block. */
		if (ext4_le32(&((const struct ext4_xattr_header_disk *)buffer)->hash) != 0 &&
		    (!hashable ||
			hash !=
			    ext4_le32(&((const struct ext4_xattr_header_disk *)buffer)->hash))) {
			return EXT4_CORRUPT;
		}
	}
	return EXT4_OK;
}

void
ext4_xattr_close(struct ext4_xattr_snapshot *snapshot)
{
	struct ext4_environment *environment = &snapshot->fs->environment;

	if (snapshot->records != NULL) {
		environment->release(environment->context, snapshot->records,
		    snapshot->capacity * sizeof(*snapshot->records));
	}
	if (snapshot->block != NULL) {
		environment->release(
		    environment->context, snapshot->block, snapshot->fs->info.block_size);
	}
	if (snapshot->inode != NULL) {
		environment->release(
		    environment->context, snapshot->inode, snapshot->fs->inode_size);
	}
}

void
ext4_xattr_checksum_set(struct ext4_fs *fs, uint64_t block, struct ext4_xattr_header_disk *header)
{
	struct ext4_block_number_disk address;
	uint32_t checksum;

	if (!fs->metadata_checksum) {
		return;
	}
	ext4_encode32(&header->checksum, 0);
	ext4_encode32(&address.low, (uint32_t)block);
	ext4_encode32(&address.high, (uint32_t)(block >> 32));
	checksum = ext4_crc32c(fs->checksum_seed, &address, sizeof(address));
	checksum = ext4_crc32c(checksum, header, fs->info.block_size);
	ext4_encode32(&header->checksum, checksum);
}

static enum ext4_result
ext4_xattr_external(struct ext4_xattr_snapshot *snapshot, uint64_t block)
{
	struct ext4_fs *fs = snapshot->fs;
	struct ext4_xattr_header_disk *header;
	uint32_t expected;
	size_t index;
	enum ext4_result error;

	error = fs->journal == NULL ? ext4_block_allocated(fs, block)
				    : ext4_data_block_valid(fs, block);
	if (error != EXT4_OK) {
		return error;
	}
	snapshot->block = fs->environment.allocate(fs->environment.context, fs->info.block_size);
	if (snapshot->block == NULL) {
		return EXT4_NO_MEMORY;
	}
	error = snapshot->transaction == NULL
	    ? ext4_block_read(fs, block, snapshot->block)
	    : ext4_transaction_read(snapshot->transaction, block, snapshot->block);
	if (error != EXT4_OK) {
		return error;
	}
	header = (struct ext4_xattr_header_disk *)snapshot->block;
	if (fs->metadata_checksum) {
		expected = ext4_le32(&header->checksum);
		ext4_xattr_checksum_set(fs, block, header);
		if (ext4_le32(&header->checksum) != expected) {
			return EXT4_CORRUPT;
		}
	}
	if (ext4_le32(&header->magic) != EXT4_XATTR_MAGIC || ext4_le32(&header->blocks) != 1 ||
	    ext4_le32(&header->references) == 0 ||
	    ext4_le32(&header->references) > EXT4_XATTR_REFCOUNT_MAX) {
		return EXT4_CORRUPT;
	}
	for (index = 0; index < sizeof(header->reserved) / sizeof(header->reserved[0]); index++) {
		if (ext4_le32(&header->reserved[index]) != 0) {
			return EXT4_CORRUPT;
		}
	}
	return EXT4_OK;
}

static enum ext4_result
ext4_xattr_parse_inode(struct ext4_xattr_snapshot *snapshot, const struct ext4_inode *inode)
{
	struct ext4_fs *fs = snapshot->fs;
	struct ext4_inode_disk *disk = (struct ext4_inode_disk *)snapshot->inode;
	uint64_t offset;
	uint64_t block;
	size_t body = fs->inode_size;
	size_t extra;
	bool body_present = false;
	enum ext4_result error;

	snapshot->number = inode->number;
	snapshot->generation = inode->generation;
	block =
	    ext4_le32(&disk->xattr_block_lo) | ((uint64_t)ext4_le16(&disk->xattr_block_hi) << 32);
	snapshot->external_block = block;
	if (fs->inode_size > EXT4_INODE_BASE_SIZE) {
		extra = ext4_le16(&disk->extra_size);
		if (extra > fs->inode_size - EXT4_INODE_BASE_SIZE ||
		    extra % EXT4_XATTR_ALIGNMENT != 0) {
			return EXT4_CORRUPT;
		}
		offset = EXT4_INODE_BASE_SIZE + extra;
		if (extra != 0 && offset <= fs->inode_size - sizeof(struct ext4_le32) &&
		    ext4_le32((const struct ext4_le32 *)(snapshot->inode + offset)) ==
			EXT4_XATTR_MAGIC) {
			body = (size_t)offset + sizeof(struct ext4_le32);
			body_present = true;
		}
	}
	if ((body_present || block != 0) &&
	    !(fs->info.feature_compat & EXT4_FEATURE_COMPAT_EXT_ATTR)) {
		return EXT4_CORRUPT;
	}
	if (block != 0) {
		if (inode->blocks_512 <
		    (uint64_t)fs->cluster_blocks * (fs->info.block_size / EXT4_SECTOR_SIZE)) {
			return EXT4_CORRUPT;
		}
		error = ext4_xattr_external(snapshot, block);
		if (error != EXT4_OK) {
			return error;
		}
	}
	snapshot->capacity = (fs->inode_size - body + (block == 0 ? 0 : fs->info.block_size)) /
	    sizeof(struct ext4_xattr_entry_disk);
	if (snapshot->capacity != 0) {
		snapshot->records = fs->environment.allocate(
		    fs->environment.context, snapshot->capacity * sizeof(*snapshot->records));
		if (snapshot->records == NULL) {
			return EXT4_NO_MEMORY;
		}
	}
	if (body_present) {
		error =
		    ext4_xattr_region(snapshot, snapshot->inode, fs->inode_size, body, body, false);
		if (error != EXT4_OK) {
			return error;
		}
	}
	if (block != 0) {
		error = ext4_xattr_region(snapshot, snapshot->block, fs->info.block_size,
		    sizeof(struct ext4_xattr_header_disk), 0, true);
		if (error != EXT4_OK) {
			return error;
		}
	}
	if (inode->blocks_512 <
	    (ext4_xattr_value_blocks(snapshot) + (block == 0 ? 0 : fs->cluster_blocks)) *
		(fs->info.block_size / EXT4_SECTOR_SIZE)) {
		return EXT4_CORRUPT;
	}
	return ext4_xattr_sort(snapshot);
}

static enum ext4_result
ext4_xattr_open_record(struct ext4_fs *fs, struct ext4_transaction *transaction,
    const struct ext4_inode *inode, const struct ext4_inode_disk *disk,
    struct ext4_xattr_snapshot *snapshot)
{
	ext4_zero(snapshot, sizeof(*snapshot));
	snapshot->fs = fs;
	snapshot->transaction = transaction;
	if (fs->aborted) {
		return EXT4_RECOVERY_REQUIRED;
	}
	snapshot->inode = fs->environment.allocate(fs->environment.context, fs->inode_size);
	if (snapshot->inode == NULL) {
		return EXT4_NO_MEMORY;
	}
	ext4_copy(snapshot->inode, disk, fs->inode_size);
	return ext4_xattr_parse_inode(snapshot, inode);
}

enum ext4_result
ext4_xattr_open_inode(struct ext4_fs *fs, const struct ext4_inode *inode,
    const struct ext4_inode_disk *disk, struct ext4_xattr_snapshot *snapshot)
{

	return ext4_xattr_open_record(fs, NULL, inode, disk, snapshot);
}

enum ext4_result
ext4_xattr_open_transaction(struct ext4_allocation *allocation, const struct ext4_inode *inode,
    const struct ext4_inode_disk *disk, struct ext4_xattr_snapshot *snapshot)
{

	return ext4_xattr_open_record(
	    allocation->fs, allocation->transaction, inode, disk, snapshot);
}

enum ext4_result
ext4_xattr_open(
    struct ext4_fs *fs, uint32_t number, uint32_t generation, struct ext4_xattr_snapshot *snapshot)
{
	struct ext4_inode inode;
	uint64_t offset = 0;
	enum ext4_result error;

	ext4_zero(snapshot, sizeof(*snapshot));
	snapshot->fs = fs;
	if (fs->aborted) {
		return EXT4_RECOVERY_REQUIRED;
	}
	error = ext4_inode_allocated(fs, number);
	if (error == EXT4_OK) {
		error = ext4_inode_location(fs, number, &offset);
	}
	if (error != EXT4_OK) {
		return error;
	}
	snapshot->inode = fs->environment.allocate(fs->environment.context, fs->inode_size);
	if (snapshot->inode == NULL) {
		return EXT4_NO_MEMORY;
	}
	error = ext4_device_read(fs, offset, snapshot->inode, fs->inode_size);
	if (error == EXT4_OK) {
		error = ext4_inode_decode_live(fs, number, snapshot->inode, &inode);
	}
	if (error != EXT4_OK) {
		return error;
	}
	if (inode.generation != generation) {
		return EXT4_STALE;
	}
	return ext4_xattr_parse_inode(snapshot, &inode);
}

enum ext4_result
ext4_get_xattr(struct ext4_fs *fs, uint32_t number, uint32_t generation, uint8_t name_index,
    const uint8_t *name, size_t name_length, void *buffer, size_t capacity, size_t *size)
{
	struct ext4_xattr_snapshot snapshot;
	const struct ext4_xattr_entry_disk *entry;
	size_t index;
	size_t value_size;
	enum ext4_result error;

	if (fs == NULL || number == 0 || number > fs->info.inodes || size == NULL ||
	    name_length > EXT4_NAME_MAX || (name == NULL && name_length != 0) ||
	    (buffer == NULL && capacity != 0) || (name_index == 0 && name_length == 0)) {
		return EXT4_INVALID_ARGUMENT;
	}
	for (index = 0; index < name_length; index++) {
		if (name[index] == 0) {
			return EXT4_INVALID_ARGUMENT;
		}
	}
	error = ext4_xattr_open(fs, number, generation, &snapshot);
	if (error == EXT4_OK) {
		error = EXT4_NOT_FOUND;
		for (index = 0; index < snapshot.count; index++) {
			entry = snapshot.records[index].entry;
			if (entry->name_index != name_index || entry->name_length != name_length ||
			    !ext4_equal(entry + 1, name, name_length)) {
				continue;
			}
			value_size = ext4_le32(&entry->value_size);
			if (buffer != NULL && capacity < value_size) {
				error = EXT4_RANGE;
				break;
			}
			if (buffer != NULL) {
				if (snapshot.records[index].inode_storage) {
					error = ext4_xattr_inode_read(fs, number, generation, entry,
					    buffer, &snapshot.records[index].value_hash);
					if (error != EXT4_OK) {
						break;
					}
				} else {
					ext4_copy(
					    buffer, snapshot.records[index].value, value_size);
				}
			}
			*size = value_size;
			error = EXT4_OK;
			break;
		}
	}
	ext4_xattr_close(&snapshot);
	return error;
}

enum ext4_result
ext4_list_xattrs(struct ext4_fs *fs, uint32_t number, uint32_t generation,
    struct ext4_xattr_key *keys, size_t capacity, size_t *count)
{
	struct ext4_xattr_snapshot snapshot;
	const struct ext4_xattr_entry_disk *entry;
	size_t index;
	enum ext4_result error;

	if (fs == NULL || number == 0 || number > fs->info.inodes || count == NULL ||
	    (keys == NULL && capacity != 0)) {
		return EXT4_INVALID_ARGUMENT;
	}
	error = ext4_xattr_open(fs, number, generation, &snapshot);
	if (error == EXT4_OK && keys != NULL && capacity < snapshot.count) {
		error = EXT4_RANGE;
	}
	if (error == EXT4_OK) {
		if (keys != NULL) {
			for (index = 0; index < snapshot.count; index++) {
				entry = snapshot.records[index].entry;
				ext4_zero(&keys[index], sizeof(keys[index]));
				keys[index].name_index = entry->name_index;
				keys[index].name_length = entry->name_length;
				keys[index].value_size = ext4_le32(&entry->value_size);
				ext4_copy(keys[index].name, entry + 1, entry->name_length);
			}
		}
		*count = snapshot.count;
	}
	ext4_xattr_close(&snapshot);
	return error;
}

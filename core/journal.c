/* SPDX-License-Identifier: BSD-3-Clause */
#include "journal.h"

struct ext4_transaction_entry {
	uint64_t block;
	void *buffer;
};

struct ext4_transaction {
	struct ext4_journal *journal;
	uint32_t credits;
	uint32_t count;
	struct ext4_transaction_entry entries[];
};

uint16_t
ext4_be16(const struct ext4_be16 *value)
{
	return (uint16_t)((uint16_t)value->bytes[0] << 8) | value->bytes[1];
}

uint32_t
ext4_be32(const struct ext4_be32 *value)
{
	return ((uint32_t)value->bytes[0] << 24) | ((uint32_t)value->bytes[1] << 16) |
	    ((uint32_t)value->bytes[2] << 8) | value->bytes[3];
}

void
ext4_encode_be16(struct ext4_be16 *output, uint16_t value)
{
	output->bytes[0] = (uint8_t)(value >> 8);
	output->bytes[1] = (uint8_t)value;
}

void
ext4_encode_be32(struct ext4_be32 *output, uint32_t value)
{
	unsigned int index;

	for (index = 0; index < sizeof(output->bytes); index++) {
		output->bytes[index] = (uint8_t)(value >> ((3U - index) * 8));
	}
}

static enum ext4_result
ext4_journal_physical(const struct ext4_journal *journal, uint32_t block, uint64_t *physical)
{
	const struct ext4_journal_run *run;
	uint32_t index;

	for (index = 0; index < journal->run_count; index++) {
		run = &journal->runs[index];
		if (block >= run->logical && block - run->logical < run->length) {
			*physical = run->physical + (block - run->logical);
			return EXT4_OK;
		}
	}
	return EXT4_CORRUPT;
}

bool
ext4_journal_target(const struct ext4_journal *journal, uint64_t block)
{
	const struct ext4_journal_run *run;
	uint32_t index;

	if (block < journal->fs->first_data_block || block >= journal->fs->info.blocks) {
		return false;
	}
	for (index = 0; index < journal->run_count; index++) {
		run = &journal->runs[index];
		if (block >= run->physical && block - run->physical < run->length) {
			return false;
		}
	}
	return true;
}

enum ext4_result
ext4_journal_read(struct ext4_journal *journal, uint32_t block, void *buffer)
{
	uint64_t physical;
	enum ext4_result error;

	if (block >= journal->blocks) {
		return EXT4_CORRUPT;
	}
	error = ext4_journal_physical(journal, block, &physical);
	if (error == EXT4_OK) {
		error = ext4_block_read(journal->fs, physical, buffer);
	}
	return error;
}

static enum ext4_result
ext4_journal_write(struct ext4_journal *journal, uint64_t physical, const void *buffer)
{
	enum ext4_result error;

	if (journal->aborted) {
		return EXT4_IO;
	}
	if (physical < journal->fs->first_data_block || physical >= journal->fs->info.blocks) {
		return EXT4_CORRUPT;
	}
	error = journal->writer.write(journal->writer.context,
	    physical * journal->fs->info.block_size, buffer, journal->fs->info.block_size);
	if (error != EXT4_OK) {
		journal->aborted = true;
	}
	return error;
}

enum ext4_result
ext4_journal_write_home(struct ext4_journal *journal, uint64_t block, const void *buffer)
{
	if (!ext4_journal_target(journal, block)) {
		return EXT4_CORRUPT;
	}
	return ext4_journal_write(journal, block, buffer);
}

static enum ext4_result
ext4_journal_write_log(struct ext4_journal *journal, uint32_t block, const void *buffer)
{
	uint64_t physical;
	enum ext4_result error;

	if (block >= journal->blocks) {
		return EXT4_CORRUPT;
	}
	error = ext4_journal_physical(journal, block, &physical);
	if (error == EXT4_OK) {
		error = ext4_journal_write(journal, physical, buffer);
	}
	return error;
}

enum ext4_result
ext4_journal_flush(struct ext4_journal *journal)
{
	enum ext4_result error;

	if (journal->aborted) {
		return EXT4_IO;
	}
	error = journal->writer.flush(journal->writer.context);
	if (error != EXT4_OK) {
		journal->aborted = true;
	}
	return error;
}

uint32_t
ext4_journal_next(const struct ext4_journal *journal, uint32_t block)
{
	return block + 1 == journal->blocks ? journal->first : block + 1;
}

size_t
ext4_journal_tag_size(const struct ext4_journal *journal)
{
	size_t length;

	if (journal->features & EXT4_JBD_CSUM_V3) {
		return sizeof(struct ext4_jbd_tag3);
	}
	length = sizeof(struct ext4_jbd_tag);
	if (journal->features & EXT4_JBD_64BIT) {
		length += sizeof(struct ext4_be32);
	}
	/* The v2 wire ABI includes two unused bytes after the base tag. */
	if (journal->features & EXT4_JBD_CSUM_V2) {
		length += sizeof(struct ext4_be16);
	}
	return length;
}

uint32_t
ext4_journal_data_checksum(
    const struct ext4_journal *journal, uint32_t sequence, const void *buffer)
{
	struct ext4_be32 wire;
	uint32_t checksum;

	ext4_encode_be32(&wire, sequence);
	checksum = ext4_crc32c(journal->checksum_seed, &wire, sizeof(wire));
	return ext4_crc32c(checksum, buffer, journal->fs->info.block_size);
}

bool
ext4_journal_checksum_valid(struct ext4_journal *journal, void *buffer, size_t offset)
{
	struct ext4_be32 *field = (struct ext4_be32 *)((uint8_t *)buffer + offset);
	uint32_t expected;
	uint32_t checksum;

	if (!journal->checksum) {
		return true;
	}
	expected = ext4_be32(field);
	ext4_encode_be32(field, 0);
	checksum = ext4_crc32c(journal->checksum_seed, buffer, journal->fs->info.block_size);
	ext4_encode_be32(field, expected);
	return checksum == expected;
}

void
ext4_journal_checksum_set(struct ext4_journal *journal, void *buffer, size_t offset)
{
	struct ext4_be32 *field = (struct ext4_be32 *)((uint8_t *)buffer + offset);
	uint32_t checksum;

	if (journal->checksum) {
		ext4_encode_be32(field, 0);
		checksum =
		    ext4_crc32c(journal->checksum_seed, buffer, journal->fs->info.block_size);
		ext4_encode_be32(field, checksum);
	}
}

static enum ext4_result
ext4_journal_map(struct ext4_journal *journal, const struct ext4_inode *inode)
{
	struct ext4_journal_run *run = NULL;
	uint64_t physical;
	uint32_t logical;
	uint32_t left;
	uint32_t right;
	enum ext4_result error;

	for (logical = 0; logical < journal->blocks; logical++) {
		error = ext4_map_block(journal->fs, inode, logical, &physical);
		if (error != EXT4_OK) {
			return error;
		}
		/* A journal cannot be sparse or overlap the primary control blocks. */
		if (physical <= journal->fs->first_data_block + 1) {
			return EXT4_CORRUPT;
		}
		if (run != NULL && physical == run->physical + run->length) {
			run->length++;
		} else {
			if (journal->run_count == EXT4_JOURNAL_MAX_RUNS) {
				return EXT4_UNSUPPORTED;
			}
			run = &journal->runs[journal->run_count++];
			run->physical = physical;
			run->logical = logical;
			run->length = 1;
		}
	}
	for (left = 0; left < journal->run_count; left++) {
		for (right = left + 1; right < journal->run_count; right++) {
			if (journal->runs[left].physical <
				journal->runs[right].physical + journal->runs[right].length &&
			    journal->runs[right].physical <
				journal->runs[left].physical + journal->runs[left].length) {
				return EXT4_CORRUPT;
			}
		}
	}
	return EXT4_OK;
}

static enum ext4_result
ext4_journal_validate(struct ext4_journal *journal)
{
	struct ext4_jbd_super *super = (struct ext4_jbd_super *)journal->super_buffer;
	uint32_t expected;
	uint32_t checksum;
	uint32_t length;

	if (ext4_be32(&super->header.magic) != EXT4_JBD_MAGIC) {
		return EXT4_CORRUPT;
	}
	if (ext4_be32(&super->header.type) != EXT4_JBD_SUPER_V2) {
		return EXT4_UNSUPPORTED;
	}
	journal->features = ext4_be32(&super->feature_incompat);
	if ((journal->features & ~EXT4_JBD_SUPPORTED) || ext4_be32(&super->feature_compat) != 0 ||
	    ext4_be32(&super->feature_ro_compat) != 0 || ext4_be32(&super->users) != 1 ||
	    ext4_be32(&super->dynamic_super) != 0 ||
	    ((journal->features & EXT4_JBD_CSUM_V2) && (journal->features & EXT4_JBD_CSUM_V3))) {
		return EXT4_UNSUPPORTED;
	}
	journal->checksum = (journal->features & (EXT4_JBD_CSUM_V2 | EXT4_JBD_CSUM_V3)) != 0;
	if (journal->checksum) {
		if (super->checksum_type != EXT4_JBD_CRC32C) {
			return EXT4_UNSUPPORTED;
		}
		expected = ext4_be32(&super->checksum);
		ext4_encode_be32(&super->checksum, 0);
		checksum = ext4_crc32c(UINT32_MAX, super, sizeof(*super));
		ext4_encode_be32(&super->checksum, expected);
		if (checksum != expected) {
			return EXT4_CORRUPT;
		}
	}
	length = ext4_be32(&super->max_length);
	journal->first = ext4_be32(&super->first);
	journal->start = ext4_be32(&super->start);
	journal->sequence = ext4_be32(&super->sequence);
	if (ext4_be32(&super->block_size) != journal->fs->info.block_size ||
	    length > journal->blocks || length < 4 || journal->first == 0 ||
	    journal->first >= length - 2 ||
	    (journal->start != 0 &&
		(journal->start < journal->first || journal->start >= length)) ||
	    !ext4_equal(super->uuid, journal->fs->info.uuid, EXT4_UUID_SIZE)) {
		return EXT4_CORRUPT;
	}
	if (ext4_be32(&super->error) != 0) {
		return EXT4_RECOVERY_REQUIRED;
	}
	journal->blocks = length;
	journal->checksum_seed = ext4_crc32c(UINT32_MAX, super->uuid, sizeof(super->uuid));
	return EXT4_OK;
}

enum ext4_result
ext4_journal_load(
    struct ext4_fs *fs, const struct ext4_write_environment *writer, struct ext4_journal **result)
{
	struct ext4_super_disk super;
	struct ext4_inode inode;
	struct ext4_journal *journal;
	uint64_t blocks;
	enum ext4_result error;

	if (result == NULL) {
		return EXT4_INVALID_ARGUMENT;
	}
	*result = NULL;
	if (fs == NULL || writer == NULL || writer->write == NULL || writer->flush == NULL ||
	    fs->writer_attached) {
		return EXT4_INVALID_ARGUMENT;
	}
	if (!(fs->info.feature_compat & EXT4_FEATURE_COMPAT_HAS_JOURNAL) ||
	    (fs->info.feature_compat & ~EXT4_WRITABLE_COMPAT) ||
	    (fs->info.feature_ro_compat & ~EXT4_WRITABLE_RO_COMPAT)) {
		return EXT4_UNSUPPORTED;
	}
	error = ext4_device_read(fs, EXT4_SUPER_OFFSET, &super, sizeof(super));
	if (error != EXT4_OK) {
		return error;
	}
	if (ext4_le32(&super.journal_device) != 0 || ext4_le32(&super.journal_inode) == 0) {
		return EXT4_UNSUPPORTED;
	}
	error = ext4_get_inode(fs, ext4_le32(&super.journal_inode), &inode);
	if (error != EXT4_OK) {
		return error;
	}
	blocks = inode.size / fs->info.block_size;
	if ((inode.mode & EXT4_MODE_TYPE) != EXT4_MODE_REGULAR || inode.links != 1 ||
	    inode.size % fs->info.block_size != 0 || blocks < 4 || blocks > UINT32_MAX) {
		return EXT4_CORRUPT;
	}
	if (blocks > EXT4_JOURNAL_MAX_BLOCKS) {
		return EXT4_UNSUPPORTED;
	}
	journal = fs->environment.allocate(fs->environment.context, sizeof(*journal));
	if (journal == NULL) {
		return EXT4_NO_MEMORY;
	}
	ext4_zero(journal, sizeof(*journal));
	journal->fs = fs;
	journal->writer = *writer;
	journal->blocks = (uint32_t)blocks;
	fs->writer_attached = true;
	journal->runs = fs->environment.allocate(
	    fs->environment.context, EXT4_JOURNAL_MAX_RUNS * sizeof(*journal->runs));
	journal->super_buffer =
	    fs->environment.allocate(fs->environment.context, fs->info.block_size);
	journal->work = fs->environment.allocate(fs->environment.context, fs->info.block_size);
	journal->data = fs->environment.allocate(fs->environment.context, fs->info.block_size);
	if (journal->runs == NULL || journal->super_buffer == NULL || journal->work == NULL ||
	    journal->data == NULL) {
		error = EXT4_NO_MEMORY;
		goto fail;
	}
	error = ext4_journal_map(journal, &inode);
	if (error == EXT4_OK) {
		error = ext4_journal_read(journal, 0, journal->super_buffer);
	}
	if (error == EXT4_OK) {
		error = ext4_journal_validate(journal);
	}
	if (error == EXT4_OK) {
		*result = journal;
		return EXT4_OK;
	}
fail:
	ext4_journal_close(journal);
	return error;
}

enum ext4_result
ext4_journal_open(
    struct ext4_fs *fs, const struct ext4_write_environment *writer, struct ext4_journal **result)
{
	struct ext4_journal *journal;
	enum ext4_result error;

	if (result == NULL) {
		return EXT4_INVALID_ARGUMENT;
	}
	*result = NULL;
	error = ext4_journal_load(fs, writer, &journal);
	if (error != EXT4_OK) {
		return error;
	}
	if ((fs->info.feature_incompat & EXT4_FEATURE_INCOMPAT_RECOVER) || journal->start != 0) {
		ext4_journal_close(journal);
		return EXT4_RECOVERY_REQUIRED;
	}
	/* A clean journal's sequence describes its previous use. Skip that ID. */
	journal->sequence++;
	*result = journal;
	return EXT4_OK;
}

void
ext4_journal_close(struct ext4_journal *journal)
{
	struct ext4_fs *fs;

	if (journal == NULL) {
		return;
	}
	fs = journal->fs;
	if (journal->runs != NULL) {
		fs->environment.release(fs->environment.context, journal->runs,
		    EXT4_JOURNAL_MAX_RUNS * sizeof(*journal->runs));
	}
	if (journal->super_buffer != NULL) {
		fs->environment.release(
		    fs->environment.context, journal->super_buffer, fs->info.block_size);
	}
	if (journal->work != NULL) {
		fs->environment.release(
		    fs->environment.context, journal->work, fs->info.block_size);
	}
	if (journal->data != NULL) {
		fs->environment.release(
		    fs->environment.context, journal->data, fs->info.block_size);
	}
	fs->writer_attached = false;
	fs->environment.release(fs->environment.context, journal, sizeof(*journal));
}

static enum ext4_result
ext4_journal_publish(struct ext4_journal *journal, uint32_t start, uint32_t sequence)
{
	struct ext4_jbd_super *super = (struct ext4_jbd_super *)journal->super_buffer;
	uint32_t checksum;
	enum ext4_result error;

	ext4_encode_be32(&super->start, start);
	ext4_encode_be32(&super->sequence, sequence);
	ext4_encode_be32(&super->head, journal->first);
	if (journal->checksum) {
		ext4_encode_be32(&super->checksum, 0);
		checksum = ext4_crc32c(UINT32_MAX, super, sizeof(*super));
		ext4_encode_be32(&super->checksum, checksum);
	}
	error = ext4_journal_write_log(journal, 0, super);
	if (error == EXT4_OK) {
		error = ext4_journal_flush(journal);
	}
	if (error == EXT4_OK) {
		journal->start = start;
		journal->sequence = sequence;
	}
	return error;
}

enum ext4_result
ext4_journal_reset(struct ext4_journal *journal, uint32_t sequence)
{
	return ext4_journal_publish(journal, 0, sequence);
}

enum ext4_result
ext4_journal_set_recovery(struct ext4_journal *journal, bool recovery)
{
	struct ext4_fs *fs = journal->fs;
	struct ext4_super_disk *super;
	uint32_t flags;
	uint32_t checksum;
	enum ext4_result error;

	error = ext4_block_read(fs, fs->first_data_block, journal->data);
	if (error != EXT4_OK) {
		return error;
	}
	super = (struct ext4_super_disk *)(journal->data + EXT4_SUPER_OFFSET % fs->info.block_size);
	if (ext4_le16(&super->magic) != EXT4_SUPER_MAGIC ||
	    !ext4_equal(super->uuid, fs->info.uuid, EXT4_UUID_SIZE)) {
		return EXT4_CORRUPT;
	}
	if (fs->metadata_checksum &&
	    ext4_crc32c(UINT32_MAX, super, offsetof(struct ext4_super_disk, checksum)) !=
		ext4_le32(&super->checksum)) {
		return EXT4_CORRUPT;
	}
	if (!recovery &&
	    (!(ext4_le16(&super->state) & EXT4_VALID_FS) ||
		(ext4_le16(&super->state) & EXT4_ERROR_FS) ||
		ext4_le32(&super->last_orphan) != 0)) {
		return EXT4_RECOVERY_REQUIRED;
	}
	flags = ext4_le32(&super->feature_incompat);
	if (recovery) {
		flags |= EXT4_FEATURE_INCOMPAT_RECOVER;
	} else {
		flags &= ~EXT4_FEATURE_INCOMPAT_RECOVER;
	}
	if (flags == ext4_le32(&super->feature_incompat)) {
		return EXT4_OK;
	}
	ext4_encode32(&super->feature_incompat, flags);
	if (fs->metadata_checksum) {
		checksum =
		    ext4_crc32c(UINT32_MAX, super, offsetof(struct ext4_super_disk, checksum));
		ext4_encode32(&super->checksum, checksum);
	}
	error = ext4_journal_write_home(journal, fs->first_data_block, journal->data);
	if (error == EXT4_OK) {
		error = ext4_journal_flush(journal);
	}
	if (error == EXT4_OK) {
		fs->info.feature_incompat = flags;
	}
	return error;
}

enum ext4_result
ext4_journal_finish(struct ext4_journal *journal)
{
	if (journal == NULL || journal->transaction_active) {
		return EXT4_INVALID_ARGUMENT;
	}
	if (journal->aborted) {
		return EXT4_IO;
	}
	if (journal->start != 0) {
		return EXT4_RECOVERY_REQUIRED;
	}
	return ext4_journal_set_recovery(journal, false);
}

enum ext4_result
ext4_transaction_begin(
    struct ext4_journal *journal, uint32_t credits, struct ext4_transaction **result)
{
	struct ext4_transaction *transaction;
	size_t size;

	if (result == NULL) {
		return EXT4_INVALID_ARGUMENT;
	}
	*result = NULL;
	if (journal == NULL || journal->transaction_active || credits == 0 ||
	    credits > EXT4_TRANSACTION_MAX_BLOCKS) {
		return EXT4_INVALID_ARGUMENT;
	}
	if (journal->aborted || journal->start != 0) {
		return EXT4_RECOVERY_REQUIRED;
	}
	/* Reserve conservatively: at worst one descriptor for every data block. */
	if (credits * 2U + 1U >= journal->blocks - journal->first) {
		return EXT4_RANGE;
	}
	size = sizeof(*transaction) + credits * sizeof(*transaction->entries);
	transaction = journal->fs->environment.allocate(journal->fs->environment.context, size);
	if (transaction == NULL) {
		return EXT4_NO_MEMORY;
	}
	ext4_zero(transaction, size);
	transaction->journal = journal;
	transaction->credits = credits;
	journal->transaction_active = true;
	*result = transaction;
	return EXT4_OK;
}

enum ext4_result
ext4_transaction_buffer(struct ext4_transaction *transaction, uint64_t block, void **result)
{
	struct ext4_journal *journal;
	struct ext4_fs *fs;
	void *buffer;
	uint32_t index;
	enum ext4_result error;

	if (result == NULL) {
		return EXT4_INVALID_ARGUMENT;
	}
	*result = NULL;
	if (transaction == NULL) {
		return EXT4_INVALID_ARGUMENT;
	}
	journal = transaction->journal;
	fs = journal->fs;
	if (!ext4_journal_target(journal, block) || block == fs->first_data_block ||
	    (!(journal->features & EXT4_JBD_64BIT) && block > UINT32_MAX)) {
		return EXT4_INVALID_ARGUMENT;
	}
	for (index = 0; index < transaction->count; index++) {
		if (transaction->entries[index].block == block) {
			*result = transaction->entries[index].buffer;
			return EXT4_OK;
		}
	}
	if (transaction->count == transaction->credits) {
		return EXT4_RANGE;
	}
	buffer = fs->environment.allocate(fs->environment.context, fs->info.block_size);
	if (buffer == NULL) {
		return EXT4_NO_MEMORY;
	}
	error = ext4_block_read(fs, block, buffer);
	if (error != EXT4_OK) {
		fs->environment.release(fs->environment.context, buffer, fs->info.block_size);
		return error;
	}
	transaction->entries[transaction->count].block = block;
	transaction->entries[transaction->count++].buffer = buffer;
	*result = buffer;
	return EXT4_OK;
}

void
ext4_transaction_cancel(struct ext4_transaction *transaction)
{
	struct ext4_fs *fs;
	uint32_t index;
	size_t size;

	if (transaction == NULL) {
		return;
	}
	fs = transaction->journal->fs;
	for (index = 0; index < transaction->count; index++) {
		fs->environment.release(fs->environment.context, transaction->entries[index].buffer,
		    fs->info.block_size);
	}
	transaction->journal->transaction_active = false;
	size = sizeof(*transaction) + transaction->credits * sizeof(*transaction->entries);
	fs->environment.release(fs->environment.context, transaction, size);
}

static void
ext4_journal_header(void *buffer, uint32_t type, uint32_t sequence)
{
	struct ext4_jbd_header *header = buffer;

	ext4_encode_be32(&header->magic, EXT4_JBD_MAGIC);
	ext4_encode_be32(&header->type, type);
	ext4_encode_be32(&header->sequence, sequence);
}

static void
ext4_journal_tag(struct ext4_journal *journal, void *buffer, uint64_t block, uint32_t flags)
{
	struct ext4_jbd_tag3 *tag3 = buffer;
	struct ext4_jbd_tag *tag = buffer;
	struct ext4_be32 *high;
	uint32_t checksum = 0;

	if (journal->checksum) {
		checksum = ext4_journal_data_checksum(journal, journal->sequence, journal->data);
	}
	if (journal->features & EXT4_JBD_CSUM_V3) {
		ext4_encode_be32(&tag3->block_lo, (uint32_t)block);
		ext4_encode_be32(&tag3->block_hi, (uint32_t)(block >> 32));
		ext4_encode_be32(&tag3->flags, flags);
		ext4_encode_be32(&tag3->checksum, checksum);
	} else {
		ext4_encode_be32(&tag->block_lo, (uint32_t)block);
		ext4_encode_be16(&tag->checksum, (uint16_t)checksum);
		ext4_encode_be16(&tag->flags, (uint16_t)flags);
		if (journal->features & EXT4_JBD_64BIT) {
			high = (struct ext4_be32 *)((uint8_t *)buffer + sizeof(*tag));
			ext4_encode_be32(high, (uint32_t)(block >> 32));
		}
	}
}

static enum ext4_result
ext4_transaction_log(struct ext4_transaction *transaction, uint32_t *commit_block)
{
	struct ext4_journal *journal = transaction->journal;
	struct ext4_jbd_super *super = (struct ext4_jbd_super *)journal->super_buffer;
	uint32_t block_size = journal->fs->info.block_size;
	uint32_t cursor = journal->first;
	uint32_t descriptor;
	uint32_t index = 0;
	uint32_t count;
	uint32_t capacity;
	uint32_t flags;
	size_t offset;
	size_t tag_size = ext4_journal_tag_size(journal);
	size_t end = block_size - (journal->checksum ? sizeof(struct ext4_be32) : 0);
	enum ext4_result error;

	capacity = (uint32_t)((end - sizeof(struct ext4_jbd_header) - EXT4_UUID_SIZE) / tag_size);
	while (index < transaction->count) {
		descriptor = cursor;
		cursor = ext4_journal_next(journal, cursor);
		ext4_zero(journal->work, block_size);
		ext4_journal_header(journal->work, EXT4_JBD_DESCRIPTOR, journal->sequence);
		offset = sizeof(struct ext4_jbd_header);
		count = 0;
		while (index < transaction->count && count < capacity) {
			ext4_copy(journal->data, transaction->entries[index].buffer, block_size);
			flags = count == 0 ? 0 : EXT4_JBD_SAME_UUID;
			if (ext4_be32((struct ext4_be32 *)journal->data) == EXT4_JBD_MAGIC) {
				flags |= EXT4_JBD_ESCAPE;
				ext4_zero(journal->data, sizeof(struct ext4_be32));
			}
			if (index + 1 == transaction->count || count + 1 == capacity) {
				flags |= EXT4_JBD_LAST_TAG;
			}
			ext4_journal_tag(journal, journal->work + offset,
			    transaction->entries[index].block, flags);
			offset += tag_size;
			if (count == 0) {
				ext4_copy(journal->work + offset, super->uuid, EXT4_UUID_SIZE);
				offset += EXT4_UUID_SIZE;
			}
			error = ext4_journal_write_log(journal, cursor, journal->data);
			if (error != EXT4_OK) {
				return error;
			}
			cursor = ext4_journal_next(journal, cursor);
			count++;
			index++;
		}
		ext4_journal_checksum_set(
		    journal, journal->work, block_size - sizeof(struct ext4_be32));
		error = ext4_journal_write_log(journal, descriptor, journal->work);
		if (error != EXT4_OK) {
			return error;
		}
	}
	*commit_block = cursor;
	return EXT4_OK;
}

enum ext4_result
ext4_transaction_commit(struct ext4_transaction *transaction)
{
	struct ext4_journal *journal;
	uint32_t commit_block = 0;
	uint32_t index;
	enum ext4_result error;

	if (transaction == NULL) {
		return EXT4_INVALID_ARGUMENT;
	}
	journal = transaction->journal;
	if (transaction->count == 0) {
		ext4_transaction_cancel(transaction);
		return EXT4_OK;
	}
	error = ext4_journal_set_recovery(journal, true);
	if (error == EXT4_OK) {
		error = ext4_journal_publish(journal, journal->first, journal->sequence);
	}
	if (error == EXT4_OK) {
		error = ext4_transaction_log(transaction, &commit_block);
	}
	if (error == EXT4_OK &&
	    (commit_block < journal->first || commit_block >= journal->blocks)) {
		error = EXT4_CORRUPT;
	}
	/* This barrier includes ordered file data submitted by the owner, all log
	 * records, and the recovery pointer. No commit can precede their persistence. */
	if (error == EXT4_OK) {
		error = ext4_journal_flush(journal);
	}
	if (error == EXT4_OK) {
		ext4_zero(journal->work, journal->fs->info.block_size);
		ext4_journal_header(journal->work, EXT4_JBD_COMMIT, journal->sequence);
		ext4_journal_checksum_set(
		    journal, journal->work, offsetof(struct ext4_jbd_commit, checksum));
		error = ext4_journal_write_log(journal, commit_block, journal->work);
	}
	if (error == EXT4_OK) {
		error = ext4_journal_flush(journal);
	}
	/* The durable commit owns recovery before the first home block changes. */
	for (index = 0; error == EXT4_OK && index < transaction->count; index++) {
		error = ext4_journal_write_home(
		    journal, transaction->entries[index].block, transaction->entries[index].buffer);
	}
	if (error == EXT4_OK) {
		error = ext4_journal_flush(journal);
	}
	if (error == EXT4_OK) {
		error = ext4_journal_reset(journal, journal->sequence + 1);
	}
	if (error != EXT4_OK) {
		journal->aborted = true;
	}
	ext4_transaction_cancel(transaction);
	return error;
}

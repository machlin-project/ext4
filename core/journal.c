/* SPDX-License-Identifier: BSD-3-Clause */
#include "transaction.h"
#include "quota.h"

#define EXT4_JOURNAL_FREED_RANGES 4096U

/* Wire bytes borrow the immutable snapshot except when JBD2 escaping requires
 * the journal's scratch block. Consume this view before encoding another one. */
struct ext4_journal_payload {
	const void *bytes;
	uint32_t flags;
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
	uint32_t low = 0;
	uint32_t high = journal->run_count;
	uint32_t middle;

	if (journal->external.read != NULL) {
		*physical = block;
		return EXT4_OK;
	}
	/* Journal admission builds complete, disjoint runs in logical order. */
	while (low < high) {
		middle = low + (high - low) / 2U;
		run = &journal->runs[middle];
		if (block < run->logical) {
			high = middle;
		} else if (block - run->logical >= run->length) {
			low = middle + 1U;
		} else {
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
	for (index = 0; index < journal->mapping_count; index++) {
		if (block == journal->mapping_blocks[index]) {
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
		if (journal->external.read != NULL) {
			error = journal->external.read(journal->external.context,
			    physical * journal->fs->info.block_size, buffer,
			    journal->fs->info.block_size);
		} else {
			error = ext4_block_read(journal->fs, physical, buffer);
		}
	}
	return error;
}

static enum ext4_result
ext4_journal_write_range(
    struct ext4_journal *journal, uint64_t physical, const void *buffer, uint32_t blocks)
{
	enum ext4_result error;

	if (journal->aborted) {
		return EXT4_IO;
	}
	if (physical < journal->fs->first_data_block || physical >= journal->fs->info.blocks ||
	    blocks == 0 || blocks > journal->fs->info.blocks - physical) {
		return EXT4_CORRUPT;
	}
	error =
	    journal->writer.write(journal->writer.context, physical * journal->fs->info.block_size,
		buffer, (size_t)blocks * journal->fs->info.block_size);
	if (error != EXT4_OK) {
		journal->aborted = true;
	}
	return error;
}

static enum ext4_result
ext4_journal_write(struct ext4_journal *journal, uint64_t physical, const void *buffer)
{
	return ext4_journal_write_range(journal, physical, buffer, 1);
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

	if (journal->aborted) {
		return EXT4_IO;
	}
	if (block >= journal->blocks || block < journal->super_block) {
		return EXT4_CORRUPT;
	}
	error = ext4_journal_physical(journal, block, &physical);
	if (error == EXT4_OK) {
		if (journal->external.read != NULL) {
			error = journal->external.write(journal->external.context,
			    physical * journal->fs->info.block_size, buffer,
			    journal->fs->info.block_size);
			if (error != EXT4_OK) {
				journal->aborted = true;
			}
		} else {
			error = ext4_journal_write(journal, physical, buffer);
		}
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

static enum ext4_result
ext4_journal_flush_log(struct ext4_journal *journal)
{
	enum ext4_result error;

	if (journal->external.read == NULL) {
		return ext4_journal_flush(journal);
	}
	if (journal->aborted) {
		return EXT4_IO;
	}
	error = journal->external.flush(journal->external.context);
	if (error != EXT4_OK) {
		journal->aborted = true;
	}
	return error;
}

uint32_t
ext4_journal_next(const struct ext4_journal *journal, uint32_t block)
{
	return block + 1 == journal->last ? journal->first : block + 1;
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
	struct ext4_block_path path;
	struct ext4_block_path previous;
	uint64_t physical;
	uint64_t blocks;
	uint32_t logical;
	uint32_t length;
	uint32_t left;
	uint32_t right;
	uint32_t index;
	uint16_t level;
	enum ext4_result error;

	ext4_zero(&previous, sizeof(previous));
	for (logical = 0; logical < journal->blocks; logical += length) {
		error =
		    ext4_map_blocks_path(journal->fs, inode, logical, &physical, &blocks, &path);
		if (error != EXT4_OK) {
			return error;
		}
		/* A journal cannot be sparse or overlap the primary control blocks. */
		if (physical <= EXT4_SUPER_OFFSET / journal->fs->info.block_size + 1U ||
		    blocks == 0) {
			return EXT4_CORRUPT;
		}
		length = blocks < journal->blocks - logical ? (uint32_t)blocks
							    : journal->blocks - logical;
		if (path.count != 0 && journal->mapping_blocks == NULL) {
			journal->mapping_blocks =
			    journal->fs->environment.allocate(journal->fs->environment.context,
				EXT4_JOURNAL_MAX_MAPPING_BLOCKS * sizeof(*journal->mapping_blocks));
			if (journal->mapping_blocks == NULL) {
				return EXT4_NO_MEMORY;
			}
		}
		for (level = 0; level < path.count; level++) {
			if (level < previous.count &&
			    path.blocks[level] == previous.blocks[level]) {
				continue;
			}
			if (path.blocks[level] <=
			    EXT4_SUPER_OFFSET / journal->fs->info.block_size + 1U) {
				return EXT4_CORRUPT;
			}
			for (index = 0; index < journal->mapping_count; index++) {
				if (path.blocks[level] == journal->mapping_blocks[index]) {
					return EXT4_CORRUPT;
				}
			}
			if (journal->mapping_count == EXT4_JOURNAL_MAX_MAPPING_BLOCKS) {
				return EXT4_UNSUPPORTED;
			}
			journal->mapping_blocks[journal->mapping_count++] = path.blocks[level];
		}
		previous = path;
		if (run != NULL && physical == run->physical + run->length) {
			run->length += length;
		} else {
			if (journal->run_count == EXT4_JOURNAL_MAX_RUNS) {
				return EXT4_UNSUPPORTED;
			}
			run = &journal->runs[journal->run_count++];
			run->physical = physical;
			run->logical = logical;
			run->length = length;
		}
	}
	for (left = 0; left < journal->run_count; left++) {
		for (index = 0; index < journal->mapping_count; index++) {
			if (journal->mapping_blocks[index] >= journal->runs[left].physical &&
			    journal->mapping_blocks[index] - journal->runs[left].physical <
				journal->runs[left].length) {
				return EXT4_CORRUPT;
			}
		}
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
	uint32_t fast_blocks;

	if (ext4_be32(&super->header.magic) != EXT4_JBD_MAGIC) {
		return EXT4_CORRUPT;
	}
	if (ext4_be32(&super->header.type) != EXT4_JBD_SUPER_V2) {
		return EXT4_UNSUPPORTED;
	}
	journal->features = ext4_be32(&super->feature_incompat);
	journal->checksum_v1 = (ext4_be32(&super->feature_compat) & EXT4_JBD_COMPAT_CHECKSUM) != 0;
	if ((journal->features & ~EXT4_JBD_SUPPORTED) ||
	    (ext4_be32(&super->feature_compat) & ~EXT4_JBD_COMPAT_CHECKSUM) ||
	    ext4_be32(&super->feature_ro_compat) != 0 || ext4_be32(&super->users) != 1 ||
	    ext4_be32(&super->dynamic_super) != 0 ||
	    ((journal->features & EXT4_JBD_CSUM_V2) && (journal->features & EXT4_JBD_CSUM_V3))) {
		return EXT4_UNSUPPORTED;
	}
	journal->checksum = (journal->features & (EXT4_JBD_CSUM_V2 | EXT4_JBD_CSUM_V3)) != 0;
	if (journal->checksum && journal->checksum_v1) {
		return EXT4_UNSUPPORTED;
	}
	if ((journal->features & EXT4_JBD_ASYNC_COMMIT) && !journal->checksum &&
	    !journal->checksum_v1) {
		return EXT4_UNSUPPORTED;
	}
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
	journal->last = length;
	if (journal->features & EXT4_JBD_FAST_COMMIT) {
		if (!(journal->fs->info.feature_compat & EXT4_FEATURE_COMPAT_FAST_COMMIT)) {
			return EXT4_CORRUPT;
		}
		fast_blocks = ext4_be32(&super->fast_commit_blocks);
		if (fast_blocks == 0) {
			fast_blocks = EXT4_JBD_DEFAULT_FAST_BLOCKS;
		}
		if (fast_blocks < 2 || fast_blocks >= length) {
			return EXT4_CORRUPT;
		}
		journal->last -= fast_blocks;
	}
	if (ext4_be32(&super->block_size) != journal->fs->info.block_size ||
	    length > journal->blocks || length < 4 || journal->first <= journal->super_block ||
	    journal->last < 4 || journal->first >= journal->last - 2 ||
	    (journal->start != 0 &&
		(journal->start < journal->first || journal->start >= journal->last)) ||
	    !ext4_equal(super->uuid,
		journal->external.read != NULL ? journal->fs->journal_uuid : journal->fs->info.uuid,
		EXT4_UUID_SIZE) ||
	    (journal->external.read != NULL &&
		!ext4_equal(super->user_ids, journal->fs->info.uuid, EXT4_UUID_SIZE))) {
		return EXT4_CORRUPT;
	}
	if (ext4_be32(&super->error) != 0) {
		return EXT4_RECOVERY_REQUIRED;
	}
	journal->blocks = length;
	journal->checksum_seed = ext4_crc32c(UINT32_MAX, super->uuid, sizeof(super->uuid));
	return EXT4_OK;
}

static enum ext4_result
ext4_journal_external_geometry(struct ext4_journal *journal)
{
	struct ext4_super_disk *super = (struct ext4_super_disk *)journal->work;
	uint32_t incompat;
	uint32_t ro_compat;
	uint32_t logarithm;
	uint64_t blocks;
	enum ext4_result error;

	if (journal->external.size_bytes < EXT4_SUPER_OFFSET + sizeof(*super)) {
		return EXT4_CORRUPT;
	}
	error = journal->external.read(
	    journal->external.context, EXT4_SUPER_OFFSET, super, sizeof(*super));
	if (error != EXT4_OK) {
		return error;
	}
	if (ext4_le16(&super->magic) != EXT4_SUPER_MAGIC ||
	    !ext4_equal(super->uuid, journal->fs->journal_uuid, EXT4_UUID_SIZE)) {
		return EXT4_CORRUPT;
	}
	incompat = ext4_le32(&super->feature_incompat);
	ro_compat = ext4_le32(&super->feature_ro_compat);
	if (!(incompat & EXT4_FEATURE_INCOMPAT_JOURNAL_DEV) ||
	    (incompat &
		~(EXT4_FEATURE_INCOMPAT_JOURNAL_DEV | EXT4_FEATURE_INCOMPAT_64BIT |
		    EXT4_FEATURE_INCOMPAT_CSUM_SEED)) ||
	    (ro_compat & ~EXT4_FEATURE_RO_METADATA_CSUM) ||
	    ext4_le32(&super->revision) > EXT4_DYNAMIC_REV) {
		return EXT4_UNSUPPORTED;
	}
	if (ro_compat & EXT4_FEATURE_RO_METADATA_CSUM) {
		if (super->checksum_type != EXT4_CHECKSUM_CRC32C) {
			return EXT4_UNSUPPORTED;
		}
		if (ext4_crc32c(UINT32_MAX, super, offsetof(struct ext4_super_disk, checksum)) !=
		    ext4_le32(&super->checksum)) {
			return EXT4_CORRUPT;
		}
	}
	logarithm = ext4_le32(&super->log_block_size);
	if (logarithm > 6 || EXT4_MIN_BLOCK_SIZE << logarithm != journal->fs->info.block_size) {
		return EXT4_CORRUPT;
	}
	blocks = ext4_le32(&super->blocks_count_lo);
	if (incompat & EXT4_FEATURE_INCOMPAT_64BIT) {
		blocks |= (uint64_t)ext4_le32(&super->blocks_count_hi) << 32;
	}
	if (blocks < 4 || blocks > journal->external.size_bytes / journal->fs->info.block_size) {
		return EXT4_CORRUPT;
	}
	if (blocks > EXT4_JOURNAL_MAX_BLOCKS) {
		return EXT4_UNSUPPORTED;
	}
	journal->blocks = (uint32_t)blocks;
	/* External ring addresses are device-relative. Only the journal
	 * superblock moves past the ext4 device superblock; tags keep home addresses. */
	journal->super_block = EXT4_SUPER_OFFSET / journal->fs->info.block_size + 1U;
	return EXT4_OK;
}

enum ext4_result
ext4_journal_load(
    struct ext4_fs *fs, const struct ext4_write_environment *writer, struct ext4_journal **result)
{
	return ext4_journal_load_external(fs, writer, NULL, result);
}

enum ext4_result
ext4_journal_load_external(struct ext4_fs *fs, const struct ext4_write_environment *writer,
    const struct ext4_journal_environment *external, struct ext4_journal **result)
{
	struct ext4_inode inode;
	struct ext4_journal *journal;
	const uint8_t zero_uuid[EXT4_UUID_SIZE] = { 0 };
	uint64_t blocks = 0;
	enum ext4_result error;

	if (result == NULL) {
		return EXT4_INVALID_ARGUMENT;
	}
	*result = NULL;
	if (fs == NULL || writer == NULL || writer->write == NULL || writer->flush == NULL ||
	    fs->writer_attached ||
	    (external != NULL &&
		(external->read == NULL || external->write == NULL || external->flush == NULL))) {
		return EXT4_INVALID_ARGUMENT;
	}
	if (!(fs->info.feature_compat & EXT4_FEATURE_COMPAT_HAS_JOURNAL) ||
	    (fs->info.feature_compat & ~EXT4_WRITABLE_COMPAT) ||
	    (fs->info.feature_ro_compat & ~EXT4_WRITABLE_RO_COMPAT)) {
		return EXT4_UNSUPPORTED;
	}
	if (external == NULL) {
		if (fs->journal_device != 0 || fs->journal_inode == 0) {
			return EXT4_UNSUPPORTED;
		}
		error = ext4_get_inode(fs, fs->journal_inode, &inode);
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
	} else if (fs->journal_inode != 0) {
		return EXT4_INVALID_ARGUMENT;
	} else if (ext4_equal(fs->journal_uuid, zero_uuid, sizeof(zero_uuid))) {
		return EXT4_CORRUPT;
	}
	journal = fs->environment.allocate(fs->environment.context, sizeof(*journal));
	if (journal == NULL) {
		return EXT4_NO_MEMORY;
	}
	ext4_zero(journal, sizeof(*journal));
	journal->fs = fs;
	journal->writer = *writer;
	journal->blocks = (uint32_t)blocks;
	if (external != NULL) {
		journal->external = *external;
	}
	fs->writer_attached = true;
	if (external == NULL) {
		journal->runs = fs->environment.allocate(
		    fs->environment.context, EXT4_JOURNAL_MAX_RUNS * sizeof(*journal->runs));
	}
	journal->super_buffer =
	    fs->environment.allocate(fs->environment.context, fs->info.block_size);
	journal->work = fs->environment.allocate(fs->environment.context, fs->info.block_size);
	journal->data = fs->environment.allocate(fs->environment.context, fs->info.block_size);
	if ((external == NULL && journal->runs == NULL) || journal->super_buffer == NULL ||
	    journal->work == NULL || journal->data == NULL) {
		error = EXT4_NO_MEMORY;
		goto fail;
	}
	error = external == NULL ? ext4_journal_map(journal, &inode)
				 : ext4_journal_external_geometry(journal);
	if (error == EXT4_OK) {
		error = ext4_journal_read(journal, journal->super_block, journal->super_buffer);
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
	return ext4_journal_open_external(fs, writer, NULL, result);
}

enum ext4_result
ext4_journal_open_external(struct ext4_fs *fs, const struct ext4_write_environment *writer,
    const struct ext4_journal_environment *external, struct ext4_journal **result)
{
	struct ext4_journal *journal;
	enum ext4_result error;

	if (result == NULL) {
		return EXT4_INVALID_ARGUMENT;
	}
	*result = NULL;
	error = ext4_journal_load_external(fs, writer, external, &journal);
	if (error != EXT4_OK) {
		return error;
	}
	if ((fs->info.feature_incompat & EXT4_FEATURE_INCOMPAT_RECOVER) || journal->start != 0) {
		ext4_journal_close(journal);
		return EXT4_RECOVERY_REQUIRED;
	}
	/* A clean journal's sequence describes its previous use. Skip that ID. */
	journal->sequence++;
	if (fs->info.blocks > UINT32_MAX) {
		/* mke2fs can leave a large filesystem's empty journal with 32-bit
		 * tags. Select the wider writer format now without changing disk.
		 * Log publication persists it before any new descriptor is sent. */
		journal->features |= EXT4_JBD_64BIT;
	}
	*result = journal;
	return EXT4_OK;
}

enum ext4_result
ext4_journal_open_direct(
    struct ext4_fs *fs, const struct ext4_write_environment *writer, struct ext4_journal **result)
{
	struct ext4_journal *journal;

	if (result == NULL) {
		return EXT4_INVALID_ARGUMENT;
	}
	*result = NULL;
	if (fs == NULL || writer == NULL || writer->write == NULL || writer->flush == NULL ||
	    fs->writer_attached) {
		return EXT4_INVALID_ARGUMENT;
	}
	if ((fs->info.feature_compat & EXT4_FEATURE_COMPAT_HAS_JOURNAL) ||
	    (fs->info.feature_incompat & EXT4_FEATURE_INCOMPAT_RECOVER) || fs->journal_inode != 0 ||
	    fs->journal_device != 0) {
		return EXT4_INVALID_ARGUMENT;
	}
	if ((fs->info.feature_compat & ~EXT4_WRITABLE_COMPAT) ||
	    (fs->info.feature_ro_compat & ~EXT4_WRITABLE_RO_COMPAT)) {
		return EXT4_UNSUPPORTED;
	}
	journal = fs->environment.allocate(fs->environment.context, sizeof(*journal));
	if (journal == NULL) {
		return EXT4_NO_MEMORY;
	}
	ext4_zero(journal, sizeof(*journal));
	journal->fs = fs;
	journal->writer = *writer;
	journal->direct = true;
	/* File data reaches its home before the metadata that references it. */
	journal->ordered_data = true;
	fs->writer_attached = true;
	journal->work = fs->environment.allocate(fs->environment.context, fs->info.block_size);
	journal->data = fs->environment.allocate(fs->environment.context, fs->info.block_size);
	if (journal->work == NULL || journal->data == NULL) {
		ext4_journal_close(journal);
		return EXT4_NO_MEMORY;
	}
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
	if (journal->mapping_blocks != NULL) {
		fs->environment.release(fs->environment.context, journal->mapping_blocks,
		    EXT4_JOURNAL_MAX_MAPPING_BLOCKS * sizeof(*journal->mapping_blocks));
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
	/* Operations still pending in a deferred commit are lost, as on power loss.
	 * Committed ones stay in the log for recovery. */
	if (journal->compound != NULL) {
		ext4_transaction_cancel(journal->compound);
		journal->compound = NULL;
	}
	if (journal->checkpoint != NULL) {
		ext4_transaction_cancel(journal->checkpoint);
		journal->checkpoint = NULL;
	}
	if (journal->freed != NULL) {
		fs->environment.release(fs->environment.context, journal->freed,
		    EXT4_JOURNAL_FREED_RANGES * sizeof(*journal->freed));
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
	/* Persist a clean-journal address-width upgrade in the same barrier as
	 * its new start/sequence. No descriptor may precede this publication. */
	ext4_encode_be32(&super->feature_incompat, journal->features);
	if (journal->checksum) {
		ext4_encode_be32(&super->checksum, 0);
		checksum = ext4_crc32c(UINT32_MAX, super, sizeof(*super));
		ext4_encode_be32(&super->checksum, checksum);
	}
	error = ext4_journal_write_log(journal, journal->super_block, super);
	if (error == EXT4_OK) {
		error = ext4_journal_flush_log(journal);
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

/* Record whether a volume without a journal is out of use in its committed
 * superblock, which journal->data holds. */
static enum ext4_result
ext4_journal_set_valid(struct ext4_journal *journal, struct ext4_super_disk *super, bool valid)
{
	struct ext4_fs *fs = journal->fs;
	uint16_t state = ext4_le16(&super->state);
	uint16_t changed = valid ? (uint16_t)(state | EXT4_VALID_FS)
				 : (uint16_t)(state & ~(uint16_t)EXT4_VALID_FS);
	enum ext4_result error;

	if (changed == state) {
		return EXT4_OK;
	}
	ext4_encode16(&super->state, changed);
	if (fs->metadata_checksum) {
		ext4_encode32(&super->checksum,
		    ext4_crc32c(UINT32_MAX, super, offsetof(struct ext4_super_disk, checksum)));
	}
	error = ext4_journal_write_home(
	    journal, EXT4_SUPER_OFFSET / fs->info.block_size, journal->data);
	if (error == EXT4_OK) {
		error = ext4_journal_flush(journal);
	}
	return error;
}

enum ext4_result
ext4_journal_set_recovery(struct ext4_journal *journal, bool recovery)
{
	struct ext4_fs *fs = journal->fs;
	struct ext4_super_disk *super;
	uint32_t flags;
	uint32_t ro_flags;
	uint32_t checksum;
	enum ext4_result error;

	/* The recovery marker changes only the committed superblock. */
	error =
	    ext4_block_read_committed(fs, EXT4_SUPER_OFFSET / fs->info.block_size, journal->data);
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
	/* Without a journal, only this writer clears the valid state. */
	if (!recovery &&
	    ((!journal->direct && !(ext4_le16(&super->state) & EXT4_VALID_FS)) ||
		(ext4_le16(&super->state) & EXT4_ERROR_FS) || ext4_le32(&super->last_orphan) != 0 ||
		(fs->orphan_file_inode != 0 &&
		    (fs->orphan_file == NULL || fs->orphan_file->pending != 0)))) {
		return EXT4_RECOVERY_REQUIRED;
	}
	if (journal->direct) {
		return ext4_journal_set_valid(journal, super, !recovery);
	}
	flags = ext4_le32(&super->feature_incompat);
	ro_flags = ext4_le32(&super->feature_ro_compat);
	if (recovery) {
		flags |= EXT4_FEATURE_INCOMPAT_RECOVER;
		if (fs->orphan_file_inode != 0) {
			ro_flags |= EXT4_FEATURE_RO_ORPHAN_PRESENT;
		}
	} else {
		flags &= ~EXT4_FEATURE_INCOMPAT_RECOVER;
		ro_flags &= ~EXT4_FEATURE_RO_ORPHAN_PRESENT;
	}
	if (flags == ext4_le32(&super->feature_incompat) &&
	    ro_flags == ext4_le32(&super->feature_ro_compat)) {
		return EXT4_OK;
	}
	ext4_encode32(&super->feature_incompat, flags);
	ext4_encode32(&super->feature_ro_compat, ro_flags);
	if (fs->metadata_checksum) {
		checksum =
		    ext4_crc32c(UINT32_MAX, super, offsetof(struct ext4_super_disk, checksum));
		ext4_encode32(&super->checksum, checksum);
	}
	error = ext4_journal_write_home(
	    journal, EXT4_SUPER_OFFSET / fs->info.block_size, journal->data);
	if (error == EXT4_OK) {
		error = ext4_journal_flush(journal);
	}
	if (error == EXT4_OK) {
		fs->info.feature_incompat = flags;
		fs->info.feature_ro_compat = ro_flags;
	}
	return error;
}

enum ext4_result
ext4_journal_finish(struct ext4_journal *journal)
{
	if (journal == NULL || journal->transaction_active ||
	    (journal->compound != NULL && journal->compound->count != 0)) {
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

static void
ext4_transaction_prepare_super(struct ext4_transaction *transaction)
{
	struct ext4_fs *fs = transaction->journal->fs;
	struct ext4_super_disk *super;
	uint32_t index;

	for (index = 0; index < transaction->count; index++) {
		if (transaction->entries[index].block != EXT4_SUPER_OFFSET / fs->info.block_size) {
			continue;
		}
		super = (struct ext4_super_disk *)((uint8_t *)transaction->entries[index].buffer +
		    EXT4_SUPER_OFFSET % fs->info.block_size);
		if (transaction->journal->direct) {
			ext4_encode16(
			    &super->state, ext4_le16(&super->state) & ~(uint16_t)EXT4_VALID_FS);
		} else {
			ext4_encode32(&super->feature_incompat,
			    ext4_le32(&super->feature_incompat) | EXT4_FEATURE_INCOMPAT_RECOVER);
		}
		if (fs->orphan_file_inode != 0 && !transaction->journal->direct) {
			ext4_encode32(&super->feature_ro_compat,
			    ext4_le32(&super->feature_ro_compat) | EXT4_FEATURE_RO_ORPHAN_PRESENT);
		}
		if (fs->metadata_checksum) {
			ext4_encode32(&super->checksum,
			    ext4_crc32c(
				UINT32_MAX, super, offsetof(struct ext4_super_disk, checksum)));
		}
	}
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
ext4_journal_tag(
    struct ext4_journal *journal, void *buffer, uint64_t block, uint32_t flags, const void *data)
{
	struct ext4_jbd_tag3 *tag3 = buffer;
	struct ext4_jbd_tag *tag = buffer;
	struct ext4_be32 *high;
	uint32_t checksum = 0;

	if (journal->checksum) {
		checksum = ext4_journal_data_checksum(journal, journal->sequence, data);
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

static struct ext4_journal_payload
ext4_journal_encode_payload(struct ext4_journal *journal, const void *snapshot)
{
	struct ext4_journal_payload payload = { snapshot, 0 };

	if (ext4_be32(snapshot) == EXT4_JBD_MAGIC) {
		ext4_copy(journal->data, snapshot, journal->fs->info.block_size);
		ext4_zero(journal->data, sizeof(struct ext4_be32));
		payload.bytes = journal->data;
		payload.flags = EXT4_JBD_ESCAPE;
	}
	return payload;
}

/* V1 checksums follow descriptor/data order, independently of submission order.
 * Escaping never modifies a snapshot that live reads or checkpointing still own. */
static uint32_t
ext4_transaction_checksum_v1(
    const struct ext4_transaction *transaction, uint32_t first, uint32_t end, uint32_t checksum)
{
	struct ext4_journal *journal = transaction->journal;
	struct ext4_journal_payload payload;
	uint32_t block_size = journal->fs->info.block_size;
	uint32_t index;

	checksum = ext4_crc32_be(checksum, journal->work, block_size);
	for (index = first; index < end; index++) {
		payload = ext4_journal_encode_payload(journal, transaction->entries[index].buffer);
		checksum = ext4_crc32_be(checksum, payload.bytes, block_size);
	}
	return checksum;
}

/* Tags one descriptor block holds, leaving room for the first tag's UUID. */
static uint32_t
ext4_journal_descriptor_tags(const struct ext4_journal *journal)
{
	size_t end =
	    journal->fs->info.block_size - (journal->checksum ? sizeof(struct ext4_be32) : 0);

	return (uint32_t)((end - sizeof(struct ext4_jbd_header) - EXT4_UUID_SIZE) /
	    ext4_journal_tag_size(journal));
}

/* Log blocks a transaction of count snapshots occupies: descriptors, data, commit. */
static uint32_t
ext4_journal_log_blocks(const struct ext4_journal *journal, uint32_t count)
{
	uint32_t capacity = ext4_journal_descriptor_tags(journal);

	return count + (count + capacity - 1U) / capacity + 1U;
}

/* Log the transaction's blocks from the log head without wrapping around the ring. */
static enum ext4_result
ext4_transaction_log(
    struct ext4_transaction *transaction, uint32_t *commit_block, uint32_t *transaction_checksum)
{
	struct ext4_journal *journal = transaction->journal;
	struct ext4_jbd_super *super = (struct ext4_jbd_super *)journal->super_buffer;
	struct ext4_journal_payload payload;
	uint32_t block_size = journal->fs->info.block_size;
	uint32_t cursor = journal->head;
	uint32_t descriptor;
	uint32_t index = 0;
	uint32_t count;
	uint32_t capacity = ext4_journal_descriptor_tags(journal);
	uint32_t flags;
	uint32_t first_entry;
	size_t offset;
	size_t tag_size = ext4_journal_tag_size(journal);
	enum ext4_result error;

	if (journal->last - journal->head < ext4_journal_log_blocks(journal, transaction->count)) {
		return EXT4_CORRUPT;
	}
	while (index < transaction->count) {
		first_entry = index;
		descriptor = cursor;
		cursor = ext4_journal_next(journal, cursor);
		ext4_zero(journal->work, block_size);
		ext4_journal_header(journal->work, EXT4_JBD_DESCRIPTOR, journal->sequence);
		offset = sizeof(struct ext4_jbd_header);
		count = 0;
		while (index < transaction->count && count < capacity) {
			payload = ext4_journal_encode_payload(
			    journal, transaction->entries[index].buffer);
			flags = payload.flags | (count == 0 ? 0 : EXT4_JBD_SAME_UUID);
			if (index + 1 == transaction->count || count + 1 == capacity) {
				flags |= EXT4_JBD_LAST_TAG;
			}
			ext4_journal_tag(journal, journal->work + offset,
			    transaction->entries[index].block, flags, payload.bytes);
			offset += tag_size;
			if (count == 0) {
				ext4_copy(journal->work + offset, super->uuid, EXT4_UUID_SIZE);
				offset += EXT4_UUID_SIZE;
			}
			error = ext4_journal_write_log(journal, cursor, payload.bytes);
			if (error != EXT4_OK) {
				return error;
			}
			cursor = ext4_journal_next(journal, cursor);
			count++;
			index++;
		}
		ext4_journal_checksum_set(
		    journal, journal->work, block_size - sizeof(struct ext4_be32));
		if (journal->checksum_v1) {
			*transaction_checksum = ext4_transaction_checksum_v1(
			    transaction, first_entry, index, *transaction_checksum);
		}
		error = ext4_journal_write_log(journal, descriptor, journal->work);
		if (error != EXT4_OK) {
			return error;
		}
	}
	*commit_block = cursor;
	return EXT4_OK;
}

bool
ext4_commit_rejected(const struct ext4_journal *journal, enum ext4_result error)
{
	return !journal->aborted && (error == EXT4_QUOTA_EXCEEDED || error == EXT4_NO_MEMORY);
}

/* Publish the free counters from the transaction's superblock; several allocation
 * contexts, such as private value inodes and quota growth, can share one. */
static void
ext4_transaction_publish(const struct ext4_transaction *transaction)
{
	struct ext4_fs *fs = transaction->journal->fs;
	const struct ext4_super_disk *super;
	uint32_t index;

	for (index = 0; index < transaction->count; index++) {
		if (transaction->entries[index].block != EXT4_SUPER_OFFSET / fs->info.block_size) {
			continue;
		}
		super =
		    (const struct ext4_super_disk *)((const uint8_t *)transaction->entries[index]
							 .buffer +
			EXT4_SUPER_OFFSET % fs->info.block_size);
		fs->info.free_inodes = ext4_le32(&super->free_inodes);
		fs->info.free_blocks = ext4_le32(&super->free_blocks_lo);
		if (fs->info.feature_incompat & EXT4_FEATURE_INCOMPAT_64BIT) {
			fs->info.free_blocks |= (uint64_t)ext4_le32(&super->free_blocks_hi) << 32;
		}
	}
}

/* Whether ordered data may write this data block in place. A block freed since the
 * last durable commit may still belong to another file after a power cut, and a
 * block the compound holds must keep its journaled copy current. Recovery replays
 * the log's copy of a block that a committed transaction logged, so while that
 * transaction is not checkpointed, new contents of the block must be logged after
 * it instead of revoking it. */
static bool
ext4_transaction_in_place(const struct ext4_transaction *transaction, uint64_t block)
{
	const struct ext4_journal *journal = transaction->journal;
	uint32_t index;

	if (!journal->ordered_data || transaction->freed_overflow || journal->freed_overflow ||
	    ext4_ranges_overlap(journal->freed, journal->freed_count, block, 1) ||
	    (journal->compound != NULL &&
		ext4_transaction_peek(journal->compound, block) != NULL) ||
	    (journal->checkpoint != NULL &&
		ext4_transaction_peek(journal->checkpoint, block) != NULL)) {
		return false;
	}
	for (index = 0; index < transaction->freed_count; index++) {
		if (block >= transaction->freed[index].first &&
		    block - transaction->freed[index].first < transaction->freed[index].length) {
			return false;
		}
	}
	return true;
}

/* Caller bytes can survive until this call's ordered write, but never in a
 * compound or checkpoint set. Resolve all required ownership before any I/O.
 * A capacity commit can only remove restrictions: it cannot introduce a logged
 * version of a block absent from both retained sets at this boundary. */
static enum ext4_result
ext4_transaction_prepare_data(struct ext4_transaction *transaction)
{
	struct ext4_transaction_entry *entry;
	uint32_t index;
	enum ext4_result error;

	for (index = 0; index < transaction->count; index++) {
		entry = &transaction->entries[index];
		entry->data = entry->data && ext4_transaction_in_place(transaction, entry->block);
		if (entry->source_blocks != 0 && !entry->data) {
			error = ext4_transaction_own(transaction->journal->fs, entry);
			if (error != EXT4_OK) {
				return error;
			}
		}
	}
	return EXT4_OK;
}

/* Merge only adjacent selected blocks backed by the same bounded caller range.
 * Address adjacency alone does not authorize reading across owned allocations. */
static uint32_t
ext4_transaction_data_run(const struct ext4_transaction *transaction, uint32_t index)
{
	const struct ext4_transaction_entry *first = &transaction->entries[index];
	const struct ext4_transaction_entry *next;
	uint32_t block_size = transaction->journal->fs->info.block_size;
	uint32_t count = 1;

	while (count < transaction->count - index && count < first->source_blocks) {
		next = &transaction->entries[index + count];
		if (!next->data || next->source_blocks == 0 ||
		    next->block != first->block + count ||
		    next->buffer != (const uint8_t *)first->buffer + (size_t)count * block_size) {
			break;
		}
		count++;
	}
	return count;
}

/* Write selected data blocks home and remove them from the transaction, so logging
 * and checkpointing see only journaled blocks. The index is not used afterwards. */
static enum ext4_result
ext4_transaction_write_data(struct ext4_transaction *transaction)
{
	struct ext4_journal *journal = transaction->journal;
	struct ext4_fs *fs = journal->fs;
	struct ext4_transaction_entry *entry;
	uint32_t index;
	uint32_t count;
	uint32_t next;
	uint32_t kept = 0;
	enum ext4_result error = EXT4_OK;

	for (index = 0; index < transaction->count; index++) {
		entry = &transaction->entries[index];
		if (error == EXT4_OK && entry->data) {
			count = ext4_transaction_data_run(transaction, index);
			for (next = 0; next < count; next++) {
				if (!ext4_journal_target(journal, entry->block + next)) {
					error = EXT4_CORRUPT;
					break;
				}
			}
			if (error == EXT4_OK) {
				error = ext4_journal_write_range(
				    journal, entry->block, entry->buffer, count);
			}
			if (error == EXT4_OK) {
				/* A coalesced caller range owns no buffers. Owned storage
				 * always consumes a single entry. */
				if (entry->source_blocks == 0) {
					ext4_transaction_release(fs, entry);
				}
				index += count - 1U;
				continue;
			}
		}
		entry->data = false;
		transaction->entries[kept++] = *entry;
	}
	transaction->count = kept;
	return error;
}

/* Remember the blocks a merged transaction freed until the compound is durable. */
static void
ext4_journal_remember_freed(
    struct ext4_journal *journal, const struct ext4_transaction *transaction)
{
	struct ext4_fs *fs = journal->fs;
	uint32_t index;

	if (!journal->ordered_data || journal->freed_overflow) {
		return;
	}
	if (transaction->freed_overflow ||
	    journal->freed_count + transaction->freed_count > EXT4_JOURNAL_FREED_RANGES) {
		journal->freed_overflow = true;
		return;
	}
	if (transaction->freed_count == 0) {
		return;
	}
	if (journal->freed == NULL) {
		journal->freed = fs->environment.allocate(
		    fs->environment.context, EXT4_JOURNAL_FREED_RANGES * sizeof(*journal->freed));
		if (journal->freed == NULL) {
			journal->freed_overflow = true;
			return;
		}
	}
	for (index = 0; index < transaction->freed_count; index++) {
		journal->freed[journal->freed_count++] = transaction->freed[index];
	}
	ext4_ranges_union(journal->freed, &journal->freed_count);
}

/* An empty set of snapshots the journal holds itself: the compound or the
 * checkpoint set. */
static enum ext4_result
ext4_journal_set_create(
    struct ext4_journal *journal, uint32_t capacity, struct ext4_transaction **result)
{
	struct ext4_transaction *set;

	set = ext4_transaction_allocate(journal, capacity);
	if (set == NULL) {
		return EXT4_NO_MEMORY;
	}
	set->credits = capacity;
	set->sequence = journal->sequence;
	set->held = true;
	*result = set;
	return EXT4_OK;
}

enum ext4_result
ext4_journal_checkpoint(struct ext4_journal *journal)
{
	struct ext4_transaction *checkpoint = journal->checkpoint;
	uint32_t index;
	enum ext4_result error = EXT4_OK;

	if (journal->aborted) {
		return EXT4_IO;
	}
	if (checkpoint == NULL) {
		return EXT4_OK;
	}
	/* The log keeps recovery authority until every home block is durable. */
	for (index = 0; error == EXT4_OK && index < checkpoint->count; index++) {
		error = ext4_journal_write_home(
		    journal, checkpoint->entries[index].block, checkpoint->entries[index].buffer);
	}
	if (error == EXT4_OK) {
		error = ext4_journal_flush(journal);
	}
	if (error == EXT4_OK) {
		error = ext4_journal_reset(journal, journal->sequence);
	}
	if (error != EXT4_OK) {
		journal->aborted = true;
		return error;
	}
	journal->checkpoint = NULL;
	ext4_transaction_cancel(checkpoint);
	return EXT4_OK;
}

/* Whether a transaction joins the committed ones in the log and the checkpoint set
 * without a checkpoint first. */
static bool
ext4_journal_fits(const struct ext4_journal *journal, const struct ext4_transaction *transaction)
{
	const struct ext4_transaction *checkpoint = journal->checkpoint;
	uint32_t added = 0;
	uint32_t index;

	if (journal->last - journal->head < ext4_journal_log_blocks(journal, transaction->count)) {
		return false;
	}
	for (index = 0; index < transaction->count; index++) {
		if (ext4_transaction_peek(checkpoint, transaction->entries[index].block) == NULL) {
			added++;
		}
	}
	return added <= checkpoint->capacity - checkpoint->count;
}

/* Without a journal, a commit first marks the volume in use, then writes file data
 * home and flushes, so that no written metadata references data that is not
 * durable, then writes the other blocks and flushes. */
static enum ext4_result
ext4_transaction_direct(struct ext4_transaction *transaction)
{
	struct ext4_journal *journal = transaction->journal;
	uint32_t count = transaction->count;
	uint32_t index;
	enum ext4_result error;

	error = ext4_journal_set_recovery(journal, true);
	if (error == EXT4_OK) {
		error = ext4_transaction_write_data(transaction);
	}
	if (error == EXT4_OK && transaction->count != count) {
		error = ext4_journal_flush(journal);
	}
	ext4_transaction_prepare_super(transaction);
	for (index = 0; error == EXT4_OK && index < transaction->count; index++) {
		error = ext4_journal_write_home(
		    journal, transaction->entries[index].block, transaction->entries[index].buffer);
	}
	if (error == EXT4_OK) {
		error = ext4_journal_flush(journal);
	}
	if (error != EXT4_OK) {
		journal->aborted = true;
	} else {
		ext4_transaction_publish(transaction);
		journal->freed_count = 0;
		journal->freed_overflow = false;
	}
	ext4_transaction_cancel(transaction);
	return error;
}

/* Log and commit one transaction, then release it. Without lazy checkpointing, and
 * for recovery conversions, its blocks are written home and the log emptied at once;
 * otherwise they stay in the log and the checkpoint set. */
static enum ext4_result
ext4_transaction_durable(struct ext4_transaction *transaction)
{
	struct ext4_journal *journal = transaction->journal;
	struct ext4_jbd_commit *commit;
	uint32_t commit_block = 0;
	uint32_t transaction_checksum = UINT32_MAX;
	uint32_t index;
	bool lazy;
	enum ext4_result error = EXT4_OK;

	if (journal->direct) {
		return ext4_transaction_direct(transaction);
	}
	/* Ordered data reaches its home before the barrier that precedes the commit. */
	if (!transaction->held) {
		error = ext4_transaction_write_data(transaction);
	}
	if (error == EXT4_OK && transaction->count == 0) {
		error = ext4_journal_flush(journal);
		if (error != EXT4_OK) {
			journal->aborted = true;
		}
		ext4_transaction_cancel(transaction);
		return error;
	}
	ext4_transaction_prepare_super(transaction);
	lazy = journal->checkpoint_blocks != 0 && !transaction->recovery &&
	    transaction->count <= journal->checkpoint_blocks;
	/* A recovery conversion, a transaction larger than the checkpoint set, a full
	 * log or a full set first writes the committed transactions home. */
	if (error == EXT4_OK && journal->checkpoint != NULL &&
	    (!lazy || !ext4_journal_fits(journal, transaction))) {
		error = ext4_journal_checkpoint(journal);
	}
	if (error == EXT4_OK && lazy && journal->checkpoint == NULL) {
		error = ext4_journal_set_create(
		    journal, journal->checkpoint_blocks, &journal->checkpoint);
		/* Without memory for the set, checkpoint this commit at once. */
		if (error == EXT4_NO_MEMORY) {
			lazy = false;
			error = EXT4_OK;
		}
	}
	/* A new log starts at the ring's first block. Ordinary transactions take the
	 * next sequence; a recovery conversion keeps the one it was assigned. */
	if (error == EXT4_OK && (!lazy || journal->start == 0)) {
		error = ext4_journal_set_recovery(journal, true);
		if (error == EXT4_OK) {
			error = ext4_journal_publish(journal, journal->first,
			    transaction->recovery ? transaction->sequence : journal->sequence);
		}
		journal->head = journal->first;
	}
	if (error == EXT4_OK) {
		error = ext4_transaction_log(transaction, &commit_block, &transaction_checksum);
	}
	if (error == EXT4_OK && (commit_block < journal->head || commit_block >= journal->last)) {
		error = EXT4_CORRUPT;
	}
	/* This barrier includes ordered file data submitted by the owner, all log
	 * records, and the recovery pointer. No commit can precede their persistence. */
	if (error == EXT4_OK && journal->external.read != NULL) {
		error = ext4_journal_flush(journal);
	}
	if (error == EXT4_OK) {
		error = ext4_journal_flush_log(journal);
	}
	if (error == EXT4_OK) {
		ext4_zero(journal->work, journal->fs->info.block_size);
		ext4_journal_header(journal->work, EXT4_JBD_COMMIT, journal->sequence);
		ext4_journal_checksum_set(
		    journal, journal->work, offsetof(struct ext4_jbd_commit, checksum));
		if (journal->checksum_v1) {
			commit = (struct ext4_jbd_commit *)journal->work;
			commit->checksum_type = EXT4_JBD_CRC32;
			commit->checksum_size = sizeof(commit->checksum[0]);
			ext4_encode_be32(&commit->checksum[0], transaction_checksum);
		}
		error = ext4_journal_write_log(journal, commit_block, journal->work);
	}
	if (error == EXT4_OK) {
		error = ext4_journal_flush_log(journal);
	}
	if (error == EXT4_OK && lazy) {
		/* Recovery replays this transaction until the next checkpoint. */
		journal->head = commit_block + 1U;
		journal->sequence++;
		ext4_transaction_publish(transaction);
		ext4_transaction_take(journal->checkpoint, transaction);
	}
	/* The durable commit owns recovery before the first home block changes. */
	for (index = 0; error == EXT4_OK && index < transaction->count; index++) {
		error = ext4_journal_write_home(
		    journal, transaction->entries[index].block, transaction->entries[index].buffer);
	}
	if (error == EXT4_OK && !lazy) {
		error = ext4_journal_flush(journal);
		if (error == EXT4_OK) {
			error = ext4_journal_reset(journal, journal->sequence + 1);
		}
	}
	if (error != EXT4_OK) {
		journal->aborted = true;
	} else {
		/* Publish the free counters from the committed superblock only after
		 * its commit. Every earlier free is now durable. */
		ext4_transaction_publish(transaction);
		journal->freed_count = 0;
		journal->freed_overflow = false;
	}
	ext4_transaction_cancel(transaction);
	return error;
}

static enum ext4_result
ext4_compound_create(struct ext4_journal *journal)
{
	return ext4_journal_set_create(journal, journal->compound_blocks, &journal->compound);
}

static enum ext4_result
ext4_compound_commit(struct ext4_journal *journal)
{
	struct ext4_transaction *compound = journal->compound;

	if (journal->aborted) {
		return EXT4_IO;
	}
	journal->compound = NULL;
	if (compound == NULL) {
		return EXT4_OK;
	}
	if (compound->count == 0) {
		ext4_transaction_cancel(compound);
		return EXT4_OK;
	}
	return ext4_transaction_durable(compound);
}

/* Count retained blocks using the prepared data/home decision. */
static uint32_t
ext4_transaction_additions(struct ext4_transaction *transaction, uint32_t *journaled)
{
	const struct ext4_transaction *compound = transaction->journal->compound;
	struct ext4_transaction_entry *entry;
	uint32_t index;
	uint32_t added = 0;

	*journaled = 0;
	for (index = 0; index < transaction->count; index++) {
		entry = &transaction->entries[index];
		if (entry->data) {
			continue;
		}
		(*journaled)++;
		if (compound == NULL || ext4_transaction_peek(compound, entry->block) == NULL) {
			added++;
		}
	}
	return added;
}

/* After any capacity-driven commit, reserve the compound's index before writing
 * ordered data. An index allocation failure precedes those writes; uncertain writes
 * abort before transferring any snapshots. The final ownership transfer cannot
 * fail and needs no allocation or copy of the operation's journaled blocks. */
static enum ext4_result
ext4_transaction_merge(struct ext4_transaction *transaction)
{
	struct ext4_journal *journal = transaction->journal;
	uint32_t journaled;
	uint32_t added;
	enum ext4_result error = EXT4_OK;

	added = ext4_transaction_additions(transaction, &journaled);
	if (journal->compound != NULL &&
	    journal->compound->count + added > journal->compound->capacity) {
		error = ext4_compound_commit(journal);
	}
	if (error != EXT4_OK) {
		ext4_transaction_cancel(transaction);
		return error;
	}
	/* An operation larger than the compound commits on its own, in order. */
	if (journaled > journal->compound_blocks) {
		return ext4_transaction_durable(transaction);
	}
	if (journal->compound == NULL) {
		error = ext4_compound_create(journal);
	}
	if (error != EXT4_OK) {
		ext4_transaction_cancel(transaction);
		return error;
	}
	/* Ordered data goes home now; the compound's commit barrier orders it first. */
	error = ext4_transaction_write_data(transaction);
	if (error != EXT4_OK) {
		journal->aborted = true;
		ext4_transaction_cancel(transaction);
		return error;
	}
	ext4_transaction_prepare_super(transaction);
	ext4_journal_remember_freed(journal, transaction);
	ext4_transaction_publish(transaction);
	ext4_transaction_take(journal->compound, transaction);
	ext4_transaction_cancel(transaction);
	return EXT4_OK;
}

enum ext4_result
ext4_transaction_commit(struct ext4_transaction *transaction)
{
	struct ext4_journal *journal;
	enum ext4_result error;

	if (transaction == NULL) {
		return EXT4_INVALID_ARGUMENT;
	}
	journal = transaction->journal;
	if (transaction->count == 0) {
		ext4_transaction_cancel(transaction);
		return EXT4_OK;
	}
	/* Quota updates and borrowed-buffer ownership still affect only private
	 * snapshots. A preparation refusal leaves held readers' views unchanged. */
	transaction->quota_phase = true;
	error = ext4_quota_commit(transaction);
	if (error == EXT4_OK) {
		error = ext4_transaction_prepare_data(transaction);
	}
	if (error != EXT4_OK) {
		ext4_transaction_cancel(transaction);
		return error;
	}
	/* Even a failed commit can have written ordered data or home blocks. Readers
	 * must refresh before reusing a snapshot; deferred publication is covered too. */
	ext4_read_cache_invalidate(journal->fs);
	if (journal->compound_blocks != 0 && !transaction->recovery) {
		return ext4_transaction_merge(transaction);
	}
	return ext4_transaction_durable(transaction);
}

enum ext4_result
ext4_journal_commit(struct ext4_journal *journal)
{
	if (journal == NULL || journal->transaction_active) {
		return EXT4_INVALID_ARGUMENT;
	}
	return ext4_compound_commit(journal);
}

/* Shorten prefix to end before the first block of the range that set holds. */
static uint64_t
ext4_journal_set_prefix(const struct ext4_transaction *set, uint64_t block, uint64_t prefix)
{
	uint64_t offset;
	uint32_t index;

	if (set == NULL || set->count == 0) {
		return prefix;
	}
	if (prefix <= set->count) {
		for (offset = 0; offset < prefix; offset++) {
			if (ext4_transaction_peek(set, block + offset) != NULL) {
				return offset;
			}
		}
		return prefix;
	}
	for (index = 0; index < set->count; index++) {
		if (set->entries[index].block >= block &&
		    set->entries[index].block - block < prefix) {
			prefix = set->entries[index].block - block;
		}
	}
	return prefix;
}

uint64_t
ext4_journal_home_prefix(const struct ext4_journal *journal, uint64_t block, uint64_t count)
{
	uint64_t prefix = ext4_journal_set_prefix(journal->checkpoint, block, count);

	return ext4_journal_set_prefix(journal->compound, block, prefix);
}

uint32_t
ext4_journal_compound_limit(const struct ext4_journal *journal)
{
	return ext4_journal_recovery_credits(journal);
}

uint32_t
ext4_journal_checkpoint_limit(const struct ext4_journal *journal)
{
	uint32_t ring = journal->last - journal->first;
	uint32_t memory = EXT4_TRANSACTION_SNAPSHOT_BYTES / journal->fs->info.block_size;

	return ring < memory ? ring : memory;
}

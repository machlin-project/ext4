/* SPDX-License-Identifier: BSD-3-Clause */
#include "fast_commit.h"

struct ext4_fc_scan {
	struct ext4_fc_record *records;
	uint32_t capacity;
	uint32_t count;
	uint32_t committed;
	uint32_t commits;
	bool discarded_tail;
};

static bool
ext4_fc_length_valid(const struct ext4_fs *fs, uint16_t type, uint16_t length)
{
	switch (type) {
	case EXT4_FC_ADD_RANGE:
		return length == sizeof(struct ext4_fc_add_disk);
	case EXT4_FC_DEL_RANGE:
		return length == sizeof(struct ext4_fc_delete_disk);
	case EXT4_FC_CREATE:
	case EXT4_FC_LINK:
	case EXT4_FC_UNLINK:
		return length > sizeof(struct ext4_fc_name_disk) &&
		    length <= sizeof(struct ext4_fc_name_disk) + EXT4_NAME_MAX;
	case EXT4_FC_INODE:
		return length >= sizeof(struct ext4_fc_inode_disk) + EXT4_INODE_BASE_SIZE &&
		    length <= sizeof(struct ext4_fc_inode_disk) + fs->inode_size;
	case EXT4_FC_PAD:
		return true;
	case EXT4_FC_TAIL:
		return length >= sizeof(struct ext4_fc_tail_disk);
	case EXT4_FC_HEAD:
		return length == sizeof(struct ext4_fc_head_disk);
	default:
		return false;
	}
}

static enum ext4_result
ext4_fc_incomplete(struct ext4_fc_scan *scan)
{
	/* Preserve an earlier checked commit without treating a malformed first
	 * record or checksum as an empty, successfully recovered filesystem. */
	scan->discarded_tail = scan->count != scan->committed;
	return scan->committed == 0 ? EXT4_CORRUPT : EXT4_OK;
}

static enum ext4_result
ext4_fc_scan(struct ext4_journal *journal, uint32_t first, uint32_t last, uint32_t sequence,
    struct ext4_fc_scan *scan)
{
	const struct ext4_fc_header_disk *header;
	const struct ext4_fc_head_disk *head;
	const struct ext4_fc_tail_disk *tail;
	const uint8_t *value;
	struct ext4_fc_record record;
	uint32_t block;
	uint32_t checksum = 0;
	uint32_t offset;
	uint32_t size = journal->fs->info.block_size;
	uint16_t type;
	uint16_t length;
	enum ext4_result error;

	for (block = first; block < last; block++) {
		error = ext4_journal_read(journal, block, journal->work);
		if (error != EXT4_OK) {
			return error;
		}
		for (offset = 0; offset <= size - sizeof(*header);
		    offset += (uint32_t)sizeof(*header) + length) {
			header = (const struct ext4_fc_header_disk *)(journal->work + offset);
			type = ext4_le16(&header->type);
			length = ext4_le16(&header->length);
			if (block == first && offset == 0 && type != EXT4_FC_HEAD) {
				return EXT4_OK;
			}
			if (length > size - offset - sizeof(*header) ||
			    !ext4_fc_length_valid(journal->fs, type, length)) {
				return ext4_fc_incomplete(scan);
			}
			value = journal->work + offset + sizeof(*header);
			if (type == EXT4_FC_HEAD) {
				head = (const struct ext4_fc_head_disk *)value;
				if (ext4_le32(&head->features) != 0) {
					return EXT4_UNSUPPORTED;
				}
				if (ext4_le32(&head->sequence) != sequence) {
					scan->discarded_tail = scan->count != scan->committed;
					return EXT4_OK;
				}
			}
			if (type == EXT4_FC_TAIL) {
				tail = (const struct ext4_fc_tail_disk *)value;
				checksum = ext4_crc32c(checksum, header,
				    sizeof(*header) + offsetof(struct ext4_fc_tail_disk, checksum));
				if (ext4_le32(&tail->sequence) != sequence ||
				    ext4_le32(&tail->checksum) != checksum) {
					return ext4_fc_incomplete(scan);
				}
				checksum = 0;
			} else {
				checksum = ext4_crc32c(checksum, header, sizeof(*header) + length);
			}
			if (scan->count == EXT4_FC_MAX_RECORDS) {
				return EXT4_UNSUPPORTED;
			}
			if (scan->records != NULL && scan->count < scan->capacity) {
				ext4_zero(&record, sizeof(record));
				record.block = block;
				record.offset = (uint16_t)offset;
				record.type = type;
				record.length = length;
				record.checksum =
				    ext4_crc32c(UINT32_MAX, header, sizeof(*header) + length);
				scan->records[scan->count] = record;
			}
			scan->count++;
			if (type == EXT4_FC_TAIL) {
				scan->committed = scan->count;
				scan->commits++;
			}
		}
	}
	scan->discarded_tail = scan->count != scan->committed;
	return EXT4_OK;
}

enum ext4_result
ext4_fast_commit_load(struct ext4_journal *journal, uint32_t first, uint32_t last,
    uint32_t sequence, struct ext4_fast_commit **result)
{
	struct ext4_fc_scan scan;
	struct ext4_fc_scan saved;
	struct ext4_fast_commit *log;
	struct ext4_fs *fs;
	size_t bytes;
	enum ext4_result error;

	if (result == NULL) {
		return EXT4_INVALID_ARGUMENT;
	}
	*result = NULL;
	if (journal == NULL || first < journal->first || first >= last || last > journal->blocks) {
		return EXT4_INVALID_ARGUMENT;
	}
	if (last - first > EXT4_FC_MAX_BLOCKS) {
		return EXT4_UNSUPPORTED;
	}
	fs = journal->fs;
	ext4_zero(&scan, sizeof(scan));
	error = ext4_fc_scan(journal, first, last, sequence, &scan);
	if (error != EXT4_OK) {
		return error;
	}
	bytes = sizeof(*log) + (size_t)scan.committed * sizeof(*log->records);
	log = fs->environment.allocate(fs->environment.context, bytes);
	if (log == NULL) {
		return EXT4_NO_MEMORY;
	}
	ext4_zero(log, bytes);
	log->journal = journal;
	log->count = scan.committed;
	log->commits = scan.commits;
	log->sequence = sequence;
	log->discarded_tail = scan.discarded_tail;
	if (scan.committed != 0) {
		ext4_zero(&saved, sizeof(saved));
		saved.records = log->records;
		saved.capacity = scan.committed;
		error = ext4_fc_scan(journal, first, last, sequence, &saved);
		if (error == EXT4_OK &&
		    (saved.committed != scan.committed || saved.commits != scan.commits)) {
			error = EXT4_CORRUPT;
		}
		if (error != EXT4_OK) {
			fs->environment.release(fs->environment.context, log, bytes);
			return error;
		}
	}
	*result = log;
	return EXT4_OK;
}

void
ext4_fast_commit_close(struct ext4_fast_commit *log)
{
	struct ext4_fs *fs;
	size_t bytes;

	if (log == NULL) {
		return;
	}
	fs = log->journal->fs;
	bytes = sizeof(*log) + (size_t)log->count * sizeof(*log->records);
	fs->environment.release(fs->environment.context, log, bytes);
}

enum ext4_result
ext4_fast_commit_read(
    const struct ext4_fast_commit *log, uint32_t index, void *buffer, const void **value)
{
	return ext4_fast_commit_read_cached(log, index, buffer, NULL, value);
}

enum ext4_result
ext4_fast_commit_read_cached(const struct ext4_fast_commit *log, uint32_t index, void *buffer,
    uint32_t *cached_block, const void **value)
{
	const struct ext4_fc_record *record;
	const struct ext4_fc_header_disk *header;
	enum ext4_result error;

	if (value == NULL) {
		return EXT4_INVALID_ARGUMENT;
	}
	*value = NULL;
	if (log == NULL || buffer == NULL || index >= log->count) {
		return EXT4_INVALID_ARGUMENT;
	}
	record = &log->records[index];
	if (cached_block == NULL || *cached_block != record->block) {
		error = ext4_journal_read(log->journal, record->block, buffer);
		if (error != EXT4_OK) {
			/* A failed read may still overwrite part of the previous block. */
			if (cached_block != NULL) {
				*cached_block = UINT32_MAX;
			}
			return error;
		}
		if (cached_block != NULL) {
			*cached_block = record->block;
		}
	}
	header = (const struct ext4_fc_header_disk *)((const uint8_t *)buffer + record->offset);
	if (ext4_crc32c(UINT32_MAX, header, sizeof(*header) + record->length) != record->checksum ||
	    ext4_le16(&header->type) != record->type ||
	    ext4_le16(&header->length) != record->length) {
		return EXT4_CORRUPT;
	}
	*value = header + 1;
	return EXT4_OK;
}

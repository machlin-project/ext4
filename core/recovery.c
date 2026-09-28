/* SPDX-License-Identifier: BSD-3-Clause */
#include "journal.h"

#define EXT4_RECOVERY_REVOKED 0x80000000U

struct ext4_recovery_record {
	uint64_t target;
	uint32_t log_block;
	uint32_t sequence;
	uint32_t transaction;
	uint32_t checksum;
	uint32_t order;
	uint32_t flags;
};

struct ext4_recovery_scan {
	struct ext4_journal *journal;
	struct ext4_recovery_record *records;
	uint64_t last_commit_seconds;
	uint32_t capacity;
	uint32_t cursor;
	uint32_t visited;
	uint32_t transaction_start;
	uint32_t transaction_visited;
	uint32_t sequence;
	uint32_t count;
	uint32_t committed_count;
	uint32_t transactions;
	uint32_t transaction_checksum;
	bool bad_data;
	bool discarded_tail;
};

static enum ext4_result
ext4_recovery_read(struct ext4_recovery_scan *scan, void *buffer)
{
	enum ext4_result error;

	if (scan->visited == scan->journal->blocks - scan->journal->first) {
		return EXT4_CORRUPT;
	}
	error = ext4_journal_read(scan->journal, scan->cursor, buffer);
	if (error == EXT4_OK) {
		scan->cursor = ext4_journal_next(scan->journal, scan->cursor);
		scan->visited++;
	}
	return error;
}

static enum ext4_result
ext4_recovery_add(struct ext4_recovery_scan *scan, struct ext4_recovery_record *record)
{
	if (!ext4_journal_target(scan->journal, record->target)) {
		return EXT4_CORRUPT;
	}
	if (scan->count == EXT4_RECOVERY_MAX_RECORDS) {
		return EXT4_UNSUPPORTED;
	}
	record->sequence = scan->sequence;
	record->transaction = scan->transactions;
	record->order = scan->count;
	if (scan->records != NULL) {
		if (scan->count >= scan->capacity) {
			return EXT4_CORRUPT;
		}
		scan->records[scan->count] = *record;
	}
	scan->count++;
	return EXT4_OK;
}

static bool
ext4_recovery_data_valid(struct ext4_journal *journal, const struct ext4_recovery_record *record)
{
	uint32_t checksum;

	if (journal->checksum_v1) {
		/* V1 covers the complete transaction on scan. A private per-block
		 * digest also binds the bytes read again during the replay pass. */
		return ext4_crc32c(UINT32_MAX, journal->data, journal->fs->info.block_size) ==
		    record->checksum;
	}
	if (!journal->checksum) {
		return true;
	}
	checksum = ext4_journal_data_checksum(journal, record->sequence, journal->data);
	if (!(journal->features & EXT4_JBD_CSUM_V3)) {
		checksum = (uint16_t)checksum;
	}
	return checksum == record->checksum;
}

static enum ext4_result
ext4_recovery_descriptor(struct ext4_recovery_scan *scan)
{
	struct ext4_journal *journal = scan->journal;
	struct ext4_recovery_record record;
	const struct ext4_jbd_tag3 *tag3;
	const struct ext4_jbd_tag *tag;
	const struct ext4_be32 *high;
	size_t offset = sizeof(struct ext4_jbd_header);
	size_t tag_size = ext4_journal_tag_size(journal);
	size_t end =
	    journal->fs->info.block_size - (journal->checksum ? sizeof(struct ext4_be32) : 0);
	uint32_t block_hi;
	bool first = true;
	enum ext4_result error;

	if (!ext4_journal_checksum_valid(
		journal, journal->work, journal->fs->info.block_size - sizeof(struct ext4_be32))) {
		return EXT4_CORRUPT;
	}
	if (journal->checksum_v1) {
		scan->transaction_checksum = ext4_crc32_be(
		    scan->transaction_checksum, journal->work, journal->fs->info.block_size);
	}
	while (offset <= end && tag_size <= end - offset) {
		ext4_zero(&record, sizeof(record));
		block_hi = 0;
		if (journal->features & EXT4_JBD_CSUM_V3) {
			tag3 = (const struct ext4_jbd_tag3 *)(journal->work + offset);
			record.target = ext4_be32(&tag3->block_lo);
			block_hi = ext4_be32(&tag3->block_hi);
			record.flags = ext4_be32(&tag3->flags);
			record.checksum = ext4_be32(&tag3->checksum);
		} else {
			tag = (const struct ext4_jbd_tag *)(journal->work + offset);
			record.target = ext4_be32(&tag->block_lo);
			record.flags = ext4_be16(&tag->flags);
			record.checksum = ext4_be16(&tag->checksum);
			if (journal->features & EXT4_JBD_64BIT) {
				high = (const struct ext4_be32 *)(journal->work + offset +
				    sizeof(*tag));
				block_hi = ext4_be32(high);
			}
		}
		if ((record.flags & ~EXT4_JBD_TAG_FLAGS) ||
		    (!(journal->features & EXT4_JBD_64BIT) && block_hi != 0)) {
			return EXT4_CORRUPT;
		}
		record.target |= (uint64_t)block_hi << 32;
		offset += tag_size;
		if (!(record.flags & EXT4_JBD_SAME_UUID)) {
			if (EXT4_UUID_SIZE > end - offset) {
				return EXT4_CORRUPT;
			}
			/* Internal journal ownership comes from its checked superblock.
			 * These historical tag UUID bytes may be zero (e.g. debugfs). */
			offset += EXT4_UUID_SIZE;
		} else if (first) {
			return EXT4_CORRUPT;
		}
		first = false;
		record.log_block = scan->cursor;
		error = ext4_recovery_add(scan, &record);
		if (error != EXT4_OK) {
			return error;
		}
		error = ext4_recovery_read(scan, journal->data);
		if (error != EXT4_OK) {
			return error;
		}
		if (journal->checksum_v1) {
			scan->transaction_checksum = ext4_crc32_be(scan->transaction_checksum,
			    journal->data, journal->fs->info.block_size);
			if (scan->records != NULL) {
				scan->records[scan->count - 1U].checksum = ext4_crc32c(
				    UINT32_MAX, journal->data, journal->fs->info.block_size);
			}
		} else if (!ext4_recovery_data_valid(journal, &record)) {
			scan->bad_data = true;
		}
		if (record.flags & EXT4_JBD_LAST_TAG) {
			return EXT4_OK;
		}
	}
	return EXT4_CORRUPT;
}

static enum ext4_result
ext4_recovery_revoke(struct ext4_recovery_scan *scan)
{
	struct ext4_journal *journal = scan->journal;
	const struct ext4_jbd_revoke *header = (const struct ext4_jbd_revoke *)journal->work;
	const struct ext4_be32 *word;
	struct ext4_recovery_record record;
	size_t offset = sizeof(*header);
	size_t stride = (journal->features & EXT4_JBD_64BIT) ? 2 * sizeof(*word) : sizeof(*word);
	size_t end = ext4_be32(&header->length);
	size_t limit =
	    journal->fs->info.block_size - (journal->checksum ? sizeof(struct ext4_be32) : 0);
	enum ext4_result error;

	/* Linux can commit its first revoke before the feature bit reaches the
	 * durable journal superblock. The record type defines this supported
	 * operation; its lengths, checksum, targets and commit still need checking. */
	if (end < offset || end > limit || (end - offset) % stride != 0 ||
	    !ext4_journal_checksum_valid(
		journal, journal->work, journal->fs->info.block_size - sizeof(struct ext4_be32))) {
		return EXT4_CORRUPT;
	}
	while (offset < end) {
		ext4_zero(&record, sizeof(record));
		word = (const struct ext4_be32 *)(journal->work + offset);
		record.target = ext4_be32(word);
		if (journal->features & EXT4_JBD_64BIT) {
			record.target = (record.target << 32) | ext4_be32(word + 1);
		}
		record.flags = EXT4_RECOVERY_REVOKED;
		error = ext4_recovery_add(scan, &record);
		if (error != EXT4_OK) {
			return error;
		}
		offset += stride;
	}
	return EXT4_OK;
}

static enum ext4_result
ext4_recovery_async_tail(struct ext4_recovery_scan *scan)
{
	struct ext4_journal *journal = scan->journal;
	const struct ext4_jbd_commit *commit = (const struct ext4_jbd_commit *)journal->data;
	uint64_t seconds;

	enum ext4_result error;

	/* An async commit can reach storage before its own log. A later commit
	 * proves that the earlier transaction completed, so its damage cannot
	 * be dismissed as the interrupted tail. Scan without trusting torn tags. */
	scan->cursor = scan->transaction_start;
	scan->visited = scan->transaction_visited;
	while (scan->visited < journal->blocks - journal->first) {
		error = ext4_recovery_read(scan, journal->data);
		if (error != EXT4_OK) {
			return error;
		}
		if (ext4_be32(&commit->header.magic) != EXT4_JBD_MAGIC ||
		    ext4_be32(&commit->header.type) != EXT4_JBD_COMMIT ||
		    ext4_be32(&commit->header.sequence) != scan->sequence + 1U) {
			continue;
		}
		seconds = ((uint64_t)ext4_be32(&commit->seconds_hi) << 32) |
		    ext4_be32(&commit->seconds_lo);
		/* Only a preceding validated commit provides a timestamp boundary.
		 * With no such boundary, ambiguous old records reject conservatively. */
		if (seconds >= scan->last_commit_seconds) {
			return EXT4_CORRUPT;
		}
	}
	scan->discarded_tail = true;
	return EXT4_OK;
}

static bool
ext4_recovery_commit_v1_valid(
    const struct ext4_journal *journal, const void *buffer, uint32_t transaction_checksum)
{
	const struct ext4_jbd_commit *commit = buffer;
	uint32_t checksum = ext4_be32(&commit->checksum[0]);

	if (!journal->checksum_v1) {
		return true;
	}
	/* Enabling the compatible feature can leave an older checksum-free commit
	 * in the same log. Linux admits only this exact all-zero legacy encoding. */
	if (commit->checksum_type == 0 && commit->checksum_size == 0 && checksum == 0) {
		return true;
	}
	return commit->checksum_type == EXT4_JBD_CRC32 &&
	    commit->checksum_size == sizeof(commit->checksum[0]) &&
	    checksum == transaction_checksum;
}

static enum ext4_result
ext4_recovery_incomplete_tail(struct ext4_recovery_scan *scan)
{
	struct ext4_journal *journal = scan->journal;
	const struct ext4_jbd_header *header = (const struct ext4_jbd_header *)journal->data;
	uint32_t checksum = UINT32_MAX;
	bool async = (journal->features & EXT4_JBD_ASYNC_COMMIT) != 0;
	enum ext4_result error;

	/* A torn descriptor cannot provide trusted tag lengths. Search the
	 * remaining ring for a valid commit with this ID before discarding it.
	 * Escaping prevents an ordinary log payload from impersonating a header. */
	/* Malformed tag counts may have consumed the commit as apparent data.
	 * Recheck from the beginning of this transaction, not from that cursor. */
	scan->cursor = scan->transaction_start;
	scan->visited = scan->transaction_visited;
	while (scan->visited < journal->blocks - journal->first) {
		error = ext4_recovery_read(scan, journal->data);
		if (error != EXT4_OK) {
			return error;
		}
		if (ext4_be32(&header->magic) == EXT4_JBD_MAGIC &&
		    ext4_be32(&header->type) == EXT4_JBD_COMMIT &&
		    ext4_be32(&header->sequence) == scan->sequence) {
			if (ext4_journal_checksum_valid(journal, journal->data,
				offsetof(struct ext4_jbd_commit, checksum)) &&
			    (!async ||
				ext4_recovery_commit_v1_valid(journal, journal->data, checksum))) {
				return EXT4_CORRUPT;
			}
			if (async) {
				return ext4_recovery_async_tail(scan);
			}
		}
		if (async && journal->checksum_v1 &&
		    !(ext4_be32(&header->magic) == EXT4_JBD_MAGIC &&
			ext4_be32(&header->type) == EXT4_JBD_REVOKE &&
			ext4_be32(&header->sequence) == scan->sequence)) {
			checksum =
			    ext4_crc32_be(checksum, journal->data, journal->fs->info.block_size);
		}
	}
	if (async) {
		return ext4_recovery_async_tail(scan);
	}
	scan->discarded_tail = true;
	return EXT4_OK;
}

static enum ext4_result
ext4_recovery_scan_log(struct ext4_recovery_scan *scan, uint32_t transaction_limit)
{
	struct ext4_journal *journal = scan->journal;
	const struct ext4_jbd_header *header = (const struct ext4_jbd_header *)journal->work;
	const struct ext4_jbd_commit *commit = (const struct ext4_jbd_commit *)journal->work;
	uint32_t type;
	bool in_transaction = false;
	enum ext4_result error;

	scan->cursor = journal->start;
	scan->sequence = journal->sequence;
	if (scan->cursor == 0) {
		return EXT4_OK;
	}
	while (scan->transactions < transaction_limit) {
		if (!in_transaction) {
			scan->transaction_start = scan->cursor;
			scan->transaction_visited = scan->visited;
			scan->transaction_checksum = UINT32_MAX;
		}
		error = ext4_recovery_read(scan, journal->work);
		if (error != EXT4_OK) {
			return error;
		}
		if (ext4_be32(&header->magic) != EXT4_JBD_MAGIC ||
		    ext4_be32(&header->sequence) != scan->sequence) {
			if (in_transaction) {
				return ext4_recovery_incomplete_tail(scan);
			}
			scan->discarded_tail = in_transaction;
			break;
		}
		type = ext4_be32(&header->type);
		if (type == EXT4_JBD_DESCRIPTOR) {
			in_transaction = true;
			error = ext4_recovery_descriptor(scan);
		} else if (type == EXT4_JBD_REVOKE) {
			in_transaction = true;
			error = ext4_recovery_revoke(scan);
		} else if (type == EXT4_JBD_COMMIT) {
			if (!ext4_recovery_commit_v1_valid(
				journal, journal->work, scan->transaction_checksum)) {
				return journal->features & EXT4_JBD_ASYNC_COMMIT
				    ? ext4_recovery_async_tail(scan)
				    : EXT4_CORRUPT;
			}
			if (!ext4_journal_checksum_valid(journal, journal->work,
				offsetof(struct ext4_jbd_commit, checksum))) {
				if (journal->features & EXT4_JBD_ASYNC_COMMIT) {
					return ext4_recovery_async_tail(scan);
				}
				scan->discarded_tail = true;
				break;
			}
			if (scan->bad_data) {
				return EXT4_CORRUPT;
			}
			scan->transactions++;
			scan->sequence++;
			scan->committed_count = scan->count;
			scan->last_commit_seconds =
			    ((uint64_t)ext4_be32(&commit->seconds_hi) << 32) |
			    ext4_be32(&commit->seconds_lo);
			in_transaction = false;
		} else {
			return EXT4_CORRUPT;
		}
		if (error != EXT4_OK) {
			if (error == EXT4_CORRUPT) {
				return ext4_recovery_incomplete_tail(scan);
			}
			return error;
		}
	}
	return EXT4_OK;
}

static bool
ext4_recovery_less(
    const struct ext4_recovery_record *left, const struct ext4_recovery_record *right)
{
	if (left->target != right->target) {
		return left->target < right->target;
	}
	if (left->transaction != right->transaction) {
		return left->transaction < right->transaction;
	}
	if ((left->flags ^ right->flags) & EXT4_RECOVERY_REVOKED) {
		return (left->flags & EXT4_RECOVERY_REVOKED) == 0;
	}
	return left->order < right->order;
}

static void
ext4_recovery_sift(struct ext4_recovery_record *records, uint32_t root, uint32_t count)
{
	struct ext4_recovery_record saved = records[root];
	uint32_t child;

	while (root < count / 2) {
		child = root * 2 + 1;
		if (child + 1 < count && ext4_recovery_less(&records[child], &records[child + 1])) {
			child++;
		}
		if (!ext4_recovery_less(&saved, &records[child])) {
			break;
		}
		records[root] = records[child];
		root = child;
	}
	records[root] = saved;
}

static void
ext4_recovery_sort(struct ext4_recovery_record *records, uint32_t count)
{
	struct ext4_recovery_record swap;
	uint32_t index;

	for (index = count / 2; index > 0; index--) {
		ext4_recovery_sift(records, index - 1, count);
	}
	for (index = count; index > 1; index--) {
		swap = records[0];
		records[0] = records[index - 1];
		records[index - 1] = swap;
		ext4_recovery_sift(records, 0, index - 1);
	}
}

static bool
ext4_recovery_compat_valid(uint32_t original, uint32_t replayed)
{
	/* The first attribute can enable EXT_ATTR in its committed transaction.
	 * Clearing it could hide existing attributes; no other format change is
	 * compatible with the ownership captured before replay. */
	return replayed == original || replayed == (original | EXT4_FEATURE_COMPAT_EXT_ATTR);
}

static bool
ext4_recovery_ro_compat_valid(uint32_t original, uint32_t replayed)
{
	/* A directory link-count overflow can enable DIR_NLINK atomically. The
	 * orphan-presence marker is maintained separately by recovery. Neither
	 * clearing DIR_NLINK nor changing another format feature is admitted. */
	original |= EXT4_FEATURE_RO_ORPHAN_PRESENT;
	replayed |= EXT4_FEATURE_RO_ORPHAN_PRESENT;
	return replayed == original || replayed == (original | EXT4_FEATURE_RO_DIR_NLINK);
}

static enum ext4_result
ext4_recovery_super(struct ext4_journal *journal)
{
	struct ext4_fs *fs = journal->fs;
	struct ext4_super_disk *super;
	uint32_t checksum;

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
	if (!ext4_recovery_compat_valid(
		fs->info.feature_compat, ext4_le32(&super->feature_compat)) ||
	    !ext4_recovery_ro_compat_valid(
		fs->info.feature_ro_compat, ext4_le32(&super->feature_ro_compat))) {
		return EXT4_UNSUPPORTED;
	}
	if (((fs->info.feature_incompat ^ ext4_le32(&super->feature_incompat)) &
		EXT4_FEATURE_INCOMPAT_META_BG) ||
	    ((fs->info.feature_incompat & EXT4_FEATURE_INCOMPAT_META_BG) &&
		fs->first_meta_group != ext4_le32(&super->first_meta_group)) ||
	    ((fs->info.feature_compat & EXT4_FEATURE_COMPAT_SPARSE_SUPER2) &&
		(fs->backup_groups[0] != ext4_le32(&super->backup_groups[0]) ||
		    fs->backup_groups[1] != ext4_le32(&super->backup_groups[1])))) {
		return EXT4_UNSUPPORTED;
	}
	/* Replaying a clean superblock must not hide an interrupted recovery. */
	ext4_encode32(&super->feature_incompat,
	    ext4_le32(&super->feature_incompat) | EXT4_FEATURE_INCOMPAT_RECOVER);
	if (fs->orphan_file_inode != 0) {
		ext4_encode32(&super->feature_ro_compat,
		    ext4_le32(&super->feature_ro_compat) | EXT4_FEATURE_RO_ORPHAN_PRESENT);
	}
	if (fs->metadata_checksum) {
		checksum =
		    ext4_crc32c(UINT32_MAX, super, offsetof(struct ext4_super_disk, checksum));
		ext4_encode32(&super->checksum, checksum);
	}
	return EXT4_OK;
}

static enum ext4_result
ext4_recovery_apply(struct ext4_recovery_scan *scan, struct ext4_recovery_report *report)
{
	struct ext4_journal *journal = scan->journal;
	const struct ext4_recovery_record *record;
	uint32_t index;
	enum ext4_result error;

	ext4_recovery_sort(scan->records, scan->count);
	for (index = 0; index < scan->count; index++) {
		record = &scan->records[index];
		if (index + 1 < scan->count && scan->records[index + 1].target == record->target) {
			continue;
		}
		/* The last committed event wins. A revoke also wins against data from
		 * its own transaction. A later allocation may legitimately log again. */
		if (record->flags & EXT4_RECOVERY_REVOKED) {
			report->revoked_blocks++;
			continue;
		}
		error = ext4_journal_read(journal, record->log_block, journal->data);
		if (error != EXT4_OK) {
			return error;
		}
		if (!ext4_recovery_data_valid(journal, record)) {
			return EXT4_CORRUPT;
		}
		if (record->flags & EXT4_JBD_ESCAPE) {
			ext4_encode_be32((struct ext4_be32 *)journal->data, EXT4_JBD_MAGIC);
		}
		if (record->target == EXT4_SUPER_OFFSET / journal->fs->info.block_size) {
			error = ext4_recovery_super(journal);
			if (error != EXT4_OK) {
				return error;
			}
		}
		error = ext4_journal_write_home(journal, record->target, journal->data);
		if (error != EXT4_OK) {
			return error;
		}
		report->replayed_blocks++;
	}
	return ext4_journal_flush(journal);
}

static enum ext4_result
ext4_recovery_validate_home(struct ext4_journal *journal)
{
	struct ext4_fs *fs = journal->fs;
	struct ext4_fs *fresh;
	struct ext4_inode root;
	enum ext4_result error;

	error = ext4_load(&fs->environment, true, &fresh);
	if (error != EXT4_OK) {
		return error;
	}
	if (fresh->info.blocks != fs->info.blocks ||
	    fresh->info.block_size != fs->info.block_size ||
	    fresh->info.inodes != fs->info.inodes || fresh->info.groups != fs->info.groups ||
	    !ext4_recovery_compat_valid(fs->info.feature_compat, fresh->info.feature_compat) ||
	    !ext4_recovery_ro_compat_valid(
		fs->info.feature_ro_compat, fresh->info.feature_ro_compat) ||
	    (fresh->info.feature_incompat | EXT4_FEATURE_INCOMPAT_RECOVER) !=
		(fs->info.feature_incompat | EXT4_FEATURE_INCOMPAT_RECOVER) ||
	    fresh->inode_size != fs->inode_size || fresh->descriptor_size != fs->descriptor_size ||
	    fresh->blocks_per_group != fs->blocks_per_group ||
	    fresh->cluster_blocks != fs->cluster_blocks ||
	    fresh->clusters_per_group != fs->clusters_per_group ||
	    fresh->inodes_per_group != fs->inodes_per_group ||
	    fresh->checksum_seed != fs->checksum_seed || fresh->first_inode != fs->first_inode ||
	    fresh->journal_inode != fs->journal_inode ||
	    fresh->orphan_file_inode != fs->orphan_file_inode ||
	    fresh->reserved_gdt_blocks != fs->reserved_gdt_blocks ||
	    fresh->first_meta_group != fs->first_meta_group ||
	    !ext4_equal(fresh->backup_groups, fs->backup_groups, sizeof(fs->backup_groups)) ||
	    !ext4_equal(fresh->info.uuid, fs->info.uuid, EXT4_UUID_SIZE)) {
		error = EXT4_UNSUPPORTED;
	} else {
		error = ext4_get_inode(fresh, EXT4_ROOT_INODE, &root);
		if (error == EXT4_OK && (root.mode & EXT4_MODE_TYPE) != EXT4_MODE_DIRECTORY) {
			error = EXT4_CORRUPT;
		}
		if (error == EXT4_OK) {
			fs->info = fresh->info;
			fs->last_orphan = fresh->last_orphan;
		}
	}
	ext4_unmount(fresh);
	return error;
}

static enum ext4_result
ext4_recovery_account(struct ext4_fs *fs, struct ext4_recovery_report *report)
{
	struct ext4_group group;
	struct ext4_super_disk *super;
	struct ext4_transaction *transaction;
	uint64_t free_blocks = 0;
	uint64_t free_inodes = 0;
	uint64_t available_blocks;
	uint64_t available_inodes;
	uint32_t index;
	enum ext4_result error;

	/* Linux's superblock summaries can lag committed allocation changes.
	 * Reconstruct them from the replayed, checksummed group descriptors only
	 * during explicit recovery. A clean writable mount still requires a match. */
	for (index = 0; index < fs->info.groups; index++) {
		error = ext4_group_get(fs, index, &group);
		if (error != EXT4_OK) {
			return error;
		}
		available_blocks =
		    fs->info.blocks - fs->first_data_block - (uint64_t)index * fs->blocks_per_group;
		available_inodes = fs->info.inodes - (uint64_t)index * fs->inodes_per_group;
		if (group.free_blocks > available_blocks || group.free_inodes > available_inodes) {
			return EXT4_CORRUPT;
		}
		free_blocks += group.free_blocks;
		free_inodes += group.free_inodes;
	}
	if (free_blocks == fs->info.free_blocks && free_inodes == fs->info.free_inodes) {
		return EXT4_OK;
	}
	error = ext4_transaction_begin(fs->journal, 1, &transaction);
	if (error != EXT4_OK) {
		return error;
	}
	error = ext4_transaction_super(transaction, &super);
	if (error != EXT4_OK) {
		ext4_transaction_cancel(transaction);
		return error;
	}
	ext4_encode32(&super->free_blocks_lo, (uint32_t)free_blocks);
	if (fs->info.feature_incompat & EXT4_FEATURE_INCOMPAT_64BIT) {
		ext4_encode32(&super->free_blocks_hi, (uint32_t)(free_blocks >> 32));
	}
	ext4_encode32(&super->free_inodes, (uint32_t)free_inodes);
	error = ext4_transaction_commit(transaction);
	if (error == EXT4_OK) {
		fs->info.free_blocks = free_blocks;
		fs->info.free_inodes = (uint32_t)free_inodes;
		report->accounting_updated = true;
	}
	return error;
}

enum ext4_result
ext4_recover(const struct ext4_environment *environment,
    const struct ext4_write_environment *writer, struct ext4_recovery_report *report)
{
	struct ext4_recovery_report completed;
	struct ext4_recovery_scan scan;
	struct ext4_recovery_scan replay;
	struct ext4_fs *fs;
	struct ext4_journal *journal;
	size_t bytes = 0;
	enum ext4_result error;

	ext4_zero(&completed, sizeof(completed));
	if (report != NULL) {
		*report = completed;
	}
	if (writer == NULL || writer->write == NULL || writer->flush == NULL) {
		return EXT4_INVALID_ARGUMENT;
	}
	error = ext4_load(environment, true, &fs);
	if (error != EXT4_OK) {
		return error;
	}
	if (!(fs->info.feature_incompat & EXT4_FEATURE_INCOMPAT_RECOVER) &&
	    !(fs->info.feature_ro_compat & EXT4_FEATURE_RO_ORPHAN_PRESENT) &&
	    fs->last_orphan == 0) {
		ext4_unmount(fs);
		error = ext4_mount(environment, &fs);
		ext4_unmount(fs);
		return error;
	}
	error = ext4_journal_load(fs, writer, &journal);
	if (error != EXT4_OK) {
		ext4_unmount(fs);
		return error;
	}
	fs->journal = journal;
	ext4_zero(&scan, sizeof(scan));
	ext4_zero(&replay, sizeof(replay));
	scan.journal = journal;
	replay.journal = journal;
	/* Validate the complete committed prefix before any home write. A second
	 * bounded pass records locations; it never keeps file data in a cache. */
	error = ext4_recovery_scan_log(&scan, UINT32_MAX);
	if (error != EXT4_OK) {
		goto out;
	}
	completed.transactions = scan.transactions;
	completed.discarded_tail = scan.discarded_tail;
	if (scan.committed_count != 0) {
		bytes = (size_t)scan.committed_count * sizeof(*replay.records);
		replay.records = fs->environment.allocate(fs->environment.context, bytes);
		if (replay.records == NULL) {
			error = EXT4_NO_MEMORY;
			goto out;
		}
		replay.capacity = scan.committed_count;
		error = ext4_recovery_scan_log(&replay, scan.transactions);
		if (error != EXT4_OK) {
			goto out;
		}
		if (replay.transactions != scan.transactions ||
		    replay.count != scan.committed_count) {
			error = EXT4_CORRUPT;
			goto out;
		}
	}
	error = ext4_recovery_apply(&replay, &completed);
	if (error == EXT4_OK) {
		error = ext4_recovery_validate_home(journal);
	}
	if (error == EXT4_OK) {
		error = ext4_journal_reset(journal, scan.sequence + 1);
	}
	if (error == EXT4_OK) {
		error = ext4_recovery_account(fs, &completed);
	}
	if (error == EXT4_OK) {
		if (fs->last_orphan != 0 || fs->orphan_file_inode != 0) {
			error = ext4_system_ranges_build(fs);
			if (error == EXT4_OK) {
				error = ext4_orphan_cleanup(fs, &completed);
			}
		}
	}
	if (error == EXT4_OK) {
		error = ext4_journal_finish(journal);
	}
out:
	if (replay.records != NULL) {
		fs->environment.release(fs->environment.context, replay.records, bytes);
	}
	if (report != NULL) {
		*report = completed;
	}
	ext4_unmount(fs);
	return error;
}

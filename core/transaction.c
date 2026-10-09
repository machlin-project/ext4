/* SPDX-License-Identifier: BSD-3-Clause */
#include "transaction.h"

#define EXT4_TRANSACTION_FREED_RANGES 64U

enum ext4_snapshot_contents {
	EXT4_SNAPSHOT_CURRENT,
	EXT4_SNAPSHOT_ZERO,
	EXT4_SNAPSHOT_REPLACE,
};

enum ext4_transaction_kind {
	EXT4_TRANSACTION_ORDINARY,
	EXT4_TRANSACTION_REQUEST,
	EXT4_TRANSACTION_RECOVERY,
};

static uint32_t
ext4_transaction_slot_count(uint32_t credits)
{
	uint32_t slots = 1;

	while (slots < credits * 2U) {
		slots <<= 1;
	}
	return slots;
}

static size_t
ext4_transaction_size(uint32_t credits)
{
	return sizeof(struct ext4_transaction) +
	    (size_t)credits * sizeof(struct ext4_transaction_entry) +
	    (size_t)ext4_transaction_slot_count(credits) * sizeof(uint32_t);
}

/* Private transactions and retained snapshot sets share storage, but their
 * admission and publication belong to their callers. Entries are initialized
 * when enrolled; only the header and empty index need initialization here. */
struct ext4_transaction *
ext4_transaction_allocate(struct ext4_journal *journal, uint32_t capacity)
{
	struct ext4_transaction *transaction;
	uint32_t slots = ext4_transaction_slot_count(capacity);
	uint32_t slot;
	size_t size = ext4_transaction_size(capacity);

	transaction = journal->fs->environment.allocate(journal->fs->environment.context, size);
	if (transaction == NULL) {
		return NULL;
	}
	ext4_zero(transaction, sizeof(*transaction));
	transaction->journal = journal;
	transaction->capacity = capacity;
	transaction->slots = (uint32_t *)(transaction->entries + capacity);
	transaction->slot_mask = slots - 1U;
	for (slot = 0; slot < slots; slot++) {
		transaction->slots[slot] = UINT32_MAX;
	}
	return transaction;
}

static uint32_t
ext4_transaction_slot(const struct ext4_transaction *transaction, uint64_t block)
{
	return (uint32_t)((block * UINT64_C(0x9e3779b97f4a7c15)) >> 32) & transaction->slot_mask;
}

/* Return the entry position holding block, or the transaction's count. */
static uint32_t
ext4_transaction_find(const struct ext4_transaction *transaction, uint64_t block)
{
	uint32_t slot = ext4_transaction_slot(transaction, block);
	uint32_t entry;

	for (;;) {
		entry = transaction->slots[slot];
		if (entry == UINT32_MAX) {
			return transaction->count;
		}
		if (transaction->entries[entry].block == block) {
			return entry;
		}
		slot = (slot + 1U) & transaction->slot_mask;
	}
}

static void
ext4_transaction_index(struct ext4_transaction *transaction, uint32_t entry)
{
	uint32_t slot = ext4_transaction_slot(transaction, transaction->entries[entry].block);

	while (transaction->slots[slot] != UINT32_MAX) {
		slot = (slot + 1U) & transaction->slot_mask;
	}
	transaction->slots[slot] = entry;
}

static enum ext4_result
ext4_transaction_create(struct ext4_journal *journal, uint32_t credits,
    enum ext4_transaction_kind kind, uint32_t sequence, struct ext4_transaction **result)
{
	struct ext4_transaction *transaction;
	uint32_t capacity;
	uint32_t maximum;
	bool recovery = kind == EXT4_TRANSACTION_RECOVERY;
	enum ext4_result error;

	if (result == NULL) {
		return EXT4_INVALID_ARGUMENT;
	}
	*result = NULL;
	if (journal == NULL || journal->transaction_active || credits == 0) {
		return EXT4_INVALID_ARGUMENT;
	}
	maximum = EXT4_TRANSACTION_MAX_BLOCKS;
	if (kind == EXT4_TRANSACTION_REQUEST) {
		maximum = ext4_journal_request_credits(journal);
	} else if (recovery) {
		maximum = ext4_journal_recovery_credits(journal);
	}
	if (credits > maximum) {
		return EXT4_INVALID_ARGUMENT;
	}
	/* A log this instance did not commit needs recovery first. */
	if (journal->aborted || (!recovery && journal->start != 0 && journal->checkpoint == NULL)) {
		return EXT4_RECOVERY_REQUIRED;
	}
	if (!recovery) {
		error = ext4_mmp_guard(journal->fs);
		if (error != EXT4_OK) {
			return error;
		}
	}
	/* Ordinary transactions reserve room for their quota updates; recovery
	 * conversions already hold the recovery bound. */
	capacity = credits + (journal->fs->quota_active && !recovery ? EXT4_QUOTA_CREDITS : 0U);
	/* Reserve conservatively: at worst one descriptor for every data block. A volume
	 * without a journal has no log to fill. */
	if (!journal->direct && capacity * 2U + 1U >= journal->last - journal->first) {
		return EXT4_RANGE;
	}
	transaction = ext4_transaction_allocate(journal, capacity);
	if (transaction == NULL) {
		return EXT4_NO_MEMORY;
	}
	transaction->credits = credits;
	transaction->sequence = recovery ? sequence : journal->sequence;
	transaction->recovery = recovery;
	journal->transaction_active = true;
	*result = transaction;
	return EXT4_OK;
}

enum ext4_result
ext4_transaction_begin(
    struct ext4_journal *journal, uint32_t credits, struct ext4_transaction **result)
{
	return ext4_transaction_create(journal, credits, EXT4_TRANSACTION_ORDINARY, 0, result);
}

enum ext4_result
ext4_transaction_begin_request(
    struct ext4_journal *journal, uint32_t credits, struct ext4_transaction **result)
{
	return ext4_transaction_create(journal, credits, EXT4_TRANSACTION_REQUEST, 0, result);
}

enum ext4_result
ext4_transaction_begin_recovery(struct ext4_journal *journal, uint32_t sequence, uint32_t credits,
    struct ext4_transaction **result)
{
	return ext4_transaction_create(
	    journal, credits, EXT4_TRANSACTION_RECOVERY, sequence, result);
}

static enum ext4_result
ext4_transaction_admit(
    struct ext4_transaction *transaction, uint64_t block, bool primary, uint32_t *index)
{
	struct ext4_journal *journal;
	struct ext4_fs *fs;

	if (transaction == NULL) {
		return EXT4_INVALID_ARGUMENT;
	}
	journal = transaction->journal;
	fs = journal->fs;
	if (!ext4_journal_target(journal, block) ||
	    (!primary && block == EXT4_SUPER_OFFSET / fs->info.block_size) ||
	    (!(journal->features & EXT4_JBD_64BIT) && block > UINT32_MAX)) {
		return EXT4_INVALID_ARGUMENT;
	}
	*index = ext4_transaction_find(transaction, block);
	if (*index != transaction->count) {
		return EXT4_OK;
	}
	if (transaction->count ==
	    (transaction->quota_phase ? transaction->capacity : transaction->credits)) {
		transaction->capacity_failed = true;
		return EXT4_RANGE;
	}
	return EXT4_OK;
}

/* Detach a caller view before exposing mutable storage or retaining it beyond
 * the private operation. Allocation failure leaves the view and source intact. */
enum ext4_result
ext4_transaction_own(struct ext4_fs *fs, struct ext4_transaction_entry *entry)
{
	void *buffer;

	if (entry->source_blocks == 0) {
		return EXT4_OK;
	}
	buffer = fs->environment.allocate(fs->environment.context, fs->info.block_size);
	if (buffer == NULL) {
		return EXT4_NO_MEMORY;
	}
	ext4_copy(buffer, entry->buffer, fs->info.block_size);
	entry->buffer = buffer;
	entry->source_blocks = 0;
	return EXT4_OK;
}

void
ext4_transaction_release(struct ext4_fs *fs, const struct ext4_transaction_entry *entry)
{
	if (entry->source_blocks == 0) {
		fs->environment.release(
		    fs->environment.context, entry->buffer, fs->info.block_size);
	}
}

static enum ext4_result
ext4_transaction_snapshot(struct ext4_transaction *transaction, uint64_t block, bool primary,
    enum ext4_snapshot_contents contents, bool data, void **result)
{
	struct ext4_fs *fs;
	void *buffer;
	uint32_t index;
	enum ext4_result error;

	if (result == NULL) {
		return EXT4_INVALID_ARGUMENT;
	}
	*result = NULL;
	error = ext4_transaction_admit(transaction, block, primary, &index);
	if (error != EXT4_OK) {
		return error;
	}
	fs = transaction->journal->fs;
	if (index != transaction->count) {
		error = ext4_transaction_own(fs, &transaction->entries[index]);
		if (error == EXT4_OK) {
			*result = transaction->entries[index].buffer;
		}
		return error;
	}
	buffer = fs->environment.allocate(fs->environment.context, fs->info.block_size);
	if (buffer == NULL) {
		return EXT4_NO_MEMORY;
	}
	error = EXT4_OK;
	if (contents == EXT4_SNAPSHOT_ZERO) {
		ext4_zero(buffer, fs->info.block_size);
	} else if (contents == EXT4_SNAPSHOT_CURRENT) {
		error = ext4_block_read(fs, block, buffer);
	}
	if (error != EXT4_OK) {
		fs->environment.release(fs->environment.context, buffer, fs->info.block_size);
		return error;
	}
	transaction->entries[transaction->count].block = block;
	transaction->entries[transaction->count].buffer = buffer;
	transaction->entries[transaction->count].source_blocks = 0;
	transaction->entries[transaction->count].data = data;
	ext4_transaction_index(transaction, transaction->count++);
	*result = buffer;
	return EXT4_OK;
}

enum ext4_result
ext4_transaction_buffer(struct ext4_transaction *transaction, uint64_t block, void **result)
{
	return ext4_transaction_snapshot(
	    transaction, block, false, EXT4_SNAPSHOT_CURRENT, false, result);
}

enum ext4_result
ext4_transaction_buffer_blank(struct ext4_transaction *transaction, uint64_t block, void **result)
{
	return ext4_transaction_snapshot(
	    transaction, block, false, EXT4_SNAPSHOT_ZERO, false, result);
}

enum ext4_result
ext4_transaction_data(
    struct ext4_transaction *transaction, uint64_t block, bool blank, void **result)
{
	return ext4_transaction_snapshot(transaction, block, false,
	    blank ? EXT4_SNAPSHOT_ZERO : EXT4_SNAPSHOT_CURRENT, true, result);
}

enum ext4_result
ext4_transaction_data_replace(struct ext4_transaction *transaction, uint64_t block, void **result)
{
	return ext4_transaction_snapshot(
	    transaction, block, false, EXT4_SNAPSHOT_REPLACE, true, result);
}

enum ext4_result
ext4_transaction_data_source(
    struct ext4_transaction *transaction, uint64_t block, const void *source, size_t available)
{
	struct ext4_transaction_entry *entry;
	uint32_t index;
	size_t blocks;
	enum ext4_result error;

	if (transaction == NULL || source == NULL ||
	    available < transaction->journal->fs->info.block_size) {
		return EXT4_INVALID_ARGUMENT;
	}
	error = ext4_transaction_admit(transaction, block, false, &index);
	if (error != EXT4_OK) {
		return error;
	}
	entry = &transaction->entries[index];
	if (index != transaction->count && entry->source_blocks == 0) {
		/* Preserve an earlier snapshot's ownership and data/metadata role. */
		ext4_copy(entry->buffer, source, transaction->journal->fs->info.block_size);
		return EXT4_OK;
	}
	entry->block = block;
	entry->buffer = (void *)source;
	blocks = available / transaction->journal->fs->info.block_size;
	entry->source_blocks =
	    blocks < transaction->capacity ? (uint32_t)blocks : transaction->capacity;
	entry->data = true;
	if (index == transaction->count) {
		ext4_transaction_index(transaction, transaction->count++);
	}
	return EXT4_OK;
}

void
ext4_transaction_freed(struct ext4_transaction *transaction, uint64_t block, uint64_t length)
{
	struct ext4_fs *fs = transaction->journal->fs;
	struct ext4_block_range *last;

	if (!transaction->journal->ordered_data || transaction->freed_overflow) {
		return;
	}
	if (transaction->freed == NULL) {
		transaction->freed = fs->environment.allocate(fs->environment.context,
		    EXT4_TRANSACTION_FREED_RANGES * sizeof(*transaction->freed));
		if (transaction->freed == NULL) {
			transaction->freed_overflow = true;
			return;
		}
	}
	last = transaction->freed_count != 0 ? &transaction->freed[transaction->freed_count - 1U]
					     : NULL;
	if (last != NULL && last->first + last->length == block) {
		last->length += length;
	} else if (transaction->freed_count == EXT4_TRANSACTION_FREED_RANGES) {
		transaction->freed_overflow = true;
	} else {
		transaction->freed[transaction->freed_count].first = block;
		transaction->freed[transaction->freed_count++].length = length;
	}
}

bool
ext4_transaction_capacity_failed(const struct ext4_transaction *transaction)
{
	return transaction->capacity_failed;
}

enum ext4_result
ext4_transaction_super(struct ext4_transaction *transaction, struct ext4_super_disk **result)
{
	struct ext4_fs *fs;
	struct ext4_super_disk *super;
	void *buffer;
	bool enrolled;
	enum ext4_result error;

	if (result == NULL) {
		return EXT4_INVALID_ARGUMENT;
	}
	*result = NULL;
	if (transaction == NULL) {
		return EXT4_INVALID_ARGUMENT;
	}
	fs = transaction->journal->fs;
	/* Validate the committed copy once; later contexts see earlier changes,
	 * whose checksum commit recalculates. */
	enrolled =
	    ext4_transaction_peek(transaction, EXT4_SUPER_OFFSET / fs->info.block_size) != NULL;
	error = ext4_transaction_snapshot(transaction, EXT4_SUPER_OFFSET / fs->info.block_size,
	    true, EXT4_SNAPSHOT_CURRENT, false, &buffer);
	if (error != EXT4_OK) {
		return error;
	}
	super =
	    (struct ext4_super_disk *)((uint8_t *)buffer + EXT4_SUPER_OFFSET % fs->info.block_size);
	if (!enrolled &&
	    (ext4_le16(&super->magic) != EXT4_SUPER_MAGIC ||
		!ext4_equal(super->uuid, fs->info.uuid, EXT4_UUID_SIZE) ||
		(fs->metadata_checksum &&
		    ext4_crc32c(UINT32_MAX, super, offsetof(struct ext4_super_disk, checksum)) !=
			ext4_le32(&super->checksum)))) {
		return EXT4_CORRUPT;
	}
	*result = super;
	return EXT4_OK;
}

enum ext4_result
ext4_transaction_read(struct ext4_transaction *transaction, uint64_t block, void *buffer)
{
	uint32_t index;

	if (transaction == NULL || buffer == NULL) {
		return EXT4_INVALID_ARGUMENT;
	}
	index = ext4_transaction_find(transaction, block);
	if (index != transaction->count) {
		ext4_copy(buffer, transaction->entries[index].buffer,
		    transaction->journal->fs->info.block_size);
		return EXT4_OK;
	}
	return ext4_block_read(transaction->journal->fs, block, buffer);
}

struct ext4_fs *
ext4_transaction_fs(const struct ext4_transaction *transaction)
{
	return transaction->journal->fs;
}

uint32_t
ext4_transaction_count(const struct ext4_transaction *transaction)
{
	return transaction->count;
}

void
ext4_transaction_entry(const struct ext4_transaction *transaction, uint32_t index, uint64_t *block,
    const void **buffer)
{
	*block = transaction->entries[index].block;
	*buffer = transaction->entries[index].buffer;
}

const void *
ext4_transaction_peek(const struct ext4_transaction *transaction, uint64_t block)
{
	uint32_t index = ext4_transaction_find(transaction, block);

	return index == transaction->count ? NULL : transaction->entries[index].buffer;
}

uint32_t
ext4_journal_credits(const struct ext4_journal *journal)
{
	uint32_t credits = journal->direct ? EXT4_TRANSACTION_MAX_BLOCKS
					   : (journal->last - journal->first - 2U) / 2U;

	/* Leave the quota reserve inside the same log-space bound. */
	if (journal->fs->quota_active) {
		credits = credits > EXT4_QUOTA_CREDITS ? credits - EXT4_QUOTA_CREDITS : 1U;
	}
	return credits > EXT4_TRANSACTION_MAX_BLOCKS ? EXT4_TRANSACTION_MAX_BLOCKS : credits;
}

uint32_t
ext4_journal_recovery_credits(const struct ext4_journal *journal)
{
	uint32_t credits = (journal->last - journal->first - 2U) / 2U;
	uint32_t memory = EXT4_TRANSACTION_SNAPSHOT_BYTES / journal->fs->info.block_size;

	if (credits > memory) {
		credits = memory;
	}
	return credits < EXT4_TRANSACTION_MAX_BLOCKS ? ext4_journal_credits(journal) : credits;
}

uint32_t
ext4_journal_request_credits(const struct ext4_journal *journal)
{
	uint32_t memory = EXT4_TRANSACTION_SNAPSHOT_BYTES / journal->fs->info.block_size;
	uint32_t credits = journal->direct ? memory : (journal->last - journal->first - 2U) / 2U;

	if (credits > memory) {
		credits = memory;
	}
	if (journal->fs->quota_active) {
		credits = credits > EXT4_QUOTA_CREDITS ? credits - EXT4_QUOTA_CREDITS : 1U;
	}
	return credits;
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
	ext4_orphan_transaction_cancel(transaction);
	for (index = 0; index < transaction->count; index++) {
		ext4_transaction_release(fs, &transaction->entries[index]);
	}
	if (transaction->freed != NULL) {
		fs->environment.release(fs->environment.context, transaction->freed,
		    EXT4_TRANSACTION_FREED_RANGES * sizeof(*transaction->freed));
	}
	if (!transaction->held) {
		transaction->journal->transaction_active = false;
	}
	size = ext4_transaction_size(transaction->capacity);
	fs->environment.release(fs->environment.context, transaction, size);
}

/* Transfer snapshots to an admitted compound or checkpoint set without copying.
 * Capacity is already reserved and ordered data is already removed. No fallible
 * work remains: replace earlier versions and leave the source with no buffers. */
void
ext4_transaction_take(struct ext4_transaction *set, struct ext4_transaction *transaction)
{
	struct ext4_fs *fs = transaction->journal->fs;
	struct ext4_transaction_entry *entry;
	uint32_t index;
	uint32_t position;

	for (index = 0; index < transaction->count; index++) {
		entry = &transaction->entries[index];
		position = ext4_transaction_find(set, entry->block);
		if (position == set->count) {
			set->entries[position].block = entry->block;
			set->entries[position].data = false;
			ext4_transaction_index(set, set->count++);
		} else {
			ext4_transaction_release(fs, &set->entries[position]);
		}
		set->entries[position].buffer = entry->buffer;
		set->entries[position].source_blocks = 0;
	}
	transaction->count = 0;
}

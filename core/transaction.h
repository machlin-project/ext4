/* SPDX-License-Identifier: BSD-3-Clause */
#ifndef MACHLIN_EXT4_TRANSACTION_H
#define MACHLIN_EXT4_TRANSACTION_H

#include "journal.h"

/* Private storage shared by transaction ownership and journal ordering.
 * Filesystem operations use the opaque journal.h transaction API. */
struct ext4_transaction_entry {
	uint64_t block;
	void *buffer;
	/* Complete blocks left in a private, immutable caller view, capped at the
	 * transaction capacity. Zero denotes owned storage. This bound also keeps
	 * the entry compact; retained journal snapshots always own their buffers. */
	uint32_t source_blocks;
	/* Regular-file data, which ordered data writes in place. */
	bool data;
};

/* Snapshots keep insertion order for logging. An open-addressed index of entry
 * positions, at least twice the credit bound, finds a block in constant time. */
struct ext4_transaction {
	struct ext4_journal *journal;
	uint32_t *slots;
	uint32_t slot_mask;
	uint32_t credits;
	/* Credits plus the quota reserve, usable only while commit updates quota. */
	uint32_t capacity;
	uint32_t count;
	uint32_t sequence;
	bool capacity_failed;
	bool quota_phase;
	/* Recovery conversions commit durably even under deferred commit. */
	bool recovery;
	/* Inode policy can strengthen the mount's deferred/ordered defaults. */
	bool synchronous;
	bool journal_data;
	/* A set the journal holds, the compound or the checkpoint set, rather than an
	 * operation's transaction. */
	bool held;
	/* Ranges this transaction freed, in freeing order; overflow journals all data. */
	bool freed_overflow;
	uint32_t freed_count;
	struct ext4_block_range *freed;
	/* Prepared private index deltas: removals first, then additions. */
	struct ext4_orphan_slot *orphan_changes;
	uint32_t orphan_removed;
	uint32_t orphan_added;
	bool orphan_touched;
	struct ext4_transaction_entry entries[];
};

struct ext4_transaction *ext4_transaction_allocate(struct ext4_journal *journal, uint32_t capacity);
enum ext4_result ext4_transaction_own(struct ext4_fs *fs, struct ext4_transaction_entry *entry);
void ext4_transaction_release(struct ext4_fs *fs, const struct ext4_transaction_entry *entry);
/* The caller already reserved capacity and made every retained buffer owned. */
void ext4_transaction_take(struct ext4_transaction *set, struct ext4_transaction *transaction);
enum ext4_result ext4_orphan_transaction_prepare(struct ext4_transaction *transaction);
void ext4_orphan_transaction_publish(struct ext4_transaction *transaction);
void ext4_orphan_transaction_cancel(struct ext4_transaction *transaction);

#endif

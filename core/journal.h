/* SPDX-License-Identifier: BSD-3-Clause */
#ifndef MACHLIN_EXT4_JOURNAL_H
#define MACHLIN_EXT4_JOURNAL_H

#include "internal.h"

#define EXT4_JBD_MAGIC 0xc03b3998U
#define EXT4_JBD_DESCRIPTOR 1U
#define EXT4_JBD_COMMIT 2U
#define EXT4_JBD_SUPER_V2 4U
#define EXT4_JBD_REVOKE 5U
#define EXT4_JBD_COMPAT_CHECKSUM 0x0001U
#define EXT4_JBD_REVOKE_FEATURE 0x0001U
#define EXT4_JBD_64BIT 0x0002U
#define EXT4_JBD_ASYNC_COMMIT 0x0004U
#define EXT4_JBD_CSUM_V2 0x0008U
#define EXT4_JBD_CSUM_V3 0x0010U
#define EXT4_JBD_FAST_COMMIT 0x0020U
#define EXT4_JBD_DEFAULT_FAST_BLOCKS 256U
#define EXT4_JBD_SUPPORTED                                                                         \
	(EXT4_JBD_REVOKE_FEATURE | EXT4_JBD_64BIT | EXT4_JBD_ASYNC_COMMIT | EXT4_JBD_CSUM_V2 |     \
	    EXT4_JBD_CSUM_V3 | EXT4_JBD_FAST_COMMIT)
#define EXT4_JBD_CRC32C 4U
#define EXT4_JBD_CRC32 1U
#define EXT4_JBD_ESCAPE 0x0001U
#define EXT4_JBD_SAME_UUID 0x0002U
#define EXT4_JBD_LAST_TAG 0x0008U
#define EXT4_JBD_TAG_FLAGS (EXT4_JBD_ESCAPE | EXT4_JBD_SAME_UUID | EXT4_JBD_LAST_TAG)
#define EXT4_JOURNAL_MAX_RUNS 1024U
#define EXT4_JOURNAL_MAX_BLOCKS (1U << 20)
#define EXT4_JOURNAL_MAX_MAPPING_BLOCKS 8192U
#define EXT4_TRANSACTION_MAX_BLOCKS 256U
/* Extra snapshots an ordinary transaction may use for quota files at commit:
 * each of three quota types can touch a few IDs and grow its tree. */
#define EXT4_QUOTA_CREDITS 48U
/* Recovery conversions may snapshot more blocks, bounded by the ordinary ring
 * and this budget for private snapshot buffers. */
#define EXT4_RECOVERY_TRANSACTION_BYTES (32U * 1024U * 1024U)
#define EXT4_RECOVERY_MAX_RECORDS (1U << 20)

struct ext4_be16 {
	uint8_t bytes[2];
};

struct ext4_be32 {
	uint8_t bytes[4];
};

struct ext4_jbd_header {
	struct ext4_be32 magic;
	struct ext4_be32 type;
	struct ext4_be32 sequence;
};

struct ext4_jbd_super {
	struct ext4_jbd_header header;
	struct ext4_be32 block_size;
	struct ext4_be32 max_length;
	struct ext4_be32 first;
	struct ext4_be32 sequence;
	struct ext4_be32 start;
	struct ext4_be32 error;
	struct ext4_be32 feature_compat;
	struct ext4_be32 feature_incompat;
	struct ext4_be32 feature_ro_compat;
	uint8_t uuid[EXT4_UUID_SIZE];
	struct ext4_be32 users;
	struct ext4_be32 dynamic_super;
	struct ext4_be32 max_transaction;
	struct ext4_be32 max_transaction_data;
	uint8_t checksum_type;
	uint8_t padding[3];
	struct ext4_be32 fast_commit_blocks;
	struct ext4_be32 head;
	struct ext4_be32 reserved[40];
	struct ext4_be32 checksum;
	uint8_t user_ids[16 * 48];
};

struct ext4_jbd_tag {
	struct ext4_be32 block_lo;
	struct ext4_be16 checksum;
	struct ext4_be16 flags;
};

struct ext4_jbd_tag3 {
	struct ext4_be32 block_lo;
	struct ext4_be32 flags;
	struct ext4_be32 block_hi;
	struct ext4_be32 checksum;
};

struct ext4_jbd_commit {
	struct ext4_jbd_header header;
	uint8_t checksum_type;
	uint8_t checksum_size;
	uint8_t padding[2];
	struct ext4_be32 checksum[8];
	struct ext4_be32 seconds_hi;
	struct ext4_be32 seconds_lo;
	struct ext4_be32 nanoseconds;
};

struct ext4_jbd_revoke {
	struct ext4_jbd_header header;
	struct ext4_be32 length;
};

struct ext4_journal_run {
	uint64_t physical;
	uint32_t logical;
	uint32_t length;
};

struct ext4_journal {
	struct ext4_fs *fs;
	struct ext4_write_environment writer;
	struct ext4_journal_environment external;
	struct ext4_journal_run *runs;
	uint64_t *mapping_blocks;
	uint8_t *super_buffer;
	uint8_t *work;
	uint8_t *data;
	uint32_t run_count;
	uint32_t mapping_count;
	uint32_t blocks;
	uint32_t super_block;
	uint32_t first;
	/* Exclusive ordinary ring limit; storage after it can belong to fast commit. */
	uint32_t last;
	uint32_t sequence;
	uint32_t start;
	uint32_t features;
	uint32_t checksum_seed;
	bool checksum;
	bool checksum_v1;
	bool aborted;
	bool transaction_active;
	/* A volume without a journal: commits write home directly, file data first,
	 * and the superblock's cleared valid state marks the volume in use instead of
	 * the recovery flag. There is no log. */
	bool direct;
	/* Deferred commit: operations completed since the last durable commit, at
	 * most compound_blocks snapshots. Zero commits every operation durably. */
	struct ext4_transaction *compound;
	uint32_t compound_blocks;
	/* Ordered data: regular-file data is written in place before its commit.
	 * Blocks freed since the last durable commit may still belong to their old
	 * owner after a power cut, so data reusing them stays journaled; when this
	 * sorted set overflows, all data does until the next durable commit. */
	bool ordered_data;
	bool freed_overflow;
	struct ext4_block_range *freed;
	size_t freed_count;
	/* Lazy checkpointing: committed transactions stay in the log from first to
	 * head, and the latest committed version of each of their blocks, at most
	 * checkpoint_blocks, stays here until a checkpoint writes it home. Zero
	 * checkpoints every commit before the next transaction is logged. */
	struct ext4_transaction *checkpoint;
	uint32_t checkpoint_blocks;
	uint32_t head;
};

struct ext4_transaction;

uint16_t ext4_be16(const struct ext4_be16 *value);
uint32_t ext4_be32(const struct ext4_be32 *value);
void ext4_encode_be16(struct ext4_be16 *output, uint16_t value);
void ext4_encode_be32(struct ext4_be32 *output, uint32_t value);

/* Internal ownership boundary. The filesystem owner serializes the journal,
 * holds every affected metadata lock until commit completes, and prevents reads
 * of partially checkpointed home blocks. These are not public block-write APIs.
 * No allocation or ordered file-data I/O may be deferred past commit's barrier.
 * A failed commit poisons this instance; destroy it and recover a fresh resource.
 * close only frees memory. finish performs the clean-volume durability sequence. */
enum ext4_result ext4_journal_open(
    struct ext4_fs *fs, const struct ext4_write_environment *writer, struct ext4_journal **result);
enum ext4_result ext4_journal_open_external(struct ext4_fs *fs,
    const struct ext4_write_environment *writer, const struct ext4_journal_environment *external,
    struct ext4_journal **result);
/* Transactions for a volume without a journal, written home directly. */
enum ext4_result ext4_journal_open_direct(
    struct ext4_fs *fs, const struct ext4_write_environment *writer, struct ext4_journal **result);
void ext4_journal_close(struct ext4_journal *journal);
enum ext4_result ext4_journal_finish(struct ext4_journal *journal);
enum ext4_result ext4_transaction_begin(
    struct ext4_journal *journal, uint32_t credits, struct ext4_transaction **result);
/* The ordinary committed prefix must already be checkpointed and flushed.
 * Keep its on-disk recovery authority until commit publishes this sequence. */
enum ext4_result ext4_transaction_begin_recovery(struct ext4_journal *journal, uint32_t sequence,
    uint32_t credits, struct ext4_transaction **result);
enum ext4_result ext4_transaction_buffer(
    struct ext4_transaction *transaction, uint64_t block, void **result);
/* Snapshot a regular file's data block, blank or with its current contents. Under
 * ordered data it is written in place before the commit instead of logged. */
enum ext4_result ext4_transaction_data(
    struct ext4_transaction *transaction, uint64_t block, bool blank, void **result);
/* Private data storage with unspecified contents, including an earlier snapshot
 * if already enrolled. The caller must fill the whole block before committing,
 * or cancel the transaction on failure. No old-data read or clearing is needed. */
enum ext4_result ext4_transaction_data_replace(
    struct ext4_transaction *transaction, uint64_t block, void **result);
/* Stage one full data block from an immutable caller range. available bounds
 * that range from source, permitting contiguous ordered writes. Keep it valid
 * until commit/cancel returns. Commit owns a copy before retaining a journaled
 * block; requesting a mutable snapshot detaches it before returning storage. */
enum ext4_result ext4_transaction_data_source(
    struct ext4_transaction *transaction, uint64_t block, const void *source, size_t available);
/* Record blocks this transaction frees for ordered data's reuse rule. */
void ext4_transaction_freed(struct ext4_transaction *transaction, uint64_t block, uint64_t length);
/* Enroll a block whose previous contents the caller replaces completely: a new
 * snapshot starts zeroed without reading the device. An enrolled block is returned
 * unchanged. */
enum ext4_result ext4_transaction_buffer_blank(
    struct ext4_transaction *transaction, uint64_t block, void **result);
/* A snapshot request exhausted this transaction's credits. Inspect before cancel;
 * other RANGE failures do not authorize retrying a smaller operation. */
bool ext4_transaction_capacity_failed(const struct ext4_transaction *transaction);
/* Allocation accounting owns this special snapshot. Commit always retains the
 * recovery bit and recalculates its checksum before logging/checkpointing it. */
enum ext4_result ext4_transaction_super(
    struct ext4_transaction *transaction, struct ext4_super_disk **result);
/* Read the transaction's current view, falling back to the live home block.
 * Merely inspecting a block does not consume a journal credit. */
enum ext4_result ext4_transaction_read(
    struct ext4_transaction *transaction, uint64_t block, void *buffer);
/* Commit-time inspection of the private snapshots, in enrollment order. */
struct ext4_fs *ext4_transaction_fs(const struct ext4_transaction *transaction);
uint32_t ext4_transaction_count(const struct ext4_transaction *transaction);
void ext4_transaction_entry(const struct ext4_transaction *transaction, uint32_t index,
    uint64_t *block, const void **buffer);
/* The transaction's snapshot of block, or NULL without enrolling it. */
const void *ext4_transaction_peek(const struct ext4_transaction *transaction, uint64_t block);
uint32_t ext4_journal_credits(const struct ext4_journal *journal);
/* Credit bound for begin_recovery conversions; never below the ordinary bound. */
uint32_t ext4_journal_recovery_credits(const struct ext4_journal *journal);
/* Both commit and cancel consume the transaction and release all snapshots. */
enum ext4_result ext4_transaction_commit(struct ext4_transaction *transaction);
/* Quota or memory refusal before this operation's writes leaves its owner usable.
 * The same status from a write/flush callback aborts the journal and is fatal. */
bool ext4_commit_rejected(const struct ext4_journal *journal, enum ext4_result error);
/* With deferred commit, make every merged operation durable as one transaction and
 * checkpoint it. Without pending operations it writes nothing. */
enum ext4_result ext4_journal_commit(struct ext4_journal *journal);
/* Read the live view: pending operations, then committed snapshots, then home.
 * ext4_device_read validates the byte range and aborted state before entry. */
enum ext4_result ext4_journal_read_current(
    const struct ext4_journal *journal, uint64_t offset, void *buffer, size_t length);
/* Leading blocks of [block, block + count) whose current contents are at home on
 * the device rather than only in the journal's memory. */
uint64_t ext4_journal_home_prefix(
    const struct ext4_journal *journal, uint64_t block, uint64_t count);
/* The largest compound transaction the log and recovery memory bound admit. */
uint32_t ext4_journal_compound_limit(const struct ext4_journal *journal);
/* The largest checkpoint set: what the log can hold and the recovery memory bound. */
uint32_t ext4_journal_checkpoint_limit(const struct ext4_journal *journal);
/* Write every committed block home and empty the log. Without committed blocks it
 * writes nothing. A failure poisons the journal. */
enum ext4_result ext4_journal_checkpoint(struct ext4_journal *journal);
void ext4_transaction_cancel(struct ext4_transaction *transaction);

/* Shared journal/recovery implementation, never exported to platform adapters. */
enum ext4_result ext4_journal_load(
    struct ext4_fs *fs, const struct ext4_write_environment *writer, struct ext4_journal **result);
enum ext4_result ext4_journal_load_external(struct ext4_fs *fs,
    const struct ext4_write_environment *writer, const struct ext4_journal_environment *external,
    struct ext4_journal **result);
enum ext4_result ext4_journal_read(struct ext4_journal *journal, uint32_t block, void *buffer);
enum ext4_result ext4_journal_write_home(
    struct ext4_journal *journal, uint64_t block, const void *buffer);
enum ext4_result ext4_journal_flush(struct ext4_journal *journal);
enum ext4_result ext4_journal_reset(struct ext4_journal *journal, uint32_t sequence);
enum ext4_result ext4_journal_set_recovery(struct ext4_journal *journal, bool recovery);
bool ext4_journal_target(const struct ext4_journal *journal, uint64_t block);
uint32_t ext4_journal_next(const struct ext4_journal *journal, uint32_t block);
size_t ext4_journal_tag_size(const struct ext4_journal *journal);
uint32_t ext4_journal_data_checksum(
    const struct ext4_journal *journal, uint32_t sequence, const void *buffer);
bool ext4_journal_checksum_valid(struct ext4_journal *journal, void *buffer, size_t offset);
void ext4_journal_checksum_set(struct ext4_journal *journal, void *buffer, size_t offset);

_Static_assert(sizeof(struct ext4_jbd_header) == 12, "journal header size");
_Static_assert(sizeof(struct ext4_jbd_super) == EXT4_SUPER_SIZE, "journal superblock size");
_Static_assert(offsetof(struct ext4_jbd_super, checksum) == 0xfc, "journal checksum position");
_Static_assert(sizeof(struct ext4_jbd_tag) == 8, "journal tag size");
_Static_assert(sizeof(struct ext4_jbd_tag3) == 16, "journal checksum-v3 tag size");
_Static_assert(sizeof(struct ext4_jbd_commit) == 60, "journal commit header size");

#endif

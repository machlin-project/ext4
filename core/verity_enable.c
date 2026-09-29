/* SPDX-License-Identifier: BSD-3-Clause */
#include "allocate.h"
#include "inline.h"
#include "journal.h"
#include "verity.h"

/* Enabling fs-verity follows Linux's ext4 sequence. A first transaction converts
 * inline data, trims blocks past EOF, because readers find the descriptor from the
 * last mapped block, and puts the inode on the orphan list. Bounded transactions then
 * write the Merkle tree past EOF as its blocks complete, level by level, without
 * changing the size or times. A final transaction writes the descriptor and its size,
 * sets the verity flag and removes the inode from the list. Recovery truncates an
 * interrupted attempt back to its size, as the rollback of a failed attempt does. */

/* Completed Merkle blocks wait in memory for a write transaction, bounded in bytes. */
#define EXT4_VERITY_QUEUE_BYTES (128U * 1024U)

struct ext4_verity_builder {
	struct ext4_fs *fs;
	uint32_t number;
	uint32_t generation;
	struct ext4_inode inode;
	struct ext4_verity verity;
	struct ext4_verity_descriptor_disk descriptor;
	uint64_t tree_blocks;
	uint64_t level_blocks[EXT4_VERITY_MAX_LEVELS];
	/* One partially filled hash block per level, and the blocks each level wrote. */
	uint8_t *pending;
	size_t pending_bytes;
	uint32_t filled[EXT4_VERITY_MAX_LEVELS];
	uint64_t emitted[EXT4_VERITY_MAX_LEVELS];
	/* Completed Merkle blocks and their file offsets, awaiting a transaction. */
	uint8_t *queue;
	uint64_t *offsets;
	uint32_t queue_capacity;
	uint32_t queued;
	uint32_t batch;
	uint8_t *data;
	uint8_t digest[EXT4_VERITY_MAX_DIGEST];
};

static enum ext4_result
ext4_verity_parameters_valid(
    struct ext4_fs *fs, const struct ext4_verity_parameters *parameters, uint8_t *log_block_size)
{
	uint8_t log = 0;

	if (parameters == NULL ||
	    (parameters->hash_algorithm != EXT4_VERITY_HASH_SHA256 &&
		parameters->hash_algorithm != EXT4_VERITY_HASH_SHA512) ||
	    parameters->salt_size > EXT4_VERITY_MAX_SALT ||
	    (parameters->salt == NULL && parameters->salt_size != 0) ||
	    parameters->block_size == 0 ||
	    (parameters->block_size & (parameters->block_size - 1U)) != 0 ||
	    parameters->block_size > fs->info.block_size) {
		return EXT4_INVALID_ARGUMENT;
	}
	while ((1U << log) < parameters->block_size) {
		log++;
	}
	if (log < EXT4_VERITY_MIN_LOG_BLOCK || log > EXT4_VERITY_MAX_LOG_BLOCK) {
		return EXT4_INVALID_ARGUMENT;
	}
	*log_block_size = log;
	return EXT4_OK;
}

/* Begin a transaction on the inode while it is on the orphan list. */
static enum ext4_result
ext4_verity_step_begin(struct ext4_verity_builder *builder, struct ext4_transaction **transaction,
    struct ext4_allocation *allocation, struct ext4_inode_disk **disk)
{
	struct ext4_fs *fs = builder->fs;
	void *buffer = NULL;
	uint64_t offset;
	bool mapped = false;
	enum ext4_result error;

	error = ext4_transaction_begin(fs->journal, ext4_journal_credits(fs->journal), transaction);
	if (error != EXT4_OK) {
		return error;
	}
	error = ext4_inode_location(fs, builder->number, &offset);
	if (error == EXT4_OK) {
		error =
		    ext4_transaction_buffer(*transaction, offset / fs->info.block_size, &buffer);
	}
	if (error == EXT4_OK) {
		*disk =
		    (struct ext4_inode_disk *)((uint8_t *)buffer + offset % fs->info.block_size);
		error = ext4_orphan_record(fs, builder->number, *disk, &builder->inode, &mapped);
	}
	if (error == EXT4_OK &&
	    (builder->inode.generation != builder->generation || builder->inode.links == 0 ||
		!mapped || !(builder->inode.flags & EXT4_INODE_EXTENTS))) {
		error = EXT4_CORRUPT;
	}
	if (error == EXT4_OK) {
		error = ext4_allocation_init(allocation, fs, *transaction, &builder->inode);
	}
	if (error != EXT4_OK) {
		ext4_transaction_cancel(*transaction);
	}
	return error;
}

/* Account the step's allocation without changing the size, then commit it. */
static enum ext4_result
ext4_verity_step_commit(struct ext4_verity_builder *builder, struct ext4_transaction *transaction,
    struct ext4_allocation *allocation, struct ext4_inode_disk *disk)
{
	struct ext4_fs *fs = builder->fs;
	enum ext4_result error;

	error = ext4_inode_account(allocation, &builder->inode, disk, builder->inode.size);
	if (error == EXT4_OK) {
		ext4_inode_checksum_set(fs, builder->number, disk);
		error = ext4_inode_decode_orphan(fs, builder->number, disk, &builder->inode);
	}
	ext4_allocation_destroy(allocation);
	if (error != EXT4_OK) {
		ext4_transaction_cancel(transaction);
		return error;
	}
	error = ext4_transaction_commit(transaction);
	if (error != EXT4_OK && !ext4_commit_rejected(error)) {
		fs->aborted = true;
	}
	return error;
}

/* Convert inline data, trim a first batch of blocks past EOF and put the inode on
 * the orphan list, all in one transaction. */
static enum ext4_result
ext4_verity_prepare(struct ext4_verity_builder *builder, bool *trimmed)
{
	struct ext4_fs *fs = builder->fs;
	struct ext4_transaction *transaction;
	struct ext4_allocation allocation;
	struct ext4_inode_disk *disk;
	struct ext4_inode *inode = &builder->inode;
	uint32_t first;
	bool ready = false;
	enum ext4_result error;

	error =
	    ext4_transaction_begin(fs->journal, ext4_journal_credits(fs->journal), &transaction);
	if (error != EXT4_OK) {
		return error;
	}
	error =
	    ext4_edit_inode(fs, transaction, builder->number, builder->generation, &disk, inode);
	if (error == EXT4_OK && (inode->mode & EXT4_MODE_TYPE) != EXT4_MODE_REGULAR) {
		error = (inode->mode & EXT4_MODE_TYPE) == EXT4_MODE_DIRECTORY
		    ? EXT4_IS_DIRECTORY
		    : EXT4_INVALID_ARGUMENT;
	}
	if (error == EXT4_OK && (inode->flags & EXT4_INODE_VERITY)) {
		error = EXT4_EXISTS;
	}
	if (error == EXT4_OK && (inode->flags & EXT4_INODE_ENCRYPT)) {
		error = EXT4_ENCRYPTED;
	}
	if (error == EXT4_OK && (inode->flags & EXT4_INODE_RESTRICTED_FLAGS)) {
		error = EXT4_PERMISSION_DENIED;
	}
	/* An unlinked file is already on the orphan list for its reclamation. */
	if (error == EXT4_OK && inode->links == 0) {
		error = EXT4_INVALID_ARGUMENT;
	}
	if (error == EXT4_OK) {
		error = ext4_allocation_init(&allocation, fs, transaction, inode);
		ready = error == EXT4_OK;
	}
	if (error == EXT4_OK) {
		error = ext4_inline_expand(&allocation, inode, disk);
	}
	if (error == EXT4_OK && !(inode->flags & EXT4_INODE_EXTENTS)) {
		error = EXT4_UNSUPPORTED;
	}
	if (error == EXT4_OK) {
		error = ext4_write_map_validate(&allocation, inode, disk);
	}
	first = (uint32_t)((inode->size + fs->info.block_size - 1U) / fs->info.block_size);
	if (error == EXT4_OK) {
		error = ext4_write_map_trim(
		    &allocation, inode, disk, first, EXT4_ORPHAN_BATCH_BLOCKS, trimmed);
	}
	if (error == EXT4_OK) {
		error = ext4_orphan_link(&allocation, builder->number, disk);
	}
	if (error != EXT4_OK) {
		if (ready) {
			ext4_allocation_destroy(&allocation);
		}
		ext4_transaction_cancel(transaction);
		return error;
	}
	error = ext4_verity_step_commit(builder, transaction, &allocation, disk);
	if (error == EXT4_OK) {
		fs->last_orphan = builder->number;
	}
	return error;
}

/* Remove the rest of the blocks past EOF in bounded steps. */
static enum ext4_result
ext4_verity_trim(struct ext4_verity_builder *builder)
{
	struct ext4_fs *fs = builder->fs;
	struct ext4_transaction *transaction;
	struct ext4_allocation allocation;
	struct ext4_inode_disk *disk;
	uint32_t first;
	uint32_t limit = EXT4_ORPHAN_BATCH_BLOCKS;
	bool done = false;
	bool capacity;
	enum ext4_result error;

	while (!done) {
		error = ext4_verity_step_begin(builder, &transaction, &allocation, &disk);
		if (error != EXT4_OK) {
			return error;
		}
		first = (uint32_t)((builder->inode.size + fs->info.block_size - 1U) /
		    fs->info.block_size);
		error =
		    ext4_write_map_trim(&allocation, &builder->inode, disk, first, limit, &done);
		if (error == EXT4_OK && !done && allocation.freed == 0 &&
		    allocation.unmapped == 0) {
			error = EXT4_CORRUPT;
		}
		if (error != EXT4_OK) {
			capacity = ext4_transaction_capacity_failed(transaction);
			ext4_allocation_destroy(&allocation);
			ext4_transaction_cancel(transaction);
			if (error == EXT4_RANGE && capacity && limit > 1U) {
				limit /= 2U;
				continue;
			}
			return error;
		}
		error = ext4_verity_step_commit(builder, transaction, &allocation, disk);
		if (error != EXT4_OK) {
			return error;
		}
	}
	return EXT4_OK;
}

/* Write queued Merkle blocks [first, first + count) past EOF in one transaction. */
static enum ext4_result
ext4_verity_write_step(
    struct ext4_verity_builder *builder, uint32_t first, uint32_t count, bool *capacity)
{
	struct ext4_fs *fs = builder->fs;
	struct ext4_transaction *transaction;
	struct ext4_allocation allocation;
	struct ext4_inode_disk *disk;
	void *snapshot = NULL;
	uint64_t logical;
	uint64_t physical;
	uint32_t index;
	uint32_t within;
	bool zero;
	enum ext4_result error;

	*capacity = false;
	error = ext4_verity_step_begin(builder, &transaction, &allocation, &disk);
	if (error != EXT4_OK) {
		return error;
	}
	for (index = first; error == EXT4_OK && index < first + count; index++) {
		logical = builder->offsets[index] / fs->info.block_size;
		within = (uint32_t)(builder->offsets[index] % fs->info.block_size);
		error = ext4_write_map_allocate(
		    &allocation, &builder->inode, disk, (uint32_t)logical, &physical, &zero);
		if (error == EXT4_OK) {
			error = ext4_allocation_valid(&allocation, physical);
		}
		/* A new block starts zeroed; Merkle blocks smaller than a filesystem
		 * block fill it over several steps. */
		if (error == EXT4_OK) {
			error = ext4_transaction_data(transaction, physical,
			    zero || builder->verity.block_size == fs->info.block_size, &snapshot);
		}
		if (error == EXT4_OK) {
			ext4_copy((uint8_t *)snapshot + within,
			    builder->queue + (size_t)index * builder->verity.block_size,
			    builder->verity.block_size);
		}
	}
	if (error != EXT4_OK) {
		*capacity = ext4_transaction_capacity_failed(transaction);
		ext4_allocation_destroy(&allocation);
		ext4_transaction_cancel(transaction);
		return error;
	}
	return ext4_verity_step_commit(builder, transaction, &allocation, disk);
}

/* Write the queued Merkle blocks, halving a batch that exceeds the journal's
 * transaction capacity. */
static enum ext4_result
ext4_verity_flush(struct ext4_verity_builder *builder)
{
	uint32_t first = 0;
	uint32_t count;
	bool capacity;
	enum ext4_result error;

	while (first < builder->queued) {
		count = builder->queued - first;
		if (count > builder->batch) {
			count = builder->batch;
		}
		error = ext4_verity_write_step(builder, first, count, &capacity);
		if (error == EXT4_RANGE && capacity && count > 1U && !builder->fs->aborted) {
			builder->batch = count / 2U;
			continue;
		}
		if (error != EXT4_OK) {
			return error;
		}
		first += count;
	}
	builder->queued = 0;
	return EXT4_OK;
}

/* Zero-pad and queue the pending block of level, and hash it into digest. */
static enum ext4_result
ext4_verity_complete(struct ext4_verity_builder *builder, unsigned int level)
{
	struct ext4_verity *verity = &builder->verity;
	uint8_t *block = builder->pending + (size_t)level * verity->block_size;
	enum ext4_result error;

	ext4_zero(block + builder->filled[level], verity->block_size - builder->filled[level]);
	if (builder->queued == builder->queue_capacity) {
		error = ext4_verity_flush(builder);
		if (error != EXT4_OK) {
			return error;
		}
	}
	ext4_copy(builder->queue + (size_t)builder->queued * verity->block_size, block,
	    verity->block_size);
	builder->offsets[builder->queued++] = verity->tree_offset +
	    (verity->level_start[level] + builder->emitted[level]) * verity->block_size;
	builder->emitted[level]++;
	builder->filled[level] = 0;
	ext4_verity_hash(verity, block, builder->digest);
	return EXT4_OK;
}

/* Add digest, the hash of a block below level, completing full blocks upward. A
 * hash above the top level is the root hash. */
static enum ext4_result
ext4_verity_push(struct ext4_verity_builder *builder, unsigned int level)
{
	struct ext4_verity *verity = &builder->verity;
	enum ext4_result error;

	while (level < verity->levels) {
		ext4_copy(
		    builder->pending + (size_t)level * verity->block_size + builder->filled[level],
		    builder->digest, verity->digest_size);
		builder->filled[level] += verity->digest_size;
		if (builder->filled[level] < verity->hashes_per_block * verity->digest_size) {
			return EXT4_OK;
		}
		error = ext4_verity_complete(builder, level);
		if (error != EXT4_OK) {
			return error;
		}
		level++;
	}
	ext4_copy(verity->root_hash, builder->digest, verity->digest_size);
	return EXT4_OK;
}

/* Hash every data block, zero-padded to the Merkle block size, and complete the
 * partial blocks of each level from the data upward. */
static enum ext4_result
ext4_verity_build(struct ext4_verity_builder *builder)
{
	struct ext4_verity *verity = &builder->verity;
	uint64_t offset;
	size_t valid;
	size_t read;
	unsigned int level;
	enum ext4_result error;

	for (offset = 0; offset < verity->data_size; offset += verity->block_size) {
		valid = verity->data_size - offset < verity->block_size
		    ? (size_t)(verity->data_size - offset)
		    : verity->block_size;
		error = ext4_read_mapped(
		    builder->fs, &builder->inode, offset, builder->data, valid, false, &read);
		if (error != EXT4_OK) {
			return error;
		}
		ext4_zero(builder->data + valid, verity->block_size - valid);
		ext4_verity_hash(verity, builder->data, builder->digest);
		error = ext4_verity_push(builder, 0);
		if (error != EXT4_OK) {
			return error;
		}
	}
	for (level = 0; level < verity->levels; level++) {
		if (builder->filled[level] == 0) {
			continue;
		}
		error = ext4_verity_complete(builder, level);
		if (error == EXT4_OK) {
			error = ext4_verity_push(builder, level + 1U);
		}
		if (error != EXT4_OK) {
			return error;
		}
	}
	for (level = 0; level < verity->levels; level++) {
		if (builder->emitted[level] != builder->level_blocks[level]) {
			return EXT4_CORRUPT;
		}
	}
	return ext4_verity_flush(builder);
}

/* Write the descriptor and its size in the block after the tree, set the verity
 * flag and leave the orphan list in one transaction. */
static enum ext4_result
ext4_verity_finish(struct ext4_verity_builder *builder, struct ext4_inode *result)
{
	struct ext4_fs *fs = builder->fs;
	struct ext4_transaction *transaction;
	struct ext4_allocation allocation;
	struct ext4_inode_disk *disk;
	struct ext4_le32 size;
	struct ext4_inode inode = { 0 };
	void *snapshot = NULL;
	uint64_t position;
	uint64_t physical;
	uint32_t last_orphan = 0;
	bool zero;
	enum ext4_result error;

	position = builder->verity.tree_offset + builder->tree_blocks * builder->verity.block_size;
	position = (position + fs->info.block_size - 1U) / fs->info.block_size;
	error = ext4_verity_step_begin(builder, &transaction, &allocation, &disk);
	if (error != EXT4_OK) {
		return error;
	}
	error = ext4_write_map_allocate(
	    &allocation, &builder->inode, disk, (uint32_t)position, &physical, &zero);
	if (error == EXT4_OK) {
		error = ext4_allocation_valid(&allocation, physical);
	}
	if (error == EXT4_OK) {
		error = ext4_transaction_data(transaction, physical, true, &snapshot);
	}
	if (error == EXT4_OK) {
		/* Linux records the size in the last four bytes of the block that holds the
		 * end of the descriptor and room for the size. */
		ext4_zero(snapshot, fs->info.block_size);
		ext4_copy(snapshot, &builder->descriptor, sizeof(builder->descriptor));
		ext4_encode32(&size, sizeof(builder->descriptor));
		ext4_copy(
		    (uint8_t *)snapshot + fs->info.block_size - sizeof(size), &size, sizeof(size));
		error = ext4_orphan_unlink(&allocation, builder->number, disk, &last_orphan);
	}
	if (error == EXT4_OK) {
		builder->inode.flags |= EXT4_INODE_VERITY;
		error = ext4_inode_account(&allocation, &builder->inode, disk, builder->inode.size);
	}
	if (error == EXT4_OK) {
		ext4_inode_checksum_set(fs, builder->number, disk);
		error = ext4_inode_decode_live(fs, builder->number, disk, &inode);
	}
	ext4_allocation_destroy(&allocation);
	if (error != EXT4_OK) {
		ext4_transaction_cancel(transaction);
		return error;
	}
	error = ext4_transaction_commit(transaction);
	if (error != EXT4_OK) {
		if (!ext4_commit_rejected(error)) {
			fs->aborted = true;
		}
		return error;
	}
	fs->last_orphan = last_orphan;
	*result = inode;
	return EXT4_OK;
}

/* Derive the tree geometry and the descriptor, and allocate the builder's buffers. */
static enum ext4_result
ext4_verity_setup(struct ext4_verity_builder *builder,
    const struct ext4_verity_parameters *parameters, uint8_t log_block_size)
{
	struct ext4_fs *fs = builder->fs;
	struct ext4_verity *verity = &builder->verity;
	uint64_t level_end;
	uint64_t end;
	unsigned int level;
	enum ext4_result error;

	error = ext4_verity_configure(verity, (uint8_t)parameters->hash_algorithm, log_block_size,
	    parameters->salt, (uint8_t)parameters->salt_size);
	if (error != EXT4_OK) {
		return error;
	}
	verity->data_size = builder->inode.size;
	error = ext4_verity_geometry(verity, &builder->tree_blocks);
	if (error != EXT4_OK) {
		return EXT4_RANGE;
	}
	for (level = 0; level < verity->levels; level++) {
		level_end = level == 0 ? builder->tree_blocks : verity->level_start[level - 1U];
		builder->level_blocks[level] = level_end - verity->level_start[level];
	}
	if (builder->inode.size > UINT64_MAX - EXT4_VERITY_METADATA_ALIGNMENT) {
		return EXT4_RANGE;
	}
	verity->tree_offset = (builder->inode.size + EXT4_VERITY_METADATA_ALIGNMENT - 1U) &
	    ~(uint64_t)(EXT4_VERITY_METADATA_ALIGNMENT - 1U);
	/* The descriptor's block must have a logical block number. */
	end = verity->tree_offset / fs->info.block_size;
	if (builder->tree_blocks >
	    (UINT32_MAX - 1U - end) * fs->info.block_size / verity->block_size) {
		return EXT4_RANGE;
	}
	builder->queue_capacity = EXT4_VERITY_QUEUE_BYTES / verity->block_size;
	if (builder->queue_capacity == 0) {
		builder->queue_capacity = 1;
	}
	builder->batch = builder->queue_capacity;
	builder->pending_bytes =
	    (size_t)(verity->levels == 0 ? 1U : verity->levels) * verity->block_size;
	builder->pending =
	    fs->environment.allocate(fs->environment.context, builder->pending_bytes);
	builder->queue = fs->environment.allocate(
	    fs->environment.context, (size_t)builder->queue_capacity * verity->block_size);
	builder->offsets = fs->environment.allocate(
	    fs->environment.context, (size_t)builder->queue_capacity * sizeof(*builder->offsets));
	builder->data = fs->environment.allocate(fs->environment.context, verity->block_size);
	if (builder->pending == NULL || builder->queue == NULL || builder->offsets == NULL ||
	    builder->data == NULL) {
		return EXT4_NO_MEMORY;
	}
	return EXT4_OK;
}

/* The descriptor Linux writes: no signature, the root hash zero-padded. */
static void
ext4_verity_describe(struct ext4_verity_builder *builder,
    const struct ext4_verity_parameters *parameters, uint8_t log_block_size)
{
	struct ext4_verity_descriptor_disk *descriptor = &builder->descriptor;
	struct ext4_verity *verity = &builder->verity;

	ext4_zero(descriptor, sizeof(*descriptor));
	descriptor->version = EXT4_VERITY_VERSION;
	descriptor->hash_algorithm = verity->algorithm;
	descriptor->log_block_size = log_block_size;
	descriptor->salt_size = (uint8_t)parameters->salt_size;
	ext4_encode32(&descriptor->data_size_lo, (uint32_t)verity->data_size);
	ext4_encode32(&descriptor->data_size_hi, (uint32_t)(verity->data_size >> 32));
	ext4_copy(descriptor->root_hash, verity->root_hash, verity->digest_size);
	if (parameters->salt_size != 0) {
		ext4_copy(descriptor->salt, parameters->salt, parameters->salt_size);
	}
}

static void
ext4_verity_release(struct ext4_verity_builder *builder)
{
	struct ext4_fs *fs = builder->fs;
	uint32_t block_size = builder->verity.block_size;

	if (builder->pending != NULL) {
		fs->environment.release(
		    fs->environment.context, builder->pending, builder->pending_bytes);
	}
	if (builder->queue != NULL) {
		fs->environment.release(fs->environment.context, builder->queue,
		    (size_t)builder->queue_capacity * block_size);
	}
	if (builder->offsets != NULL) {
		fs->environment.release(fs->environment.context, builder->offsets,
		    (size_t)builder->queue_capacity * sizeof(*builder->offsets));
	}
	if (builder->data != NULL) {
		fs->environment.release(fs->environment.context, builder->data, block_size);
	}
	fs->environment.release(fs->environment.context, builder, sizeof(*builder));
}

enum ext4_result
ext4_enable_verity(struct ext4_fs *fs, uint32_t number, uint32_t generation,
    const struct ext4_verity_parameters *parameters, struct ext4_inode *result)
{
	struct ext4_verity_builder *builder;
	uint8_t log_block_size = 0;
	bool trimmed = false;
	bool listed = false;
	enum ext4_result error;
	enum ext4_result rollback;

	if (fs == NULL || result == NULL) {
		return EXT4_INVALID_ARGUMENT;
	}
	if (fs->aborted) {
		return EXT4_RECOVERY_REQUIRED;
	}
	if (fs->journal == NULL) {
		return EXT4_READ_ONLY;
	}
	error = ext4_verity_parameters_valid(fs, parameters, &log_block_size);
	if (error != EXT4_OK) {
		return error;
	}
	if (!(fs->info.feature_ro_compat & EXT4_FEATURE_RO_VERITY)) {
		return EXT4_UNSUPPORTED;
	}
	builder = fs->environment.allocate(fs->environment.context, sizeof(*builder));
	if (builder == NULL) {
		return EXT4_NO_MEMORY;
	}
	ext4_zero(builder, sizeof(*builder));
	builder->fs = fs;
	builder->number = number;
	builder->generation = generation;
	error = ext4_verity_prepare(builder, &trimmed);
	listed = error == EXT4_OK;
	if (error == EXT4_OK) {
		error = ext4_verity_setup(builder, parameters, log_block_size);
	}
	if (error == EXT4_OK && !trimmed) {
		error = ext4_verity_trim(builder);
	}
	if (error == EXT4_OK) {
		error = ext4_verity_build(builder);
	}
	if (error == EXT4_OK) {
		ext4_verity_describe(builder, parameters, log_block_size);
		error = ext4_verity_finish(builder, result);
	}
	/* Truncate the partial tree and leave the orphan list, as recovery would. */
	if (error != EXT4_OK && listed && !fs->aborted) {
		rollback = ext4_orphan_finish_inode(fs, number, generation, false);
		if (rollback != EXT4_OK) {
			fs->aborted = true;
		}
	}
	ext4_verity_release(builder);
	return error;
}

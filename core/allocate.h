/* SPDX-License-Identifier: BSD-3-Clause */
#ifndef MACHLIN_EXT4_ALLOCATE_H
#define MACHLIN_EXT4_ALLOCATE_H

#include "journal.h"

#define EXT4_ORPHAN_BATCH_BLOCKS 32U

/* Live mutations must leave enough journal room for one bounded reclamation
 * step, including an external attribute release and a nonhead predecessor. */
enum ext4_result ext4_orphan_reserve(
    struct ext4_fs *fs, const struct ext4_inode *inode, const struct ext4_inode_disk *disk);

/* Private transaction state. Counters become visible in fs->info only after a
 * successful commit. The caller owns all snapshots through that commit. */
struct ext4_allocation {
	struct ext4_fs *fs;
	struct ext4_transaction *transaction;
	struct ext4_super_disk *super;
	struct ext4_group_disk *descriptor;
	struct ext4_group group;
	uint8_t *bitmap;
	uint8_t *scratch;
	/* A verified committed block bitmap reused while validating ranges in the
	 * same group; a bitmap already in the transaction is read in place. */
	uint8_t *validated;
	uint64_t validated_bitmap;
	uint32_t validated_group;
	uint64_t free_blocks;
	uint64_t reserved_blocks;
	uint64_t maximum_block;
	/* Size published by this transaction, when it extends the inode. */
	uint64_t mapping_size;
	uint32_t free_inodes;
	uint32_t group_index;
	uint32_t next_bit;
	uint32_t allocated;
	uint64_t freed;
	/* Removing a mapping can retain a cluster referenced by another extent. */
	uint64_t unmapped;
	/* Dropping a shared external attribute reference changes i_blocks without
	 * freeing that physical block. Keep it separate from allocation counters. */
	uint64_t detached_shared_blocks;
	/* Value-inode references charge rounded logical value blocks to each
	 * owning inode independently of physical allocation or sharing. */
	uint64_t attribute_blocks_added;
	uint64_t attribute_blocks_removed;
	/* Offline semantic replay excludes every logged data range from new
	 * metadata allocation until the complete prefix has been materialized.
	 * This immutable set is sorted and disjoint, with adjacent ranges merged. */
	const struct ext4_block_range *excluded;
	size_t excluded_count;
};

enum ext4_result ext4_allocation_init(struct ext4_allocation *allocation, struct ext4_fs *fs,
    struct ext4_transaction *transaction, const struct ext4_inode *inode);
void ext4_allocation_destroy(struct ext4_allocation *allocation);
/* Remove a previously validated modern orphan slot in the caller's private
 * transaction. Publish the pending-count decrement only after commit. */
enum ext4_result ext4_orphan_file_remove(
    struct ext4_allocation *allocation, uint32_t number, bool *removed);
enum ext4_result ext4_allocate_block(struct ext4_allocation *allocation, uint64_t *block);
enum ext4_result ext4_allocation_valid(struct ext4_allocation *allocation, uint64_t block);
enum ext4_result ext4_allocation_valid_range(
    struct ext4_allocation *allocation, uint64_t block, uint64_t length);
enum ext4_result ext4_free_blocks(
    struct ext4_allocation *allocation, uint64_t block, uint64_t length);
enum ext4_result ext4_cluster_allocate(struct ext4_allocation *allocation,
    const struct ext4_inode *inode, const struct ext4_inode_disk *disk, uint32_t logical,
    uint64_t *physical);
enum ext4_result ext4_cluster_release(struct ext4_allocation *allocation,
    const struct ext4_inode *inode, const struct ext4_inode_disk *disk, uint32_t logical,
    uint64_t physical, uint32_t length, uint64_t removed_end);
enum ext4_result ext4_allocation_super(struct ext4_allocation *allocation);
/* Recovery alone may claim an exact logged range idempotently. This updates
 * bitmap/free-space accounting, not the per-inode allocation delta. */
enum ext4_result ext4_allocation_claim(
    struct ext4_allocation *allocation, uint64_t block, uint64_t length);
enum ext4_result ext4_inode_claim(
    struct ext4_allocation *allocation, uint32_t number, uint16_t mode, bool *created);
/* Enroll one free inode and initialize its empty record in this transaction.
 * The caller links it into a directory before committing, and publishes the
 * primary free-inode count only after commit. */
enum ext4_result ext4_allocate_inode(struct ext4_allocation *allocation, uint16_t mode,
    struct ext4_inode_disk **disk, struct ext4_inode *inode);
enum ext4_result ext4_free_inode(struct ext4_allocation *allocation, struct ext4_inode_disk *disk,
    const struct ext4_inode *inode);
enum ext4_result ext4_inode_account(struct ext4_allocation *allocation,
    const struct ext4_inode *inode, struct ext4_inode_disk *disk, uint64_t size);

struct ext4_map_run {
	uint64_t physical;
	uint64_t length;
	bool unwritten;
};

struct ext4_unwritten_extent {
	uint64_t physical;
	uint32_t logical;
	uint32_t length;
};

/* A missing unwritten extent returns length zero. Initialization requires that
 * the selected prefix has been durably zeroed under the same exclusive owner.
 * It uses existing leaf capacity and never allocates a mapping block. */
enum ext4_result ext4_write_map_unwritten(struct ext4_allocation *allocation,
    const struct ext4_inode *inode, struct ext4_inode_disk *disk, uint32_t logical,
    struct ext4_unwritten_extent *range);
enum ext4_result ext4_write_map_initialize(struct ext4_allocation *allocation,
    const struct ext4_inode *inode, struct ext4_inode_disk *disk,
    const struct ext4_unwritten_extent *range, uint32_t length);
/* Find a zeroable suffix in the target leaf whose initialization merges with
 * its predecessor and releases an existing record. */
enum ext4_result ext4_write_map_mergeable(struct ext4_allocation *allocation,
    const struct ext4_inode *inode, struct ext4_inode_disk *disk, uint32_t logical, uint64_t end,
    struct ext4_unwritten_extent *range);

/* These mapping operations read the private transaction view. Lookup returns
 * runs so sparse gaps can be skipped without visiting each logical block. */
enum ext4_result ext4_write_map_lookup(struct ext4_allocation *allocation,
    const struct ext4_inode *inode, const struct ext4_inode_disk *disk, uint32_t logical,
    struct ext4_map_run *run);
enum ext4_result ext4_write_map_allocate(struct ext4_allocation *allocation,
    const struct ext4_inode *inode, struct ext4_inode_disk *disk, uint32_t logical,
    uint64_t *physical, bool *zero);
/* Reserve an extent-mapped hole without changing existing data. New allocations
 * remain unwritten; indirect records cannot encode this reservation. */
enum ext4_result ext4_write_map_reserve(struct ext4_allocation *allocation,
    const struct ext4_inode *inode, struct ext4_inode_disk *disk, uint32_t logical);
/* Retain room for a later EOF boundary inside an existing reservation. */
enum ext4_result ext4_write_map_reserve_capacity(struct ext4_allocation *allocation,
    const struct ext4_inode *inode, struct ext4_inode_disk *disk, uint32_t logical);
/* Remove an allocated run within one extent or one indirect data block. The
 * complete map must have passed ownership validation before the first removal. */
enum ext4_result ext4_write_map_punch(struct ext4_allocation *allocation,
    const struct ext4_inode *inode, struct ext4_inode_disk *disk, uint32_t logical,
    uint32_t length);
/* Validate the entire inode's allocated map before releasing any part of it.
 * Reject shared physical ranges, invalid bitmaps and inconsistent i_blocks.
 * Validation reads the transaction view without enrolling mapping snapshots. */
enum ext4_result ext4_write_map_validate(struct ext4_allocation *allocation,
    const struct ext4_inode *inode, struct ext4_inode_disk *disk);
/* Materialize a logged extent or hole in the private recovery transaction.
 * Recount validates ownership, claims surviving backing and derives i_blocks
 * from the resulting tree instead of trusting the logged tree shape. */
enum ext4_result ext4_write_map_replay(struct ext4_allocation *allocation,
    const struct ext4_inode *inode, struct ext4_inode_disk *disk, uint32_t logical,
    uint64_t physical, uint32_t length, bool unwritten);
enum ext4_result ext4_write_map_recount(struct ext4_allocation *allocation,
    const struct ext4_inode *inode, struct ext4_inode_disk *disk);
enum ext4_result ext4_write_map_truncate(struct ext4_allocation *allocation,
    const struct ext4_inode *inode, struct ext4_inode_disk *disk, uint32_t first);
/* The exclusive owner must validate the complete map before its first step.
 * Limit bounds data removal; extent cleanup may free two paths of extra nodes.
 * A successful incomplete step leaves a valid map for the next transaction. */
enum ext4_result ext4_write_map_trim(struct ext4_allocation *allocation,
    const struct ext4_inode *inode, struct ext4_inode_disk *disk, uint32_t first, uint32_t limit,
    bool *done);

#endif

/* SPDX-License-Identifier: BSD-3-Clause */
#ifndef MACHLIN_EXT4_ALLOCATE_H
#define MACHLIN_EXT4_ALLOCATE_H

#include "journal.h"

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
	uint64_t free_blocks;
	uint64_t reserved_blocks;
	uint64_t maximum_block;
	uint32_t group_index;
	uint32_t next_bit;
	uint32_t allocated;
};

enum ext4_result ext4_allocation_init(struct ext4_allocation *allocation, struct ext4_fs *fs,
    struct ext4_transaction *transaction, const struct ext4_inode *inode);
void ext4_allocation_destroy(struct ext4_allocation *allocation);
enum ext4_result ext4_allocate_block(struct ext4_allocation *allocation, uint64_t *block);
enum ext4_result ext4_allocation_valid(struct ext4_allocation *allocation, uint64_t block);

struct ext4_map_run {
	uint64_t physical;
	uint64_t length;
	bool unwritten;
};

/* These mapping operations read the private transaction view. Lookup returns
 * runs so sparse gaps can be skipped without visiting each logical block. */
enum ext4_result ext4_write_map_lookup(struct ext4_allocation *allocation,
    const struct ext4_inode *inode, const struct ext4_inode_disk *disk, uint32_t logical,
    struct ext4_map_run *run);
enum ext4_result ext4_write_map_allocate(struct ext4_allocation *allocation,
    const struct ext4_inode *inode, struct ext4_inode_disk *disk, uint32_t logical,
    uint64_t *physical, bool *zero);

#endif

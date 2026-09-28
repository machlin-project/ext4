/* SPDX-License-Identifier: BSD-3-Clause */
#ifndef MACHLIN_EXT4_DIRECTORY_WRITE_H
#define MACHLIN_EXT4_DIRECTORY_WRITE_H

#include "allocate.h"

struct ext4_directory_slot {
	struct ext4_inode_disk *inline_disk;
	uint32_t inline_tail;
	uint32_t inline_hash;
	uint64_t physical;
	uint32_t logical;
	uint32_t offset;
	uint32_t used;
	uint32_t length;
	uint32_t previous;
	uint32_t number;
	bool repack;
	enum ext4_file_type type;
};

enum ext4_directory_action { EXT4_DIRECTORY_INSERT, EXT4_DIRECTORY_FIND, EXT4_DIRECTORY_EMPTY };

enum ext4_result ext4_directory_scan(struct ext4_allocation *allocation, struct ext4_inode *parent,
    struct ext4_inode_disk *disk, const uint8_t *name, size_t name_length,
    enum ext4_directory_action action, uint32_t expected_parent, struct ext4_directory_slot *slot);
enum ext4_result ext4_directory_insert(struct ext4_allocation *allocation,
    const struct ext4_inode *parent, struct ext4_inode_disk *disk, struct ext4_directory_slot *slot,
    uint32_t number, enum ext4_file_type type, const uint8_t *name, size_t name_length);
enum ext4_result ext4_directory_initialize(struct ext4_allocation *allocation,
    struct ext4_inode *inode, struct ext4_inode_disk *disk, uint32_t parent);
enum ext4_result ext4_directory_remove(struct ext4_allocation *allocation,
    const struct ext4_inode *parent, const struct ext4_directory_slot *slot);
enum ext4_result ext4_directory_replace(struct ext4_allocation *allocation,
    const struct ext4_inode *parent, const struct ext4_directory_slot *slot, uint32_t number,
    enum ext4_file_type type);

#endif

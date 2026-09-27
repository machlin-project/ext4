/* SPDX-License-Identifier: BSD-3-Clause */
#ifndef MACHLIN_EXT4_XATTR_H
#define MACHLIN_EXT4_XATTR_H

#include "internal.h"

struct ext4_allocation;

struct ext4_xattr_record {
	const struct ext4_xattr_entry_disk *entry;
	const uint8_t *value;
	bool external;
};

struct ext4_xattr_snapshot {
	struct ext4_fs *fs;
	uint8_t *inode;
	uint8_t *block;
	struct ext4_xattr_record *records;
	size_t capacity;
	size_t count;
	uint64_t external_block;
};

enum ext4_result ext4_xattr_open(
    struct ext4_fs *fs, uint32_t number, uint32_t generation, struct ext4_xattr_snapshot *snapshot);
void ext4_xattr_close(struct ext4_xattr_snapshot *snapshot);
int ext4_xattr_compare(
    const struct ext4_xattr_entry_disk *left, const struct ext4_xattr_entry_disk *right);
uint32_t ext4_xattr_hash(
    const struct ext4_xattr_entry_disk *entry, const uint8_t *value, bool signed_names);
void ext4_xattr_checksum_set(
    struct ext4_fs *fs, uint64_t block, struct ext4_xattr_header_disk *header);
enum ext4_result ext4_xattr_changes_validate(
    struct ext4_fs *fs, const struct ext4_xattr_change *changes, size_t count);
enum ext4_result ext4_xattr_apply(struct ext4_allocation *allocation,
    const struct ext4_inode *inode, struct ext4_inode_disk *disk,
    const struct ext4_xattr_change *changes, size_t count);

#endif

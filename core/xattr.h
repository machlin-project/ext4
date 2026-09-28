/* SPDX-License-Identifier: BSD-3-Clause */
#ifndef MACHLIN_EXT4_XATTR_H
#define MACHLIN_EXT4_XATTR_H

#include "internal.h"

struct ext4_allocation;

struct ext4_xattr_record {
	const struct ext4_xattr_entry_disk *entry;
	const uint8_t *value;
	uint32_t value_inode;
	uint32_t value_hash;
	bool inode_storage;
	bool new_inode;
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
	uint32_t number;
	uint32_t generation;
};

enum ext4_result ext4_xattr_open(
    struct ext4_fs *fs, uint32_t number, uint32_t generation, struct ext4_xattr_snapshot *snapshot);
/* Copy an already decoded, exclusively owned inode record. Its checksum may
 * be stale inside a private transaction; external storage is still validated. */
enum ext4_result ext4_xattr_open_inode(struct ext4_fs *fs, const struct ext4_inode *inode,
    const struct ext4_inode_disk *disk, struct ext4_xattr_snapshot *snapshot);
void ext4_xattr_close(struct ext4_xattr_snapshot *snapshot);
int ext4_xattr_compare(
    const struct ext4_xattr_entry_disk *left, const struct ext4_xattr_entry_disk *right);
uint32_t ext4_xattr_hash(
    const struct ext4_xattr_entry_disk *entry, const uint8_t *value, bool signed_names);
uint32_t ext4_xattr_inode_entry_hash(
    const struct ext4_xattr_entry_disk *entry, uint32_t value_hash, bool signed_names);
/* Attribute values are private regular inodes, never ordinary namespace objects.
 * A NULL output validates their header and entry hash without reading payload. */
enum ext4_result ext4_xattr_inode_read(struct ext4_fs *fs, uint32_t parent, uint32_t generation,
    const struct ext4_xattr_entry_disk *entry, void *output, uint32_t *value_hash);
enum ext4_result ext4_xattr_inode_create(struct ext4_allocation *allocation,
    const struct ext4_inode *parent, struct ext4_xattr_record *record);
enum ext4_result ext4_xattr_inode_adjust(
    struct ext4_allocation *allocation, const struct ext4_xattr_record *record, int change);
uint64_t ext4_xattr_value_blocks(const struct ext4_xattr_snapshot *snapshot);
uint64_t ext4_xattr_value_charge(const struct ext4_fs *fs, uint32_t size);
void ext4_xattr_checksum_set(
    struct ext4_fs *fs, uint64_t block, struct ext4_xattr_header_disk *header);
enum ext4_result ext4_xattr_changes_validate(
    struct ext4_fs *fs, const struct ext4_xattr_change *changes, size_t count);
enum ext4_result ext4_xattr_apply(struct ext4_allocation *allocation, struct ext4_inode *inode,
    struct ext4_inode_disk *disk, const struct ext4_xattr_change *changes, size_t count);
enum ext4_result ext4_xattr_drop(struct ext4_allocation *allocation, struct ext4_inode *inode,
    struct ext4_inode_disk *disk, bool *done);

#endif

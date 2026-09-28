/* SPDX-License-Identifier: BSD-3-Clause */
#ifndef MACHLIN_EXT4_INLINE_H
#define MACHLIN_EXT4_INLINE_H

#include "xattr.h"

/* Directory cookies use the equivalent linear block: synthetic dot entries,
 * followed by the two independent inline entry regions at unchanged offsets. */
#define EXT4_INLINE_PARENT_SIZE ((uint32_t)sizeof(struct ext4_le32))
#define EXT4_INLINE_DOT_SIZE 12U
#define EXT4_INLINE_DOTS_SIZE (2U * EXT4_INLINE_DOT_SIZE)
#define EXT4_INLINE_FIRST_END                                                                      \
	(EXT4_INLINE_DOTS_SIZE + EXT4_INODE_BLOCK_BYTES - EXT4_INLINE_PARENT_SIZE)

struct ext4_inline_view {
	struct ext4_xattr_snapshot attributes;
	const uint8_t *head;
	const uint8_t *tail;
	uint32_t tail_size;
	uint32_t tail_offset;
	uint32_t hash_offset;
};

bool ext4_inline_key(uint8_t index, const uint8_t *name, size_t length);
/* A NULL disk resolves a fresh, generation-checked record. Otherwise the
 * exclusively owned transaction record supplies both data and attributes. */
enum ext4_result ext4_inline_open(struct ext4_fs *fs, const struct ext4_inode *inode,
    const struct ext4_inode_disk *disk, struct ext4_inline_view *view);
void ext4_inline_close(struct ext4_inline_view *view);
enum ext4_result ext4_inline_validate(
    struct ext4_fs *fs, const struct ext4_inode *inode, const struct ext4_inode_disk *disk);
enum ext4_result ext4_inline_read(struct ext4_fs *fs, const struct ext4_inode *inode,
    uint64_t offset, void *buffer, size_t length);
/* Materialize a checked linear directory. With pad false the last record ends
 * at *used; padding extends it to the block checksum tail for readers/conversion. */
enum ext4_result ext4_inline_directory(struct ext4_fs *fs, const struct ext4_inode *inode,
    const struct ext4_inode_disk *disk, uint8_t *buffer, bool pad, uint32_t *used,
    uint32_t *tail_offset, uint32_t *hash_offset);
/* Conversion is part of the caller's private transaction. It accounts itself
 * before returning, preserving allocation deltas belonging to the caller. */
enum ext4_result ext4_inline_expand(
    struct ext4_allocation *allocation, struct ext4_inode *inode, struct ext4_inode_disk *disk);
/* New empty objects can use inode storage without moving admitted attributes. */
enum ext4_result ext4_inline_start(struct ext4_allocation *allocation, struct ext4_inode *inode,
    struct ext4_inode_disk *disk, uint32_t parent, bool *created);
/* Grow in unused inode space without moving other keys or allocating blocks. */
enum ext4_result ext4_inline_grow(struct ext4_allocation *allocation, struct ext4_inode *inode,
    struct ext4_inode_disk *disk, uint64_t minimum, bool *grown);
/* Mutate within current inline capacity without allocating a data block;
 * otherwise convert and let the caller use its ordinary mapped path. */
enum ext4_result ext4_inline_edit(struct ext4_allocation *allocation, struct ext4_inode *inode,
    struct ext4_inode_disk *disk, uint64_t size, uint64_t offset, const void *buffer, size_t length,
    bool *handled);

#endif

/* SPDX-License-Identifier: BSD-3-Clause */
#ifndef MACHLIN_EXT4_FAST_COMMIT_H
#define MACHLIN_EXT4_FAST_COMMIT_H

#include "journal.h"

#define EXT4_FC_ADD_RANGE 1U
#define EXT4_FC_DEL_RANGE 2U
#define EXT4_FC_CREATE 3U
#define EXT4_FC_LINK 4U
#define EXT4_FC_UNLINK 5U
#define EXT4_FC_INODE 6U
#define EXT4_FC_PAD 7U
#define EXT4_FC_TAIL 8U
#define EXT4_FC_HEAD 9U
#define EXT4_FC_MAX_BLOCKS 4096U
#define EXT4_FC_MAX_RECORDS 65536U

struct ext4_fc_header_disk {
	struct ext4_le16 type;
	struct ext4_le16 length;
};

struct ext4_fc_head_disk {
	struct ext4_le32 features;
	struct ext4_le32 sequence;
};

struct ext4_fc_tail_disk {
	struct ext4_le32 sequence;
	struct ext4_le32 checksum;
};

struct ext4_fc_add_disk {
	struct ext4_le32 inode;
	struct ext4_extent_disk extent;
};

struct ext4_fc_delete_disk {
	struct ext4_le32 inode;
	struct ext4_le32 logical;
	struct ext4_le32 blocks;
};

struct ext4_fc_name_disk {
	struct ext4_le32 parent;
	struct ext4_le32 inode;
	/* The remaining value bytes are the name, without a terminator. */
};

struct ext4_fc_inode_disk {
	struct ext4_le32 inode;
	/* The remaining value bytes are the logged portion of the raw inode. */
};

struct ext4_fc_record {
	uint32_t block;
	uint32_t checksum;
	uint16_t offset;
	uint16_t type;
	uint16_t length;
};

struct ext4_fast_commit {
	struct ext4_journal *journal;
	uint32_t count;
	uint32_t commits;
	uint32_t sequence;
	bool discarded_tail;
	struct ext4_fc_record records[];
};

/* Decode a bounded, explicitly located fast area. The caller obtains sequence
 * from the complete ordinary journal prefix, not from the fast HEAD itself.
 * This only retains committed records; it neither admits the filesystem feature
 * nor changes any device. Semantic replay must validate ownership separately. */
enum ext4_result ext4_fast_commit_load(struct ext4_journal *journal, uint32_t first, uint32_t last,
    uint32_t sequence, struct ext4_fast_commit **result);
void ext4_fast_commit_close(struct ext4_fast_commit *log);
/* The caller supplies a filesystem-block-sized buffer. Re-read records are bound
 * to their scanned bytes before their value is exposed to semantic replay. */
enum ext4_result ext4_fast_commit_read(
    const struct ext4_fast_commit *log, uint32_t index, void *buffer, const void **value);
/* The ordinary journal prefix has been durably checkpointed, but its on-disk
 * authority has not been reset. Materialize the complete fast prefix privately,
 * then commit a full transaction with the same ID to supersede it atomically. */
enum ext4_result ext4_fast_commit_replay(struct ext4_fast_commit *log);

#endif

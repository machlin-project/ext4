/* SPDX-License-Identifier: BSD-3-Clause */
#ifndef MACHLIN_EXT4_QUOTA_H
#define MACHLIN_EXT4_QUOTA_H

#include "journal.h"

/* Quota usage follows Linux and e2fsck: every in-use inode other than reserved,
 * private attribute-value and quota/orphan system inodes charges its i_blocks in
 * bytes and one inode, plus one inode for each attribute-value inode reference.
 * Usage changes are derived at commit from every changed inode record, so every
 * mutation path, including recovery cleanup, stays consistent with the records
 * it commits. Limits and grace times are preserved but not enforced. */

/* Validate quota inode numbers named by a superblock with the QUOTA feature. */
enum ext4_result ext4_quota_super_validate(struct ext4_fs *fs, const struct ext4_super_disk *super);
/* Prepare writable accounting after the journal and system ranges exist. */
enum ext4_result ext4_quota_open(struct ext4_fs *fs);
void ext4_quota_close(struct ext4_fs *fs);
bool ext4_quota_system_inode(const struct ext4_fs *fs, uint32_t number);
/* Apply the usage change of every inode record in the transaction to the quota
 * files inside the same transaction, inserting entries and growing quota files
 * as needed. Called by commit before the superblock is sealed. */
enum ext4_result ext4_quota_commit(struct ext4_transaction *transaction);

#endif

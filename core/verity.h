/* SPDX-License-Identifier: BSD-3-Clause */
#ifndef MACHLIN_EXT4_VERITY_H
#define MACHLIN_EXT4_VERITY_H

#include "internal.h"

/* fs-verity metadata follows the file data. ext4 starts the Merkle tree at the
 * first 64 KiB boundary at or after EOF, stores its levels from the root toward
 * the data, then the descriptor on a filesystem-block boundary, and records the
 * descriptor size in the last four bytes of the last mapped block. */
#define EXT4_VERITY_METADATA_ALIGNMENT 65536U
#define EXT4_VERITY_VERSION 1U
#define EXT4_VERITY_SHA256 1U
#define EXT4_VERITY_SHA512 2U
#define EXT4_VERITY_MIN_LOG_BLOCK 10U
#define EXT4_VERITY_MAX_LOG_BLOCK 16U
#define EXT4_VERITY_MAX_LEVELS 8U
#define EXT4_VERITY_MAX_DIGEST 64U
#define EXT4_VERITY_MAX_SALT 32U
#define EXT4_VERITY_MAX_PADDED_SALT 128U

struct ext4_verity_descriptor_disk {
	uint8_t version;
	uint8_t hash_algorithm;
	uint8_t log_block_size;
	uint8_t salt_size;
	struct ext4_le32 signature_size;
	struct ext4_le32 data_size_lo;
	struct ext4_le32 data_size_hi;
	uint8_t root_hash[EXT4_VERITY_MAX_DIGEST];
	uint8_t salt[EXT4_VERITY_MAX_SALT];
	uint8_t reserved[144];
};

_Static_assert(sizeof(struct ext4_verity_descriptor_disk) == 256, "verity descriptor size");

struct ext4_verity {
	uint64_t data_size;
	uint64_t tree_offset;
	uint64_t descriptor_offset;
	uint64_t level_start[EXT4_VERITY_MAX_LEVELS];
	uint32_t block_size;
	uint32_t digest_size;
	uint32_t hashes_per_block;
	uint32_t padded_salt_size;
	uint8_t algorithm;
	uint8_t levels;
	uint8_t root_hash[EXT4_VERITY_MAX_DIGEST];
	uint8_t padded_salt[EXT4_VERITY_MAX_PADDED_SALT];
};

/* Set the hash geometry of an algorithm, Merkle block size and salt. */
enum ext4_result ext4_verity_configure(struct ext4_verity *verity, uint8_t algorithm,
    uint8_t log_block_size, const uint8_t *salt, uint8_t salt_size);
/* Derive the levels of the tree over data_size bytes, stored from the root toward
 * the data, and the tree's size in Merkle blocks. */
enum ext4_result ext4_verity_geometry(struct ext4_verity *verity, uint64_t *tree_blocks);
/* Hash one Merkle block after the padded salt. */
void ext4_verity_hash(const struct ext4_verity *verity, const uint8_t *block, uint8_t *digest);
/* The file digest of a descriptor; clears its signature size. */
void ext4_verity_file_digest(const struct ext4_verity *verity,
    struct ext4_verity_descriptor_disk *descriptor, uint8_t *digest);
/* Validate the descriptor and derive tree geometry for a verity inode. */
enum ext4_result ext4_verity_open(
    struct ext4_fs *fs, const struct ext4_inode *inode, struct ext4_verity *verity);
/* Read bytes below EOF, verifying every covering Merkle data block. */
enum ext4_result ext4_verity_read(struct ext4_fs *fs, const struct ext4_inode *inode,
    uint64_t offset, void *buffer, size_t length, size_t *completed);

#endif

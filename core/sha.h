/* SPDX-License-Identifier: BSD-3-Clause */
#ifndef MACHLIN_EXT4_SHA_H
#define MACHLIN_EXT4_SHA_H

#include "internal.h"

#define EXT4_SHA256_BLOCK_SIZE 64U
#define EXT4_SHA256_DIGEST_SIZE 32U
#define EXT4_SHA512_BLOCK_SIZE 128U
#define EXT4_SHA512_DIGEST_SIZE 64U

struct ext4_sha256 {
	uint32_t state[8];
	uint64_t length;
	uint32_t used;
	uint8_t block[EXT4_SHA256_BLOCK_SIZE];
};

struct ext4_sha512 {
	uint64_t state[8];
	uint64_t length;
	uint32_t used;
	uint8_t block[EXT4_SHA512_BLOCK_SIZE];
};

void ext4_sha256_init(struct ext4_sha256 *context);
void ext4_sha256_update(struct ext4_sha256 *context, const void *buffer, size_t length);
void ext4_sha256_final(struct ext4_sha256 *context, uint8_t *digest);
void ext4_sha512_init(struct ext4_sha512 *context);
void ext4_sha512_update(struct ext4_sha512 *context, const void *buffer, size_t length);
void ext4_sha512_final(struct ext4_sha512 *context, uint8_t *digest);

#endif

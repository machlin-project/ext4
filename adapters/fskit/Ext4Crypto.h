/* SPDX-License-Identifier: BSD-3-Clause */
#ifndef MACHLIN_EXT4_NATIVE_CRYPTO_H
#define MACHLIN_EXT4_NATIVE_CRYPTO_H

#include <ext4/ext4.h>

#define EXT4_NATIVE_MASTER_SIZE 64U
#define EXT4_NATIVE_IDENTIFIER_SIZE 16U
#define EXT4_NATIVE_DESCRIPTOR_SIZE 8U
#define EXT4_NATIVE_MAX_KEYS 16U

struct ext4_native_crypto;

/* One serialized mount owner. Add keys before publishing the environment;
 * seal makes the master-key set immutable until unmount. Destroy only after
 * ext4_unmount has released all derived handles. Raw keys never leave here. */
struct ext4_native_crypto *ext4_native_crypto_create(void);
void ext4_native_crypto_destroy(struct ext4_native_crypto *crypto);
enum ext4_result ext4_native_crypto_add(struct ext4_native_crypto *crypto, uint8_t version,
    const uint8_t *identifier, size_t identifier_size, const void *master, size_t master_size);
void ext4_native_crypto_seal(struct ext4_native_crypto *crypto);
size_t ext4_native_crypto_count(const struct ext4_native_crypto *crypto);
struct ext4_crypto_environment ext4_native_crypto_environment(struct ext4_native_crypto *crypto);
enum ext4_result ext4_native_crypto_identifier(
    const void *master, size_t size, uint8_t identifier[EXT4_NATIVE_IDENTIFIER_SIZE]);

#endif

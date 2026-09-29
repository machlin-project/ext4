/* SPDX-License-Identifier: BSD-3-Clause */
#ifndef MACHLIN_EXT4_TEST_KEYRING_H
#define MACHLIN_EXT4_TEST_KEYRING_H

#include "crypto.h"
#include "ext4/ext4.h"

#include <stdlib.h>

/* A test adapter's fscrypt callbacks on the reference cryptography. Its master keys
 * are byte patterns: byte i of the key with offset k is i * 7 + k, and offset 3 is
 * the key the Linux probe adds. Nonces come from a counter, so a caller that saves
 * and restores the counter repeats an operation's bytes exactly. */

#define KEYRING_MASTER_BYTES 64U
#define KEYRING_MASTER_MULTIPLIER 7U
#define FSCRYPT_HKDF_PREFIX "fscrypt"
#define FSCRYPT_HKDF_PREFIX_SIZE 8U
#define FSCRYPT_KEY_IDENTIFIER_CONTEXT 1U
#define FSCRYPT_IDENTIFIER_BYTES 16U
#define FSCRYPT_V2 2U
#define FSCRYPT_V1_KEY_BYTES 16U

#define KEYRING_REQUIRE(expression)                                                                \
	do {                                                                                       \
		if (!(expression)) {                                                               \
			abort();                                                                   \
		}                                                                                  \
	} while (0)

/* A test adapter's keyring: one master key, found by its fscrypt identifier, and a
 * counter that makes every nonce distinct. */
struct keyring {
	uint8_t master[KEYRING_MASTER_BYTES];
	uint8_t identifier[FSCRYPT_IDENTIFIER_BYTES];
	uint32_t handles;
	uint32_t derivations;
	uint64_t nonces;
};

struct key {
	uint8_t bytes[KEYRING_MASTER_BYTES];
	size_t size;
};

static void
keyring_init(struct keyring *keyring, uint8_t offset)
{
	uint8_t info[FSCRYPT_HKDF_PREFIX_SIZE + 1U];
	unsigned int index;

	memset(keyring, 0, sizeof(*keyring));
	for (index = 0; index < KEYRING_MASTER_BYTES; index++) {
		keyring->master[index] = (uint8_t)(index * KEYRING_MASTER_MULTIPLIER + offset);
	}
	memcpy(info, FSCRYPT_HKDF_PREFIX, FSCRYPT_HKDF_PREFIX_SIZE);
	info[FSCRYPT_HKDF_PREFIX_SIZE] = FSCRYPT_KEY_IDENTIFIER_CONTEXT;
	test_hkdf_sha512(keyring->master, sizeof(keyring->master), info, sizeof(info),
	    keyring->identifier, sizeof(keyring->identifier));
}

static struct key *
keyring_key(struct keyring *keyring)
{
	struct key *key = calloc(1, sizeof(*key));

	KEYRING_REQUIRE(key != NULL);
	keyring->handles++;
	return key;
}

static enum ext4_result
keyring_find(void *context, uint8_t version, const uint8_t *identifier, size_t size, void **master)
{
	struct keyring *keyring = context;
	struct key *key;

	if (version != FSCRYPT_V2 || size != sizeof(keyring->identifier) ||
	    memcmp(identifier, keyring->identifier, size) != 0) {
		return EXT4_NOT_FOUND;
	}
	key = keyring_key(keyring);
	memcpy(key->bytes, keyring->master, sizeof(keyring->master));
	key->size = sizeof(keyring->master);
	*master = key;
	return EXT4_OK;
}

static enum ext4_result
keyring_derive(void *context, void *master, uint8_t version, const uint8_t *info, size_t info_size,
    size_t key_size, void **result)
{
	struct keyring *keyring = context;
	struct key *source = master;
	struct key *key;
	struct test_aes aes;
	size_t offset;

	KEYRING_REQUIRE(key_size <= sizeof(key->bytes) && key_size <= source->size);
	key = keyring_key(keyring);
	key->size = key_size;
	if (version == FSCRYPT_V2) {
		test_hkdf_sha512(
		    source->bytes, source->size, info, info_size, key->bytes, key_size);
	} else {
		/* Version 1: the master key encrypted with AES-128-ECB under the nonce. */
		KEYRING_REQUIRE(info_size == FSCRYPT_V1_KEY_BYTES);
		test_aes_init(&aes, info, FSCRYPT_V1_KEY_BYTES);
		for (offset = 0; offset < key_size; offset += TEST_AES_BLOCK) {
			test_aes_encrypt(&aes, source->bytes + offset, key->bytes + offset);
		}
	}
	keyring->derivations++;
	*result = key;
	return EXT4_OK;
}

static enum ext4_result
keyring_cipher(void *context, void *handle, uint8_t mode, bool encrypt, const uint8_t *iv,
    const void *input, void *output, size_t length)
{
	struct key *key = handle;

	(void)context;
	KEYRING_REQUIRE(input != output);
	if (mode == EXT4_FSCRYPT_MODE_AES_256_XTS) {
		test_aes_xts(key->bytes, key->size, iv, encrypt, input, output, length);
	} else if (mode == EXT4_FSCRYPT_MODE_AES_256_CTS) {
		test_aes_cts(key->bytes, key->size, iv, encrypt, input, output, length);
	} else {
		return EXT4_UNSUPPORTED;
	}
	return EXT4_OK;
}

static void
keyring_release(void *context, void *handle)
{
	struct keyring *keyring = context;

	KEYRING_REQUIRE(keyring->handles != 0);
	keyring->handles--;
	free(handle);
}

static enum ext4_result
keyring_random(void *context, void *buffer, size_t length)
{
	struct keyring *keyring = context;
	uint8_t counter[sizeof(keyring->nonces)];
	uint8_t digest[EXT4_SHA256_DIGEST_SIZE];
	struct ext4_sha256 hash;
	unsigned int index;

	KEYRING_REQUIRE(length <= sizeof(digest));
	keyring->nonces++;
	for (index = 0; index < sizeof(counter); index++) {
		counter[index] = (uint8_t)(keyring->nonces >> (8U * index));
	}
	ext4_sha256_init(&hash);
	ext4_sha256_update(&hash, counter, sizeof(counter));
	ext4_sha256_final(&hash, digest);
	memcpy(buffer, digest, length);
	return EXT4_OK;
}

static struct ext4_crypto_environment
keyring_environment(struct keyring *keyring)
{
	struct ext4_crypto_environment crypto = { keyring, NULL, false, keyring_find,
		keyring_derive, keyring_cipher, keyring_release, keyring_random };

	return crypto;
}

#endif

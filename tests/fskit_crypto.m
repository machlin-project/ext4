/* SPDX-License-Identifier: BSD-3-Clause */
#include "../adapters/fskit/Ext4Crypto.h"
#include "crypto.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

#define NATIVE_TEST_BYTES 65536U

static void
check_cipher(uint8_t version, uint8_t mode)
{
	static const size_t xts_lengths[] = { 16, 32, 512, 1024, 4096, 65536 };
	static const size_t cts_lengths[] = { 16, 17, 31, 32, 33, 47, 48, 49, 255, 256, 511, 4093,
		65536 };
	const size_t *lengths = mode == EXT4_FSCRYPT_MODE_AES_256_XTS ? xts_lengths : cts_lengths;
	size_t count = mode == EXT4_FSCRYPT_MODE_AES_256_XTS
	    ? sizeof(xts_lengths) / sizeof(xts_lengths[0])
	    : sizeof(cts_lengths) / sizeof(cts_lengths[0]);
	struct ext4_native_crypto *provider = ext4_native_crypto_create();
	struct ext4_crypto_environment environment = ext4_native_crypto_environment(provider);
	uint8_t master[EXT4_NATIVE_MASTER_SIZE];
	uint8_t identifier[EXT4_NATIVE_IDENTIFIER_SIZE];
	uint8_t info[25] = { 'f', 's', 'c', 'r', 'y', 'p', 't', 0, 2 };
	uint8_t expected_key[EXT4_NATIVE_MASTER_SIZE];
	uint8_t iv[TEST_AES_BLOCK];
	uint8_t *plain = malloc(NATIVE_TEST_BYTES + 2);
	uint8_t *cipher = malloc(NATIVE_TEST_BYTES + 2);
	uint8_t *back = malloc(NATIVE_TEST_BYTES + 2);
	uint8_t *reference = malloc(NATIVE_TEST_BYTES);
	void *master_handle = NULL;
	void *key = NULL;
	void *sentinel = (void *)(uintptr_t)1;
	struct test_aes aes;
	size_t key_size = mode == EXT4_FSCRYPT_MODE_AES_256_XTS ? 64 : 32;
	size_t identifier_size = version == 2 ? sizeof(identifier) : EXT4_NATIVE_DESCRIPTOR_SIZE;
	size_t index;
	size_t sample;
	size_t length;

	assert(provider != NULL && plain != NULL && cipher != NULL && back != NULL &&
	    reference != NULL);
	for (index = 0; index < sizeof(master); index++) {
		master[index] = (uint8_t)(index * 7 + 3);
	}
	assert(ext4_native_crypto_identifier(master, sizeof(master), identifier) == EXT4_OK);
	assert(ext4_native_crypto_add(provider, version, identifier, identifier_size, master,
		   sizeof(master)) == EXT4_OK);
	assert(ext4_native_crypto_count(provider) == 1);
	assert(ext4_native_crypto_add(provider, version, identifier, identifier_size, master,
		   sizeof(master)) == EXT4_EXISTS);
	ext4_native_crypto_seal(provider);
	assert(ext4_native_crypto_add(provider, version, identifier, identifier_size, master,
		   sizeof(master)) == EXT4_BUSY);
	assert(environment.find_key(
		   provider, version, identifier, identifier_size, &master_handle) == EXT4_OK);
	assert(environment.derive_key(provider, master_handle, version, info,
		   version == 1 ? 16 : sizeof(info), key_size, &key) == EXT4_OK);
	assert(environment.derive_key(provider, master_handle, version, info, sizeof(info), 1,
		   &sentinel) == EXT4_INVALID_ARGUMENT);
	assert(sentinel == (void *)(uintptr_t)1);
	if (version == 2) {
		test_hkdf_sha512(
		    master, sizeof(master), info, sizeof(info), expected_key, key_size);
	} else {
		test_aes_init(&aes, info, TEST_AES_BLOCK);
		for (index = 0; index < key_size; index += TEST_AES_BLOCK) {
			test_aes_encrypt(&aes, master + index, expected_key + index);
		}
	}
	environment.release_key(provider, master_handle);
	for (index = 0; index < sizeof(iv); index++) {
		iv[index] = (uint8_t)(index * 17 + 5);
	}
	for (index = 0; index < NATIVE_TEST_BYTES; index++) {
		plain[index + 1] = (uint8_t)(index * 11 + 3);
	}
	for (sample = 0; sample < count; sample++) {
		length = lengths[sample];
		memset(cipher, 0xa5, NATIVE_TEST_BYTES + 2);
		memset(back, 0xa5, NATIVE_TEST_BYTES + 2);
		if (mode == EXT4_FSCRYPT_MODE_AES_256_XTS) {
			test_aes_xts(
			    expected_key, key_size, iv, true, plain + 1, reference, length);
		} else {
			test_aes_cts(
			    expected_key, key_size, iv, true, plain + 1, reference, length);
		}
		assert(environment.cipher(provider, key, mode, true, iv, plain + 1, cipher + 1,
			   length) == EXT4_OK);
		assert(memcmp(cipher + 1, reference, length) == 0);
		assert(environment.cipher(
			   provider, key, mode, false, iv, reference, back + 1, length) == EXT4_OK);
		assert(memcmp(back + 1, plain + 1, length) == 0);
		assert(cipher[0] == 0xa5 && cipher[length + 1] == 0xa5);
		assert(back[0] == 0xa5 && back[length + 1] == 0xa5);
	}
	assert(environment.cipher(provider, key, mode, true, iv, plain, plain + 1, 16) ==
	    EXT4_INVALID_ARGUMENT);
	assert(environment.cipher(provider, key, mode, true, iv, plain, cipher, 15) ==
	    EXT4_INVALID_ARGUMENT);
	assert(environment.cipher(provider, key, mode, true, iv, plain, cipher,
		   NATIVE_TEST_BYTES + 1) == EXT4_INVALID_ARGUMENT);
	assert(environment.cipher(provider, key, 255, true, iv, plain, cipher, 16) ==
	    EXT4_UNSUPPORTED);
	if (mode == EXT4_FSCRYPT_MODE_AES_256_XTS) {
		assert(environment.cipher(provider, key, mode, true, iv, plain, cipher, 17) ==
		    EXT4_INVALID_ARGUMENT);
	}
	identifier[0] ^= 1;
	assert(environment.find_key(provider, version, identifier, identifier_size, &sentinel) ==
	    EXT4_NOT_FOUND);
	assert(sentinel == (void *)(uintptr_t)1);
	environment.release_key(provider, key);
	ext4_native_crypto_destroy(provider);
	free(plain);
	free(cipher);
	free(back);
	free(reference);
}

static void
check_keyring(void)
{
	static const uint8_t linux_identifier[] = { 0xa5, 0x25, 0xb3, 0x10, 0xd9, 0x75, 0x60, 0x4e,
		0x26, 0xc7, 0x61, 0x13, 0x4e, 0x6c, 0x35, 0xd1 };
	struct ext4_native_crypto *provider = ext4_native_crypto_create();
	struct ext4_crypto_environment environment = ext4_native_crypto_environment(provider);
	uint8_t master[EXT4_NATIVE_MASTER_SIZE];
	uint8_t identifier[EXT4_NATIVE_IDENTIFIER_SIZE];
	uint8_t random[64];
	uint8_t again[64];
	size_t index;
	size_t entry;

	assert(provider != NULL);
	for (index = 0; index < sizeof(master); index++) {
		master[index] = (uint8_t)(index * 7 + 3);
	}
	assert(ext4_native_crypto_identifier(master, sizeof(master), identifier) == EXT4_OK);
	assert(memcmp(identifier, linux_identifier, sizeof(identifier)) == 0);
	assert(ext4_native_crypto_identifier(master, sizeof(master) - 1, identifier) ==
	    EXT4_INVALID_ARGUMENT);
	identifier[0] ^= 1;
	assert(ext4_native_crypto_add(provider, 2, identifier, sizeof(identifier), master,
		   sizeof(master)) == EXT4_INVALID_ARGUMENT);
	for (entry = 0; entry <= EXT4_NATIVE_MAX_KEYS; entry++) {
		master[0] = (uint8_t)entry;
		assert(
		    ext4_native_crypto_identifier(master, sizeof(master), identifier) == EXT4_OK);
		assert(ext4_native_crypto_add(
			   provider, 2, identifier, sizeof(identifier), master, sizeof(master)) ==
		    (entry == EXT4_NATIVE_MAX_KEYS ? EXT4_NO_SPACE : EXT4_OK));
	}
	assert(ext4_native_crypto_count(provider) == EXT4_NATIVE_MAX_KEYS);
	assert(environment.random_bytes(provider, random, sizeof(random)) == EXT4_OK);
	assert(environment.random_bytes(provider, again, sizeof(again)) == EXT4_OK);
	assert(memcmp(random, again, sizeof(random)) != 0);
	assert(environment.random_bytes(provider, NULL, 1) == EXT4_INVALID_ARGUMENT);
	ext4_native_crypto_destroy(provider);
}

int
main(void)
{
	check_keyring();
	check_cipher(1, EXT4_FSCRYPT_MODE_AES_256_XTS);
	check_cipher(1, EXT4_FSCRYPT_MODE_AES_256_CTS);
	check_cipher(2, EXT4_FSCRYPT_MODE_AES_256_XTS);
	check_cipher(2, EXT4_FSCRYPT_MODE_AES_256_CTS);
	puts("PASS native fscrypt: Linux identifier, v1/v2 derivation, XTS/CTS oracle, bounds and "
	     "key lifetime");
	return 0;
}

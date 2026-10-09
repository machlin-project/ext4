/* SPDX-License-Identifier: BSD-3-Clause */
#ifndef MACHLIN_EXT4_TEST_CRYPTO_FAILURES_H
#define MACHLIN_EXT4_TEST_CRYPTO_FAILURES_H

#define CRYPTO_BUFFER_GUARD 0x7bU

/* Included by encrypt.c, sharing its filesystem and reference keyring helpers. */
enum crypto_failure { CRYPTO_WORKING, CRYPTO_FIND, CRYPTO_DERIVE, CRYPTO_CIPHER, CRYPTO_RANDOM };

struct failing_crypto {
	struct keyring keyring;
	enum crypto_failure operation;
	unsigned int calls;
	unsigned int fail_at;
};

static bool
crypto_fails(struct failing_crypto *crypto, enum crypto_failure operation)
{
	return crypto->operation == operation && ++crypto->calls == crypto->fail_at;
}

static enum ext4_result
failing_find(void *context, uint8_t version, const uint8_t *identifier, size_t size, void **key)
{
	struct failing_crypto *crypto = context;

	return crypto_fails(crypto, CRYPTO_FIND)
	    ? EXT4_IO
	    : keyring_find(&crypto->keyring, version, identifier, size, key);
}

static enum ext4_result
failing_derive(void *context, void *master, uint8_t version, const uint8_t *info, size_t info_size,
    size_t key_size, void **key)
{
	struct failing_crypto *crypto = context;

	return crypto_fails(crypto, CRYPTO_DERIVE)
	    ? EXT4_IO
	    : keyring_derive(&crypto->keyring, master, version, info, info_size, key_size, key);
}

static enum ext4_result
failing_cipher(void *context, void *key, uint8_t mode, bool encrypt, const uint8_t *iv,
    const void *input, void *output, size_t length)
{
	struct failing_crypto *crypto = context;

	CHECK(input != output);
	if (crypto_fails(crypto, CRYPTO_CIPHER)) {
		/* A provider may have changed its output before reporting a failure. */
		memset(output, 0xa5, length / 2U);
		return EXT4_IO;
	}
	return keyring_cipher(&crypto->keyring, key, mode, encrypt, iv, input, output, length);
}

static void
failing_release(void *context, void *key)
{
	struct failing_crypto *crypto = context;

	keyring_release(&crypto->keyring, key);
}

static enum ext4_result
failing_random(void *context, void *buffer, size_t length)
{
	struct failing_crypto *crypto = context;

	if (crypto_fails(crypto, CRYPTO_RANDOM)) {
		memset(buffer, 0xa5, length / 2U);
		return EXT4_IO;
	}
	return keyring_random(&crypto->keyring, buffer, length);
}

static void
crypto_failure_select(struct ext4_fs *fs, struct failing_crypto *crypto,
    enum crypto_failure operation, unsigned int fail_at)
{
	struct ext4_crypto_environment environment = { crypto, NULL, false, failing_find,
		failing_derive, failing_cipher, failing_release, failing_random, NULL };

	EXPECT(ext4_set_crypto(fs, NULL), EXT4_OK);
	CHECK(crypto->keyring.handles == 0);
	crypto->operation = operation;
	crypto->calls = 0;
	crypto->fail_at = fail_at;
	EXPECT(ext4_set_crypto(fs, &environment), EXT4_OK);
}

static void
keyed_callback_failures(struct device *device)
{
	struct failing_crypto crypto;
	struct ext4_encryption_policy policy = { 0 };
	struct ext4_inode_update update = creation();
	struct ext4_fs *fs;
	struct ext4_inode directory;
	struct ext4_inode inode;
	struct ext4_inode result;
	uint8_t *before = malloc(device->size);
	uint8_t *plain_storage = malloc(3U * device->block_size + 2U);
	uint8_t *read_storage = malloc(3U * device->block_size + 2U);
	uint8_t *plain;
	uint8_t *read_back;
	uint32_t writes;
	uint32_t allocations;
	size_t completed;
	size_t index;
	size_t length = 3U * device->block_size;
	enum crypto_failure failure;

	CHECK(before != NULL && plain_storage != NULL && read_storage != NULL);
	/* Cipher providers must accept an unaligned complete block, including direct
	 * caller input. Guards also cover partial output on a callback failure. */
	plain = plain_storage + 1;
	read_back = read_storage + 1;
	plain_storage[0] = plain_storage[length + 1U] = CRYPTO_BUFFER_GUARD;
	read_storage[0] = read_storage[length + 1U] = CRYPTO_BUFFER_GUARD;
	enable_encryption(device);
	device_reset(device, device->base);
	keyring_init(&crypto.keyring, PROBE_KEY_OFFSET);
	EXPECT(ext4_mount_writable(&device->environment, &device->writer, &fs), EXT4_OK);
	crypto_failure_select(fs, &crypto, CRYPTO_WORKING, 0);
	EXPECT(ext4_mkdir(fs, EXT4_ROOT_INODE, 0, (const uint8_t *)"vault", 5, &update,
		   &encrypt_time, &directory),
	    EXT4_OK);
	policy.version = FSCRYPT_V2;
	policy.contents_mode = EXT4_FSCRYPT_MODE_AES_256_XTS;
	policy.filenames_mode = EXT4_FSCRYPT_MODE_AES_256_CTS;
	memcpy(policy.identifier, crypto.keyring.identifier, sizeof(policy.identifier));
	memcpy(before, device->cache, device->size);
	writes = device->writes;
	crypto_failure_select(fs, &crypto, CRYPTO_RANDOM, 1);
	EXPECT(ext4_set_encryption_policy(
		   fs, directory.number, directory.generation, &policy, &result),
	    EXT4_IO);
	CHECK(device->writes == writes && memcmp(before, device->cache, device->size) == 0);
	crypto_failure_select(fs, &crypto, CRYPTO_WORKING, 0);
	EXPECT(ext4_set_encryption_policy(
		   fs, directory.number, directory.generation, &policy, &directory),
	    EXT4_OK);
	/* Every provider failure must cancel an encrypted create before device writes. */
	for (failure = CRYPTO_FIND; failure <= CRYPTO_RANDOM; failure++) {
		crypto_failure_select(fs, &crypto, failure, 1);
		memcpy(before, device->cache, device->size);
		writes = device->writes;
		EXPECT(ext4_create(fs, directory.number, directory.generation,
			   (const uint8_t *)"failure", 7, &update, &encrypt_time, &result),
		    EXT4_IO);
		CHECK(crypto.calls >= crypto.fail_at && device->writes == writes &&
		    memcmp(before, device->cache, device->size) == 0);
	}
	crypto_failure_select(fs, &crypto, CRYPTO_WORKING, 0);
	EXPECT(ext4_create(fs, directory.number, directory.generation, (const uint8_t *)"data", 4,
		   &update, &encrypt_time, &inode),
	    EXT4_OK);
	for (index = 0; index < length; index++) {
		plain[index] = (uint8_t)(index * 131U + 17U);
	}
	update = data_update(&inode);
	EXPECT(
	    ext4_write(fs, inode.number, inode.generation, 0, plain, length, &update, &completed),
	    EXT4_OK);
	CHECK(completed == length);
	EXPECT(ext4_get_inode(fs, inode.number, &inode), EXT4_OK);
	for (failure = CRYPTO_FIND; failure <= CRYPTO_CIPHER; failure++) {
		crypto_failure_select(fs, &crypto, failure, failure == CRYPTO_CIPHER ? 2U : 1U);
		memset(read_back, 0xcc, length);
		EXPECT(ext4_read(fs, &inode, 0, read_back, length, &completed), EXT4_IO);
		CHECK(completed == (failure == CRYPTO_CIPHER ? device->block_size : 0));
		CHECK(memcmp(read_back, plain, completed) == 0);
		for (index = completed; index < length; index++) {
			CHECK(read_back[index] == 0xcc);
		}
	}
	/* A partially encrypted private snapshot must never reach the device. */
	crypto_failure_select(fs, &crypto, CRYPTO_CIPHER, 2);
	memcpy(before, device->cache, device->size);
	writes = device->writes;
	memset(read_back, 0x33, length);
	EXPECT(ext4_write(
		   fs, inode.number, inode.generation, 0, read_back, length, &update, &completed),
	    EXT4_IO);
	CHECK(completed == 0 && device->writes == writes &&
	    memcmp(before, device->cache, device->size) == 0);
	for (index = 0; index < length; index++) {
		CHECK(read_back[index] == 0x33U);
	}
	crypto_failure_select(fs, &crypto, CRYPTO_WORKING, 0);
	EXPECT(ext4_read(fs, &inode, 0, read_back, length, &completed), EXT4_OK);
	CHECK(completed == length && memcmp(read_back, plain, length) == 0);
	crypto_failure_select(fs, &crypto, CRYPTO_CIPHER, 0);
	allocations = device->allocations;
	EXPECT(
	    ext4_write(fs, inode.number, inode.generation, 0, plain, length, &update, &completed),
	    EXT4_OK);
	CHECK(completed == length && crypto.calls == 3U);
	printf("PASS unaligned encrypted full overwrite: %u allocations, %u cipher calls\n",
	    device->allocations - allocations, crypto.calls);
	EXPECT(ext4_read(fs, &inode, 0, read_back, length, &completed), EXT4_OK);
	CHECK(completed == length && memcmp(read_back, plain, length) == 0);
	CHECK(plain_storage[0] == CRYPTO_BUFFER_GUARD &&
	    plain_storage[length + 1U] == CRYPTO_BUFFER_GUARD);
	CHECK(read_storage[0] == CRYPTO_BUFFER_GUARD &&
	    read_storage[length + 1U] == CRYPTO_BUFFER_GUARD);
	ext4_unmount(fs);
	CHECK(crypto.keyring.handles == 0 && device->live == 0);
	free(read_storage);
	free(plain_storage);
	free(before);
	puts("PASS crypto callback errors preserve images, key ownership and completed reads");
}

#endif

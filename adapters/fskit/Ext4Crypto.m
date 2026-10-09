/* SPDX-License-Identifier: BSD-3-Clause */
#include "Ext4Crypto.h"
#include "Ext4SipHash.h"
#include <CommonCrypto/CommonCryptor.h>
#include <CommonCrypto/CommonHMAC.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#define EXT4_NATIVE_AES_BLOCK kCCBlockSizeAES128
#define EXT4_NATIVE_SHA512_SIZE 64U
#define EXT4_NATIVE_INFO_MAX 256U
#define EXT4_NATIVE_CIPHER_MAX 65536U
#define EXT4_NATIVE_XTS_BATCH 1024U

struct ext4_native_key {
	uint8_t bytes[EXT4_NATIVE_MASTER_SIZE];
	size_t size;
	size_t allocation;
	size_t references;
	bool master;
	CCCryptorRef encrypt;
	CCCryptorRef decrypt;
	CCCryptorRef auxiliary;
};

struct ext4_native_master {
	struct ext4_native_key *key;
	uint8_t identifier[EXT4_NATIVE_IDENTIFIER_SIZE];
	uint8_t version;
};

struct ext4_native_crypto {
	struct ext4_native_master masters[EXT4_NATIVE_MAX_KEYS];
	size_t count;
	bool sealed;
};

static void
ext4_native_wipe(void *memory, size_t size)
{
	volatile uint8_t *bytes = memory;

	while (size != 0) {
		*bytes++ = 0;
		size--;
	}
}

static struct ext4_native_key *
ext4_native_key_create(void)
{
	long page = sysconf(_SC_PAGESIZE);
	struct ext4_native_key *key;

	if (page <= 0 || (size_t)page < sizeof(*key)) {
		return NULL;
	}
	key = mmap(NULL, (size_t)page, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
	if (key == MAP_FAILED) {
		return NULL;
	}
	/* Dedicated pages avoid munlock accidentally unlocking another live key. */
	if (mlock(key, (size_t)page) != 0) {
		munmap(key, (size_t)page);
		return NULL;
	}
	key->allocation = (size_t)page;
	key->references = 1;
	return key;
}

static void
ext4_native_key_release(void *context, void *handle)
{
	struct ext4_native_key *key = handle;
	size_t allocation;

	(void)context;
	if (key == NULL || --key->references != 0) {
		return;
	}
	allocation = key->allocation;
	if (key->encrypt != NULL) {
		CCCryptorRelease(key->encrypt);
	}
	if (key->decrypt != NULL) {
		CCCryptorRelease(key->decrypt);
	}
	if (key->auxiliary != NULL) {
		CCCryptorRelease(key->auxiliary);
	}
	ext4_native_wipe(key, allocation);
	munlock(key, allocation);
	munmap(key, allocation);
}

/* fscrypt only requests up to one SHA-512 output block from RFC 5869. */
static void
ext4_native_hkdf(const void *master, size_t size, const void *info, size_t info_size, void *output,
    size_t output_size)
{
	uint8_t salt[EXT4_NATIVE_SHA512_SIZE] = { 0 };
	uint8_t prk[EXT4_NATIVE_SHA512_SIZE];
	uint8_t digest[EXT4_NATIVE_SHA512_SIZE];
	uint8_t counter = 1;
	CCHmacContext context;

	CCHmac(kCCHmacAlgSHA512, salt, sizeof(salt), master, size, prk);
	CCHmacInit(&context, kCCHmacAlgSHA512, prk, sizeof(prk));
	CCHmacUpdate(&context, info, info_size);
	CCHmacUpdate(&context, &counter, sizeof(counter));
	CCHmacFinal(&context, digest);
	memcpy(output, digest, output_size);
	ext4_native_wipe(&context, sizeof(context));
	ext4_native_wipe(digest, sizeof(digest));
	ext4_native_wipe(prk, sizeof(prk));
}

enum ext4_result
ext4_native_crypto_identifier(
    const void *master, size_t size, uint8_t identifier[EXT4_NATIVE_IDENTIFIER_SIZE])
{
	static const uint8_t info[] = { 'f', 's', 'c', 'r', 'y', 'p', 't', 0, 1 };

	if (master == NULL || size != EXT4_NATIVE_MASTER_SIZE || identifier == NULL) {
		return EXT4_INVALID_ARGUMENT;
	}
	ext4_native_hkdf(master, size, info, sizeof(info), identifier, EXT4_NATIVE_IDENTIFIER_SIZE);
	return EXT4_OK;
}

struct ext4_native_crypto *
ext4_native_crypto_create(void)
{
	return calloc(1, sizeof(struct ext4_native_crypto));
}

void
ext4_native_crypto_destroy(struct ext4_native_crypto *crypto)
{
	size_t index;

	if (crypto == NULL) {
		return;
	}
	for (index = 0; index < crypto->count; index++) {
		ext4_native_key_release(NULL, crypto->masters[index].key);
	}
	free(crypto);
}

enum ext4_result
ext4_native_crypto_add(struct ext4_native_crypto *crypto, uint8_t version,
    const uint8_t *identifier, size_t identifier_size, const void *master, size_t master_size)
{
	uint8_t computed[EXT4_NATIVE_IDENTIFIER_SIZE];
	struct ext4_native_master *entry;
	struct ext4_native_key *key;
	size_t index;

	if (crypto == NULL || master == NULL || master_size != EXT4_NATIVE_MASTER_SIZE ||
	    identifier == NULL || (version != 1 && version != 2) ||
	    identifier_size !=
		(version == 1 ? EXT4_NATIVE_DESCRIPTOR_SIZE : EXT4_NATIVE_IDENTIFIER_SIZE)) {
		return EXT4_INVALID_ARGUMENT;
	}
	if (crypto->sealed) {
		return EXT4_BUSY;
	}
	if (version == 2) {
		ext4_native_crypto_identifier(master, master_size, computed);
		if (memcmp(identifier, computed, sizeof(computed)) != 0) {
			return EXT4_INVALID_ARGUMENT;
		}
	}
	for (index = 0; index < crypto->count; index++) {
		entry = &crypto->masters[index];
		if (entry->version == version &&
		    memcmp(entry->identifier, identifier, identifier_size) == 0) {
			return EXT4_EXISTS;
		}
	}
	if (crypto->count == EXT4_NATIVE_MAX_KEYS) {
		return EXT4_NO_SPACE;
	}
	key = ext4_native_key_create();
	if (key == NULL) {
		return EXT4_NO_MEMORY;
	}
	memcpy(key->bytes, master, master_size);
	key->size = master_size;
	key->master = true;
	entry = &crypto->masters[crypto->count++];
	entry->key = key;
	entry->version = version;
	memcpy(entry->identifier, identifier, identifier_size);
	return EXT4_OK;
}

void
ext4_native_crypto_seal(struct ext4_native_crypto *crypto)
{
	crypto->sealed = true;
}

size_t
ext4_native_crypto_count(const struct ext4_native_crypto *crypto)
{
	return crypto == NULL ? 0 : crypto->count;
}

static enum ext4_result
ext4_native_find(
    void *context, uint8_t version, const uint8_t *identifier, size_t size, void **master)
{
	struct ext4_native_crypto *crypto = context;
	struct ext4_native_master *entry;
	size_t index;

	if (master == NULL || identifier == NULL || (version != 1 && version != 2) ||
	    size != (version == 1 ? EXT4_NATIVE_DESCRIPTOR_SIZE : EXT4_NATIVE_IDENTIFIER_SIZE)) {
		return EXT4_INVALID_ARGUMENT;
	}
	for (index = 0; index < crypto->count; index++) {
		entry = &crypto->masters[index];
		if (entry->version == version && memcmp(entry->identifier, identifier, size) == 0) {
			entry->key->references++;
			*master = entry->key;
			return EXT4_OK;
		}
	}
	return EXT4_NOT_FOUND;
}

static enum ext4_result
ext4_native_key_prepare(struct ext4_native_key *key)
{
	CCOptions options = key->size == EXT4_NATIVE_MASTER_SIZE ? kCCOptionECBMode : 0;
	CCCryptorStatus status;

	status = CCCryptorCreate(kCCEncrypt, kCCAlgorithmAES, options, key->bytes, kCCKeySizeAES256,
	    NULL, &key->encrypt);
	if (status == kCCSuccess) {
		status = CCCryptorCreate(kCCDecrypt, kCCAlgorithmAES, options, key->bytes,
		    kCCKeySizeAES256, NULL, &key->decrypt);
	}
	if (status == kCCSuccess) {
		status =
		    CCCryptorCreate(key->size == EXT4_NATIVE_MASTER_SIZE ? kCCEncrypt : kCCDecrypt,
			kCCAlgorithmAES, kCCOptionECBMode,
			key->bytes + (key->size == EXT4_NATIVE_MASTER_SIZE ? kCCKeySizeAES256 : 0),
			kCCKeySizeAES256, NULL, &key->auxiliary);
	}
	return status == kCCSuccess ? EXT4_OK
				    : (status == kCCMemoryFailure ? EXT4_NO_MEMORY : EXT4_IO);
}

static enum ext4_result
ext4_native_derive(void *context, void *master, uint8_t version, const uint8_t *info,
    size_t info_size, size_t key_size, void **result)
{
	struct ext4_native_key *source = master;
	struct ext4_native_key *key;
	size_t completed = 0;
	CCCryptorStatus status;
	enum ext4_result error;

	(void)context;
	if (source == NULL || !source->master || result == NULL || info == NULL ||
	    (version != 1 && version != 2) || info_size == 0 || info_size > EXT4_NATIVE_INFO_MAX ||
	    (version == 1 && info_size != EXT4_NATIVE_AES_BLOCK) ||
	    (key_size != kCCKeySizeAES256 && key_size != EXT4_NATIVE_MASTER_SIZE &&
		!(version == 2 && key_size == EXT4_NATIVE_SIPHASH_KEY_BYTES))) {
		return EXT4_INVALID_ARGUMENT;
	}
	key = ext4_native_key_create();
	if (key == NULL) {
		return EXT4_NO_MEMORY;
	}
	key->size = key_size;
	if (version == 2) {
		ext4_native_hkdf(
		    source->bytes, source->size, info, info_size, key->bytes, key_size);
	} else {
		status =
		    CCCrypt(kCCEncrypt, kCCAlgorithmAES, kCCOptionECBMode, info, kCCKeySizeAES128,
			NULL, source->bytes, key_size, key->bytes, key_size, &completed);
		if (status != kCCSuccess || completed != key_size) {
			ext4_native_key_release(NULL, key);
			return status == kCCMemoryFailure ? EXT4_NO_MEMORY : EXT4_IO;
		}
	}
	error = key_size == EXT4_NATIVE_SIPHASH_KEY_BYTES ? EXT4_OK : ext4_native_key_prepare(key);
	if (error != EXT4_OK) {
		ext4_native_key_release(NULL, key);
		return error;
	}
	*result = key;
	return EXT4_OK;
}

static bool
ext4_native_aes(CCCryptorRef cryptor, const void *input, void *output, size_t size)
{
	size_t completed = 0;

	return size == 0 ||
	    (CCCryptorUpdate(cryptor, input, size, output, size, &completed) == kCCSuccess &&
		completed == size);
}

static void
ext4_native_xts_advance(uint8_t tweak[EXT4_NATIVE_AES_BLOCK])
{
	unsigned int index;
	uint8_t carry = 0;
	uint8_t next;

	for (index = 0; index < EXT4_NATIVE_AES_BLOCK; index++) {
		next = tweak[index] >> 7;
		tweak[index] = (uint8_t)((tweak[index] << 1) | carry);
		carry = next;
	}
	tweak[0] ^= (uint8_t)(0x87U & (0U - carry));
}

static enum ext4_result
ext4_native_xts(struct ext4_native_key *key, bool encrypt, const uint8_t *iv, const uint8_t *input,
    uint8_t *output, size_t length)
{
	uint8_t tweak[EXT4_NATIVE_AES_BLOCK];
	uint8_t masks[EXT4_NATIVE_XTS_BATCH];
	size_t amount;
	size_t offset;
	size_t index;
	enum ext4_result error = EXT4_IO;

	if (!ext4_native_aes(key->auxiliary, iv, tweak, sizeof(tweak))) {
		goto done;
	}
	while (length != 0) {
		amount = length < sizeof(masks) ? length : sizeof(masks);
		for (offset = 0; offset < amount; offset += EXT4_NATIVE_AES_BLOCK) {
			memcpy(masks + offset, tweak, sizeof(tweak));
			ext4_native_xts_advance(tweak);
		}
		for (index = 0; index < amount; index++) {
			output[index] = input[index] ^ masks[index];
		}
		if (!ext4_native_aes(
			encrypt ? key->encrypt : key->decrypt, output, output, amount)) {
			goto done;
		}
		for (index = 0; index < amount; index++) {
			output[index] ^= masks[index];
		}
		input += amount;
		output += amount;
		length -= amount;
	}
	error = EXT4_OK;
done:
	ext4_native_wipe(tweak, sizeof(tweak));
	ext4_native_wipe(masks, sizeof(masks));
	return error;
}

/* Linux cts(cbc(aes)) uses CS3, including swapping the final two full
 * ciphertext blocks when the input length is an exact block multiple. */
static enum ext4_result
ext4_native_cts(struct ext4_native_key *key, bool encrypt, const uint8_t *iv, const uint8_t *input,
    uint8_t *output, size_t length)
{
	CCCryptorRef cryptor = encrypt ? key->encrypt : key->decrypt;
	uint8_t penultimate[EXT4_NATIVE_AES_BLOCK] = { 0 };
	uint8_t last[EXT4_NATIVE_AES_BLOCK] = { 0 };
	uint8_t decoded[EXT4_NATIVE_AES_BLOCK];
	size_t tail = (length - 1) % EXT4_NATIVE_AES_BLOCK + 1;
	size_t prefix;
	enum ext4_result error = EXT4_IO;

	if (CCCryptorReset(cryptor, iv) != kCCSuccess) {
		goto done;
	}
	if (length == EXT4_NATIVE_AES_BLOCK) {
		error = ext4_native_aes(cryptor, input, output, length) ? EXT4_OK : EXT4_IO;
		goto done;
	}
	prefix = length - tail - EXT4_NATIVE_AES_BLOCK;
	if (!ext4_native_aes(cryptor, input, output, prefix)) {
		goto done;
	}
	input += prefix;
	output += prefix;
	if (encrypt) {
		memcpy(last, input + EXT4_NATIVE_AES_BLOCK, tail);
		if (!ext4_native_aes(cryptor, input, penultimate, sizeof(penultimate)) ||
		    !ext4_native_aes(cryptor, last, output, sizeof(last))) {
			goto done;
		}
		memcpy(output + EXT4_NATIVE_AES_BLOCK, penultimate, tail);
	} else {
		if (!ext4_native_aes(key->auxiliary, input, penultimate, sizeof(penultimate))) {
			goto done;
		}
		memcpy(penultimate, input + EXT4_NATIVE_AES_BLOCK, tail);
		if (!ext4_native_aes(cryptor, penultimate, output, sizeof(penultimate)) ||
		    !ext4_native_aes(cryptor, input, decoded, sizeof(decoded))) {
			goto done;
		}
		memcpy(output + EXT4_NATIVE_AES_BLOCK, decoded, tail);
	}
	error = EXT4_OK;
done:
	ext4_native_wipe(penultimate, sizeof(penultimate));
	ext4_native_wipe(last, sizeof(last));
	ext4_native_wipe(decoded, sizeof(decoded));
	return error;
}

static enum ext4_result
ext4_native_cipher(void *context, void *handle, uint8_t mode, bool encrypt, const uint8_t *iv,
    const void *input, void *output, size_t length)
{
	struct ext4_native_key *key = handle;
	uintptr_t source = (uintptr_t)input;
	uintptr_t target = (uintptr_t)output;

	(void)context;
	if (key == NULL || key->master || iv == NULL || input == NULL || output == NULL ||
	    length < EXT4_NATIVE_AES_BLOCK || length > EXT4_NATIVE_CIPHER_MAX ||
	    source > UINTPTR_MAX - length || target > UINTPTR_MAX - length ||
	    (source < target + length && target < source + length)) {
		return EXT4_INVALID_ARGUMENT;
	}
	if (mode == EXT4_FSCRYPT_MODE_AES_256_XTS) {
		if (key->size != EXT4_NATIVE_MASTER_SIZE || length % EXT4_NATIVE_AES_BLOCK != 0) {
			return EXT4_INVALID_ARGUMENT;
		}
		return ext4_native_xts(key, encrypt, iv, input, output, length);
	}
	if (mode == EXT4_FSCRYPT_MODE_AES_256_CTS) {
		if (key->size != kCCKeySizeAES256) {
			return EXT4_INVALID_ARGUMENT;
		}
		return ext4_native_cts(key, encrypt, iv, input, output, length);
	}
	return EXT4_UNSUPPORTED;
}

static enum ext4_result
ext4_native_siphash(void *context, void *handle, const uint8_t *bytes, size_t length,
    uint64_t *result)
{
	struct ext4_native_key *key = handle;
	uint64_t hash;
	uintptr_t source = (uintptr_t)bytes;

	(void)context;
	if (key == NULL || key->master || key->size != EXT4_NATIVE_SIPHASH_KEY_BYTES ||
	    result == NULL || (bytes == NULL && length != 0) || source > UINTPTR_MAX - length) {
		return EXT4_INVALID_ARGUMENT;
	}
	hash = ext4_native_siphash_bytes(key->bytes, bytes, length);
	*result = hash;
	return EXT4_OK;
}

static enum ext4_result
ext4_native_random(void *context, void *buffer, size_t length)
{
	(void)context;
	if (buffer == NULL && length != 0) {
		return EXT4_INVALID_ARGUMENT;
	}
	arc4random_buf(buffer, length);
	return EXT4_OK;
}

struct ext4_crypto_environment
ext4_native_crypto_environment(struct ext4_native_crypto *crypto)
{
	struct ext4_crypto_environment environment = { 0 };

	environment.context = crypto;
	environment.find_key = ext4_native_find;
	environment.derive_key = ext4_native_derive;
	environment.cipher = ext4_native_cipher;
	environment.release_key = ext4_native_key_release;
	environment.random_bytes = ext4_native_random;
	environment.siphash = ext4_native_siphash;
	return environment;
}

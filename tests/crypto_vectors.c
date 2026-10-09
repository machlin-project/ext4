/* SPDX-License-Identifier: BSD-3-Clause */
#include "crypto.h"
#include "siphash.h"

#include <stdio.h>
#include <stdlib.h>

/* Known answers for the tests' reference cryptography. AES matches FIPS-197
 * appendix C. The XTS, CBC-CTS and HKDF answers were computed with OpenSSL: XTS from
 * its AES-ECB with the IEEE 1619 tweak arithmetic, CTS from its AES-CBC with the last
 * two blocks swapped and truncated, and HKDF from its HKDF-SHA512. The fscrypt key
 * identifier of the Linux probe's master key is the one Linux 6.12 stored in the
 * encryption context of the probe's tree. */

#define CHECK(expression)                                                                          \
	do {                                                                                       \
		if (!(expression)) {                                                               \
			fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #expression);           \
			exit(1);                                                                   \
		}                                                                                  \
	} while (0)

#define XTS_BYTES 4096U
#define XTS_INDEX 5U
#define FSCRYPT_INFO_PREFIX "fscrypt"
#define FSCRYPT_INFO_PREFIX_SIZE 8U
#define FSCRYPT_KEY_IDENTIFIER_CONTEXT 1U
#define FSCRYPT_PER_FILE_KEY_CONTEXT 2U
#define FSCRYPT_NONCE_BYTES 16U
#define MASTER_KEY_BYTES 64U

static void
hex(const char *text, uint8_t *output, size_t length)
{
	unsigned int value;
	size_t index;

	CHECK(strlen(text) == 2U * length);
	for (index = 0; index < length; index++) {
		CHECK(sscanf(text + 2U * index, "%2x", &value) == 1);
		output[index] = (uint8_t)value;
	}
}

static void
sha256_hex(const uint8_t *data, size_t length, const char *expected)
{
	struct ext4_sha256 context;
	uint8_t digest[EXT4_SHA256_DIGEST_SIZE];
	uint8_t wanted[EXT4_SHA256_DIGEST_SIZE];

	ext4_sha256_init(&context);
	ext4_sha256_update(&context, data, length);
	ext4_sha256_final(&context, digest);
	hex(expected, wanted, sizeof(wanted));
	CHECK(memcmp(digest, wanted, sizeof(digest)) == 0);
}

static void
aes_blocks(void)
{
	static const char *const answers[2] = { "69c4e0d86a7b0430d8cdb78070b4c55a",
		"8ea2b7ca516745bfeafc49904b496089" };
	struct test_aes aes;
	uint8_t key[32];
	uint8_t plain[TEST_AES_BLOCK];
	uint8_t cipher[TEST_AES_BLOCK];
	uint8_t wanted[TEST_AES_BLOCK];
	uint8_t back[TEST_AES_BLOCK];
	unsigned int variant;
	unsigned int index;

	hex("00112233445566778899aabbccddeeff", plain, sizeof(plain));
	for (variant = 0; variant < 2U; variant++) {
		for (index = 0; index < sizeof(key); index++) {
			key[index] = (uint8_t)index;
		}
		test_aes_init(&aes, key, variant == 0 ? 16U : 32U);
		test_aes_encrypt(&aes, plain, cipher);
		hex(answers[variant], wanted, sizeof(wanted));
		CHECK(memcmp(cipher, wanted, sizeof(cipher)) == 0);
		test_aes_decrypt(&aes, cipher, back);
		CHECK(memcmp(back, plain, sizeof(plain)) == 0);
	}
}

static void
xts(void)
{
	uint8_t key[64];
	uint8_t tweak[TEST_AES_BLOCK] = { XTS_INDEX };
	uint8_t *plain = malloc(XTS_BYTES);
	uint8_t *cipher = malloc(XTS_BYTES);
	uint8_t *back = malloc(XTS_BYTES);
	unsigned int index;

	CHECK(plain != NULL && cipher != NULL && back != NULL);
	for (index = 0; index < sizeof(key); index++) {
		key[index] = (uint8_t)(index * 3U + 1U);
	}
	for (index = 0; index < XTS_BYTES; index++) {
		plain[index] = (uint8_t)(index * 11U + 5U);
	}
	test_aes_xts(key, sizeof(key), tweak, true, plain, cipher, XTS_BYTES);
	sha256_hex(
	    cipher, XTS_BYTES, "211d0b2fc5e5a2ed9b62aebad20654cb81cf4ee8c5b47e0e2ebd2f881a9d1e65");
	test_aes_xts(key, sizeof(key), tweak, false, cipher, back, XTS_BYTES);
	CHECK(memcmp(back, plain, XTS_BYTES) == 0);
	free(back);
	free(cipher);
	free(plain);
}

static void
cts(void)
{
	static const struct {
		size_t length;
		const char *answer;
		bool digest;
	} vectors[] = {
		{ 16, "9c19065bea3335a1bebbfa4b6db3b81b", false },
		{ 17, "36923aaeec59fe51fc0a8a8fdc8829359c", false },
		{ 31, "855045ebe920296cbd1f39198affb0e89c19065bea3335a1bebbfa4b6db3b8", false },
		{ 32, "582205cba7c97dedd9504e164e337e039c19065bea3335a1bebbfa4b6db3b81b", false },
		{ 48,
		    "9c19065bea3335a1bebbfa4b6db3b81b8649035cda1191fc3dea1ac646ca47dc582205cba7c97d"
		    "edd9504e164e337e03",
		    false },
		{ 255, "24229f47b319d65dd4163b302813954ffe08d09c9184119c2c0df07ca5a1f418", true },
	};

	uint8_t key[32];
	uint8_t iv[TEST_AES_BLOCK] = { 0 };
	uint8_t plain[256];
	uint8_t cipher[256];
	uint8_t wanted[256];
	uint8_t back[256];
	unsigned int index;
	unsigned int vector;

	for (index = 0; index < sizeof(key); index++) {
		key[index] = (uint8_t)(index * 5U + 2U);
	}
	for (index = 0; index < sizeof(plain); index++) {
		plain[index] = (uint8_t)(index * 13U + 1U);
	}
	for (vector = 0; vector < sizeof(vectors) / sizeof(vectors[0]); vector++) {
		test_aes_cts(key, sizeof(key), iv, true, plain, cipher, vectors[vector].length);
		if (vectors[vector].digest) {
			sha256_hex(cipher, vectors[vector].length, vectors[vector].answer);
		} else {
			hex(vectors[vector].answer, wanted, vectors[vector].length);
			CHECK(memcmp(cipher, wanted, vectors[vector].length) == 0);
		}
		test_aes_cts(key, sizeof(key), iv, false, cipher, back, vectors[vector].length);
		CHECK(memcmp(back, plain, vectors[vector].length) == 0);
	}
}

static void
hkdf(void)
{
	uint8_t master[MASTER_KEY_BYTES];
	uint8_t info[FSCRYPT_INFO_PREFIX_SIZE + 1U + FSCRYPT_NONCE_BYTES];
	uint8_t output[64];
	uint8_t wanted[64];
	unsigned int index;

	/* The Linux encryption probe's raw master key. */
	for (index = 0; index < sizeof(master); index++) {
		master[index] = (uint8_t)(index * 7U + 3U);
	}
	memcpy(info, FSCRYPT_INFO_PREFIX, FSCRYPT_INFO_PREFIX_SIZE);
	info[FSCRYPT_INFO_PREFIX_SIZE] = FSCRYPT_KEY_IDENTIFIER_CONTEXT;
	test_hkdf_sha512(master, sizeof(master), info, FSCRYPT_INFO_PREFIX_SIZE + 1U, output, 16);
	hex("a525b310d975604e26c761134e6c35d1", wanted, 16);
	CHECK(memcmp(output, wanted, 16) == 0);
	info[FSCRYPT_INFO_PREFIX_SIZE] = FSCRYPT_PER_FILE_KEY_CONTEXT;
	for (index = 0; index < FSCRYPT_NONCE_BYTES; index++) {
		info[FSCRYPT_INFO_PREFIX_SIZE + 1U + index] = (uint8_t)index;
	}
	test_hkdf_sha512(master, sizeof(master), info, sizeof(info), output, sizeof(output));
	hex("511bfdcdebee121406a8be826a2d89f084c858eda59deb8a505e6eff314af29ec103164cef7fee209a04e9"
	    "b96b1832b948af38fdcf7e66be8e8200fe513d1a6a",
	    wanted, sizeof(wanted));
	CHECK(memcmp(output, wanted, sizeof(output)) == 0);
}

static void
siphash(void)
{
	static const uint8_t lengths[] = { 0, 1, 7, 8, 15, 16 };
	static const uint64_t answers[] = { UINT64_C(0x726fdb47dd0e0e31),
		UINT64_C(0x74f839c593dc67fd), UINT64_C(0xab0200f58b01d137),
		UINT64_C(0x93f5f5799a932462), UINT64_C(0xa129ca6149be45e5),
		UINT64_C(0x3f2acc7f57c29bdb) };
	uint8_t key[16];
	uint8_t message[16];
	size_t index;

	/* SipHash authors' published CC0 vectors_sip64, also checked by the
	 * independent EVP filename generator. */
	for (index = 0; index < sizeof(key); index++) {
		key[index] = message[index] = (uint8_t)index;
	}
	for (index = 0; index < sizeof(lengths); index++) {
		CHECK(test_siphash24(key, message, lengths[index]) == answers[index]);
	}
}

int
main(void)
{
	aes_blocks();
	xts();
	cts();
	hkdf();
	siphash();
	printf("PASS reference AES, XTS, CBC-CTS, HKDF-SHA512 and SipHash-2-4 known answers\n");
	return 0;
}

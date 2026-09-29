/* SPDX-License-Identifier: BSD-3-Clause */
#include "sha.h"
#include "fscrypt.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(expression)                                                                          \
	do {                                                                                       \
		if (!(expression)) {                                                               \
			fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #expression);           \
			exit(1);                                                                   \
		}                                                                                  \
	} while (0)

struct hash_vector {
	size_t length;
	const char *sha256;
	const char *sha512;
};

/* Independent hashlib answers for byte[i] = (131 * i + 17) modulo 256. */
#include "hash_vectors.h"

static void
decode_digest(const char *text, uint8_t *bytes, size_t length)
{
	unsigned int value;
	size_t index;

	for (index = 0; index < length; index++) {
		CHECK(sscanf(text + 2U * index, "%2x", &value) == 1);
		bytes[index] = (uint8_t)value;
	}
}

static void
hash_streams(void)
{
	struct ext4_sha256 sha256;
	struct ext4_sha512 sha512;
	uint8_t digest[EXT4_SHA512_DIGEST_SIZE];
	uint8_t expected256[EXT4_SHA256_DIGEST_SIZE];
	uint8_t expected512[EXT4_SHA512_DIGEST_SIZE];
	uint8_t *allocation;
	uint8_t *bytes;
	size_t vector;
	size_t alignment;
	size_t length;
	size_t cut;
	size_t index;

	for (vector = 0; vector < sizeof(hash_vectors) / sizeof(hash_vectors[0]); vector++) {
		length = hash_vectors[vector].length;
		decode_digest(hash_vectors[vector].sha256, expected256, sizeof(expected256));
		decode_digest(hash_vectors[vector].sha512, expected512, sizeof(expected512));
		for (alignment = 0; alignment < 16U; alignment++) {
			/* The input ends at the allocation boundary, exposing overreads to ASan. */
			allocation = malloc(length + alignment + (length == 0));
			CHECK(allocation != NULL);
			bytes = allocation + alignment;
			for (index = 0; index < length; index++) {
				bytes[index] = (uint8_t)(131U * index + 17U);
			}
			for (cut = 0; cut <= length && cut <= EXT4_SHA512_BLOCK_SIZE + 1U; cut++) {
				ext4_sha256_init(&sha256);
				ext4_sha256_update(&sha256, bytes, cut);
				ext4_sha256_update(&sha256, NULL, 0);
				ext4_sha256_update(&sha256, bytes + cut, length - cut);
				ext4_sha256_final(&sha256, digest);
				CHECK(memcmp(digest, expected256, sizeof(expected256)) == 0);
				ext4_sha512_init(&sha512);
				ext4_sha512_update(&sha512, bytes, cut);
				ext4_sha512_update(&sha512, NULL, 0);
				ext4_sha512_update(&sha512, bytes + cut, length - cut);
				ext4_sha512_final(&sha512, digest);
				CHECK(memcmp(digest, expected512, sizeof(expected512)) == 0);
			}
			ext4_sha256_init(&sha256);
			ext4_sha512_init(&sha512);
			for (index = 0; index < length; index++) {
				ext4_sha256_update(&sha256, bytes + index, 1);
				ext4_sha512_update(&sha512, bytes + index, 1);
			}
			ext4_sha256_final(&sha256, digest);
			CHECK(memcmp(digest, expected256, sizeof(expected256)) == 0);
			ext4_sha512_final(&sha512, digest);
			CHECK(memcmp(digest, expected512, sizeof(expected512)) == 0);
			free(allocation);
		}
	}
}

static void
nokey_names(void)
{
	struct ext4_fscrypt_nokey decoded;
	uint8_t cipher[EXT4_NAME_MAX];
	uint8_t name[EXT4_FSCRYPT_NOKEY_NAME_MAX + 1U];
	uint8_t canonical[sizeof(name)];
	size_t length;
	size_t size;
	size_t index;
	unsigned int byte;
	bool alphabet;

	for (index = 0; index < sizeof(cipher); index++) {
		cipher[index] = (uint8_t)(131U * index + 17U);
	}
	for (length = EXT4_FSCRYPT_NAME_MIN; length <= sizeof(cipher); length++) {
		size = ext4_fscrypt_nokey_encode(cipher, length, 0x12345678U, 0x9abcdef0U, name);
		CHECK(size <= EXT4_FSCRYPT_NOKEY_NAME_MAX);
		CHECK(ext4_fscrypt_nokey_decode(name, size, &decoded));
		CHECK(ext4_fscrypt_nokey_match(&decoded, cipher, length));
		if (size % 4U == 0) {
			/* Linux 6.12 accepts an extra zero sextet, within its name bound. */
			name[size] = 'A';
			CHECK(ext4_fscrypt_nokey_decode(name, size + 1U, &decoded) ==
			    (size < EXT4_FSCRYPT_NOKEY_NAME_MAX));
		}
		if (size % 4U != 0) {
			/* This final sextet has nonzero unused bits. */
			name[size - 1U] = '_';
			CHECK(!ext4_fscrypt_nokey_decode(name, size, &decoded));
		}
	}
	size = ext4_fscrypt_nokey_encode(cipher, EXT4_FSCRYPT_NAME_MIN, 0, 0, canonical);
	for (byte = 0; byte <= UINT8_MAX; byte++) {
		memcpy(name, canonical, size);
		name[0] = (uint8_t)byte;
		alphabet = (byte >= 'A' && byte <= 'Z') || (byte >= 'a' && byte <= 'z') ||
		    (byte >= '0' && byte <= '9') || byte == '-' || byte == '_';
		CHECK(ext4_fscrypt_nokey_decode(name, size, &decoded) == alphabet);
	}
}

int
main(void)
{
	hash_streams();
	nokey_names();
	puts("PASS independent SHA answers, streaming boundaries and Linux no-key names");
	return 0;
}

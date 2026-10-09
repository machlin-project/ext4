/* SPDX-License-Identifier: BSD-3-Clause */
#include "../adapters/fskit/Ext4SipHash.h"
#include "native_siphash_vectors.h"

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

static void
authors_vectors(void)
{
	/* Published CC0 SipHash authors' vectors_sip64; also verified independently
	 * by the EVP generator before it produces the derived-key vectors. */
	static const size_t lengths[] = { 0, 1, 7, 8, 15, 16 };
	static const uint64_t answers[] = { UINT64_C(0x726fdb47dd0e0e31),
		UINT64_C(0x74f839c593dc67fd), UINT64_C(0xab0200f58b01d137),
		UINT64_C(0x93f5f5799a932462), UINT64_C(0xa129ca6149be45e5),
		UINT64_C(0x3f2acc7f57c29bdb) };
	uint8_t key[17];
	uint8_t message[17];
	size_t index;

	for (index = 0; index < 16; index++) {
		key[index + 1] = message[index + 1] = (uint8_t)index;
	}
	for (index = 0; index < sizeof(lengths) / sizeof(lengths[0]); index++) {
		CHECK(ext4_native_siphash_bytes(key + 1, message + 1, lengths[index]) == answers[index]);
	}
	CHECK(ext4_native_siphash_bytes(key + 1, NULL, 0) == answers[0]);
}

int
main(void)
{
	uint8_t *message = malloc(NATIVE_SIPHASH_INPUT_MAX + 2U);
	uint8_t *before = malloc(NATIVE_SIPHASH_INPUT_MAX + 2U);
	size_t index;
	size_t profile;
	size_t sample;
	uint64_t hash;

	CHECK(message != NULL && before != NULL);
	authors_vectors();
	message[0] = message[NATIVE_SIPHASH_INPUT_MAX + 1U] = 0xa5;
	for (index = 0; index < NATIVE_SIPHASH_INPUT_MAX; index++) {
		message[index + 1U] = (uint8_t)(index * 11U + 3U);
	}
	memcpy(before, message, NATIVE_SIPHASH_INPUT_MAX + 2U);
	for (profile = 0; profile < sizeof(native_siphash_vectors) / sizeof(native_siphash_vectors[0]);
	     profile++) {
		const struct native_siphash_vector *vector = &native_siphash_vectors[profile];

		for (sample = 0; sample < NATIVE_SIPHASH_LENGTHS; sample++) {
			hash = ext4_native_siphash_bytes(vector->key, message + 1, native_siphash_lengths[sample]);
			CHECK(hash == vector->answers[sample]);
			CHECK(ext4_native_siphash_bytes(vector->key, message + 1,
			    native_siphash_lengths[sample]) == hash);
			CHECK(memcmp(before, message, NATIVE_SIPHASH_INPUT_MAX + 2U) == 0);
		}
		CHECK(ext4_native_siphash_bytes(vector->key, NULL, 0) == vector->answers[0]);
	}
	free(before);
	free(message);
	puts("PASS native SipHash primitive: 6 author vectors, 207 EVP compositions, unaligned/empty/expanded inputs");
	return 0;
}

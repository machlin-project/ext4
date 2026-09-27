/* SPDX-License-Identifier: BSD-3-Clause */
#include "internal.h"

#include <stdio.h>
#include <stdlib.h>

#define CHECKSUM_MAX_LENGTH 65536U
#define CHECKSUM_ALIGNMENTS 16U

#define CHECK(expression)                                                                          \
	do {                                                                                       \
		if (!(expression)) {                                                               \
			fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #expression);           \
			exit(EXIT_FAILURE);                                                        \
		}                                                                                  \
	} while (0)

/* Independent bit-serial division. This deliberately uses neither the core's
 * remainder table nor its byte-at-a-time update. */
static uint32_t
reference_crc(uint32_t state, const uint8_t *bytes, size_t length)
{
	size_t index;
	unsigned int bit;
	bool carry;

	for (index = 0; index < length; index++) {
		for (bit = 0; bit < EXT4_BITS_PER_BYTE; bit++) {
			carry = ((state ^ (bytes[index] >> bit)) & 1U) != 0;
			state >>= 1;
			if (carry) {
				state ^= EXT4_CRC32C_POLYNOMIAL;
			}
		}
	}
	return state;
}

int
main(void)
{
	static const size_t lengths[] = { 0, 1, 2, 3, 4, 7, 8, 9, 15, 16, 17, 31, 32, 33, 63, 64,
		65, 255, 256, 257, 511, 512, 513, 1023, 1024, 1025, 4095, 4096, 4097, 65535,
		65536 };
	static const size_t strides[] = { 1, 3, 17, 253, 1024, 4096, 65535 };
	static const uint32_t seeds[] = { 0, UINT32_MAX, 1, 0x80000000U, 0x01234567U, 0x89abcdefU };
	uint8_t *bytes = malloc(CHECKSUM_MAX_LENGTH + CHECKSUM_ALIGNMENTS);
	uint8_t byte;
	uint32_t expected;
	uint32_t state;
	size_t alignment;
	size_t length;
	size_t seed;
	size_t stride;
	size_t offset;
	size_t chunk;
	size_t index;
	size_t cases = 0;

	CHECK(bytes != NULL);
	/* Conventional CRC32C check value, with the complement kept outside this
	 * raw-state API as it is for ext4's UUID/inode/block checksum chains. */
	CHECK(ext4_crc32c(UINT32_MAX, "123456789", 9) == (uint32_t)~UINT32_C(0xe3069283));
	for (index = 0; index <= UINT8_MAX; index++) {
		byte = (uint8_t)index;
		CHECK(ext4_crc32c(0, &byte, 1) == reference_crc(0, &byte, 1));
	}
	for (index = 0; index < CHECKSUM_MAX_LENGTH + CHECKSUM_ALIGNMENTS; index++) {
		bytes[index] = (uint8_t)(index * 17U + index / 251U);
	}
	for (seed = 0; seed < sizeof(seeds) / sizeof(seeds[0]); seed++) {
		CHECK(ext4_crc32c(seeds[seed], NULL, 0) == seeds[seed]);
		for (alignment = 0; alignment < CHECKSUM_ALIGNMENTS; alignment++) {
			for (length = 0; length < sizeof(lengths) / sizeof(lengths[0]); length++) {
				expected =
				    reference_crc(seeds[seed], bytes + alignment, lengths[length]);
				CHECK(ext4_crc32c(seeds[seed], bytes + alignment,
					  lengths[length]) == expected);
				cases++;
			}
			expected =
			    reference_crc(seeds[seed], bytes + alignment, CHECKSUM_MAX_LENGTH);
			for (stride = 0; stride < sizeof(strides) / sizeof(strides[0]); stride++) {
				state = seeds[seed];
				for (offset = 0; offset < CHECKSUM_MAX_LENGTH; offset += chunk) {
					chunk = strides[stride];
					if (chunk > CHECKSUM_MAX_LENGTH - offset) {
						chunk = CHECKSUM_MAX_LENGTH - offset;
					}
					state =
					    ext4_crc32c(state, bytes + alignment + offset, chunk);
				}
				CHECK(state == expected);
				cases++;
			}
		}
	}
	free(bytes);
	printf("PASS CRC32C: known vector, all byte remainders, %zu seeded/aligned/range/stream "
	       "cases\n",
	    cases);
	return EXIT_SUCCESS;
}

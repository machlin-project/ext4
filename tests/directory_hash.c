/* SPDX-License-Identifier: BSD-3-Clause */
#include "internal.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define HASH_NAME_HEX_WIDTH 510
#define HASH_STRING_LITERAL(value) #value
#define HASH_STRING_VALUE(value) HASH_STRING_LITERAL(value)

_Static_assert(HASH_NAME_HEX_WIDTH == EXT4_NAME_MAX * 2U, "encoded filename scan width");

#define CHECK(expression)                                                                          \
	do {                                                                                       \
		if (!(expression)) {                                                               \
			fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #expression);           \
			exit(EXIT_FAILURE);                                                        \
		}                                                                                  \
	} while (0)

static uint8_t
hex_digit(char value)
{
	if (value >= '0' && value <= '9') {
		return (uint8_t)(value - '0');
	}
	CHECK(value >= 'a' && value <= 'f');
	return (uint8_t)(value - 'a' + 10);
}

static void
stream(void)
{
	char encoded[EXT4_NAME_MAX * 2U + 1U];
	uint8_t name[EXT4_NAME_MAX];
	uint32_t seed[4];
	struct ext4_name_hash hash;
	size_t length;
	size_t index;
	unsigned int version;
	int fields;

	for (;;) {
		fields = scanf("%u %" SCNx32 " %" SCNx32 " %" SCNx32 " %" SCNx32
			       " %" HASH_STRING_VALUE(HASH_NAME_HEX_WIDTH) "s",
		    &version, &seed[0], &seed[1], &seed[2], &seed[3], encoded);
		if (fields == EOF) {
			break;
		}
		CHECK(fields == 6 && version <= EXT4_HASH_TEA_UNSIGNED);
		length = strlen(encoded);
		CHECK(length != 0 && (length & 1U) == 0);
		length /= 2;
		for (index = 0; index < length; index++) {
			name[index] = (uint8_t)((hex_digit(encoded[index * 2]) << 4) |
			    hex_digit(encoded[index * 2 + 1]));
		}
		CHECK(ext4_directory_hash((uint8_t)version, seed, name, length, &hash) == EXT4_OK);
		printf("%08" PRIx32 " %08" PRIx32 "\n", hash.major, hash.minor);
	}
	CHECK(!ferror(stdin) && !ferror(stdout));
}

int
main(int argc, char **argv)
{
	/* Independently produced by e2fsprogs debugfs dx_hash, with its default seed. */
	static const struct ext4_name_hash expected[3] = { { 0x32252546U, 0 },
		{ 0x1746da32U, 0x420013b5U }, { 0x6f5bb1a8U, 0x231917c2U } };
	const uint8_t name[] = "hello";
	const uint32_t zero_seed[4] = { 0, 0, 0, 0 };
	struct ext4_name_hash result;
	struct ext4_name_hash empty;
	struct ext4_name_hash empty_name;
	struct ext4_name_hash untouched = { 0x12345678U, 0x87654321U };
	unsigned int version;

	if (argc == 2 && strcmp(argv[1], "--stream") == 0) {
		stream();
		return EXIT_SUCCESS;
	}
	CHECK(argc == 1);
	for (version = EXT4_HASH_LEGACY; version <= EXT4_HASH_TEA_UNSIGNED; version++) {
		CHECK(ext4_directory_hash(
			  (uint8_t)version, NULL, name, sizeof(name) - 1, &result) == EXT4_OK);
		CHECK(result.major == expected[version % 3].major &&
		    result.minor == expected[version % 3].minor);
		CHECK(ext4_directory_hash(
			  (uint8_t)version, zero_seed, name, sizeof(name) - 1, &result) == EXT4_OK);
		CHECK(result.major == expected[version % 3].major &&
		    result.minor == expected[version % 3].minor);
	}
	result = untouched;
	CHECK(ext4_directory_hash(EXT4_HASH_HALF_MD4, NULL, NULL, 1, &result) ==
	    EXT4_INVALID_ARGUMENT);
	/* A casefolded name may fold to nothing, and ext4 hashes that empty input. */
	CHECK(ext4_directory_hash(EXT4_HASH_HALF_MD4, NULL, NULL, 0, &empty) == EXT4_OK);
	CHECK(ext4_directory_hash(EXT4_HASH_HALF_MD4, NULL, name, 0, &empty_name) == EXT4_OK);
	CHECK(empty.major == empty_name.major && empty.minor == empty_name.minor);
	CHECK(ext4_directory_hash(EXT4_HASH_HALF_MD4, NULL, name, SIZE_MAX, &result) ==
	    EXT4_NAME_TOO_LONG);
	CHECK(
	    ext4_directory_hash(EXT4_HASH_HALF_MD4, NULL, name, 1, NULL) == EXT4_INVALID_ARGUMENT);
	CHECK(ext4_directory_hash(UINT8_MAX, NULL, name, 1, &result) == EXT4_UNSUPPORTED);
	CHECK(ext4_directory_hash(EXT4_HASH_TEA_UNSIGNED + 1U, NULL, name, 1, &result) ==
	    EXT4_UNSUPPORTED);
	CHECK(result.major == untouched.major && result.minor == untouched.minor);
	puts("PASS directory hash algorithms, default/zero seed and argument guards");
	return EXIT_SUCCESS;
}

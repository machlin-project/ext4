/* SPDX-License-Identifier: BSD-3-Clause */
/* Test-only oracle linked with e2fsprogs: prints ext4 utf8-12.1 casefold and
 * casefolded directory-hash vectors computed by libext2fs, which follows the
 * Linux utf8data semantics. The portable core never links this program. */
#include <sys/stat.h>
#include "ext2fs/ext2fsP.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ORACLE_BYTES 4096
#define SEQUENCE_COUNT 200000U
#define HASH_COUNT 20000U
#define SEQUENCE_LIMIT 12U
#define LONG_COUNT 4000U
#define LONG_BYTES 255U
#define UTF8_MAX_BYTES 4U
#define UNICODE_LIMIT 0x110000U

static const struct ext2fs_nls_table *table;
static unsigned long long state = 0x9e3779b97f4a7c15ULL;

static unsigned int
next(void)
{
	state ^= state << 13;
	state ^= state >> 7;
	state ^= state << 17;
	return (unsigned int)(state >> 11);
}

/* Encode without validation so surrogates and out-of-range forms reach libext2fs. */
static size_t
encode(unsigned int code, unsigned char *output)
{
	if (code < 0x80U) {
		output[0] = (unsigned char)code;
		return 1;
	}
	if (code < 0x800U) {
		output[0] = (unsigned char)(0xc0U | code >> 6);
		output[1] = (unsigned char)(0x80U | (code & 0x3fU));
		return 2;
	}
	if (code < 0x10000U) {
		output[0] = (unsigned char)(0xe0U | code >> 12);
		output[1] = (unsigned char)(0x80U | (code >> 6 & 0x3fU));
		output[2] = (unsigned char)(0x80U | (code & 0x3fU));
		return 3;
	}
	output[0] = (unsigned char)(0xf0U | code >> 18);
	output[1] = (unsigned char)(0x80U | (code >> 12 & 0x3fU));
	output[2] = (unsigned char)(0x80U | (code >> 6 & 0x3fU));
	output[3] = (unsigned char)(0x80U | (code & 0x3fU));
	return 4;
}

static void
hex(const unsigned char *bytes, size_t length)
{
	size_t index;

	for (index = 0; index < length; index++) {
		printf("%02x", bytes[index]);
	}
}

static void
fold(const unsigned char *input, size_t length)
{
	unsigned char output[ORACLE_BYTES];
	int result;

	result = table->ops->casefold(table, input, length, output, sizeof(output));
	printf("F ");
	hex(input, length);
	printf(" ");
	if (result < 0) {
		printf("-\n");
	} else {
		hex(output, (size_t)result);
		printf("\n");
	}
}

/* Mix bases, combining marks, casefolded letters, Hangul and ignorables. */
static unsigned int
sample(void)
{
	static const unsigned int interesting[] = { 0x41, 0x61, 0xc5, 0xdf, 0x130, 0x1c4, 0x300,
		0x301, 0x316, 0x323, 0x345, 0x3a3, 0x3c2, 0x1e9e, 0x1f80, 0x200c, 0x200d, 0xfeff,
		0xac00, 0xd7a3, 0xfb01, 0x10400, 0x1d15e, 0x0e33, 0x0f73, 0x1100, 0x1161 };
	unsigned int choice = next() % 4U;

	if (choice == 0) {
		return interesting[next() % (sizeof(interesting) / sizeof(interesting[0]))];
	}
	if (choice == 1) {
		return 0x300U + next() % 0x70U;
	}
	if (choice == 2) {
		return 0x20U + next() % 0x5fU;
	}
	return next() % 0x3000U;
}

/* Code points whose folds expand, so near-maximal names fold beyond 255 bytes. */
static unsigned int
expanding(void)
{
	static const unsigned int points[] = { 0xdf, 0x130, 0x149, 0x390, 0x3b0, 0x587, 0x1e9e,
		0x1f80, 0x1f82, 0x1fb7, 0x1ff7, 0xfb03, 0xfb17, 0xac01, 0xd7a3 };

	return next() % 4U == 0 ? sample() : points[next() % (sizeof(points) / sizeof(points[0]))];
}

static int
hash_vector(const unsigned char *input, size_t length)
{
	ext2_dirhash_t hash;
	ext2_dirhash_t minor;
	__u32 seed[4];
	unsigned int part;
	int version = (int)(next() % 6U);

	for (part = 0; part < 4; part++) {
		seed[part] = next();
	}
	if (next() % 3U == 0) {
		memset(seed, 0, sizeof(seed));
	}
	if (ext2fs_dirhash2(version, (const char *)input, (int)length, table, EXT4_CASEFOLD_FL,
		seed, &hash, &minor) != 0) {
		return 1;
	}
	printf("H %d %08x %08x %08x %08x ", version, seed[0], seed[1], seed[2], seed[3]);
	hex(input, length);
	printf(" %08x %08x\n", hash, minor);
	return 0;
}

int
main(void)
{
	static const unsigned char malformed[][4] = { { 0x80 }, { 0xc0, 0x80 }, { 0xc1, 0xbf },
		{ 0xe0, 0x80, 0x80 }, { 0xed, 0xa0, 0x80 }, { 0xf4, 0x90, 0x80, 0x80 },
		{ 0xf8, 0x88, 0x80, 0x80 }, { 0xc3 }, { 0xe2, 0x82 }, { 0xff } };
	static const size_t malformed_lengths[] = { 1, 2, 2, 3, 3, 4, 4, 1, 2, 1 };
	unsigned char input[LONG_BYTES];
	unsigned int code;
	unsigned int index;
	unsigned int count;
	unsigned int part;
	size_t length;

	table = ext2fs_load_nls_table(EXT4_ENC_UTF8_12_1);
	if (table == NULL) {
		return 1;
	}
	for (code = 0; code < UNICODE_LIMIT; code++) {
		length = encode(code, input);
		fold(input, length);
	}
	for (index = 0; index < sizeof(malformed_lengths) / sizeof(malformed_lengths[0]); index++) {
		fold(malformed[index], malformed_lengths[index]);
	}
	for (index = 0; index < SEQUENCE_COUNT; index++) {
		count = 1U + next() % SEQUENCE_LIMIT;
		length = 0;
		for (part = 0; part < count; part++) {
			length += encode(sample(), input + length);
		}
		fold(input, length);
	}
	for (index = 0; index < HASH_COUNT; index++) {
		count = 1U + next() % SEQUENCE_LIMIT;
		length = 0;
		for (part = 0; part < count; part++) {
			length += encode(sample(), input + length);
		}
		if (hash_vector(input, length) != 0) {
			return 1;
		}
	}
	for (index = 0; index < LONG_COUNT; index++) {
		length = 0;
		while (length + UTF8_MAX_BYTES <= LONG_BYTES) {
			length += encode(expanding(), input + length);
		}
		fold(input, length);
		if (hash_vector(input, length) != 0) {
			return 1;
		}
	}
	return 0;
}

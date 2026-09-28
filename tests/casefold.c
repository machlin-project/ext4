/* SPDX-License-Identifier: BSD-3-Clause */
#include "unicode.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Compare the core's utf8-12.1 casefold and casefolded directory hashes with
 * vectors produced by e2fsprogs for every code point, malformed sequences and
 * random combining sequences. */

#define LINE_BYTES 16384U
#define CHECK(expression)                                                                          \
	do {                                                                                       \
		if (!(expression)) {                                                               \
			fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #expression);           \
			exit(1);                                                                   \
		}                                                                                  \
	} while (0)

static size_t
decode_hex(const char *text, size_t length, uint8_t *output, size_t capacity)
{
	unsigned int value;
	size_t index;

	CHECK(length % 2U == 0 && length / 2U <= capacity);
	for (index = 0; index < length / 2U; index++) {
		CHECK(sscanf(text + index * 2U, "%2x", &value) == 1);
		output[index] = (uint8_t)value;
	}
	return length / 2U;
}

int
main(int argc, char **argv)
{
	static struct ext4_casefold fold;
	static char line[LINE_BYTES];
	static uint8_t input[EXT4_NAME_MAX + 1U];
	static uint8_t expected[EXT4_CASEFOLD_BYTES];
	struct ext4_name_hash hash;
	char *fields[10];
	FILE *vectors;
	uint32_t seed[4];
	uint32_t major;
	uint32_t minor;
	size_t input_length;
	size_t expected_length;
	uint64_t folds = 0;
	uint64_t opaque = 0;
	uint64_t hashes = 0;
	unsigned int version;
	unsigned int field;
	enum ext4_result error;

	if (argc != 2) {
		fprintf(stderr, "usage: %s VECTORS\n", argv[0]);
		return 2;
	}
	vectors = fopen(argv[1], "r");
	CHECK(vectors != NULL);
	while (fgets(line, sizeof(line), vectors) != NULL) {
		line[strcspn(line, "\n")] = 0;
		fields[0] = strtok(line, " ");
		for (field = 1; field < 10U; field++) {
			fields[field] = strtok(NULL, " ");
			if (fields[field] == NULL) {
				break;
			}
		}
		if (strcmp(fields[0], "F") == 0) {
			input_length =
			    decode_hex(fields[1], strlen(fields[1]), input, sizeof(input));
			/* libext2fs stops at NUL, which no ext4 name contains. */
			if (memchr(input, 0, input_length) != NULL) {
				continue;
			}
			if (fields[2] == NULL) {
				fields[2] = "";
			}
			error = ext4_casefold_name(&fold, input, input_length);
			if (strcmp(fields[2], "-") == 0) {
				CHECK(error == EXT4_INVALID_ARGUMENT);
				CHECK(!ext4_utf8_name_valid(input, input_length));
				opaque++;
			} else {
				expected_length = decode_hex(
				    fields[2], strlen(fields[2]), expected, sizeof(expected));
				if (error != EXT4_OK || fold.length != expected_length ||
				    memcmp(fold.bytes, expected, expected_length) != 0) {
					fprintf(stderr, "casefold mismatch for %s\n", fields[1]);
					return 1;
				}
				CHECK(ext4_utf8_name_valid(input, input_length));
			}
			folds++;
		} else {
			CHECK(strcmp(fields[0], "H") == 0 && field == 9U);
			version = (unsigned int)strtoul(fields[1], NULL, 10);
			for (field = 0; field < 4U; field++) {
				seed[field] = (uint32_t)strtoul(fields[2U + field], NULL, 16);
			}
			input_length =
			    decode_hex(fields[6], strlen(fields[6]), input, sizeof(input));
			if (memchr(input, 0, input_length) != NULL) {
				continue;
			}
			CHECK(sscanf(fields[7], "%" SCNx32, &major) == 1);
			CHECK(sscanf(fields[8], "%" SCNx32, &minor) == 1);
			if (ext4_casefold_name(&fold, input, input_length) == EXT4_OK) {
				CHECK(ext4_directory_hash((uint8_t)version, seed, fold.bytes,
					  fold.length, &hash) == EXT4_OK);
			} else {
				CHECK(ext4_directory_hash((uint8_t)version, seed, input,
					  input_length, &hash) == EXT4_OK);
			}
			if (hash.major != major || hash.minor != minor) {
				fprintf(stderr, "hash mismatch for %s version %u\n", fields[6],
				    version);
				return 1;
			}
			hashes++;
		}
	}
	CHECK(fclose(vectors) == 0 && folds > 0x110000U && hashes > 0);
	printf("PASS %" PRIu64 " casefold vectors (%" PRIu64 " opaque) and %" PRIu64
	       " casefolded hashes match e2fsprogs\n",
	    folds, opaque, hashes);
	return 0;
}

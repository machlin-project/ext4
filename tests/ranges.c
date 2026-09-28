/* SPDX-License-Identifier: BSD-3-Clause */
#include "internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define RANGE_COUNT 256U
#define RANGE_DOMAIN 4096U
#define QUERY_LENGTH 17U
#define CHECK(expression)                                                                          \
	do {                                                                                       \
		if (!(expression)) {                                                               \
			fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #expression);           \
			exit(1);                                                                   \
		}                                                                                  \
	} while (0)

int
main(void)
{
	const uint64_t origins[] = { 0, (uint64_t)1U << 32, UINT64_MAX - RANGE_DOMAIN };
	struct ext4_block_range *ranges;
	struct ext4_block_range duplicate[] = { { 7, 4 }, { 7, 4 } };
	bool *covered;
	bool expected;
	size_t count;
	size_t index;
	uint32_t pattern;
	uint32_t origin;
	uint32_t start;
	uint32_t length;
	uint32_t position;
	uint32_t seed;
	uint32_t queries = 0;

	ranges = calloc(RANGE_COUNT, sizeof(*ranges));
	covered = calloc(RANGE_DOMAIN, sizeof(*covered));
	CHECK(ranges != NULL && covered != NULL);
	count = 0;
	ext4_ranges_union(NULL, &count);
	CHECK(count == 0 && !ext4_ranges_overlap(NULL, 0, 0, 1));
	count = sizeof(duplicate) / sizeof(duplicate[0]);
	CHECK(ext4_ranges_sort(duplicate, &count) == EXT4_CORRUPT);
	count = sizeof(duplicate) / sizeof(duplicate[0]);
	ext4_ranges_union(duplicate, &count);
	CHECK(count == 1 && duplicate[0].first == 7 && duplicate[0].length == 4);
	for (origin = 0; origin < sizeof(origins) / sizeof(origins[0]); origin++) {
		for (pattern = 0; pattern < 4; pattern++) {
			memset(covered, 0, RANGE_DOMAIN * sizeof(*covered));
			seed = 0x6d2b79f5U;
			for (index = 0; index < RANGE_COUNT; index++) {
				seed = seed * 1664525U + 1013904223U;
				start = pattern == 0 ? (uint32_t)(RANGE_COUNT - index - 1U) * 8U
				    : pattern == 1   ? (uint32_t)index * 2U
				    : pattern == 2   ? (uint32_t)(index % 3U)
						     : seed % (RANGE_DOMAIN - QUERY_LENGTH);
				length = pattern == 0 ? 2U : 1U + seed % QUERY_LENGTH;
				ranges[index].first = origins[origin] + start;
				ranges[index].length = length;
				for (position = start; position < start + length; position++) {
					covered[position] = true;
				}
			}
			count = RANGE_COUNT;
			ext4_ranges_union(ranges, &count);
			CHECK(count > 0 && count <= RANGE_COUNT);
			for (index = 1; index < count; index++) {
				CHECK(ranges[index - 1U].first + ranges[index - 1U].length <
				    ranges[index].first);
			}
			for (start = 0; start <= RANGE_DOMAIN - QUERY_LENGTH; start++) {
				for (length = 1; length <= QUERY_LENGTH; length++) {
					expected = false;
					for (position = start; position < start + length;
					    position++) {
						expected |= covered[position];
					}
					CHECK(ext4_ranges_overlap(ranges, count,
						  origins[origin] + start, length) == expected);
					queries++;
				}
			}
		}
	}
	free(covered);
	free(ranges);
	printf("PASS %u range queries match independent block membership\n", queries);
	return 0;
}

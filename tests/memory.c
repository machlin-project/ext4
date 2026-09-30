/* SPDX-License-Identifier: BSD-3-Clause */
#define _GNU_SOURCE
#include "internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#define ALIGNMENT_CASES 256U
#define SMALL_BYTES 512U

#define CHECK(condition)                                                                           \
	do {                                                                                       \
		if (!(condition)) {                                                                \
			fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition);            \
			exit(1);                                                                   \
		}                                                                                  \
	} while (0)

static void
check_zero(uint8_t *buffer, size_t capacity, size_t offset, size_t length)
{
	size_t index;

	memset(buffer, 0xa5, capacity);
	ext4_zero(buffer + offset, length);
	for (index = 0; index < capacity; index++) {
		CHECK(buffer[index] == (index >= offset && index - offset < length ? 0 : 0xa5));
	}
}

int
main(void)
{
	static const size_t lengths[] = { 1023, 1024, 1025, 4095, 4096, 4097, 16383, 16384, 16385,
		65535, 65536 };
	uint8_t small[SMALL_BYTES + 2U * ALIGNMENT_CASES];
	uint8_t *guarded;
	uint8_t *buffer;
	long page;
	size_t capacity;
	size_t mapping_size;
	size_t offset;
	size_t length;
	size_t index;

	ext4_zero(NULL, 0);
	for (offset = 0; offset < ALIGNMENT_CASES; offset++) {
		for (length = 0; length <= SMALL_BYTES; length++) {
			check_zero(small, sizeof(small), offset, length);
		}
	}
	page = sysconf(_SC_PAGESIZE);
	CHECK(page > 0);
	capacity = 65536U + (size_t)page;
	capacity = (capacity + (size_t)page - 1U) / (size_t)page * (size_t)page;
	mapping_size = capacity + 2U * (size_t)page;
	guarded = mmap(NULL, mapping_size, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	CHECK(guarded != MAP_FAILED);
	buffer = guarded + page;
	CHECK(mprotect(buffer, capacity, PROT_READ | PROT_WRITE) == 0);
	for (index = 0; index < sizeof(lengths) / sizeof(lengths[0]); index++) {
		length = lengths[index];
		for (offset = 0; offset < ALIGNMENT_CASES; offset++) {
			check_zero(buffer, capacity, offset, length);
			check_zero(buffer, capacity, capacity - length - offset, length);
		}
	}
	CHECK(munmap(guarded, mapping_size) == 0);
	puts("PASS zero bounds, all small lengths/alignments, block edges and guarded pages");
	return 0;
}

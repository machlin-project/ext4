/* SPDX-License-Identifier: BSD-3-Clause */
#include "internal.h"

#if defined(__aarch64__) && !defined(EXT4_MEMORY_PORTABLE)
#define EXT4_ZERO_INSTRUCTION_MIN_BYTES 256U
#define EXT4_DCZID_PROHIBIT (1U << 4)
#define EXT4_DCZID_WORD_SHIFT_MASK 0x0fU
#define EXT4_DCZID_WORD_BYTES 4U

/* DC ZVA clears a naturally aligned block of normal memory. Query both its
 * availability and size, then admit only whole blocks inside the caller's range.
 * Only general registers are used, including in kernel builds. Core buffers are
 * normal memory, never device-register mappings. */
static size_t
ext4_zero_blocks(uint8_t *output, size_t length)
{
	uint64_t features;
	size_t block;
	size_t prefix;
	size_t position;

	if (length < EXT4_ZERO_INSTRUCTION_MIN_BYTES) {
		return 0;
	}
	__asm__ volatile("mrs %0, dczid_el0" : "=r"(features));
	if (features & EXT4_DCZID_PROHIBIT) {
		return 0;
	}
	block = (size_t)EXT4_DCZID_WORD_BYTES << (features & EXT4_DCZID_WORD_SHIFT_MASK);
	prefix = (block - (uintptr_t)output % block) % block;
	if (prefix > length || block > length - prefix) {
		return 0;
	}
	for (position = 0; position < prefix; position++) {
		output[position] = 0;
	}
	while (block <= length - position) {
		__asm__ volatile("dc zva, %0" : : "r"(output + position) : "memory");
		position += block;
	}
	return position;
}
#endif

void
ext4_copy(void *destination, const void *source, size_t length)
{
	uint8_t *output = destination;
	const uint8_t *input = source;
	size_t index;

	for (index = 0; index < length; index++) {
		output[index] = input[index];
	}
}

void
ext4_zero(void *destination, size_t length)
{
	uint8_t *output = destination;
	size_t index = 0;

#if defined(__aarch64__) && !defined(EXT4_MEMORY_PORTABLE)
	index = ext4_zero_blocks(output, length);
#endif
	for (; index < length; index++) {
		output[index] = 0;
	}
}

bool
ext4_equal(const void *left, const void *right, size_t length)
{
	const uint8_t *a = left;
	const uint8_t *b = right;
	size_t index;

	for (index = 0; index < length; index++) {
		if (a[index] != b[index]) {
			return false;
		}
	}
	return true;
}

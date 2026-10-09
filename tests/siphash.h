/* SPDX-License-Identifier: BSD-3-Clause */
#ifndef MACHLIN_EXT4_TEST_SIPHASH_H
#define MACHLIN_EXT4_TEST_SIPHASH_H

#include <stddef.h>
#include <stdint.h>

/* Test-adapter SipHash-2-4. Independent EVP fixtures and published vectors
 * check this readable reference; it is not linked into the filesystem core. */
static uint64_t
test_siphash_rotate(uint64_t value, unsigned int bits)
{
	return (value << bits) | (value >> (64U - bits));
}

static uint64_t
test_siphash_word(const uint8_t *bytes)
{
	uint64_t value = 0;
	unsigned int index;

	for (index = 0; index < 8; index++) {
		value |= (uint64_t)bytes[index] << (index * 8U);
	}
	return value;
}

static void
test_siphash_round(uint64_t v[4])
{
	v[0] += v[1];
	v[1] = test_siphash_rotate(v[1], 13) ^ v[0];
	v[0] = test_siphash_rotate(v[0], 32);
	v[2] += v[3];
	v[3] = test_siphash_rotate(v[3], 16) ^ v[2];
	v[0] += v[3];
	v[3] = test_siphash_rotate(v[3], 21) ^ v[0];
	v[2] += v[1];
	v[1] = test_siphash_rotate(v[1], 17) ^ v[2];
	v[2] = test_siphash_rotate(v[2], 32);
}

static uint64_t
test_siphash24(const uint8_t key[16], const uint8_t *name, size_t length)
{
	uint64_t first = test_siphash_word(key);
	uint64_t second = test_siphash_word(key + 8);
	uint64_t v[4] = { first ^ UINT64_C(0x736f6d6570736575),
		second ^ UINT64_C(0x646f72616e646f6d), first ^ UINT64_C(0x6c7967656e657261),
		second ^ UINT64_C(0x7465646279746573) };
	uint64_t word;
	uint64_t tail = (uint64_t)length << 56;
	size_t offset = 0;
	unsigned int index;

	while (length - offset >= 8) {
		word = test_siphash_word(name + offset);
		v[3] ^= word;
		test_siphash_round(v);
		test_siphash_round(v);
		v[0] ^= word;
		offset += 8;
	}
	for (index = 0; offset + index < length; index++) {
		tail |= (uint64_t)name[offset + index] << (index * 8U);
	}
	v[3] ^= tail;
	test_siphash_round(v);
	test_siphash_round(v);
	v[0] ^= tail;
	v[2] ^= UINT64_C(0xff);
	for (index = 0; index < 4; index++) {
		test_siphash_round(v);
	}
	return v[0] ^ v[1] ^ v[2] ^ v[3];
}

#endif

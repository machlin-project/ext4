/* SPDX-License-Identifier: BSD-3-Clause */
#include "internal.h"

/* These are the on-disk filename hash algorithms, not cryptographic APIs. */
static uint32_t
ext4_hash_byte(uint8_t byte, bool unsigned_bytes)
{
	return (uint32_t)byte - (!unsigned_bytes && byte >= 128U ? 256U : 0U);
}

static void
ext4_hash_words(
    const uint8_t *name, size_t remaining, bool unsigned_bytes, uint32_t *words, unsigned int count)
{
	uint32_t padding = (uint32_t)remaining * 0x01010101U;
	uint32_t value;
	size_t offset = 0;
	unsigned int word;
	unsigned int byte;

	for (word = 0; word < count; word++) {
		value = padding;
		for (byte = 0; byte < sizeof(value) && offset < remaining; byte++) {
			value = (value << 8) + ext4_hash_byte(name[offset], unsigned_bytes);
			offset++;
		}
		words[word] = value;
	}
}

static uint32_t
ext4_hash_rotate(uint32_t value, unsigned int bits)
{
	return (value << bits) | (value >> (32U - bits));
}

static void
ext4_hash_half_md4(uint32_t state[4], const uint32_t words[8])
{
	static const uint8_t order[3][8] = {
		{ 0, 1, 2, 3, 4, 5, 6, 7 },
		{ 1, 3, 5, 7, 0, 2, 4, 6 },
		{ 3, 7, 2, 6, 1, 5, 0, 4 },
	};
	static const uint8_t rotations[3][4] = {
		{ 3, 7, 11, 19 },
		{ 3, 5, 9, 13 },
		{ 3, 9, 11, 15 },
	};
	static const uint32_t constants[3] = { 0, 0x5a827999U, 0x6ed9eba1U };
	uint32_t working[4];
	uint32_t b;
	uint32_t c;
	uint32_t d;
	uint32_t mixed;
	unsigned int round;
	unsigned int step;
	unsigned int target;

	for (target = 0; target < 4; target++) {
		working[target] = state[target];
	}
	for (round = 0; round < 3; round++) {
		for (step = 0; step < 8; step++) {
			target = (4U - (step & 3U)) & 3U;
			b = working[(target + 1U) & 3U];
			c = working[(target + 2U) & 3U];
			d = working[(target + 3U) & 3U];
			if (round == 0) {
				mixed = (b & c) | (~b & d);
			} else if (round == 1) {
				mixed = (b & c) | (b & d) | (c & d);
			} else {
				mixed = b ^ c ^ d;
			}
			working[target] = ext4_hash_rotate(
			    working[target] + mixed + words[order[round][step]] + constants[round],
			    rotations[round][step & 3U]);
		}
	}
	for (target = 0; target < 4; target++) {
		state[target] += working[target];
	}
}

static void
ext4_hash_tea(uint32_t state[4], const uint32_t words[4])
{
	const uint32_t delta = 0x9e3779b9U;
	uint32_t left = state[0];
	uint32_t right = state[1];
	uint32_t sum = 0;
	unsigned int round;

	for (round = 0; round < 16; round++) {
		sum += delta;
		left += ((right << 4) + words[0]) ^ (right + sum) ^ ((right >> 5) + words[1]);
		right += ((left << 4) + words[2]) ^ (left + sum) ^ ((left >> 5) + words[3]);
	}
	state[0] += left;
	state[1] += right;
}

enum ext4_result
ext4_directory_hash(uint8_t version, const uint32_t seed[4], const uint8_t *name, size_t length,
    struct ext4_name_hash *result)
{
	uint32_t state[4] = { 0x67452301U, 0xefcdab89U, 0x98badcfeU, 0x10325476U };
	struct ext4_name_hash hash = { 0, 0 };
	uint32_t words[8];
	uint32_t current = 0x12a3fe2dU;
	uint32_t previous = 0x37abe8f9U;
	uint32_t next;
	uint32_t nonzero = 0;
	size_t offset;
	size_t chunk;
	unsigned int index;
	unsigned int count;
	bool unsigned_bytes = version >= EXT4_HASH_LEGACY_UNSIGNED;

	if (name == NULL || length == 0 || result == NULL) {
		return EXT4_INVALID_ARGUMENT;
	}
	if (length > EXT4_NAME_MAX) {
		return EXT4_NAME_TOO_LONG;
	}
	if (version > EXT4_HASH_TEA_UNSIGNED) {
		return EXT4_UNSUPPORTED;
	}
	if (unsigned_bytes) {
		version -= EXT4_HASH_LEGACY_UNSIGNED;
	}
	if (seed != NULL) {
		for (index = 0; index < 4; index++) {
			nonzero |= seed[index];
		}
		if (nonzero != 0) {
			for (index = 0; index < 4; index++) {
				state[index] = seed[index];
			}
		}
	}
	if (version == EXT4_HASH_LEGACY) {
		for (offset = 0; offset < length; offset++) {
			next = previous +
			    (current ^ (ext4_hash_byte(name[offset], unsigned_bytes) * 0x6d22f5U));
			if (next & 0x80000000U) {
				next -= 0x7fffffffU;
			}
			previous = current;
			current = next;
		}
		hash.major = current << 1;
	} else {
		count = version == EXT4_HASH_HALF_MD4 ? 8U : 4U;
		chunk = count * sizeof(*words);
		for (offset = 0; offset < length; offset += chunk) {
			ext4_hash_words(
			    name + offset, length - offset, unsigned_bytes, words, count);
			if (version == EXT4_HASH_HALF_MD4) {
				ext4_hash_half_md4(state, words);
			} else {
				ext4_hash_tea(state, words);
			}
		}
		index = version == EXT4_HASH_HALF_MD4 ? 1U : 0U;
		hash.major = state[index];
		hash.minor = state[index + 1U];
	}
	hash.major &= ~1U;
	if (hash.major == EXT4_HASH_EOF) {
		hash.major -= 2U;
	}
	*result = hash;
	return EXT4_OK;
}

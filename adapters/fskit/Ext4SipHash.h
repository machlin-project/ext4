/* SPDX-License-Identifier: BSD-3-Clause */
#ifndef MACHLIN_EXT4_NATIVE_SIPHASH_H
#define MACHLIN_EXT4_NATIVE_SIPHASH_H

#include <stddef.h>
#include <stdint.h>

#define EXT4_NATIVE_SIPHASH_KEY_BYTES 16U

/* Private, Foundation-free SipHash-2-4 primitive. The provider validates pointer
 * arguments and keeps the key in its locked allocation. All words are explicit
 * little-endian; no alignment, host byte order or maximum filename assumption. */
static inline uint64_t
ext4_native_siphash_word(const uint8_t *bytes)
{
	uint64_t value = 0;
	unsigned int index;

	for (index = 0; index < 8; index++) {
		value |= (uint64_t)bytes[index] << (index * 8U);
	}
	return value;
}

static inline uint64_t
ext4_native_siphash_rotate(uint64_t value, unsigned int shift)
{
	return (value << shift) | (value >> (64U - shift));
}

static inline void
ext4_native_siphash_round(uint64_t state[4])
{
	state[0] += state[1];
	state[1] = ext4_native_siphash_rotate(state[1], 13) ^ state[0];
	state[0] = ext4_native_siphash_rotate(state[0], 32);
	state[2] += state[3];
	state[3] = ext4_native_siphash_rotate(state[3], 16) ^ state[2];
	state[0] += state[3];
	state[3] = ext4_native_siphash_rotate(state[3], 21) ^ state[0];
	state[2] += state[1];
	state[1] = ext4_native_siphash_rotate(state[1], 17) ^ state[2];
	state[2] = ext4_native_siphash_rotate(state[2], 32);
}

static inline uint64_t
ext4_native_siphash_bytes(const uint8_t key[EXT4_NATIVE_SIPHASH_KEY_BYTES],
    const uint8_t *bytes, size_t length)
{
	uint64_t state[4] = {
		UINT64_C(0x736f6d6570736575) ^ ext4_native_siphash_word(key),
		UINT64_C(0x646f72616e646f6d) ^ ext4_native_siphash_word(key + 8),
		UINT64_C(0x6c7967656e657261) ^ ext4_native_siphash_word(key),
		UINT64_C(0x7465646279746573) ^ ext4_native_siphash_word(key + 8)
	};
	volatile uint64_t *wipe = state;
	uint64_t tail = (uint64_t)length << 56;
	uint64_t word;
	uint64_t result;
	unsigned int index;

	while (length >= 8) {
		word = ext4_native_siphash_word(bytes);
		state[3] ^= word;
		ext4_native_siphash_round(state);
		ext4_native_siphash_round(state);
		state[0] ^= word;
		bytes += 8;
		length -= 8;
	}
	for (index = 0; index < length; index++) {
		tail |= (uint64_t)bytes[index] << (index * 8U);
	}
	state[3] ^= tail;
	ext4_native_siphash_round(state);
	ext4_native_siphash_round(state);
	state[0] ^= tail;
	state[2] ^= UINT64_C(0xff);
	for (index = 0; index < 4; index++) {
		ext4_native_siphash_round(state);
	}
	result = state[0] ^ state[1] ^ state[2] ^ state[3];
	/* Match the native provider's volatile wipe contract for key-derived state. */
	for (index = 0; index < 4; index++) {
		wipe[index] = 0;
	}
	return result;
}

#endif

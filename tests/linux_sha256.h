/* SPDX-License-Identifier: BSD-3-Clause */
#ifndef MACHLIN_EXT4_LINUX_SHA256_H
#define MACHLIN_EXT4_LINUX_SHA256_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define SHA256_BYTES 32U
#define SHA256_BLOCK 64U

/* FIPS 180-4 SHA-256; the static guest has no library for it. */
struct sha256 {
	uint32_t state[8];
	uint8_t block[SHA256_BLOCK];
	uint64_t length;
	size_t used;
};

static const uint32_t sha256_rounds[64] = { 0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5,
	0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be,
	0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786,
	0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152,
	0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
	0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e,
	0x92722c85, 0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624,
	0xf40e3585, 0x106aa070, 0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3,
	0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
	0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2 };

static uint32_t
rotate(uint32_t value, unsigned int count)
{
	return (value >> count) | (value << (32U - count));
}

static void
sha256_block(struct sha256 *context)
{
	uint32_t words[64];
	uint32_t state[8];
	uint32_t first;
	uint32_t second;
	unsigned int index;

	for (index = 0; index < 16U; index++) {
		words[index] = (uint32_t)context->block[4U * index] << 24 |
		    (uint32_t)context->block[4U * index + 1U] << 16 |
		    (uint32_t)context->block[4U * index + 2U] << 8 |
		    context->block[4U * index + 3U];
	}
	for (index = 16; index < 64U; index++) {
		words[index] = words[index - 16U] +
		    (rotate(words[index - 15U], 7) ^ rotate(words[index - 15U], 18) ^
			(words[index - 15U] >> 3)) +
		    words[index - 7U] +
		    (rotate(words[index - 2U], 17) ^ rotate(words[index - 2U], 19) ^
			(words[index - 2U] >> 10));
	}
	memcpy(state, context->state, sizeof(state));
	for (index = 0; index < 64U; index++) {
		first = state[7] +
		    (rotate(state[4], 6) ^ rotate(state[4], 11) ^ rotate(state[4], 25)) +
		    ((state[4] & state[5]) ^ (~state[4] & state[6])) + sha256_rounds[index] +
		    words[index];
		second = (rotate(state[0], 2) ^ rotate(state[0], 13) ^ rotate(state[0], 22)) +
		    ((state[0] & state[1]) ^ (state[0] & state[2]) ^ (state[1] & state[2]));
		memmove(state + 1, state, 7U * sizeof(state[0]));
		state[4] += first;
		state[0] = first + second;
	}
	for (index = 0; index < 8U; index++) {
		context->state[index] += state[index];
	}
}

static void
sha256_init(struct sha256 *context)
{
	static const uint32_t initial[8] = { 0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
		0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19 };

	memset(context, 0, sizeof(*context));
	memcpy(context->state, initial, sizeof(initial));
}

static void
sha256_update(struct sha256 *context, const uint8_t *bytes, size_t length)
{
	size_t index;

	for (index = 0; index < length; index++) {
		context->block[context->used++] = bytes[index];
		if (context->used == SHA256_BLOCK) {
			sha256_block(context);
			context->used = 0;
		}
	}
	context->length += length;
}

static void
sha256_final(struct sha256 *context, char *text)
{
	uint64_t bits = context->length * 8U;
	uint8_t padding = 0x80;
	uint8_t zero = 0;
	uint8_t length[8];
	unsigned int index;

	sha256_update(context, &padding, 1);
	while (context->used != SHA256_BLOCK - sizeof(length)) {
		sha256_update(context, &zero, 1);
	}
	for (index = 0; index < sizeof(length); index++) {
		length[index] = (uint8_t)(bits >> (56U - 8U * index));
	}
	sha256_update(context, length, sizeof(length));
	for (index = 0; index < 8U; index++) {
		snprintf(text + 8U * index, 9, "%08x", context->state[index]);
	}
}

#endif

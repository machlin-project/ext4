/* SPDX-License-Identifier: BSD-3-Clause */
#include "sha.h"

/* The target must guarantee SHA-256 instructions and permit SIMD use. Kernel
 * builds retain the portable transform until native SIMD ownership is accepted. */
#if defined(__aarch64__) && defined(__ARM_FEATURE_SHA2) && defined(__ARM_NEON) &&                  \
    !defined(KERNEL) && !defined(EXT4_SHA_PORTABLE) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
#include <arm_neon.h>
#define EXT4_SHA256_ARM64 1
#endif

/* FIPS 180-4 SHA-256 and SHA-512 with immutable round constants,
 * big-endian message decoding and no allocation or mutable global state.
 * Full input blocks are consumed directly, including unaligned input; only a
 * partial block is copied into the context's streaming buffer. */

static const uint32_t ext4_sha256_rounds[64] = { 0x428a2f98U, 0x71374491U, 0xb5c0fbcfU, 0xe9b5dba5U,
	0x3956c25bU, 0x59f111f1U, 0x923f82a4U, 0xab1c5ed5U, 0xd807aa98U, 0x12835b01U, 0x243185beU,
	0x550c7dc3U, 0x72be5d74U, 0x80deb1feU, 0x9bdc06a7U, 0xc19bf174U, 0xe49b69c1U, 0xefbe4786U,
	0x0fc19dc6U, 0x240ca1ccU, 0x2de92c6fU, 0x4a7484aaU, 0x5cb0a9dcU, 0x76f988daU, 0x983e5152U,
	0xa831c66dU, 0xb00327c8U, 0xbf597fc7U, 0xc6e00bf3U, 0xd5a79147U, 0x06ca6351U, 0x14292967U,
	0x27b70a85U, 0x2e1b2138U, 0x4d2c6dfcU, 0x53380d13U, 0x650a7354U, 0x766a0abbU, 0x81c2c92eU,
	0x92722c85U, 0xa2bfe8a1U, 0xa81a664bU, 0xc24b8b70U, 0xc76c51a3U, 0xd192e819U, 0xd6990624U,
	0xf40e3585U, 0x106aa070U, 0x19a4c116U, 0x1e376c08U, 0x2748774cU, 0x34b0bcb5U, 0x391c0cb3U,
	0x4ed8aa4aU, 0x5b9cca4fU, 0x682e6ff3U, 0x748f82eeU, 0x78a5636fU, 0x84c87814U, 0x8cc70208U,
	0x90befffaU, 0xa4506cebU, 0xbef9a3f7U, 0xc67178f2U };

static const uint64_t ext4_sha512_rounds[80] = { UINT64_C(0x428a2f98d728ae22),
	UINT64_C(0x7137449123ef65cd), UINT64_C(0xb5c0fbcfec4d3b2f), UINT64_C(0xe9b5dba58189dbbc),
	UINT64_C(0x3956c25bf348b538), UINT64_C(0x59f111f1b605d019), UINT64_C(0x923f82a4af194f9b),
	UINT64_C(0xab1c5ed5da6d8118), UINT64_C(0xd807aa98a3030242), UINT64_C(0x12835b0145706fbe),
	UINT64_C(0x243185be4ee4b28c), UINT64_C(0x550c7dc3d5ffb4e2), UINT64_C(0x72be5d74f27b896f),
	UINT64_C(0x80deb1fe3b1696b1), UINT64_C(0x9bdc06a725c71235), UINT64_C(0xc19bf174cf692694),
	UINT64_C(0xe49b69c19ef14ad2), UINT64_C(0xefbe4786384f25e3), UINT64_C(0x0fc19dc68b8cd5b5),
	UINT64_C(0x240ca1cc77ac9c65), UINT64_C(0x2de92c6f592b0275), UINT64_C(0x4a7484aa6ea6e483),
	UINT64_C(0x5cb0a9dcbd41fbd4), UINT64_C(0x76f988da831153b5), UINT64_C(0x983e5152ee66dfab),
	UINT64_C(0xa831c66d2db43210), UINT64_C(0xb00327c898fb213f), UINT64_C(0xbf597fc7beef0ee4),
	UINT64_C(0xc6e00bf33da88fc2), UINT64_C(0xd5a79147930aa725), UINT64_C(0x06ca6351e003826f),
	UINT64_C(0x142929670a0e6e70), UINT64_C(0x27b70a8546d22ffc), UINT64_C(0x2e1b21385c26c926),
	UINT64_C(0x4d2c6dfc5ac42aed), UINT64_C(0x53380d139d95b3df), UINT64_C(0x650a73548baf63de),
	UINT64_C(0x766a0abb3c77b2a8), UINT64_C(0x81c2c92e47edaee6), UINT64_C(0x92722c851482353b),
	UINT64_C(0xa2bfe8a14cf10364), UINT64_C(0xa81a664bbc423001), UINT64_C(0xc24b8b70d0f89791),
	UINT64_C(0xc76c51a30654be30), UINT64_C(0xd192e819d6ef5218), UINT64_C(0xd69906245565a910),
	UINT64_C(0xf40e35855771202a), UINT64_C(0x106aa07032bbd1b8), UINT64_C(0x19a4c116b8d2d0c8),
	UINT64_C(0x1e376c085141ab53), UINT64_C(0x2748774cdf8eeb99), UINT64_C(0x34b0bcb5e19b48a8),
	UINT64_C(0x391c0cb3c5c95a63), UINT64_C(0x4ed8aa4ae3418acb), UINT64_C(0x5b9cca4f7763e373),
	UINT64_C(0x682e6ff3d6b2b8a3), UINT64_C(0x748f82ee5defb2fc), UINT64_C(0x78a5636f43172f60),
	UINT64_C(0x84c87814a1f0ab72), UINT64_C(0x8cc702081a6439ec), UINT64_C(0x90befffa23631e28),
	UINT64_C(0xa4506cebde82bde9), UINT64_C(0xbef9a3f7b2c67915), UINT64_C(0xc67178f2e372532b),
	UINT64_C(0xca273eceea26619c), UINT64_C(0xd186b8c721c0c207), UINT64_C(0xeada7dd6cde0eb1e),
	UINT64_C(0xf57d4f7fee6ed178), UINT64_C(0x06f067aa72176fba), UINT64_C(0x0a637dc5a2c898a6),
	UINT64_C(0x113f9804bef90dae), UINT64_C(0x1b710b35131c471b), UINT64_C(0x28db77f523047d84),
	UINT64_C(0x32caab7b40c72493), UINT64_C(0x3c9ebe0a15c9bebc), UINT64_C(0x431d67c49c100d4c),
	UINT64_C(0x4cc5d4becb3e42b6), UINT64_C(0x597f299cfc657e2a), UINT64_C(0x5fcb6fab3ad6faec),
	UINT64_C(0x6c44198c4a475817) };

#ifndef EXT4_SHA256_ARM64
static uint32_t
ext4_rotate32(uint32_t value, unsigned int count)
{
	return (value >> count) | (value << (32U - count));
}

#endif

static uint64_t
ext4_rotate64(uint64_t value, unsigned int count)
{
	return (value >> count) | (value << (64U - count));
}

#ifdef EXT4_SHA256_ARM64
/* Four rounds keep the two halves' original state paired. The schedule ring
 * consumes four words and replaces only that slot with the words 16 rounds on. */
static void
ext4_sha256_block(struct ext4_sha256 *context, const uint8_t *block)
{
	uint32x4_t initial_abcd = vld1q_u32(context->state);
	uint32x4_t initial_efgh = vld1q_u32(context->state + 4U);
	uint32x4_t abcd = initial_abcd;
	uint32x4_t efgh = initial_efgh;
	uint32x4_t messages[4];
	uint32x4_t words;
	uint32x4_t previous;
	unsigned int round;
	unsigned int slot;

	for (slot = 0; slot < 4U; slot++) {
		messages[slot] = vreinterpretq_u32_u8(vrev32q_u8(vld1q_u8(block + slot * 16U)));
	}
	for (round = 0; round < 16U; round++) {
		slot = round % 4U;
		words = vaddq_u32(messages[slot], vld1q_u32(ext4_sha256_rounds + round * 4U));
		previous = abcd;
		abcd = vsha256hq_u32(abcd, efgh, words);
		efgh = vsha256h2q_u32(efgh, previous, words);
		if (round < 12U) {
			messages[slot] = vsha256su1q_u32(
			    vsha256su0q_u32(messages[slot], messages[(slot + 1U) % 4U]),
			    messages[(slot + 2U) % 4U], messages[(slot + 3U) % 4U]);
		}
	}
	vst1q_u32(context->state, vaddq_u32(abcd, initial_abcd));
	vst1q_u32(context->state + 4U, vaddq_u32(efgh, initial_efgh));
}

#else
/* One FIPS 180-4 round. Eight calls rotate the roles of the working words,
 * removing the seven explicit word moves from each loop iteration. */
static inline void
ext4_sha256_round(uint32_t a, uint32_t b, uint32_t c, uint32_t *d, uint32_t e, uint32_t f,
    uint32_t g, uint32_t *h, uint32_t word)
{
	uint32_t first;
	uint32_t second;

	first = *h + (ext4_rotate32(e, 6) ^ ext4_rotate32(e, 11) ^ ext4_rotate32(e, 25)) +
	    ((e & f) ^ (~e & g)) + word;
	second = (ext4_rotate32(a, 2) ^ ext4_rotate32(a, 13) ^ ext4_rotate32(a, 22)) +
	    ((a & b) ^ (a & c) ^ (b & c));
	*d += first;
	*h = first + second;
}

static void
ext4_sha256_block(struct ext4_sha256 *context, const uint8_t *block)
{
	uint32_t schedule[64];
	uint32_t work[8];
	uint32_t first;
	uint32_t second;
	unsigned int index;

	for (index = 0; index < 16U; index++) {
		schedule[index] = (uint32_t)block[index * 4U] << 24 |
		    (uint32_t)block[index * 4U + 1U] << 16 | (uint32_t)block[index * 4U + 2U] << 8 |
		    block[index * 4U + 3U];
	}
	for (; index < 64U; index++) {
		first = ext4_rotate32(schedule[index - 15U], 7) ^
		    ext4_rotate32(schedule[index - 15U], 18) ^ (schedule[index - 15U] >> 3);
		second = ext4_rotate32(schedule[index - 2U], 17) ^
		    ext4_rotate32(schedule[index - 2U], 19) ^ (schedule[index - 2U] >> 10);
		schedule[index] = schedule[index - 16U] + first + schedule[index - 7U] + second;
	}
	for (index = 0; index < 8U; index++) {
		work[index] = context->state[index];
	}
	for (index = 0; index < 64U; index += 8U) {
		ext4_sha256_round(work[0], work[1], work[2], &work[3], work[4], work[5], work[6],
		    &work[7], schedule[index] + ext4_sha256_rounds[index]);
		ext4_sha256_round(work[7], work[0], work[1], &work[2], work[3], work[4], work[5],
		    &work[6], schedule[index + 1U] + ext4_sha256_rounds[index + 1U]);
		ext4_sha256_round(work[6], work[7], work[0], &work[1], work[2], work[3], work[4],
		    &work[5], schedule[index + 2U] + ext4_sha256_rounds[index + 2U]);
		ext4_sha256_round(work[5], work[6], work[7], &work[0], work[1], work[2], work[3],
		    &work[4], schedule[index + 3U] + ext4_sha256_rounds[index + 3U]);
		ext4_sha256_round(work[4], work[5], work[6], &work[7], work[0], work[1], work[2],
		    &work[3], schedule[index + 4U] + ext4_sha256_rounds[index + 4U]);
		ext4_sha256_round(work[3], work[4], work[5], &work[6], work[7], work[0], work[1],
		    &work[2], schedule[index + 5U] + ext4_sha256_rounds[index + 5U]);
		ext4_sha256_round(work[2], work[3], work[4], &work[5], work[6], work[7], work[0],
		    &work[1], schedule[index + 6U] + ext4_sha256_rounds[index + 6U]);
		ext4_sha256_round(work[1], work[2], work[3], &work[4], work[5], work[6], work[7],
		    &work[0], schedule[index + 7U] + ext4_sha256_rounds[index + 7U]);
	}
	for (index = 0; index < 8U; index++) {
		context->state[index] += work[index];
	}
}

#endif

/* One FIPS 180-4 round. Eight calls rotate the roles of the working words,
 * removing the seven explicit word moves from each loop iteration. */
static inline void
ext4_sha512_round(uint64_t a, uint64_t b, uint64_t c, uint64_t *d, uint64_t e, uint64_t f,
    uint64_t g, uint64_t *h, uint64_t word)
{
	uint64_t first;
	uint64_t second;

	first = *h + (ext4_rotate64(e, 14) ^ ext4_rotate64(e, 18) ^ ext4_rotate64(e, 41)) +
	    ((e & f) ^ (~e & g)) + word;
	second = (ext4_rotate64(a, 28) ^ ext4_rotate64(a, 34) ^ ext4_rotate64(a, 39)) +
	    ((a & b) ^ (a & c) ^ (b & c));
	*d += first;
	*h = first + second;
}

static void
ext4_sha512_block(struct ext4_sha512 *context, const uint8_t *block)
{
	uint64_t schedule[80];
	uint64_t work[8];
	uint64_t first;
	uint64_t second;
	unsigned int index;
	unsigned int byte;

	for (index = 0; index < 16U; index++) {
		schedule[index] = 0;
		for (byte = 0; byte < 8U; byte++) {
			schedule[index] = schedule[index] << 8 | block[index * 8U + byte];
		}
	}
	for (; index < 80U; index++) {
		first = ext4_rotate64(schedule[index - 15U], 1) ^
		    ext4_rotate64(schedule[index - 15U], 8) ^ (schedule[index - 15U] >> 7);
		second = ext4_rotate64(schedule[index - 2U], 19) ^
		    ext4_rotate64(schedule[index - 2U], 61) ^ (schedule[index - 2U] >> 6);
		schedule[index] = schedule[index - 16U] + first + schedule[index - 7U] + second;
	}
	for (index = 0; index < 8U; index++) {
		work[index] = context->state[index];
	}
	for (index = 0; index < 80U; index += 8U) {
		ext4_sha512_round(work[0], work[1], work[2], &work[3], work[4], work[5], work[6],
		    &work[7], schedule[index] + ext4_sha512_rounds[index]);
		ext4_sha512_round(work[7], work[0], work[1], &work[2], work[3], work[4], work[5],
		    &work[6], schedule[index + 1U] + ext4_sha512_rounds[index + 1U]);
		ext4_sha512_round(work[6], work[7], work[0], &work[1], work[2], work[3], work[4],
		    &work[5], schedule[index + 2U] + ext4_sha512_rounds[index + 2U]);
		ext4_sha512_round(work[5], work[6], work[7], &work[0], work[1], work[2], work[3],
		    &work[4], schedule[index + 3U] + ext4_sha512_rounds[index + 3U]);
		ext4_sha512_round(work[4], work[5], work[6], &work[7], work[0], work[1], work[2],
		    &work[3], schedule[index + 4U] + ext4_sha512_rounds[index + 4U]);
		ext4_sha512_round(work[3], work[4], work[5], &work[6], work[7], work[0], work[1],
		    &work[2], schedule[index + 5U] + ext4_sha512_rounds[index + 5U]);
		ext4_sha512_round(work[2], work[3], work[4], &work[5], work[6], work[7], work[0],
		    &work[1], schedule[index + 6U] + ext4_sha512_rounds[index + 6U]);
		ext4_sha512_round(work[1], work[2], work[3], &work[4], work[5], work[6], work[7],
		    &work[0], schedule[index + 7U] + ext4_sha512_rounds[index + 7U]);
	}
	for (index = 0; index < 8U; index++) {
		context->state[index] += work[index];
	}
}

void
ext4_sha256_init(struct ext4_sha256 *context)
{
	static const uint32_t initial[8] = { 0x6a09e667U, 0xbb67ae85U, 0x3c6ef372U, 0xa54ff53aU,
		0x510e527fU, 0x9b05688cU, 0x1f83d9abU, 0x5be0cd19U };

	ext4_copy(context->state, initial, sizeof(initial));
	context->length = 0;
	context->used = 0;
}

void
ext4_sha256_update(struct ext4_sha256 *context, const void *buffer, size_t length)
{
	const uint8_t *bytes = buffer;
	size_t part;

	context->length += (uint64_t)length;
	while (length != 0) {
		if (context->used == 0 && length >= EXT4_SHA256_BLOCK_SIZE) {
			ext4_sha256_block(context, bytes);
			bytes += EXT4_SHA256_BLOCK_SIZE;
			length -= EXT4_SHA256_BLOCK_SIZE;
			continue;
		}
		part = EXT4_SHA256_BLOCK_SIZE - context->used;
		if (part > length) {
			part = length;
		}
		ext4_copy(context->block + context->used, bytes, part);
		context->used += (uint32_t)part;
		bytes += part;
		length -= part;
		if (context->used == EXT4_SHA256_BLOCK_SIZE) {
			ext4_sha256_block(context, context->block);
			context->used = 0;
		}
	}
}

void
ext4_sha256_final(struct ext4_sha256 *context, uint8_t *digest)
{
	uint64_t bits = context->length * 8U;
	unsigned int index;

	context->block[context->used++] = 0x80U;
	if (context->used > EXT4_SHA256_BLOCK_SIZE - 8U) {
		ext4_zero(context->block + context->used, EXT4_SHA256_BLOCK_SIZE - context->used);
		ext4_sha256_block(context, context->block);
		context->used = 0;
	}
	ext4_zero(context->block + context->used, EXT4_SHA256_BLOCK_SIZE - 8U - context->used);
	for (index = 0; index < 8U; index++) {
		context->block[EXT4_SHA256_BLOCK_SIZE - 1U - index] =
		    (uint8_t)(bits >> (8U * index));
	}
	ext4_sha256_block(context, context->block);
	for (index = 0; index < EXT4_SHA256_DIGEST_SIZE; index++) {
		digest[index] = (uint8_t)(context->state[index / 4U] >> (24U - 8U * (index % 4U)));
	}
}

void
ext4_sha512_init(struct ext4_sha512 *context)
{
	static const uint64_t initial[8] = { UINT64_C(0x6a09e667f3bcc908),
		UINT64_C(0xbb67ae8584caa73b), UINT64_C(0x3c6ef372fe94f82b),
		UINT64_C(0xa54ff53a5f1d36f1), UINT64_C(0x510e527fade682d1),
		UINT64_C(0x9b05688c2b3e6c1f), UINT64_C(0x1f83d9abfb41bd6b),
		UINT64_C(0x5be0cd19137e2179) };

	ext4_copy(context->state, initial, sizeof(initial));
	context->length = 0;
	context->used = 0;
}

void
ext4_sha512_update(struct ext4_sha512 *context, const void *buffer, size_t length)
{
	const uint8_t *bytes = buffer;
	size_t part;

	context->length += (uint64_t)length;
	while (length != 0) {
		if (context->used == 0 && length >= EXT4_SHA512_BLOCK_SIZE) {
			ext4_sha512_block(context, bytes);
			bytes += EXT4_SHA512_BLOCK_SIZE;
			length -= EXT4_SHA512_BLOCK_SIZE;
			continue;
		}
		part = EXT4_SHA512_BLOCK_SIZE - context->used;
		if (part > length) {
			part = length;
		}
		ext4_copy(context->block + context->used, bytes, part);
		context->used += (uint32_t)part;
		bytes += part;
		length -= part;
		if (context->used == EXT4_SHA512_BLOCK_SIZE) {
			ext4_sha512_block(context, context->block);
			context->used = 0;
		}
	}
}

void
ext4_sha512_final(struct ext4_sha512 *context, uint8_t *digest)
{
	uint64_t bits = context->length * 8U;
	unsigned int index;

	context->block[context->used++] = 0x80U;
	if (context->used > EXT4_SHA512_BLOCK_SIZE - 16U) {
		ext4_zero(context->block + context->used, EXT4_SHA512_BLOCK_SIZE - context->used);
		ext4_sha512_block(context, context->block);
		context->used = 0;
	}
	/* Messages here stay below 2^61 bytes, so the upper length word is zero. */
	ext4_zero(context->block + context->used, EXT4_SHA512_BLOCK_SIZE - 8U - context->used);
	for (index = 0; index < 8U; index++) {
		context->block[EXT4_SHA512_BLOCK_SIZE - 1U - index] =
		    (uint8_t)(bits >> (8U * index));
	}
	ext4_sha512_block(context, context->block);
	for (index = 0; index < EXT4_SHA512_DIGEST_SIZE; index++) {
		digest[index] = (uint8_t)(context->state[index / 8U] >> (56U - 8U * (index % 8U)));
	}
}

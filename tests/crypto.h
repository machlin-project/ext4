/* SPDX-License-Identifier: BSD-3-Clause */
#ifndef MACHLIN_EXT4_TEST_CRYPTO_H
#define MACHLIN_EXT4_TEST_CRYPTO_H

#include "sha.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

/* Reference cryptography for tests, standing in for an adapter's platform library:
 * FIPS-197 AES, IEEE 1619 XTS, CBC with ciphertext stealing in the CS3 form of Linux's
 * cts(cbc(aes)), and RFC 2104 HMAC with RFC 5869 HKDF over the core's SHA-512. It is
 * written for clarity rather than speed or side-channel resistance and never ships. */

#define TEST_AES_BLOCK 16U
#define TEST_AES_MAX_ROUNDS 14U
#define TEST_SHA512_BLOCK 128U
#define TEST_SHA512_DIGEST 64U

struct test_aes {
	uint8_t round_keys[(TEST_AES_MAX_ROUNDS + 1U) * TEST_AES_BLOCK];
	unsigned int rounds;
};

static const uint8_t test_aes_sbox[256] = { 0x63, 0x7c, 0x77, 0x7b, 0xf2, 0x6b, 0x6f, 0xc5, 0x30,
	0x01, 0x67, 0x2b, 0xfe, 0xd7, 0xab, 0x76, 0xca, 0x82, 0xc9, 0x7d, 0xfa, 0x59, 0x47, 0xf0,
	0xad, 0xd4, 0xa2, 0xaf, 0x9c, 0xa4, 0x72, 0xc0, 0xb7, 0xfd, 0x93, 0x26, 0x36, 0x3f, 0xf7,
	0xcc, 0x34, 0xa5, 0xe5, 0xf1, 0x71, 0xd8, 0x31, 0x15, 0x04, 0xc7, 0x23, 0xc3, 0x18, 0x96,
	0x05, 0x9a, 0x07, 0x12, 0x80, 0xe2, 0xeb, 0x27, 0xb2, 0x75, 0x09, 0x83, 0x2c, 0x1a, 0x1b,
	0x6e, 0x5a, 0xa0, 0x52, 0x3b, 0xd6, 0xb3, 0x29, 0xe3, 0x2f, 0x84, 0x53, 0xd1, 0x00, 0xed,
	0x20, 0xfc, 0xb1, 0x5b, 0x6a, 0xcb, 0xbe, 0x39, 0x4a, 0x4c, 0x58, 0xcf, 0xd0, 0xef, 0xaa,
	0xfb, 0x43, 0x4d, 0x33, 0x85, 0x45, 0xf9, 0x02, 0x7f, 0x50, 0x3c, 0x9f, 0xa8, 0x51, 0xa3,
	0x40, 0x8f, 0x92, 0x9d, 0x38, 0xf5, 0xbc, 0xb6, 0xda, 0x21, 0x10, 0xff, 0xf3, 0xd2, 0xcd,
	0x0c, 0x13, 0xec, 0x5f, 0x97, 0x44, 0x17, 0xc4, 0xa7, 0x7e, 0x3d, 0x64, 0x5d, 0x19, 0x73,
	0x60, 0x81, 0x4f, 0xdc, 0x22, 0x2a, 0x90, 0x88, 0x46, 0xee, 0xb8, 0x14, 0xde, 0x5e, 0x0b,
	0xdb, 0xe0, 0x32, 0x3a, 0x0a, 0x49, 0x06, 0x24, 0x5c, 0xc2, 0xd3, 0xac, 0x62, 0x91, 0x95,
	0xe4, 0x79, 0xe7, 0xc8, 0x37, 0x6d, 0x8d, 0xd5, 0x4e, 0xa9, 0x6c, 0x56, 0xf4, 0xea, 0x65,
	0x7a, 0xae, 0x08, 0xba, 0x78, 0x25, 0x2e, 0x1c, 0xa6, 0xb4, 0xc6, 0xe8, 0xdd, 0x74, 0x1f,
	0x4b, 0xbd, 0x8b, 0x8a, 0x70, 0x3e, 0xb5, 0x66, 0x48, 0x03, 0xf6, 0x0e, 0x61, 0x35, 0x57,
	0xb9, 0x86, 0xc1, 0x1d, 0x9e, 0xe1, 0xf8, 0x98, 0x11, 0x69, 0xd9, 0x8e, 0x94, 0x9b, 0x1e,
	0x87, 0xe9, 0xce, 0x55, 0x28, 0xdf, 0x8c, 0xa1, 0x89, 0x0d, 0xbf, 0xe6, 0x42, 0x68, 0x41,
	0x99, 0x2d, 0x0f, 0xb0, 0x54, 0xbb, 0x16 };

static uint8_t
test_aes_times(uint8_t value, uint8_t factor)
{
	uint8_t product = 0;

	while (factor != 0) {
		if (factor & 1U) {
			product ^= value;
		}
		value = (uint8_t)((value << 1) ^ ((value & 0x80U) ? 0x1bU : 0U));
		factor >>= 1;
	}
	return product;
}

static uint8_t
test_aes_inverse_sbox(uint8_t value)
{
	unsigned int index;

	for (index = 0; index < 256U; index++) {
		if (test_aes_sbox[index] == value) {
			break;
		}
	}
	return (uint8_t)index;
}

/* Key sizes of 16 or 32 bytes select AES-128 or AES-256. */
static void
test_aes_init(struct test_aes *aes, const uint8_t *key, size_t size)
{
	uint8_t word[4];
	uint8_t swap;
	uint8_t constant = 1;
	unsigned int words = (unsigned int)(size / 4U);
	unsigned int index;
	unsigned int total;

	aes->rounds = words + 6U;
	total = 4U * (aes->rounds + 1U);
	memcpy(aes->round_keys, key, size);
	for (index = words; index < total; index++) {
		memcpy(word, aes->round_keys + (index - 1U) * 4U, 4);
		if (index % words == 0) {
			swap = word[0];
			word[0] = (uint8_t)(test_aes_sbox[word[1]] ^ constant);
			word[1] = test_aes_sbox[word[2]];
			word[2] = test_aes_sbox[word[3]];
			word[3] = test_aes_sbox[swap];
			constant = test_aes_times(constant, 2);
		} else if (words > 6U && index % words == 4U) {
			word[0] = test_aes_sbox[word[0]];
			word[1] = test_aes_sbox[word[1]];
			word[2] = test_aes_sbox[word[2]];
			word[3] = test_aes_sbox[word[3]];
		}
		aes->round_keys[index * 4U] = aes->round_keys[(index - words) * 4U] ^ word[0];
		aes->round_keys[index * 4U + 1U] =
		    aes->round_keys[(index - words) * 4U + 1U] ^ word[1];
		aes->round_keys[index * 4U + 2U] =
		    aes->round_keys[(index - words) * 4U + 2U] ^ word[2];
		aes->round_keys[index * 4U + 3U] =
		    aes->round_keys[(index - words) * 4U + 3U] ^ word[3];
	}
}

static void
test_aes_add(uint8_t *state, const uint8_t *round_key)
{
	unsigned int index;

	for (index = 0; index < TEST_AES_BLOCK; index++) {
		state[index] ^= round_key[index];
	}
}

/* The state is column-major: byte 4 * column + row. */
static void
test_aes_shift(uint8_t *state, bool inverse)
{
	uint8_t copy[TEST_AES_BLOCK];
	unsigned int row;
	unsigned int column;

	memcpy(copy, state, sizeof(copy));
	for (row = 1; row < 4U; row++) {
		for (column = 0; column < 4U; column++) {
			if (inverse) {
				state[4U * ((column + row) % 4U) + row] = copy[4U * column + row];
			} else {
				state[4U * column + row] = copy[4U * ((column + row) % 4U) + row];
			}
		}
	}
}

static void
test_aes_mix(uint8_t *state, bool inverse)
{
	static const uint8_t forward[4] = { 2, 3, 1, 1 };
	static const uint8_t backward[4] = { 14, 11, 13, 9 };
	const uint8_t *factors = inverse ? backward : forward;
	uint8_t column[4];
	unsigned int index;
	unsigned int row;

	for (index = 0; index < 4U; index++) {
		memcpy(column, state + 4U * index, 4);
		for (row = 0; row < 4U; row++) {
			state[4U * index + row] =
			    (uint8_t)(test_aes_times(column[row], factors[0]) ^
				test_aes_times(column[(row + 1U) % 4U], factors[1]) ^
				test_aes_times(column[(row + 2U) % 4U], factors[2]) ^
				test_aes_times(column[(row + 3U) % 4U], factors[3]));
		}
	}
}

static void
test_aes_encrypt(const struct test_aes *aes, const uint8_t *input, uint8_t *output)
{
	uint8_t state[TEST_AES_BLOCK];
	unsigned int round;
	unsigned int index;

	memcpy(state, input, sizeof(state));
	test_aes_add(state, aes->round_keys);
	for (round = 1; round <= aes->rounds; round++) {
		for (index = 0; index < TEST_AES_BLOCK; index++) {
			state[index] = test_aes_sbox[state[index]];
		}
		test_aes_shift(state, false);
		if (round != aes->rounds) {
			test_aes_mix(state, false);
		}
		test_aes_add(state, aes->round_keys + round * TEST_AES_BLOCK);
	}
	memcpy(output, state, sizeof(state));
}

static void
test_aes_decrypt(const struct test_aes *aes, const uint8_t *input, uint8_t *output)
{
	uint8_t state[TEST_AES_BLOCK];
	unsigned int round;
	unsigned int index;

	memcpy(state, input, sizeof(state));
	test_aes_add(state, aes->round_keys + aes->rounds * TEST_AES_BLOCK);
	for (round = aes->rounds; round > 0; round--) {
		test_aes_shift(state, true);
		for (index = 0; index < TEST_AES_BLOCK; index++) {
			state[index] = test_aes_inverse_sbox(state[index]);
		}
		test_aes_add(state, aes->round_keys + (round - 1U) * TEST_AES_BLOCK);
		if (round != 1U) {
			test_aes_mix(state, true);
		}
	}
	memcpy(output, state, sizeof(state));
}

/* IEEE 1619 XTS over whole 16-byte blocks: key is two AES keys, data then tweak. */
static void
test_aes_xts(const uint8_t *key, size_t key_size, const uint8_t *tweak, bool encrypt,
    const uint8_t *input, uint8_t *output, size_t length)
{
	struct test_aes data;
	struct test_aes whitening;
	uint8_t mask[TEST_AES_BLOCK];
	uint8_t block[TEST_AES_BLOCK];
	uint8_t carry;
	uint8_t next;
	size_t offset;
	unsigned int index;

	test_aes_init(&data, key, key_size / 2U);
	test_aes_init(&whitening, key + key_size / 2U, key_size / 2U);
	test_aes_encrypt(&whitening, tweak, mask);
	for (offset = 0; offset < length; offset += TEST_AES_BLOCK) {
		for (index = 0; index < TEST_AES_BLOCK; index++) {
			block[index] = input[offset + index] ^ mask[index];
		}
		if (encrypt) {
			test_aes_encrypt(&data, block, block);
		} else {
			test_aes_decrypt(&data, block, block);
		}
		for (index = 0; index < TEST_AES_BLOCK; index++) {
			output[offset + index] = block[index] ^ mask[index];
		}
		/* Multiply the mask by x in GF(2^128), little-endian. */
		carry = 0;
		for (index = 0; index < TEST_AES_BLOCK; index++) {
			next = (uint8_t)(mask[index] >> 7);
			mask[index] = (uint8_t)((mask[index] << 1) | carry);
			carry = next;
		}
		if (carry) {
			mask[0] ^= 0x87U;
		}
	}
}

/* CBC with ciphertext stealing, CS3: after plain CBC over the zero-padded input, the
 * last two ciphertext blocks swap and the final one is truncated. One block is CBC. */
static void
test_aes_cts(const uint8_t *key, size_t key_size, const uint8_t *iv, bool encrypt,
    const uint8_t *input, uint8_t *output, size_t length)
{
	struct test_aes aes;
	uint8_t chain[TEST_AES_BLOCK];
	uint8_t block[TEST_AES_BLOCK];
	uint8_t last[TEST_AES_BLOCK];
	uint8_t middle[TEST_AES_BLOCK];
	size_t full = (length - 1U) / TEST_AES_BLOCK * TEST_AES_BLOCK;
	size_t tail = length - full;
	size_t offset;
	unsigned int index;

	test_aes_init(&aes, key, key_size);
	memcpy(chain, iv, sizeof(chain));
	if (length == TEST_AES_BLOCK || full == 0) {
		for (index = 0; index < TEST_AES_BLOCK; index++) {
			block[index] =
			    encrypt ? (uint8_t)(input[index] ^ chain[index]) : input[index];
		}
		if (encrypt) {
			test_aes_encrypt(&aes, block, output);
		} else {
			test_aes_decrypt(&aes, block, output);
			for (index = 0; index < TEST_AES_BLOCK; index++) {
				output[index] ^= chain[index];
			}
		}
		return;
	}
	/* Blocks before the last two are ordinary CBC. */
	for (offset = 0; offset + TEST_AES_BLOCK < full; offset += TEST_AES_BLOCK) {
		if (encrypt) {
			for (index = 0; index < TEST_AES_BLOCK; index++) {
				block[index] = input[offset + index] ^ chain[index];
			}
			test_aes_encrypt(&aes, block, output + offset);
			memcpy(chain, output + offset, sizeof(chain));
		} else {
			test_aes_decrypt(&aes, input + offset, block);
			for (index = 0; index < TEST_AES_BLOCK; index++) {
				output[offset + index] = block[index] ^ chain[index];
			}
			memcpy(chain, input + offset, sizeof(chain));
		}
	}
	offset = full - TEST_AES_BLOCK;
	if (encrypt) {
		for (index = 0; index < TEST_AES_BLOCK; index++) {
			block[index] = input[offset + index] ^ chain[index];
		}
		test_aes_encrypt(&aes, block, middle);
		memset(block, 0, sizeof(block));
		memcpy(block, input + full, tail);
		for (index = 0; index < TEST_AES_BLOCK; index++) {
			block[index] ^= middle[index];
		}
		test_aes_encrypt(&aes, block, last);
		memcpy(output + offset, last, TEST_AES_BLOCK);
		memcpy(output + full, middle, tail);
		return;
	}
	/* The stored order is the last full block, then the truncated one before it. */
	test_aes_decrypt(&aes, input + offset, block);
	memcpy(middle, input + full, tail);
	memcpy(middle + tail, block + tail, TEST_AES_BLOCK - tail);
	for (index = 0; index < tail; index++) {
		output[full + index] = block[index] ^ input[full + index];
	}
	test_aes_decrypt(&aes, middle, block);
	for (index = 0; index < TEST_AES_BLOCK; index++) {
		output[offset + index] = block[index] ^ chain[index];
	}
}

static void
test_hmac_sha512(
    const uint8_t *key, size_t key_size, const uint8_t *data, size_t length, uint8_t *digest)
{
	struct ext4_sha512 context;
	uint8_t pad[TEST_SHA512_BLOCK];
	uint8_t inner[TEST_SHA512_DIGEST];
	uint8_t block_key[TEST_SHA512_BLOCK];
	unsigned int index;

	memset(block_key, 0, sizeof(block_key));
	if (key_size > TEST_SHA512_BLOCK) {
		ext4_sha512_init(&context);
		ext4_sha512_update(&context, key, key_size);
		ext4_sha512_final(&context, block_key);
	} else {
		memcpy(block_key, key, key_size);
	}
	for (index = 0; index < TEST_SHA512_BLOCK; index++) {
		pad[index] = block_key[index] ^ 0x36U;
	}
	ext4_sha512_init(&context);
	ext4_sha512_update(&context, pad, sizeof(pad));
	ext4_sha512_update(&context, data, length);
	ext4_sha512_final(&context, inner);
	for (index = 0; index < TEST_SHA512_BLOCK; index++) {
		pad[index] = block_key[index] ^ 0x5cU;
	}
	ext4_sha512_init(&context);
	ext4_sha512_update(&context, pad, sizeof(pad));
	ext4_sha512_update(&context, inner, sizeof(inner));
	ext4_sha512_final(&context, digest);
}

/* RFC 5869 HKDF-SHA512 with the default salt of zero bytes, as fscrypt uses it. */
static void
test_hkdf_sha512(const uint8_t *secret, size_t secret_size, const uint8_t *info, size_t info_size,
    uint8_t *output, size_t length)
{
	static const uint8_t salt[TEST_SHA512_DIGEST] = { 0 };
	uint8_t key[TEST_SHA512_DIGEST];
	uint8_t block[TEST_SHA512_DIGEST + 256U + 1U];
	uint8_t previous[TEST_SHA512_DIGEST];
	size_t previous_size = 0;
	size_t produced = 0;
	size_t chunk;
	uint8_t counter = 1;

	test_hmac_sha512(salt, sizeof(salt), secret, secret_size, key);
	while (produced < length) {
		memcpy(block, previous, previous_size);
		memcpy(block + previous_size, info, info_size);
		block[previous_size + info_size] = counter++;
		test_hmac_sha512(key, sizeof(key), block, previous_size + info_size + 1U, previous);
		previous_size = sizeof(previous);
		chunk = length - produced < sizeof(previous) ? length - produced : sizeof(previous);
		memcpy(output + produced, previous, chunk);
		produced += chunk;
	}
}

#endif

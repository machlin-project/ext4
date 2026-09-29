/* SPDX-License-Identifier: BSD-3-Clause */
#define _POSIX_C_SOURCE 200809L
#include "sha.h"
#include "fscrypt.h"

#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#define SAMPLES 9U
#define SAMPLE_BYTES (8U * 1024U * 1024U)
#define MAX_BYTES 65536U

static double
now(void)
{
	struct timespec value;

	if (clock_gettime(CLOCK_MONOTONIC, &value) != 0) {
		abort();
	}
	return (double)value.tv_sec + (double)value.tv_nsec / 1000000000.0;
}

static void
digest(unsigned int algorithm, const uint8_t *bytes, size_t length, uint8_t *output)
{
	struct ext4_sha256 sha256;
	struct ext4_sha512 sha512;

	if (algorithm == 256U) {
		ext4_sha256_init(&sha256);
		ext4_sha256_update(&sha256, bytes, length);
		ext4_sha256_final(&sha256, output);
	} else {
		ext4_sha512_init(&sha512);
		ext4_sha512_update(&sha512, bytes, length);
		ext4_sha512_final(&sha512, output);
	}
}

static void
measure(unsigned int algorithm, const uint8_t *bytes, size_t length)
{
	uint8_t output[EXT4_SHA512_DIGEST_SIZE];
	uint8_t expected[sizeof(output)];
	size_t iterations = SAMPLE_BYTES / length;
	size_t index;
	unsigned int sample;
	double start;
	double seconds;

	digest(algorithm, bytes, length, expected);
	printf("{\"algorithm\":\"sha%u\",\"bytes\":%zu,\"iterations\":%zu,\"seconds\":[", algorithm,
	    length, iterations);
	for (sample = 0; sample < SAMPLES; sample++) {
		start = now();
		for (index = 0; index < iterations; index++) {
			digest(algorithm, bytes, length, output);
		}
		seconds = now() - start;
		if (!ext4_equal(output, expected, algorithm / 8U)) {
			abort();
		}
		printf("%s%.9f", sample == 0 ? "" : ",", seconds);
	}
	printf("],\"digest\":\"");
	for (index = 0; index < algorithm / 8U; index++) {
		printf("%02x", output[index]);
	}
	puts("\"}");
}

static void
measure_names(const uint8_t *cipher)
{
	struct ext4_fscrypt_nokey decoded;
	uint8_t name[EXT4_FSCRYPT_NOKEY_NAME_MAX];
	size_t size;
	size_t iteration;
	unsigned int sample;
	double start;
	double seconds;

	size = ext4_fscrypt_nokey_encode(cipher, EXT4_NAME_MAX, 0, 0, name);
	printf("{\"algorithm\":\"nokey-decode\",\"bytes\":%zu,\"iterations\":32768,\"seconds\":[",
	    size);
	for (sample = 0; sample < SAMPLES; sample++) {
		start = now();
		for (iteration = 0; iteration < 32768U; iteration++) {
			if (!ext4_fscrypt_nokey_decode(name, size, &decoded)) {
				abort();
			}
		}
		seconds = now() - start;
		if (!ext4_fscrypt_nokey_match(&decoded, cipher, EXT4_NAME_MAX)) {
			abort();
		}
		printf("%s%.9f", sample == 0 ? "" : ",", seconds);
	}
	puts("]}");
}

int
main(void)
{
	static const size_t lengths[] = { 64U, 1024U, 4096U, MAX_BYTES };
	uint8_t *allocation = malloc(MAX_BYTES + 3U);
	uint8_t *bytes;
	size_t index;
	unsigned int algorithm;

	if (allocation == NULL) {
		return 1;
	}
	bytes = allocation + 3U;
	for (index = 0; index < MAX_BYTES; index++) {
		bytes[index] = (uint8_t)(131U * index + 17U);
	}
	for (algorithm = 256U; algorithm <= 512U; algorithm += 256U) {
		for (index = 0; index < sizeof(lengths) / sizeof(lengths[0]); index++) {
			measure(algorithm, bytes, lengths[index]);
		}
	}
	measure_names(bytes);
	free(allocation);
	return 0;
}

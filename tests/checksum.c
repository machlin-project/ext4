/* SPDX-License-Identifier: BSD-3-Clause */
#include "internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECKSUM_MAX_LENGTH 65536U
#define CHECKSUM_ALIGNMENTS 16U

#define CHECK(expression)                                                                          \
	do {                                                                                       \
		if (!(expression)) {                                                               \
			fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #expression);           \
			exit(EXIT_FAILURE);                                                        \
		}                                                                                  \
	} while (0)

/* Independent bit-serial division. This deliberately uses neither the core's
 * remainder table nor its byte-at-a-time update. */
static uint32_t
reference_crc(uint32_t state, const uint8_t *bytes, size_t length)
{
	size_t index;
	unsigned int bit;
	bool carry;

	for (index = 0; index < length; index++) {
		for (bit = 0; bit < EXT4_BITS_PER_BYTE; bit++) {
			carry = ((state ^ (bytes[index] >> bit)) & 1U) != 0;
			state >>= 1;
			if (carry) {
				state ^= EXT4_CRC32C_POLYNOMIAL;
			}
		}
	}
	return state;
}

static uint16_t
reference_crc16(uint16_t state, const uint8_t *bytes, size_t length)
{
	size_t index;
	unsigned int bit;
	bool carry;

	for (index = 0; index < length; index++) {
		for (bit = 0; bit < EXT4_BITS_PER_BYTE; bit++) {
			carry = ((state ^ (bytes[index] >> bit)) & 1U) != 0;
			state >>= 1;
			if (carry) {
				state ^= EXT4_CRC16_POLYNOMIAL;
			}
		}
	}
	return state;
}

static void
transaction_checksum(const uint8_t *bytes)
{
	static const size_t lengths[] = { 0, 1, 2, 7, 16, 511, 1024, 4096, 65536 };
	static const uint32_t seeds[] = { 0, UINT32_MAX, 0x12345678U };
	uint32_t expected;
	uint32_t streamed;
	size_t seed;
	size_t item;
	size_t index;
	size_t alignment;
	unsigned int bit;
	bool carry;

	CHECK(ext4_crc32_be(UINT32_MAX, "123456789", 9) == 0x0376e6e7U);
	for (seed = 0; seed < sizeof(seeds) / sizeof(seeds[0]); seed++) {
		CHECK(ext4_crc32_be(seeds[seed], NULL, 0) == seeds[seed]);
		for (alignment = 0; alignment < CHECKSUM_ALIGNMENTS; alignment++) {
			for (item = 0; item < sizeof(lengths) / sizeof(lengths[0]); item++) {
				expected = seeds[seed];
				streamed = seeds[seed];
				for (index = 0; index < lengths[item]; index++) {
					for (bit = 0; bit < EXT4_BITS_PER_BYTE; bit++) {
						carry =
						    ((expected >> 31) ^
							(bytes[alignment + index] >> (7U - bit))) &
						    1U;
						expected <<= 1;
						if (carry) {
							expected ^= EXT4_CRC32_POLYNOMIAL;
						}
					}
					streamed =
					    ext4_crc32_be(streamed, bytes + alignment + index, 1);
				}
				CHECK(ext4_crc32_be(seeds[seed], bytes + alignment,
					  lengths[item]) == expected);
				CHECK(streamed == expected);
			}
		}
	}
	puts("PASS journal CRC32: published MPEG-2 vector, seeded/aligned ranges and streaming");
}

static void
legacy_checksum(const uint8_t *bytes)
{
	static const size_t lengths[] = { 0, 1, 2, 3, 15, 16, 17, 31, 32, 33, 63, 64, 65, 127, 128,
		255, 256, 511, 512, 1023, 1024 };
	static const uint16_t seeds[] = { 0, UINT16_MAX, 1, 0x8000U, 0x1234U, 0xcdefU };
	uint8_t byte;
	uint16_t expected;
	uint16_t state;
	size_t index;
	size_t seed;
	size_t alignment;
	size_t length;
	size_t split;
	size_t cases = 0;

	CHECK(ext4_crc16(0, "123456789", 9) == 0xbb3dU);
	CHECK(ext4_crc16(UINT16_MAX, "123456789", 9) == 0x4b37U);
	for (index = 0; index <= UINT8_MAX; index++) {
		byte = (uint8_t)index;
		CHECK(ext4_crc16(0, &byte, 1) == reference_crc16(0, &byte, 1));
	}
	for (seed = 0; seed < sizeof(seeds) / sizeof(seeds[0]); seed++) {
		CHECK(ext4_crc16(seeds[seed], NULL, 0) == seeds[seed]);
		for (alignment = 0; alignment < CHECKSUM_ALIGNMENTS; alignment++) {
			for (index = 0; index < sizeof(lengths) / sizeof(lengths[0]); index++) {
				length = lengths[index];
				split = length / 3;
				expected = reference_crc16(seeds[seed], bytes + alignment, length);
				CHECK(
				    ext4_crc16(seeds[seed], bytes + alignment, length) == expected);
				state = ext4_crc16(seeds[seed], bytes + alignment, split);
				CHECK(ext4_crc16(state, bytes + alignment + split,
					  length - split) == expected);
				cases++;
			}
		}
	}
	printf(
	    "PASS CRC16: all byte remainders and %zu seeded/aligned/range/stream cases\n", cases);
}

static void
group_checksums(void)
{
	static const uint32_t groups[] = { 0, 1, 7, 0x80000101U };
	static const uint8_t zero[sizeof(struct ext4_le16)] = { 0 };
	struct ext4_fs fs = { 0 };
	struct ext4_group result;
	struct ext4_group unchanged;
	uint8_t *bytes = malloc(EXT4_GROUP_MAX_SIZE);
	struct ext4_group_disk *disk = (struct ext4_group_disk *)bytes;
	uint8_t group_wire[sizeof(struct ext4_le32)];
	uint32_t checksum;
	uint16_t expected;
	size_t prefix = offsetof(struct ext4_group_disk, checksum);
	size_t suffix = prefix + sizeof(disk->checksum);
	size_t mode;
	size_t size;
	size_t index;
	size_t selected;
	size_t cases = 0;
	size_t mutations = 0;

	CHECK(bytes != NULL);
	fs.info.blocks = UINT64_C(1) << 34;
	fs.info.groups = UINT32_MAX;
	fs.info.block_size = 4096;
	fs.blocks_per_group = 32768;
	fs.inodes_per_group = 8192;
	fs.inode_size = 256;
	for (index = 0; index < sizeof(fs.info.uuid); index++) {
		fs.info.uuid[index] = (uint8_t)(17 + index * 31);
	}
	fs.checksum_seed = reference_crc(UINT32_MAX, fs.info.uuid, sizeof(fs.info.uuid));
	memset(&unchanged, 0xa5, sizeof(unchanged));
	for (mode = 0; mode < 3; mode++) {
		fs.metadata_checksum = mode != 0;
		fs.info.feature_ro_compat = mode == 0
		    ? EXT4_FEATURE_RO_GDT_CSUM
		    : (mode == 1 ? EXT4_FEATURE_RO_METADATA_CSUM : EXT4_GROUP_CHECKSUM_FEATURES);
		for (size = EXT4_GROUP_BASE_SIZE; size <= EXT4_GROUP_MAX_SIZE; size *= 2) {
			fs.descriptor_size = (uint16_t)size;
			fs.info.feature_incompat =
			    size == EXT4_GROUP_BASE_SIZE ? 0 : EXT4_FEATURE_INCOMPAT_64BIT;
			for (selected = 0; selected < sizeof(groups) / sizeof(groups[0]);
			    selected++) {
				for (index = 0; index < size; index++) {
					bytes[index] = (uint8_t)(index * 37 + 59);
				}
				ext4_encode32(&disk->block_bitmap_lo, 16);
				ext4_encode32(&disk->inode_bitmap_lo, 17);
				ext4_encode32(&disk->inode_table_lo, 32);
				ext4_encode16(&disk->free_blocks_lo, 123);
				ext4_encode16(&disk->free_inodes_lo, 37);
				if (size >= EXT4_GROUP_64_SIZE) {
					ext4_encode32(&disk->block_bitmap_hi, 1);
					ext4_encode32(&disk->inode_bitmap_hi, 2);
					ext4_encode32(&disk->inode_table_hi, 3);
					ext4_encode16(&disk->free_blocks_hi, 0);
					ext4_encode16(&disk->free_inodes_hi, 0);
				}
				for (index = 0; index < sizeof(group_wire); index++) {
					group_wire[index] = (uint8_t)(groups[selected] >>
					    (index * EXT4_BITS_PER_BYTE));
				}
				if (mode == 0) {
					expected = reference_crc16(
					    UINT16_MAX, fs.info.uuid, sizeof(fs.info.uuid));
					expected = reference_crc16(
					    expected, group_wire, sizeof(group_wire));
					expected = reference_crc16(expected, bytes, prefix);
					expected = reference_crc16(
					    expected, bytes + suffix, size - suffix);
				} else {
					checksum = reference_crc(
					    fs.checksum_seed, group_wire, sizeof(group_wire));
					checksum = reference_crc(checksum, bytes, prefix);
					checksum = reference_crc(checksum, zero, sizeof(zero));
					expected = (uint16_t)reference_crc(
					    checksum, bytes + suffix, size - suffix);
				}
				ext4_group_checksum_set(&fs, groups[selected], disk);
				CHECK(ext4_le16(&disk->checksum) == expected);
				CHECK(ext4_group_decode(&fs, groups[selected], disk, &result) ==
				    EXT4_OK);
				CHECK(ext4_le16(&disk->checksum) == expected);
				CHECK(ext4_group_decode(&fs, groups[selected] ^ 1U, disk,
					  &result) == EXT4_CORRUPT);
				/* Every descriptor byte, including high fields and its stored
				 * checksum, participates in legacy corruption detection. */
				for (index = 0; mode == 0 && index < size; index++) {
					bytes[index] ^= 1U;
					memcpy(&result, &unchanged, sizeof(result));
					CHECK(ext4_group_decode(&fs, groups[selected], disk,
						  &result) == EXT4_CORRUPT);
					CHECK(memcmp(&result, &unchanged, sizeof(result)) == 0);
					bytes[index] ^= 1U;
					mutations++;
				}
				cases++;
			}
		}
	}
	free(bytes);
	printf("PASS group checksums: %zu CRC16/CRC32C descriptors and %zu legacy mutations\n",
	    cases, mutations);
}

int
main(void)
{
	static const size_t lengths[] = { 0, 1, 2, 3, 4, 7, 8, 9, 15, 16, 17, 31, 32, 33, 63, 64,
		65, 255, 256, 257, 511, 512, 513, 1023, 1024, 1025, 4095, 4096, 4097, 65535,
		65536 };
	static const size_t strides[] = { 1, 3, 17, 253, 1024, 4096, 65535 };
	static const uint32_t seeds[] = { 0, UINT32_MAX, 1, 0x80000000U, 0x01234567U, 0x89abcdefU };
	uint8_t *bytes = malloc(CHECKSUM_MAX_LENGTH + CHECKSUM_ALIGNMENTS);
	uint8_t byte;
	uint32_t expected;
	uint32_t state;
	size_t alignment;
	size_t length;
	size_t seed;
	size_t stride;
	size_t offset;
	size_t chunk;
	size_t index;
	size_t cases = 0;

	CHECK(bytes != NULL);
	/* Conventional CRC32C check value, with the complement kept outside this
	 * raw-state API as it is for ext4's UUID/inode/block checksum chains. */
	CHECK(ext4_crc32c(UINT32_MAX, "123456789", 9) == (uint32_t)~UINT32_C(0xe3069283));
	for (index = 0; index <= UINT8_MAX; index++) {
		byte = (uint8_t)index;
		CHECK(ext4_crc32c(0, &byte, 1) == reference_crc(0, &byte, 1));
	}
	for (index = 0; index < CHECKSUM_MAX_LENGTH + CHECKSUM_ALIGNMENTS; index++) {
		bytes[index] = (uint8_t)(index * 17U + index / 251U);
	}
	for (seed = 0; seed < sizeof(seeds) / sizeof(seeds[0]); seed++) {
		CHECK(ext4_crc32c(seeds[seed], NULL, 0) == seeds[seed]);
		for (alignment = 0; alignment < CHECKSUM_ALIGNMENTS; alignment++) {
			for (length = 0; length < sizeof(lengths) / sizeof(lengths[0]); length++) {
				expected =
				    reference_crc(seeds[seed], bytes + alignment, lengths[length]);
				CHECK(ext4_crc32c(seeds[seed], bytes + alignment,
					  lengths[length]) == expected);
				cases++;
			}
			expected =
			    reference_crc(seeds[seed], bytes + alignment, CHECKSUM_MAX_LENGTH);
			for (stride = 0; stride < sizeof(strides) / sizeof(strides[0]); stride++) {
				state = seeds[seed];
				for (offset = 0; offset < CHECKSUM_MAX_LENGTH; offset += chunk) {
					chunk = strides[stride];
					if (chunk > CHECKSUM_MAX_LENGTH - offset) {
						chunk = CHECKSUM_MAX_LENGTH - offset;
					}
					state =
					    ext4_crc32c(state, bytes + alignment + offset, chunk);
				}
				CHECK(state == expected);
				cases++;
			}
		}
	}
	legacy_checksum(bytes);
	transaction_checksum(bytes);
	group_checksums();
	free(bytes);
	printf("PASS CRC32C: known vector, all byte remainders, %zu seeded/aligned/range/stream "
	       "cases\n",
	    cases);
	return EXIT_SUCCESS;
}

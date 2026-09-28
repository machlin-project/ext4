/* SPDX-License-Identifier: BSD-3-Clause */
#include "internal.h"

uint16_t
ext4_le16(const struct ext4_le16 *value)
{
	return (uint16_t)value->bytes[0] | (uint16_t)((uint16_t)value->bytes[1] << 8);
}

uint32_t
ext4_le32(const struct ext4_le32 *value)
{
	return (uint32_t)value->bytes[0] | ((uint32_t)value->bytes[1] << 8) |
	    ((uint32_t)value->bytes[2] << 16) | ((uint32_t)value->bytes[3] << 24);
}

void
ext4_encode16(struct ext4_le16 *output, uint16_t value)
{
	output->bytes[0] = (uint8_t)value;
	output->bytes[1] = (uint8_t)(value >> 8);
}

void
ext4_encode32(struct ext4_le32 *output, uint32_t value)
{
	unsigned int index;

	for (index = 0; index < sizeof(output->bytes); index++) {
		output->bytes[index] = (uint8_t)(value >> (index * 8));
	}
}

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
	size_t index;

	for (index = 0; index < length; index++) {
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

/* Nibble remainders for the non-reflected IEEE polynomial used by JBD2 v1.
 * Seed and final-complement conventions remain with the caller. */
_Static_assert(EXT4_CRC32_POLYNOMIAL == 0x04c11db7U, "CRC32 table polynomial");
static const uint32_t ext4_crc32_be_table[16] = {
	0x00000000U,
	0x04c11db7U,
	0x09823b6eU,
	0x0d4326d9U,
	0x130476dcU,
	0x17c56b6bU,
	0x1a864db2U,
	0x1e475005U,
	0x2608edb8U,
	0x22c9f00fU,
	0x2f8ad6d6U,
	0x2b4bcb61U,
	0x350c9b64U,
	0x31cd86d3U,
	0x3c8ea00aU,
	0x384fbdbdU,
};

uint32_t
ext4_crc32_be(uint32_t checksum, const void *buffer, size_t length)
{
	const uint8_t *bytes = buffer;
	size_t index;

	for (index = 0; index < length; index++) {
		checksum ^= (uint32_t)bytes[index] << 24;
		checksum = (checksum << 4) ^ ext4_crc32_be_table[checksum >> 28];
		checksum = (checksum << 4) ^ ext4_crc32_be_table[checksum >> 28];
	}
	return checksum;
}

#if defined(__clang__) && defined(__aarch64__) && defined(__ARM_FEATURE_CRC32)
/* The compilation target guarantees the ARMv8 CRC32C instructions. They use
 * general registers and the same raw reflected update as the remainder table;
 * selection happens at compile time, without a runtime CPU-feature probe. */
#define EXT4_CRC32C_INSTRUCTIONS 1
#define EXT4_CRC32C_WORD_BYTES 8U
#else
/* Byte remainders for the reflected Castagnoli polynomial. Read-only storage
 * avoids per-mount allocation, initialization races and platform CPU features.
 * The caller owns seed and final-complement conventions. */
_Static_assert(EXT4_CRC32C_POLYNOMIAL == 0x82f63b78U, "CRC32C table polynomial");
static const uint32_t ext4_crc32c_table[UINT8_MAX + 1U] = {
	0x00000000U,
	0xf26b8303U,
	0xe13b70f7U,
	0x1350f3f4U,
	0xc79a971fU,
	0x35f1141cU,
	0x26a1e7e8U,
	0xd4ca64ebU,
	0x8ad958cfU,
	0x78b2dbccU,
	0x6be22838U,
	0x9989ab3bU,
	0x4d43cfd0U,
	0xbf284cd3U,
	0xac78bf27U,
	0x5e133c24U,
	0x105ec76fU,
	0xe235446cU,
	0xf165b798U,
	0x030e349bU,
	0xd7c45070U,
	0x25afd373U,
	0x36ff2087U,
	0xc494a384U,
	0x9a879fa0U,
	0x68ec1ca3U,
	0x7bbcef57U,
	0x89d76c54U,
	0x5d1d08bfU,
	0xaf768bbcU,
	0xbc267848U,
	0x4e4dfb4bU,
	0x20bd8edeU,
	0xd2d60dddU,
	0xc186fe29U,
	0x33ed7d2aU,
	0xe72719c1U,
	0x154c9ac2U,
	0x061c6936U,
	0xf477ea35U,
	0xaa64d611U,
	0x580f5512U,
	0x4b5fa6e6U,
	0xb93425e5U,
	0x6dfe410eU,
	0x9f95c20dU,
	0x8cc531f9U,
	0x7eaeb2faU,
	0x30e349b1U,
	0xc288cab2U,
	0xd1d83946U,
	0x23b3ba45U,
	0xf779deaeU,
	0x05125dadU,
	0x1642ae59U,
	0xe4292d5aU,
	0xba3a117eU,
	0x4851927dU,
	0x5b016189U,
	0xa96ae28aU,
	0x7da08661U,
	0x8fcb0562U,
	0x9c9bf696U,
	0x6ef07595U,
	0x417b1dbcU,
	0xb3109ebfU,
	0xa0406d4bU,
	0x522bee48U,
	0x86e18aa3U,
	0x748a09a0U,
	0x67dafa54U,
	0x95b17957U,
	0xcba24573U,
	0x39c9c670U,
	0x2a993584U,
	0xd8f2b687U,
	0x0c38d26cU,
	0xfe53516fU,
	0xed03a29bU,
	0x1f682198U,
	0x5125dad3U,
	0xa34e59d0U,
	0xb01eaa24U,
	0x42752927U,
	0x96bf4dccU,
	0x64d4cecfU,
	0x77843d3bU,
	0x85efbe38U,
	0xdbfc821cU,
	0x2997011fU,
	0x3ac7f2ebU,
	0xc8ac71e8U,
	0x1c661503U,
	0xee0d9600U,
	0xfd5d65f4U,
	0x0f36e6f7U,
	0x61c69362U,
	0x93ad1061U,
	0x80fde395U,
	0x72966096U,
	0xa65c047dU,
	0x5437877eU,
	0x4767748aU,
	0xb50cf789U,
	0xeb1fcbadU,
	0x197448aeU,
	0x0a24bb5aU,
	0xf84f3859U,
	0x2c855cb2U,
	0xdeeedfb1U,
	0xcdbe2c45U,
	0x3fd5af46U,
	0x7198540dU,
	0x83f3d70eU,
	0x90a324faU,
	0x62c8a7f9U,
	0xb602c312U,
	0x44694011U,
	0x5739b3e5U,
	0xa55230e6U,
	0xfb410cc2U,
	0x092a8fc1U,
	0x1a7a7c35U,
	0xe811ff36U,
	0x3cdb9bddU,
	0xceb018deU,
	0xdde0eb2aU,
	0x2f8b6829U,
	0x82f63b78U,
	0x709db87bU,
	0x63cd4b8fU,
	0x91a6c88cU,
	0x456cac67U,
	0xb7072f64U,
	0xa457dc90U,
	0x563c5f93U,
	0x082f63b7U,
	0xfa44e0b4U,
	0xe9141340U,
	0x1b7f9043U,
	0xcfb5f4a8U,
	0x3dde77abU,
	0x2e8e845fU,
	0xdce5075cU,
	0x92a8fc17U,
	0x60c37f14U,
	0x73938ce0U,
	0x81f80fe3U,
	0x55326b08U,
	0xa759e80bU,
	0xb4091bffU,
	0x466298fcU,
	0x1871a4d8U,
	0xea1a27dbU,
	0xf94ad42fU,
	0x0b21572cU,
	0xdfeb33c7U,
	0x2d80b0c4U,
	0x3ed04330U,
	0xccbbc033U,
	0xa24bb5a6U,
	0x502036a5U,
	0x4370c551U,
	0xb11b4652U,
	0x65d122b9U,
	0x97baa1baU,
	0x84ea524eU,
	0x7681d14dU,
	0x2892ed69U,
	0xdaf96e6aU,
	0xc9a99d9eU,
	0x3bc21e9dU,
	0xef087a76U,
	0x1d63f975U,
	0x0e330a81U,
	0xfc588982U,
	0xb21572c9U,
	0x407ef1caU,
	0x532e023eU,
	0xa145813dU,
	0x758fe5d6U,
	0x87e466d5U,
	0x94b49521U,
	0x66df1622U,
	0x38cc2a06U,
	0xcaa7a905U,
	0xd9f75af1U,
	0x2b9cd9f2U,
	0xff56bd19U,
	0x0d3d3e1aU,
	0x1e6dcdeeU,
	0xec064eedU,
	0xc38d26c4U,
	0x31e6a5c7U,
	0x22b65633U,
	0xd0ddd530U,
	0x0417b1dbU,
	0xf67c32d8U,
	0xe52cc12cU,
	0x1747422fU,
	0x49547e0bU,
	0xbb3ffd08U,
	0xa86f0efcU,
	0x5a048dffU,
	0x8ecee914U,
	0x7ca56a17U,
	0x6ff599e3U,
	0x9d9e1ae0U,
	0xd3d3e1abU,
	0x21b862a8U,
	0x32e8915cU,
	0xc083125fU,
	0x144976b4U,
	0xe622f5b7U,
	0xf5720643U,
	0x07198540U,
	0x590ab964U,
	0xab613a67U,
	0xb831c993U,
	0x4a5a4a90U,
	0x9e902e7bU,
	0x6cfbad78U,
	0x7fab5e8cU,
	0x8dc0dd8fU,
	0xe330a81aU,
	0x115b2b19U,
	0x020bd8edU,
	0xf0605beeU,
	0x24aa3f05U,
	0xd6c1bc06U,
	0xc5914ff2U,
	0x37faccf1U,
	0x69e9f0d5U,
	0x9b8273d6U,
	0x88d28022U,
	0x7ab90321U,
	0xae7367caU,
	0x5c18e4c9U,
	0x4f48173dU,
	0xbd23943eU,
	0xf36e6f75U,
	0x0105ec76U,
	0x12551f82U,
	0xe03e9c81U,
	0x34f4f86aU,
	0xc69f7b69U,
	0xd5cf889dU,
	0x27a40b9eU,
	0x79b737baU,
	0x8bdcb4b9U,
	0x988c474dU,
	0x6ae7c44eU,
	0xbe2da0a5U,
	0x4c4623a6U,
	0x5f16d052U,
	0xad7d5351U,
};
#endif

uint32_t
ext4_crc32c(uint32_t checksum, const void *buffer, size_t length)
{
	const uint8_t *bytes = buffer;
	size_t index = 0;
#ifdef EXT4_CRC32C_INSTRUCTIONS
	uint64_t word;
	unsigned int shift;

	for (; length - index >= EXT4_CRC32C_WORD_BYTES; index += EXT4_CRC32C_WORD_BYTES) {
		word = 0;
		for (shift = 0; shift < EXT4_CRC32C_WORD_BYTES; shift++) {
			word |= (uint64_t)bytes[index + shift] << (shift * EXT4_BITS_PER_BYTE);
		}
		checksum = __builtin_arm_crc32cd(checksum, word);
	}
	for (; index < length; index++) {
		checksum = __builtin_arm_crc32cb(checksum, bytes[index]);
	}
#else
	for (; index < length; index++) {
		checksum = ext4_crc32c_table[(uint8_t)(checksum ^ bytes[index])] ^
		    (checksum >> EXT4_BITS_PER_BYTE);
	}
#endif
	return checksum;
}

/* Legacy group descriptors use CRC16 with a raw caller-supplied seed.
 * Two nibble steps need only a 32-byte immutable remainder table. */
#define EXT4_CRC16_NIBBLE_BITS 4U
#define EXT4_CRC16_NIBBLE_MASK 0x0fU
_Static_assert(EXT4_CRC16_POLYNOMIAL == 0xa001U, "CRC16 table polynomial");
static const uint16_t ext4_crc16_table[EXT4_CRC16_NIBBLE_MASK + 1U] = { 0x0000U, 0xcc01U, 0xd801U,
	0x1400U, 0xf001U, 0x3c00U, 0x2800U, 0xe401U, 0xa001U, 0x6c00U, 0x7800U, 0xb401U, 0x5000U,
	0x9c01U, 0x8801U, 0x4400U };

uint16_t
ext4_crc16(uint16_t checksum, const void *buffer, size_t length)
{
	const uint8_t *bytes = buffer;
	size_t index;

	for (index = 0; index < length; index++) {
		checksum ^= bytes[index];
		checksum = (uint16_t)((checksum >> EXT4_CRC16_NIBBLE_BITS) ^
		    ext4_crc16_table[checksum & EXT4_CRC16_NIBBLE_MASK]);
		checksum = (uint16_t)((checksum >> EXT4_CRC16_NIBBLE_BITS) ^
		    ext4_crc16_table[checksum & EXT4_CRC16_NIBBLE_MASK]);
	}
	return checksum;
}

uint32_t
ext4_inode_seed(const struct ext4_fs *fs, const struct ext4_inode *inode)
{
	struct ext4_le32 number;
	struct ext4_le32 generation;
	uint32_t checksum;

	ext4_encode32(&number, inode->number);
	ext4_encode32(&generation, inode->generation);
	checksum = ext4_crc32c(fs->checksum_seed, &number, sizeof(number));
	return ext4_crc32c(checksum, &generation, sizeof(generation));
}

enum ext4_result
ext4_device_read(struct ext4_fs *fs, uint64_t offset, void *buffer, size_t length)
{
	if (fs->aborted) {
		return EXT4_RECOVERY_REQUIRED;
	}
	if (offset > fs->environment.size_bytes || length > fs->environment.size_bytes - offset) {
		return EXT4_CORRUPT;
	}
	return fs->environment.read(fs->environment.context, offset, buffer, length);
}

enum ext4_result
ext4_block_read(struct ext4_fs *fs, uint64_t block, void *buffer)
{
	if (block < fs->first_data_block || block >= fs->info.blocks) {
		return EXT4_CORRUPT;
	}
	return ext4_device_read(fs, block * fs->info.block_size, buffer, fs->info.block_size);
}

const char *
ext4_result_string(enum ext4_result result)
{
	switch (result) {
	case EXT4_OK:
		return "success";
	case EXT4_INVALID_ARGUMENT:
		return "invalid argument";
	case EXT4_NOT_EXT4:
		return "not an ext4 filesystem";
	case EXT4_UNSUPPORTED:
		return "unsupported filesystem feature";
	case EXT4_CORRUPT:
		return "corrupt filesystem metadata";
	case EXT4_IO:
		return "resource I/O failed";
	case EXT4_NO_MEMORY:
		return "allocation failed";
	case EXT4_NOT_FOUND:
		return "entry not found";
	case EXT4_NOT_DIRECTORY:
		return "not a directory";
	case EXT4_NAME_TOO_LONG:
		return "name too long";
	case EXT4_READ_ONLY:
		return "read-only filesystem";
	case EXT4_RECOVERY_REQUIRED:
		return "filesystem requires recovery";
	case EXT4_IS_DIRECTORY:
		return "operation requires a nondirectory";
	case EXT4_RANGE:
		return "value out of range";
	case EXT4_STALE:
		return "inode generation changed";
	case EXT4_NO_SPACE:
		return "no free filesystem space";
	case EXT4_EXISTS:
		return "entry already exists";
	case EXT4_TOO_MANY_LINKS:
		return "too many hard links";
	case EXT4_NOT_EMPTY:
		return "directory not empty";
	case EXT4_PERMISSION_DENIED:
		return "inode policy forbids this operation";
	}
	return "unknown filesystem error";
}

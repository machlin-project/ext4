/* SPDX-License-Identifier: BSD-3-Clause */
#ifndef EXT4_CHECK_PROTOCOL_H
#define EXT4_CHECK_PROTOCOL_H

#include <stddef.h>
#include <stdint.h>

#define EXT4_CHECK_MAGIC UINT32_C(0x4534434b)
#define EXT4_CHECK_VERSION UINT32_C(1)
#define EXT4_CHECK_MAX_TRANSFER (1024U * 1024U)
#define EXT4_CHECK_REQUEST_SECONDS 30U
#define EXT4_CHECK_WRITABLE UINT32_C(1)
#define EXT4_CHECK_RESOURCE_NAME "FSKit-block-resource"

enum ext4_check_operation {
	EXT4_CHECK_OPEN = 1,
	EXT4_CHECK_READ,
	EXT4_CHECK_WRITE,
	EXT4_CHECK_FLUSH,
	EXT4_CHECK_CLOSE
};

struct ext4_check_u32 {
	uint8_t bytes[4];
};

struct ext4_check_u64 {
	uint8_t bytes[8];
};

/* A private inherited socket carries one resource, never a device path or key.
 * Each request has exactly one response; only READ/WRITE carry payload bytes. */
struct ext4_check_message {
	struct ext4_check_u32 magic;
	struct ext4_check_u32 version;
	struct ext4_check_u32 operation;
	struct ext4_check_u32 error;
	struct ext4_check_u64 offset;
	struct ext4_check_u32 length;
	struct ext4_check_u32 flags;
};

_Static_assert(sizeof(struct ext4_check_message) == 32, "maintenance frame size");

static inline uint32_t
ext4_check_decode32(struct ext4_check_u32 value)
{
	return (uint32_t)value.bytes[0] << 24 | (uint32_t)value.bytes[1] << 16 |
	    (uint32_t)value.bytes[2] << 8 | value.bytes[3];
}

static inline struct ext4_check_u32
ext4_check_encode32(uint32_t value)
{
	struct ext4_check_u32 encoded = { { (uint8_t)(value >> 24), (uint8_t)(value >> 16),
	    (uint8_t)(value >> 8), (uint8_t)value } };

	return encoded;
}

static inline uint64_t
ext4_check_decode64(struct ext4_check_u64 value)
{
	uint64_t decoded = 0;
	size_t index;

	for (index = 0; index < sizeof(value.bytes); index++) {
		decoded = decoded << 8 | value.bytes[index];
	}
	return decoded;
}

static inline struct ext4_check_u64
ext4_check_encode64(uint64_t value)
{
	struct ext4_check_u64 encoded = { 0 };
	size_t index;

	for (index = 0; index < sizeof(encoded.bytes); index++) {
		encoded.bytes[sizeof(encoded.bytes) - index - 1] = (uint8_t)value;
		value >>= 8;
	}
	return encoded;
}

/* Absolute monotonic deadlines prevent a slow peer extending a transfer forever.
 * The descriptor must be a stream socket; sends suppress SIGPIPE. */
int64_t ext4_check_deadline(unsigned seconds);
int ext4_check_receive(int socket, void *bytes, size_t length, int64_t deadline);
int ext4_check_send(int socket, const void *bytes, size_t length, int64_t deadline);

#endif

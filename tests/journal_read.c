/* SPDX-License-Identifier: BSD-3-Clause */
#include "journal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(expression)                                                                          \
	do {                                                                                       \
		if (!(expression)) {                                                               \
			fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #expression);           \
			exit(1);                                                                   \
		}                                                                                  \
	} while (0)

#define DEVICE_BLOCKS 16U
#define HELD_BLOCKS 3U
#define POISON_BYTE 0xccU

struct device {
	uint8_t *home;
	size_t size;
	size_t reads;
	size_t read_bytes;
	size_t fail_read;
	size_t live;
	size_t allocations;
};

static enum ext4_result
device_read(void *context, uint64_t offset, void *buffer, size_t length)
{
	struct device *device = context;

	CHECK(offset <= device->size && length <= device->size - offset);
	device->reads++;
	device->read_bytes += length;
	if (device->reads == device->fail_read) {
		return EXT4_IO;
	}
	memcpy(buffer, device->home + offset, length);
	return EXT4_OK;
}

static void *
device_allocate(void *context, size_t size)
{
	struct device *device = context;
	void *buffer = malloc(size);

	CHECK(buffer != NULL);
	device->live++;
	device->allocations++;
	return buffer;
}

static void
device_release(void *context, void *buffer, size_t size)
{
	struct device *device = context;

	(void)size;
	CHECK(device->live != 0 && buffer != NULL);
	device->live--;
	free(buffer);
}

/* Build read-view fixtures with the real snapshot allocator and index. Separate
 * transaction owners avoid fabricating private transaction state; durability and
 * publication transitions are exercised by the deferred/power-cut tests. */
static struct ext4_transaction *
snapshots(struct ext4_journal *owner, const uint64_t *blocks, uint8_t pattern, uint8_t *expected)
{
	struct ext4_transaction *transaction;
	uint32_t block_size = owner->fs->info.block_size;
	void *buffer;
	size_t index;

	CHECK(ext4_transaction_begin(owner, HELD_BLOCKS, &transaction) == EXT4_OK);
	for (index = 0; index < HELD_BLOCKS; index++) {
		CHECK(
		    ext4_transaction_buffer_blank(transaction, blocks[index], &buffer) == EXT4_OK);
		memset(buffer, pattern + index, block_size);
		memcpy(expected + blocks[index] * block_size, buffer, block_size);
	}
	return transaction;
}

static void
check_view(uint32_t block_size)
{
	static const uint64_t committed[] = { 8, 2, 4 };
	static const uint64_t pending[] = { 4, 9, 3 };
	struct device device = { .size = (size_t)DEVICE_BLOCKS * block_size };
	struct ext4_fs fs = { 0 };
	struct ext4_journal view = { .fs = &fs };
	struct ext4_journal committed_owner = { .fs = &fs, .direct = true };
	struct ext4_journal pending_owner = { .fs = &fs, .direct = true };
	uint8_t *expected = malloc(device.size);
	uint8_t *output = malloc(device.size);
	size_t allocations;
	size_t offset;
	size_t length;
	size_t fault;
	size_t index;

	device.home = malloc(device.size);
	CHECK(device.home != NULL && expected != NULL && output != NULL);
	for (index = 0; index < device.size; index++) {
		device.home[index] = (uint8_t)(index * 17U + index / block_size);
	}
	memcpy(expected, device.home, device.size);
	fs.info.block_size = block_size;
	fs.info.blocks = DEVICE_BLOCKS;
	fs.environment.context = &device;
	fs.environment.size_bytes = device.size;
	fs.environment.read = device_read;
	fs.environment.allocate = device_allocate;
	fs.environment.release = device_release;
	fs.journal = &view;
	view.checkpoint = snapshots(&committed_owner, committed, 0x40U, expected);
	view.compound = snapshots(&pending_owner, pending, 0x80U, expected);
	allocations = device.allocations;

	/* Pending bytes win at block 4. Fully held, unaligned reads do no I/O. */
	offset = 2U * block_size + 3U;
	length = 3U * block_size - 7U;
	device.fail_read = 1;
	CHECK(ext4_device_read(&fs, offset, output, length) == EXT4_OK);
	CHECK(device.reads == 0 && memcmp(output, expected + offset, length) == 0);
	device.fail_read = 0;
	/* Three maximal home ranges remain, regardless of snapshot enrollment order. */
	CHECK(ext4_device_read(&fs, 0, output, device.size) == EXT4_OK);
	CHECK(memcmp(output, expected, device.size) == 0);
	CHECK(device.reads == 3U && device.read_bytes == (DEVICE_BLOCKS - 5U) * block_size);
	/* Partial home head/tail, a cached boundary, and a one-byte read. */
	for (offset = block_size - 1U; offset < device.size; offset += block_size) {
		length = device.size - offset < 2U * block_size + 3U ? device.size - offset
								     : 2U * block_size + 3U;
		CHECK(ext4_device_read(&fs, offset, output, length) == EXT4_OK);
		CHECK(memcmp(output, expected + offset, length) == 0);
		CHECK(ext4_device_read(&fs, offset, output, 1) == EXT4_OK);
		CHECK(output[0] == expected[offset]);
	}
	for (fault = 1; fault <= 3U; fault++) {
		device.reads = 0;
		device.fail_read = fault;
		memset(output, POISON_BYTE, device.size);
		CHECK(ext4_device_read(&fs, 0, output, device.size) == EXT4_IO);
		CHECK(device.reads == fault);
		/* No bytes after the failing home range may be delivered. */
		offset = (fault == 1 ? 0U : fault == 2 ? 5U : 10U) * block_size;
		CHECK(memcmp(output, expected, offset) == 0);
		for (index = offset; index < device.size; index++) {
			CHECK(output[index] == POISON_BYTE);
		}
	}
	device.reads = 0;
	device.fail_read = 1;
	CHECK(ext4_device_read(&fs, device.size, NULL, 0) == EXT4_OK);
	CHECK(ext4_device_read(&fs, device.size, output, 1) == EXT4_CORRUPT);
	CHECK(ext4_device_read(&fs, UINT64_MAX, output, 2) == EXT4_CORRUPT);
	CHECK(ext4_device_read(&fs, 1, output, SIZE_MAX) == EXT4_CORRUPT);
	fs.aborted = true;
	CHECK(ext4_device_read(&fs, 2U * block_size, output, 1) == EXT4_RECOVERY_REQUIRED);
	CHECK(device.reads == 0 && device.allocations == allocations);
	fs.aborted = false;
	device.fail_read = 0;
	ext4_transaction_cancel(view.compound);
	view.compound = NULL;
	/* With no pending version, the committed snapshot becomes visible. */
	CHECK(ext4_device_read(&fs, 4U * block_size, output, block_size) == EXT4_OK);
	for (index = 0; index < block_size; index++) {
		CHECK(output[index] == 0x42U);
	}
	ext4_transaction_cancel(view.checkpoint);
	view.checkpoint = NULL;
	device.reads = 0;
	CHECK(ext4_device_read(&fs, 0, output, device.size) == EXT4_OK);
	CHECK(device.reads == 1 && memcmp(output, device.home, device.size) == 0);
	CHECK(device.live == 0);
	free(device.home);
	free(expected);
	free(output);
	printf(
	    "PASS journal live reads: %u-byte blocks, precedence, ranges, I/O faults and bounds\n",
	    block_size);
}

int
main(void)
{
	check_view(1024);
	check_view(4096);
	check_view(65536);
	return 0;
}

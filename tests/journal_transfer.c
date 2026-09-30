/* SPDX-License-Identifier: BSD-3-Clause */
#include "journal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

#define CHECK(expression)                                                                          \
	do {                                                                                       \
		if (!(expression)) {                                                               \
			fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #expression);           \
			exit(1);                                                                   \
		}                                                                                  \
	} while (0)

#define DEVICE_BLOCKS 64U
#define TRANSACTION_CREDITS 6U
#define COMPOUND_BLOCKS 8U
#define METADATA_FIRST 4U
#define METADATA_SECOND 8U
#define METADATA_NEW 9U
#define DATA_FIRST 12U
#define DATA_SECOND 13U
#define HOME_BYTE 0x11U
#define ALLOCATION_BYTE 0xa5U

enum source_case {
	SOURCE_CONTIGUOUS,
	SOURCE_BOUNDED,
	SOURCE_FRAGMENTED,
	SOURCE_LOGGED,
	SOURCE_COMPOUND,
	SOURCE_CHECKPOINT,
	SOURCE_REUSED,
	SOURCE_CASES
};

struct device {
	uint8_t *home;
	size_t size;
	size_t allocations;
	size_t fail_allocation;
	size_t live;
	size_t writes;
	size_t largest_write;
	size_t fail_write;
	enum ext4_result write_error;
};

static void *
device_allocate(void *context, size_t size)
{
	struct device *device = context;
	void *buffer;

	if (++device->allocations == device->fail_allocation) {
		return NULL;
	}
	buffer = malloc(size);
	CHECK(buffer != NULL);
	/* The environment does not promise zero-filled memory. */
	memset(buffer, ALLOCATION_BYTE, size);
	device->live++;
	return buffer;
}

static void
device_release(void *context, void *buffer, size_t size)
{
	struct device *device = context;

	(void)size;
	CHECK(buffer != NULL && device->live != 0);
	device->live--;
	free(buffer);
}

static enum ext4_result
device_read(void *context, uint64_t offset, void *buffer, size_t length)
{
	struct device *device = context;

	CHECK(offset <= device->size && length <= device->size - offset);
	memcpy(buffer, device->home + offset, length);
	return EXT4_OK;
}

static enum ext4_result
device_write(void *context, uint64_t offset, const void *buffer, size_t length)
{
	struct device *device = context;

	CHECK(offset <= device->size && length <= device->size - offset);
	if (length > device->largest_write) {
		device->largest_write = length;
	}
	if (++device->writes == device->fail_write) {
		return device->write_error == EXT4_OK ? EXT4_IO : device->write_error;
	}
	memcpy(device->home + offset, buffer, length);
	return EXT4_OK;
}

static void *
stage(struct ext4_transaction *transaction, uint32_t block, bool data, uint8_t value)
{
	struct ext4_fs *fs = ext4_transaction_fs(transaction);
	void *buffer;

	CHECK((data ? ext4_transaction_data_replace(transaction, block, &buffer)
		    : ext4_transaction_buffer_blank(transaction, block, &buffer)) == EXT4_OK);
	memset(buffer, value, fs->info.block_size);
	return buffer;
}

static void
check_bytes(const void *buffer, uint8_t value, size_t length)
{
	const uint8_t *bytes = buffer;
	size_t index;

	CHECK(buffer != NULL);
	for (index = 0; index < length; index++) {
		CHECK(bytes[index] == value);
	}
}

/* A deferred owner exercises real snapshot enrollment and commit with a synthetic
 * resource. Disk logging, checkpoint barriers and torn writes are covered by the
 * independent-image deferred tests. No private transaction layout is fabricated. */
static void
check_transfer(uint32_t block_size, bool ordered, size_t fail_write)
{
	struct device device = { .size = (size_t)DEVICE_BLOCKS * block_size };
	struct ext4_fs fs = { 0 };
	struct ext4_journal journal = { .fs = &fs,
		.first = 1,
		.last = DEVICE_BLOCKS,
		.compound_blocks = COMPOUND_BLOCKS,
		.ordered_data = ordered };
	struct ext4_transaction *transaction;
	void *first = NULL;
	void *second = NULL;
	void *replacement;
	void *added;
	void *data_first;
	void *data_second;
	uint8_t *output = malloc(block_size);
	size_t allocations;
	size_t live;
	unsigned int attempt;
	enum ext4_result error;

	device.home = malloc(device.size);
	CHECK(device.home != NULL && output != NULL);
	memset(device.home, HOME_BYTE, device.size);
	fs.info.block_size = block_size;
	fs.info.blocks = DEVICE_BLOCKS;
	fs.environment.context = &device;
	fs.environment.size_bytes = device.size;
	fs.environment.read = device_read;
	fs.environment.allocate = device_allocate;
	fs.environment.release = device_release;
	fs.journal = &journal;
	journal.writer.context = &device;
	journal.writer.write = device_write;
	for (attempt = 0; attempt < 2; attempt++) {
		device.fail_allocation = 0;
		CHECK(
		    ext4_transaction_begin(&journal, TRANSACTION_CREDITS, &transaction) == EXT4_OK);
		first = stage(transaction, METADATA_FIRST, false, 0x41U);
		second = stage(transaction, METADATA_SECOND, false, 0x81U);
		allocations = device.allocations;
		/* Only the first compound's index may need memory at this boundary. */
		device.fail_allocation = allocations + (attempt == 0 ? 1U : 2U);
		error = ext4_transaction_commit(transaction);
		CHECK(error == (attempt == 0 ? EXT4_NO_MEMORY : EXT4_OK));
		CHECK(ext4_commit_rejected(&journal, error) == (attempt == 0));
		CHECK(device.allocations == allocations + 1U);
		CHECK(!journal.aborted && !journal.transaction_active && device.writes == 0);
		if (attempt == 0) {
			CHECK(journal.compound == NULL && device.live == 0);
		}
	}
	CHECK(ext4_transaction_peek(journal.compound, METADATA_FIRST) == first);
	CHECK(ext4_transaction_peek(journal.compound, METADATA_SECOND) == second);
	live = device.live;
	device.fail_allocation = 0;
	CHECK(ext4_transaction_begin(&journal, TRANSACTION_CREDITS, &transaction) == EXT4_OK);
	/* Interleave data and metadata so a write failure exercises compaction after
	 * zero or one data buffers have already been released. */
	data_first = stage(transaction, DATA_FIRST, true, 0xc1U);
	replacement = stage(transaction, METADATA_FIRST, false, 0x51U);
	data_second = stage(transaction, DATA_SECOND, true, 0xd1U);
	added = stage(transaction, METADATA_NEW, false, 0x91U);
	allocations = device.allocations;
	device.fail_allocation = allocations + 1U;
	device.fail_write = fail_write;
	error = ext4_transaction_commit(transaction);
	CHECK(error == (fail_write == 0 ? EXT4_OK : EXT4_IO));
	CHECK(device.allocations == allocations && !journal.transaction_active);
	CHECK(journal.aborted == (fail_write != 0));
	CHECK(!ext4_commit_rejected(&journal, error));
	CHECK(device.writes == (ordered ? (fail_write == 0 ? 2U : fail_write) : 0U));
	CHECK(ext4_transaction_peek(journal.compound, METADATA_SECOND) == second);
	check_bytes(second, 0x81U, block_size);
	if (fail_write != 0) {
		CHECK(device.live == live);
		CHECK(ext4_transaction_peek(journal.compound, METADATA_FIRST) == first);
		check_bytes(first, 0x41U, block_size);
		CHECK(ext4_transaction_peek(journal.compound, METADATA_NEW) == NULL);
	} else {
		CHECK(device.live == live + (ordered ? 1U : 3U));
		CHECK(ext4_transaction_peek(journal.compound, METADATA_FIRST) == replacement);
		CHECK(ext4_transaction_peek(journal.compound, METADATA_NEW) == added);
		CHECK(ext4_transaction_peek(journal.compound, DATA_FIRST) ==
		    (ordered ? NULL : data_first));
		CHECK(ext4_transaction_peek(journal.compound, DATA_SECOND) ==
		    (ordered ? NULL : data_second));
		CHECK(ext4_device_read(&fs, METADATA_FIRST * (uint64_t)block_size, output,
			  block_size) == EXT4_OK);
		check_bytes(output, 0x51U, block_size);
		CHECK(ext4_device_read(
			  &fs, DATA_SECOND * (uint64_t)block_size, output, block_size) == EXT4_OK);
		check_bytes(output, 0xd1U, block_size);
		/* Canceling a later private overwrite must preserve transferred bytes. */
		device.fail_allocation = 0;
		live = device.live;
		CHECK(
		    ext4_transaction_begin(&journal, TRANSACTION_CREDITS, &transaction) == EXT4_OK);
		stage(transaction, METADATA_FIRST, false, 0xffU);
		ext4_transaction_cancel(transaction);
		CHECK(device.live == live);
		check_bytes(
		    ext4_transaction_peek(journal.compound, METADATA_FIRST), 0x51U, block_size);
	}
	check_bytes(device.home + METADATA_FIRST * block_size, HOME_BYTE, block_size);
	check_bytes(device.home + DATA_FIRST * block_size,
	    ordered && fail_write != 1U ? 0xc1U : HOME_BYTE, block_size);
	check_bytes(device.home + DATA_SECOND * block_size,
	    ordered && fail_write == 0 ? 0xd1U : HOME_BYTE, block_size);
	ext4_transaction_cancel(journal.compound);
	journal.compound = NULL;
	CHECK(device.live == 0);
	free(output);
	free(device.home);
}

/* Read-only source pages expose accidental writes or releases of caller memory.
 * Ordered data can borrow them through the callback; retained data must detach
 * before returning, including when an older compound/checkpoint owns the block. */
static void
check_source(uint32_t block_size, enum source_case kind, size_t fail_allocation, size_t fail_write,
    enum ext4_result write_error)
{
	struct device device = { .size = (size_t)DEVICE_BLOCKS * block_size };
	struct ext4_fs fs = { 0 };
	struct ext4_block_range reused = { DATA_FIRST, 2 };
	struct ext4_journal journal = { .fs = &fs,
		.first = 1,
		.last = DEVICE_BLOCKS,
		.compound_blocks = COMPOUND_BLOCKS,
		.ordered_data = kind != SOURCE_LOGGED };
	struct ext4_transaction *transaction;
	uint8_t *source;
	void *buffer;
	const void *retained;
	uint32_t second = DATA_SECOND + (kind == SOURCE_FRAGMENTED ? 1U : 0U);
	size_t allocations;
	size_t live;
	bool retain_first = kind >= SOURCE_LOGGED;
	bool retain_second = kind == SOURCE_LOGGED || kind == SOURCE_REUSED;
	enum ext4_result error;

	device.home = malloc(device.size);
	source =
	    mmap(NULL, 2U * block_size, PROT_READ | PROT_WRITE, MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
	CHECK(device.home != NULL && source != MAP_FAILED);
	memset(device.home, HOME_BYTE, device.size);
	memset(source, 0xc1U, block_size);
	memset(source + block_size, 0xd1U, block_size);
	CHECK(mprotect(source, 2U * block_size, PROT_READ) == 0);
	fs.info.block_size = block_size;
	fs.info.blocks = DEVICE_BLOCKS;
	fs.environment = (struct ext4_environment){ &device, device.size, device_read,
		device_allocate, device_release };
	fs.journal = &journal;
	journal.writer.context = &device;
	journal.writer.write = device_write;

	/* Mutable access detaches an immutable view, including replacement requests.
	 * An allocation refusal must not expose the source as writable storage. */
	CHECK(ext4_transaction_begin(&journal, TRANSACTION_CREDITS, &transaction) == EXT4_OK);
	allocations = device.allocations;
	CHECK(ext4_transaction_data_source(transaction, DATA_FIRST, source, block_size - 1U) ==
	    EXT4_INVALID_ARGUMENT);
	CHECK(ext4_transaction_count(transaction) == 0);
	CHECK(ext4_transaction_data_source(transaction, DATA_FIRST, source, 2U * block_size) ==
	    EXT4_OK);
	CHECK(device.allocations == allocations);
	CHECK(ext4_transaction_peek(transaction, DATA_FIRST) == source);
	device.fail_allocation = allocations + 1U;
	buffer = (void *)source;
	CHECK(ext4_transaction_data_replace(transaction, DATA_FIRST, &buffer) == EXT4_NO_MEMORY);
	CHECK(buffer == NULL && ext4_transaction_peek(transaction, DATA_FIRST) == source);
	device.fail_allocation = 0;
	CHECK(ext4_transaction_buffer(transaction, DATA_FIRST, &buffer) == EXT4_OK);
	CHECK(buffer != source);
	check_bytes(buffer, 0xc1U, block_size);
	memset(buffer, 0x71U, block_size);
	check_bytes(source, 0xc1U, block_size);
	CHECK(ext4_transaction_data_source(
		  transaction, DATA_FIRST, source + block_size, block_size) == EXT4_OK);
	CHECK(ext4_transaction_peek(transaction, DATA_FIRST) == buffer);
	check_bytes(buffer, 0xd1U, block_size);
	ext4_transaction_cancel(transaction);
	CHECK(device.live == 0 && device.writes == 0);

	if (kind == SOURCE_COMPOUND || kind == SOURCE_CHECKPOINT) {
		CHECK(
		    ext4_transaction_begin(&journal, TRANSACTION_CREDITS, &transaction) == EXT4_OK);
		stage(transaction, DATA_FIRST, false, 0x51U);
		CHECK(ext4_transaction_commit(transaction) == EXT4_OK);
		if (kind == SOURCE_CHECKPOINT) {
			journal.checkpoint = journal.compound;
			journal.compound = NULL;
		}
	}
	if (kind == SOURCE_REUSED) {
		journal.freed = &reused;
		journal.freed_count = 1;
	}
	live = device.live;
	CHECK(ext4_transaction_begin(&journal, TRANSACTION_CREDITS, &transaction) == EXT4_OK);
	stage(transaction, METADATA_FIRST, false, 0x41U);
	allocations = device.allocations;
	CHECK(ext4_transaction_data_source(transaction, DATA_FIRST, source,
		  kind == SOURCE_BOUNDED ? block_size : 2U * block_size) == EXT4_OK);
	CHECK(ext4_transaction_data_source(transaction, second, source + block_size, block_size) ==
	    EXT4_OK);
	CHECK(device.allocations == allocations);
	device.fail_allocation = fail_allocation == 0 ? 0 : allocations + fail_allocation;
	device.fail_write = fail_write;
	device.write_error = write_error;
	error = ext4_transaction_commit(transaction);
	CHECK(error ==
	    (fail_allocation != 0 ? EXT4_NO_MEMORY : (fail_write != 0 ? write_error : EXT4_OK)));
	CHECK(!journal.transaction_active && journal.aborted == (fail_write != 0));
	CHECK(ext4_commit_rejected(&journal, error) == (fail_allocation != 0));
	check_bytes(source, 0xc1U, block_size);
	check_bytes(source + block_size, 0xd1U, block_size);
	CHECK(mprotect(source, 2U * block_size, PROT_READ | PROT_WRITE) == 0);
	memset(source, 0xffU, 2U * block_size);
	if (error == EXT4_OK) {
		retained = ext4_transaction_peek(journal.compound, DATA_FIRST);
		CHECK((retained != NULL) == retain_first && retained != source);
		check_bytes(retain_first ? retained : device.home + DATA_FIRST * block_size, 0xc1U,
		    block_size);
		retained = ext4_transaction_peek(journal.compound, second);
		CHECK((retained != NULL) == retain_second && retained != source + block_size);
		check_bytes(retain_second ? retained : device.home + second * block_size, 0xd1U,
		    block_size);
		CHECK(device.writes ==
		    (retain_second ? 0U : (retain_first || kind == SOURCE_CONTIGUOUS ? 1U : 2U)));
		CHECK(device.largest_write ==
		    (kind == SOURCE_CONTIGUOUS ? 2U * block_size
					       : (retain_second ? 0U : block_size)));
	} else if (fail_allocation != 0) {
		CHECK(device.writes == 0 && device.live == live);
		check_bytes(device.home, HOME_BYTE, device.size);
	}
	check_bytes(device.home + METADATA_FIRST * block_size, HOME_BYTE, block_size);
	if (journal.checkpoint != NULL) {
		check_bytes(
		    ext4_transaction_peek(journal.checkpoint, DATA_FIRST), 0x51U, block_size);
	}
	ext4_transaction_cancel(journal.compound);
	ext4_transaction_cancel(journal.checkpoint);
	CHECK(device.live == 0);
	CHECK(munmap(source, 2U * block_size) == 0);
	free(device.home);
}

int
main(void)
{
	static const uint32_t block_sizes[] = { EXT4_MIN_BLOCK_SIZE, 4096U, EXT4_MAX_BLOCK_SIZE };
	size_t index;
	size_t fail_write;
	enum source_case kind;
	size_t fail_allocation;

	for (index = 0; index < sizeof(block_sizes) / sizeof(block_sizes[0]); index++) {
		check_transfer(block_sizes[index], false, 0);
		for (fail_write = 0; fail_write <= 2; fail_write++) {
			check_transfer(block_sizes[index], true, fail_write);
		}
		for (kind = SOURCE_CONTIGUOUS; kind < SOURCE_CASES; kind++) {
			check_source(block_sizes[index], kind, 0, 0, EXT4_OK);
		}
		check_source(block_sizes[index], SOURCE_CONTIGUOUS, 1, 0, EXT4_OK);
		check_source(block_sizes[index], SOURCE_CONTIGUOUS, 0, 1, EXT4_IO);
		check_source(block_sizes[index], SOURCE_FRAGMENTED, 0, 2, EXT4_IO);
		/* Callback statuses cannot masquerade as pre-I/O admission refusals. */
		check_source(block_sizes[index], SOURCE_FRAGMENTED, 0, 2, EXT4_NO_MEMORY);
		check_source(block_sizes[index], SOURCE_CONTIGUOUS, 0, 1, EXT4_QUOTA_EXCEEDED);
		for (fail_allocation = 1; fail_allocation <= 3; fail_allocation++) {
			check_source(
			    block_sizes[index], SOURCE_LOGGED, fail_allocation, 0, EXT4_OK);
		}
	}
	puts("PASS journal snapshot ownership, allocation refusal and ordered write failures");
	return 0;
}

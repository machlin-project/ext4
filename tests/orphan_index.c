/* SPDX-License-Identifier: BSD-3-Clause */
#include "allocate.h"
#include "transaction.h"

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

struct device {
	uint8_t *home;
	size_t size;
	size_t allocations;
	size_t fail_allocation;
	size_t live;
	size_t reads;
	size_t writes;
	size_t flushes;
	size_t fail_flush;
	uint8_t first_log_byte;
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
	device->reads++;
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
	/* The policy model places the first metadata payload after the descriptor. */
	if (device->first_log_byte == 0 && offset == 34U * (device->size / DEVICE_BLOCKS)) {
		device->first_log_byte = *(const uint8_t *)buffer;
	}
	memcpy(device->home + offset, buffer, length);
	return EXT4_OK;
}

static enum ext4_result
device_flush(void *context)
{
	struct device *device = context;

	return ++device->flushes == device->fail_flush ? EXT4_IO : EXT4_OK;
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

enum orphan_fault {
	ORPHAN_NO_FAULT,
	ORPHAN_INDEX_ALLOCATION,
	ORPHAN_DELTA_ALLOCATION,
	ORPHAN_COMPOUND_ALLOCATION,
	ORPHAN_SYNC_COMMIT,
	ORPHAN_CAPACITY_COMMIT
};

/* Admission/ownership model only: these are checksummed orphan blocks and real
 * transaction snapshots, not an independently authored filesystem image. */
static void
seal_orphan(struct ext4_fs *fs, uint64_t block, void *buffer)
{
	struct ext4_orphan_tail_disk *tail = (struct ext4_orphan_tail_disk *)((uint8_t *)buffer +
	    fs->info.block_size - sizeof(*tail));
	struct ext4_block_number_disk address;
	uint32_t checksum;

	ext4_encode32(&tail->magic, EXT4_ORPHAN_MAGIC);
	ext4_encode32(&address.low, (uint32_t)block);
	ext4_encode32(&address.high, (uint32_t)(block >> 32));
	checksum = ext4_crc32c(ext4_inode_seed(fs, &fs->orphan_file->inode), &address,
	    sizeof(address));
	ext4_encode32(&tail->checksum,
	    ext4_crc32c(checksum, buffer, fs->info.block_size - sizeof(*tail)));
}

static void
check_index(uint32_t block_size, bool checksum, enum orphan_fault fault)
{
	struct device device = { .size = (size_t)DEVICE_BLOCKS * block_size };
	struct ext4_fs fs = { 0 };
	struct ext4_journal journal = { .fs = &fs, .first = 1, .last = DEVICE_BLOCKS,
		.compound_blocks = COMPOUND_BLOCKS };
	uint64_t blocks[2] = { 20, 21 };
	struct ext4_orphan_block states[2] = { 0 };
	struct ext4_orphan_file file = { .blocks = blocks, .block_count = 2, .state = states };
	struct ext4_inode inode = { .number = 11, .generation = 1, .mode = EXT4_MODE_REGULAR };
	struct ext4_inode_disk *disk;
	struct ext4_super_disk *super;
	struct ext4_transaction *transaction;
	struct ext4_allocation allocation;
	void *buffer;
	uint32_t slots = (block_size - sizeof(struct ext4_orphan_tail_disk)) / 4U;
	uint32_t logical;
	uint32_t slot;
	struct ext4_le32 *entries;
	size_t reads;
	bool removed;
	bool allocation_fault =
	    fault >= ORPHAN_INDEX_ALLOCATION && fault <= ORPHAN_COMPOUND_ALLOCATION;
	enum ext4_result error;

	device.home = calloc(1, device.size);
	CHECK(device.home != NULL);
	fs.info.block_size = block_size;
	fs.info.blocks = DEVICE_BLOCKS;
	fs.info.inodes = 65536;
	fs.first_inode = EXT4_FIRST_NON_RESERVED_INODE;
	fs.inodes_per_group = 4096;
	fs.journal_inode = 8;
	fs.orphan_file_inode = 9;
	fs.metadata_checksum = checksum;
	fs.environment = (struct ext4_environment){ &device, device.size, device_read,
		device_allocate, device_release };
	fs.journal = &journal;
	fs.orphan_file = &file;
	file.inode.number = fs.orphan_file_inode;
	file.inode.generation = 1;
	states[0].free = states[1].free = slots;
	if (fault == ORPHAN_CAPACITY_COMMIT) {
		journal.compound_blocks = 3;
	}
	journal.writer.context = &device;
	journal.writer.write = device_write;
	journal.writer.flush = device_flush;
	super = (struct ext4_super_disk *)(device.home + EXT4_SUPER_OFFSET);
	ext4_encode16(&super->magic, EXT4_SUPER_MAGIC);
	ext4_encode16(&super->state, EXT4_VALID_FS);
	if (checksum) {
		ext4_encode32(&super->checksum,
		    ext4_crc32c(UINT32_MAX, super, offsetof(struct ext4_super_disk, checksum)));
	}
	seal_orphan(&fs, blocks[0], device.home + blocks[0] * block_size);
	seal_orphan(&fs, blocks[1], device.home + blocks[1] * block_size);
	CHECK(ext4_transaction_begin(&journal, TRANSACTION_CREDITS, &transaction) == EXT4_OK);
	disk = stage(transaction, METADATA_FIRST, false, 0);
	CHECK(ext4_allocation_init(&allocation, &fs, transaction, &inode) == EXT4_OK);
	CHECK(ext4_orphan_link(&allocation, inode.number, disk) == EXT4_OK);
	CHECK(ext4_le32(&disk->deletion_time) == 0 && file.pending == 0);
	ext4_allocation_destroy(&allocation);
	reads = device.reads;
	device.fail_allocation = allocation_fault ? device.allocations + fault : 0;
	error = ext4_transaction_commit(transaction);
	CHECK(error == (allocation_fault ? EXT4_NO_MEMORY : EXT4_OK));
	CHECK(device.writes == 0 && device.flushes == 0 && device.reads == reads);
	CHECK(fs.last_orphan == 0 && file.pending == (allocation_fault ? 0U : 1U));
	CHECK(states[0].free == slots - file.pending);
	if (allocation_fault) {
		CHECK(journal.compound == NULL && !journal.aborted);
		goto out;
	}
	CHECK(file.slots[0].number == inode.number && file.slots[0].logical == 0 &&
	    file.slots[0].slot == 0);
	if (fault >= ORPHAN_SYNC_COMMIT) {
		/* Force a prior compound through either SYNC or capacity pressure. Its
		 * first write fails after current deltas prepare but before publication. */
		journal.data = calloc(1, block_size);
		CHECK(journal.data != NULL);
		CHECK(
		    ext4_transaction_begin(&journal, TRANSACTION_CREDITS, &transaction) == EXT4_OK);
		disk = stage(transaction, METADATA_FIRST, false, 0);
		CHECK(ext4_allocation_init(&allocation, &fs, transaction, &inode) == EXT4_OK);
		CHECK(ext4_orphan_link(&allocation, 12, disk) == EXT4_OK);
		if (fault == ORPHAN_SYNC_COMMIT) {
			inode.flags = EXT4_INODE_SYNC;
			ext4_transaction_inode_policy(transaction, &inode);
		} else {
			stage(transaction, METADATA_SECOND, false, 0);
		}
		ext4_allocation_destroy(&allocation);
		device.fail_write = 1;
		CHECK(ext4_transaction_commit(transaction) == EXT4_IO);
		CHECK(journal.aborted && journal.compound == NULL && device.writes == 1);
		CHECK(file.pending == 1 && file.slots[0].number == 11 &&
		    states[0].free == slots - 1U);
		goto out;
	}
	/* A private cancellation cannot change membership or reserve a live slot. */
	CHECK(ext4_transaction_begin(&journal, TRANSACTION_CREDITS, &transaction) == EXT4_OK);
	disk = stage(transaction, METADATA_FIRST, false, 0);
	CHECK(ext4_allocation_init(&allocation, &fs, transaction, &inode) == EXT4_OK);
	CHECK(ext4_orphan_link(&allocation, 12, disk) == EXT4_OK);
	ext4_allocation_destroy(&allocation);
	ext4_transaction_cancel(transaction);
	CHECK(file.pending == 1 && states[0].free == slots - 1U);
	/* Indexed removal reads only the retained slot block; repeated removal in a
	 * fast-commit conversion is idempotent before index publication. */
	CHECK(ext4_transaction_begin(&journal, TRANSACTION_CREDITS, &transaction) == EXT4_OK);
	CHECK(ext4_allocation_init(&allocation, &fs, transaction, &inode) == EXT4_OK);
	reads = device.reads;
	CHECK(ext4_orphan_file_remove(&allocation, inode.number, &removed) == EXT4_OK && removed);
	CHECK(ext4_orphan_file_remove(&allocation, inode.number, &removed) == EXT4_OK && !removed);
	CHECK(device.reads == reads && file.pending == 1);
	ext4_allocation_destroy(&allocation);
	CHECK(ext4_transaction_commit(transaction) == EXT4_OK);
	CHECK(file.pending == 0 && states[0].free == slots);
	CHECK(ext4_transaction_begin(&journal, TRANSACTION_CREDITS, &transaction) == EXT4_OK);
	disk = stage(transaction, METADATA_FIRST, false, 0);
	CHECK(ext4_allocation_init(&allocation, &fs, transaction, &inode) == EXT4_OK);
	CHECK(ext4_orphan_link(&allocation, 12, disk) == EXT4_OK);
	ext4_allocation_destroy(&allocation);
	CHECK(ext4_transaction_commit(transaction) == EXT4_OK);
	CHECK(file.pending == 1 && file.slots[0].number == 12 && file.slots[0].slot == 0);
	/* Checksum/magic refusal cannot silently select the legacy fallback. */
	CHECK(ext4_transaction_begin(&journal, TRANSACTION_CREDITS, &transaction) == EXT4_OK);
	disk = stage(transaction, METADATA_FIRST, false, 0);
	CHECK(ext4_allocation_init(&allocation, &fs, transaction, &inode) == EXT4_OK);
	CHECK(ext4_transaction_buffer(transaction, blocks[0], &buffer) == EXT4_OK);
	((uint8_t *)buffer)[block_size - sizeof(struct ext4_orphan_tail_disk)] ^= 1U;
	CHECK(ext4_orphan_link(&allocation, 13, disk) == EXT4_CORRUPT);
	ext4_allocation_destroy(&allocation);
	ext4_transaction_cancel(transaction);
	CHECK(file.pending == 1 && fs.last_orphan == 0 && !journal.aborted);
	/* Fill both modeled blocks in a private transaction. Full capacity must use
	 * the legacy chain, while index publication handles a bounded bulk delta. */
	CHECK(ext4_transaction_begin(&journal, TRANSACTION_CREDITS, &transaction) == EXT4_OK);
	for (logical = 0; logical < 2; logical++) {
		CHECK(ext4_transaction_buffer(transaction, blocks[logical], &buffer) == EXT4_OK);
		entries = buffer;
		for (slot = 0; slot < slots; slot++) {
			ext4_encode32(&entries[slot], 32U + logical * slots + slot);
		}
		seal_orphan(&fs, blocks[logical], buffer);
	}
	transaction->orphan_touched = true;
	CHECK(ext4_transaction_commit(transaction) == EXT4_OK);
	CHECK(file.pending == slots * 2U && states[0].free == 0 && states[1].free == 0);
	CHECK(ext4_transaction_begin(&journal, TRANSACTION_CREDITS, &transaction) == EXT4_OK);
	disk = stage(transaction, METADATA_FIRST, false, 0);
	CHECK(ext4_allocation_init(&allocation, &fs, transaction, &inode) == EXT4_OK);
	CHECK(ext4_orphan_link(&allocation, 13, disk) == EXT4_OK);
	ext4_allocation_destroy(&allocation);
	CHECK(ext4_transaction_commit(transaction) == EXT4_OK);
	CHECK(fs.last_orphan == 13 && file.pending == slots * 2U);
out:
	check_bytes(device.home + METADATA_FIRST * block_size, 0, block_size);
	CHECK(device.writes == (fault >= ORPHAN_SYNC_COMMIT ? 1U : 0U) && device.flushes == 0);
	ext4_transaction_cancel(journal.compound);
	if (file.slots != NULL) {
		device_release(&device, file.slots, file.slot_capacity * sizeof(*file.slots));
	}
	CHECK(device.live == 0);
	free(journal.data);
	free(device.home);
}

int
main(void)
{
	static const uint32_t sizes[] = { 1024, 4096, 65536 };
	size_t index;
	enum orphan_fault fault;
	unsigned int checksum;

	for (index = 0; index < sizeof(sizes) / sizeof(sizes[0]); index++) {
		for (checksum = 0; checksum < 2; checksum++) {
			for (fault = ORPHAN_NO_FAULT; fault <= ORPHAN_CAPACITY_COMMIT; fault++) {
				check_index(sizes[index], checksum != 0, fault);
			}
		}
	}
	puts("PASS live orphan slot publication, cancellation, index allocation refusal and reuse");
	return 0;
}

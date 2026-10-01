/* SPDX-License-Identifier: BSD-3-Clause */
#include "storage.h"

#define REQUEST_BYTES (1024U * 1024U + 37U)
#define REQUEST_OFFSET 17U
#define REQUEST_SHORTAGE_BLOCKS 20U
#define REQUEST_FREE_BLOCKS 5U

static struct ext4_inode_update
update(void)
{
	struct ext4_inode_update result = { 0 };

	result.fields = EXT4_ATTR_PERMISSIONS | EXT4_ATTR_MODIFY_TIME | EXT4_ATTR_CHANGE_TIME;
	result.permissions = 0640;
	result.modify_time.seconds = 1700000090;
	result.change_time = result.modify_time;
	return result;
}

static struct ext4_fs *
open_file(struct device *device, struct ext4_inode *inode)
{
	struct ext4_fs *fs;
	struct ext4_inode root;

	EXPECT(ext4_mount_writable(&device->environment, &device->writer, &fs), EXT4_OK);
	EXPECT(ext4_get_inode(fs, EXT4_ROOT_INODE, &root), EXT4_OK);
	EXPECT(ext4_lookup(fs, &root, (const uint8_t *)"empty", 5, inode), EXT4_OK);
	return fs;
}

static void
check_contents(struct ext4_fs *fs, uint32_t number, const uint8_t *data)
{
	struct ext4_inode inode;
	uint8_t *observed = malloc(REQUEST_OFFSET + REQUEST_BYTES + 1U);
	size_t completed;
	size_t index;

	CHECK(observed != NULL);
	EXPECT(ext4_get_inode(fs, number, &inode), EXT4_OK);
	CHECK(inode.size == REQUEST_OFFSET + REQUEST_BYTES);
	CHECK((inode.mode & EXT4_MODE_PERMISSIONS) == 0640);
	memset(observed, 0xa5, REQUEST_OFFSET + REQUEST_BYTES + 1U);
	EXPECT(ext4_read(fs, &inode, 0, observed, (size_t)inode.size + 1U, &completed), EXT4_OK);
	CHECK(completed == inode.size && observed[completed] == 0xa5);
	for (index = 0; index < REQUEST_OFFSET; index++) {
		CHECK(observed[index] == 0);
	}
	CHECK(memcmp(observed + REQUEST_OFFSET, data, REQUEST_BYTES) == 0);
	free(observed);
}

static void
guards(struct device *device)
{
	struct ext4_inode inode;
	struct ext4_fs *fs = open_file(device, &inode);
	struct ext4_inode_update attributes = update();
	size_t maximum = (size_t)ext4_journal_request_credits(fs->journal) * device->block_size;
	uint8_t *data = calloc(maximum, 1);
	size_t completed;

	CHECK(data != NULL);
	EXPECT(ext4_write_request(
		   NULL, inode.number, inode.generation, 0, data, 1, &attributes, &completed),
	    EXT4_INVALID_ARGUMENT);
	CHECK(completed == 0);
	EXPECT(
	    ext4_write_request(fs, inode.number, inode.generation, 0, data, 1, &attributes, NULL),
	    EXT4_INVALID_ARGUMENT);
	EXPECT(ext4_write_request(
		   fs, inode.number, inode.generation, 0, NULL, 1, &attributes, &completed),
	    EXT4_INVALID_ARGUMENT);
	EXPECT(ext4_write_request(fs, inode.number, inode.generation, UINT64_MAX, data, 2,
		   &attributes, &completed),
	    EXT4_RANGE);
	EXPECT(ext4_write_request(
		   fs, inode.number, inode.generation, 0, data, maximum, &attributes, &completed),
	    EXT4_RANGE);
	EXPECT(ext4_write_request(
		   fs, inode.number, inode.generation + 1U, 0, data, 1, &attributes, &completed),
	    EXT4_STALE);
	EXPECT(ext4_write_request(
		   fs, inode.number, inode.generation, 0, NULL, 0, &attributes, &completed),
	    EXT4_OK);
	attributes.modify_time.nanoseconds = EXT4_NANOSECONDS_PER_SECOND;
	EXPECT(ext4_write_request(
		   fs, inode.number, inode.generation, 0, data, 1, &attributes, &completed),
	    EXT4_RANGE);
	CHECK(completed == 0 && device->writes == 0);
	CHECK(memcmp(device->cache, device->base, device->size) == 0);
	ext4_unmount(fs);
	free(data);
	puts("PASS complete write admission guards, including oversized journal request");
}

static void
operations(struct device *device, const char *exports, const char *path)
{
	struct ext4_inode inode;
	struct ext4_fs *fs = open_file(device, &inode);
	struct ext4_inode_update attributes = update();
	uint8_t *data = malloc(REQUEST_BYTES);
	size_t completed;
	size_t index;

	CHECK(data != NULL);
	for (index = 0; index < REQUEST_BYTES; index++) {
		data[index] = (uint8_t)(index * 29U + index / 251U);
	}
	EXPECT(ext4_write_request(fs, inode.number, inode.generation, REQUEST_OFFSET, data,
		   REQUEST_BYTES, &attributes, &completed),
	    EXT4_OK);
	CHECK(completed == REQUEST_BYTES);
	check_contents(fs, inode.number, data);
	EXPECT(ext4_sync(fs), EXT4_OK);
	storage_export(device, exports, path, "request-growth-");
	data[0] ^= 0xff;
	data[REQUEST_BYTES - 1U] ^= 0xff;
	EXPECT(ext4_write_request(fs, inode.number, inode.generation, REQUEST_OFFSET, data,
		   REQUEST_BYTES, &attributes, &completed),
	    EXT4_OK);
	CHECK(completed == REQUEST_BYTES);
	check_contents(fs, inode.number, data);
	EXPECT(ext4_sync(fs), EXT4_OK);
	storage_export(device, exports, path, "request-overwrite-");
	ext4_unmount(fs);
	fs = open_file(device, &inode);
	check_contents(fs, inode.number, data);
	ext4_unmount(fs);
	free(data);
	puts("PASS complete write requests: large unaligned growth, overwrite and remount");
}

static void
shortage(struct device *device, bool overwrite)
{
	struct ext4_inode inode;
	struct ext4_inode after;
	struct ext4_fs *fs = open_file(device, &inode);
	struct ext4_super_disk *super =
	    (struct ext4_super_disk *)(device->cache + EXT4_SUPER_OFFSET);
	struct ext4_inode_update attributes = update();
	size_t length = REQUEST_SHORTAGE_BLOCKS * device->block_size;
	uint8_t *data = malloc(length);
	uint8_t *before = malloc(device->size);
	uint64_t free_blocks;
	uint64_t reserved;
	uint32_t writes;
	size_t completed;

	CHECK(data != NULL && before != NULL);
	memset(data, 0x6d, length);
	if (overwrite) {
		EXPECT(ext4_write_request(fs, inode.number, inode.generation, 0, data,
			   device->block_size, &attributes, &completed),
		    EXT4_OK);
		CHECK(completed == device->block_size);
		EXPECT(ext4_sync(fs), EXT4_OK);
		EXPECT(ext4_get_inode(fs, inode.number, &inode), EXT4_OK);
		memset(data, 0xa5, length);
	}
	free_blocks = fs->info.free_blocks;
	reserved = free_blocks - REQUEST_FREE_BLOCKS;
	ext4_encode32(&super->reserved_blocks_lo, (uint32_t)reserved);
	ext4_encode32(&super->reserved_blocks_hi, (uint32_t)(reserved >> 32));
	if (fs->metadata_checksum) {
		ext4_encode32(&super->checksum,
		    ext4_crc32c(UINT32_MAX, super, offsetof(struct ext4_super_disk, checksum)));
	}
	memcpy(device->stable, device->cache, device->size);
	memcpy(before, device->cache, device->size);
	writes = device->writes;
	EXPECT(ext4_write_request(
		   fs, inode.number, inode.generation, 0, data, length, &attributes, &completed),
	    EXT4_NO_SPACE);
	CHECK(completed == 0 && !fs->aborted && device->writes == writes);
	CHECK(fs->info.free_blocks == free_blocks);
	CHECK(memcmp(before, device->cache, device->size) == 0);
	EXPECT(ext4_get_inode(fs, inode.number, &after), EXT4_OK);
	CHECK(after.size == inode.size && after.blocks_512 == inode.blocks_512);
	/* A smaller request can still use the remaining free blocks, and an
	 * overwrite must remain available when the allocator refused growth. */
	EXPECT(ext4_write_request(fs, inode.number, inode.generation, 0, data, device->block_size,
		   &attributes, &completed),
	    EXT4_OK);
	CHECK(completed == device->block_size);
	EXPECT(ext4_sync(fs), EXT4_OK);
	ext4_unmount(fs);
	free(before);
	free(data);
	puts("PASS complete write ENOSPC: zero device writes, unchanged data/size/accounting, "
	     "retry");
}

static void
faults(struct device *device)
{
	struct ext4_inode inode;
	struct ext4_fs *fs = open_file(device, &inode);
	struct ext4_inode_update attributes = update();
	uint8_t *data = malloc(REQUEST_BYTES);
	uint8_t *expected = malloc(device->size);
	uint32_t positions[8];
	uint32_t events;
	uint32_t reads;
	uint32_t allocations;
	uint32_t position;
	uint32_t phase;
	uint32_t count;
	uint32_t index;
	uint32_t survival;
	size_t completed;
	enum ext4_result error;

	CHECK(data != NULL && expected != NULL);
	memset(data, 0x6d, REQUEST_BYTES);
	device->reads = device->allocations = 0;
	EXPECT(ext4_write_request(fs, inode.number, inode.generation, REQUEST_OFFSET, data,
		   REQUEST_BYTES, &attributes, &completed),
	    EXT4_OK);
	CHECK(completed == REQUEST_BYTES);
	reads = device->reads;
	allocations = device->allocations;
	events = device->events;
	CHECK(device->commit_barrier > 1 && device->commit_barrier < events);
	positions[0] = 1;
	positions[1] = 2;
	positions[2] = events / 2U;
	positions[3] = device->commit_barrier - 1U;
	positions[4] = device->commit_barrier;
	positions[5] = device->commit_barrier + 1U;
	positions[6] = events - 1U;
	positions[7] = events;
	EXPECT(ext4_sync(fs), EXT4_OK);
	memcpy(expected, device->stable, device->size);
	ext4_unmount(fs);
	for (phase = 0; phase < 2; phase++) {
		count = phase == 0 ? allocations : reads;
		for (index = 0; index < 3; index++) {
			position = index == 0 ? 1U : index == 1 ? count / 2U : count;
			device_reset(device, device->base);
			fs = open_file(device, &inode);
			device->reads = device->allocations = 0;
			device->fail_allocation = phase == 0 ? position : 0;
			device->fail_read = phase == 1 ? position : 0;
			error = ext4_write_request(fs, inode.number, inode.generation,
			    REQUEST_OFFSET, data, REQUEST_BYTES, &attributes, &completed);
			EXPECT(error, phase == 0 ? EXT4_NO_MEMORY : EXT4_IO);
			CHECK(completed == 0 && device->writes == 0);
			CHECK(memcmp(device->cache, device->base, device->size) == 0);
			ext4_unmount(fs);
		}
	}
	for (index = 0; index < sizeof(positions) / sizeof(positions[0]); index++) {
		for (survival = 0; survival < 3; survival++) {
			device_reset(device, device->base);
			fs = open_file(device, &inode);
			device->stop_at = positions[index];
			device->partial = true;
			device->survival = survival;
			EXPECT(ext4_write_request(fs, inode.number, inode.generation,
				   REQUEST_OFFSET, data, REQUEST_BYTES, &attributes, &completed),
			    EXT4_IO);
			CHECK(completed == 0 && fs->aborted && device->off);
			ext4_unmount(fs);
			/* Recovery must select the old or complete new operation, never an
			 * earlier transaction prefix. Torn superblocks are explicit corruption. */
			(void)storage_recover(device, expected, device->intent_durable);
		}
	}
	free(expected);
	free(data);
	puts("PASS complete write request: sampled resource/device faults and atomic recovery");
}

int
main(int argc, char **argv)
{
	struct device device;
	const char *exports = NULL;
	bool fault_mode = false;
	int index = 1;

	if (index < argc && strcmp(argv[index], "--faults") == 0) {
		fault_mode = true;
		index++;
	} else if (index < argc && strcmp(argv[index], "--export") == 0) {
		CHECK(index + 2 < argc);
		exports = argv[++index];
		index++;
	}
	CHECK(index < argc);
	for (; index < argc; index++) {
		storage_open(&device, argv[index]);
		if (fault_mode) {
			faults(&device);
		} else {
			guards(&device);
			device_reset(&device, device.base);
			operations(&device, exports, argv[index]);
			device_reset(&device, device.base);
			shortage(&device, false);
			device_reset(&device, device.base);
			shortage(&device, true);
		}
		storage_close(&device);
		printf("PASS complete write request: %s\n", argv[index]);
	}
	return 0;
}

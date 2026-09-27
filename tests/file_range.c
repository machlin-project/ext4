/* SPDX-License-Identifier: BSD-3-Clause */
#include "storage.h"

#define RANGE_BLOCKS 269U
#define RANGE_SECONDS 1700000160
#define RANGE_SEED_BLOCKS 21U
#define RANGE_TAIL 73U

static const uint8_t attribute_value[300] = { 7, 0xff, 0, 0x80, 31 };

static struct ext4_inode_update
attributes(const struct ext4_xattr_change *change)
{
	struct ext4_inode_update update = { 0 };

	update.fields = EXT4_ATTR_PERMISSIONS | EXT4_ATTR_MODIFY_TIME | EXT4_ATTR_CHANGE_TIME |
	    EXT4_ATTR_XATTRS;
	update.permissions = 0640;
	update.modify_time.seconds = RANGE_SECONDS;
	update.change_time.seconds = RANGE_SECONDS + 1;
	update.xattrs = change;
	update.xattr_count = change == NULL ? 0 : 1;
	return update;
}

static struct ext4_xattr_change
attribute(enum ext4_xattr_policy policy)
{
	struct ext4_xattr_change change = { 0 };

	change.policy = policy;
	change.name_index = EXT4_XATTR_USER;
	change.name = (const uint8_t *)"range";
	change.name_length = 5;
	if (policy != EXT4_XATTR_REMOVE) {
		change.value = attribute_value;
		change.value_size = sizeof(attribute_value);
	}
	return change;
}

static struct ext4_fs *
mount_file(struct device *device, struct ext4_inode *inode)
{
	struct ext4_fs *fs;
	struct ext4_inode root;

	EXPECT(ext4_mount_writable(&device->environment, &device->writer, &fs), EXT4_OK);
	EXPECT(ext4_get_inode(fs, EXT4_ROOT_INODE, &root), EXT4_OK);
	EXPECT(ext4_lookup(fs, &root, (const uint8_t *)"empty", 5, inode), EXT4_OK);
	return fs;
}

static void
contents(struct ext4_fs *fs, struct ext4_inode *inode, const uint8_t *expected, size_t length)
{
	uint8_t *observed = malloc(length + 1U);
	size_t count;

	CHECK(observed != NULL);
	EXPECT(ext4_get_inode(fs, inode->number, inode), EXT4_OK);
	CHECK(inode->size == length);
	memset(observed, 0xa5, length + 1U);
	EXPECT(ext4_read(fs, inode, 0, observed, length + 1U, &count), EXT4_OK);
	CHECK(
	    count == length && observed[length] == 0xa5 && memcmp(observed, expected, length) == 0);
	free(observed);
}

static uint8_t *
seed_file(struct device *device, struct ext4_fs *fs, const struct ext4_inode *inode, size_t length)
{
	struct ext4_inode_update update = attributes(NULL);
	uint8_t *bytes = malloc(length);
	size_t index;
	size_t completed;

	(void)device;
	CHECK(bytes != NULL);
	for (index = 0; index < length; index++) {
		bytes[index] = (uint8_t)(index * 13U + index / 257U + 11U);
	}
	update.modify_time.seconds -= 10;
	update.change_time.seconds -= 10;
	EXPECT(ext4_write_partial(
		   fs, inode->number, inode->generation, 0, bytes, length, &update, &completed),
	    EXT4_OK);
	CHECK(completed == length);
	return bytes;
}

static void
operations(struct device *device, const char *exports, const char *path)
{
	struct ext4_inode inode;
	struct ext4_inode root;
	struct ext4_inode_hold *hold;
	struct ext4_fs *fs = mount_file(device, &inode);
	struct ext4_xattr_change change = attribute(EXT4_XATTR_CREATE);
	struct ext4_inode_update update = attributes(&change);
	size_t seed_size = RANGE_SEED_BLOCKS * device->block_size + RANGE_TAIL;
	size_t offset = device->block_size + 17U;
	size_t length = RANGE_BLOCKS * device->block_size + RANGE_TAIL;
	size_t end = offset + length;
	uint8_t *seed = seed_file(device, fs, &inode, seed_size);
	uint8_t *expected = calloc(end, 1);
	uint8_t *before = malloc(device->size);
	uint8_t value[sizeof(attribute_value)];
	uint64_t completed;
	uint64_t blocks;
	uint64_t free_blocks;
	uint32_t writes;
	size_t count;
	size_t index;

	CHECK(expected != NULL && before != NULL);
	memcpy(expected, seed, seed_size);
	free(seed);
	if (inode.flags & EXT4_INODE_EXTENTS) {
		EXPECT(ext4_fallocate(fs, inode.number, inode.generation, offset, length,
			   EXT4_FALLOC_KEEP_SIZE, &update, &completed),
		    EXT4_OK);
		CHECK(completed == length);
		contents(fs, &inode, expected, seed_size);
		CHECK(inode.blocks_512 >= ((end + device->block_size - 1U) / device->block_size) *
			(device->block_size / EXT4_SECTOR_SIZE));
		EXPECT(ext4_get_xattr(fs, inode.number, inode.generation, EXT4_XATTR_USER,
			   change.name, change.name_length, value, sizeof(value), &count),
		    EXT4_OK);
		CHECK(count == sizeof(value) && memcmp(value, attribute_value, count) == 0);
		EXPECT(ext4_sync(fs), EXT4_OK);
		storage_export(device, exports, path, "range-reserved-");
		memcpy(before, device->cache, device->size);
		writes = device->writes;
		EXPECT(ext4_fallocate(fs, inode.number, inode.generation, offset, length,
			   EXT4_FALLOC_KEEP_SIZE, &update, &completed),
		    EXT4_EXISTS);
		CHECK(completed == 0 && device->writes == writes &&
		    memcmp(before, device->cache, device->size) == 0);
		change = attribute(EXT4_XATTR_REMOVE);
		EXPECT(ext4_fallocate(fs, inode.number, inode.generation, offset, length, 0,
			   &update, &completed),
		    EXT4_OK);
		CHECK(completed == length);
		contents(fs, &inode, expected, end);
		EXPECT(ext4_sync(fs), EXT4_OK);
		storage_export(device, exports, path, "range-grown-");
	} else {
		writes = device->writes;
		memcpy(before, device->cache, device->size);
		EXPECT(ext4_fallocate(fs, inode.number, inode.generation, offset, length,
			   EXT4_FALLOC_KEEP_SIZE, &update, &completed),
		    EXT4_UNSUPPORTED);
		CHECK(completed == 0 && device->writes == writes &&
		    memcmp(before, device->cache, device->size) == 0);
		update = attributes(NULL);
		EXPECT(ext4_write_partial(
			   fs, inode.number, inode.generation, 0, expected, end, &update, &count),
		    EXT4_OK);
		CHECK(count == end);
		contents(fs, &inode, expected, end);
	}
	update = attributes(NULL);
	blocks = inode.blocks_512;
	free_blocks = fs->info.free_blocks;
	offset = device->block_size - 7U;
	length = (RANGE_BLOCKS - 1U) * device->block_size + 20U;
	EXPECT(ext4_fallocate(fs, inode.number, inode.generation, offset, length,
		   EXT4_FALLOC_KEEP_SIZE | EXT4_FALLOC_PUNCH_HOLE, &update, &completed),
	    EXT4_OK);
	CHECK(completed == length);
	memset(expected + offset, 0, length);
	contents(fs, &inode, expected, end);
	CHECK(inode.blocks_512 < blocks && fs->info.free_blocks > free_blocks);
	EXPECT(ext4_sync(fs), EXT4_OK);
	storage_export(device, exports, path, "range-punched-");
	offset = 123U * device->block_size + 9U;
	for (index = 0; index < 45; index++) {
		expected[offset + index] = (uint8_t)(index + 1U);
	}
	EXPECT(ext4_write(fs, inode.number, inode.generation, offset, expected + offset, 45,
		   &update, &count),
	    EXT4_OK);
	CHECK(count == 45);
	contents(fs, &inode, expected, end);
	EXPECT(ext4_sync(fs), EXT4_OK);
	storage_export(device, exports, path, "range-reallocated-");
	EXPECT(ext4_hold_inode(fs, inode.number, inode.generation, &hold), EXT4_OK);
	EXPECT(ext4_get_inode(fs, EXT4_ROOT_INODE, &root), EXT4_OK);
	EXPECT(ext4_unlink(fs, root.number, root.generation, (const uint8_t *)"empty", 5,
		   inode.number, inode.generation, &update.change_time, &inode),
	    EXT4_OK);
	EXPECT(ext4_fallocate(fs, inode.number, inode.generation, 0, end + device->block_size,
		   EXT4_FALLOC_KEEP_SIZE | EXT4_FALLOC_PUNCH_HOLE, &update, &completed),
	    EXT4_OK);
	CHECK(completed == end + device->block_size);
	EXPECT(ext4_refresh_inode(hold, &inode), EXT4_OK);
	CHECK(inode.size == end && inode.blocks_512 == 0 && inode.links == 0);
	EXPECT(ext4_release_inode(hold), EXT4_OK);
	EXPECT(ext4_sync(fs), EXT4_OK);
	storage_export(device, exports, path, "range-released-");
	ext4_unmount(fs);
	free(before);
	free(expected);
	puts("PASS range reservation, growth, partial-block punch, reuse, attributes and held "
	     "lifetime");
}

static void
tree_edges(struct device *device)
{
	struct ext4_inode inode;
	struct ext4_fs *fs = mount_file(device, &inode);
	struct ext4_inode_update update = attributes(NULL);
	struct ext4_extent_header_disk *header;
	uint8_t *expected = calloc(15U * device->block_size, 1);
	uint64_t completed;
	size_t count;
	size_t index;

	CHECK(expected != NULL);
	for (index = 0; index < 4; index++) {
		memset(expected + index * 4U * device->block_size, (int)(index + 1U),
		    3U * device->block_size);
		EXPECT(
		    ext4_write(fs, inode.number, inode.generation, index * 4U * device->block_size,
			expected + index * 4U * device->block_size, 3U * device->block_size,
			&update, &count),
		    EXT4_OK);
	}
	/* Splitting an extent in a full four-record root needs a new mapping node.
	 * Subsequent leading removals must update index keys and collapse it again. */
	EXPECT(ext4_fallocate(fs, inode.number, inode.generation, device->block_size,
		   device->block_size, EXT4_FALLOC_KEEP_SIZE | EXT4_FALLOC_PUNCH_HOLE, &update,
		   &completed),
	    EXT4_OK);
	memset(expected + device->block_size, 0, device->block_size);
	contents(fs, &inode, expected, 15U * device->block_size);
	if (inode.flags & EXT4_INODE_EXTENTS) {
		header = (struct ext4_extent_header_disk *)inode.block_data;
		CHECK(ext4_le16(&header->depth) == 1);
	}
	EXPECT(ext4_fallocate(fs, inode.number, inode.generation, 0, 12U * device->block_size,
		   EXT4_FALLOC_KEEP_SIZE | EXT4_FALLOC_PUNCH_HOLE, &update, &completed),
	    EXT4_OK);
	memset(expected, 0, 12U * device->block_size);
	contents(fs, &inode, expected, 15U * device->block_size);
	if (inode.flags & EXT4_INODE_EXTENTS) {
		header = (struct ext4_extent_header_disk *)inode.block_data;
		CHECK(ext4_le16(&header->depth) == 0 && ext4_le16(&header->entries) == 1);
	}
	EXPECT(ext4_fallocate(fs, inode.number, inode.generation, 0, 16U * device->block_size,
		   EXT4_FALLOC_KEEP_SIZE | EXT4_FALLOC_PUNCH_HOLE, &update, &completed),
	    EXT4_OK);
	memset(expected, 0, 15U * device->block_size);
	contents(fs, &inode, expected, 15U * device->block_size);
	CHECK(inode.blocks_512 == 0);
	ext4_unmount(fs);
	free(expected);
	puts("PASS middle-extent split, leading index keys, root collapse and indirect-node "
	     "release");
}

static void
guards(struct device *device)
{
	struct ext4_inode inode;
	struct ext4_fs *fs = mount_file(device, &inode);
	struct ext4_inode_update update = attributes(NULL);
	uint64_t completed;

	EXPECT(ext4_fallocate(fs, inode.number, inode.generation, 0, 0, 0, &update, &completed),
	    EXT4_INVALID_ARGUMENT);
	EXPECT(ext4_fallocate(fs, inode.number, inode.generation, 0, 1, EXT4_FALLOC_PUNCH_HOLE,
		   &update, &completed),
	    EXT4_INVALID_ARGUMENT);
	EXPECT(ext4_fallocate(
		   fs, inode.number, inode.generation, 0, 1, UINT32_MAX, &update, &completed),
	    EXT4_INVALID_ARGUMENT);
	EXPECT(ext4_fallocate(
		   fs, inode.number, inode.generation, UINT64_MAX, 2, 0, &update, &completed),
	    EXT4_RANGE);
	EXPECT(
	    ext4_fallocate(fs, inode.number, inode.generation + 1U, 0, 1, 0, &update, &completed),
	    EXT4_STALE);
	EXPECT(ext4_get_inode(fs, EXT4_ROOT_INODE, &inode), EXT4_OK);
	EXPECT(ext4_fallocate(fs, inode.number, inode.generation, 0, 1, 0, &update, &completed),
	    EXT4_IS_DIRECTORY);
	CHECK(completed == 0 && device->writes == 0 && storage_equal(device, device->base));
	ext4_unmount(fs);
	puts("PASS range admission rejects unchanged media");
}

struct range_trace {
	struct device *device;
	uint64_t inode_offset;
	uint64_t journal_offset;
	uint64_t size;
	uint32_t blocks;
	bool stopped;
};

static enum ext4_result
trace_write(void *context, uint64_t offset, const void *buffer, size_t length)
{
	struct range_trace *trace = context;

	return device_write(trace->device, offset, buffer, length);
}

static enum ext4_result
trace_flush(void *context)
{
	struct range_trace *trace = context;
	struct device *device = trace->device;
	const struct ext4_jbd_super *journal;
	const struct ext4_inode_disk *inode;
	uint64_t size;
	enum ext4_result error = device_flush(device);

	if (error != EXT4_OK || trace->stopped) {
		return error;
	}
	journal = (const struct ext4_jbd_super *)(device->stable + trace->journal_offset);
	inode = (const struct ext4_inode_disk *)(device->stable + trace->inode_offset);
	size = ext4_le32(&inode->size_lo) | ((uint64_t)ext4_le32(&inode->size_hi) << 32);
	if (ext4_be32(&journal->start) == 0 &&
	    (size != trace->size || ext4_le32(&inode->blocks_lo) != trace->blocks)) {
		trace->stopped = true;
		device->fail_allocation = device->allocations + 1U;
	}
	return error;
}

static void
partial_progress(struct device *device, bool punch)
{
	struct ext4_inode inode;
	struct ext4_inode journal;
	struct ext4_inode_disk *disk;
	struct ext4_fs *fs = mount_file(device, &inode);
	struct ext4_xattr_change change = attribute(EXT4_XATTR_CREATE);
	struct ext4_inode_update update = attributes(&change);
	struct range_trace trace = { 0 };
	size_t length = 75U * device->block_size + RANGE_TAIL;
	uint8_t *expected;
	uint64_t physical;
	uint64_t completed;
	uint64_t resumed;
	uint32_t flags = punch ? EXT4_FALLOC_KEEP_SIZE | EXT4_FALLOC_PUNCH_HOLE : 0;

	if (!punch && !(inode.flags & EXT4_INODE_EXTENTS)) {
		EXPECT(ext4_fallocate(fs, inode.number, inode.generation, 0, length, flags, &update,
			   &completed),
		    EXT4_UNSUPPORTED);
		CHECK(completed == 0 && device->writes == 0);
		ext4_unmount(fs);
		return;
	}
	expected = punch ? seed_file(device, fs, &inode, length) : calloc(length, 1);
	CHECK(expected != NULL);
	EXPECT(ext4_get_inode(fs, inode.number, &inode), EXT4_OK);
	EXPECT(ext4_inode_location(fs, inode.number, &trace.inode_offset), EXT4_OK);
	EXPECT(ext4_get_inode(fs, fs->journal_inode, &journal), EXT4_OK);
	EXPECT(ext4_map_block(fs, &journal, 0, &physical), EXT4_OK);
	trace.journal_offset = physical * device->block_size;
	trace.device = device;
	trace.size = inode.size;
	disk = (struct ext4_inode_disk *)(device->cache + trace.inode_offset);
	trace.blocks = ext4_le32(&disk->blocks_lo);
	fs->journal->writer = (struct ext4_write_environment){ &trace, trace_write, trace_flush };
	EXPECT(ext4_fallocate(
		   fs, inode.number, inode.generation, 0, length, flags, &update, &completed),
	    EXT4_NO_MEMORY);
	CHECK(trace.stopped && completed > 0 && completed < length && !fs->aborted);
	device->fail_allocation = 0;
	if (punch) {
		memset(expected, 0, (size_t)completed);
	}
	contents(fs, &inode, expected, punch ? length : (size_t)completed);
	update = attributes(NULL);
	EXPECT(ext4_fallocate(fs, inode.number, inode.generation, completed, length - completed,
		   flags, &update, &resumed),
	    EXT4_OK);
	CHECK(resumed == length - completed);
	memset(expected, 0, length);
	contents(fs, &inode, expected, length);
	EXPECT(ext4_sync(fs), EXT4_OK);
	ext4_unmount(fs);
	free(expected);
	puts("PASS range failure after durable prefix and resumed one-time attribute transition");
}

static void
faults(struct device *device, bool punch, const char *exports, const char *path)
{
	struct ext4_inode inode;
	struct ext4_fs *fs = mount_file(device, &inode);
	struct ext4_xattr_change change = attribute(EXT4_XATTR_CREATE);
	struct ext4_inode_update update = attributes(&change);
	uint8_t *original = device->base;
	uint8_t *prepared = malloc(device->size);
	uint8_t *expected = malloc(device->size);
	uint8_t *seed;
	uint64_t completed;
	uint64_t offset = punch ? device->block_size - 7U : 32U * device->block_size + 17U;
	uint64_t length = (punch ? 3U : 6U) * device->block_size + 29U;
	uint32_t flags = EXT4_FALLOC_KEEP_SIZE | (punch ? EXT4_FALLOC_PUNCH_HOLE : 0);
	uint32_t reads;
	uint32_t allocations;
	uint32_t events;
	uint32_t barrier;
	uint32_t position;
	uint32_t phase;
	uint32_t count;
	uint32_t partial;
	uint32_t survival;
	uint32_t cuts = 0;
	uint32_t recovered = 0;
	char prefix[96];
	bool committed;
	enum ext4_result error;

	CHECK(prepared != NULL && expected != NULL);
	if (!punch && !(inode.flags & EXT4_INODE_EXTENTS)) {
		EXPECT(ext4_fallocate(fs, inode.number, inode.generation, offset, length, flags,
			   &update, &completed),
		    EXT4_UNSUPPORTED);
		CHECK(completed == 0 && device->writes == 0);
		ext4_unmount(fs);
		free(expected);
		free(prepared);
		puts("PASS indirect reservation rejects without changes");
		return;
	}
	seed = seed_file(device, fs, &inode, 8U * device->block_size + RANGE_TAIL);
	free(seed);
	EXPECT(ext4_sync(fs), EXT4_OK);
	memcpy(prepared, device->stable, device->size);
	ext4_unmount(fs);
	device->base = prepared;
	device_reset(device, prepared);
	snprintf(prefix, sizeof(prefix), "range-%s-before-", punch ? "punch" : "reserve");
	storage_export(device, exports, path, prefix);
	fs = mount_file(device, &inode);
	device->reads = device->allocations = 0;
	EXPECT(ext4_fallocate(
		   fs, inode.number, inode.generation, offset, length, flags, &update, &completed),
	    EXT4_OK);
	CHECK(completed == length);
	reads = device->reads;
	allocations = device->allocations;
	events = device->events;
	barrier = device->commit_barrier;
	CHECK(barrier != 0 && barrier < events);
	/* Compare recovered images with a clean successful volume, keeping cuts
	 * limited to the operation itself rather than the final explicit sync. */
	EXPECT(ext4_sync(fs), EXT4_OK);
	memcpy(expected, device->stable, device->size);
	snprintf(prefix, sizeof(prefix), "range-%s-atomic-", punch ? "punch" : "reserve");
	storage_export(device, exports, path, prefix);
	ext4_unmount(fs);
	for (phase = 0; exports == NULL && phase < 2; phase++) {
		count = phase == 0 ? allocations : reads;
		for (position = 1; position <= count; position++) {
			device_reset(device, prepared);
			fs = mount_file(device, &inode);
			device->reads = device->allocations = 0;
			device->fail_allocation = phase == 0 ? position : 0;
			device->fail_read = phase == 1 ? position : 0;
			error = ext4_fallocate(fs, inode.number, inode.generation, offset, length,
			    flags, &update, &completed);
			EXPECT(error, phase == 0 ? EXT4_NO_MEMORY : EXT4_IO);
			CHECK(completed == 0);
			ext4_unmount(fs);
			CHECK(storage_recover(device, prepared, true));
		}
	}
	for (position = 1; position <= events; position++) {
		if (exports != NULL && position != barrier && position != barrier + 1U) {
			continue;
		}
		for (partial = 0; partial < (exports == NULL ? 2U : 1U); partial++) {
			for (survival = 0; survival < (exports == NULL ? 3U : 1U); survival++) {
				device_reset(device, prepared);
				fs = mount_file(device, &inode);
				device->stop_at = position;
				device->partial = partial != 0;
				device->survival = survival;
				EXPECT(ext4_fallocate(fs, inode.number, inode.generation, offset,
					   length, flags, &update, &completed),
				    EXT4_IO);
				CHECK(completed == 0 && fs->aborted && device->off);
				committed = device->intent_durable;
				ext4_unmount(fs);
				snprintf(prefix, sizeof(prefix), "range-%s-%s-",
				    punch ? "punch" : "reserve",
				    committed ? "pending" : "uncommitted");
				storage_export(device, exports, path, prefix);
				recovered += storage_recover(device, expected, committed) ? 1U : 0U;
				cuts++;
			}
		}
	}
	device->base = original;
	free(expected);
	free(prepared);
	printf("PASS %s faults: allocations=%u reads=%u cuts=%u recovered=%u "
	       "torn_super_fail_closed=%u\n",
	    punch ? "punch" : "reserve", allocations, reads, cuts, recovered, cuts - recovered);
}

static void
linux_read(struct device *device)
{
	struct ext4_fs *fs;
	struct ext4_inode root;
	struct ext4_inode inode;
	size_t length = 75U * device->block_size + RANGE_TAIL;
	uint8_t *expected = malloc(length);
	size_t index;

	CHECK(expected != NULL);
	for (index = 0; index < length; index++) {
		expected[index] = (uint8_t)(index * 19U + 23U);
	}
	memset(expected + device->block_size - 7U, 0, 3U * device->block_size + 29U);
	EXPECT(ext4_mount(&device->environment, &fs), EXT4_OK);
	EXPECT(ext4_get_inode(fs, EXT4_ROOT_INODE, &root), EXT4_OK);
	EXPECT(ext4_lookup(fs, &root, (const uint8_t *)"linux-range", 11, &inode), EXT4_OK);
	contents(fs, &inode, expected, length);
	ext4_unmount(fs);
	CHECK(device->writes == 0 && storage_equal(device, device->base));
	free(expected);
	puts("PASS portable reads of Linux preallocation and hole punching");
}

int
main(int argc, char **argv)
{
	struct device device;
	const char *exports = NULL;
	bool fault_mode = false;
	bool read_mode = false;
	int index = 1;

	while (index < argc && argv[index][0] == '-') {
		if (strcmp(argv[index], "--faults") == 0) {
			fault_mode = true;
			index++;
		} else if (strcmp(argv[index], "--linux-read") == 0) {
			read_mode = true;
			index++;
		} else {
			CHECK(strcmp(argv[index], "--export") == 0 && index + 2 < argc);
			exports = argv[index + 1];
			index += 2;
		}
	}
	CHECK(index < argc && !(fault_mode && exports != NULL));
	CHECK(!read_mode || (!fault_mode && exports == NULL));
	for (; index < argc; index++) {
		storage_open(&device, argv[index]);
		if (read_mode) {
			linux_read(&device);
			storage_close(&device);
			continue;
		}
		if (!fault_mode) {
			operations(&device, exports, argv[index]);
			if (exports == NULL) {
				device_reset(&device, device.base);
				guards(&device);
				device_reset(&device, device.base);
				tree_edges(&device);
				device_reset(&device, device.base);
				partial_progress(&device, false);
				device_reset(&device, device.base);
				partial_progress(&device, true);
			}
		}
		if (fault_mode || exports != NULL) {
			device_reset(&device, device.base);
			faults(&device, false, exports, argv[index]);
			device_reset(&device, device.base);
			faults(&device, true, exports, argv[index]);
		}
		storage_close(&device);
		printf("PASS file ranges: %s\n", argv[index]);
	}
	return 0;
}

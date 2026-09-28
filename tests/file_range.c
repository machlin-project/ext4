/* SPDX-License-Identifier: BSD-3-Clause */
#include "allocate.h"
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
	fs->journal->writer =
	    (struct ext4_write_environment){ &trace, trace_write, trace_flush, NULL };
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

static void
capacity(struct device *device, bool full, bool external, bool seed_only, bool keep_size,
    const char *exports, const char *path)
{
	struct ext4_inode inode;
	struct ext4_inode root;
	struct ext4_inode reserve;
	struct ext4_inode result;
	struct ext4_fs *fs = mount_file(device, &inode);
	struct ext4_inode_update update = attributes(NULL);
	struct ext4_extent_header_disk *header;
	uint32_t blocks = full ? 2U : 2U * EXT4_ORPHAN_BATCH_BLOCKS + 5U;
	/* One reservation spans the gap between the fixture's released ranges,
	 * consuming two records. The external leaf is still completely full. */
	uint32_t runs = external
	    ? (device->block_size - sizeof(*header)) / sizeof(struct ext4_extent_disk) - 1U
	    : 4U;
	uint32_t owned_blocks = runs * blocks + (external ? 1U : 0U);
	size_t length = ((size_t)runs * (blocks + 1U) - 1U) * device->block_size;
	uint8_t *expected = calloc(length, 1);
	uint8_t *before = malloc(device->size);
	uint64_t completed;
	uint64_t inode_offset;
	struct ext4_inode_disk *disk;
	size_t written;
	size_t offset;
	uint32_t index;
	uint32_t writes;
	uint32_t fallbacks = 0;
	uint8_t value;
	enum ext4_result error;

	CHECK(expected != NULL && before != NULL && (inode.flags & EXT4_INODE_EXTENTS));
	CHECK(fs->info.free_blocks == 0);
	EXPECT(ext4_get_inode(fs, EXT4_ROOT_INODE, &root), EXT4_OK);
	EXPECT(ext4_lookup(fs, &root, (const uint8_t *)(full ? "reserve" : "filler"), full ? 7 : 6,
		   &reserve),
	    EXT4_OK);
	if (full) {
		EXPECT(ext4_truncate(fs, reserve.number, reserve.generation, 0, &update, &result),
		    EXT4_OK);
		if (external) {
			EXPECT(ext4_lookup(fs, &root, (const uint8_t *)"filler", 6, &reserve),
			    EXT4_OK);
			EXPECT(ext4_fallocate(fs, reserve.number, reserve.generation, 0,
				   (uint64_t)(owned_blocks - 8U) * device->block_size,
				   EXT4_FALLOC_KEEP_SIZE | EXT4_FALLOC_PUNCH_HOLE, &update,
				   &completed),
			    EXT4_OK);
			CHECK(completed == (uint64_t)(owned_blocks - 8U) * device->block_size);
		}
	} else {
		EXPECT(ext4_fallocate(fs, reserve.number, reserve.generation, 0,
			   (uint64_t)4U * blocks * device->block_size,
			   EXT4_FALLOC_KEEP_SIZE | EXT4_FALLOC_PUNCH_HOLE, &update, &completed),
		    EXT4_OK);
		CHECK(completed == (uint64_t)4U * blocks * device->block_size);
	}
	CHECK(fs->info.free_blocks == owned_blocks);
	for (index = 0; index < runs; index++) {
		EXPECT(ext4_fallocate(fs, inode.number, inode.generation,
			   (uint64_t)index * (blocks + 1U) * device->block_size,
			   (uint64_t)blocks * device->block_size, 0, &update, &completed),
		    EXT4_OK);
		CHECK(completed == (uint64_t)blocks * device->block_size);
	}
	if (keep_size) {
		/* Model an imported full leaf without the core's spare-slot policy.
		 * Unwritten allocations past a zero EOF are a valid ext4 layout. */
		EXPECT(ext4_sync(fs), EXT4_OK);
		EXPECT(ext4_inode_location(fs, inode.number, &inode_offset), EXT4_OK);
		disk = (struct ext4_inode_disk *)(device->cache + inode_offset);
		ext4_encode32(&disk->size_lo, 0);
		ext4_encode32(&disk->size_hi, 0);
		ext4_inode_checksum_set(fs, inode.number, disk);
		memcpy(device->stable, device->cache, device->size);
	}
	contents(fs, &inode, expected, keep_size ? 0 : length);
	header = (struct ext4_extent_header_disk *)inode.block_data;
	CHECK(ext4_le16(&header->depth) == (external ? 1 : 0) &&
	    ext4_le16(&header->entries) == (external ? 1 : runs));
	EXPECT(ext4_sync(fs), EXT4_OK);
	CHECK(fs->info.free_blocks == 0);
	storage_export(device, exports, path, "range-capacity-reserved-");
	for (index = 0; !seed_only && index < runs; index++) {
		offset = ((size_t)index * (blocks + 1U) + 1U) * device->block_size + 7U;
		value = (uint8_t)(0xd1U + index);
		memcpy(before, device->cache, device->size);
		writes = device->writes;
		error = ext4_write(
		    fs, inode.number, inode.generation, offset, &value, 1, &update, &written);
		if (external && error == EXT4_OK) {
			/* The single-block fragment converts without splitting its record. */
			CHECK(written == 1);
		} else {
			EXPECT(error, EXT4_NO_SPACE);
			CHECK(written == 0 && device->writes == writes &&
			    memcmp(before, device->cache, device->size) == 0);
			fallbacks++;
		}
		EXPECT(ext4_write_partial(fs, inode.number, inode.generation, offset, &value, 1,
			   &update, &written),
		    EXT4_OK);
		CHECK(written == 1);
		expected[offset] = value;
		contents(fs, &inode, expected, length);
		CHECK(inode.blocks_512 == owned_blocks * (device->block_size / EXT4_SECTOR_SIZE));
		CHECK(fs->info.free_blocks == 0);
	}
	CHECK(seed_only || fallbacks >= runs - (external ? 1U : 0U));
	EXPECT(ext4_sync(fs), EXT4_OK);
	if (!seed_only) {
		storage_export(device, exports, path, "range-capacity-written-");
	}
	ext4_unmount(fs);
	free(before);
	free(expected);
	if (!seed_only) {
		puts("PASS writes into preallocation without free metadata space or stale-data "
		     "exposure");
	}
}

struct capacity_trace {
	struct device *device;
	uint32_t commits;
	uint32_t durable_commits;
	uint32_t last_barrier;
	bool pending;
};

static void
capacity_poison(struct device *device, const struct ext4_inode *inode)
{
	const struct ext4_extent_header_disk *header =
	    (const struct ext4_extent_header_disk *)inode->block_data;
	const struct ext4_extent_index_disk *child;
	const struct ext4_extent_disk *entries;
	uint64_t physical;
	uint32_t index;

	if (ext4_le16(&header->depth) == 1) {
		CHECK(ext4_le16(&header->entries) == 1);
		child = (const struct ext4_extent_index_disk *)(header + 1);
		physical =
		    ext4_le32(&child->child_lo) | (uint64_t)ext4_le16(&child->child_hi) << 32;
		CHECK(physical < device->blocks);
		header = (const struct ext4_extent_header_disk *)(device->cache +
		    physical * device->block_size);
	}
	CHECK(ext4_le16(&header->depth) == 0);
	entries = (const struct ext4_extent_disk *)(header + 1);
	for (index = 0; index < ext4_le16(&header->entries); index++) {
		uint16_t blocks = ext4_le16(&entries[index].length) - EXT4_EXTENT_UNWRITTEN_LIMIT;

		CHECK(ext4_le16(&entries[index].length) > EXT4_EXTENT_UNWRITTEN_LIMIT);
		physical = ext4_le32(&entries[index].physical_lo) |
		    (uint64_t)ext4_le16(&entries[index].physical_hi) << 32;
		CHECK(physical < device->blocks && blocks <= device->blocks - physical);
		memset(device->cache + physical * device->block_size, 0xd3,
		    (size_t)blocks * device->block_size);
	}
	memcpy(device->stable, device->cache, device->size);
}

static void
capacity_keep_size(
    struct device *device, bool large, bool external, const char *exports, const char *path)
{
	struct ext4_inode inode;
	struct ext4_fs *fs;
	struct ext4_inode_update update = attributes(NULL);
	uint32_t blocks = large ? 2U * EXT4_ORPHAN_BATCH_BLOCKS + 5U : 2U;
	uint32_t runs = external ? (device->block_size - sizeof(struct ext4_extent_header_disk)) /
		    sizeof(struct ext4_extent_disk) -
		1U
				 : 4U;
	size_t length = ((size_t)runs * (blocks + 1U) - 2U) * device->block_size + 8U;
	uint8_t *expected = calloc(length, 1);
	uint8_t *before = malloc(device->size);
	uint8_t value;
	size_t offset;
	size_t written;
	uint32_t index;
	uint32_t writes;
	enum ext4_result error;

	CHECK(expected != NULL && before != NULL);
	capacity(device, !large, external, true, true, NULL, path);
	fs = mount_file(device, &inode);
	CHECK(inode.size == 0 && fs->info.free_blocks == 0);
	capacity_poison(device, &inode);
	storage_export(device, exports, path, "range-growth-reserved-");
	/* A write ending before the last block still needs mapping space. Reject
	 * without publishing an initialized block beyond the requested new EOF. */
	memcpy(before, device->cache, device->size);
	writes = device->writes;
	value = 0xe3;
	EXPECT(
	    ext4_write_partial(fs, inode.number, inode.generation, 7, &value, 1, &update, &written),
	    EXT4_NO_SPACE);
	CHECK(written == 0 && !fs->aborted && device->writes == writes &&
	    memcmp(before, device->cache, device->size) == 0);
	for (index = 0; index < runs; index++) {
		offset = ((size_t)index * (blocks + 1U) + blocks - 1U) * device->block_size + 7U;
		value = (uint8_t)(0xd1U + index);
		memcpy(before, device->cache, device->size);
		writes = device->writes;
		error = ext4_write(
		    fs, inode.number, inode.generation, offset, &value, 1, &update, &written);
		if (external && error == EXT4_OK) {
			CHECK(written == 1);
		} else {
			EXPECT(error, EXT4_NO_SPACE);
			CHECK(written == 0 && device->writes == writes &&
			    memcmp(before, device->cache, device->size) == 0);
		}
		EXPECT(ext4_write_partial(fs, inode.number, inode.generation, offset, &value, 1,
			   &update, &written),
		    EXT4_OK);
		CHECK(written == 1 && fs->info.free_blocks == 0);
		expected[offset] = value;
		contents(fs, &inode, expected, offset + 1U);
		CHECK(inode.blocks_512 ==
		    (runs * blocks + (external ? 1U : 0U)) *
			(device->block_size / EXT4_SECTOR_SIZE));
	}
	EXPECT(ext4_sync(fs), EXT4_OK);
	storage_export(device, exports, path, "range-growth-written-");
	ext4_unmount(fs);
	free(before);
	free(expected);
	puts("PASS KEEP_SIZE growth commits zeroed conversion, data and EOF without free blocks");
}

static void
capacity_keep_prefix(struct device *device, const char *path)
{
	struct ext4_inode inode;
	struct ext4_fs *fs;
	struct ext4_inode_update update = attributes(NULL);
	size_t offset = device->block_size + 7U;
	size_t length = 2U * device->block_size;
	size_t prefix = device->block_size - 7U;
	size_t written;
	uint8_t *bytes = malloc(length);
	uint8_t *expected = calloc(length, 1);
	uint8_t *prepared = malloc(device->size);
	uint32_t writes;

	CHECK(bytes != NULL && expected != NULL && prepared != NULL);
	capacity(device, true, false, true, true, NULL, path);
	fs = mount_file(device, &inode);
	capacity_poison(device, &inode);
	memcpy(prepared, device->stable, device->size);
	memset(bytes, 0x5d, length);
	update.modify_time.seconds += 9;
	update.change_time.seconds += 9;
	EXPECT(ext4_write_partial(
		   fs, inode.number, inode.generation, offset, bytes, length, &update, &written),
	    EXT4_NO_SPACE);
	CHECK(written == prefix && !fs->aborted && fs->info.free_blocks == 0);
	memset(expected + offset, 0x5d, prefix);
	contents(fs, &inode, expected, length);
	CHECK(inode.modify_time.seconds == RANGE_SECONDS + 9 &&
	    inode.change_time.seconds == RANGE_SECONDS + 10 &&
	    inode.blocks_512 == 8U * (device->block_size / EXT4_SECTOR_SIZE));
	writes = device->writes;
	EXPECT(ext4_write_partial(fs, inode.number, inode.generation, offset + prefix,
		   bytes + prefix, length - prefix, &update, &written),
	    EXT4_NO_SPACE);
	CHECK(written == 0 && device->writes == writes && !fs->aborted);
	contents(fs, &inode, expected, length);
	EXPECT(ext4_sync(fs), EXT4_OK);
	ext4_unmount(fs);
	/* Reaching the last block also works when the request starts before it.
	 * The whole extent's bytes and EOF must fit the same final transaction. */
	device_reset(device, prepared);
	fs = mount_file(device, &inode);
	EXPECT(ext4_write_partial(
		   fs, inode.number, inode.generation, 0, bytes, length, &update, &written),
	    EXT4_OK);
	CHECK(written == length && fs->info.free_blocks == 0 && !fs->aborted);
	contents(fs, &inode, bytes, length);
	EXPECT(ext4_sync(fs), EXT4_OK);
	ext4_unmount(fs);
	free(prepared);
	free(expected);
	free(bytes);
	puts("PASS KEEP_SIZE whole-extent growth and durable data/EOF prefix before a full-disk "
	     "hole");
}

static enum ext4_result
capacity_trace_write(void *context, uint64_t offset, const void *buffer, size_t length)
{
	struct capacity_trace *trace = context;
	const struct ext4_jbd_header *header = buffer;
	enum ext4_result error = device_write(trace->device, offset, buffer, length);

	if (error == EXT4_OK && trace->device->journal_blocks[offset / trace->device->block_size] &&
	    ext4_be32(&header->magic) == EXT4_JBD_MAGIC &&
	    ext4_be32(&header->type) == EXT4_JBD_COMMIT) {
		trace->commits++;
		trace->pending = true;
	}
	return error;
}

static enum ext4_result
capacity_trace_flush(void *context)
{
	struct capacity_trace *trace = context;
	enum ext4_result error = device_flush(trace->device);

	if (trace->pending && (error == EXT4_OK || trace->device->survival == 1)) {
		trace->durable_commits = trace->commits;
		trace->last_barrier = trace->device->events;
	}
	trace->pending = false;
	return error;
}

static bool
capacity_recover(
    struct device *device, size_t length, size_t offset, bool committed, bool keep_size)
{
	struct ext4_recovery_report report;
	struct ext4_super_disk *super;
	struct ext4_fs *fs;
	struct ext4_inode root;
	struct ext4_inode inode;
	uint8_t *expected = calloc(length, 1);
	uint8_t value = 0;
	size_t count;
	enum ext4_result error;

	CHECK(expected != NULL);
	device_reset(device, device->stable);
	error = ext4_recover(&device->environment, &device->writer, &report);
	if (error == EXT4_CORRUPT) {
		super = (struct ext4_super_disk *)(device->cache + EXT4_SUPER_OFFSET);
		CHECK(device->metadata_checksum && device->writes == 0 &&
		    ext4_le32(&super->checksum) !=
			ext4_crc32c(UINT32_MAX, super, offsetof(struct ext4_super_disk, checksum)));
		free(expected);
		return false;
	}
	EXPECT(error, EXT4_OK);
	EXPECT(ext4_mount(&device->environment, &fs), EXT4_OK);
	EXPECT(ext4_get_inode(fs, EXT4_ROOT_INODE, &root), EXT4_OK);
	EXPECT(ext4_lookup(fs, &root, (const uint8_t *)"empty", 5, &inode), EXT4_OK);
	EXPECT(ext4_read(fs, &inode, offset, &value, 1, &count), EXT4_OK);
	CHECK(count == (size_t)(!keep_size || inode.size != 0) && (value == 0 || value == 0xe3) &&
	    (!committed || value == 0xe3));
	expected[offset] = value;
	contents(fs, &inode, expected, keep_size && value == 0 ? 0 : length);
	if (keep_size) {
		const struct ext4_extent_header_disk *header =
		    (const struct ext4_extent_header_disk *)inode.block_data;
		const struct ext4_extent_disk *entries =
		    (const struct ext4_extent_disk *)(header + 1);
		uint32_t index;

		CHECK(ext4_le16(&header->depth) == 0 && ext4_le16(&header->entries) == 4);
		for (index = 0; index < 4; index++) {
			CHECK(ext4_le16(&entries[index].length) ==
			    2U * EXT4_ORPHAN_BATCH_BLOCKS + 5U +
				(index == 0 && value != 0 ? 0 : EXT4_EXTENT_UNWRITTEN_LIMIT));
		}
	}
	CHECK(fs->info.free_blocks == 0 && inode.mode == (EXT4_MODE_REGULAR | 0640) &&
	    inode.links == 1 &&
	    inode.blocks_512 ==
		4U * (2U * EXT4_ORPHAN_BATCH_BLOCKS + 5U) *
		    (device->block_size / EXT4_SECTOR_SIZE));
	CHECK(inode.modify_time.seconds == RANGE_SECONDS + (value == 0 ? 0 : 9) &&
	    inode.change_time.seconds == RANGE_SECONDS + 1 + (value == 0 ? 0 : 9));
	ext4_unmount(fs);
	CHECK(device->live == 0 && memcmp(device->cache, device->stable, device->size) == 0);
	free(expected);
	return true;
}

static void
capacity_faults(struct device *device, bool keep_size, const char *exports, const char *path)
{
	struct ext4_inode inode;
	struct ext4_fs *fs;
	struct ext4_inode_update update = attributes(NULL);
	struct capacity_trace trace;
	uint8_t *original = device->base;
	uint8_t *prepared = malloc(device->size);
	uint8_t value = 0xe3;
	size_t length;
	size_t offset =
	    (keep_size ? 2U * EXT4_ORPHAN_BATCH_BLOCKS + 4U : 1U) * device->block_size + 7U;
	size_t completed;
	uint32_t phase;
	uint32_t count;
	uint32_t position;
	uint32_t allocations;
	uint32_t reads;
	uint32_t events;
	uint32_t commits;
	uint32_t barrier;
	uint32_t partial;
	uint32_t survival;
	uint32_t cuts = 0;
	uint32_t recovered = 0;
	bool committed;
	enum ext4_result error;

	CHECK(prepared != NULL);
	CHECK(exports == NULL || keep_size);
	capacity(device, false, false, true, keep_size, NULL, path);
	fs = mount_file(device, &inode);
	length = keep_size ? offset + 1U : (size_t)inode.size;
	capacity_poison(device, &inode);
	ext4_unmount(fs);
	memcpy(prepared, device->cache, device->size);
	device->base = prepared;
	device_reset(device, prepared);
	storage_export(device, exports, path, "range-growth-before-");
	update.modify_time.seconds += 9;
	update.change_time.seconds += 9;
	fs = mount_file(device, &inode);
	trace = (struct capacity_trace){ .device = device };
	fs->journal->writer = (struct ext4_write_environment){ &trace, capacity_trace_write,
		capacity_trace_flush, NULL };
	device->allocations = device->reads = 0;
	EXPECT(ext4_write_partial(
		   fs, inode.number, inode.generation, offset, &value, 1, &update, &completed),
	    EXT4_OK);
	CHECK(completed == 1 && trace.commits >= 4 && trace.durable_commits == trace.commits);
	allocations = device->allocations;
	reads = device->reads;
	events = device->events;
	commits = trace.commits;
	barrier = trace.last_barrier;
	EXPECT(ext4_sync(fs), EXT4_OK);
	storage_export(device, exports, path, "range-growth-after-");
	ext4_unmount(fs);
	CHECK(capacity_recover(device, length, offset, true, keep_size));
	for (phase = 0; exports == NULL && phase < 2; phase++) {
		count = phase == 0 ? allocations : reads;
		for (position = 1; position <= count; position++) {
			device_reset(device, prepared);
			fs = mount_file(device, &inode);
			trace = (struct capacity_trace){ .device = device };
			fs->journal->writer = (struct ext4_write_environment){ &trace,
				capacity_trace_write, capacity_trace_flush, NULL };
			device->allocations = device->reads = 0;
			device->fail_allocation = phase == 0 ? position : 0;
			device->fail_read = phase == 1 ? position : 0;
			error = ext4_write_partial(fs, inode.number, inode.generation, offset,
			    &value, 1, &update, &completed);
			EXPECT(error, phase == 0 ? EXT4_NO_MEMORY : EXT4_IO);
			CHECK(completed == 0);
			ext4_unmount(fs);
			CHECK(capacity_recover(
			    device, length, offset, trace.durable_commits == commits, keep_size));
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
				trace = (struct capacity_trace){ .device = device };
				fs->journal->writer = (struct ext4_write_environment){ &trace,
					capacity_trace_write, capacity_trace_flush, NULL };
				device->stop_at = position;
				device->partial = partial != 0;
				device->survival = survival;
				EXPECT(ext4_write_partial(fs, inode.number, inode.generation,
					   offset, &value, 1, &update, &completed),
				    EXT4_IO);
				CHECK(completed == 0 && fs->aborted && device->off);
				committed = trace.durable_commits == commits;
				ext4_unmount(fs);
				storage_export(device, exports, path,
				    committed ? "range-growth-pending-"
					      : "range-growth-uncommitted-");
				recovered +=
				    capacity_recover(device, length, offset, committed, keep_size)
				    ? 1U
				    : 0U;
				cuts++;
			}
		}
	}
	device->base = original;
	free(prepared);
	printf("PASS full-space %sconversion faults: allocations=%u reads=%u commits=%u cuts=%u "
	       "recovered=%u torn_super_fail_closed=%u\n",
	    keep_size ? "growth " : "", allocations, reads, commits, cuts, recovered,
	    cuts - recovered);
}

#include "range_reservation.h"
#include "reservation_faults.h"

int
main(int argc, char **argv)
{
	struct device device;
	const char *exports = NULL;
	bool fault_mode = false;
	bool read_mode = false;
	bool capacity_mode = false;
	bool full_capacity = false;
	bool capacity_fault_mode = false;
	bool capacity_tree = false;
	bool capacity_keep = false;
	bool capacity_prefix = false;
	bool reservation = false;
	bool reservation_tree = false;
	bool reservation_fault_mode = false;
	bool reservation_controls = false;
	int index = 1;

	while (index < argc && argv[index][0] == '-') {
		if (strcmp(argv[index], "--reservation") == 0 ||
		    strcmp(argv[index], "--reservation-tree") == 0 ||
		    strcmp(argv[index], "--reservation-faults") == 0 ||
		    strcmp(argv[index], "--reservation-controls") == 0) {
			reservation = true;
			reservation_tree = strcmp(argv[index], "--reservation-tree") == 0;
			reservation_fault_mode = strcmp(argv[index], "--reservation-faults") == 0;
			reservation_controls = strcmp(argv[index], "--reservation-controls") == 0;
			index++;
		} else if (strcmp(argv[index], "--faults") == 0) {
			fault_mode = true;
			index++;
		} else if (strcmp(argv[index], "--linux-read") == 0) {
			read_mode = true;
			index++;
		} else if (strcmp(argv[index], "--capacity") == 0 ||
		    strcmp(argv[index], "--capacity-full") == 0 ||
		    strcmp(argv[index], "--capacity-tree") == 0) {
			capacity_mode = true;
			full_capacity = strcmp(argv[index], "--capacity") != 0;
			capacity_tree = strcmp(argv[index], "--capacity-tree") == 0;
			index++;
		} else if (strcmp(argv[index], "--capacity-faults") == 0 ||
		    strcmp(argv[index], "--capacity-keep-faults") == 0) {
			capacity_fault_mode = true;
			capacity_keep = strcmp(argv[index], "--capacity-keep-faults") == 0;
			index++;
		} else if (strcmp(argv[index], "--capacity-keep-prefix") == 0) {
			capacity_prefix = true;
			index++;
		} else if (strcmp(argv[index], "--capacity-keep") == 0 ||
		    strcmp(argv[index], "--capacity-keep-large") == 0 ||
		    strcmp(argv[index], "--capacity-keep-tree") == 0) {
			capacity_keep = true;
			full_capacity = strcmp(argv[index], "--capacity-keep-large") != 0;
			capacity_tree = strcmp(argv[index], "--capacity-keep-tree") == 0;
			index++;
		} else {
			CHECK(strcmp(argv[index], "--export") == 0 && index + 2 < argc);
			exports = argv[index + 1];
			index += 2;
		}
	}
	CHECK(index < argc && !(fault_mode && exports != NULL));
	CHECK(!read_mode || (!fault_mode && exports == NULL));
	CHECK(!capacity_mode || (!fault_mode && !read_mode));
	CHECK(!capacity_fault_mode ||
	    (!capacity_mode && !fault_mode && !read_mode && (capacity_keep || exports == NULL)));
	for (; index < argc; index++) {
		storage_open(&device, argv[index]);
		if (reservation) {
			if (reservation_controls) {
				CHECK(exports == NULL);
				reservation_contract(&device);
				device_reset(&device, device.base);
				reservation_partial_growth(&device);
				device_reset(&device, device.base);
				reservation_reduced_capacity(&device, false);
				device_reset(&device, device.base);
				reservation_reduced_capacity(&device, true);
			} else if (reservation_fault_mode) {
				reservation_faults(&device, exports, argv[index]);
			} else {
				reservation_writes(&device, reservation_tree, exports, argv[index]);
			}
			storage_close(&device);
			continue;
		}
		if (capacity_prefix) {
			CHECK(exports == NULL);
			capacity_keep_prefix(&device, argv[index]);
			storage_close(&device);
			continue;
		}
		if (capacity_keep && !capacity_fault_mode) {
			capacity_keep_size(
			    &device, !full_capacity, capacity_tree, exports, argv[index]);
			storage_close(&device);
			continue;
		}
		if (capacity_fault_mode) {
			capacity_faults(&device, capacity_keep, exports, argv[index]);
			storage_close(&device);
			continue;
		}
		if (capacity_mode) {
			capacity(&device, full_capacity, capacity_tree, false, false, exports,
			    argv[index]);
			storage_close(&device);
			continue;
		}
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

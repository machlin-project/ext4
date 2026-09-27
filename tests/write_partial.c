/* SPDX-License-Identifier: BSD-3-Clause */
#include "storage.h"

#define PARTIAL_SMALL_CREDITS 12U
#define PARTIAL_FAULT_BLOCKS 13U
#define PARTIAL_TRACE_LIMIT 32U
#define PARTIAL_SECONDS 1700000090
#define PARTIAL_OFFSET 17U

struct write_trace {
	struct device *device;
	uint64_t inode_offset;
	uint64_t journal_offset;
	uint64_t sizes[PARTIAL_TRACE_LIMIT];
	size_t count;
};

static const uint8_t attribute_name[] = "partial-write";
static const uint8_t attribute_value[] = { 0, 0xff, 1, 0x80, 2, 3 };

static struct ext4_inode_update
write_update(struct ext4_xattr_change *change)
{
	struct ext4_inode_update update = { 0 };

	update.fields = EXT4_ATTR_PERMISSIONS | EXT4_ATTR_MODIFY_TIME | EXT4_ATTR_CHANGE_TIME |
	    EXT4_ATTR_XATTRS;
	update.permissions = 0640;
	update.modify_time.seconds = PARTIAL_SECONDS;
	update.change_time.seconds = PARTIAL_SECONDS + 1;
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
	change.name = attribute_name;
	change.name_length = sizeof(attribute_name) - 1U;
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

static uint8_t *
pattern(size_t length, uint8_t seed)
{
	uint8_t *bytes = malloc(length);
	size_t index;

	CHECK(bytes != NULL);
	for (index = 0; index < length; index++) {
		bytes[index] = (uint8_t)(seed + index * 29U + index / 251U);
	}
	return bytes;
}

static void
check_contents(struct ext4_fs *fs, uint32_t number, uint64_t offset, const uint8_t *bytes,
    size_t length, bool has_attribute, struct ext4_inode_hold *hold)
{
	struct ext4_inode inode;
	uint8_t value[sizeof(attribute_value)];
	uint8_t *observed = malloc((size_t)offset + length + 1U);
	size_t completed;
	size_t index;
	enum ext4_result error;

	CHECK(observed != NULL);
	EXPECT(hold == NULL ? ext4_get_inode(fs, number, &inode) : ext4_refresh_inode(hold, &inode),
	    EXT4_OK);
	CHECK(inode.size == (length == 0 ? 0 : offset + length));
	memset(observed, 0xa5, (size_t)offset + length + 1U);
	EXPECT(ext4_read(fs, &inode, 0, observed, (size_t)inode.size + 1U, &completed), EXT4_OK);
	CHECK(completed == inode.size && observed[completed] == 0xa5);
	if (length != 0) {
		for (index = 0; index < offset; index++) {
			CHECK(observed[index] == 0);
		}
		CHECK(memcmp(observed + offset, bytes, length) == 0);
		CHECK((inode.mode & EXT4_MODE_PERMISSIONS) == 0640 &&
		    inode.modify_time.seconds == PARTIAL_SECONDS &&
		    inode.change_time.seconds == PARTIAL_SECONDS + 1);
	}
	error = ext4_get_xattr(fs, number, inode.generation, EXT4_XATTR_USER, attribute_name,
	    sizeof(attribute_name) - 1U, value, sizeof(value), &completed);
	EXPECT(error, has_attribute ? EXT4_OK : EXT4_NOT_FOUND);
	if (has_attribute) {
		CHECK(completed == sizeof(value) &&
		    memcmp(value, attribute_value, sizeof(value)) == 0);
	}
	free(observed);
}

static void
operations(struct device *device, const char *exports, const char *path)
{
	struct ext4_inode inode;
	struct ext4_inode root;
	struct ext4_inode_hold *hold;
	struct ext4_fs *fs = mount_file(device, &inode);
	struct ext4_xattr_change change = attribute(EXT4_XATTR_CREATE);
	struct ext4_inode_update update = write_update(&change);
	/* Exceed an entire transaction while fitting the ordinary free pool of the
	 * 32 KiB fixture, whose journal already owns half of its 64 MiB image. */
	size_t length = (EXT4_TRANSACTION_MAX_BLOCKS + 13U) * device->block_size + 43U;
	uint8_t *bytes = pattern(length, 7);
	uint8_t *before = malloc(device->size);
	size_t completed;
	uint32_t writes;

	CHECK(before != NULL);
	EXPECT(ext4_write(fs, inode.number, inode.generation, PARTIAL_OFFSET, bytes, length,
		   &update, &completed),
	    EXT4_RANGE);
	CHECK(completed == 0 && device->writes == 0 && storage_equal(device, device->base));
	EXPECT(ext4_write_partial(fs, inode.number, inode.generation, PARTIAL_OFFSET, bytes, length,
		   &update, &completed),
	    EXT4_OK);
	CHECK(completed == length);
	check_contents(fs, inode.number, PARTIAL_OFFSET, bytes, length, true, NULL);
	EXPECT(ext4_sync(fs), EXT4_OK);
	storage_export(device, exports, path, "large-");
	memcpy(before, device->cache, device->size);
	writes = device->writes;
	EXPECT(ext4_write_partial(fs, inode.number, inode.generation, PARTIAL_OFFSET, bytes, length,
		   &update, &completed),
	    EXT4_EXISTS);
	CHECK(completed == 0 && writes == device->writes &&
	    memcmp(before, device->cache, device->size) == 0);
	/* REMOVE, like CREATE, must run once even though every data chunk commits. */
	change = attribute(EXT4_XATTR_REMOVE);
	bytes[0] ^= 0xff;
	EXPECT(ext4_write_partial(fs, inode.number, inode.generation, PARTIAL_OFFSET, bytes, length,
		   &update, &completed),
	    EXT4_OK);
	CHECK(completed == length);
	check_contents(fs, inode.number, PARTIAL_OFFSET, bytes, length, false, NULL);
	EXPECT(ext4_sync(fs), EXT4_OK);
	storage_export(device, exports, path, "overwrite-");
	EXPECT(ext4_hold_inode(fs, inode.number, inode.generation, &hold), EXT4_OK);
	EXPECT(ext4_get_inode(fs, EXT4_ROOT_INODE, &root), EXT4_OK);
	EXPECT(ext4_unlink(fs, root.number, root.generation, (const uint8_t *)"empty", 5,
		   inode.number, inode.generation, &update.change_time, &inode),
	    EXT4_OK);
	update = write_update(NULL);
	bytes[length - 1U] ^= 0xff;
	EXPECT(ext4_write_partial(fs, inode.number, inode.generation, PARTIAL_OFFSET, bytes, length,
		   &update, &completed),
	    EXT4_OK);
	CHECK(completed == length);
	check_contents(fs, inode.number, PARTIAL_OFFSET, bytes, length, false, hold);
	EXPECT(ext4_release_inode(hold), EXT4_OK);
	EXPECT(ext4_sync(fs), EXT4_OK);
	storage_export(device, exports, path, "released-");
	ext4_unmount(fs);
	free(before);
	free(bytes);
	puts("PASS large writes: growth, overwrite, one-time CREATE/REMOVE, held unlinked inode");
}

static void
guards(struct device *device)
{
	struct ext4_inode inode;
	struct ext4_fs *fs = mount_file(device, &inode);
	struct ext4_inode_update update = write_update(NULL);
	uint8_t byte = 1;
	size_t completed;

	EXPECT(ext4_write_partial(fs, inode.number, inode.generation, 0, &byte, 1, &update, NULL),
	    EXT4_INVALID_ARGUMENT);
	EXPECT(ext4_write_partial(
		   NULL, inode.number, inode.generation, 0, &byte, 1, &update, &completed),
	    EXT4_INVALID_ARGUMENT);
	CHECK(completed == 0);
	EXPECT(
	    ext4_write_partial(fs, inode.number, inode.generation, 0, NULL, 1, &update, &completed),
	    EXT4_INVALID_ARGUMENT);
	EXPECT(ext4_write_partial(
		   fs, inode.number, inode.generation, UINT64_MAX, &byte, 2, &update, &completed),
	    EXT4_RANGE);
	EXPECT(ext4_write_partial(fs, inode.number, inode.generation,
		   (uint64_t)UINT32_MAX * device->block_size - 1U, &byte, 2, &update, &completed),
	    EXT4_RANGE);
	EXPECT(ext4_write_partial(
		   fs, inode.number, inode.generation + 1U, 0, &byte, 1, &update, &completed),
	    EXT4_STALE);
	EXPECT(
	    ext4_write_partial(fs, inode.number, inode.generation, 0, NULL, 0, &update, &completed),
	    EXT4_OK);
	update.modify_time.nanoseconds = EXT4_NANOSECONDS_PER_SECOND;
	EXPECT(ext4_write_partial(
		   fs, inode.number, inode.generation, 0, &byte, 1, &update, &completed),
	    EXT4_RANGE);
	CHECK(completed == 0 && device->writes == 0 && storage_equal(device, device->base));
	ext4_unmount(fs);
	puts("PASS partial-write guards: no writes on invalid input, time, generation or overflow");
}

static void
reserved_space(struct device *device)
{
	struct ext4_inode inode;
	struct ext4_fs *fs = mount_file(device, &inode);
	struct ext4_super_disk *super =
	    (struct ext4_super_disk *)(device->cache + EXT4_SUPER_OFFSET);
	struct ext4_xattr_change change = attribute(EXT4_XATTR_CREATE);
	struct ext4_inode_update update = write_update(&change);
	uint64_t reserved = fs->info.free_blocks - 5U;
	size_t length = 20U * device->block_size;
	uint8_t *bytes = pattern(length, 19);
	size_t completed;
	size_t prefix;
	uint32_t writes;

	ext4_encode32(&super->reserved_blocks_lo, (uint32_t)reserved);
	ext4_encode32(&super->reserved_blocks_hi, (uint32_t)(reserved >> 32));
	if (fs->metadata_checksum) {
		ext4_encode32(&super->checksum,
		    ext4_crc32c(UINT32_MAX, super, offsetof(struct ext4_super_disk, checksum)));
	}
	memcpy(device->stable, device->cache, device->size);
	EXPECT(ext4_write_partial(
		   fs, inode.number, inode.generation, 0, bytes, length, &update, &completed),
	    EXT4_NO_SPACE);
	CHECK(completed > 0 && completed < length && fs->info.free_blocks >= reserved);
	prefix = completed;
	check_contents(fs, inode.number, 0, bytes, prefix, true, NULL);
	update = write_update(NULL);
	writes = device->writes;
	EXPECT(ext4_write_partial(
		   fs, inode.number, inode.generation, prefix, bytes, 1, &update, &completed),
	    EXT4_NO_SPACE);
	CHECK(completed == 0 && device->writes == writes);
	EXPECT(ext4_write_partial(
		   fs, inode.number, inode.generation, 0, bytes, prefix, &update, &completed),
	    EXT4_OK);
	CHECK(completed == prefix);
	EXPECT(ext4_sync(fs), EXT4_OK);
	ext4_unmount(fs);
	free(bytes);
	puts("PASS allocation shortage: durable prefix, preserved reserved pool, existing-block "
	     "overwrite");
}

static uint8_t *
small_journal(struct device *device)
{
	struct ext4_inode inode;
	struct ext4_inode journal;
	struct ext4_fs *fs = mount_file(device, &inode);
	struct ext4_jbd_super *super;
	uint8_t *prepared = malloc(device->size);
	uint64_t physical;

	CHECK(prepared != NULL);
	EXPECT(ext4_get_inode(fs, fs->journal_inode, &journal), EXT4_OK);
	EXPECT(ext4_map_block(fs, &journal, 0, &physical), EXT4_OK);
	ext4_unmount(fs);
	memcpy(prepared, device->cache, device->size);
	super = (struct ext4_jbd_super *)(prepared + physical * device->block_size);
	ext4_encode_be32(
	    &super->max_length, ext4_be32(&super->first) + 2U * PARTIAL_SMALL_CREDITS + 2U);
	if (ext4_be32(&super->feature_incompat) & (EXT4_JBD_CSUM_V2 | EXT4_JBD_CSUM_V3)) {
		ext4_encode_be32(&super->checksum, 0);
		ext4_encode_be32(&super->checksum, ext4_crc32c(UINT32_MAX, super, sizeof(*super)));
	}
	return prepared;
}

static enum ext4_result
trace_write(void *context, uint64_t offset, const void *buffer, size_t length)
{
	struct write_trace *trace = context;

	return device_write(trace->device, offset, buffer, length);
}

static void
allocated_gap(struct device *device)
{
	struct ext4_inode inode;
	struct ext4_inode_disk *disk;
	struct ext4_fs *fs;
	struct ext4_inode_update update = write_update(NULL);
	uint8_t *prepared = small_journal(device);
	size_t length = (PARTIAL_SMALL_CREDITS + 3U) * device->block_size;
	uint8_t *bytes = pattern(length, 47);
	uint8_t *before = malloc(device->size);
	uint64_t location;
	size_t completed;
	uint32_t writes;

	CHECK(before != NULL);
	device_reset(device, prepared);
	fs = mount_file(device, &inode);
	EXPECT(ext4_write_partial(
		   fs, inode.number, inode.generation, 0, bytes, length, &update, &completed),
	    EXT4_OK);
	EXPECT(ext4_sync(fs), EXT4_OK);
	EXPECT(ext4_inode_location(fs, inode.number, &location), EXT4_OK);
	/* Keep valid written preallocation beyond a shortened EOF in this model.
	 * Exposing the gap must never publish its old nonzero bytes. */
	disk = (struct ext4_inode_disk *)(device->cache + location);
	ext4_encode32(&disk->size_lo, 1);
	ext4_encode32(&disk->size_hi, 0);
	ext4_inode_checksum_set(fs, inode.number, disk);
	memcpy(device->stable, device->cache, device->size);
	memcpy(before, device->cache, device->size);
	writes = device->writes;
	EXPECT(ext4_write_partial(
		   fs, inode.number, inode.generation, length, bytes, 1, &update, &completed),
	    EXT4_RANGE);
	CHECK(completed == 0 && !fs->aborted && device->writes == writes &&
	    memcmp(device->cache, before, device->size) == 0);
	ext4_unmount(fs);
	free(before);
	free(bytes);
	free(prepared);
	puts("PASS allocated EOF gap: explicit atomic zeroing limit, unchanged data and metadata");
}

static enum ext4_result
trace_flush(void *context)
{
	struct write_trace *trace = context;
	struct device *device = trace->device;
	const struct ext4_jbd_super *journal;
	const struct ext4_inode_disk *inode;
	uint64_t size;
	enum ext4_result error = device_flush(device);

	if (error != EXT4_OK) {
		return error;
	}
	journal = (const struct ext4_jbd_super *)(device->stable + trace->journal_offset);
	inode = (const struct ext4_inode_disk *)(device->stable + trace->inode_offset);
	size = ext4_le32(&inode->size_lo) | ((uint64_t)ext4_le32(&inode->size_hi) << 32);
	if (ext4_be32(&journal->start) == 0 && size != trace->sizes[trace->count - 1U]) {
		CHECK(trace->count < PARTIAL_TRACE_LIMIT && size > trace->sizes[trace->count - 1U]);
		trace->sizes[trace->count++] = size;
	}
	return EXT4_OK;
}

static void
attach_trace(struct write_trace *trace, struct device *device, struct ext4_fs *fs,
    const struct ext4_inode *inode)
{
	struct ext4_inode journal;
	uint64_t physical;

	memset(trace, 0, sizeof(*trace));
	trace->device = device;
	trace->count = 1;
	trace->sizes[0] = inode->size;
	EXPECT(ext4_inode_location(fs, inode->number, &trace->inode_offset), EXT4_OK);
	EXPECT(ext4_get_inode(fs, fs->journal_inode, &journal), EXT4_OK);
	EXPECT(ext4_map_block(fs, &journal, 0, &physical), EXT4_OK);
	trace->journal_offset = physical * device->block_size;
	fs->journal->writer = (struct ext4_write_environment){ trace, trace_write, trace_flush };
}

static void
faults(struct device *device)
{
	struct ext4_inode inode;
	struct ext4_inode after;
	struct ext4_fs *fs;
	struct ext4_recovery_report report;
	struct ext4_xattr_change change = attribute(EXT4_XATTR_CREATE);
	struct ext4_inode_update update;
	struct write_trace trace;
	uint8_t *prepared = small_journal(device);
	size_t length = PARTIAL_FAULT_BLOCKS * device->block_size + 43U;
	uint8_t *bytes = pattern(length, 31);
	uint32_t reads;
	uint32_t allocations;
	uint32_t events;
	uint32_t phase;
	uint32_t position;
	uint32_t count;
	uint32_t partial;
	uint32_t survival;
	uint32_t resource_cases = 0;
	uint32_t commit_read_errors = 0;
	uint32_t crash_cases = 0;
	uint32_t damaged_superblocks = 0;
	size_t completed;
	size_t resumed;
	size_t prefix;
	enum ext4_result error;

	device_reset(device, prepared);
	fs = mount_file(device, &inode);
	CHECK(ext4_journal_credits(fs->journal) == PARTIAL_SMALL_CREDITS);
	attach_trace(&trace, device, fs, &inode);
	device->reads = device->allocations = 0;
	update = write_update(&change);
	EXPECT(ext4_write_partial(fs, inode.number, inode.generation, PARTIAL_OFFSET, bytes, length,
		   &update, &completed),
	    EXT4_OK);
	CHECK(completed == length && trace.count > 2);
	reads = device->reads;
	allocations = device->allocations;
	events = device->events;
	ext4_unmount(fs);
	for (phase = 0; phase < 2; phase++) {
		count = phase == 0 ? allocations : reads;
		for (position = 1; position <= count; position++) {
			device_reset(device, prepared);
			fs = mount_file(device, &inode);
			device->reads = device->allocations = 0;
			device->fail_allocation = phase == 0 ? position : 0;
			device->fail_read = phase == 1 ? position : 0;
			update = write_update(&change);
			error = ext4_write_partial(fs, inode.number, inode.generation,
			    PARTIAL_OFFSET, bytes, length, &update, &completed);
			EXPECT(error, phase == 0 ? EXT4_NO_MEMORY : EXT4_IO);
			CHECK(completed < length);
			device->fail_allocation = device->fail_read = 0;
			if (fs->aborted) {
				/* Commit reads the recovery marker before its first write.
				 * That failure conservatively poisons the owner. Recovery
				 * must preserve exactly the earlier completed prefix. */
				CHECK(
				    phase == 1 && fs->journal->aborted && fs->journal->start == 0);
				ext4_unmount(fs);
				device_reset(device, device->stable);
				EXPECT(ext4_recover(&device->environment, &device->writer, &report),
				    EXT4_OK);
				fs = mount_file(device, &inode);
				commit_read_errors++;
			}
			check_contents(fs, inode.number, PARTIAL_OFFSET, bytes, completed,
			    completed != 0, NULL);
			if (completed != 0) {
				update = write_update(NULL);
			}
			EXPECT(ext4_write_partial(fs, inode.number, inode.generation,
				   PARTIAL_OFFSET + completed, bytes + completed,
				   length - completed, &update, &resumed),
			    EXT4_OK);
			CHECK(resumed == length - completed);
			check_contents(fs, inode.number, PARTIAL_OFFSET, bytes, length, true, NULL);
			EXPECT(ext4_sync(fs), EXT4_OK);
			ext4_unmount(fs);
			resource_cases++;
		}
	}
	for (position = 1; position <= events; position++) {
		for (partial = 0; partial < 2; partial++) {
			for (survival = 0; survival < 3; survival++) {
				device_reset(device, prepared);
				fs = mount_file(device, &inode);
				device->stop_at = position;
				device->partial = partial != 0;
				device->survival = survival;
				update = write_update(&change);
				EXPECT(ext4_write_partial(fs, inode.number, inode.generation,
					   PARTIAL_OFFSET, bytes, length, &update, &completed),
				    EXT4_IO);
				CHECK(fs->aborted && device->off && device->events == position);
				EXPECT(ext4_get_inode(fs, inode.number, &after),
				    EXT4_RECOVERY_REQUIRED);
				ext4_unmount(fs);
				device_reset(device, device->stable);
				error =
				    ext4_recover(&device->environment, &device->writer, &report);
				crash_cases++;
				if (error == EXT4_CORRUPT) {
					const struct ext4_super_disk *super =
					    (const struct ext4_super_disk *)(device->cache +
						EXT4_SUPER_OFFSET);

					CHECK(partial != 0 && survival != 0 &&
					    device->writes == 0 && device->metadata_checksum &&
					    ext4_le32(&super->checksum) !=
						ext4_crc32c(UINT32_MAX, super,
						    offsetof(struct ext4_super_disk, checksum)));
					damaged_superblocks++;
					continue;
				}
				EXPECT(error, EXT4_OK);
				fs = mount_file(device, &after);
				for (prefix = 0; prefix < trace.count; prefix++) {
					if (trace.sizes[prefix] ==
					    (completed == 0 ? 0 : PARTIAL_OFFSET + completed)) {
						break;
					}
				}
				CHECK(prefix + 1U < trace.count &&
				    (after.size == trace.sizes[prefix] ||
					after.size == trace.sizes[prefix + 1U]));
				check_contents(fs, after.number, PARTIAL_OFFSET, bytes,
				    after.size == 0 ? 0 : (size_t)after.size - PARTIAL_OFFSET,
				    after.size != 0, NULL);
				ext4_unmount(fs);
			}
		}
	}
	printf("PASS partial-write faults: resource=%u commit-read-errors=%u crash=%u "
	       "damaged-superblock=%u batches=%zu\n",
	    resource_cases, commit_read_errors, crash_cases, damaged_superblocks, trace.count - 1U);
	free(prepared);
	free(bytes);
}

int
main(int argc, char **argv)
{
	struct device device;
	const char *exports = NULL;
	bool fault_mode = false;
	int index = 1;

	while (index < argc && argv[index][0] == '-') {
		if (strcmp(argv[index], "--faults") == 0) {
			fault_mode = true;
			index++;
		} else {
			CHECK(strcmp(argv[index], "--export") == 0 && index + 2 < argc);
			exports = argv[index + 1];
			index += 2;
		}
	}
	CHECK(index < argc);
	CHECK(!fault_mode || exports == NULL);
	for (; index < argc; index++) {
		storage_open(&device, argv[index]);
		if (fault_mode) {
			faults(&device);
		} else {
			operations(&device, exports, argv[index]);
			/* Export mode prepares the three states for independent inspection;
			 * ordinary test mode additionally checks guards and bounded gaps. */
			if (exports == NULL) {
				device_reset(&device, device.base);
				guards(&device);
				device_reset(&device, device.base);
				reserved_space(&device);
				device_reset(&device, device.base);
				allocated_gap(&device);
			}
		}
		storage_close(&device);
		printf("PASS partial writes: %s\n", argv[index]);
	}
	return 0;
}

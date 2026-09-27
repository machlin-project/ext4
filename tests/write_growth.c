/* SPDX-License-Identifier: BSD-3-Clause */
#include "storage.h"

#define GROWTH_BLOCKS 269U
#define GROWTH_FAULT_BLOCKS 15U
#define GROWTH_SMALL_CREDITS 12U
#define GROWTH_TAIL 73U
#define GROWTH_OFFSET 17U
#define GROWTH_HOLE_FIRST 131U
#define GROWTH_HOLE_BLOCKS 3U
#define GROWTH_SECONDS 1700000120

static const uint8_t old_name[] = "growth-old";
static const uint8_t new_name[] = "growth-new";
/* Force an external block so admitted attributes also consume a journal credit. */
static const uint8_t value[300] = { 7, 0, 0x80, 0xff, 1, 9 };
static const uint8_t written[] = "newly exposed data!";

struct growth_fixture {
	uint8_t *prepared;
	struct ext4_inode before;
	uint64_t inode_offset;
	uint64_t end;
	uint64_t free_blocks;
	uint32_t blocks;
};

static uint8_t
data_byte(size_t index)
{
	return (uint8_t)(11U + index * 13U + index / 257U);
}

static struct ext4_xattr_change
attribute(const uint8_t *name, enum ext4_xattr_policy policy)
{
	struct ext4_xattr_change change = { 0 };

	change.name_index = EXT4_XATTR_USER;
	change.name = name;
	change.name_length = sizeof(old_name) - 1U;
	change.policy = policy;
	if (policy != EXT4_XATTR_REMOVE) {
		change.value = value;
		change.value_size = sizeof(value);
	}
	return change;
}

static struct ext4_inode_update
attributes(bool changed, const struct ext4_xattr_change *changes, size_t count)
{
	struct ext4_inode_update update = { 0 };

	update.fields = EXT4_ATTR_PERMISSIONS | EXT4_ATTR_MODIFY_TIME | EXT4_ATTR_CHANGE_TIME |
	    EXT4_ATTR_XATTRS;
	update.permissions = changed ? 0640 : 06750;
	update.modify_time.seconds = GROWTH_SECONDS + (changed ? 10 : 0);
	update.change_time.seconds = update.modify_time.seconds + 1;
	update.xattrs = changes;
	update.xattr_count = count;
	return update;
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
prepare(struct device *device, struct growth_fixture *fixture, bool small)
{
	struct ext4_inode inode;
	struct ext4_inode journal;
	struct ext4_inode_disk *disk;
	struct ext4_jbd_super *super;
	struct ext4_fs *fs = mount_file(device, &inode);
	struct ext4_xattr_change change = attribute(old_name, EXT4_XATTR_CREATE);
	struct ext4_inode_update update = attributes(false, &change, 1);
	uint8_t *bytes;
	uint64_t physical;
	size_t length;
	size_t first;
	size_t second;
	size_t index;
	size_t completed;

	memset(fixture, 0, sizeof(*fixture));
	fixture->blocks = small ? GROWTH_FAULT_BLOCKS : GROWTH_BLOCKS;
	length = (size_t)fixture->blocks * device->block_size + GROWTH_TAIL;
	bytes = malloc(length);
	fixture->prepared = malloc(device->size);
	CHECK(bytes != NULL && fixture->prepared != NULL);
	for (index = 0; index < length; index++) {
		bytes[index] = data_byte(index);
	}
	first = small ? length : GROWTH_HOLE_FIRST * device->block_size;
	EXPECT(ext4_write_partial(
		   fs, inode.number, inode.generation, 0, bytes, first, &update, &completed),
	    EXT4_OK);
	CHECK(completed == first);
	if (!small) {
		second = (GROWTH_HOLE_FIRST + GROWTH_HOLE_BLOCKS) * device->block_size;
		update = attributes(false, NULL, 0);
		EXPECT(ext4_write_partial(fs, inode.number, inode.generation, second,
			   bytes + second, length - second, &update, &completed),
		    EXT4_OK);
		CHECK(completed == length - second);
	}
	EXPECT(ext4_sync(fs), EXT4_OK);
	EXPECT(ext4_inode_location(fs, inode.number, &fixture->inode_offset), EXT4_OK);
	disk = (struct ext4_inode_disk *)(device->cache + fixture->inode_offset);
	/* Model existing written preallocation without freeing its backing blocks.
	 * The first partial block contains visible bytes that must survive every cut. */
	ext4_encode32(&disk->size_lo, device->block_size + GROWTH_OFFSET);
	ext4_encode32(&disk->size_hi, 0);
	ext4_inode_checksum_set(fs, inode.number, disk);
	EXPECT(ext4_get_inode(fs, inode.number, &fixture->before), EXT4_OK);
	fixture->free_blocks = fs->info.free_blocks;
	fixture->end =
	    (uint64_t)fixture->blocks * device->block_size + GROWTH_OFFSET + sizeof(written) - 1U;
	if (small) {
		EXPECT(ext4_get_inode(fs, fs->journal_inode, &journal), EXT4_OK);
		EXPECT(ext4_map_block(fs, &journal, 0, &physical), EXT4_OK);
		super = (struct ext4_jbd_super *)(device->cache + physical * device->block_size);
		ext4_encode_be32(
		    &super->max_length, ext4_be32(&super->first) + 2U * GROWTH_SMALL_CREDITS + 2U);
		if (ext4_be32(&super->feature_incompat) & (EXT4_JBD_CSUM_V2 | EXT4_JBD_CSUM_V3)) {
			ext4_encode_be32(&super->checksum, 0);
			ext4_encode_be32(
			    &super->checksum, ext4_crc32c(UINT32_MAX, super, sizeof(*super)));
		}
	}
	memcpy(fixture->prepared, device->cache, device->size);
	ext4_unmount(fs);
	free(bytes);
}

static enum ext4_result
grow(struct ext4_fs *fs, const struct growth_fixture *fixture, bool truncate,
    const struct ext4_inode_update *update, bool atomic)
{
	struct ext4_inode result = fixture->before;
	size_t completed;
	enum ext4_result error;

	if (truncate) {
		error = atomic ? ext4_truncate_atomic(fs, fixture->before.number,
				     fixture->before.generation, fixture->end, update, &result)
			       : ext4_truncate(fs, fixture->before.number,
				     fixture->before.generation, fixture->end, update, &result);
		if (error != EXT4_OK) {
			CHECK(memcmp(&result, &fixture->before, sizeof(result)) == 0);
		} else {
			CHECK(result.size == fixture->end);
		}
	} else {
		error = atomic
		    ? ext4_write(fs, fixture->before.number, fixture->before.generation,
			  fixture->end - (sizeof(written) - 1U), written, sizeof(written) - 1U,
			  update, &completed)
		    : ext4_write_partial(fs, fixture->before.number, fixture->before.generation,
			  fixture->end - (sizeof(written) - 1U), written, sizeof(written) - 1U,
			  update, &completed);
		CHECK(completed == (error == EXT4_OK ? sizeof(written) - 1U : 0));
	}
	return error;
}

static bool
check_state(struct device *device, struct ext4_fs *fs, const struct growth_fixture *fixture,
    bool truncate, struct ext4_inode_hold *hold)
{
	struct ext4_inode inode;
	uint8_t observed_value[sizeof(value)];
	uint8_t *bytes;
	uint64_t physical;
	uint64_t offset = fixture->end - (sizeof(written) - 1U);
	size_t index;
	size_t completed;
	bool changed;
	enum ext4_result error;

	EXPECT(hold == NULL ? ext4_get_inode(fs, fixture->before.number, &inode)
			    : ext4_refresh_inode(hold, &inode),
	    EXT4_OK);
	CHECK(inode.size == fixture->before.size || inode.size == fixture->end);
	changed = inode.size == fixture->end;
	CHECK(inode.blocks_512 == fixture->before.blocks_512 &&
	    inode.generation == fixture->before.generation && inode.uid == fixture->before.uid &&
	    inode.gid == fixture->before.gid && fs->info.free_blocks == fixture->free_blocks);
	if (!changed) {
		CHECK(memcmp(device->cache + fixture->inode_offset,
			  fixture->prepared + fixture->inode_offset, fs->inode_size) == 0);
	} else {
		CHECK((inode.mode & EXT4_MODE_PERMISSIONS) == 0640 &&
		    inode.modify_time.seconds == GROWTH_SECONDS + 10 &&
		    inode.change_time.seconds == GROWTH_SECONDS + 11);
	}
	bytes = malloc((size_t)inode.size + 1U);
	CHECK(bytes != NULL);
	bytes[inode.size] = 0xa5;
	EXPECT(ext4_read(fs, &inode, 0, bytes, (size_t)inode.size + 1U, &completed), EXT4_OK);
	CHECK(completed == inode.size && bytes[inode.size] == 0xa5);
	for (index = 0; index < inode.size; index++) {
		CHECK(bytes[index] ==
		    (index < fixture->before.size
			    ? data_byte(index)
			    : (!truncate && index >= offset ? written[index - offset] : 0)));
	}
	free(bytes);
	for (index = 0; index < 2; index++) {
		error = ext4_get_xattr(fs, inode.number, inode.generation, EXT4_XATTR_USER,
		    index == 0 ? old_name : new_name, sizeof(old_name) - 1U, observed_value,
		    sizeof(observed_value), &completed);
		EXPECT(error, ((index != 0) == changed) ? EXT4_OK : EXT4_NOT_FOUND);
		if (error == EXT4_OK) {
			CHECK(completed == sizeof(value) &&
			    memcmp(observed_value, value, sizeof(value)) == 0);
		}
	}
	/* Preparation and publication stop at the requested byte, retaining hidden
	 * bytes later in the last allocated block. */
	EXPECT(ext4_map_block(fs, &inode, (uint32_t)(fixture->end / device->block_size), &physical),
	    EXT4_OK);
	CHECK(physical != 0);
	for (index = (size_t)(fixture->end % device->block_size); index < GROWTH_TAIL; index++) {
		CHECK(device->cache[physical * device->block_size + index] ==
		    data_byte(
			(size_t)(fixture->end / device->block_size) * device->block_size + index));
	}
	return changed;
}

static void
operations(struct device *device, const struct growth_fixture *fixture, const char *exports,
    const char *path)
{
	struct ext4_inode inode;
	struct ext4_inode root;
	struct ext4_inode_hold *hold;
	struct ext4_fs *fs;
	struct ext4_xattr_change changes[2] = { attribute(old_name, EXT4_XATTR_REMOVE),
		attribute(new_name, EXT4_XATTR_CREATE) };
	struct ext4_xattr_change missing = attribute(new_name, EXT4_XATTR_REMOVE);
	struct ext4_inode_update update;
	unsigned int operation;

	for (operation = 0; operation < 2; operation++) {
		device_reset(device, fixture->prepared);
		fs = mount_file(device, &inode);
		update = attributes(true, changes, 2);
		EXPECT(grow(fs, fixture, operation != 0, &update, true), EXT4_RANGE);
		update = attributes(true, &missing, 1);
		EXPECT(grow(fs, fixture, operation != 0, &update, false), EXT4_NOT_FOUND);
		update = attributes(true, changes, 2);
		update.modify_time.nanoseconds = EXT4_NANOSECONDS_PER_SECOND;
		EXPECT(grow(fs, fixture, operation != 0, &update, false), EXT4_RANGE);
		CHECK(device->writes == 0 && storage_equal(device, fixture->prepared));
		update = attributes(true, changes, 2);
		EXPECT(grow(fs, fixture, operation != 0, &update, false), EXT4_OK);
		CHECK(check_state(device, fs, fixture, operation != 0, NULL));
		EXPECT(ext4_sync(fs), EXT4_OK);
		storage_export(
		    device, exports, path, operation == 0 ? "growth-write-" : "growth-truncate-");
		ext4_unmount(fs);
		if (exports == NULL) {
			device_reset(device, fixture->prepared);
			fs = mount_file(device, &inode);
			EXPECT(ext4_hold_inode(fs, inode.number, inode.generation, &hold), EXT4_OK);
			EXPECT(ext4_get_inode(fs, EXT4_ROOT_INODE, &root), EXT4_OK);
			EXPECT(
			    ext4_unlink(fs, root.number, root.generation, (const uint8_t *)"empty",
				5, inode.number, inode.generation, &update.change_time, &inode),
			    EXT4_OK);
			EXPECT(grow(fs, fixture, operation != 0, &update, false), EXT4_OK);
			CHECK(check_state(device, fs, fixture, operation != 0, hold));
			EXPECT(ext4_release_inode(hold), EXT4_OK);
			EXPECT(ext4_sync(fs), EXT4_OK);
			ext4_unmount(fs);
		}
	}
	puts("PASS preallocated growth: write/truncate, sparse gap, partial blocks, atomic limits, "
	     "attribute admission and retained inode");
}

static void
faults(struct device *device, const struct growth_fixture *fixture)
{
	struct growth_fixture selected = *fixture;
	struct ext4_inode inode;
	struct ext4_fs *fs;
	struct ext4_recovery_report report;
	struct ext4_xattr_change changes[2] = { attribute(old_name, EXT4_XATTR_REMOVE),
		attribute(new_name, EXT4_XATTR_CREATE) };
	struct ext4_inode_update update = attributes(true, changes, 2);
	uint32_t counts[3];
	uint32_t position;
	uint32_t phase;
	uint32_t resource_cases = 0;
	uint32_t crash_cases = 0;
	uint32_t damaged_superblocks = 0;
	unsigned int operation;
	unsigned int torn;
	unsigned int survival;
	enum ext4_result error;

	fixture = &selected;
	for (operation = 0; operation < 2; operation++) {
		/* The write gap alone fills all credits; its external xattr causes
		 * the shortage. Truncate needs two zeroing transactions before the
		 * final metadata publication. Both must take the preparation path. */
		selected.end =
		    (uint64_t)(operation == 0 ? GROWTH_SMALL_CREDITS - 1U : GROWTH_FAULT_BLOCKS) *
			device->block_size +
		    GROWTH_OFFSET + sizeof(written) - 1U;
		device_reset(device, fixture->prepared);
		fs = mount_file(device, &inode);
		CHECK(ext4_journal_credits(fs->journal) == GROWTH_SMALL_CREDITS);
		device->reads = device->allocations = 0;
		EXPECT(grow(fs, fixture, operation != 0, &update, false), EXT4_OK);
		counts[0] = device->allocations;
		counts[1] = device->reads;
		counts[2] = device->events;
		CHECK(check_state(device, fs, fixture, operation != 0, NULL));
		ext4_unmount(fs);
		for (phase = 0; phase < 2; phase++) {
			for (position = 1; position <= counts[phase]; position++) {
				device_reset(device, fixture->prepared);
				fs = mount_file(device, &inode);
				device->reads = device->allocations = 0;
				device->fail_allocation = phase == 0 ? position : 0;
				device->fail_read = phase == 1 ? position : 0;
				EXPECT(grow(fs, fixture, operation != 0, &update, false),
				    phase == 0 ? EXT4_NO_MEMORY : EXT4_IO);
				device->fail_read = device->fail_allocation = 0;
				if (fs->aborted) {
					CHECK(phase == 1);
					ext4_unmount(fs);
					device_reset(device, device->stable);
					EXPECT(ext4_recover(
						   &device->environment, &device->writer, &report),
					    EXT4_OK);
					fs = mount_file(device, &inode);
				}
				CHECK(!check_state(device, fs, fixture, operation != 0, NULL));
				EXPECT(grow(fs, fixture, operation != 0, &update, false), EXT4_OK);
				CHECK(check_state(device, fs, fixture, operation != 0, NULL));
				ext4_unmount(fs);
				resource_cases++;
			}
		}
		for (position = 1; position <= counts[2]; position++) {
			for (torn = 0; torn < 2; torn++) {
				for (survival = 0; survival < 3; survival++) {
					device_reset(device, fixture->prepared);
					fs = mount_file(device, &inode);
					device->stop_at = position;
					device->partial = torn != 0;
					device->survival = survival;
					EXPECT(grow(fs, fixture, operation != 0, &update, false),
					    EXT4_IO);
					CHECK(fs->aborted && device->off &&
					    device->events == position);
					ext4_unmount(fs);
					device_reset(device, device->stable);
					error = ext4_recover(
					    &device->environment, &device->writer, &report);
					crash_cases++;
					if (error == EXT4_CORRUPT) {
						const struct ext4_super_disk *super =
						    (const struct ext4_super_disk *)(device->cache +
							EXT4_SUPER_OFFSET);

						CHECK(torn != 0 && survival != 0 &&
						    device->writes == 0 &&
						    device->metadata_checksum &&
						    ext4_le32(&super->checksum) !=
							ext4_crc32c(UINT32_MAX, super,
							    offsetof(
								struct ext4_super_disk, checksum)));
						damaged_superblocks++;
						continue;
					}
					EXPECT(error, EXT4_OK);
					fs = mount_file(device, &inode);
					if (!check_state(
						device, fs, fixture, operation != 0, NULL)) {
						EXPECT(grow(fs, fixture, operation != 0, &update,
							   false),
						    EXT4_OK);
						CHECK(check_state(
						    device, fs, fixture, operation != 0, NULL));
					}
					ext4_unmount(fs);
				}
			}
		}
	}
	printf("PASS preallocated growth faults: resource=%u crash=%u damaged-superblock=%u\n",
	    resource_cases, crash_cases, damaged_superblocks);
}

int
main(int argc, char **argv)
{
	struct device device;
	struct growth_fixture fixture;
	const char *exports = NULL;
	bool fault_mode = false;
	int index = 1;

	if (index < argc && strcmp(argv[index], "--faults") == 0) {
		fault_mode = true;
		index++;
	} else if (index < argc && strcmp(argv[index], "--export") == 0) {
		CHECK(index + 2 < argc);
		exports = argv[index + 1];
		index += 2;
	}
	CHECK(index < argc);
	for (; index < argc; index++) {
		storage_open(&device, argv[index]);
		prepare(&device, &fixture, fault_mode);
		if (fault_mode) {
			faults(&device, &fixture);
		} else {
			operations(&device, &fixture, exports, argv[index]);
		}
		free(fixture.prepared);
		storage_close(&device);
		printf("PASS preallocated growth: %s\n", argv[index]);
	}
	return 0;
}

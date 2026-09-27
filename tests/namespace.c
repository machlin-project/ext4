/* SPDX-License-Identifier: BSD-3-Clause */
#include "storage.h"

#define TEST_CREATE_FIELDS                                                                         \
	(EXT4_ATTR_PERMISSIONS | EXT4_ATTR_UID | EXT4_ATTR_GID | EXT4_ATTR_ACCESS_TIME |           \
	    EXT4_ATTR_MODIFY_TIME | EXT4_ATTR_CHANGE_TIME)
#define TEST_UID (UINT32_MAX - 2U)
#define TEST_GID 0x81234567U
#define TEST_SMALL_JOURNAL_CREDITS 4U

enum operation {
	CREATE_FILE,
	CREATE_DIRECTORY,
	CREATE_LINK,
	CREATE_SYMLINK_SHORT,
	CREATE_SYMLINK_LONG,
	CREATE_SYMLINK_MAXIMUM
};

struct trace {
	uint32_t allocations;
	uint32_t reads;
	uint32_t events;
	uint32_t commit_event;
	bool committed;
};

static struct ext4_fs *
mount_writer(struct device *device)
{
	struct ext4_fs *fs;

	EXPECT(ext4_mount_writable(&device->environment, &device->writer, &fs), EXT4_OK);
	return fs;
}

static struct ext4_inode
root_inode(struct ext4_fs *fs)
{
	struct ext4_inode root;

	EXPECT(ext4_get_inode(fs, EXT4_ROOT_INODE, &root), EXT4_OK);
	return root;
}

static struct ext4_inode
lookup(struct ext4_fs *fs, const struct ext4_inode *parent, const char *name)
{
	struct ext4_inode result;

	EXPECT(ext4_lookup(fs, parent, (const uint8_t *)name, strlen(name), &result), EXT4_OK);
	return result;
}

static struct ext4_inode_update
create_attributes(struct ext4_fs *fs)
{
	struct ext4_inode_update update;

	memset(&update, 0, sizeof(update));
	update.fields = TEST_CREATE_FIELDS;
	update.permissions = 02751;
	update.uid = TEST_UID;
	update.gid = TEST_GID;
	update.access_time.seconds = -1;
	update.modify_time.seconds = 1700000001;
	update.change_time.seconds = 1700000002;
	if (fs->inode_size > EXT4_INODE_BASE_SIZE) {
		update.fields |= EXT4_ATTR_BIRTH_TIME;
		update.birth_time.seconds = INT64_C(1) << 32;
		update.birth_time.nanoseconds = 987654321;
		update.access_time.nanoseconds = 123456789;
		update.modify_time.nanoseconds = 999999999;
		update.change_time.nanoseconds = 42;
	}
	return update;
}

static bool
equal_time(struct ext4_timestamp a, struct ext4_timestamp b)
{
	return a.seconds == b.seconds && a.nanoseconds == b.nanoseconds;
}

static void
check_created(struct ext4_fs *fs, const struct ext4_inode *inode, uint16_t mode)
{
	struct ext4_inode_update update = create_attributes(fs);

	if (mode == EXT4_MODE_SYMLINK) {
		update.permissions = 0777;
	}
	CHECK(inode->mode == (mode | update.permissions));
	CHECK(inode->uid == update.uid && inode->gid == update.gid && inode->generation != 0);
	CHECK(equal_time(inode->access_time, update.access_time));
	CHECK(equal_time(inode->modify_time, update.modify_time));
	CHECK(equal_time(inode->change_time, update.change_time));
	CHECK(inode->birth_time_valid == (fs->inode_size > EXT4_INODE_BASE_SIZE));
	if (inode->birth_time_valid) {
		CHECK(equal_time(inode->birth_time, update.birth_time));
	}
}

static void
symlink_contents(uint8_t *target, size_t length, bool binary)
{
	size_t index;

	for (index = 0; index < length; index++) {
		target[index] = index % 17 == 16 ? '/' : (uint8_t)('a' + index % 26);
		if (binary && index % 7 == 0) {
			target[index] = (uint8_t)(0x80U + index % 128);
		}
	}
}

static enum ext4_result
operate(struct ext4_fs *fs, const struct ext4_inode *parent, const struct ext4_inode *target,
    enum operation operation, struct ext4_inode *result)
{
	struct ext4_inode_update update = create_attributes(fs);
	const uint8_t *name = (const uint8_t *)"atomic-entry";
	uint8_t *link_target;
	size_t length = strlen((const char *)name);
	size_t link_length;
	enum ext4_result error;

	switch (operation) {
	case CREATE_FILE:
		return ext4_create(fs, parent->number, parent->generation, name, length, &update,
		    &update.change_time, result);
	case CREATE_DIRECTORY:
		return ext4_mkdir(fs, parent->number, parent->generation, name, length, &update,
		    &update.change_time, result);
	case CREATE_LINK:
		return ext4_link(fs, parent->number, parent->generation, name, length,
		    target->number, target->generation, &update.change_time, result);
	case CREATE_SYMLINK_SHORT:
	case CREATE_SYMLINK_LONG:
	case CREATE_SYMLINK_MAXIMUM:
		link_length = operation == CREATE_SYMLINK_MAXIMUM
		    ? fs->info.block_size - 1
		    : sizeof(result->block_data) - (operation == CREATE_SYMLINK_SHORT ? 1U : 0U);
		link_target = malloc(link_length);
		CHECK(link_target != NULL);
		symlink_contents(link_target, link_length, false);
		update.permissions = 0777;
		error = ext4_symlink(fs, parent->number, parent->generation, name, length,
		    link_target, link_length, &update, &update.change_time, result);
		free(link_target);
		return error;
	}
	return EXT4_INVALID_ARGUMENT;
}

static enum ext4_result
attempt(struct device *device, enum operation operation, unsigned int fault, uint32_t point,
    unsigned int survival, bool partial, struct trace *trace)
{
	struct ext4_fs *fs = mount_writer(device);
	struct ext4_inode parent = root_inode(fs);
	struct ext4_inode target = lookup(fs, &parent, "hello.txt");
	struct ext4_inode result;
	struct ext4_inode untouched;
	uint32_t allocations = device->allocations;
	uint32_t reads = device->reads;
	uint32_t events = device->events;
	enum ext4_result error;

	memset(&result, 0xa5, sizeof(result));
	untouched = result;
	device->survival = survival;
	device->partial = partial;
	if (fault == 1) {
		device->fail_allocation = allocations + point;
	} else if (fault == 2) {
		device->fail_read = reads + point;
	} else if (fault == 3) {
		device->stop_at = events + point;
	}
	error = operate(fs, &parent, &target, operation, &result);
	if (error == EXT4_OK) {
		error = ext4_sync(fs);
	} else {
		CHECK(memcmp(&result, &untouched, sizeof(result)) == 0);
	}
	trace->allocations = device->allocations - allocations;
	trace->reads = device->reads - reads;
	trace->events = device->events - events;
	trace->commit_event = device->commit_barrier;
	trace->committed = device->intent_durable;
	if (error != EXT4_OK && device->writes != 0) {
		CHECK(fs->aborted);
		EXPECT(ext4_get_inode(fs, parent.number, &result), EXT4_RECOVERY_REQUIRED);
	}
	ext4_unmount(fs);
	CHECK(device->live == 0);
	return error;
}

static void
fault_cases(struct device *device, enum operation operation, bool smoke, const char *exports,
    const char *path, const char *scenario)
{
	struct trace baseline;
	struct trace trace;
	uint8_t *expected = malloc(device->size);
	uint32_t point;
	uint32_t limit;
	uint32_t recovered = 0;
	uint32_t torn = 0;
	unsigned int fault;
	unsigned int survival;
	unsigned int partial;
	char prefix[32];

	CHECK(expected != NULL);
	device_reset(device, device->base);
	EXPECT(attempt(device, operation, 0, 0, 0, false, &baseline), EXT4_OK);
	memcpy(expected, device->stable, device->size);
	CHECK(snprintf(prefix, sizeof(prefix), "%satomic-%u-", scenario, operation) > 0);
	storage_export(device, exports, path, prefix);
	if (!smoke) {
		for (fault = 1; fault <= 2; fault++) {
			limit = fault == 1 ? baseline.allocations : baseline.reads;
			for (point = 1; point <= limit; point++) {
				device_reset(device, device->base);
				EXPECT(attempt(device, operation, fault, point, 0, false, &trace),
				    fault == 1 ? EXT4_NO_MEMORY : EXT4_IO);
				CHECK(storage_recover(device, expected, trace.committed));
			}
		}
		for (point = 1; point <= baseline.events; point++) {
			for (survival = 0; survival < 3; survival++) {
				for (partial = 0; partial < 2; partial++) {
					device_reset(device, device->base);
					EXPECT(attempt(device, operation, 3, point, survival,
						   partial != 0, &trace),
					    EXT4_IO);
					CHECK(device->off);
					if (storage_recover(device, expected, trace.committed)) {
						recovered++;
					} else {
						torn++;
					}
				}
			}
		}
		printf("PASS namespace faults scenario=%s operation=%u allocations=%u reads=%u "
		       "cuts=%u "
		       "recovered=%u torn_super_fail_closed=%u\n",
		    scenario, operation, baseline.allocations, baseline.reads, baseline.events * 6,
		    recovered, torn);
	}
	if (exports != NULL) {
		CHECK(baseline.commit_event > 1 && baseline.commit_event < baseline.events);
		device_reset(device, device->base);
		EXPECT(attempt(device, operation, 3, baseline.commit_event + 1, 0, false, &trace),
		    EXT4_IO);
		CHECK(trace.committed);
		CHECK(snprintf(prefix, sizeof(prefix), "%spending-%u-", scenario, operation) > 0);
		storage_export(device, exports, path, prefix);
		CHECK(storage_recover(device, expected, trace.committed));
		device_reset(device, device->base);
		EXPECT(attempt(device, operation, 3, baseline.commit_event - 1, 0, false, &trace),
		    EXT4_IO);
		CHECK(!trace.committed);
		CHECK(
		    snprintf(prefix, sizeof(prefix), "%suncommitted-%u-", scenario, operation) > 0);
		storage_export(device, exports, path, prefix);
		CHECK(storage_recover(device, expected, trace.committed));
	}
	free(expected);
}

static void
basic_cases(struct device *device, const char *exports, const char *path)
{
	struct ext4_fs *fs;
	struct ext4_inode root;
	struct ext4_inode before;
	struct ext4_inode file;
	struct ext4_inode directory;
	struct ext4_inode child;
	struct ext4_inode result;
	struct ext4_inode_update update;
	struct ext4_inode_update write_update;
	struct ext4_dir_entry entry;
	uint8_t bytes[37];
	uint8_t got[37];
	uint64_t cookie = 0;
	uint32_t free_inodes;
	size_t index;
	size_t completed;

	device_reset(device, device->base);
	fs = mount_writer(device);
	root = root_inode(fs);
	before = root;
	free_inodes = fs->info.free_inodes;
	update = create_attributes(fs);
	EXPECT(ext4_create(fs, root.number, root.generation, (const uint8_t *)"created", 7, &update,
		   &update.change_time, &file),
	    EXT4_OK);
	check_created(fs, &file, EXT4_MODE_REGULAR);
	CHECK(file.links == 1 && file.size == 0 && file.blocks_512 == 0);
	root = root_inode(fs);
	result = lookup(fs, &root, "created");
	CHECK(memcmp(&result, &file, sizeof(file)) == 0);
	CHECK(root.links == before.links && root.uid == before.uid && root.gid == before.gid &&
	    root.mode == before.mode && equal_time(root.access_time, before.access_time) &&
	    equal_time(root.change_time, update.change_time) &&
	    equal_time(root.modify_time, update.change_time));
	EXPECT(ext4_mkdir(fs, root.number, root.generation, (const uint8_t *)"created-dir", 11,
		   &update, &update.change_time, &directory),
	    EXT4_OK);
	check_created(fs, &directory, EXT4_MODE_DIRECTORY);
	CHECK(directory.links == 2 && directory.size == device->block_size &&
	    directory.blocks_512 == device->block_size / EXT4_SECTOR_SIZE);
	root = root_inode(fs);
	CHECK(root.links == before.links + 1 && fs->info.free_inodes == free_inodes - 2);
	result = lookup(fs, &directory, ".");
	CHECK(result.number == directory.number);
	result = lookup(fs, &directory, "..");
	CHECK(result.number == root.number);
	EXPECT(ext4_next_dir(fs, &directory, &cookie, &entry), EXT4_OK);
	CHECK(entry.inode == directory.number && strcmp((char *)entry.name, ".") == 0);
	EXPECT(ext4_next_dir(fs, &directory, &cookie, &entry), EXT4_OK);
	CHECK(entry.inode == root.number && strcmp((char *)entry.name, "..") == 0);
	EXPECT(ext4_next_dir(fs, &directory, &cookie, &entry), EXT4_NOT_FOUND);
	EXPECT(ext4_create(fs, directory.number, directory.generation, (const uint8_t *)"child", 5,
		   &update, &update.change_time, &child),
	    EXT4_OK);
	CHECK(child.number != file.number && child.number != directory.number);
	EXPECT(ext4_link(fs, directory.number, directory.generation, (const uint8_t *)"alias", 5,
		   file.number, file.generation, &update.change_time, &result),
	    EXT4_OK);
	CHECK(result.number == file.number && result.links == 2 && result.size == 0);
	CHECK(equal_time(result.modify_time, file.modify_time) && result.uid == file.uid &&
	    result.gid == file.gid && result.mode == file.mode);
	EXPECT(ext4_get_inode(fs, directory.number, &directory), EXT4_OK);
	result = lookup(fs, &directory, "alias");
	CHECK(result.number == file.number && result.links == 2);
	write_update = update;
	write_update.fields = EXT4_ATTR_PERMISSIONS | EXT4_ATTR_MODIFY_TIME | EXT4_ATTR_CHANGE_TIME;
	for (index = 0; index < sizeof(bytes); index++) {
		bytes[index] = (uint8_t)(index * 29 + 7);
	}
	EXPECT(ext4_write(fs, file.number, file.generation, device->block_size + 7, bytes,
		   sizeof(bytes), &write_update, &completed),
	    EXT4_OK);
	CHECK(completed == sizeof(bytes));
	result = lookup(fs, &directory, "alias");
	EXPECT(
	    ext4_read(fs, &result, device->block_size + 7, got, sizeof(got), &completed), EXT4_OK);
	CHECK(completed == sizeof(got) && memcmp(bytes, got, sizeof(got)) == 0);
	file = lookup(fs, &root, "hello-link");
	EXPECT(ext4_link(fs, root.number, root.generation, (const uint8_t *)"symlink-alias", 13,
		   file.number, file.generation, &update.change_time, &result),
	    EXT4_OK);
	CHECK(
	    result.number == file.number && result.links == file.links + 1 && result.fast_symlink);
	EXPECT(ext4_sync(fs), EXT4_OK);
	ext4_unmount(fs);
	storage_export(device, exports, path, "basic-");
	EXPECT(ext4_mount(&device->environment, &fs), EXT4_OK);
	root = root_inode(fs);
	directory = lookup(fs, &root, "created-dir");
	file = lookup(fs, &directory, "alias");
	CHECK(file.links == 2 && file.size == device->block_size + 7 + sizeof(bytes));
	ext4_unmount(fs);
	CHECK(device->live == 0);
}

static void
guards(struct device *device)
{
	struct ext4_fs *fs;
	struct ext4_inode root;
	struct ext4_inode file;
	struct ext4_inode result;
	struct ext4_inode untouched;
	struct ext4_inode_update update;
	const char *invalid[] = { "", ".", "..", "bad/name" };
	uint8_t long_name[EXT4_NAME_MAX + 1];
	size_t index;

	device_reset(device, device->base);
	fs = mount_writer(device);
	root = root_inode(fs);
	file = lookup(fs, &root, "hello.txt");
	update = create_attributes(fs);
	memset(&result, 0x5a, sizeof(result));
	untouched = result;
	for (index = 0; index < sizeof(invalid) / sizeof(invalid[0]); index++) {
		EXPECT(
		    ext4_create(fs, root.number, root.generation, (const uint8_t *)invalid[index],
			strlen(invalid[index]), &update, &update.change_time, &result),
		    EXT4_INVALID_ARGUMENT);
	}
	memset(long_name, 'n', sizeof(long_name));
	EXPECT(ext4_create(fs, root.number, root.generation, long_name, sizeof(long_name), &update,
		   &update.change_time, &result),
	    EXT4_NAME_TOO_LONG);
	EXPECT(ext4_create(fs, root.number, root.generation, (const uint8_t *)"bad\0name", 8,
		   &update, &update.change_time, &result),
	    EXT4_INVALID_ARGUMENT);
	EXPECT(ext4_create(fs, root.number, root.generation, (const uint8_t *)"hello.txt", 9,
		   &update, &update.change_time, &result),
	    EXT4_EXISTS);
	EXPECT(ext4_mkdir(fs, root.number, root.generation, (const uint8_t *)"hello.txt", 9,
		   &update, &update.change_time, &result),
	    EXT4_EXISTS);
	EXPECT(ext4_link(fs, root.number, root.generation, (const uint8_t *)"hello.txt", 9,
		   file.number, file.generation, &update.change_time, &result),
	    EXT4_EXISTS);
	EXPECT(ext4_create(fs, root.number, root.generation + 1, (const uint8_t *)"bad", 3, &update,
		   &update.change_time, &result),
	    EXT4_STALE);
	EXPECT(ext4_create(fs, file.number, file.generation, (const uint8_t *)"bad", 3, &update,
		   &update.change_time, &result),
	    EXT4_NOT_DIRECTORY);
	EXPECT(ext4_link(fs, root.number, root.generation, (const uint8_t *)"bad", 3, root.number,
		   root.generation, &update.change_time, &result),
	    EXT4_IS_DIRECTORY);
	EXPECT(ext4_link(fs, root.number, root.generation, (const uint8_t *)"bad", 3, file.number,
		   file.generation + 1, &update.change_time, &result),
	    EXT4_STALE);
	update.fields &= ~(uint32_t)EXT4_ATTR_UID;
	EXPECT(ext4_create(fs, root.number, root.generation, (const uint8_t *)"bad", 3, &update,
		   &update.change_time, &result),
	    EXT4_INVALID_ARGUMENT);
	update = create_attributes(fs);
	update.change_time.nanoseconds = EXT4_NANOSECONDS_PER_SECOND;
	EXPECT(ext4_create(fs, root.number, root.generation, (const uint8_t *)"bad", 3, &update,
		   &update.change_time, &result),
	    EXT4_RANGE);
	CHECK(memcmp(&result, &untouched, sizeof(result)) == 0);
	CHECK(device->writes == 0 && memcmp(device->cache, device->base, device->size) == 0);
	ext4_unmount(fs);
	EXPECT(ext4_mount(&device->environment, &fs), EXT4_OK);
	update = create_attributes(fs);
	EXPECT(ext4_create(fs, root.number, root.generation, (const uint8_t *)"bad", 3, &update,
		   &update.change_time, &result),
	    EXT4_READ_ONLY);
	ext4_unmount(fs);
	CHECK(device->live == 0);
}

static void
symlink_cases(struct device *device, const char *exports, const char *path)
{
	struct ext4_fs *fs;
	struct ext4_inode root;
	struct ext4_inode inode;
	struct ext4_inode alias;
	struct ext4_inode_update update;
	struct ext4_inode_update write_update;
	uint8_t *target;
	uint8_t *observed;
	uint64_t physical;
	uint32_t free_inodes;
	uint16_t root_links;
	size_t lengths[] = { 1, sizeof(inode.block_data) - 1, sizeof(inode.block_data),
		sizeof(inode.block_data) + 1, 0, 15 };
	size_t completed;
	size_t index;
	size_t byte;
	uint32_t writes;
	char name[32];
	char alias_name[32];
	int name_length;
	int alias_length;

	device_reset(device, device->base);
	fs = mount_writer(device);
	root = root_inode(fs);
	root_links = root.links;
	free_inodes = fs->info.free_inodes;
	update = create_attributes(fs);
	update.permissions = 0777;
	write_update = update;
	write_update.fields = EXT4_ATTR_PERMISSIONS | EXT4_ATTR_MODIFY_TIME | EXT4_ATTR_CHANGE_TIME;
	target = malloc(device->block_size);
	observed = malloc(device->block_size + 13);
	CHECK(target != NULL && observed != NULL);
	lengths[4] = device->block_size - 1;
	for (index = 0; index < sizeof(lengths) / sizeof(lengths[0]); index++) {
		symlink_contents(target, lengths[index], index == 5);
		name_length = snprintf(name, sizeof(name), "symbolic-%zu", index);
		alias_length =
		    snprintf(alias_name, sizeof(alias_name), "symbolic-alias-%zu", index);
		CHECK(name_length > 0 && (size_t)name_length < sizeof(name) && alias_length > 0 &&
		    (size_t)alias_length < sizeof(alias_name));
		EXPECT(ext4_symlink(fs, root.number, root.generation, (const uint8_t *)name,
			   (size_t)name_length, target, lengths[index], &update,
			   &update.change_time, &inode),
		    EXT4_OK);
		check_created(fs, &inode, EXT4_MODE_SYMLINK);
		CHECK(inode.size == lengths[index] && inode.links == 1);
		CHECK(inode.fast_symlink == (lengths[index] < sizeof(inode.block_data)));
		CHECK(inode.blocks_512 ==
		    (inode.fast_symlink ? 0 : device->block_size / EXT4_SECTOR_SIZE));
		if (inode.fast_symlink) {
			CHECK((inode.flags & EXT4_INODE_EXTENTS) == 0);
			for (byte = lengths[index]; byte < sizeof(inode.block_data); byte++) {
				CHECK(inode.block_data[byte] == 0);
			}
		} else {
			EXPECT(ext4_map_block(fs, &inode, 0, &physical), EXT4_OK);
			for (byte = lengths[index]; byte < device->block_size; byte++) {
				CHECK(device->cache[physical * device->block_size + byte] == 0);
			}
		}
		EXPECT(ext4_link(fs, root.number, root.generation, (const uint8_t *)alias_name,
			   (size_t)alias_length, inode.number, inode.generation,
			   &update.change_time, &alias),
		    EXT4_OK);
		CHECK(alias.number == inode.number && alias.links == 2 &&
		    alias.generation == inode.generation);
		memset(observed, 0xa5, device->block_size + 13);
		EXPECT(
		    ext4_read(fs, &alias, 0, observed, lengths[index] + 13, &completed), EXT4_OK);
		CHECK(completed == lengths[index] && memcmp(target, observed, completed) == 0 &&
		    observed[completed] == 0xa5);
		EXPECT(ext4_read(fs, &alias, lengths[index] - 1, observed, 3, &completed), EXT4_OK);
		CHECK(completed == 1 && observed[0] == target[lengths[index] - 1]);
		EXPECT(ext4_read(fs, &alias, lengths[index], observed, 1, &completed), EXT4_OK);
		CHECK(completed == 0);
		writes = device->writes;
		EXPECT(ext4_write(fs, inode.number, inode.generation, 0, target, 1, &write_update,
			   &completed),
		    EXT4_UNSUPPORTED);
		CHECK(completed == 0);
		EXPECT(ext4_truncate_atomic(
			   fs, inode.number, inode.generation, 0, &write_update, &alias),
		    EXT4_UNSUPPORTED);
		CHECK(device->writes == writes);
		root = root_inode(fs);
		CHECK(root.links == root_links && fs->info.free_inodes == free_inodes - index - 1);
	}
	EXPECT(ext4_sync(fs), EXT4_OK);
	ext4_unmount(fs);
	storage_export(device, exports, path, "symlinks-");
	EXPECT(ext4_mount(&device->environment, &fs), EXT4_OK);
	root = root_inode(fs);
	inode = lookup(fs, &root, "symbolic-4");
	EXPECT(ext4_read(fs, &inode, 0, observed, device->block_size, &completed), EXT4_OK);
	symlink_contents(target, lengths[4], false);
	CHECK(completed == lengths[4] && memcmp(target, observed, completed) == 0);
	ext4_unmount(fs);
	free(target);
	free(observed);
	printf("PASS symbolic link boundaries, opaque bytes, hardlink identity and read limits\n");
}

static void
inline_symlink_bounds(struct device *device)
{
	struct ext4_fs *fs;
	struct ext4_inode root;
	struct ext4_inode inode;
	struct ext4_inode result;
	struct ext4_inode untouched;
	struct ext4_inode_disk *disk;
	uint8_t bytes[sizeof(inode.block_data) + 1];
	uint64_t offset;
	size_t completed;
	size_t size;

	device_reset(device, device->base);
	EXPECT(ext4_mount(&device->environment, &fs), EXT4_OK);
	root = root_inode(fs);
	inode = lookup(fs, &root, "hello-link");
	CHECK(inode.fast_symlink);
	EXPECT(ext4_inode_location(fs, inode.number, &offset), EXT4_OK);
	disk = (struct ext4_inode_disk *)(device->cache + offset);
	memset(disk->block_data, 'x', sizeof(disk->block_data));
	disk->block_data[sizeof(disk->block_data) - 1] = 0;
	for (size = sizeof(disk->block_data) - 1; size <= sizeof(disk->block_data) + 1; size++) {
		ext4_encode32(&disk->size_lo, (uint32_t)size);
		ext4_inode_checksum_set(fs, inode.number, disk);
		memset(&result, 0xa5, sizeof(result));
		untouched = result;
		EXPECT(ext4_get_inode(fs, inode.number, &result),
		    size < sizeof(disk->block_data) ? EXT4_OK : EXT4_CORRUPT);
		if (size < sizeof(disk->block_data)) {
			memset(bytes, 0xa5, sizeof(bytes));
			EXPECT(
			    ext4_read(fs, &result, 0, bytes, sizeof(bytes), &completed), EXT4_OK);
			CHECK(completed == size && bytes[size] == 0xa5 &&
			    memcmp(bytes, disk->block_data, size) == 0);
		} else {
			CHECK(memcmp(&result, &untouched, sizeof(result)) == 0);
		}
	}
	CHECK(device->writes == 0);
	ext4_unmount(fs);
	printf("PASS inline symbolic link length validation with valid inode checksums\n");
}

static void
symlink_guards(struct device *device)
{
	struct ext4_fs *fs;
	struct ext4_inode root;
	struct ext4_inode result;
	struct ext4_inode untouched;
	struct ext4_inode_update update;
	const uint8_t *name = (const uint8_t *)"invalid-link";
	const uint8_t *targets[] = { NULL, (const uint8_t *)"", (const uint8_t *)"a\0b",
		(const uint8_t *)"a", (const uint8_t *)"a" };
	size_t lengths[] = { 1, 0, 3, 0, SIZE_MAX };
	size_t index;

	device_reset(device, device->base);
	fs = mount_writer(device);
	root = root_inode(fs);
	update = create_attributes(fs);
	update.permissions = 0777;
	lengths[3] = device->block_size;
	memset(&result, 0xa5, sizeof(result));
	untouched = result;
	for (index = 0; index < sizeof(lengths) / sizeof(lengths[0]); index++) {
		EXPECT(ext4_symlink(fs, root.number, root.generation, name, 12, targets[index],
			   lengths[index], &update, &update.change_time, &result),
		    index < 3 ? EXT4_INVALID_ARGUMENT : EXT4_NAME_TOO_LONG);
	}
	EXPECT(ext4_symlink(fs, root.number, root.generation, (const uint8_t *)"hello.txt", 9,
		   (const uint8_t *)"a", 1, &update, &update.change_time, &result),
	    EXT4_EXISTS);
	EXPECT(ext4_symlink(fs, root.number, root.generation + 1, name, 12, (const uint8_t *)"a", 1,
		   &update, &update.change_time, &result),
	    EXT4_STALE);
	CHECK(device->writes == 0 && memcmp(device->cache, device->base, device->size) == 0 &&
	    memcmp(&result, &untouched, sizeof(result)) == 0);
	ext4_unmount(fs);
	EXPECT(ext4_mount(&device->environment, &fs), EXT4_OK);
	EXPECT(ext4_symlink(fs, root.number, root.generation, name, 12, (const uint8_t *)"a", 1,
		   &update, &update.change_time, &result),
	    EXT4_READ_ONLY);
	ext4_unmount(fs);
}

static void
symlink_block_space(struct device *device)
{
	struct ext4_fs *fs;
	struct ext4_inode root;
	struct ext4_inode target;
	struct ext4_inode result;
	struct ext4_inode untouched;
	struct ext4_super_disk *super;
	uint8_t *before = malloc(device->size);
	uint64_t free_blocks;
	uint32_t free_inodes;

	CHECK(before != NULL);
	device_reset(device, device->base);
	fs = mount_writer(device);
	root = root_inode(fs);
	target = lookup(fs, &root, "hello.txt");
	free_blocks = fs->info.free_blocks;
	free_inodes = fs->info.free_inodes;
	super = (struct ext4_super_disk *)(device->cache + EXT4_SUPER_OFFSET);
	ext4_encode32(&super->reserved_blocks_lo, (uint32_t)free_blocks);
	ext4_encode32(&super->reserved_blocks_hi, (uint32_t)(free_blocks >> 32));
	if (device->metadata_checksum) {
		ext4_encode32(&super->checksum,
		    ext4_crc32c(UINT32_MAX, super, offsetof(struct ext4_super_disk, checksum)));
	}
	memcpy(before, device->cache, device->size);
	memset(&result, 0xa5, sizeof(result));
	untouched = result;
	EXPECT(operate(fs, &root, &target, CREATE_SYMLINK_LONG, &result), EXT4_NO_SPACE);
	EXPECT(operate(fs, &root, &target, CREATE_SYMLINK_MAXIMUM, &result), EXT4_NO_SPACE);
	CHECK(memcmp(&result, &untouched, sizeof(result)) == 0 && device->writes == 0 &&
	    memcmp(before, device->cache, device->size) == 0);
	CHECK(fs->info.free_blocks == free_blocks && fs->info.free_inodes == free_inodes);
	EXPECT(operate(fs, &root, &target, CREATE_SYMLINK_SHORT, &result), EXT4_OK);
	CHECK(result.fast_symlink && result.blocks_512 == 0);
	CHECK(fs->info.free_blocks == free_blocks && fs->info.free_inodes == free_inodes - 1);
	EXPECT(ext4_sync(fs), EXT4_OK);
	ext4_unmount(fs);
	free(before);
	printf("PASS symbolic link block exhaustion: inline creation needs no data block\n");
}

static void
inode_reuse_and_flags(struct device *device)
{
	static const uint32_t generations[] = { 1234567U, UINT32_MAX };

	static const enum operation operations[] = { CREATE_FILE, CREATE_DIRECTORY,
		CREATE_SYMLINK_SHORT, CREATE_SYMLINK_LONG };
	struct ext4_fs *fs;
	struct ext4_inode root;
	struct ext4_inode target;
	struct ext4_inode created;
	struct ext4_inode result;
	struct ext4_inode_disk *disk;
	uint64_t offset;
	uint32_t number;
	uint32_t flags;
	uint32_t expected;
	unsigned int index;
	unsigned int choice;
	enum operation operation;

	device_reset(device, device->base);
	fs = mount_writer(device);
	root = root_inode(fs);
	target = lookup(fs, &root, "hello.txt");
	EXPECT(operate(fs, &root, &target, CREATE_FILE, &created), EXT4_OK);
	number = created.number;
	ext4_unmount(fs);
	for (index = 0; index < sizeof(generations) / sizeof(generations[0]); index++) {
		for (choice = 0; choice < sizeof(operations) / sizeof(operations[0]); choice++) {
			operation = operations[choice];
			device_reset(device, device->base);
			fs = mount_writer(device);
			root = root_inode(fs);
			target = lookup(fs, &root, "hello.txt");
			EXPECT(ext4_inode_location(fs, number, &offset), EXT4_OK);
			disk = (struct ext4_inode_disk *)(device->cache + offset);
			/* Released records retain their old generation. New allocation must
			 * discard other stale bytes without resurrecting that identity. */
			memset(disk, 0xa5, fs->inode_size);
			ext4_encode32(&disk->generation, generations[index]);
			EXPECT(ext4_inode_location(fs, root.number, &offset), EXT4_OK);
			disk = (struct ext4_inode_disk *)(device->cache + offset);
			flags = EXT4_INODE_SYNC | EXT4_INODE_NODUMP | EXT4_INODE_NOATIME |
			    EXT4_INODE_NOTAIL | EXT4_INODE_JOURNAL_DATA;
			ext4_encode32(&disk->flags,
			    root.flags | flags | EXT4_INODE_DIRSYNC | EXT4_INODE_TOPDIR);
			ext4_inode_checksum_set(fs, root.number, disk);
			EXPECT(operate(fs, &root, &target, operation, &created), EXT4_OK);
			expected = generations[index] == UINT32_MAX ? 1 : generations[index] + 1;
			CHECK(created.number == number && created.generation == expected);
			check_created(fs, &created,
			    operation == CREATE_FILE		? EXT4_MODE_REGULAR
				: operation == CREATE_DIRECTORY ? EXT4_MODE_DIRECTORY
								: EXT4_MODE_SYMLINK);
			if (operation >= CREATE_SYMLINK_SHORT) {
				flags &= EXT4_INODE_NODUMP | EXT4_INODE_NOATIME;
			}
			if ((fs->info.feature_incompat & EXT4_FEATURE_INCOMPAT_EXTENTS) &&
			    operation != CREATE_SYMLINK_SHORT) {
				flags |= EXT4_INODE_EXTENTS;
			}
			if (operation == CREATE_DIRECTORY) {
				flags |= EXT4_INODE_DIRSYNC;
			}
			CHECK(created.flags == flags);
			EXPECT(ext4_link(fs, root.number, root.generation,
				   (const uint8_t *)"stale-reuse", 11, number, generations[index],
				   &created.change_time, &result),
			    EXT4_STALE);
			EXPECT(ext4_sync(fs), EXT4_OK);
			ext4_unmount(fs);
			EXPECT(ext4_mount(&device->environment, &fs), EXT4_OK);
			EXPECT(ext4_get_inode(fs, number, &result), EXT4_OK);
			CHECK(memcmp(&result, &created, sizeof(result)) == 0);
			ext4_unmount(fs);
		}
	}
	printf("PASS inode reuse: stale identities, generation wrap, cleared records and inherited "
	       "flags\n");
}

static void
credit_exhaustion(struct device *device)
{
	struct ext4_fs *fs;
	struct ext4_inode root;
	struct ext4_inode target;
	struct ext4_inode result;
	struct ext4_inode untouched;
	struct ext4_inode journal;
	struct ext4_jbd_super *super;
	uint8_t *before = malloc(device->size);
	uint64_t physical;
	uint32_t free_inodes;
	uint64_t free_blocks;
	enum operation operation;

	CHECK(before != NULL);
	for (operation = CREATE_FILE; operation <= CREATE_SYMLINK_MAXIMUM; operation++) {
		if (operation == CREATE_LINK) {
			continue;
		}
		device_reset(device, device->base);
		EXPECT(ext4_mount(&device->environment, &fs), EXT4_OK);
		EXPECT(ext4_get_inode(fs, fs->journal_inode, &journal), EXT4_OK);
		EXPECT(ext4_map_block(fs, &journal, 0, &physical), EXT4_OK);
		ext4_unmount(fs);
		super = (struct ext4_jbd_super *)(device->cache + physical * device->block_size);
		/* An admitted but small journal permits four metadata snapshots. Inode
		 * allocation must cancel even if it runs out after private edits. */
		ext4_encode_be32(&super->max_length,
		    ext4_be32(&super->first) + 2 * TEST_SMALL_JOURNAL_CREDITS + 2);
		if (ext4_be32(&super->feature_incompat) & (EXT4_JBD_CSUM_V2 | EXT4_JBD_CSUM_V3)) {
			ext4_encode_be32(&super->checksum, 0);
			ext4_encode_be32(
			    &super->checksum, ext4_crc32c(UINT32_MAX, super, sizeof(*super)));
		}
		memcpy(before, device->cache, device->size);
		fs = mount_writer(device);
		CHECK(ext4_journal_credits(fs->journal) == TEST_SMALL_JOURNAL_CREDITS);
		free_inodes = fs->info.free_inodes;
		free_blocks = fs->info.free_blocks;
		root = root_inode(fs);
		target = lookup(fs, &root, "hello.txt");
		memset(&result, 0xa5, sizeof(result));
		untouched = result;
		EXPECT(operate(fs, &root, &target, operation, &result), EXT4_RANGE);
		CHECK(memcmp(&result, &untouched, sizeof(result)) == 0 && !fs->aborted);
		CHECK(device->writes == 0 && memcmp(before, device->cache, device->size) == 0);
		CHECK(fs->info.free_inodes == free_inodes && fs->info.free_blocks == free_blocks);
		ext4_unmount(fs);
	}
	free(before);
	printf("PASS namespace journal credit exhaustion without writes or leaked allocation\n");
}

enum malformed_case {
	DIRECTORY_CHECKSUM,
	DIRECTORY_TAIL,
	DOT_INODE,
	DOTDOT_INODE,
	RECORD_ALIGNMENT,
	RECORD_OVERRUN,
	NAME_NUL,
	NAME_SLASH,
	ENTRY_NUMBER,
	DIRECTORY_HOLE,
	INDEXED_PARENT,
	IMMUTABLE_PARENT,
	IMMUTABLE_TARGET,
	TARGET_LINK_LIMIT,
	PARENT_LINK_LIMIT,
	INODE_BITMAP_CHECKSUM,
	INODE_BITMAP_TAIL,
	FREE_RESERVED_INODE,
	INODE_BITMAP_COUNT,
	INODE_HIGH_WATER,
	RESERVED_BLOCK_POOL,
	MALFORMED_COUNT
};

static void
malformed(struct device *device)
{
	struct ext4_fs *fs;
	struct ext4_inode root;
	struct ext4_inode target;
	struct ext4_inode result;
	struct ext4_inode untouched;
	struct ext4_inode_disk *parent_disk;
	struct ext4_inode_disk *target_disk;
	struct ext4_dir_header_disk *entry;
	struct ext4_group group;
	struct ext4_group_disk *descriptor;
	struct ext4_super_disk *super;
	struct ext4_extent_disk *extent;
	uint8_t *directory;
	uint8_t *bitmap;
	uint8_t *before = malloc(device->size);
	uint64_t offset;
	uint64_t physical;
	uint32_t checksum;
	uint32_t index;
	uint32_t skipped = 0;
	enum operation operation;
	enum ext4_result expected;

	CHECK(before != NULL);
	for (index = 0; index < MALFORMED_COUNT; index++) {
		if (!device->metadata_checksum &&
		    (index == DIRECTORY_CHECKSUM || index == DIRECTORY_TAIL ||
			index == INODE_BITMAP_CHECKSUM)) {
			skipped++;
			continue;
		}
		device_reset(device, device->base);
		fs = mount_writer(device);
		root = root_inode(fs);
		target = lookup(fs, &root, "hello.txt");
		EXPECT(ext4_inode_location(fs, root.number, &offset), EXT4_OK);
		parent_disk = (struct ext4_inode_disk *)(device->cache + offset);
		EXPECT(ext4_inode_location(fs, target.number, &offset), EXT4_OK);
		target_disk = (struct ext4_inode_disk *)(device->cache + offset);
		EXPECT(ext4_map_block(fs, &root, 0, &physical), EXT4_OK);
		directory = device->cache + physical * device->block_size;
		EXPECT(ext4_group_get(fs, 0, &group), EXT4_OK);
		descriptor = (struct ext4_group_disk *)(device->cache +
		    (uint64_t)(fs->first_data_block + 1) * device->block_size);
		bitmap = device->cache + group.inode_bitmap * device->block_size;
		super = (struct ext4_super_disk *)(device->cache + EXT4_SUPER_OFFSET);
		entry = (struct ext4_dir_header_disk *)directory;
		operation = CREATE_FILE;
		expected = EXT4_CORRUPT;
		switch ((enum malformed_case)index) {
		case DIRECTORY_CHECKSUM:
			directory[device->block_size - 1] ^= 1;
			break;
		case DIRECTORY_TAIL:
			directory[device->block_size - sizeof(struct ext4_dir_tail_disk)] = 1;
			break;
		case DOT_INODE:
			ext4_encode32(&entry->inode, target.number);
			break;
		case DOTDOT_INODE:
			entry = (struct ext4_dir_header_disk *)(directory +
			    ext4_directory_record_length(fs, entry));
			ext4_encode32(&entry->inode, 0);
			break;
		case RECORD_ALIGNMENT:
			ext4_encode16(&entry->record_length, ext4_le16(&entry->record_length) - 1);
			break;
		case RECORD_OVERRUN:
			ext4_encode16(&entry->record_length,
			    (uint16_t)(device->block_size + EXT4_DIRECTORY_ALIGNMENT));
			break;
		case NAME_NUL:
			directory[sizeof(*entry)] = 0;
			break;
		case NAME_SLASH:
			directory[sizeof(*entry)] = '/';
			break;
		case ENTRY_NUMBER:
			ext4_encode32(&entry->inode, fs->info.inodes + 1);
			break;
		case DIRECTORY_HOLE:
			if (root.flags & EXT4_INODE_EXTENTS) {
				extent = (struct ext4_extent_disk *)(parent_disk->block_data +
				    sizeof(struct ext4_extent_header_disk));
				ext4_encode32(&extent->physical_lo, 0);
				ext4_encode16(&extent->physical_hi, 0);
			} else {
				ext4_encode32((struct ext4_le32 *)parent_disk->block_data, 0);
			}
			break;
		case INDEXED_PARENT:
			ext4_encode32(&parent_disk->flags, root.flags | EXT4_INODE_INDEX);
			expected = EXT4_UNSUPPORTED;
			break;
		case IMMUTABLE_PARENT:
			ext4_encode32(&parent_disk->flags, root.flags | EXT4_INODE_IMMUTABLE);
			expected = EXT4_UNSUPPORTED;
			break;
		case IMMUTABLE_TARGET:
			ext4_encode32(&target_disk->flags, target.flags | EXT4_INODE_IMMUTABLE);
			operation = CREATE_LINK;
			expected = EXT4_UNSUPPORTED;
			break;
		case TARGET_LINK_LIMIT:
			ext4_encode16(&target_disk->links, EXT4_LINK_MAX);
			operation = CREATE_LINK;
			expected = EXT4_TOO_MANY_LINKS;
			break;
		case PARENT_LINK_LIMIT:
			ext4_encode16(&parent_disk->links, EXT4_LINK_MAX);
			operation = CREATE_DIRECTORY;
			expected = EXT4_TOO_MANY_LINKS;
			break;
		case INODE_BITMAP_CHECKSUM:
			bitmap[fs->inodes_per_group / EXT4_BITS_PER_BYTE - 1] ^= 1;
			break;
		case INODE_BITMAP_TAIL:
			CHECK(fs->inodes_per_group < device->block_size * EXT4_BITS_PER_BYTE);
			bitmap[device->block_size - 1] &= 0x7fU;
			break;
		case FREE_RESERVED_INODE:
			bitmap[0] &= 0xfeU;
			break;
		case INODE_BITMAP_COUNT:
			ext4_encode16(
			    &descriptor->free_inodes_lo, (uint16_t)(group.free_inodes + 1));
			break;
		case INODE_HIGH_WATER:
			ext4_encode16(
			    &descriptor->unused_inodes_lo, (uint16_t)fs->inodes_per_group);
			break;
		case RESERVED_BLOCK_POOL:
			ext4_encode32(&super->reserved_blocks_lo, (uint32_t)fs->info.free_blocks);
			ext4_encode32(
			    &super->reserved_blocks_hi, (uint32_t)(fs->info.free_blocks >> 32));
			operation = CREATE_DIRECTORY;
			expected = EXT4_NO_SPACE;
			break;
		case MALFORMED_COUNT:
			CHECK(false);
		}
		if (device->metadata_checksum) {
			if (index != DIRECTORY_CHECKSUM) {
				ext4_encode32(
				    &((struct ext4_dir_tail_disk *)(directory + device->block_size -
					  sizeof(struct ext4_dir_tail_disk)))
					->checksum,
				    ext4_crc32c(ext4_inode_seed(fs, &root), directory,
					device->block_size - sizeof(struct ext4_dir_tail_disk)));
			}
			if (index != INODE_BITMAP_CHECKSUM) {
				checksum = ext4_crc32c(fs->checksum_seed, bitmap,
				    fs->inodes_per_group / EXT4_BITS_PER_BYTE);
				ext4_encode16(
				    &descriptor->inode_bitmap_checksum_lo, (uint16_t)checksum);
				if (fs->descriptor_size >= EXT4_GROUP_64_SIZE) {
					ext4_encode16(&descriptor->inode_bitmap_checksum_hi,
					    (uint16_t)(checksum >> 16));
				}
			}
			ext4_encode32(&super->checksum,
			    ext4_crc32c(
				UINT32_MAX, super, offsetof(struct ext4_super_disk, checksum)));
		}
		ext4_group_checksum_set(fs, 0, descriptor);
		ext4_inode_checksum_set(fs, root.number, parent_disk);
		ext4_inode_checksum_set(fs, target.number, target_disk);
		memcpy(before, device->cache, device->size);
		memset(&result, 0xa5, sizeof(result));
		untouched = result;
		printf("CHECK namespace malformed case=%u\n", index);
		EXPECT(operate(fs, &root, &target, operation, &result), expected);
		CHECK(device->writes == 0 && memcmp(before, device->cache, device->size) == 0 &&
		    memcmp(&result, &untouched, sizeof(result)) == 0);
		ext4_unmount(fs);
		CHECK(device->live == 0);
	}
	printf("PASS namespace malformed cases=%u; SKIP checksum-absent cases=%u\n",
	    MALFORMED_COUNT - skipped, skipped);
	free(before);
}

static void
append_fixture(struct device *device)
{
	struct ext4_fs *fs;
	struct ext4_inode root;
	struct ext4_inode target;
	struct ext4_inode result;
	struct ext4_inode_update update;
	uint8_t *before = malloc(device->size);
	char name[EXT4_NAME_MAX + 1];
	char prefix[32];
	uint64_t size;
	uint32_t index;
	unsigned int phase;
	size_t length;
	int written;

	CHECK(before != NULL);
	device_reset(device, device->base);
	for (phase = 0; phase < 2; phase++) {
		fs = mount_writer(device);
		root = root_inode(fs);
		target = lookup(fs, &root, "hello.txt");
		update = create_attributes(fs);
		size = root.size;
		for (index = 0;; index++) {
			CHECK(index < device->block_size);
			EXPECT(ext4_sync(fs), EXT4_OK);
			memcpy(before, device->stable, device->size);
			length = phase == 0 ? EXT4_NAME_MAX : strlen("atomic-entry");
			memset(name, 'p', length);
			written = snprintf(prefix, sizeof(prefix), "f%u-%08u", phase, index);
			CHECK(written > 0 && (size_t)written <= length);
			memcpy(name, prefix, (size_t)written);
			name[length] = 0;
			EXPECT(ext4_link(fs, root.number, root.generation, (const uint8_t *)name,
				   length, target.number, target.generation, &update.change_time,
				   &result),
			    EXT4_OK);
			root = root_inode(fs);
			if (root.size > size) {
				break;
			}
		}
		ext4_unmount(fs);
		device_reset(device, before);
	}
	memcpy(device->base, device->stable, device->size);
	free(before);
}

static void
exhaustion(struct device *device, const char *exports, const char *path)
{
	struct ext4_fs *fs;
	struct ext4_inode root;
	struct ext4_inode result;
	struct ext4_inode found;
	struct ext4_inode target;
	struct ext4_inode_update update;
	struct ext4_group group;
	uint8_t *before = malloc(device->size);
	uint8_t *seen;
	char name[EXT4_NAME_MAX + 1];
	char prefix[32];
	uint32_t free_inodes;
	uint32_t initial_links;
	uint32_t directories = 0;
	uint32_t lazy = 0;
	uint32_t index;
	uint32_t writes;
	int written;

	CHECK(before != NULL);
	device_reset(device, device->base);
	fs = mount_writer(device);
	CHECK(fs->info.inodes <= 256 && fs->info.groups > 1);
	seen = calloc(fs->info.inodes + 1, 1);
	CHECK(seen != NULL);
	root = root_inode(fs);
	target = lookup(fs, &root, "hello.txt");
	initial_links = root.links;
	free_inodes = fs->info.free_inodes;
	update = create_attributes(fs);
	for (index = 0; index < fs->info.groups; index++) {
		EXPECT(ext4_group_get(fs, index, &group), EXT4_OK);
		lazy += (group.flags & EXT4_GROUP_INODE_UNINIT) != 0;
	}
	for (index = 0; index < free_inodes; index++) {
		memset(name, 'n', EXT4_NAME_MAX);
		written = snprintf(prefix, sizeof(prefix), "node-%08u", index);
		CHECK(written > 0);
		memcpy(name, prefix, (size_t)written);
		name[EXT4_NAME_MAX] = 0;
		if (index % 5 == 0) {
			EXPECT(ext4_mkdir(fs, root.number, root.generation, (const uint8_t *)name,
				   EXT4_NAME_MAX, &update, &update.change_time, &result),
			    EXT4_OK);
			directories++;
		} else {
			EXPECT(ext4_create(fs, root.number, root.generation, (const uint8_t *)name,
				   EXT4_NAME_MAX, &update, &update.change_time, &result),
			    EXT4_OK);
		}
		CHECK(result.number <= fs->info.inodes && !seen[result.number]);
		seen[result.number] = 1;
		root = root_inode(fs);
		found = lookup(fs, &root, name);
		CHECK(found.number == result.number && found.generation == result.generation);
		CHECK(fs->info.free_inodes == free_inodes - index - 1);
	}
	CHECK(root.links == initial_links + directories);
	CHECK(root.size > EXT4_DIRECT_BLOCKS * device->block_size);
	for (index = 0; index < fs->info.groups; index++) {
		EXPECT(ext4_group_get(fs, index, &group), EXT4_OK);
		CHECK(group.free_inodes == 0 && !(group.flags & EXT4_GROUP_INODE_UNINIT));
	}
	writes = device->writes;
	memcpy(before, device->cache, device->size);
	EXPECT(ext4_create(fs, root.number, root.generation, (const uint8_t *)"full", 4, &update,
		   &update.change_time, &result),
	    EXT4_NO_SPACE);
	EXPECT(ext4_mkdir(fs, root.number, root.generation, (const uint8_t *)"full", 4, &update,
		   &update.change_time, &result),
	    EXT4_NO_SPACE);
	EXPECT(operate(fs, &root, &target, CREATE_SYMLINK_SHORT, &result), EXT4_NO_SPACE);
	EXPECT(operate(fs, &root, &target, CREATE_SYMLINK_LONG, &result), EXT4_NO_SPACE);
	CHECK(writes == device->writes && memcmp(before, device->cache, device->size) == 0);
	EXPECT(ext4_link(fs, root.number, root.generation, (const uint8_t *)"still-links", 11,
		   target.number, target.generation, &update.change_time, &result),
	    EXT4_OK);
	CHECK(fs->info.free_inodes == 0);
	EXPECT(ext4_sync(fs), EXT4_OK);
	ext4_unmount(fs);
	storage_export(device, exports, path, "exhaust-");
	printf("PASS inode exhaustion: created=%u directories=%u lazy_groups=%u\n", free_inodes,
	    directories, lazy);
	free(seen);
	free(before);
}

static void
group_fixture(struct device *device)
{
	struct ext4_fs *fs;
	struct ext4_inode root;
	struct ext4_inode result;
	struct ext4_inode_update update;
	struct ext4_group group;
	uint32_t count;
	uint32_t index;
	char name[32];
	int length;

	device_reset(device, device->base);
	fs = mount_writer(device);
	CHECK(fs->info.inodes <= 256 && fs->info.groups > 1);
	root = root_inode(fs);
	update = create_attributes(fs);
	EXPECT(ext4_group_get(fs, 0, &group), EXT4_OK);
	count = group.free_inodes;
	for (index = 0; index < count; index++) {
		length = snprintf(name, sizeof(name), "group-fill-%u", index);
		CHECK(length > 0 && (size_t)length < sizeof(name));
		EXPECT(ext4_create(fs, root.number, root.generation, (const uint8_t *)name,
			   (size_t)length, &update, &update.change_time, &result),
		    EXT4_OK);
		CHECK(result.number <= fs->inodes_per_group);
	}
	EXPECT(ext4_group_get(fs, 0, &group), EXT4_OK);
	CHECK(group.free_inodes == 0);
	EXPECT(ext4_sync(fs), EXT4_OK);
	ext4_unmount(fs);
	memcpy(device->base, device->stable, device->size);
}

static void
indexed_guard(struct device *device)
{
	struct ext4_fs *fs = mount_writer(device);
	struct ext4_inode root = root_inode(fs);
	struct ext4_inode directory = lookup(fs, &root, "many");
	struct ext4_inode target = lookup(fs, &root, "hello.txt");
	struct ext4_inode result;
	struct ext4_inode untouched;
	unsigned int operation;

	CHECK(directory.flags & EXT4_INODE_INDEX);
	memset(&result, 0xa5, sizeof(result));
	untouched = result;
	for (operation = CREATE_FILE; operation <= CREATE_SYMLINK_MAXIMUM; operation++) {
		EXPECT(operate(fs, &directory, &target, (enum operation)operation, &result),
		    EXT4_UNSUPPORTED);
		CHECK(memcmp(&result, &untouched, sizeof(result)) == 0);
	}
	CHECK(device->writes == 0 && memcmp(device->base, device->cache, device->size) == 0);
	ext4_unmount(fs);
	printf("PASS indexed namespace mutations rejected without writes\n");
}

int
main(int argc, char **argv)
{
	struct device device;
	const char *exports = NULL;
	bool smoke = false;
	bool exhaust = false;
	bool groups = false;
	bool indexed = false;
	bool symlinks = false;
	int argument = 1;
	unsigned int operation;

	while (argument < argc && argv[argument][0] == '-') {
		if (strcmp(argv[argument], "--smoke") == 0) {
			smoke = true;
		} else if (strcmp(argv[argument], "--exhaust") == 0) {
			exhaust = true;
		} else if (strcmp(argv[argument], "--groups") == 0) {
			groups = true;
		} else if (strcmp(argv[argument], "--indexed") == 0) {
			indexed = true;
		} else if (strcmp(argv[argument], "--symlinks") == 0) {
			symlinks = true;
		} else if (strcmp(argv[argument], "--export") == 0 && argument + 1 < argc) {
			exports = argv[++argument];
		} else {
			fprintf(stderr, "Unknown or incomplete option: %s\n", argv[argument]);
			return 2;
		}
		argument++;
	}
	CHECK(argument < argc);
	for (; argument < argc; argument++) {
		storage_open(&device, argv[argument]);
		if (indexed) {
			indexed_guard(&device);
			storage_close(&device);
			continue;
		}
		if (exhaust) {
			exhaustion(&device, exports, argv[argument]);
			storage_close(&device);
			continue;
		}
		if (groups) {
			group_fixture(&device);
			storage_export(&device, exports, argv[argument], "group-before-");
			for (operation = symlinks ? CREATE_SYMLINK_SHORT : CREATE_FILE;
			    operation <= (symlinks ? CREATE_SYMLINK_MAXIMUM : CREATE_DIRECTORY);
			    operation++) {
				fault_cases(&device, (enum operation)operation, smoke, exports,
				    argv[argument], "group-");
			}
			storage_close(&device);
			continue;
		}
		if (symlinks) {
			inline_symlink_bounds(&device);
			symlink_guards(&device);
			symlink_block_space(&device);
			symlink_cases(&device, exports, argv[argument]);
			for (operation = CREATE_SYMLINK_SHORT; operation <= CREATE_SYMLINK_MAXIMUM;
			    operation++) {
				fault_cases(&device, (enum operation)operation, smoke, exports,
				    argv[argument], "");
			}
			append_fixture(&device);
			storage_export(&device, exports, argv[argument], "append-before-");
			for (operation = CREATE_SYMLINK_SHORT; operation <= CREATE_SYMLINK_MAXIMUM;
			    operation++) {
				fault_cases(&device, (enum operation)operation, smoke, exports,
				    argv[argument], "append-");
			}
			storage_close(&device);
			printf("PASS symlinks: %s\n", argv[argument]);
			continue;
		}
		guards(&device);
		inode_reuse_and_flags(&device);
		credit_exhaustion(&device);
		malformed(&device);
		basic_cases(&device, exports, argv[argument]);
		for (operation = CREATE_FILE; operation <= CREATE_LINK; operation++) {
			fault_cases(
			    &device, (enum operation)operation, smoke, exports, argv[argument], "");
		}
		append_fixture(&device);
		storage_export(&device, exports, argv[argument], "append-before-");
		for (operation = CREATE_FILE; operation <= CREATE_LINK; operation++) {
			fault_cases(&device, (enum operation)operation, smoke, exports,
			    argv[argument], "append-");
		}
		storage_close(&device);
		printf("PASS namespace: %s\n", argv[argument]);
	}
	return 0;
}

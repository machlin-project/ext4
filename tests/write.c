/* SPDX-License-Identifier: BSD-3-Clause */
#include "journal.h"
#include "image.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TEST_IMAGE_LIMIT (128U * 1024U * 1024U)
#define TEST_PAYLOAD_SIZE 200000U
#define TEST_SECTOR_SIZE 512U
#define TEST_UNKNOWN_INODE_FLAG 0x80000000U
#define TEST_UNKNOWN_ATTRIBUTE (1U << 31)
#define TEST_WIDE_UID (UINT32_MAX - 1U)
#define TEST_WRITE_FIELDS (EXT4_ATTR_PERMISSIONS | EXT4_ATTR_MODIFY_TIME | EXT4_ATTR_CHANGE_TIME)

#define CHECK(expression)                                                                          \
	do {                                                                                       \
		if (!(expression)) {                                                               \
			fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #expression);           \
			exit(1);                                                                   \
		}                                                                                  \
	} while (0)
#define EXPECT(expression, expected)                                                               \
	do {                                                                                       \
		enum ext4_result actual = (expression);                                            \
		if (actual != (expected)) {                                                        \
			fprintf(stderr, "%s:%d: %s: %s, expected %s\n", __FILE__, __LINE__,        \
			    #expression, ext4_result_string(actual),                               \
			    ext4_result_string(expected));                                         \
			exit(1);                                                                   \
		}                                                                                  \
	} while (0)

struct device {
	struct ext4_environment environment;
	struct ext4_write_environment writer;
	uint8_t *base;
	uint8_t *cache;
	uint8_t *stable;
	uint8_t *dirty;
	size_t size;
	uint32_t block_size;
	uint32_t blocks;
	uint32_t events;
	uint32_t stop_at;
	uint32_t writes;
	uint32_t reads;
	uint32_t allocations;
	uint32_t fail_read;
	uint32_t fail_allocation;
	uint32_t live;
	uint64_t operation_offset;
	unsigned int survival;
	bool partial;
	bool off;
	bool truncate;
	const char *source_path;
	const char *export_directory;
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
	if (buffer != NULL) {
		device->live++;
	}
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
	if (++device->reads == device->fail_read || device->off) {
		return EXT4_IO;
	}
	memcpy(buffer, device->cache + offset, length);
	return EXT4_OK;
}

static void
device_persist(struct device *device, unsigned int survival)
{
	uint32_t index;

	for (index = 0; index < device->blocks; index++) {
		if (device->dirty[index] && (survival == 1 || (survival == 2 && (index & 1)))) {
			memcpy(device->stable + (size_t)index * device->block_size,
			    device->cache + (size_t)index * device->block_size, device->block_size);
		}
		device->dirty[index] = 0;
	}
}

static enum ext4_result
device_write(void *context, uint64_t offset, const void *buffer, size_t length)
{
	struct device *device = context;
	size_t partial;

	CHECK(!device->off && offset % device->block_size == 0 && length == device->block_size);
	CHECK(offset <= device->size && length <= device->size - offset);
	device->writes++;
	if (++device->events == device->stop_at) {
		if (device->partial) {
			partial = length / 2;
			if (offset ==
			    (EXT4_SUPER_OFFSET / device->block_size) * device->block_size) {
				partial = EXT4_SUPER_OFFSET % device->block_size + TEST_SECTOR_SIZE;
			}
			memcpy(device->cache + offset, buffer, partial);
			device->dirty[offset / device->block_size] = 1;
		}
		device_persist(device, device->survival);
		device->off = true;
		return EXT4_IO;
	}
	memcpy(device->cache + offset, buffer, length);
	device->dirty[offset / device->block_size] = 1;
	return EXT4_OK;
}

static enum ext4_result
device_flush(void *context)
{
	struct device *device = context;
	bool stop;

	CHECK(!device->off);
	stop = ++device->events == device->stop_at;
	device_persist(device, stop ? device->survival : 1);
	if (stop) {
		device->off = true;
		return EXT4_IO;
	}
	return EXT4_OK;
}

static void
device_reset(struct device *device, const uint8_t *source)
{
	CHECK(device->live == 0);
	memcpy(device->cache, source, device->size);
	if (source != device->stable) {
		memcpy(device->stable, source, device->size);
	}
	memset(device->dirty, 0, device->blocks);
	device->events = device->stop_at = device->writes = device->reads = 0;
	device->allocations = device->fail_read = device->fail_allocation = 0;
	device->off = false;
	device->partial = false;
}

static struct ext4_fs *
mount_writer(struct device *device)
{
	struct ext4_fs *fs;

	EXPECT(ext4_mount_writable(&device->environment, &device->writer, &fs), EXT4_OK);
	return fs;
}

static struct ext4_inode
lookup(struct ext4_fs *fs, const char *name)
{
	struct ext4_inode root;
	struct ext4_inode inode;

	EXPECT(ext4_get_inode(fs, EXT4_ROOT_INODE, &root), EXT4_OK);
	EXPECT(ext4_lookup(fs, &root, (const uint8_t *)name, strlen(name), &inode), EXT4_OK);
	return inode;
}

static struct ext4_inode_update
write_update(struct ext4_fs *fs)
{
	struct ext4_inode_update update;

	memset(&update, 0, sizeof(update));
	update.fields = TEST_WRITE_FIELDS;
	update.permissions = 0640;
	update.modify_time.seconds =
	    fs->inode_size == EXT4_INODE_BASE_SIZE ? 1700000001 : INT32_MIN;
	update.change_time.seconds = fs->inode_size == EXT4_INODE_BASE_SIZE
	    ? 1700000002
	    : INT32_MAX + ((int64_t)EXT4_TIME_EPOCH_MASK << 32);
	if (fs->inode_size > EXT4_INODE_BASE_SIZE) {
		update.modify_time.nanoseconds = 123456789;
		update.change_time.nanoseconds = 999999999;
	}
	return update;
}

static uint8_t *
write_bytes(struct device *device, size_t *length)
{
	uint8_t *bytes;
	size_t index;

	*length = device->block_size + 23;
	bytes = malloc(*length);
	CHECK(bytes != NULL);
	for (index = 0; index < *length; index++) {
		bytes[index] = (uint8_t)(index * 29 + 7);
	}
	return bytes;
}

static void
check_payload(struct ext4_fs *fs, const uint8_t *patch, size_t patch_length)
{
	struct ext4_inode inode = lookup(fs, "payload.bin");
	uint8_t *bytes = malloc(TEST_PAYLOAD_SIZE);
	size_t completed;
	size_t index;
	size_t start = fs->info.block_size - 7;
	uint8_t expected;

	CHECK(bytes != NULL && inode.size == TEST_PAYLOAD_SIZE);
	EXPECT(ext4_read(fs, &inode, 0, bytes, TEST_PAYLOAD_SIZE, &completed), EXT4_OK);
	CHECK(completed == TEST_PAYLOAD_SIZE);
	for (index = 0; index < completed; index++) {
		expected = (uint8_t)(index * 17 + 23);
		if (patch != NULL && index >= start && index - start < patch_length) {
			expected = patch[index - start];
		}
		CHECK(bytes[index] == expected);
	}
	free(bytes);
}

static void
check_inode_neighbors(struct device *device, struct ext4_fs *fs, uint32_t number, uint64_t offset,
    const uint8_t *before)
{
	size_t within = (size_t)(offset % device->block_size);
	uint64_t block = offset - within;

	(void)number;
	CHECK(memcmp(before, device->cache + block, within) == 0);
	CHECK(memcmp(before + within + fs->inode_size, device->cache + offset + fs->inode_size,
		  device->block_size - within - fs->inode_size) == 0);
}

static void
basic_operations(struct device *device)
{
	struct ext4_fs *fs = mount_writer(device);
	struct ext4_inode inode = lookup(fs, "metadata.txt");
	struct ext4_inode after;
	struct ext4_inode alias;
	struct ext4_inode_update update = write_update(fs);
	uint8_t *neighbor = malloc(device->block_size);
	uint8_t *patch;
	uint64_t offset;
	size_t length;
	size_t completed;
	uint32_t writes;
	char first;

	CHECK(neighbor != NULL);
	EXPECT(ext4_inode_location(fs, inode.number, &offset), EXT4_OK);
	memcpy(neighbor, device->cache + offset - offset % device->block_size, device->block_size);
	update.fields |= EXT4_ATTR_UID | EXT4_ATTR_GID | EXT4_ATTR_ACCESS_TIME;
	update.uid = TEST_WIDE_UID;
	update.gid = 0x81234567U;
	update.permissions = 0610;
	update.access_time.seconds = -1;
	update.access_time.nanoseconds = fs->inode_size == EXT4_INODE_BASE_SIZE ? 0 : 42;
	if (inode.birth_time_valid) {
		update.fields |= EXT4_ATTR_BIRTH_TIME;
		update.birth_time.seconds = INT64_C(1) << 32;
		update.birth_time.nanoseconds = 987654321;
	}
	EXPECT(ext4_set_attributes(fs, inode.number, inode.generation, &update, &after), EXT4_OK);
	CHECK(after.uid == update.uid && after.gid == update.gid &&
	    after.mode == (EXT4_MODE_REGULAR | update.permissions));
	CHECK(after.access_time.seconds == -1 &&
	    after.access_time.nanoseconds == update.access_time.nanoseconds);
	CHECK(after.modify_time.seconds == update.modify_time.seconds &&
	    after.change_time.seconds == update.change_time.seconds);
	CHECK(after.size == inode.size && after.blocks_512 == inode.blocks_512 &&
	    after.links == inode.links && after.flags == inode.flags &&
	    after.generation == inode.generation);
	CHECK(memcmp(after.block_data, inode.block_data, sizeof(inode.block_data)) == 0);
	check_inode_neighbors(device, fs, inode.number, offset, neighbor);
	/* A field-selective update must preserve all other live fields; an old
	 * caller snapshot is never copied back over the just-committed inode. */
	update.fields = EXT4_ATTR_ACCESS_TIME;
	update.access_time.seconds = 0;
	update.access_time.nanoseconds = 0;
	EXPECT(ext4_set_attributes(fs, inode.number, inode.generation, &update, &after), EXT4_OK);
	CHECK(after.uid == TEST_WIDE_UID && after.gid == 0x81234567U &&
	    after.mode == (EXT4_MODE_REGULAR | 0610));
	update.fields = 0;
	writes = device->writes;
	EXPECT(ext4_set_attributes(fs, inode.number, inode.generation, &update, &after), EXT4_OK);
	CHECK(device->writes == writes);
	inode = lookup(fs, "hello.txt");
	update = write_update(fs);
	update.fields |= EXT4_ATTR_UID | EXT4_ATTR_GID;
	update.uid = 1111;
	update.gid = 2222;
	EXPECT(ext4_set_attributes(fs, inode.number, inode.generation, &update, &after), EXT4_OK);
	alias = lookup(fs, "hello-hardlink");
	CHECK(alias.number == inode.number && alias.links == 2 && alias.uid == 1111 &&
	    alias.gid == 2222);
	update.fields = TEST_WRITE_FIELDS;
	EXPECT(ext4_write(fs, alias.number, alias.generation, 0, "X", 1, &update, &completed),
	    EXT4_OK);
	CHECK(completed == 1);
	EXPECT(ext4_get_inode(fs, inode.number, &inode), EXT4_OK);
	EXPECT(ext4_read(fs, &inode, 0, &first, 1, &completed), EXT4_OK);
	CHECK(completed == 1 && first == 'X');
	inode = lookup(fs, "payload.bin");
	patch = write_bytes(device, &length);
	EXPECT(ext4_write(fs, inode.number, inode.generation, device->block_size - 7, patch, length,
		   &update, &completed),
	    EXT4_OK);
	CHECK(completed == length);
	check_payload(fs, patch, length);
	writes = device->writes;
	EXPECT(ext4_write(
		   fs, inode.number, inode.generation, UINT64_MAX, NULL, 0, &update, &completed),
	    EXT4_OK);
	CHECK(completed == 0 && device->writes == writes);
	EXPECT(ext4_sync(fs), EXT4_OK);
	ext4_unmount(fs);
	CHECK(device->live == 0 && memcmp(device->cache, device->stable, device->size) == 0);
	EXPECT(ext4_mount(&device->environment, &fs), EXT4_OK);
	check_payload(fs, patch, length);
	EXPECT(ext4_write(fs, inode.number, inode.generation, 0, patch, 1, &update, &completed),
	    EXT4_READ_ONLY);
	EXPECT(ext4_set_attributes(fs, inode.number, inode.generation, &update, &after),
	    EXT4_READ_ONLY);
	ext4_unmount(fs);
	free(neighbor);
	free(patch);
}

static void
timestamp_cases(struct device *device)
{
	static const int64_t seconds[] = { INT32_MIN, -1, 0, INT32_MAX, INT64_C(2147483648),
		UINT32_MAX, INT64_C(4294967296), INT64_C(6442450943), INT64_C(6442450944),
		INT64_C(8589934592), INT64_C(10737418240), INT64_C(12884901888),
		INT64_C(15032385535) };
	struct ext4_fs *fs = mount_writer(device);
	struct ext4_inode inode = lookup(fs, "metadata.txt");
	struct ext4_inode after;
	struct ext4_inode_update update = write_update(fs);
	size_t index;
	uint32_t writes;
	enum ext4_result expected;

	update.fields = EXT4_ATTR_ACCESS_TIME;
	for (index = 0; index < sizeof(seconds) / sizeof(seconds[0]); index++) {
		update.access_time.seconds = seconds[index];
		update.access_time.nanoseconds =
		    fs->inode_size == EXT4_INODE_BASE_SIZE ? 0 : 999999999;
		expected = fs->inode_size == EXT4_INODE_BASE_SIZE && seconds[index] > INT32_MAX
		    ? EXT4_RANGE
		    : EXT4_OK;
		writes = device->writes;
		EXPECT(ext4_set_attributes(fs, inode.number, inode.generation, &update, &after),
		    expected);
		if (expected == EXT4_OK) {
			CHECK(after.access_time.seconds == seconds[index] &&
			    after.access_time.nanoseconds == update.access_time.nanoseconds);
		} else {
			CHECK(device->writes == writes);
		}
	}
	writes = device->writes;
	update.access_time.seconds = (int64_t)INT32_MIN - 1;
	EXPECT(
	    ext4_set_attributes(fs, inode.number, inode.generation, &update, &after), EXT4_RANGE);
	update.access_time.seconds = INT64_C(15032385536);
	EXPECT(
	    ext4_set_attributes(fs, inode.number, inode.generation, &update, &after), EXT4_RANGE);
	update.access_time.seconds = 0;
	update.access_time.nanoseconds = EXT4_NANOSECONDS_PER_SECOND;
	EXPECT(
	    ext4_set_attributes(fs, inode.number, inode.generation, &update, &after), EXT4_RANGE);
	if (fs->inode_size == EXT4_INODE_BASE_SIZE) {
		update.access_time.nanoseconds = 1;
		EXPECT(ext4_set_attributes(fs, inode.number, inode.generation, &update, &after),
		    EXT4_RANGE);
		update.fields = EXT4_ATTR_BIRTH_TIME | EXT4_ATTR_CHANGE_TIME;
		EXPECT(ext4_set_attributes(fs, inode.number, inode.generation, &update, &after),
		    EXT4_UNSUPPORTED);
	}
	CHECK(device->writes == writes);
	EXPECT(ext4_sync(fs), EXT4_OK);
	ext4_unmount(fs);
}

static void
rejected_operations(struct device *device)
{
	struct ext4_fs *fs = mount_writer(device);
	struct ext4_inode inode = lookup(fs, "payload.bin");
	struct ext4_inode link = lookup(fs, "hello-link");
	struct ext4_inode root;
	struct ext4_inode after;
	struct ext4_inode_update update = write_update(fs);
	size_t completed;

	EXPECT(ext4_get_inode(fs, EXT4_ROOT_INODE, &root), EXT4_OK);
	EXPECT(ext4_write(fs, inode.number, inode.generation + 1, 0, "a", 1, &update, &completed),
	    EXT4_STALE);
	EXPECT(ext4_set_attributes(fs, inode.number, inode.generation + 1, &update, &after),
	    EXT4_STALE);
	EXPECT(
	    ext4_write(fs, inode.number, inode.generation, UINT64_MAX, "a", 1, &update, &completed),
	    EXT4_RANGE);
	EXPECT(ext4_write(fs, inode.number, inode.generation, 0, "a",
		   (size_t)device->block_size * EXT4_TRANSACTION_MAX_BLOCKS, &update, &completed),
	    EXT4_RANGE);
	EXPECT(ext4_write(fs, link.number, link.generation, 0, "a", 1, &update, &completed),
	    EXT4_UNSUPPORTED);
	EXPECT(ext4_write(fs, root.number, root.generation, 0, "a", 1, &update, &completed),
	    EXT4_IS_DIRECTORY);
	EXPECT(
	    ext4_write(fs, fs->journal_inode, 0, 0, "a", 1, &update, &completed), EXT4_UNSUPPORTED);
	update.permissions |= EXT4_MODE_REGULAR;
	EXPECT(ext4_set_attributes(fs, inode.number, inode.generation, &update, &after),
	    EXT4_INVALID_ARGUMENT);
	update = write_update(fs);
	update.fields |= TEST_UNKNOWN_ATTRIBUTE;
	EXPECT(ext4_set_attributes(fs, inode.number, inode.generation, &update, &after),
	    EXT4_INVALID_ARGUMENT);
	update.fields = EXT4_ATTR_UID | EXT4_ATTR_CHANGE_TIME;
	EXPECT(ext4_set_attributes(fs, inode.number, inode.generation, &update, &after),
	    EXT4_INVALID_ARGUMENT);
	update.fields = EXT4_ATTR_PERMISSIONS;
	EXPECT(ext4_set_attributes(fs, inode.number, inode.generation, &update, &after),
	    EXT4_INVALID_ARGUMENT);
	update.fields = TEST_WRITE_FIELDS;
	update.change_time.nanoseconds = EXT4_NANOSECONDS_PER_SECOND;
	EXPECT(ext4_write(fs, inode.number, inode.generation, 0, "a", 1, &update, &completed),
	    EXT4_RANGE);
	CHECK(completed == 0 && device->writes == 0 &&
	    memcmp(device->cache, device->base, device->size) == 0);
	ext4_unmount(fs);
}

static void
extra_inode_fields(struct device *device)
{
	static const uint32_t fields[] = { EXT4_ATTR_ACCESS_TIME, EXT4_ATTR_CHANGE_TIME,
		EXT4_ATTR_MODIFY_TIME, EXT4_ATTR_BIRTH_TIME };
	struct ext4_fs *fs = mount_writer(device);
	struct ext4_inode inode = lookup(fs, "metadata.txt");
	struct ext4_inode result;
	struct ext4_inode_disk *disk;
	struct ext4_inode_update update;
	struct ext4_timestamp *time;
	uint8_t *original;
	uint64_t offset;
	uint16_t extra;
	size_t index;
	uint32_t writes;
	bool represented;
	enum ext4_result expected;

	if (fs->inode_size == EXT4_INODE_BASE_SIZE) {
		ext4_unmount(fs);
		return;
	}
	EXPECT(ext4_inode_location(fs, inode.number, &offset), EXT4_OK);
	disk = (struct ext4_inode_disk *)(device->cache + offset);
	original = malloc(fs->inode_size);
	CHECK(original != NULL);
	memcpy(original, disk, fs->inode_size);
	for (extra = 0; extra <= sizeof(struct ext4_inode_disk) - EXT4_INODE_BASE_SIZE;
	    extra += 4) {
		for (index = 0; index < sizeof(fields) / sizeof(fields[0]); index++) {
			memcpy(disk, original, fs->inode_size);
			ext4_encode16(&disk->extra_size, extra);
			ext4_inode_checksum_set(fs, inode.number, disk);
			memset(&update, 0, sizeof(update));
			update.fields = fields[index] | EXT4_ATTR_CHANGE_TIME;
			switch (fields[index]) {
			case EXT4_ATTR_ACCESS_TIME:
				time = &update.access_time;
				represented = EXT4_INODE_HAS_FIELD(extra, access_time_extra);
				break;
			case EXT4_ATTR_CHANGE_TIME:
				time = &update.change_time;
				represented = EXT4_INODE_HAS_FIELD(extra, change_time_extra);
				break;
			case EXT4_ATTR_MODIFY_TIME:
				time = &update.modify_time;
				represented = EXT4_INODE_HAS_FIELD(extra, modify_time_extra);
				break;
			default:
				time = &update.birth_time;
				represented = EXT4_INODE_HAS_FIELD(extra, birth_time_extra);
				break;
			}
			time->seconds = (int64_t)INT32_MAX + 1;
			time->nanoseconds = 1;
			expected = represented ? EXT4_OK : EXT4_RANGE;
			if (fields[index] == EXT4_ATTR_BIRTH_TIME &&
			    !EXT4_INODE_HAS_FIELD(extra, birth_time)) {
				expected = EXT4_UNSUPPORTED;
			}
			writes = device->writes;
			EXPECT(ext4_set_attributes(
				   fs, inode.number, inode.generation, &update, &result),
			    expected);
			if (expected != EXT4_OK) {
				CHECK(device->writes == writes &&
				    ext4_le16(&disk->extra_size) == extra);
			} else {
				switch (fields[index]) {
				case EXT4_ATTR_ACCESS_TIME:
					time = &result.access_time;
					break;
				case EXT4_ATTR_CHANGE_TIME:
					time = &result.change_time;
					break;
				case EXT4_ATTR_MODIFY_TIME:
					time = &result.modify_time;
					break;
				default:
					time = &result.birth_time;
					break;
				}
				CHECK(time->seconds == (int64_t)INT32_MAX + 1 &&
				    time->nanoseconds == 1);
			}
		}
	}
	free(original);
	EXPECT(ext4_sync(fs), EXT4_OK);
	ext4_unmount(fs);
}

static enum ext4_result
run_write(struct device *device, struct ext4_fs *fs, const struct ext4_inode *inode,
    const uint8_t *patch, size_t length)
{
	struct ext4_inode_update update = write_update(fs);
	struct ext4_inode after;
	size_t completed;
	enum ext4_result error;

	if (device->truncate) {
		memset(&after, 0xa5, sizeof(after));
		error = ext4_truncate_atomic(fs, inode->number, inode->generation,
		    device->operation_offset, &update, &after);
		if (error == EXT4_OK) {
			CHECK(after.size == device->operation_offset &&
			    after.number == inode->number && after.generation == inode->generation);
		} else {
			CHECK(after.size == UINT64_C(0xa5a5a5a5a5a5a5a5));
		}
		return error;
	}
	error = ext4_write(fs, inode->number, inode->generation, device->operation_offset, patch,
	    length, &update, &completed);
	CHECK(completed == (error == EXT4_OK ? length : 0));
	return error;
}

static void
precommit_failures(struct device *device, bool growth)
{
	struct ext4_fs *fs;
	struct ext4_inode inode;
	uint8_t *patch;
	size_t length;
	uint32_t allocations;
	uint32_t reads;
	uint32_t index;
	uint32_t phase;
	uint32_t count;
	enum ext4_result error;

	patch = write_bytes(device, &length);
	fs = mount_writer(device);
	inode = lookup(fs, growth ? "empty" : "payload.bin");
	device->reads = device->allocations = 0;
	EXPECT(run_write(device, fs, &inode, patch, length), EXT4_OK);
	allocations = device->allocations;
	reads = device->reads;
	ext4_unmount(fs);
	for (phase = 0; phase < 2; phase++) {
		count = phase == 0 ? allocations : reads;
		for (index = 1; index <= count; index++) {
			device_reset(device, device->base);
			fs = mount_writer(device);
			device->reads = device->allocations = 0;
			device->fail_allocation = phase == 0 ? index : 0;
			device->fail_read = phase == 1 ? index : 0;
			error = run_write(device, fs, &inode, patch, length);
			CHECK(error == (phase == 0 ? EXT4_NO_MEMORY : EXT4_IO));
			/* All fallible reads and allocation precede the first commit write. */
			CHECK(device->writes == 0 &&
			    memcmp(device->cache, device->base, device->size) == 0);
			device->fail_read = device->fail_allocation = 0;
			if (!fs->aborted) {
				EXPECT(run_write(device, fs, &inode, patch, length), EXT4_OK);
			}
			ext4_unmount(fs);
			CHECK(device->live == 0);
		}
	}
	printf("%s resource faults: allocations=%u reads=%u\n",
	    device->truncate ? "truncate" : (growth ? "allocation" : "write"), allocations, reads);
	free(patch);
}

static void
crash_cases(struct device *device, bool growth)
{
	struct ext4_fs *fs = mount_writer(device);
	struct ext4_inode inode = lookup(fs, growth ? "empty" : "payload.bin");
	struct ext4_inode after;
	struct ext4_recovery_report report;
	struct ext4_inode_update update = write_update(fs);
	struct ext4_mapping mapping;
	uint8_t *patch;
	uint8_t *expected_inode;
	uint8_t *expected_image;
	const uint8_t *expected;
	uint64_t inode_offset;
	size_t length;
	size_t completed;
	uint32_t events;
	uint32_t stop;
	uint32_t partial;
	uint32_t survival;
	uint32_t cases = 0;
	uint32_t repaired = 0;
	uint32_t rejected = 0;
	uint32_t block;
	uint16_t inode_size = fs->inode_size;
	bool old;
	bool changed;
	enum ext4_result error;

	expected_inode = malloc(inode_size);
	expected_image = growth ? malloc(device->size) : NULL;
	CHECK(expected_inode != NULL);
	CHECK(!growth || expected_image != NULL);
	EXPECT(ext4_inode_location(fs, inode.number, &inode_offset), EXT4_OK);
	patch = write_bytes(device, &length);
	EXPECT(run_write(device, fs, &inode, patch, length), EXT4_OK);
	memcpy(expected_inode, device->cache + inode_offset, inode_size);
	EXPECT(ext4_sync(fs), EXT4_OK);
	if (growth) {
		memcpy(expected_image, device->stable, device->size);
	}
	events = device->events;
	ext4_unmount(fs);
	for (stop = 1; stop <= events; stop++) {
		for (partial = 0; partial < 2; partial++) {
			for (survival = 0; survival < 3; survival++) {
				device_reset(device, device->base);
				device->stop_at = stop;
				device->partial = partial != 0;
				device->survival = survival;
				fs = mount_writer(device);
				error = run_write(device, fs, &inode, patch, length);
				if (error == EXT4_OK) {
					error = ext4_sync(fs);
				}
				CHECK(error == EXT4_IO && device->off && device->events == stop);
				EXPECT(ext4_get_inode(fs, inode.number, &after),
				    EXT4_RECOVERY_REQUIRED);
				EXPECT(ext4_read(fs, &inode, 0, patch, 0, &completed),
				    EXT4_RECOVERY_REQUIRED);
				EXPECT(ext4_map_read(fs, &inode, inode.size, 1, &mapping),
				    EXT4_RECOVERY_REQUIRED);
				EXPECT(ext4_set_attributes(
					   fs, inode.number, inode.generation, &update, &after),
				    EXT4_RECOVERY_REQUIRED);
				EXPECT(ext4_sync(fs), EXT4_RECOVERY_REQUIRED);
				ext4_unmount(fs);
				device_reset(device, device->stable);
				error =
				    ext4_recover(&device->environment, &device->writer, &report);
				cases++;
				if (error == EXT4_CORRUPT) {
					CHECK(partial != 0 && survival != 0 && device->writes == 0);
					rejected++;
					continue;
				}
				CHECK(error == EXT4_OK);
				repaired++;
				old = memcmp(device->stable + inode_offset,
					  device->base + inode_offset, inode_size) == 0;
				changed = memcmp(device->stable + inode_offset, expected_inode,
					      inode_size) == 0;
				CHECK(old != changed);
				fs = mount_writer(device);
				if (growth) {
					expected = changed ? expected_image : device->base;
					/* All allocation metadata, mapping nodes and file data
					 * must agree on the same commit. The log's sequence and
					 * unused records need not match a clean initial image. */
					for (block = 0; block < device->blocks; block++) {
						if (ext4_journal_target(fs->journal, block)) {
							CHECK(memcmp(device->stable +
								      (size_t)block *
									  device->block_size,
								  expected +
								      (size_t)block *
									  device->block_size,
								  device->block_size) == 0);
						}
					}
				} else {
					check_payload(fs, changed ? patch : NULL, length);
				}
				ext4_unmount(fs);
				CHECK(device->live == 0);
			}
		}
	}
	printf("%s crash cuts: cases=%u recovered=%u damaged-superblock=%u\n",
	    device->truncate ? "truncate" : (growth ? "allocation" : "write"), cases, repaired,
	    rejected);
	free(expected_image);
	free(expected_inode);
	free(patch);
}

static void
metadata_guards(struct device *device)
{
	static const uint32_t flags[] = { EXT4_INODE_IMMUTABLE, EXT4_INODE_APPEND,
		TEST_UNKNOWN_INODE_FLAG };
	struct ext4_fs *fs;
	struct ext4_inode inode;
	struct ext4_inode after;
	struct ext4_inode_disk *disk;
	struct ext4_inode_update update;
	struct ext4_extent_disk *extent;
	struct ext4_group group;
	uint64_t offset;
	uint64_t targets[5];
	uint32_t original_flags;
	size_t index;
	size_t completed;
	size_t extra;

	fs = mount_writer(device);
	inode = lookup(fs, "payload.bin");
	update = write_update(fs);
	EXPECT(ext4_inode_location(fs, inode.number, &offset), EXT4_OK);
	disk = (struct ext4_inode_disk *)(device->cache + offset);
	original_flags = ext4_le32(&disk->flags);
	for (index = 0; index < sizeof(flags) / sizeof(flags[0]); index++) {
		ext4_encode32(&disk->flags, original_flags | flags[index]);
		ext4_inode_checksum_set(fs, inode.number, disk);
		EXPECT(
		    ext4_write(fs, inode.number, inode.generation, 0, "a", 1, &update, &completed),
		    flags[index] == TEST_UNKNOWN_INODE_FLAG ? EXT4_UNSUPPORTED
							    : EXT4_PERMISSION_DENIED);
		EXPECT(ext4_set_attributes(fs, inode.number, inode.generation, &update, &after),
		    flags[index] == TEST_UNKNOWN_INODE_FLAG ? EXT4_UNSUPPORTED
							    : EXT4_PERMISSION_DENIED);
	}
	ext4_encode32(&disk->flags, original_flags);
	ext4_encode32(&disk->xattr_block_lo, 1);
	ext4_inode_checksum_set(fs, inode.number, disk);
	EXPECT(
	    ext4_set_attributes(fs, inode.number, inode.generation, &update, &after), EXT4_CORRUPT);
	ext4_encode32(&disk->xattr_block_lo, 0);
	if (fs->inode_size > EXT4_INODE_BASE_SIZE) {
		extra = EXT4_INODE_BASE_SIZE + ext4_le16(&disk->extra_size);
		ext4_encode32((struct ext4_le32 *)((uint8_t *)disk + extra), EXT4_XATTR_MAGIC);
		ext4_inode_checksum_set(fs, inode.number, disk);
		EXPECT(ext4_set_attributes(fs, inode.number, inode.generation, &update, &after),
		    EXT4_INVALID_ARGUMENT);
		ext4_encode32((struct ext4_le32 *)((uint8_t *)disk + extra), 0);
	}
	ext4_inode_checksum_set(fs, inode.number, disk);
	EXPECT(ext4_group_get(fs, 0, &group), EXT4_OK);
	targets[0] = fs->first_data_block;
	targets[1] = fs->first_data_block + 1;
	targets[2] = group.block_bitmap;
	targets[3] = group.inode_bitmap;
	targets[4] = group.inode_table;
	for (index = 0; index < sizeof(targets) / sizeof(targets[0]); index++) {
		if (original_flags & EXT4_INODE_EXTENTS) {
			extent = (struct ext4_extent_disk *)(disk->block_data +
			    sizeof(struct ext4_extent_header_disk));
			ext4_encode16(&extent->length, 1);
			ext4_encode32(&extent->physical_lo, (uint32_t)targets[index]);
			ext4_encode16(&extent->physical_hi, (uint16_t)(targets[index] >> 32));
		} else {
			ext4_encode32(
			    (struct ext4_le32 *)disk->block_data, (uint32_t)targets[index]);
		}
		ext4_inode_checksum_set(fs, inode.number, disk);
		/* A zero block is the legacy indirect hole marker. */
		EXPECT(
		    ext4_write(fs, inode.number, inode.generation, 0, "a", 1, &update, &completed),
		    targets[index] == 0 && !(original_flags & EXT4_INODE_EXTENTS) ? EXT4_UNSUPPORTED
										  : EXT4_CORRUPT);
	}
	CHECK(device->writes == 0);
	ext4_unmount(fs);
}

static void
check_contents(struct ext4_fs *fs, const char *name, const uint8_t *expected, size_t size)
{
	struct ext4_inode inode = lookup(fs, name);
	uint8_t *contents = malloc(size == 0 ? 1 : size);
	size_t completed;

	CHECK(contents != NULL && inode.size == size);
	EXPECT(ext4_read(fs, &inode, 0, contents, size, &completed), EXT4_OK);
	CHECK(completed == size && memcmp(contents, expected, size) == 0);
	free(contents);
}

static void
allocation_operations(struct device *device)
{
	struct ext4_fs *fs = mount_writer(device);
	struct ext4_inode inode = lookup(fs, "empty");
	struct ext4_inode after;
	struct ext4_inode_update update = write_update(fs);
	uint8_t *expected = calloc(1, (size_t)device->block_size * 724);
	uint8_t *patch;
	uint8_t bytes[17];
	uint64_t initial_free = fs->info.free_blocks;
	uint64_t initial_blocks = inode.blocks_512;
	size_t size = (size_t)inode.size;
	size_t length;
	size_t completed;
	size_t offset;
	uint32_t index;
	uint32_t logical;
	uint16_t depth;

	CHECK(expected != NULL && size <= (size_t)device->block_size * 724);
	check_contents(fs, "empty", expected, size);
	patch = write_bytes(device, &length);
	offset = device->block_size - 7;
	EXPECT(ext4_write(
		   fs, inode.number, inode.generation, offset, patch, length, &update, &completed),
	    EXT4_OK);
	CHECK(completed == length);
	memcpy(expected + offset, patch, length);
	if (size < offset + length) {
		size = offset + length;
	}
	/* Permute nonadjacent logical blocks. Insertions at both ends and in the
	 * middle force inode-root growth, leaf splits and parent splits at 1 KiB. */
	for (index = 0; index < 350; index++) {
		logical = 4 + ((index * 73U) % 359U) * 2U;
		offset = (size_t)logical * device->block_size + index % 31U;
		memset(bytes, (int)((index % 251U) + 1U), sizeof(bytes));
		EXPECT(ext4_write(fs, inode.number, inode.generation, offset, bytes, sizeof(bytes),
			   &update, &completed),
		    EXT4_OK);
		CHECK(completed == sizeof(bytes));
		memcpy(expected + offset, bytes, sizeof(bytes));
		if (size < offset + sizeof(bytes)) {
			size = offset + sizeof(bytes);
		}
	}
	/* Join an allocated range across a hole without losing either neighbor. */
	offset = device->block_size * 3U - 7;
	EXPECT(ext4_write(
		   fs, inode.number, inode.generation, offset, patch, length, &update, &completed),
	    EXT4_OK);
	memcpy(expected + offset, patch, length);
	CHECK(completed == length);
	check_contents(fs, "empty", expected, size);
	EXPECT(ext4_get_inode(fs, inode.number, &after), EXT4_OK);
	CHECK(after.blocks_512 - initial_blocks ==
	    (initial_free - fs->info.free_blocks) * (device->block_size / TEST_SECTOR_SIZE));
	if (inode.flags & EXT4_INODE_EXTENTS) {
		depth = ext4_le16(&((struct ext4_extent_header_disk *)after.block_data)->depth);
		CHECK(depth >= (device->block_size == EXT4_MIN_BLOCK_SIZE ? 2U : 1U));
	}
	check_payload(fs, NULL, 0);
	EXPECT(ext4_sync(fs), EXT4_OK);
	ext4_unmount(fs);
	fs = mount_writer(device);
	check_contents(fs, "empty", expected, size);
	ext4_unmount(fs);
	CHECK(device->live == 0 && memcmp(device->cache, device->stable, device->size) == 0);
	free(patch);
	free(expected);
}

static void
allocated_eof_gap(struct device *device)
{
	struct ext4_fs *fs = mount_writer(device);
	struct ext4_inode inode = lookup(fs, "payload.bin");
	struct ext4_inode_disk *disk;
	struct ext4_inode_update update = write_update(fs);
	uint8_t *expected = calloc(1, (size_t)device->block_size * 3 + 8);
	uint64_t inode_offset;
	uint64_t initial_free = fs->info.free_blocks;
	size_t end = (size_t)device->block_size * 3 + 8;
	size_t completed;

	CHECK(expected != NULL && end < inode.size);
	EXPECT(ext4_inode_location(fs, inode.number, &inode_offset), EXT4_OK);
	disk = (struct ext4_inode_disk *)(device->cache + inode_offset);
	/* Exercise a written allocation beyond EOF. Its old bytes must not be
	 * exposed by a later sparse write, even though the bitmap is already set. */
	ext4_encode32(&disk->size_lo, 1);
	ext4_encode32(&disk->size_hi, 0);
	ext4_inode_checksum_set(fs, inode.number, disk);
	expected[0] = 23;
	expected[end - 1] = 'Z';
	EXPECT(ext4_write(fs, inode.number, inode.generation, end - 1, "Z", 1, &update, &completed),
	    EXT4_OK);
	CHECK(completed == 1 && fs->info.free_blocks == initial_free);
	check_contents(fs, "payload.bin", expected, end);
	EXPECT(ext4_sync(fs), EXT4_OK);
	ext4_unmount(fs);
	free(expected);
}

static void
allocation_guards(struct device *device)
{
	struct ext4_fs *fs = mount_writer(device);
	struct ext4_inode inode = lookup(fs, "empty");
	struct ext4_inode_update update = write_update(fs);
	struct ext4_super_disk *super =
	    (struct ext4_super_disk *)(device->cache + EXT4_SUPER_OFFSET);
	uint32_t credits = ext4_journal_credits(fs->journal);
	uint8_t *bytes = calloc(credits, device->block_size);
	uint64_t free_blocks = fs->info.free_blocks;
	size_t completed;

	CHECK(bytes != NULL);
	/* Data alone fits the admission bound, but allocation metadata consumes
	 * additional credits. A late credit failure must still precede all I/O. */
	EXPECT(ext4_write(fs, inode.number, inode.generation, inode.size, bytes,
		   (size_t)(credits - 1) * device->block_size, &update, &completed),
	    EXT4_RANGE);
	CHECK(completed == 0 && device->writes == 0 && fs->info.free_blocks == free_blocks &&
	    memcmp(device->cache, device->base, device->size) == 0);
	ext4_encode32(&super->reserved_blocks_lo, (uint32_t)free_blocks);
	ext4_encode32(&super->reserved_blocks_hi, (uint32_t)(free_blocks >> 32));
	if (fs->metadata_checksum) {
		ext4_encode32(&super->checksum,
		    ext4_crc32c(UINT32_MAX, super, offsetof(struct ext4_super_disk, checksum)));
	}
	EXPECT(ext4_write(fs, inode.number, inode.generation, inode.size + device->block_size, "a",
		   1, &update, &completed),
	    EXT4_NO_SPACE);
	CHECK(completed == 0 && device->writes == 0 && fs->info.free_blocks == free_blocks);
	ext4_unmount(fs);
	free(bytes);
}

static void
full_disk(struct device *device)
{
	struct ext4_fs *fs = mount_writer(device);
	struct ext4_inode inode = lookup(fs, "empty");
	struct ext4_inode after;
	struct ext4_inode_update update = write_update(fs);
	struct ext4_super_disk *super;
	struct ext4_group group;
	uint8_t *bytes;
	uint8_t *before;
	uint64_t offset = 0;
	uint64_t free_blocks;
	size_t length = (size_t)device->block_size * 128;
	size_t completed;
	uint32_t index;
	uint32_t uninitialized = 0;
	enum ext4_result error;

	/* A single multi-group profile bounds runtime while exercising lazy
	 * bitmap initialization, the short final group, and actual exhaustion. */
	if (device->block_size != EXT4_MIN_BLOCK_SIZE || fs->inode_size == EXT4_INODE_BASE_SIZE ||
	    !(inode.flags & EXT4_INODE_EXTENTS) || inode.size != 0) {
		ext4_unmount(fs);
		return;
	}
	for (index = 0; index < fs->info.groups; index++) {
		EXPECT(ext4_group_get(fs, index, &group), EXT4_OK);
		if (group.flags & EXT4_GROUP_BLOCK_UNINIT) {
			uninitialized++;
		}
	}
	CHECK(uninitialized != 0);
	bytes = malloc(length);
	before = malloc(device->size);
	CHECK(bytes != NULL && before != NULL);
	memset(bytes, 0x6d, length);
	super = (struct ext4_super_disk *)(device->cache + EXT4_SUPER_OFFSET);
	ext4_encode32(&super->reserved_blocks_lo, 0);
	ext4_encode32(&super->reserved_blocks_hi, 0);
	ext4_encode32(&super->checksum,
	    ext4_crc32c(UINT32_MAX, super, offsetof(struct ext4_super_disk, checksum)));
	for (;;) {
		free_blocks = fs->info.free_blocks;
		/* Only the last few transactions can fail. Preserve their whole
		 * resource image to verify rollback after partially preparing a batch. */
		if (free_blocks < 256) {
			memcpy(before, device->cache, device->size);
		}
		error = ext4_write(
		    fs, inode.number, inode.generation, offset, bytes, length, &update, &completed);
		if (error == EXT4_NO_SPACE) {
			CHECK(free_blocks < 256 && completed == 0 &&
			    fs->info.free_blocks == free_blocks &&
			    memcmp(before, device->cache, device->size) == 0);
			if (length == device->block_size) {
				break;
			}
			length = device->block_size;
			continue;
		}
		CHECK(error == EXT4_OK && completed == length);
		offset += length;
	}
	CHECK(fs->info.free_blocks == 0);
	for (index = 0; index < fs->info.groups; index++) {
		EXPECT(ext4_group_get(fs, index, &group), EXT4_OK);
		CHECK(group.free_blocks == 0 && !(group.flags & EXT4_GROUP_BLOCK_UNINIT));
	}
	EXPECT(ext4_get_inode(fs, inode.number, &after), EXT4_OK);
	CHECK(after.size == offset);
	EXPECT(ext4_read(
		   fs, &after, offset - device->block_size, before, device->block_size, &completed),
	    EXT4_OK);
	CHECK(completed == device->block_size && memcmp(before, bytes, completed) == 0);
	EXPECT(ext4_sync(fs), EXT4_OK);
	ext4_unmount(fs);
	fs = mount_writer(device);
	CHECK(fs->info.free_blocks == 0);
	free_blocks = after.blocks_512 / (device->block_size / TEST_SECTOR_SIZE);
	EXPECT(
	    ext4_truncate_atomic(fs, inode.number, inode.generation, 0, &update, &after), EXT4_OK);
	CHECK(after.blocks_512 == 0 && fs->info.free_blocks == free_blocks);
	EXPECT(ext4_write(fs, inode.number, inode.generation, 7, "R", 1, &update, &completed),
	    EXT4_OK);
	CHECK(completed == 1 && fs->info.free_blocks == free_blocks - 1);
	EXPECT(ext4_get_inode(fs, inode.number, &after), EXT4_OK);
	EXPECT(ext4_map_block(fs, &after, 0, &offset), EXT4_OK);
	CHECK(offset != 0);
	for (index = 0; index < device->block_size; index++) {
		CHECK(device->cache[offset * device->block_size + index] == (index == 7 ? 'R' : 0));
	}
	EXPECT(ext4_sync(fs), EXT4_OK);
	ext4_unmount(fs);
	free(bytes);
	free(before);
	printf("full disk: initialized-groups=%u exhausted, released, reused with zeroed bytes\n",
	    uninitialized);
}

static void
indirect_boundaries(struct device *device)
{
	struct ext4_fs *fs = mount_writer(device);
	struct ext4_inode inode = lookup(fs, "empty");
	struct ext4_inode_update update = write_update(fs);
	uint32_t per_block = device->block_size / sizeof(struct ext4_le32);
	uint64_t logical[8];
	uint64_t limit;
	uint64_t offset;
	uint64_t initial_free = fs->info.free_blocks;
	uint8_t byte;
	uint8_t observed;
	size_t completed;
	size_t index;
	size_t verify;

	if (inode.flags & EXT4_INODE_EXTENTS) {
		ext4_unmount(fs);
		return;
	}
	logical[0] = EXT4_DIRECT_BLOCKS - 1;
	logical[1] = EXT4_DIRECT_BLOCKS;
	logical[2] = EXT4_DIRECT_BLOCKS + per_block - 1;
	logical[3] = EXT4_DIRECT_BLOCKS + per_block;
	logical[4] = EXT4_DIRECT_BLOCKS + per_block + (uint64_t)per_block * per_block - 1;
	logical[5] = logical[4] + 1;
	limit = logical[5] + (uint64_t)per_block * per_block * per_block;
	logical[6] = limit - 1;
	logical[7] = 0;
	CHECK(limit < UINT32_MAX);
	for (index = 0; index < sizeof(logical) / sizeof(logical[0]); index++) {
		offset = logical[index] * device->block_size + 17;
		byte = (uint8_t)(index + 1);
		EXPECT(ext4_write(fs, inode.number, inode.generation, offset, &byte, 1, &update,
			   &completed),
		    EXT4_OK);
		CHECK(completed == 1);
	}
	EXPECT(ext4_get_inode(fs, inode.number, &inode), EXT4_OK);
	for (index = 0; index < sizeof(logical) / sizeof(logical[0]); index++) {
		offset = logical[index] * device->block_size + 17;
		EXPECT(ext4_read(fs, &inode, offset, &observed, 1, &completed), EXT4_OK);
		CHECK(completed == 1 && observed == index + 1);
		EXPECT(ext4_read(fs, &inode, offset - 1, &observed, 1, &completed), EXT4_OK);
		CHECK(completed == 1 && observed == 0);
	}
	EXPECT(ext4_write(fs, inode.number, inode.generation, limit * device->block_size, "a", 1,
		   &update, &completed),
	    EXT4_RANGE);
	/* Remove the last direct/single/double/triple mappings independently,
	 * then release the surviving path. Every lower boundary remains readable. */
	for (index = 7; index > 0; index--) {
		EXPECT(ext4_truncate_atomic(fs, inode.number, inode.generation,
			   logical[index - 1] * device->block_size, &update, &inode),
		    EXT4_OK);
		for (verify = 0; verify < sizeof(logical) / sizeof(logical[0]); verify++) {
			if (logical[verify] < logical[index - 1]) {
				EXPECT(
				    ext4_read(fs, &inode, logical[verify] * device->block_size + 17,
					&observed, 1, &completed),
				    EXT4_OK);
				CHECK(completed == 1 && observed == verify + 1);
			}
		}
	}
	EXPECT(
	    ext4_truncate_atomic(fs, inode.number, inode.generation, 0, &update, &inode), EXT4_OK);
	CHECK(inode.blocks_512 == 0 && fs->info.free_blocks == initial_free);
	EXPECT(ext4_sync(fs), EXT4_OK);
	ext4_unmount(fs);
	printf("indirect boundaries: direct/single/double/triple, maximum logical block, path "
	       "release\n");
}

static void
bitmap_corruption(struct device *device)
{
	enum {
		BITMAP_CHECKSUM_DAMAGE,
		BITMAP_SYSTEM_FIRST,
		BITMAP_FREE_COUNT_DAMAGE = BITMAP_SYSTEM_FIRST + EXT4_BITS_PER_BYTE,
		BITMAP_DAMAGE_COUNT
	};
	struct ext4_fs *fs;
	struct ext4_inode inode;
	struct ext4_inode_update update;
	struct ext4_group group;
	struct ext4_group_disk *descriptor;
	struct ext4_super_disk *super;
	uint8_t *bitmap;
	uint64_t relative;
	uint64_t first;
	uint32_t checksum;
	uint32_t test;
	uint32_t index;
	uint32_t group_index;
	size_t completed;

	for (test = 0; test < BITMAP_DAMAGE_COUNT; test++) {
		device_reset(device, device->base);
		fs = mount_writer(device);
		inode = lookup(fs, "empty");
		update = write_update(fs);
		for (group_index = 0; group_index < fs->info.groups; group_index++) {
			EXPECT(ext4_group_get(fs, group_index, &group), EXT4_OK);
			if (group.free_blocks != 0) {
				break;
			}
		}
		CHECK(group_index < fs->info.groups && !(group.flags & EXT4_GROUP_BLOCK_UNINIT));
		first = fs->first_data_block + (uint64_t)group_index * fs->blocks_per_group;
		for (relative = 0; relative < fs->blocks_per_group; relative++) {
			if (ext4_system_block(fs, first + relative) &&
			    (test == BITMAP_CHECKSUM_DAMAGE || test == BITMAP_FREE_COUNT_DAMAGE ||
				(relative % EXT4_BITS_PER_BYTE == test - BITMAP_SYSTEM_FIRST))) {
				break;
			}
		}
		CHECK(relative < fs->blocks_per_group);
		for (index = 0; index < fs->journal->mapping_count; index++) {
			CHECK(ext4_system_block(fs, fs->journal->mapping_blocks[index]) &&
			    !ext4_journal_target(fs->journal, fs->journal->mapping_blocks[index]));
		}
		descriptor = (struct ext4_group_disk *)(device->cache +
		    (size_t)(fs->first_data_block + 1) * device->block_size +
		    (size_t)group_index * fs->descriptor_size);
		super = (struct ext4_super_disk *)(device->cache + EXT4_SUPER_OFFSET);
		bitmap = device->cache + group.block_bitmap * device->block_size;
		if (test == BITMAP_CHECKSUM_DAMAGE) {
			/* An ordinary damaged bitmap, with its old checksum. */
			bitmap[0] ^= 1U;
		} else {
			if (test != BITMAP_FREE_COUNT_DAMAGE) {
				/* A forged free system block with matching checksums and
				 * counters must reject at every bit position within a byte. */
				bitmap[relative / EXT4_BITS_PER_BYTE] &=
				    (uint8_t)~(1U << (relative % EXT4_BITS_PER_BYTE));
			}
			group.free_blocks++;
			fs->info.free_blocks++;
			ext4_encode16(&descriptor->free_blocks_lo, (uint16_t)group.free_blocks);
			ext4_encode32(&super->free_blocks_lo, (uint32_t)fs->info.free_blocks);
			if (fs->metadata_checksum) {
				checksum = ext4_crc32c(fs->checksum_seed, bitmap,
				    fs->blocks_per_group / EXT4_BITS_PER_BYTE);
				ext4_encode16(
				    &descriptor->block_bitmap_checksum_lo, (uint16_t)checksum);
				if (fs->descriptor_size >= EXT4_GROUP_64_SIZE) {
					ext4_encode16(&descriptor->block_bitmap_checksum_hi,
					    (uint16_t)(checksum >> 16));
				}
				ext4_encode32(&super->checksum,
				    ext4_crc32c(UINT32_MAX, super,
					offsetof(struct ext4_super_disk, checksum)));
			}
			ext4_group_checksum_set(fs, group_index, descriptor);
		}
		EXPECT(ext4_write(fs, inode.number, inode.generation, inode.size, "a", 1, &update,
			   &completed),
		    EXT4_CORRUPT);
		CHECK(device->writes == 0 && completed == 0);
		ext4_unmount(fs);
	}
}

static void
mapping_faults(struct device *device)
{
	struct ext4_fs *fs = mount_writer(device);
	struct ext4_inode inode = lookup(fs, "empty");
	struct ext4_inode_update update = write_update(fs);
	uint8_t *saved = device->base;
	uint8_t *prepared;
	uint64_t saved_offset = device->operation_offset;
	uint64_t per_block = device->block_size / sizeof(struct ext4_le32);
	size_t completed;
	uint32_t index;

	if (inode.size != 0) {
		ext4_unmount(fs);
		return;
	}
	prepared = malloc(device->size);
	CHECK(prepared != NULL);
	if (inode.flags & EXT4_INODE_EXTENTS) {
		for (index = 1; index <= 4; index++) {
			EXPECT(ext4_write(fs, inode.number, inode.generation,
				   (uint64_t)index * 4 * device->block_size, "S", 1, &update,
				   &completed),
			    EXT4_OK);
		}
		EXPECT(ext4_get_inode(fs, inode.number, &inode), EXT4_OK);
		CHECK(ext4_le16(&((struct ext4_extent_header_disk *)inode.block_data)->entries) ==
			4 &&
		    ext4_le16(&((struct ext4_extent_header_disk *)inode.block_data)->depth) == 0);
	} else {
		device->operation_offset =
		    (EXT4_DIRECT_BLOCKS + per_block + per_block * per_block) * device->block_size -
		    7;
	}
	EXPECT(ext4_sync(fs), EXT4_OK);
	ext4_unmount(fs);
	memcpy(prepared, device->stable, device->size);
	device->base = prepared;
	device_reset(device, prepared);
	precommit_failures(device, true);
	device_reset(device, prepared);
	crash_cases(device, true);
	device->base = saved;
	device->operation_offset = saved_offset;
	free(prepared);
	printf("mapping growth faults: %s\n",
	    inode.flags & EXT4_INODE_EXTENTS ? "extent root promotion"
					     : "double/triple indirect boundary");
}

static void
export_state(struct device *device, const char *prefix)
{
	const char *name;
	char *destination;
	FILE *output;
	size_t path_size;

	if (device->export_directory == NULL) {
		return;
	}
	name = strrchr(device->source_path, '/');
	name = name == NULL ? device->source_path : name + 1;
	path_size = strlen(device->export_directory) + strlen(name) + strlen(prefix) + 2;
	destination = malloc(path_size);
	CHECK(destination != NULL);
	CHECK(snprintf(destination, path_size, "%s/%s%s", device->export_directory, prefix, name) >
	    0);
	output = fopen(destination, "wbx");
	CHECK(output != NULL && fwrite(device->stable, 1, device->size, output) == device->size);
	CHECK(fclose(output) == 0);
	free(destination);
}

static void
truncate_operations(struct device *device)
{
	static const char *checkpoints[] = { "cut400-", "cut40-", "cut4-" };
	struct ext4_fs *fs = mount_writer(device);
	struct ext4_inode inode = lookup(fs, "empty");
	struct ext4_inode after;
	struct ext4_inode_update update = write_update(fs);
	uint64_t total_free =
	    fs->info.free_blocks + inode.blocks_512 / (device->block_size / TEST_SECTOR_SIZE);
	uint64_t physical;
	uint8_t *expected;
	size_t original_size;
	size_t size;
	size_t completed;
	size_t index;
	size_t targets[4];
	uint32_t test;

	ext4_unmount(fs);
	allocation_operations(device);
	fs = mount_writer(device);
	inode = lookup(fs, "empty");
	original_size = (size_t)inode.size;
	expected = malloc(original_size);
	CHECK(expected != NULL);
	EXPECT(ext4_read(fs, &inode, 0, expected, original_size, &completed), EXT4_OK);
	CHECK(completed == original_size);
	targets[0] = (size_t)400 * device->block_size + 7;
	targets[1] = (size_t)40 * device->block_size;
	targets[2] = (size_t)4 * device->block_size + 9;
	targets[3] = 0;
	for (test = 0; test < 4; test++) {
		size = targets[test];
		EXPECT(
		    ext4_truncate_atomic(fs, inode.number, inode.generation, size, &update, &after),
		    EXT4_OK);
		CHECK(after.size == size &&
		    after.mode == (EXT4_MODE_REGULAR | update.permissions) &&
		    after.links == inode.links &&
		    after.modify_time.seconds == update.modify_time.seconds &&
		    after.change_time.seconds == update.change_time.seconds);
		CHECK(fs->info.free_blocks +
			after.blocks_512 / (device->block_size / TEST_SECTOR_SIZE) ==
		    total_free);
		check_contents(fs, "empty", expected, size);
		if (size % device->block_size != 0) {
			EXPECT(ext4_map_block(
				   fs, &after, (uint32_t)(size / device->block_size), &physical),
			    EXT4_OK);
			if (physical != 0) {
				for (index = size % device->block_size; index < device->block_size;
				    index++) {
					CHECK(
					    device->cache[physical * device->block_size + index] ==
					    0);
				}
			}
		}
		if (test >= 2 && (inode.flags & EXT4_INODE_EXTENTS)) {
			CHECK(
			    ext4_le16(
				&((struct ext4_extent_header_disk *)after.block_data)->depth) == 0);
		}
		if (test < 3 && device->export_directory != NULL) {
			EXPECT(ext4_sync(fs), EXT4_OK);
			export_state(device, checkpoints[test]);
		}
		memset(expected + size, 0, original_size - size);
		/* Growth after each shrink must not reveal the old tail or reallocate
		 * holes, including after removal of a multi-level mapping tree. */
		inode = after;
		EXPECT(ext4_truncate_atomic(
			   fs, inode.number, inode.generation, original_size, &update, &after),
		    EXT4_OK);
		CHECK(after.blocks_512 == inode.blocks_512);
		check_contents(fs, "empty", expected, original_size);
		inode = after;
	}
	EXPECT(
	    ext4_truncate_atomic(fs, inode.number, inode.generation, 0, &update, &after), EXT4_OK);
	CHECK(after.blocks_512 == 0 && fs->info.free_blocks == total_free);
	EXPECT(ext4_write(fs, inode.number, inode.generation, (uint64_t)4 * device->block_size + 7,
		   "T", 1, &update, &completed),
	    EXT4_OK);
	CHECK(completed == 1);
	size = (size_t)9 * device->block_size + 13;
	expected[4 * device->block_size + 7] = 'T';
	EXPECT(ext4_truncate_atomic(fs, inode.number, inode.generation, size, &update, &after),
	    EXT4_OK);
	CHECK(after.blocks_512 == device->block_size / TEST_SECTOR_SIZE &&
	    fs->info.free_blocks == total_free - 1);
	check_contents(fs, "empty", expected, size);
	check_payload(fs, NULL, 0);
	EXPECT(ext4_sync(fs), EXT4_OK);
	ext4_unmount(fs);
	fs = mount_writer(device);
	check_contents(fs, "empty", expected, size);
	ext4_unmount(fs);
	free(expected);
	printf("truncate operations: partial/aligned/empty, tree collapse, grow, block reuse\n");
}

static void
truncate_preallocation(struct device *device)
{
	struct ext4_fs *fs = mount_writer(device);
	struct ext4_inode inode = lookup(fs, "empty");
	struct ext4_inode after;
	struct ext4_inode_update update = write_update(fs);
	uint64_t total_free =
	    fs->info.free_blocks + inode.blocks_512 / (device->block_size / TEST_SECTOR_SIZE);
	size_t size = (size_t)129 * device->block_size + 5;
	uint8_t *expected;

	if (inode.size == 0) {
		ext4_unmount(fs);
		return;
	}
	expected = calloc(1, size);
	CHECK(expected != NULL);
	EXPECT(ext4_truncate_atomic(fs, inode.number, inode.generation,
		   (uint64_t)17 * device->block_size + 13, &update, &after),
	    EXT4_OK);
	CHECK(after.blocks_512 == 18 * (device->block_size / TEST_SECTOR_SIZE));
	EXPECT(ext4_truncate_atomic(fs, inode.number, inode.generation, size, &update, &after),
	    EXT4_OK);
	CHECK(after.blocks_512 == 18 * (device->block_size / TEST_SECTOR_SIZE));
	check_contents(fs, "empty", expected, size);
	EXPECT(
	    ext4_truncate_atomic(fs, inode.number, inode.generation, 0, &update, &after), EXT4_OK);
	CHECK(after.blocks_512 == 0 && fs->info.free_blocks == total_free);
	EXPECT(ext4_sync(fs), EXT4_OK);
	ext4_unmount(fs);
	free(expected);
	printf("truncate unwritten preallocation: released blocks and zero growth\n");
}

static void
truncate_guards(struct device *device)
{
	struct ext4_fs *fs = mount_writer(device);
	struct ext4_inode inode = lookup(fs, "payload.bin");
	struct ext4_inode link = lookup(fs, "hello-link");
	struct ext4_inode root;
	struct ext4_inode after;
	struct ext4_inode_update update = write_update(fs);
	struct ext4_inode_disk *disk;
	uint64_t inode_offset;
	uint32_t ring_end;
	uint32_t saved_blocks;
	uint32_t sector_units = device->block_size / TEST_SECTOR_SIZE;

	EXPECT(ext4_get_inode(fs, EXT4_ROOT_INODE, &root), EXT4_OK);
	EXPECT(ext4_truncate_atomic(fs, inode.number, inode.generation + 1, 0, &update, &after),
	    EXT4_STALE);
	EXPECT(ext4_truncate_atomic(fs, root.number, root.generation, 0, &update, &after),
	    EXT4_IS_DIRECTORY);
	EXPECT(ext4_truncate_atomic(fs, link.number, link.generation, 0, &update, &after),
	    EXT4_UNSUPPORTED);
	EXPECT(
	    ext4_truncate_atomic(fs, inode.number, inode.generation, UINT64_MAX, &update, &after),
	    EXT4_RANGE);
	EXPECT(ext4_truncate_atomic(fs, inode.number, inode.generation, 0, &update, NULL),
	    EXT4_INVALID_ARGUMENT);
	update.fields |= EXT4_ATTR_ACCESS_TIME;
	EXPECT(ext4_truncate_atomic(fs, inode.number, inode.generation, 0, &update, &after),
	    EXT4_INVALID_ARGUMENT);
	update = write_update(fs);
	ring_end = fs->journal->last;
	fs->journal->last = fs->journal->first + 6;
	EXPECT(ext4_truncate_atomic(fs, inode.number, inode.generation, 1, &update, &after),
	    EXT4_RANGE);
	fs->journal->last = ring_end;
	CHECK(device->writes == 0 && memcmp(device->cache, device->base, device->size) == 0);
	EXPECT(ext4_inode_location(fs, inode.number, &inode_offset), EXT4_OK);
	disk = (struct ext4_inode_disk *)(device->cache + inode_offset);
	saved_blocks = ext4_le32(&disk->blocks_lo);
	ext4_encode32(&disk->blocks_lo, saved_blocks + sector_units);
	ext4_inode_checksum_set(fs, inode.number, disk);
	EXPECT(ext4_truncate_atomic(fs, inode.number, inode.generation, 0, &update, &after),
	    EXT4_CORRUPT);
	CHECK(device->writes == 0);
	ext4_encode32(&disk->blocks_lo, saved_blocks);
	ext4_inode_checksum_set(fs, inode.number, disk);
	CHECK(memcmp(device->cache, device->base, device->size) == 0);
	ext4_unmount(fs);
	EXPECT(ext4_mount(&device->environment, &fs), EXT4_OK);
	EXPECT(ext4_truncate_atomic(fs, inode.number, inode.generation, 0, &update, &after),
	    EXT4_READ_ONLY);
	ext4_unmount(fs);
}

static void
truncate_sparse_limits(struct device *device)
{
	struct ext4_fs *fs = mount_writer(device);
	struct ext4_inode inode = lookup(fs, "empty");
	struct ext4_inode after;
	struct ext4_inode alias;
	struct ext4_inode_update update = write_update(fs);
	uint64_t per_block = device->block_size / sizeof(struct ext4_le32);
	uint64_t blocks = inode.flags & EXT4_INODE_EXTENTS ? UINT32_MAX
							   : EXT4_DIRECT_BLOCKS + per_block +
		per_block * per_block + per_block * per_block * per_block;
	uint64_t free_blocks;
	uint8_t byte = 0xa5;
	size_t completed;
	uint32_t writes;

	if (blocks > UINT32_MAX) {
		blocks = UINT32_MAX;
	}
	EXPECT(
	    ext4_truncate_atomic(fs, inode.number, inode.generation, 0, &update, &after), EXT4_OK);
	free_blocks = fs->info.free_blocks;
	EXPECT(ext4_truncate_atomic(fs, inode.number, inode.generation, blocks * device->block_size,
		   &update, &after),
	    EXT4_OK);
	CHECK(after.size == blocks * device->block_size && after.blocks_512 == 0 &&
	    fs->info.free_blocks == free_blocks);
	EXPECT(ext4_read(fs, &after, after.size - 1, &byte, 1, &completed), EXT4_OK);
	CHECK(completed == 1 && byte == 0);
	writes = device->writes;
	EXPECT(ext4_truncate_atomic(
		   fs, inode.number, inode.generation, after.size + 1, &update, &after),
	    EXT4_RANGE);
	CHECK(device->writes == writes);
	EXPECT(
	    ext4_truncate_atomic(fs, inode.number, inode.generation, 0, &update, &after), EXT4_OK);
	CHECK(after.blocks_512 == 0 && fs->info.free_blocks == free_blocks);
	inode = lookup(fs, "hello.txt");
	EXPECT(
	    ext4_truncate_atomic(fs, inode.number, inode.generation, 3, &update, &after), EXT4_OK);
	alias = lookup(fs, "hello-hardlink");
	CHECK(alias.number == inode.number && alias.links == 2 && alias.size == 3);
	check_contents(fs, "hello-hardlink", (const uint8_t *)"Mac", 3);
	EXPECT(ext4_sync(fs), EXT4_OK);
	ext4_unmount(fs);
	printf("truncate sparse limits: maximum size, no allocation, hardlink identity\n");
}

static void
truncate_mapping_corruption(struct device *device)
{
	struct ext4_fs *fs = mount_writer(device);
	struct ext4_inode inode = lookup(fs, "empty");
	struct ext4_inode after;
	struct ext4_inode_update update = write_update(fs);
	struct ext4_inode_disk *disk;
	struct ext4_extent_header_disk *header;
	struct ext4_extent_index_disk *indices;
	struct ext4_extent_disk *entries;
	struct ext4_le32 *pointers;
	struct ext4_le32 *tail;
	uint8_t *prepared = malloc(device->size);
	uint8_t *before = malloc(device->size);
	uint64_t inode_offset;
	uint64_t node;
	uint64_t replacement;
	size_t tail_offset;
	size_t completed;
	uint32_t index;
	uint32_t test;

	CHECK(prepared != NULL && before != NULL);
	EXPECT(
	    ext4_truncate_atomic(fs, inode.number, inode.generation, 0, &update, &after), EXT4_OK);
	for (index = 0; index < 6; index++) {
		EXPECT(ext4_write(fs, inode.number, inode.generation,
			   (uint64_t)index * 2 * device->block_size, "C", 1, &update, &completed),
		    EXT4_OK);
	}
	if (!(inode.flags & EXT4_INODE_EXTENTS)) {
		EXPECT(ext4_write(fs, inode.number, inode.generation,
			   (uint64_t)EXT4_DIRECT_BLOCKS * device->block_size, "C", 1, &update,
			   &completed),
		    EXT4_OK);
	}
	EXPECT(ext4_inode_location(fs, inode.number, &inode_offset), EXT4_OK);
	EXPECT(ext4_get_inode(fs, inode.number, &inode), EXT4_OK);
	EXPECT(ext4_sync(fs), EXT4_OK);
	ext4_unmount(fs);
	memcpy(prepared, device->stable, device->size);
	for (test = 0; test < 3; test++) {
		device_reset(device, prepared);
		fs = mount_writer(device);
		disk = (struct ext4_inode_disk *)(device->cache + inode_offset);
		if (inode.flags & EXT4_INODE_EXTENTS) {
			header = (struct ext4_extent_header_disk *)disk->block_data;
			CHECK(ext4_le16(&header->depth) == 1 && ext4_le16(&header->entries) == 1);
			indices = (struct ext4_extent_index_disk *)(header + 1);
			node = ext4_le32(&indices[0].child_lo) |
			    (uint64_t)ext4_le16(&indices[0].child_hi) << 32;
			header = (struct ext4_extent_header_disk *)(device->cache +
			    node * device->block_size);
			entries = (struct ext4_extent_disk *)(header + 1);
			CHECK(ext4_le16(&header->entries) == 6);
			replacement = test == 0 ? ext4_le32(&entries[0].physical_lo)
						: (test == 1 ? node : fs->first_data_block);
			ext4_encode32(&entries[5].physical_lo, (uint32_t)replacement);
			ext4_encode16(&entries[5].physical_hi, 0);
			if (fs->metadata_checksum) {
				tail_offset = sizeof(*header) +
				    ext4_le16(&header->maximum) * sizeof(*entries);
				tail = (struct ext4_le32 *)((uint8_t *)header + tail_offset);
				ext4_encode32(tail,
				    ext4_crc32c(ext4_inode_seed(fs, &inode), header, tail_offset));
			}
		} else {
			pointers = (struct ext4_le32 *)disk->block_data;
			replacement = test == 0
			    ? ext4_le32(&pointers[0])
			    : (test == 1 ? ext4_le32(&pointers[EXT4_DIRECT_BLOCKS])
					 : fs->first_data_block);
			ext4_encode32(&pointers[10], (uint32_t)replacement);
			ext4_inode_checksum_set(fs, inode.number, disk);
		}
		memcpy(before, device->cache, device->size);
		EXPECT(ext4_truncate_atomic(fs, inode.number, inode.generation,
			   (uint64_t)2 * device->block_size + 7, &update, &after),
		    EXT4_CORRUPT);
		CHECK(device->writes == 0 && memcmp(before, device->cache, device->size) == 0);
		ext4_unmount(fs);
	}
	free(prepared);
	free(before);
	printf(
	    "truncate malformed mappings: duplicate data, data/node alias, protected metadata\n");
}

static void
truncate_faults(struct device *device)
{
	struct ext4_fs *fs = mount_writer(device);
	struct ext4_inode inode = lookup(fs, "empty");
	struct ext4_inode_update update = write_update(fs);
	struct ext4_inode_disk *disk;
	struct ext4_inode after;
	uint8_t *prepared = malloc(device->size);
	uint8_t *saved = device->base;
	uint8_t *bytes = malloc(device->block_size);
	uint64_t offset;
	uint64_t per_block = device->block_size / sizeof(struct ext4_le32);
	size_t completed;
	uint32_t index;
	uint32_t test;

	CHECK(prepared != NULL && bytes != NULL);
	EXPECT(
	    ext4_truncate_atomic(fs, inode.number, inode.generation, 0, &update, &after), EXT4_OK);
	for (index = 0; index < 6; index++) {
		memset(bytes, (int)(index + 31), device->block_size);
		EXPECT(ext4_write(fs, inode.number, inode.generation,
			   (uint64_t)index * 2 * device->block_size, bytes, device->block_size,
			   &update, &completed),
		    EXT4_OK);
		CHECK(completed == device->block_size);
	}
	if (!(inode.flags & EXT4_INODE_EXTENTS)) {
		offset =
		    (EXT4_DIRECT_BLOCKS + per_block + per_block * per_block) * device->block_size;
		EXPECT(ext4_write(fs, inode.number, inode.generation, offset, bytes,
			   device->block_size, &update, &completed),
		    EXT4_OK);
		EXPECT(ext4_write(fs, inode.number, inode.generation,
			   (EXT4_DIRECT_BLOCKS + per_block) * device->block_size, bytes,
			   device->block_size, &update, &completed),
		    EXT4_OK);
	}
	EXPECT(ext4_sync(fs), EXT4_OK);
	ext4_unmount(fs);
	memcpy(prepared, device->stable, device->size);
	device->base = prepared;
	device->truncate = true;
	for (test = 0; test < 3; test++) {
		device_reset(device, prepared);
		if (test == 2) {
			/* A valid allocated range past EOF must be zeroed when resize
			 * exposes it. Keep this state local to the storage model. */
			fs = mount_writer(device);
			EXPECT(ext4_inode_location(fs, inode.number, &offset), EXT4_OK);
			disk = (struct ext4_inode_disk *)(prepared + offset);
			ext4_encode32(&disk->size_lo, 1);
			ext4_encode32(&disk->size_hi, 0);
			ext4_inode_checksum_set(fs, inode.number, disk);
			ext4_unmount(fs);
			device_reset(device, prepared);
		}
		device->operation_offset = test == 0
		    ? (uint64_t)2 * device->block_size + 7
		    : (test == 1 ? 0 : (uint64_t)11 * device->block_size);
		precommit_failures(device, true);
		device_reset(device, prepared);
		crash_cases(device, true);
	}
	device->base = saved;
	device->truncate = false;
	free(prepared);
	free(bytes);
}

static void
test_image(
    const char *path, const char *export_directory, bool growth, bool truncate, bool export_only)
{
	struct ext4_posix_image image;
	struct ext4_fs *fs;
	struct device device;

	memset(&device, 0, sizeof(device));
	device.source_path = path;
	device.export_directory = export_directory;
	EXPECT(ext4_posix_open(&image, path), EXT4_OK);
	EXPECT(ext4_mount(&image.environment, &fs), EXT4_OK);
	device.size = (size_t)image.environment.size_bytes;
	CHECK(device.size <= TEST_IMAGE_LIMIT);
	device.block_size = fs->info.block_size;
	device.operation_offset = device.block_size - 7;
	device.blocks = (uint32_t)(device.size / device.block_size);
	device.base = malloc(device.size);
	device.cache = malloc(device.size);
	device.stable = malloc(device.size);
	device.dirty = calloc(device.blocks, 1);
	CHECK(device.base && device.cache && device.stable && device.dirty);
	EXPECT(image.environment.read(&image, 0, device.base, device.size), EXT4_OK);
	ext4_unmount(fs);
	CHECK(image.live_allocations == 0);
	ext4_posix_close(&image);
	device.environment.context = &device;
	device.environment.size_bytes = device.size;
	device.environment.read = device_read;
	device.environment.allocate = device_allocate;
	device.environment.release = device_release;
	device.writer.context = &device;
	device.writer.write = device_write;
	device.writer.flush = device_flush;
	device_reset(&device, device.base);
	if (truncate) {
		truncate_operations(&device);
	} else if (growth) {
		allocation_operations(&device);
	} else {
		basic_operations(&device);
	}
	export_state(&device, "");
	if (export_only) {
		goto release;
	}
	if (truncate) {
		device_reset(&device, device.base);
		truncate_preallocation(&device);
		device_reset(&device, device.base);
		truncate_guards(&device);
		device_reset(&device, device.base);
		truncate_sparse_limits(&device);
		device_reset(&device, device.base);
		truncate_mapping_corruption(&device);
		device_reset(&device, device.base);
		truncate_faults(&device);
	} else if (growth) {
		device_reset(&device, device.base);
		allocated_eof_gap(&device);
		device_reset(&device, device.base);
		allocation_guards(&device);
		bitmap_corruption(&device);
		device_reset(&device, device.base);
		full_disk(&device);
		device_reset(&device, device.base);
		indirect_boundaries(&device);
	} else {
		device_reset(&device, device.base);
		timestamp_cases(&device);
		device_reset(&device, device.base);
		rejected_operations(&device);
		device_reset(&device, device.base);
		extra_inode_fields(&device);
		device_reset(&device, device.base);
		metadata_guards(&device);
	}
	if (!truncate) {
		device_reset(&device, device.base);
		precommit_failures(&device, growth);
		device_reset(&device, device.base);
		crash_cases(&device, growth);
		if (growth) {
			device_reset(&device, device.base);
			mapping_faults(&device);
		}
	}
release:
	CHECK(device.live == 0);
	free(device.base);
	free(device.cache);
	free(device.stable);
	free(device.dirty);
	printf("PASS %s: %s\n",
	    truncate ? "truncate and free"
		     : (growth ? "allocation and growth" : "writable inode and file"),
	    path);
}

int
main(int argc, char **argv)
{
	const char *export_directory = NULL;
	bool growth = false;
	bool truncate = false;
	bool export_only = false;
	int index = 1;

	while (index < argc) {
		if (strcmp(argv[index], "--allocation") == 0) {
			growth = true;
			index++;
		} else if (strcmp(argv[index], "--truncate") == 0) {
			truncate = true;
			index++;
		} else if (strcmp(argv[index], "--export-only") == 0) {
			export_only = true;
			index++;
		} else if (strcmp(argv[index], "--export") == 0) {
			CHECK(index + 1 < argc);
			export_directory = argv[index + 1];
			index += 2;
		} else {
			break;
		}
	}
	CHECK(index < argc);
	CHECK(!growth || !truncate);
	CHECK(!export_only || export_directory != NULL);
	for (; index < argc; index++) {
		test_image(argv[index], export_directory, growth, truncate, export_only);
	}
	return 0;
}

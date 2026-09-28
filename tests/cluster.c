/* SPDX-License-Identifier: BSD-3-Clause */
#include "storage.h"
#include "allocate.h"

#define CLUSTER_TEST_SECONDS 1700000300

static void
pattern(uint8_t *bytes, size_t size)
{
	size_t index;

	for (index = 0; index < size; index++) {
		bytes[index] = (uint8_t)(index * 37U + 0x91U);
	}
}

static struct ext4_inode_update
attributes(bool create)
{
	struct ext4_inode_update update = { 0 };

	update.fields = EXT4_ATTR_PERMISSIONS | EXT4_ATTR_MODIFY_TIME | EXT4_ATTR_CHANGE_TIME |
	    EXT4_ATTR_XATTRS;
	if (create) {
		update.fields |= EXT4_ATTR_UID | EXT4_ATTR_GID | EXT4_ATTR_ACCESS_TIME;
	}
	update.permissions = 0640;
	update.uid = 70000;
	update.gid = 80000;
	update.access_time.seconds = CLUSTER_TEST_SECONDS;
	update.modify_time.seconds = CLUSTER_TEST_SECONDS;
	update.change_time.seconds = CLUSTER_TEST_SECONDS;
	return update;
}

static struct ext4_fs *
mount_writer(struct device *device)
{
	struct ext4_fs *fs;

	EXPECT(ext4_mount_writable(&device->environment, &device->writer, &fs), EXT4_OK);
	CHECK(fs->cluster_blocks > 1 && (fs->info.feature_ro_compat & EXT4_FEATURE_RO_BIGALLOC));
	return fs;
}

static struct ext4_inode
find(struct ext4_fs *fs, const struct ext4_inode *parent, const char *name)
{
	struct ext4_inode inode;

	EXPECT(ext4_lookup(fs, parent, (const uint8_t *)name, strlen(name), &inode), EXT4_OK);
	return inode;
}

static struct ext4_inode
lookup(struct ext4_fs *fs, const char *name)
{
	struct ext4_inode root;

	EXPECT(ext4_get_inode(fs, EXT4_ROOT_INODE, &root), EXT4_OK);
	return find(fs, &root, name);
}

static void
bytes_are(struct ext4_fs *fs, const struct ext4_inode *inode, const uint8_t *expected, size_t size)
{
	uint8_t *observed = malloc(size + 1);
	size_t completed;

	CHECK(observed != NULL && inode->size == size);
	observed[size] = 0x5a;
	EXPECT(ext4_read(fs, inode, 0, observed, size + 1, &completed), EXT4_OK);
	CHECK(completed == size && memcmp(observed, expected, size) == 0 && observed[size] == 0x5a);
	free(observed);
}

static void
finish(struct device *device, struct ext4_fs *fs, const char *exports, const char *path,
    const char *label)
{
	EXPECT(ext4_sync(fs), EXT4_OK);
	ext4_unmount(fs);
	CHECK(device->live == 0);
	storage_export(device, exports, path, label);
}

static void
sparse_mutations(struct device *device, const char *exports, const char *path)
{
	struct ext4_fs *fs;
	struct ext4_inode inode;
	struct ext4_inode result;
	struct ext4_inode_update update = attributes(false);
	uint8_t *expected;
	uint8_t *bytes;
	uint64_t free_blocks;
	uint64_t sectors;
	uint64_t progress;
	uint32_t ratio;
	uint32_t offsets[9];
	size_t size;
	size_t index;
	size_t completed;

	device_reset(device, device->base);
	fs = mount_writer(device);
	ratio = fs->cluster_blocks;
	size = (size_t)(9U * ratio + 1U) * device->block_size;
	expected = calloc(1, size);
	bytes = malloc(device->block_size);
	CHECK(expected != NULL && bytes != NULL);
	pattern(bytes, device->block_size);
	offsets[0] = 0;
	offsets[1] = 1;
	offsets[2] = ratio - 1;
	offsets[3] = ratio + 1;
	offsets[4] = 3U * ratio;
	offsets[5] = 3U * ratio + 2;
	offsets[6] = 5U * ratio + 1;
	offsets[7] = 7U * ratio + 1;
	offsets[8] = 9U * ratio;
	for (index = 0; index < 9; index++) {
		memcpy(expected + (size_t)offsets[index] * device->block_size, bytes,
		    device->block_size);
	}
	inode = lookup(fs, "sparse");
	bytes_are(fs, &inode, expected, size);
	free_blocks = fs->info.free_blocks;
	sectors = inode.blocks_512;
	EXPECT(ext4_write(fs, inode.number, inode.generation, 2U * device->block_size, bytes,
		   device->block_size, &update, &completed),
	    EXT4_OK);
	CHECK(completed == device->block_size && fs->info.free_blocks == free_blocks);
	memcpy(expected + 2U * device->block_size, bytes, device->block_size);
	inode = lookup(fs, "sparse");
	CHECK(inode.blocks_512 == sectors);
	bytes_are(fs, &inode, expected, size);
	EXPECT(ext4_fallocate(fs, inode.number, inode.generation, 0, device->block_size,
		   EXT4_FALLOC_KEEP_SIZE | EXT4_FALLOC_PUNCH_HOLE, &update, &progress),
	    EXT4_OK);
	result = lookup(fs, "sparse");
	CHECK(progress == device->block_size && fs->info.free_blocks == free_blocks);
	memset(expected, 0, device->block_size);
	bytes_are(fs, &result, expected, size);
	EXPECT(ext4_fallocate(fs, inode.number, inode.generation, device->block_size,
		   (uint64_t)(ratio - 1U) * device->block_size,
		   EXT4_FALLOC_KEEP_SIZE | EXT4_FALLOC_PUNCH_HOLE, &update, &progress),
	    EXT4_OK);
	result = lookup(fs, "sparse");
	CHECK(fs->info.free_blocks == free_blocks + ratio);
	memset(expected + device->block_size, 0, (size_t)(ratio - 1U) * device->block_size);
	bytes_are(fs, &result, expected, size);
	EXPECT(ext4_write(fs, inode.number, inode.generation, device->block_size, bytes,
		   device->block_size, &update, &completed),
	    EXT4_OK);
	CHECK(fs->info.free_blocks == free_blocks);
	memcpy(expected + device->block_size, bytes, device->block_size);
	inode = lookup(fs, "sparse");
	bytes_are(fs, &inode, expected, size);
	finish(device, fs, exports, path, "cluster-sparse-");
	free(bytes);
	free(expected);
}

static void
file_mutations(struct device *device, const char *exports, const char *path)
{
	struct ext4_fs *fs;
	struct ext4_inode inode;
	struct ext4_inode result;
	struct ext4_inode_update update = attributes(false);
	struct ext4_xattr_change change = { 0 };
	uint8_t *expected;
	uint8_t *value;
	uint64_t progress;
	uint64_t free_blocks;
	size_t cluster;
	size_t size;
	size_t completed;
	size_t value_size;

	device_reset(device, device->base);
	fs = mount_writer(device);
	cluster = (size_t)fs->cluster_blocks * device->block_size;
	size = 3 * cluster + 17;
	expected = calloc(1, size);
	value = malloc(device->block_size + 8U);
	CHECK(expected != NULL && value != NULL);
	pattern(expected, size);
	inode = lookup(fs, "file");
	bytes_are(fs, &inode, expected, size);
	EXPECT(ext4_truncate(
		   fs, inode.number, inode.generation, device->block_size + 7U, &update, &result),
	    EXT4_OK);
	memset(expected + device->block_size + 7U, 0, size - device->block_size - 7U);
	CHECK(result.blocks_512 == 2U * cluster / EXT4_SECTOR_SIZE);
	bytes_are(fs, &result, expected, device->block_size + 7U);
	EXPECT(
	    ext4_truncate(fs, inode.number, inode.generation, 2U * cluster + 9U, &update, &result),
	    EXT4_OK);
	bytes_are(fs, &result, expected, 2U * cluster + 9U);
	memset(value, 0xd7, device->block_size + 8U);
	EXPECT(ext4_write(fs, inode.number, inode.generation, cluster - 3U, value,
		   device->block_size + 8U, &update, &completed),
	    EXT4_OK);
	memcpy(expected + cluster - 3U, value, device->block_size + 8U);
	inode = lookup(fs, "file");
	bytes_are(fs, &inode, expected, 2U * cluster + 9U);
	EXPECT(ext4_sync(fs), EXT4_OK);
	storage_export(device, exports, path, "cluster-file-");
	free_blocks = fs->info.free_blocks;
	pattern(value, 7);
	change.policy = EXT4_XATTR_REPLACE;
	change.name_index = EXT4_XATTR_USER;
	change.name = (const uint8_t *)"value";
	change.name_length = 5;
	change.value = value;
	change.value_size = 7;
	update.xattrs = &change;
	update.xattr_count = 1;
	EXPECT(ext4_set_attributes(fs, inode.number, inode.generation, &update, &result), EXT4_OK);
	CHECK(fs->info.free_blocks == free_blocks);
	memset(value, 0, 7);
	EXPECT(ext4_get_xattr(fs, inode.number, inode.generation, EXT4_XATTR_USER,
		   (const uint8_t *)"value", 5, value, 7, &value_size),
	    EXT4_OK);
	CHECK(value_size == 7 && value[0] == 0x91);
	change.policy = EXT4_XATTR_REMOVE;
	change.value = NULL;
	change.value_size = 0;
	EXPECT(ext4_set_attributes(fs, inode.number, inode.generation, &update, &result), EXT4_OK);
	CHECK(fs->info.free_blocks == free_blocks + fs->cluster_blocks);
	pattern(value, 7);
	change.policy = EXT4_XATTR_CREATE;
	change.value = value;
	change.value_size = 7;
	EXPECT(ext4_set_attributes(fs, inode.number, inode.generation, &update, &result), EXT4_OK);
	CHECK(fs->info.free_blocks == free_blocks + fs->cluster_blocks);
	update.xattrs = NULL;
	update.xattr_count = 0;
	EXPECT(ext4_fallocate(fs, inode.number, inode.generation, 4U * cluster, cluster + 1U,
		   EXT4_FALLOC_KEEP_SIZE, &update, &progress),
	    EXT4_OK);
	result = lookup(fs, "file");
	CHECK(progress == cluster + 1U);
	bytes_are(fs, &result, expected, 2U * cluster + 9U);
	finish(device, fs, exports, path, "cluster-attributes-");
	free(value);
	free(expected);
}

static void
namespace_lifetime(struct device *device, const char *exports, const char *path)
{
	struct ext4_fs *fs;
	struct ext4_inode parent;
	struct ext4_inode directory;
	struct ext4_inode inode;
	struct ext4_inode result;
	struct ext4_inode_hold *hold;
	struct ext4_inode_update create = attributes(true);
	struct ext4_inode_update update = attributes(false);
	struct ext4_timestamp time = { CLUSTER_TEST_SECONDS, 0 };
	uint8_t *bytes;
	uint64_t free_blocks;
	uint32_t free_inodes;
	size_t size;
	size_t completed;

	device_reset(device, device->base);
	fs = mount_writer(device);
	free_blocks = fs->info.free_blocks;
	free_inodes = fs->info.free_inodes;
	parent = lookup(fs, "empty");
	size = (size_t)fs->cluster_blocks * device->block_size + 7U;
	bytes = malloc(size);
	CHECK(bytes != NULL);
	pattern(bytes, size);
	EXPECT(ext4_mkdir(fs, parent.number, parent.generation, (const uint8_t *)"dir", 3, &create,
		   &time, &directory),
	    EXT4_OK);
	EXPECT(ext4_create(fs, directory.number, directory.generation, (const uint8_t *)"file", 4,
		   &create, &time, &inode),
	    EXT4_OK);
	EXPECT(ext4_write(fs, inode.number, inode.generation, 0, bytes, size, &update, &completed),
	    EXT4_OK);
	inode = find(fs, &directory, "file");
	bytes_are(fs, &inode, bytes, size);
	EXPECT(ext4_hold_inode(fs, inode.number, inode.generation, &hold), EXT4_OK);
	EXPECT(ext4_unlink(fs, directory.number, directory.generation, (const uint8_t *)"file", 4,
		   inode.number, inode.generation, &time, &result),
	    EXT4_OK);
	EXPECT(ext4_refresh_inode(hold, &inode), EXT4_OK);
	bytes_are(fs, &inode, bytes, size);
	EXPECT(ext4_truncate(fs, inode.number, inode.generation, 1, &update, &result), EXT4_OK);
	EXPECT(ext4_write(fs, inode.number, inode.generation, size - 1, bytes + size - 1, 1,
		   &update, &completed),
	    EXT4_OK);
	EXPECT(ext4_release_inode(hold), EXT4_OK);
	EXPECT(ext4_rmdir(fs, parent.number, parent.generation, (const uint8_t *)"dir", 3,
		   directory.number, directory.generation, &time, &result),
	    EXT4_OK);
	CHECK(fs->info.free_blocks == free_blocks && fs->info.free_inodes == free_inodes);
	finish(device, fs, exports, path, "cluster-lifetime-");
	free(bytes);
}

#include "cluster_corruption.h"
#include "cluster_faults.h"
#include "cluster_linux.h"
#include "cluster_space.h"
#include "cluster_values.h"

int
main(int argc, char **argv)
{
	struct device device;
	const char *exports = NULL;
	int argument = 1;
	bool faults = false;
	bool smoke = false;
	bool full = false;
	bool values = false;

	CHECK(argc >= 2);
	if (strcmp(argv[argument], "--linux-return") == 0) {
		CHECK(argc == 4);
		storage_open(&device, argv[3]);
		linux_return(&device, argv[2]);
		storage_close(&device);
		return 0;
	}
	if (strcmp(argv[argument], "--faults") == 0 ||
	    strcmp(argv[argument], "--fault-smoke") == 0) {
		faults = true;
		smoke = strcmp(argv[argument], "--fault-smoke") == 0;
		argument++;
		CHECK(argument < argc);
	}
	if (strcmp(argv[argument], "--full") == 0) {
		full = true;
		argument++;
		CHECK(argument < argc);
	}
	if (strcmp(argv[argument], "--values") == 0) {
		values = true;
		argument++;
		CHECK(argument < argc);
	}
	if (strcmp(argv[argument], "--export") == 0) {
		CHECK(argc >= 4);
		exports = argv[++argument];
		argument++;
	}
	for (; argument < argc; argument++) {
		storage_open(&device, argv[argument]);
		if (values) {
			private_values(&device, exports, argv[argument]);
			storage_close(&device);
			continue;
		}
		if (full) {
			full_space(&device, exports, argv[argument]);
			storage_close(&device);
			continue;
		}
		if (faults) {
			mutation_faults(&device, exports, argv[argument], smoke);
			storage_close(&device);
			continue;
		}
		geometry_corruption(&device);
		corruption(&device);
		sparse_mutations(&device, exports, argv[argument]);
		file_mutations(&device, exports, argv[argument]);
		namespace_lifetime(&device, exports, argv[argument]);
		storage_close(&device);
		printf("PASS clustered allocation, holes, attributes and inode lifetime: %s\n",
		    argv[argument]);
	}
	return 0;
}

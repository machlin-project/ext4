/* SPDX-License-Identifier: BSD-3-Clause */
#include "storage.h"
#include "xattr.h"

#define EA_TEST_SECONDS 1700000120
#define EA_TEST_KEYS 8U
#define EA_SMALL_BYTES 13U
#define EA_TARGET_BYTES 100U

static void
pattern(uint8_t *bytes, size_t size, bool changed)
{
	size_t index;

	for (index = 0; index < size; index++) {
		bytes[index] = (uint8_t)(index * (changed ? 17U : 29U) + (changed ? 0x51U : 0x83U));
	}
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

static struct ext4_inode_disk *
record(struct device *device, struct ext4_fs *fs, uint32_t number)
{
	uint64_t offset;

	EXPECT(ext4_inode_location(fs, number, &offset), EXT4_OK);
	return (struct ext4_inode_disk *)(device->cache + offset);
}

static struct ext4_xattr_change
change(enum ext4_xattr_policy policy, const char *name, const void *bytes, size_t size)
{
	struct ext4_xattr_change result = { 0 };

	result.policy = policy;
	result.name_index = EXT4_XATTR_USER;
	result.name = (const uint8_t *)name;
	result.name_length = strlen(name);
	result.value = bytes;
	result.value_size = size;
	return result;
}

static struct ext4_inode_update
attributes(const struct ext4_xattr_change *changes, size_t count, bool create)
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
	update.access_time.seconds = EA_TEST_SECONDS;
	update.modify_time.seconds = EA_TEST_SECONDS;
	update.change_time.seconds = EA_TEST_SECONDS;
	update.xattrs = changes;
	update.xattr_count = count;
	return update;
}

static void
value_is(struct ext4_fs *fs, const struct ext4_inode *inode, const char *name, const void *expected,
    size_t size)
{
	uint8_t *bytes = malloc(size + 1);
	size_t returned = SIZE_MAX;

	CHECK(bytes != NULL);
	memset(bytes, 0xa5, size + 1);
	EXPECT(ext4_get_xattr(fs, inode->number, inode->generation, EXT4_XATTR_USER,
		   (const uint8_t *)name, strlen(name), bytes, size, &returned),
	    EXT4_OK);
	CHECK(returned == size && memcmp(bytes, expected, size) == 0 && bytes[size] == 0xa5);
	free(bytes);
}

static uint32_t
value_number(struct ext4_fs *fs, const struct ext4_inode *inode, const char *name)
{
	struct ext4_xattr_snapshot snapshot;
	uint32_t number = 0;
	size_t index;

	EXPECT(ext4_xattr_open(fs, inode->number, inode->generation, &snapshot), EXT4_OK);
	for (index = 0; index < snapshot.count; index++) {
		if (snapshot.records[index].entry->name_length == strlen(name) &&
		    memcmp(snapshot.records[index].entry + 1, name, strlen(name)) == 0) {
			number = snapshot.records[index].value_inode;
		}
	}
	ext4_xattr_close(&snapshot);
	return number;
}

static void
remove_name(struct ext4_fs *fs, const char *name, const struct ext4_inode *inode)
{
	struct ext4_inode root;
	struct ext4_inode result;
	struct ext4_timestamp time = { EA_TEST_SECONDS, 0 };

	EXPECT(ext4_get_inode(fs, EXT4_ROOT_INODE, &root), EXT4_OK);
	if ((inode->mode & EXT4_MODE_TYPE) == EXT4_MODE_DIRECTORY) {
		EXPECT(ext4_rmdir(fs, root.number, root.generation, (const uint8_t *)name,
			   strlen(name), inode->number, inode->generation, &time, &result),
		    EXT4_OK);
	} else {
		EXPECT(ext4_unlink(fs, root.number, root.generation, (const uint8_t *)name,
			   strlen(name), inode->number, inode->generation, &time, &result),
		    EXT4_OK);
	}
}

static void
mutation(struct device *device, const char *exports, const char *path)
{
	struct ext4_fs *fs;
	struct ext4_inode inode;
	struct ext4_inode result;
	struct ext4_inode hidden;
	struct ext4_xattr_change changes[3];
	struct ext4_inode_update update;
	uint8_t *bytes = malloc(EXT4_XATTR_VALUE_MAX);
	uint64_t free_blocks;
	uint32_t free_inodes;
	uint32_t number;
	size_t medium = 3U * device->block_size + 7U;
	size_t count;

	CHECK(bytes != NULL);
	pattern(bytes, EXT4_XATTR_VALUE_MAX, true);
	device_reset(device, device->base);
	fs = mount_writer(device);
	inode = lookup(fs, "plain");
	free_blocks = fs->info.free_blocks;
	free_inodes = fs->info.free_inodes;
	changes[0] = change(EXT4_XATTR_CREATE, "maximum", bytes, EXT4_XATTR_VALUE_MAX);
	changes[1] = change(EXT4_XATTR_CREATE, "medium", bytes, medium);
	changes[2] = change(EXT4_XATTR_CREATE, "small", bytes, EA_SMALL_BYTES);
	update = attributes(changes, 3, false);
	EXPECT(ext4_set_attributes(fs, inode.number, inode.generation, &update, &result), EXT4_OK);
	CHECK(fs->info.free_inodes == free_inodes - 2);
	value_is(fs, &result, "maximum", bytes, EXT4_XATTR_VALUE_MAX);
	value_is(fs, &result, "medium", bytes, medium);
	number = value_number(fs, &result, "maximum");
	CHECK(number != 0);
	EXPECT(ext4_get_inode(fs, number, &hidden), EXT4_CORRUPT);
	storage_export(device, exports, path, "ea-created-");
	changes[0] = change(EXT4_XATTR_REPLACE, "maximum", bytes, EA_SMALL_BYTES);
	changes[1] = change(EXT4_XATTR_REMOVE, "medium", NULL, 0);
	changes[2] = change(EXT4_XATTR_REMOVE, "small", NULL, 0);
	EXPECT(ext4_set_attributes(fs, inode.number, inode.generation, &update, &result), EXT4_OK);
	CHECK(fs->info.free_inodes == free_inodes);
	value_is(fs, &result, "maximum", bytes, EA_SMALL_BYTES);
	EXPECT(ext4_inode_allocated(fs, number), EXT4_CORRUPT);
	storage_export(device, exports, path, "ea-shrunk-");
	changes[0] = change(EXT4_XATTR_REMOVE, "maximum", NULL, 0);
	update.xattr_count = 1;
	EXPECT(ext4_set_attributes(fs, inode.number, inode.generation, &update, &result), EXT4_OK);
	EXPECT(ext4_list_xattrs(fs, inode.number, inode.generation, NULL, 0, &count), EXT4_OK);
	CHECK(count == 0 && result.blocks_512 == inode.blocks_512 &&
	    fs->info.free_blocks == free_blocks && fs->info.free_inodes == free_inodes);
	inode = lookup(fs, "body");
	free_inodes = fs->info.free_inodes;
	number = value_number(fs, &inode, "maximum");
	changes[0] = change(EXT4_XATTR_REPLACE, "maximum", bytes, EXT4_XATTR_VALUE_MAX - 3U);
	EXPECT(ext4_set_attributes(fs, inode.number, inode.generation, &update, &result), EXT4_OK);
	CHECK(
	    fs->info.free_inodes == free_inodes && value_number(fs, &result, "maximum") != number);
	value_is(fs, &result, "maximum", bytes, EXT4_XATTR_VALUE_MAX - 3U);
	storage_export(device, exports, path, "ea-replaced-");
	ext4_unmount(fs);
	CHECK(device->live == 0);
	free(bytes);
}

static void
creation(struct device *device, const char *exports, const char *path)
{
	static const char *const names[] = { "created-file", "created-directory", "created-fast",
		"created-mapped" };
	struct ext4_fs *fs;
	struct ext4_inode root;
	struct ext4_inode inode;
	struct ext4_inode result;
	struct ext4_inode_hold *hold;
	struct ext4_xattr_change changes[2];
	struct ext4_inode_update update;
	uint8_t *bytes = malloc(EXT4_XATTR_VALUE_MAX);
	uint8_t target[EA_TARGET_BYTES];
	uint8_t output[EA_TARGET_BYTES];
	char prefix[64];
	uint64_t free_blocks;
	uint32_t free_inodes;
	uint32_t events;
	size_t completed;
	size_t length;
	size_t maximum;
	size_t medium;
	unsigned int kind;
	enum ext4_result error;

	CHECK(bytes != NULL);
	pattern(bytes, EXT4_XATTR_VALUE_MAX, true);
	memset(target, 'q', sizeof(target));
	for (kind = 0; kind < sizeof(names) / sizeof(names[0]); kind++) {
		device_reset(device, device->base);
		fs = mount_writer(device);
		EXPECT(ext4_get_inode(fs, EXT4_ROOT_INODE, &root), EXT4_OK);
		free_blocks = fs->info.free_blocks;
		free_inodes = fs->info.free_inodes;
		changes[0] = change(EXT4_XATTR_CREATE, "maximum", bytes, EXT4_XATTR_VALUE_MAX);
		changes[1] =
		    change(EXT4_XATTR_CREATE, "medium", bytes, 3U * device->block_size + 7U);
		update = attributes(changes, 2, true);
		length = kind == 2 ? EA_SMALL_BYTES : sizeof(target);
		maximum = kind == 2 ? EA_SMALL_BYTES : EXT4_XATTR_VALUE_MAX;
		medium = kind == 2 ? EA_SMALL_BYTES : 3U * device->block_size + 7U;
		if (kind == 2) {
			events = device->events;
			EXPECT(ext4_symlink(fs, root.number, root.generation,
				   (const uint8_t *)names[kind], strlen(names[kind]), target,
				   length, &update, &update.change_time, &inode),
			    EXT4_UNSUPPORTED);
			CHECK(device->events == events && fs->info.free_inodes == free_inodes &&
			    fs->info.free_blocks == free_blocks &&
			    memcmp(device->cache, device->base, device->size) == 0);
			changes[0].value_size = maximum;
			changes[1].value_size = medium;
		}
		if (kind == 0) {
			EXPECT(ext4_create(fs, root.number, root.generation,
				   (const uint8_t *)names[kind], strlen(names[kind]), &update,
				   &update.change_time, &inode),
			    EXT4_OK);
		} else if (kind == 1) {
			EXPECT(ext4_mkdir(fs, root.number, root.generation,
				   (const uint8_t *)names[kind], strlen(names[kind]), &update,
				   &update.change_time, &inode),
			    EXT4_OK);
		} else {
			EXPECT(ext4_symlink(fs, root.number, root.generation,
				   (const uint8_t *)names[kind], strlen(names[kind]), target,
				   length, &update, &update.change_time, &inode),
			    EXT4_OK);
			CHECK(inode.fast_symlink == (kind == 2));
			EXPECT(
			    ext4_read(fs, &inode, 0, output, sizeof(output), &completed), EXT4_OK);
			CHECK(completed == length && memcmp(output, target, length) == 0);
		}
		CHECK(fs->info.free_inodes == free_inodes - (kind == 2 ? 1U : 3U));
		value_is(fs, &inode, "maximum", bytes, maximum);
		CHECK(snprintf(prefix, sizeof(prefix), "ea-%s-", names[kind]) > 0);
		storage_export(device, exports, path, prefix);
		EXPECT(ext4_hold_inode(fs, inode.number, inode.generation, &hold), EXT4_OK);
		remove_name(fs, names[kind], &inode);
		EXPECT(ext4_refresh_inode(hold, &result), EXT4_OK);
		CHECK(result.links == 0);
		value_is(fs, &result, "maximum", bytes, maximum);
		changes[0] =
		    change(EXT4_XATTR_REPLACE, "maximum", bytes, EXT4_XATTR_VALUE_MAX - 7U);
		update = attributes(changes, 1, false);
		if (kind == 2) {
			events = device->events;
			EXPECT(ext4_set_attributes(
				   fs, inode.number, inode.generation, &update, &result),
			    EXT4_UNSUPPORTED);
			CHECK(device->events == events);
			changes[0].value_size = maximum;
		}
		EXPECT(ext4_set_attributes(fs, inode.number, inode.generation, &update, &result),
		    EXT4_OK);
		value_is(
		    fs, &result, "maximum", bytes, kind == 2 ? maximum : EXT4_XATTR_VALUE_MAX - 7U);
		error = ext4_release_inode(hold);
		if (error != EXT4_OK) {
			fprintf(stderr, "EA_INODE cleanup failed for %s\n", names[kind]);
		}
		EXPECT(error, EXT4_OK);
		CHECK(fs->last_orphan == 0 && fs->info.free_inodes == free_inodes &&
		    fs->info.free_blocks == free_blocks);
		EXPECT(ext4_sync(fs), EXT4_OK);
		ext4_unmount(fs);
		CHECK(snprintf(prefix, sizeof(prefix), "ea-released-%u-", kind) > 0);
		storage_export(device, exports, path, prefix);
	}
	free(bytes);
}

static void
clone_attributes(struct device *device, struct ext4_fs *fs, const struct ext4_inode *source,
    const struct ext4_inode *alias)
{
	struct ext4_xattr_snapshot snapshot;
	struct ext4_inode_disk *from = record(device, fs, source->number);
	struct ext4_inode_disk *to = record(device, fs, alias->number);
	struct ext4_inode_disk *value;
	struct ext4_xattr_header_disk *header;
	uint64_t references;
	size_t body;
	size_t index;

	EXPECT(ext4_xattr_open(fs, source->number, source->generation, &snapshot), EXT4_OK);
	if (fs->inode_size > EXT4_INODE_BASE_SIZE) {
		CHECK(ext4_le16(&from->extra_size) == ext4_le16(&to->extra_size));
		body = EXT4_INODE_BASE_SIZE + ext4_le16(&from->extra_size);
		memcpy((uint8_t *)to + body, (uint8_t *)from + body, fs->inode_size - body);
	}
	to->xattr_block_lo = from->xattr_block_lo;
	to->xattr_block_hi = from->xattr_block_hi;
	to->blocks_lo = from->blocks_lo;
	to->blocks_hi = from->blocks_hi;
	for (index = 0; index < snapshot.count; index++) {
		if (!snapshot.records[index].inode_storage || snapshot.records[index].external) {
			continue;
		}
		value = record(device, fs, snapshot.records[index].value_inode);
		references = ((uint64_t)ext4_le32(&value->change_time) << 32) |
		    ext4_le32(&value->version_lo);
		references++;
		ext4_encode32(&value->change_time, (uint32_t)(references >> 32));
		ext4_encode32(&value->version_lo, (uint32_t)references);
		ext4_inode_checksum_set(fs, snapshot.records[index].value_inode, value);
	}
	if (snapshot.external_block != 0) {
		header = (struct ext4_xattr_header_disk *)(device->cache +
		    snapshot.external_block * device->block_size);
		ext4_encode32(&header->references, ext4_le32(&header->references) + 1U);
		ext4_xattr_checksum_set(fs, snapshot.external_block, header);
	}
	ext4_inode_checksum_set(fs, alias->number, to);
	ext4_xattr_close(&snapshot);
	memcpy(device->stable, device->cache, device->size);
}

static void
sharing(struct device *device, const char *exports, const char *path, bool copy)
{
	struct ext4_fs *fs;
	struct ext4_inode source;
	struct ext4_inode alias;
	struct ext4_inode result;
	struct ext4_xattr_change edit = change(EXT4_XATTR_REMOVE, "value7", NULL, 0);
	struct ext4_inode_update update = attributes(&edit, 1, false);
	uint8_t *bytes = malloc(3U * device->block_size + 7U);
	uint64_t free_blocks;
	uint32_t free_inodes;
	char name[16];
	unsigned int index;

	CHECK(bytes != NULL);
	pattern(bytes, 3U * device->block_size + 7U, false);
	device_reset(device, device->base);
	fs = mount_writer(device);
	source = lookup(fs, "many");
	alias = lookup(fs, "alias");
	clone_attributes(device, fs, &source, &alias);
	alias = lookup(fs, "alias");
	free_blocks = fs->info.free_blocks;
	free_inodes = fs->info.free_inodes;
	if (copy) {
		storage_export(device, exports, path, "ea-shared-");
		EXPECT(ext4_set_attributes(fs, alias.number, alias.generation, &update, &result),
		    EXT4_OK);
		for (index = 0; index < EA_TEST_KEYS - 1; index++) {
			CHECK(snprintf(name, sizeof(name), "value%u", index) > 0);
			value_is(fs, &result, name, bytes, 3U * device->block_size + 7U);
		}
		storage_export(device, exports, path, "ea-copied-");
	}
	remove_name(fs, "alias", &alias);
	for (index = 0; index < EA_TEST_KEYS; index++) {
		CHECK(snprintf(name, sizeof(name), "value%u", index) > 0);
		value_is(fs, &source, name, bytes, 3U * device->block_size + 7U);
	}
	CHECK(fs->info.free_blocks == free_blocks && fs->info.free_inodes == free_inodes + 1U);
	EXPECT(ext4_sync(fs), EXT4_OK);
	ext4_unmount(fs);
	storage_export(device, exports, path, copy ? "ea-copy-released-" : "ea-detached-");
	free(bytes);
}

static void
data_and_removal(struct device *device, const char *exports, const char *path)
{
	struct ext4_fs *fs;
	struct ext4_inode inode;
	struct ext4_inode result;
	struct ext4_inode_update update = attributes(NULL, 0, false);
	uint8_t *bytes = malloc(EXT4_XATTR_VALUE_MAX);
	uint8_t output[EA_SMALL_BYTES];
	uint64_t free_blocks;
	uint32_t free_inodes;
	size_t completed;

	CHECK(bytes != NULL);
	pattern(bytes, EXT4_XATTR_VALUE_MAX, false);
	device_reset(device, device->base);
	fs = mount_writer(device);
	inode = lookup(fs, "body");
	free_blocks = fs->info.free_blocks;
	free_inodes = fs->info.free_inodes;
	EXPECT(ext4_write(fs, inode.number, inode.generation, 7U * device->block_size, bytes,
		   EA_SMALL_BYTES, &update, &completed),
	    EXT4_OK);
	CHECK(completed == EA_SMALL_BYTES);
	EXPECT(ext4_get_inode(fs, inode.number, &result), EXT4_OK);
	EXPECT(ext4_read(fs, &result, 7U * device->block_size, output, sizeof(output), &completed),
	    EXT4_OK);
	CHECK(completed == sizeof(output) && memcmp(bytes, output, sizeof(output)) == 0);
	value_is(fs, &result, "maximum", bytes, EXT4_XATTR_VALUE_MAX);
	EXPECT(ext4_truncate(fs, inode.number, inode.generation, 0, &update, &result), EXT4_OK);
	CHECK(fs->info.free_blocks == free_blocks && result.blocks_512 == inode.blocks_512);
	value_is(fs, &result, "maximum", bytes, EXT4_XATTR_VALUE_MAX);
	remove_name(fs, "body", &result);
	CHECK(fs->info.free_inodes == free_inodes + 2U && fs->info.free_blocks > free_blocks);
	EXPECT(ext4_sync(fs), EXT4_OK);
	ext4_unmount(fs);
	storage_export(device, exports, path, "ea-data-released-");
	free(bytes);
}

#include "ea_inode_faults.h"
#include "ea_inode_corruption.h"
#include "ea_inode_orphan.h"

int
main(int argc, char **argv)
{
	struct device device;
	const char *exports = NULL;
	bool faults = false;
	bool orphans = false;
	bool smoke = false;
	int argument = 1;

	CHECK(argc >= 2);
	while (argument < argc && argv[argument][0] == '-') {
		if (strcmp(argv[argument], "--faults") == 0 ||
		    strcmp(argv[argument], "--fault-smoke") == 0) {
			faults = true;
			smoke = strcmp(argv[argument], "--fault-smoke") == 0;
			argument++;
		} else if (strcmp(argv[argument], "--value-orphans") == 0) {
			orphans = true;
			argument++;
		} else {
			CHECK(strcmp(argv[argument], "--export") == 0 && argument + 2 < argc);
			exports = argv[argument + 1];
			argument += 2;
		}
	}
	CHECK(!(faults && orphans));
	for (; argument < argc; argument++) {
		storage_open(&device, argv[argument]);
		if (orphans) {
			value_orphan(&device, exports, argv[argument]);
			storage_close(&device);
			continue;
		}
		if (faults) {
			mutation_faults(&device, exports, argv[argument], smoke);
			storage_close(&device);
			continue;
		}
		mutation(&device, exports, argv[argument]);
		corruption(&device);
		creation(&device, exports, argv[argument]);
		sharing(&device, exports, argv[argument], true);
		sharing(&device, exports, argv[argument], false);
		data_and_removal(&device, exports, argv[argument]);
		storage_close(&device);
		printf(
		    "PASS EA_INODE mutation: %s; create/replace/shrink, shared values/block COW, "
		    "held file/directory/symlink cleanup and attributed data I/O\n",
		    argv[argument]);
	}
	return 0;
}

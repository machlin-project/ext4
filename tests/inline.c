/* SPDX-License-Identifier: BSD-3-Clause */
#include "storage.h"
#include "inline.h"

#define INLINE_TEST_SECONDS 1700000200

static void
pattern(uint8_t *bytes, size_t length)
{
	size_t index;

	for (index = 0; index < length; index++) {
		bytes[index] = (uint8_t)(index * 29U + 0x83U);
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
	update.access_time.seconds = INLINE_TEST_SECONDS;
	update.modify_time.seconds = INLINE_TEST_SECONDS;
	update.change_time.seconds = INLINE_TEST_SECONDS;
	return update;
}

static void
bytes_are(struct ext4_fs *fs, const struct ext4_inode *inode, const uint8_t *expected, size_t size)
{
	uint8_t *observed = malloc(size + 11);
	size_t completed;

	CHECK(observed != NULL && inode->size == size);
	memset(observed, 0x5a, size + 11);
	EXPECT(ext4_read(fs, inode, 0, observed, size + 11, &completed), EXT4_OK);
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
reader(struct device *device)
{
	static const unsigned int sizes[] = { 1, 59, 60, 61, 120 };
	struct ext4_fs *fs;
	struct ext4_inode inode;
	struct ext4_inode directory;
	struct ext4_inode child;
	struct ext4_dir_entry entry;
	struct ext4_mapping mapping;
	uint8_t expected[120];
	uint8_t observed[128];
	char name[32];
	size_t completed;
	size_t index;
	size_t offset;
	uint64_t cookie;
	unsigned int seen;

	device_reset(device, device->base);
	EXPECT(ext4_mount(&device->environment, &fs), EXT4_OK);
	pattern(expected, sizeof(expected));
	for (index = 0; index < sizeof(sizes) / sizeof(sizes[0]); index++) {
		CHECK(snprintf(name, sizeof(name), "file%u", sizes[index]) > 0);
		inode = lookup(fs, name);
		CHECK((inode.flags & EXT4_INODE_INLINE_DATA) && inode.blocks_512 == 0);
		bytes_are(fs, &inode, expected, sizes[index]);
		for (offset = 0; offset <= sizes[index]; offset++) {
			memset(observed, 0x5a, sizeof(observed));
			EXPECT(
			    ext4_read(fs, &inode, offset, observed, sizeof(observed), &completed),
			    EXT4_OK);
			CHECK(completed == sizes[index] - offset &&
			    memcmp(observed, expected + offset, completed) == 0 &&
			    observed[completed] == 0x5a);
		}
		EXPECT(ext4_map_read(fs, &inode, 0, 1, &mapping), EXT4_UNSUPPORTED);
	}
	directory = lookup(fs, "entries");
	CHECK(directory.flags & EXT4_INODE_INLINE_DATA);
	cookie = 0;
	seen = 0;
	while (ext4_next_dir(fs, &directory, &cookie, &entry) == EXT4_OK) {
		if (entry.name[0] == 'e') {
			CHECK(
			    entry.name_length == 2 && entry.name[1] >= '0' && entry.name[1] <= '5');
			CHECK(!(seen & (1U << (entry.name[1] - '0'))));
			seen |= 1U << (entry.name[1] - '0');
			child = find(fs, &directory, (const char *)entry.name);
			CHECK(child.number == entry.inode);
		}
	}
	CHECK(seen == 63 && cookie == fs->info.block_size);
	cookie = EXT4_INLINE_DOTS_SIZE + 1;
	EXPECT(ext4_next_dir(fs, &directory, &cookie, &entry), EXT4_CORRUPT);
	child = find(fs, &directory, "..");
	CHECK(child.number == EXT4_ROOT_INODE);
	EXPECT(ext4_lookup(fs, &directory, (const uint8_t *)"absent", 6, &child), EXT4_NOT_FOUND);
	ext4_unmount(fs);
	CHECK(device->writes == 0 && storage_equal(device, device->base));
	puts("PASS inline reads: every offset, both directory regions, lookup and cookies");
}

static void
file_mutations(struct device *device, const char *exports, const char *path)
{
	struct ext4_fs *fs;
	struct ext4_inode inode;
	struct ext4_inode result;
	struct ext4_inode_update update = attributes(false);
	uint8_t *expected = calloc(1, 2 * device->block_size);
	uint8_t replacement[12];
	size_t completed;
	uint64_t progress;
	uint64_t free_blocks;

	CHECK(expected != NULL);
	memset(replacement, 0xd7, sizeof(replacement));
	device_reset(device, device->base);
	fs = mount_writer(device);
	inode = lookup(fs, "file120");
	free_blocks = fs->info.free_blocks;
	pattern(expected, 120);
	EXPECT(ext4_write(fs, inode.number, inode.generation, 56, replacement, sizeof(replacement),
		   &update, &completed),
	    EXT4_OK);
	CHECK(completed == sizeof(replacement));
	memcpy(expected + 56, replacement, sizeof(replacement));
	inode = lookup(fs, "file120");
	bytes_are(fs, &inode, expected, 120);
	CHECK(inode.flags & EXT4_INODE_INLINE_DATA);
	EXPECT(ext4_truncate(fs, inode.number, inode.generation, 59, &update, &result), EXT4_OK);
	memset(expected + 59, 0, 61);
	EXPECT(ext4_truncate(fs, inode.number, inode.generation, 120, &update, &result), EXT4_OK);
	bytes_are(fs, &result, expected, 120);
	EXPECT(ext4_fallocate(fs, inode.number, inode.generation, 50, 20,
		   EXT4_FALLOC_KEEP_SIZE | EXT4_FALLOC_PUNCH_HOLE, &update, &progress),
	    EXT4_OK);
	CHECK(progress == 20);
	memset(expected + 50, 0, 20);
	inode = lookup(fs, "file120");
	bytes_are(fs, &inode, expected, 120);
	CHECK((inode.flags & EXT4_INODE_INLINE_DATA) && inode.blocks_512 == 0 &&
	    fs->info.free_blocks == free_blocks);
	finish(device, fs, exports, path, "inline-kept-");

	device_reset(device, device->base);
	fs = mount_writer(device);
	inode = lookup(fs, "file120");
	memset(expected, 0, 2 * device->block_size);
	pattern(expected, 120);
	memcpy(expected + device->block_size + 3, replacement, sizeof(replacement));
	EXPECT(ext4_write(fs, inode.number, inode.generation, device->block_size + 3, replacement,
		   sizeof(replacement), &update, &completed),
	    EXT4_OK);
	inode = lookup(fs, "file120");
	CHECK(!(inode.flags & EXT4_INODE_INLINE_DATA));
	bytes_are(fs, &inode, expected, device->block_size + 3 + sizeof(replacement));
	finish(device, fs, exports, path, "inline-converted-");

	device_reset(device, device->base);
	fs = mount_writer(device);
	inode = lookup(fs, "file60");
	memset(expected, 0, 2 * device->block_size);
	pattern(expected, 60);
	EXPECT(ext4_truncate(
		   fs, inode.number, inode.generation, device->block_size + 7, &update, &result),
	    EXT4_OK);
	bytes_are(fs, &result, expected, device->block_size + 7);
	EXPECT(ext4_sync(fs), EXT4_OK);
	storage_export(device, exports, path, "inline-truncate-grown-");
	EXPECT(ext4_truncate(fs, inode.number, inode.generation, 0, &update, &result), EXT4_OK);
	CHECK(result.size == 0 && result.blocks_512 == 0);
	finish(device, fs, exports, path, "inline-truncate-zero-");

	device_reset(device, device->base);
	fs = mount_writer(device);
	inode = lookup(fs, "file61");
	EXPECT(ext4_fallocate(fs, inode.number, inode.generation, 0, 3 * device->block_size,
		   EXT4_FALLOC_KEEP_SIZE, &update, &progress),
	    EXT4_OK);
	CHECK(progress == 3 * device->block_size);
	inode = lookup(fs, "file61");
	pattern(expected, 61);
	bytes_are(fs, &inode, expected, 61);
	CHECK(!(inode.flags & EXT4_INODE_INLINE_DATA));
	finish(device, fs, exports, path, "inline-reserved-");
	free(expected);
}

static void
attribute_mutations(struct device *device, const char *exports, const char *path)
{
	struct ext4_fs *fs;
	struct ext4_inode inode;
	struct ext4_inode result;
	struct ext4_inode_update update = attributes(false);
	struct ext4_xattr_change changes[2] = { 0 };
	uint8_t expected[120];
	uint8_t *value = malloc(device->block_size);
	uint8_t *observed = malloc(device->block_size);
	size_t size;
	unsigned int index;

	CHECK(value != NULL && observed != NULL);
	pattern(value, device->block_size);
	pattern(expected, sizeof(expected));
	device_reset(device, device->base);
	fs = mount_writer(device);
	inode = lookup(fs, "file120");
	for (index = 0; index < 2; index++) {
		changes[index].policy = EXT4_XATTR_CREATE;
		changes[index].name_index = EXT4_XATTR_USER;
		changes[index].name = (const uint8_t *)(index == 0 ? "large" : "small");
		changes[index].name_length = 5;
		changes[index].value = value;
		changes[index].value_size = index == 0 ? device->block_size - 120 : 52;
	}
	update.xattrs = changes;
	update.xattr_count = 2;
	EXPECT(ext4_set_attributes(fs, inode.number, inode.generation, &update, &result), EXT4_OK);
	bytes_are(fs, &result, expected, sizeof(expected));
	for (index = 0; index < 2; index++) {
		EXPECT(ext4_get_xattr(fs, inode.number, inode.generation, EXT4_XATTR_USER,
			   changes[index].name, 5, observed, device->block_size, &size),
		    EXT4_OK);
		CHECK(size == changes[index].value_size && memcmp(observed, value, size) == 0);
	}
	changes[0].name_index = EXT4_XATTR_SYSTEM;
	changes[0].name = (const uint8_t *)"data";
	changes[0].name_length = 4;
	update.xattr_count = 1;
	EXPECT(ext4_set_attributes(fs, inode.number, inode.generation, &update, &result),
	    EXT4_INVALID_ARGUMENT);
	finish(device, fs, exports, path, "inline-xattrs-");
	free(observed);
	free(value);
}

static void
directory_mutations(struct device *device, const char *exports, const char *path)
{
	struct ext4_fs *fs;
	struct ext4_inode directory;
	struct ext4_inode empty;
	struct ext4_inode destination;
	struct ext4_inode child;
	struct ext4_inode result;
	struct ext4_inode_update create = attributes(true);
	struct ext4_timestamp time = { INLINE_TEST_SECONDS, 0 };
	struct ext4_rename_entry source;
	struct ext4_rename_entry target;
	char name[96];
	uint64_t free_blocks;
	unsigned int added;
	unsigned int index;

	device_reset(device, device->base);
	fs = mount_writer(device);
	directory = lookup(fs, "entries");
	free_blocks = fs->info.free_blocks;
	child = find(fs, &directory, "e4");
	EXPECT(ext4_unlink(fs, directory.number, directory.generation, (const uint8_t *)"e4", 2,
		   child.number, child.generation, &time, &result),
	    EXT4_OK);
	EXPECT(ext4_create(fs, directory.number, directory.generation, (const uint8_t *)"new", 3,
		   &create, &time, &result),
	    EXT4_OK);
	directory = lookup(fs, "entries");
	CHECK(directory.flags & EXT4_INODE_INLINE_DATA);
	CHECK(fs->info.free_blocks == free_blocks);
	destination = lookup(fs, "destination");
	empty = lookup(fs, "empty");
	memset(&source, 0, sizeof(source));
	source.directory = EXT4_ROOT_INODE;
	source.name = (const uint8_t *)"empty";
	source.name_length = 5;
	source.inode = empty.number;
	source.generation = empty.generation;
	target = source;
	target.directory = destination.number;
	target.directory_generation = destination.generation;
	target.name = (const uint8_t *)"moved";
	target.inode = 0;
	target.generation = 0;
	EXPECT(ext4_rename(fs, &source, &target, 0, &time, &result), EXT4_OK);
	destination = lookup(fs, "destination");
	empty = find(fs, &destination, "moved");
	child = find(fs, &empty, "..");
	CHECK(child.number == destination.number && (empty.flags & EXT4_INODE_INLINE_DATA));
	EXPECT(ext4_rmdir(fs, destination.number, destination.generation, (const uint8_t *)"moved",
		   5, empty.number, empty.generation, &time, &result),
	    EXT4_OK);
	EXPECT(ext4_sync(fs), EXT4_OK);
	storage_export(device, exports, path, "inline-directory-kept-");
	for (index = 0; index < sizeof(name) - 1; index++) {
		name[index] = 'q';
	}
	name[sizeof(name) - 1] = 0;
	for (added = 0; added < 4; added++) {
		name[0] = (char)('a' + added);
		EXPECT(ext4_create(fs, directory.number, directory.generation,
			   (const uint8_t *)name, strlen(name), &create, &time, &result),
		    EXT4_OK);
	}
	directory = lookup(fs, "entries");
	CHECK(!(directory.flags & EXT4_INODE_INLINE_DATA));
	child = find(fs, &directory, "e5");
	CHECK(child.size == 6);
	child = find(fs, &directory, name);
	CHECK(child.number == result.number);
	finish(device, fs, exports, path, "inline-directory-expanded-");
}

#include "inline_lifetime.h"
#include "inline_faults.h"
#include "inline_corruption.h"
#include "inline_linux.h"

int
main(int argc, char **argv)
{
	struct device device;
	const char *exports = NULL;
	bool full = false;
	bool faults = false;
	bool smoke = false;
	int argument = 1;

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
	if (strcmp(argv[argument], "--export") == 0) {
		CHECK(argc >= 4);
		exports = argv[++argument];
		argument++;
	}
	for (; argument < argc; argument++) {
		storage_open(&device, argv[argument]);
		if (faults) {
			mutation_faults(&device, exports, argv[argument], smoke);
			storage_close(&device);
			continue;
		}
		if (full) {
			creation_lifetime(&device, exports, argv[argument], true);
			storage_close(&device);
			continue;
		}
		reader(&device);
		corruption(&device);
		file_mutations(&device, exports, argv[argument]);
		attribute_mutations(&device, exports, argv[argument]);
		directory_mutations(&device, exports, argv[argument]);
		creation_lifetime(&device, exports, argv[argument], false);
		storage_close(&device);
		printf("PASS inline mutations: %s\n", argv[argument]);
	}
	return 0;
}

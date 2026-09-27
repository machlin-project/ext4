/* SPDX-License-Identifier: BSD-3-Clause */
#include "storage.h"

#define SPACE_RESERVE_BLOCKS 8U
#define SPACE_PREPARE_LIMIT 512U
#define SPACE_FAILED_OPERATIONS 6U

static const struct ext4_timestamp mutation_time = { .seconds = 1700000060 };

static struct ext4_inode_update
attributes(void)
{
	struct ext4_inode_update update;

	memset(&update, 0, sizeof(update));
	update.fields = EXT4_ATTR_PERMISSIONS | EXT4_ATTR_UID | EXT4_ATTR_GID |
	    EXT4_ATTR_ACCESS_TIME | EXT4_ATTR_MODIFY_TIME | EXT4_ATTR_CHANGE_TIME;
	update.permissions = 0750;
	update.uid = 70000;
	update.gid = 80000;
	update.access_time = update.modify_time = update.change_time = mutation_time;
	return update;
}

static struct ext4_inode_update
write_attributes(void)
{
	struct ext4_inode_update update = attributes();

	update.fields = EXT4_ATTR_PERMISSIONS | EXT4_ATTR_MODIFY_TIME | EXT4_ATTR_CHANGE_TIME;
	return update;
}

static struct ext4_inode
lookup(struct ext4_fs *fs, const struct ext4_inode *parent, const char *name)
{
	struct ext4_inode inode;

	EXPECT(ext4_lookup(fs, parent, (const uint8_t *)name, strlen(name), &inode), EXT4_OK);
	return inode;
}

static struct ext4_fs *
mount_writer(struct device *device, struct ext4_inode *root)
{
	struct ext4_fs *fs;

	EXPECT(ext4_mount_writable(&device->environment, &device->writer, &fs), EXT4_OK);
	EXPECT(ext4_get_inode(fs, EXT4_ROOT_INODE, root), EXT4_OK);
	return fs;
}

static void
filename(uint8_t name[EXT4_NAME_MAX + 1], unsigned int index)
{
	int length;

	length = snprintf((char *)name, EXT4_NAME_MAX + 1, "capacity-%06u-", index);
	CHECK(length > 0 && length < (int)EXT4_NAME_MAX);
	memset(name + length, 'n', EXT4_NAME_MAX - (size_t)length);
	name[EXT4_NAME_MAX] = 0;
}

static void
full_bitmaps(struct ext4_fs *fs)
{
	struct ext4_group group;
	uint8_t *bitmap = malloc(fs->info.block_size);
	uint32_t index;
	uint32_t byte;

	CHECK(bitmap != NULL && fs->info.free_blocks == 0);
	for (index = 0; index < fs->info.groups; index++) {
		EXPECT(ext4_group_get(fs, index, &group), EXT4_OK);
		CHECK(group.free_blocks == 0 && !(group.flags & EXT4_GROUP_BLOCK_UNINIT));
		EXPECT(ext4_block_read(fs, group.block_bitmap, bitmap), EXT4_OK);
		for (byte = 0; byte < fs->info.block_size; byte++) {
			CHECK(bitmap[byte] == UINT8_MAX);
		}
	}
	free(bitmap);
}

static unsigned int
prepare(struct device *device, const char *exports, const char *path)
{
	struct ext4_fs *fs;
	struct ext4_inode root;
	struct ext4_inode parent;
	struct ext4_inode target;
	struct ext4_inode result;
	uint8_t name[EXT4_NAME_MAX + 1];
	uint8_t *before = malloc(device->size);
	uint32_t writes;
	unsigned int index;
	enum ext4_result error;

	CHECK(before != NULL);
	fs = mount_writer(device, &root);
	full_bitmaps(fs);
	parent = lookup(fs, &root, "indexed");
	target = lookup(fs, &root, "target");
	/* Fill existing leaf slack until the next name really needs allocation.
	 * Successful preparation changes names, never the already full bitmap. */
	for (index = 0; index < SPACE_PREPARE_LIMIT; index++) {
		filename(name, index);
		writes = device->writes;
		memcpy(before, device->cache, device->size);
		error = ext4_link(fs, parent.number, parent.generation, name, EXT4_NAME_MAX,
		    target.number, target.generation, &mutation_time, &result);
		CHECK(fs->info.free_blocks == 0);
		if (error == EXT4_NO_SPACE) {
			CHECK(device->writes == writes &&
			    memcmp(before, device->cache, device->size) == 0);
			break;
		}
		EXPECT(error, EXT4_OK);
	}
	CHECK(index < SPACE_PREPARE_LIMIT);
	EXPECT(ext4_sync(fs), EXT4_OK);
	ext4_unmount(fs);
	CHECK(device->live == 0 && memcmp(device->cache, device->stable, device->size) == 0);
	storage_export(device, exports, path, "space-prepared-");
	free(before);
	return index;
}

static struct ext4_rename_entry
rename_entry(const struct ext4_inode *parent, const uint8_t *name, const struct ext4_inode *inode)
{
	struct ext4_rename_entry entry;

	memset(&entry, 0, sizeof(entry));
	entry.directory = parent->number;
	entry.directory_generation = parent->generation;
	entry.name = name;
	entry.name_length = strlen((const char *)name);
	if (inode != NULL) {
		entry.inode = inode->number;
		entry.generation = inode->generation;
	}
	return entry;
}

static void
failed_names(struct device *device, const uint8_t *baseline, unsigned int candidate)
{
	struct ext4_fs *fs;
	struct ext4_inode root;
	struct ext4_inode parent;
	struct ext4_inode target;
	struct ext4_inode empty;
	struct ext4_inode result;
	struct ext4_inode unchanged;
	struct ext4_inode_update update = attributes();
	struct ext4_rename_entry from;
	struct ext4_rename_entry to;
	uint8_t name[EXT4_NAME_MAX + 1];
	uint8_t long_target[EXT4_INODE_BLOCK_BYTES];
	uint32_t free_inodes;
	uint32_t live;
	unsigned int parent_index;
	unsigned int operation;
	enum ext4_result error = EXT4_OK;

	device_reset(device, baseline);
	fs = mount_writer(device, &root);
	target = lookup(fs, &root, "target");
	empty = lookup(fs, &root, "empty");
	from = rename_entry(&root, (const uint8_t *)"empty", &empty);
	free_inodes = fs->info.free_inodes;
	live = device->live;
	memset(&unchanged, 0xa5, sizeof(unchanged));
	memset(long_target, 'L', sizeof(long_target));
	for (parent_index = 0; parent_index < 2; parent_index++) {
		parent = lookup(fs, &root, parent_index == 0 ? "linear" : "indexed");
		filename(name, parent_index == 0 ? 0 : candidate);
		to = rename_entry(&parent, name, NULL);
		for (operation = 0; operation < SPACE_FAILED_OPERATIONS; operation++) {
			result = unchanged;
			switch (operation) {
			case 0:
				error = ext4_create(fs, parent.number, parent.generation, name,
				    EXT4_NAME_MAX, &update, &mutation_time, &result);
				break;
			case 1:
				error = ext4_mkdir(fs, parent.number, parent.generation, name,
				    EXT4_NAME_MAX, &update, &mutation_time, &result);
				break;
			case 2:
			case 3:
				error = ext4_symlink(fs, parent.number, parent.generation, name,
				    EXT4_NAME_MAX, long_target,
				    operation == 2 ? 1 : sizeof(long_target), &update,
				    &mutation_time, &result);
				break;
			case 4:
				error = ext4_link(fs, parent.number, parent.generation, name,
				    EXT4_NAME_MAX, target.number, target.generation, &mutation_time,
				    &result);
				break;
			case 5:
				error = ext4_rename(fs, &from, &to, 0, &mutation_time, &result);
				break;
			}
			EXPECT(error, EXT4_NO_SPACE);
			CHECK(memcmp(&result, &unchanged, sizeof(result)) == 0 &&
			    fs->info.free_blocks == 0 && fs->info.free_inodes == free_inodes &&
			    device->events == 0 && device->writes == 0 && device->live == live &&
			    memcmp(device->cache, baseline, device->size) == 0 &&
			    memcmp(device->stable, baseline, device->size) == 0);
		}
	}
	full_bitmaps(fs);
	ext4_unmount(fs);
	CHECK(device->live == 0);
	puts("PASS full block bitmaps: 12 namespace ENOSPC operations preserve all bytes and "
	     "output");
}

static void
reuse(struct device *device, const uint8_t *baseline, const char *exports, const char *path)
{
	struct ext4_fs *fs;
	struct ext4_inode root;
	struct ext4_inode room;
	struct ext4_inode target;
	struct ext4_inode empty;
	struct ext4_inode created;
	struct ext4_inode result;
	struct ext4_inode_update update = attributes();
	struct ext4_inode_update write_update = write_attributes();
	struct ext4_rename_entry from;
	struct ext4_rename_entry to;
	uint8_t *before = malloc(device->size);
	uint8_t *bytes = malloc(device->block_size);
	uint32_t writes;
	uint32_t index;
	size_t completed;

	CHECK(before != NULL && bytes != NULL);
	device_reset(device, baseline);
	fs = mount_writer(device, &root);
	room = lookup(fs, &root, "room");
	target = lookup(fs, &root, "target");
	empty = lookup(fs, &root, "empty");
	EXPECT(ext4_create(fs, room.number, room.generation, (const uint8_t *)"created", 7, &update,
		   &mutation_time, &created),
	    EXT4_OK);
	EXPECT(ext4_symlink(fs, room.number, room.generation, (const uint8_t *)"short-link", 10,
		   (const uint8_t *)"../target", 9, &update, &mutation_time, &result),
	    EXT4_OK);
	EXPECT(ext4_link(fs, room.number, room.generation, (const uint8_t *)"hardlink", 8,
		   target.number, target.generation, &mutation_time, &result),
	    EXT4_OK);
	from = rename_entry(&room, (const uint8_t *)"created", &created);
	to = rename_entry(&room, (const uint8_t *)"renamed", NULL);
	EXPECT(ext4_rename(fs, &from, &to, 0, &mutation_time, &result), EXT4_OK);
	EXPECT(
	    ext4_write(fs, target.number, target.generation, 7, "R", 1, &write_update, &completed),
	    EXT4_OK);
	CHECK(completed == 1);
	EXPECT(ext4_truncate_atomic(fs, empty.number, empty.generation,
		   (uint64_t)device->block_size * 3, &write_update, &result),
	    EXT4_OK);
	EXPECT(ext4_read(fs, &result, device->block_size, bytes, device->block_size, &completed),
	    EXT4_OK);
	CHECK(completed == device->block_size);
	for (index = 0; index < device->block_size; index++) {
		CHECK(bytes[index] == 0);
	}
	memcpy(before, device->cache, device->size);
	writes = device->writes;
	completed = SIZE_MAX;
	EXPECT(ext4_write(fs, empty.number, empty.generation, device->block_size, "X", 1,
		   &write_update, &completed),
	    EXT4_NO_SPACE);
	CHECK(completed == 0 && device->writes == writes &&
	    memcmp(before, device->cache, device->size) == 0);
	EXPECT(ext4_truncate_atomic(fs, empty.number, empty.generation, 0, &write_update, &result),
	    EXT4_OK);
	CHECK(result.size == 0 && result.blocks_512 == 0);
	full_bitmaps(fs);
	EXPECT(ext4_sync(fs), EXT4_OK);
	ext4_unmount(fs);
	storage_export(device, exports, path, "space-reused-");
	CHECK(device->live == 0);
	free(bytes);
	free(before);
	puts("PASS full block bitmaps: record reuse, inline symlink, hardlink, rename, overwrite "
	     "and sparse growth");
}

static void
partial_allocation(struct device *device, const uint8_t *baseline, unsigned int candidate,
    bool indexed, const char *exports, const char *path)
{
	struct ext4_fs *fs;
	struct ext4_inode root;
	struct ext4_inode parent;
	struct ext4_inode reserve;
	struct ext4_inode result;
	struct ext4_inode unchanged;
	struct ext4_inode after;
	struct ext4_inode_update update = attributes();
	struct ext4_inode_update write_update = write_attributes();
	uint8_t name[EXT4_NAME_MAX + 1];
	uint8_t *before = malloc(device->size);
	uint32_t free_inodes;
	uint32_t writes;
	uint32_t live;
	unsigned int released;
	enum ext4_result error;

	CHECK(before != NULL);
	filename(name, indexed ? candidate : 0);
	memset(&unchanged, 0xa5, sizeof(unchanged));
	for (released = 1; released <= SPACE_RESERVE_BLOCKS; released++) {
		device_reset(device, baseline);
		fs = mount_writer(device, &root);
		parent = lookup(fs, &root, indexed ? "indexed" : "linear");
		reserve = lookup(fs, &root, "reserve");
		CHECK(reserve.size == (uint64_t)device->block_size * SPACE_RESERVE_BLOCKS);
		EXPECT(ext4_truncate_atomic(fs, reserve.number, reserve.generation,
			   (uint64_t)(SPACE_RESERVE_BLOCKS - released) * device->block_size,
			   &write_update, &result),
		    EXT4_OK);
		CHECK(fs->info.free_blocks == released &&
		    result.blocks_512 ==
			(uint64_t)(SPACE_RESERVE_BLOCKS - released) * device->block_size /
			    EXT4_SECTOR_SIZE);
		free_inodes = fs->info.free_inodes;
		writes = device->writes;
		live = device->live;
		memcpy(before, device->cache, device->size);
		result = unchanged;
		error = ext4_mkdir(fs, parent.number, parent.generation, name, EXT4_NAME_MAX,
		    &update, &mutation_time, &result);
		if (error == EXT4_NO_SPACE) {
			CHECK(memcmp(&result, &unchanged, sizeof(result)) == 0 &&
			    fs->info.free_blocks == released &&
			    fs->info.free_inodes == free_inodes && device->writes == writes &&
			    device->live == live &&
			    memcmp(device->cache, before, device->size) == 0);
			ext4_unmount(fs);
			CHECK(device->live == 0);
			continue;
		}
		EXPECT(error, EXT4_OK);
		CHECK(released >= 2 && fs->info.free_inodes == free_inodes - 1);
		EXPECT(ext4_get_inode(fs, parent.number, &parent), EXT4_OK);
		after = lookup(fs, &parent, (const char *)name);
		CHECK(after.number == result.number && after.generation == result.generation &&
		    after.size == device->block_size &&
		    after.blocks_512 == device->block_size / EXT4_SECTOR_SIZE);
		after = lookup(fs, &result, "..");
		CHECK(after.number == parent.number);
		full_bitmaps(fs);
		EXPECT(ext4_sync(fs), EXT4_OK);
		ext4_unmount(fs);
		storage_export(device, exports, path,
		    indexed ? "space-indexed-created-" : "space-linear-created-");
		break;
	}
	CHECK(released <= SPACE_RESERVE_BLOCKS && device->live == 0);
	free(before);
	printf("PASS full block bitmaps: %s mkdir rolls back with insufficient blocks, succeeds "
	       "with %u released blocks\n",
	    indexed ? "indexed" : "linear", released);
}

int
main(int argc, char **argv)
{
	struct device device;
	uint8_t *baseline;
	const char *exports = NULL;
	unsigned int candidate;
	int argument;

	CHECK(argc >= 2);
	argument = 1;
	if (strcmp(argv[argument], "--export") == 0) {
		CHECK(argc >= 4);
		exports = argv[argument + 1];
		argument += 2;
	}
	for (; argument < argc; argument++) {
		storage_open(&device, argv[argument]);
		candidate = prepare(&device, exports, argv[argument]);
		baseline = malloc(device.size);
		CHECK(baseline != NULL);
		memcpy(baseline, device.stable, device.size);
		failed_names(&device, baseline, candidate);
		reuse(&device, baseline, exports, argv[argument]);
		partial_allocation(&device, baseline, candidate, false, exports, argv[argument]);
		partial_allocation(&device, baseline, candidate, true, exports, argv[argument]);
		memcpy(baseline, device.stable, device.size);
		CHECK(storage_recover(&device, baseline, true) && device.writes == 0 &&
		    memcmp(device.cache, baseline, device.size) == 0);
		free(baseline);
		storage_close(&device);
		printf("PASS full-space namespace: %s; prepared-index-names=%u\n", argv[argument],
		    candidate);
	}
	return 0;
}

/* SPDX-License-Identifier: BSD-3-Clause */
#include "storage.h"

#define FLAG_SECONDS 1700000200
#define FLAG_PASSIVE                                                                               \
	(EXT4_INODE_SYNC | EXT4_INODE_NODUMP | EXT4_INODE_NOATIME | EXT4_INODE_NOTAIL |            \
	    EXT4_INODE_JOURNAL_DATA)

static const struct ext4_timestamp flag_time = { FLAG_SECONDS, 0 };

static struct ext4_inode_update
attributes(bool create)
{
	struct ext4_inode_update update = { 0 };

	update.fields = EXT4_ATTR_PERMISSIONS | EXT4_ATTR_MODIFY_TIME | EXT4_ATTR_CHANGE_TIME |
	    EXT4_ATTR_XATTRS;
	update.permissions = 0640;
	update.modify_time = flag_time;
	update.change_time = flag_time;
	if (create) {
		update.fields |= EXT4_ATTR_UID | EXT4_ATTR_GID | EXT4_ATTR_ACCESS_TIME;
		update.uid = 12345;
		update.gid = 23456;
		update.access_time = flag_time;
	}
	return update;
}

static struct ext4_fs *
mount_file(struct device *device, struct ext4_inode *inode)
{
	struct ext4_fs *fs;
	struct ext4_inode root;

	EXPECT(ext4_mount_writable(&device->environment, &device->writer, &fs), EXT4_OK);
	EXPECT(ext4_get_inode(fs, EXT4_ROOT_INODE, &root), EXT4_OK);
	EXPECT(ext4_lookup(fs, &root, (const uint8_t *)"block", 5, inode), EXT4_OK);
	return fs;
}

static void
set_flags(struct ext4_fs *fs, struct ext4_inode *inode, uint32_t mask, uint32_t flags)
{
	struct ext4_inode before;

	EXPECT(ext4_get_inode(fs, inode->number, &before), EXT4_OK);
	EXPECT(ext4_set_inode_flags(
		   fs, inode->number, inode->generation, mask, flags, &flag_time, inode),
	    EXT4_OK);
	CHECK(inode->flags == ((before.flags & ~mask) | flags));
	CHECK(inode->size == before.size && inode->blocks_512 == before.blocks_512 &&
	    inode->number == before.number && inode->generation == before.generation &&
	    inode->mode == before.mode && inode->uid == before.uid && inode->gid == before.gid &&
	    inode->links == before.links &&
	    inode->modify_time.seconds == before.modify_time.seconds &&
	    inode->modify_time.nanoseconds == before.modify_time.nanoseconds &&
	    inode->access_time.seconds == before.access_time.seconds &&
	    inode->access_time.nanoseconds == before.access_time.nanoseconds &&
	    inode->birth_time.seconds == before.birth_time.seconds &&
	    inode->birth_time.nanoseconds == before.birth_time.nanoseconds &&
	    inode->change_time.seconds == FLAG_SECONDS && inode->change_time.nanoseconds == 0 &&
	    memcmp(inode->block_data, before.block_data, sizeof(inode->block_data)) == 0);
}

static struct ext4_rename_entry
name_entry(const struct ext4_inode *parent, const char *name, const struct ext4_inode *inode)
{
	struct ext4_rename_entry entry = { 0 };

	entry.directory = parent->number;
	entry.directory_generation = parent->generation;
	entry.name = (const uint8_t *)name;
	entry.name_length = strlen(name);
	if (inode != NULL) {
		entry.inode = inode->number;
		entry.generation = inode->generation;
	}
	return entry;
}

static void
file_guards(struct device *device, struct ext4_fs *fs, struct ext4_inode *inode,
    const struct ext4_inode *root, bool immutable)
{
	struct ext4_inode_update update = attributes(false);
	struct ext4_inode_update creation = attributes(true);
	struct ext4_inode output;
	struct ext4_rename_entry source = name_entry(root, "block", inode);
	struct ext4_rename_entry destination = name_entry(root, "flag-moved", NULL);
	uint8_t *before = malloc(device->size);
	uint64_t completed_range;
	size_t completed;
	uint32_t writes = device->writes;

	CHECK(before != NULL);
	memcpy(before, device->cache, device->size);
	EXPECT(ext4_write(fs, inode->number, inode->generation, inode->size + 1U, "x", 1, &update,
		   &completed),
	    EXT4_PERMISSION_DENIED);
	EXPECT(ext4_write_partial(fs, inode->number, inode->generation, inode->size + 1U, "x", 1,
		   &update, &completed),
	    EXT4_PERMISSION_DENIED);
	CHECK(completed == 0);
	EXPECT(ext4_truncate(fs, inode->number, inode->generation, inode->size, &update, &output),
	    EXT4_PERMISSION_DENIED);
	EXPECT(ext4_truncate_atomic(fs, inode->number, inode->generation, 0, &update, &output),
	    EXT4_PERMISSION_DENIED);
	EXPECT(ext4_fallocate(fs, inode->number, inode->generation, 0, 1,
		   EXT4_FALLOC_KEEP_SIZE | EXT4_FALLOC_PUNCH_HOLE, &update, &completed_range),
	    EXT4_PERMISSION_DENIED);
	CHECK(completed_range == 0);
	if (immutable) {
		EXPECT(ext4_write(fs, inode->number, inode->generation, inode->size, "x", 1,
			   &update, &completed),
		    EXT4_PERMISSION_DENIED);
		EXPECT(ext4_fallocate(fs, inode->number, inode->generation, 0, 1, 0, &update,
			   &completed_range),
		    EXT4_PERMISSION_DENIED);
		EXPECT(ext4_set_inode_flags(fs, inode->number, inode->generation, EXT4_INODE_NODUMP,
			   0, &flag_time, &output),
		    EXT4_PERMISSION_DENIED);
	}
	EXPECT(ext4_set_attributes(fs, inode->number, inode->generation, &update, &output),
	    EXT4_PERMISSION_DENIED);
	EXPECT(ext4_link(fs, root->number, root->generation, (const uint8_t *)"flag-alias", 10,
		   inode->number, inode->generation, &flag_time, &output),
	    EXT4_PERMISSION_DENIED);
	EXPECT(ext4_unlink(fs, root->number, root->generation, (const uint8_t *)"block", 5,
		   inode->number, inode->generation, &flag_time, &output),
	    EXT4_PERMISSION_DENIED);
	EXPECT(
	    ext4_rename(fs, &source, &destination, 0, &flag_time, &output), EXT4_PERMISSION_DENIED);
	creation.permissions = 0;
	EXPECT(ext4_rename_whiteout(fs, &source, &destination, 0, &creation, &flag_time, &output),
	    EXT4_PERMISSION_DENIED);
	CHECK(device->writes == writes && !fs->aborted &&
	    memcmp(before, device->cache, device->size) == 0);
	free(before);
}

static void
operations(struct device *device, const char *exports, const char *path)
{
	struct ext4_inode file;
	struct ext4_inode root;
	struct ext4_inode directory;
	struct ext4_inode child;
	struct ext4_inode nested;
	struct ext4_inode link;
	struct ext4_inode fifo;
	struct ext4_inode output;
	struct ext4_inode sentinel;
	struct ext4_inode_hold *hold;
	struct ext4_fs *fs = mount_file(device, &file);
	struct ext4_inode_update create = attributes(true);
	struct ext4_inode_update write = attributes(false);
	struct ext4_special_file special = { .type = EXT4_FT_FIFO };
	struct ext4_rename_entry source;
	struct ext4_rename_entry destination;
	struct ext4_timestamp invalid_time = { FLAG_SECONDS, 1000000000U };
	uint8_t *before = malloc(device->size);
	uint8_t bytes[4] = { 0 };
	uint64_t completed_range;
	size_t completed;
	uint32_t writes;
	uint32_t index;
	uint32_t passive = FLAG_PASSIVE;

	CHECK(before != NULL);
	EXPECT(ext4_get_inode(fs, EXT4_ROOT_INODE, &root), EXT4_OK);
	memset(&sentinel, 0xa5, sizeof(sentinel));
	output = sentinel;
	EXPECT(ext4_set_inode_flags(fs, file.number, file.generation, 0, 0, &flag_time, &output),
	    EXT4_INVALID_ARGUMENT);
	EXPECT(ext4_set_inode_flags(fs, file.number, file.generation, EXT4_INODE_NODUMP,
		   EXT4_INODE_NOATIME, &flag_time, &output),
	    EXT4_INVALID_ARGUMENT);
	EXPECT(ext4_set_inode_flags(
		   fs, file.number, file.generation, EXT4_INODE_EXTENTS, 0, &flag_time, &output),
	    EXT4_UNSUPPORTED);
	EXPECT(ext4_set_inode_flags(fs, file.number, file.generation, EXT4_INODE_DIRSYNC,
		   EXT4_INODE_DIRSYNC, &flag_time, &output),
	    EXT4_INVALID_ARGUMENT);
	EXPECT(ext4_set_inode_flags(fs, file.number, file.generation + 1U, EXT4_INODE_NODUMP, 0,
		   &flag_time, &output),
	    EXT4_STALE);
	EXPECT(ext4_set_inode_flags(fs, file.number, file.generation, EXT4_INODE_NODUMP,
		   EXT4_INODE_NODUMP, &invalid_time, &output),
	    EXT4_RANGE);
	CHECK(device->writes == 0 && memcmp(&output, &sentinel, sizeof(output)) == 0 &&
	    storage_equal(device, device->base));
	set_flags(fs, &file, EXT4_INODE_MODIFIABLE_FLAGS, passive);
	EXPECT(ext4_sync(fs), EXT4_OK);
	storage_export(device, exports, path, "flags-passive-");
	set_flags(fs, &file, EXT4_INODE_IMMUTABLE, EXT4_INODE_IMMUTABLE);
	file_guards(device, fs, &file, &root, true);
	set_flags(fs, &file, EXT4_INODE_IMMUTABLE | EXT4_INODE_APPEND, EXT4_INODE_APPEND);
	file_guards(device, fs, &file, &root, false);
	EXPECT(
	    ext4_write(fs, file.number, file.generation, file.size, "abc", 3, &write, &completed),
	    EXT4_OK);
	CHECK(completed == 3);
	EXPECT(ext4_get_inode(fs, file.number, &file), EXT4_OK);
	EXPECT(ext4_read(fs, &file, 0, bytes, sizeof(bytes), &completed), EXT4_OK);
	CHECK(completed == 3 && memcmp(bytes, "abc", 3) == 0);
	writes = device->writes;
	EXPECT(ext4_write(fs, file.number, file.generation, 0, "x", 1, &write, &completed),
	    EXT4_PERMISSION_DENIED);
	CHECK(completed == 0 && device->writes == writes);
	if (file.flags & EXT4_INODE_EXTENTS) {
		EXPECT(ext4_fallocate(fs, file.number, file.generation, 0, device->block_size,
			   EXT4_FALLOC_KEEP_SIZE, &write, &completed_range),
		    EXT4_OK);
		CHECK(completed_range == device->block_size);
	}
	EXPECT(ext4_mkdir(fs, root.number, root.generation, (const uint8_t *)"flag-dir", 8, &create,
		   &flag_time, &directory),
	    EXT4_OK);
	set_flags(fs, &directory, EXT4_INODE_MODIFIABLE_FLAGS,
	    passive | EXT4_INODE_DIRSYNC | EXT4_INODE_TOPDIR);
	EXPECT(ext4_create(fs, directory.number, directory.generation, (const uint8_t *)"child", 5,
		   &create, &flag_time, &child),
	    EXT4_OK);
	EXPECT(ext4_mkdir(fs, directory.number, directory.generation, (const uint8_t *)"nested", 6,
		   &create, &flag_time, &nested),
	    EXT4_OK);
	EXPECT(ext4_symlink(fs, directory.number, directory.generation, (const uint8_t *)"link", 4,
		   (const uint8_t *)"child", 5, &create, &flag_time, &link),
	    EXT4_OK);
	EXPECT(ext4_mknod(fs, directory.number, directory.generation, (const uint8_t *)"fifo", 4,
		   &special, &create, &flag_time, &fifo),
	    EXT4_OK);
	CHECK((child.flags & EXT4_INODE_MODIFIABLE_FLAGS) == passive &&
	    (nested.flags & EXT4_INODE_MODIFIABLE_FLAGS) == (passive | EXT4_INODE_DIRSYNC) &&
	    (link.flags & EXT4_INODE_MODIFIABLE_FLAGS) ==
		(EXT4_INODE_NODUMP | EXT4_INODE_NOATIME) &&
	    (fifo.flags & EXT4_INODE_MODIFIABLE_FLAGS) == (EXT4_INODE_NODUMP | EXT4_INODE_NOATIME));
	for (index = 0; index < 2; index++) {
		struct ext4_inode *other = index == 0 ? &link : &fifo;

		writes = device->writes;
		EXPECT(ext4_set_inode_flags(fs, other->number, other->generation, EXT4_INODE_SYNC,
			   EXT4_INODE_SYNC, &flag_time, &output),
		    EXT4_INVALID_ARGUMENT);
		CHECK(device->writes == writes);
		set_flags(fs, other, EXT4_INODE_NODUMP, 0);
	}
	/* Append-only directories admit additions, including moving in a name. */
	set_flags(fs, &directory, EXT4_INODE_APPEND, EXT4_INODE_APPEND);
	EXPECT(ext4_create(fs, root.number, root.generation, (const uint8_t *)"incoming", 8,
		   &create, &flag_time, &output),
	    EXT4_OK);
	source = name_entry(&root, "incoming", &output);
	destination = name_entry(&directory, "incoming", NULL);
	EXPECT(ext4_rename(fs, &source, &destination, 0, &flag_time, &output), EXT4_OK);
	source = name_entry(&directory, "incoming", &output);
	destination = name_entry(&root, "outgoing", NULL);
	memcpy(before, device->cache, device->size);
	writes = device->writes;
	EXPECT(
	    ext4_rename(fs, &source, &destination, 0, &flag_time, &output), EXT4_PERMISSION_DENIED);
	EXPECT(ext4_unlink(fs, directory.number, directory.generation, (const uint8_t *)"child", 5,
		   child.number, child.generation, &flag_time, &output),
	    EXT4_PERMISSION_DENIED);
	EXPECT(ext4_rmdir(fs, directory.number, directory.generation, (const uint8_t *)"nested", 6,
		   nested.number, nested.generation, &flag_time, &output),
	    EXT4_PERMISSION_DENIED);
	CHECK(device->writes == writes && memcmp(before, device->cache, device->size) == 0);
	set_flags(fs, &directory, EXT4_INODE_APPEND | EXT4_INODE_IMMUTABLE, EXT4_INODE_IMMUTABLE);
	memcpy(before, device->cache, device->size);
	writes = device->writes;
	EXPECT(ext4_create(fs, directory.number, directory.generation, (const uint8_t *)"denied", 6,
		   &create, &flag_time, &output),
	    EXT4_PERMISSION_DENIED);
	CHECK(device->writes == writes && memcmp(before, device->cache, device->size) == 0);
	EXPECT(ext4_sync(fs), EXT4_OK);
	storage_export(device, exports, path, "flags-protected-");
	/* An admitted flag change on an open, unlinked inode cannot prevent its
	 * final release or offline orphan cleanup from reclaiming storage. */
	set_flags(fs, &file, EXT4_INODE_RESTRICTED_FLAGS, 0);
	EXPECT(ext4_hold_inode(fs, file.number, file.generation, &hold), EXT4_OK);
	EXPECT(ext4_unlink(fs, root.number, root.generation, (const uint8_t *)"block", 5,
		   file.number, file.generation, &flag_time, &output),
	    EXT4_OK);
	EXPECT(ext4_set_inode_flags(fs, file.number, file.generation, EXT4_INODE_RESTRICTED_FLAGS,
		   EXT4_INODE_RESTRICTED_FLAGS, &flag_time, &output),
	    EXT4_OK);
	EXPECT(ext4_release_inode(hold), EXT4_OK);
	EXPECT(ext4_get_inode(fs, file.number, &output), EXT4_NOT_FOUND);
	EXPECT(ext4_sync(fs), EXT4_OK);
	ext4_unmount(fs);
	device_reset(device, device->stable);
	EXPECT(ext4_mount(&device->environment, &fs), EXT4_OK);
	EXPECT(ext4_set_inode_flags(fs, directory.number, directory.generation,
		   EXT4_INODE_IMMUTABLE, 0, &flag_time, &output),
	    EXT4_READ_ONLY);
	ext4_unmount(fs);
	CHECK(device->writes == 0);
	free(before);
	puts("PASS persistent flags, inode protections, append writes, inheritance and held "
	     "cleanup");
}

static void
faults(struct device *device, bool clear, const char *exports, const char *path)
{
	struct ext4_inode inode;
	struct ext4_inode output;
	struct ext4_inode sentinel;
	struct ext4_fs *fs = mount_file(device, &inode);
	uint8_t *original = device->base;
	uint8_t *prepared = malloc(device->size);
	uint8_t *expected = malloc(device->size);
	uint32_t desired = clear ? 0 : EXT4_INODE_IMMUTABLE | EXT4_INODE_NODUMP;
	uint32_t mask = EXT4_INODE_IMMUTABLE | EXT4_INODE_NODUMP;
	uint32_t allocations;
	uint32_t reads;
	uint32_t events;
	uint32_t barrier;
	uint32_t phase;
	uint32_t position;
	uint32_t count;
	uint32_t partial;
	uint32_t survival;
	uint32_t cuts = 0;
	uint32_t recovered = 0;
	bool committed;
	char prefix[96];
	enum ext4_result error;

	CHECK(prepared != NULL && expected != NULL);
	if (clear) {
		set_flags(fs, &inode, mask, mask);
	}
	EXPECT(ext4_sync(fs), EXT4_OK);
	ext4_unmount(fs);
	memcpy(prepared, device->stable, device->size);
	device->base = prepared;
	device_reset(device, prepared);
	snprintf(prefix, sizeof(prefix), "flags-%s-before-", clear ? "clear" : "set");
	storage_export(device, exports, path, prefix);
	fs = mount_file(device, &inode);
	device->reads = device->allocations = 0;
	EXPECT(ext4_set_inode_flags(
		   fs, inode.number, inode.generation, mask, desired, &flag_time, &output),
	    EXT4_OK);
	allocations = device->allocations;
	reads = device->reads;
	events = device->events;
	barrier = device->commit_barrier;
	CHECK(barrier != 0 && barrier < events);
	EXPECT(ext4_sync(fs), EXT4_OK);
	memcpy(expected, device->stable, device->size);
	snprintf(prefix, sizeof(prefix), "flags-%s-after-", clear ? "clear" : "set");
	storage_export(device, exports, path, prefix);
	ext4_unmount(fs);
	memset(&sentinel, 0xa5, sizeof(sentinel));
	for (phase = 0; exports == NULL && phase < 2; phase++) {
		count = phase == 0 ? allocations : reads;
		for (position = 1; position <= count; position++) {
			device_reset(device, prepared);
			fs = mount_file(device, &inode);
			output = sentinel;
			device->reads = device->allocations = 0;
			device->fail_allocation = phase == 0 ? position : 0;
			device->fail_read = phase == 1 ? position : 0;
			error = ext4_set_inode_flags(
			    fs, inode.number, inode.generation, mask, desired, &flag_time, &output);
			EXPECT(error, phase == 0 ? EXT4_NO_MEMORY : EXT4_IO);
			CHECK(memcmp(&output, &sentinel, sizeof(output)) == 0);
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
				output = sentinel;
				device->stop_at = position;
				device->partial = partial != 0;
				device->survival = survival;
				EXPECT(ext4_set_inode_flags(fs, inode.number, inode.generation,
					   mask, desired, &flag_time, &output),
				    EXT4_IO);
				CHECK(fs->aborted && device->off &&
				    memcmp(&output, &sentinel, sizeof(output)) == 0);
				committed = device->intent_durable;
				ext4_unmount(fs);
				snprintf(prefix, sizeof(prefix), "flags-%s-%s-",
				    clear ? "clear" : "set", committed ? "pending" : "uncommitted");
				storage_export(device, exports, path, prefix);
				recovered += storage_recover(device, expected, committed) ? 1U : 0U;
				cuts++;
			}
		}
	}
	device->base = original;
	free(expected);
	free(prepared);
	printf("PASS %s flag faults: allocations=%u reads=%u cuts=%u recovered=%u "
	       "torn_super_fail_closed=%u\n",
	    clear ? "clear" : "set", allocations, reads, cuts, recovered, cuts - recovered);
}

static void
linux_read(struct device *device)
{
	struct ext4_fs *fs;
	struct ext4_inode root;
	struct ext4_inode file;
	struct ext4_inode directory;
	struct ext4_inode child;
	uint8_t data[5];
	size_t count;

	EXPECT(ext4_mount(&device->environment, &fs), EXT4_OK);
	EXPECT(ext4_get_inode(fs, EXT4_ROOT_INODE, &root), EXT4_OK);
	EXPECT(ext4_lookup(fs, &root, (const uint8_t *)"block", 5, &file), EXT4_OK);
	CHECK((file.flags & EXT4_INODE_MODIFIABLE_FLAGS) ==
	    (EXT4_INODE_IMMUTABLE | EXT4_INODE_NODUMP | EXT4_INODE_NOATIME));
	EXPECT(ext4_read(fs, &file, 0, data, sizeof(data), &count), EXT4_OK);
	CHECK((count == 1 && data[0] == 'Z') || (count == 4 && memcmp(data, "abcZ", 4) == 0));
	EXPECT(ext4_lookup(fs, &root, (const uint8_t *)"linux-flag-dir", 14, &directory), EXT4_OK);
	CHECK((directory.flags & EXT4_INODE_MODIFIABLE_FLAGS) ==
	    (EXT4_INODE_APPEND | EXT4_INODE_NODUMP | EXT4_INODE_NOATIME | EXT4_INODE_DIRSYNC));
	EXPECT(ext4_lookup(fs, &directory, (const uint8_t *)"child", 5, &child), EXT4_OK);
	CHECK((child.flags & EXT4_INODE_MODIFIABLE_FLAGS) ==
	    (EXT4_INODE_NODUMP | EXT4_INODE_NOATIME));
	EXPECT(ext4_read(fs, &child, 0, data, sizeof(data), &count), EXT4_OK);
	CHECK(count == 1 && data[0] == 'L');
	ext4_unmount(fs);
	CHECK(device->writes == 0 && storage_equal(device, device->base));
	puts("PASS portable reads of Linux protection flags, inheritance and exact appended data");
}

int
main(int argc, char **argv)
{
	struct device device;
	const char *exports = NULL;
	bool fault_mode = false;
	bool read_mode = false;
	int index = 1;

	if (index < argc && strcmp(argv[index], "--linux-read") == 0) {
		read_mode = true;
		index++;
	}
	if (index < argc && strcmp(argv[index], "--faults") == 0) {
		fault_mode = true;
		index++;
	}
	if (index + 1 < argc && strcmp(argv[index], "--export") == 0) {
		exports = argv[index + 1];
		index += 2;
	}
	CHECK(index < argc);
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
		}
		if (fault_mode || exports != NULL) {
			device_reset(&device, device.base);
			faults(&device, false, exports, argv[index]);
			device_reset(&device, device.base);
			faults(&device, true, exports, argv[index]);
		}
		storage_close(&device);
	}
	return 0;
}

/* SPDX-License-Identifier: BSD-3-Clause */
#include "storage.h"
#include "allocate.h"

#define TEST_RENAME_DATA_BLOCKS 35U
#define TEST_RENAME_SMALL_CREDITS 4U

enum test_kind { TEST_REGULAR, TEST_DIRECTORY, TEST_INLINE_SYMLINK, TEST_MAPPED_SYMLINK };

static unsigned int functional_sequences;

static struct ext4_inode_update
attributes(enum test_kind kind)
{
	struct ext4_inode_update update;

	memset(&update, 0, sizeof(update));
	update.fields = EXT4_ATTR_PERMISSIONS | EXT4_ATTR_UID | EXT4_ATTR_GID |
	    EXT4_ATTR_ACCESS_TIME | EXT4_ATTR_MODIFY_TIME | EXT4_ATTR_CHANGE_TIME;
	update.permissions = kind == TEST_DIRECTORY ? 0750 : kind == TEST_REGULAR ? 0640 : 0777;
	update.uid = UINT32_MAX - 2U;
	update.gid = UINT32_C(0x81234567);
	update.access_time.seconds = 1700000000;
	update.modify_time.seconds = 1700000001;
	update.change_time.seconds = 1700000002;
	return update;
}

static struct ext4_fs *
mount_writer(struct device *device, struct ext4_inode *root)
{
	struct ext4_fs *fs;

	EXPECT(ext4_mount_writable(&device->environment, &device->writer, &fs), EXT4_OK);
	EXPECT(ext4_get_inode(fs, EXT4_ROOT_INODE, root), EXT4_OK);
	return fs;
}

static struct ext4_inode
create(struct ext4_fs *fs, const struct ext4_inode *parent, const char *name, enum test_kind kind,
    uint8_t value)
{
	struct ext4_inode_update update = attributes(kind);
	struct ext4_inode inode;
	uint8_t target[EXT4_INODE_BLOCK_BYTES];
	size_t completed;

	if (kind == TEST_DIRECTORY) {
		EXPECT(ext4_mkdir(fs, parent->number, parent->generation, (const uint8_t *)name,
			   strlen(name), &update, &update.change_time, &inode),
		    EXT4_OK);
	} else if (kind == TEST_REGULAR) {
		EXPECT(ext4_create(fs, parent->number, parent->generation, (const uint8_t *)name,
			   strlen(name), &update, &update.change_time, &inode),
		    EXT4_OK);
		update.fields =
		    EXT4_ATTR_PERMISSIONS | EXT4_ATTR_MODIFY_TIME | EXT4_ATTR_CHANGE_TIME;
		EXPECT(ext4_write(
			   fs, inode.number, inode.generation, 0, &value, 1, &update, &completed),
		    EXT4_OK);
		CHECK(completed == 1);
		EXPECT(ext4_get_inode(fs, inode.number, &inode), EXT4_OK);
	} else {
		memset(target, value, sizeof(target));
		EXPECT(ext4_symlink(fs, parent->number, parent->generation, (const uint8_t *)name,
			   strlen(name), target,
			   sizeof(target) - (kind == TEST_INLINE_SYMLINK ? 1U : 0U), &update,
			   &update.change_time, &inode),
		    EXT4_OK);
	}
	return inode;
}

static struct ext4_rename_entry
entry(const struct ext4_inode *parent, const char *name, const struct ext4_inode *inode)
{
	struct ext4_rename_entry result;

	memset(&result, 0, sizeof(result));
	result.directory = parent->number;
	result.directory_generation = parent->generation;
	result.name = (const uint8_t *)name;
	result.name_length = strlen(name);
	if (inode != NULL) {
		result.inode = inode->number;
		result.generation = inode->generation;
	}
	return result;
}

static void
contents(struct ext4_fs *fs, const struct ext4_inode *inode, enum test_kind kind, uint8_t value,
    uint32_t parent)
{
	struct ext4_inode child;
	uint8_t bytes[EXT4_INODE_BLOCK_BYTES];
	size_t completed;
	size_t length =
	    kind == TEST_REGULAR ? 1 : sizeof(bytes) - (kind == TEST_INLINE_SYMLINK ? 1U : 0U);
	size_t index;

	if (kind == TEST_DIRECTORY) {
		EXPECT(ext4_lookup(fs, inode, (const uint8_t *)"..", 2, &child), EXT4_OK);
		CHECK(child.number == parent);
		EXPECT(ext4_lookup(fs, inode, (const uint8_t *)"inside", 6, &child), EXT4_OK);
		contents(fs, &child, TEST_REGULAR, value, 0);
	} else {
		EXPECT(ext4_read(fs, inode, 0, bytes, sizeof(bytes), &completed), EXT4_OK);
		CHECK(completed == length);
		for (index = 0; index < length; index++) {
			CHECK(bytes[index] == value);
		}
	}
}

static void
sequence(struct device *device, enum test_kind source_kind, enum test_kind target_kind,
    bool replace, bool exchange, bool same_parent, bool reverse, bool held)
{
	struct ext4_fs *fs;
	struct ext4_inode root;
	struct ext4_inode parents[2];
	struct ext4_inode objects[2];
	struct ext4_inode observed;
	struct ext4_inode result;
	struct ext4_rename_entry names[2];
	struct ext4_inode_hold *hold = NULL;
	struct ext4_timestamp time = { .seconds = 1700000050 };
	uint8_t *clean = malloc(device->size);
	uint32_t free_inodes;
	int delta[2];
	unsigned int index;
	unsigned int current;
	bool directory[2] = { source_kind == TEST_DIRECTORY,
		replace && target_kind == TEST_DIRECTORY };

	CHECK(clean != NULL);
	functional_sequences++;
	device_reset(device, device->base);
	fs = mount_writer(device, &root);
	parents[0] = create(fs, &root, "left", TEST_DIRECTORY, 0);
	parents[1] = same_parent ? parents[0] : create(fs, &root, "right", TEST_DIRECTORY, 0);
	for (index = 0; index < 2; index++) {
		current = reverse ? 1U - index : index;
		if (current == 1 && !replace) {
			continue;
		}
		objects[current] = create(fs, &parents[current], current ? "destination" : "source",
		    current ? target_kind : source_kind, current ? 't' : 's');
		if (directory[current] && (current == 0 || exchange)) {
			(void)create(
			    fs, &objects[current], "inside", TEST_REGULAR, current ? 't' : 's');
		}
	}
	for (index = 0; index < 2; index++) {
		EXPECT(ext4_get_inode(fs, parents[index].number, &parents[index]), EXT4_OK);
	}
	names[0] = entry(&parents[0], "source", &objects[0]);
	names[1] = entry(&parents[1], "destination", replace ? &objects[1] : NULL);
	if (held) {
		CHECK(replace && !exchange);
		EXPECT(
		    ext4_hold_inode(fs, objects[1].number, objects[1].generation, &hold), EXT4_OK);
	}
	free_inodes = fs->info.free_inodes;
	EXPECT(ext4_rename(
		   fs, &names[0], &names[1], exchange ? EXT4_RENAME_EXCHANGE : 0, &time, &result),
	    EXT4_OK);
	CHECK(result.number == objects[0].number && result.generation == objects[0].generation &&
	    result.links == objects[0].links && result.change_time.seconds == time.seconds);
	EXPECT(
	    ext4_lookup(fs, &parents[1], (const uint8_t *)"destination", 11, &observed), EXT4_OK);
	CHECK(observed.number == objects[0].number);
	contents(fs, &observed, source_kind, 's', parents[1].number);
	EXPECT(ext4_lookup(fs, &parents[0], (const uint8_t *)"source", 6, &observed),
	    exchange ? EXT4_OK : EXT4_NOT_FOUND);
	if (exchange) {
		CHECK(observed.number == objects[1].number &&
		    observed.change_time.seconds == time.seconds);
		contents(fs, &observed, target_kind, 't', parents[0].number);
	}
	delta[0] = same_parent ? -(int)(replace && !exchange && directory[1])
			       : -(int)directory[0] + (int)(exchange && directory[1]);
	delta[1] = same_parent ? delta[0] : (int)directory[0] - (int)directory[1];
	for (index = 0; index < 2; index++) {
		EXPECT(ext4_get_inode(fs, parents[index].number, &observed), EXT4_OK);
		CHECK(observed.links == (int)parents[index].links + delta[index] &&
		    observed.change_time.seconds == time.seconds &&
		    observed.modify_time.seconds == time.seconds);
	}
	CHECK(fs->info.free_inodes == free_inodes + (replace && !exchange && !held ? 1U : 0U));
	if (held) {
		EXPECT(ext4_refresh_inode(hold, &observed), EXT4_OK);
		CHECK(observed.links == 0);
		if (target_kind == TEST_DIRECTORY) {
			CHECK(observed.size == 0);
		} else {
			contents(fs, &observed, target_kind, 't', 0);
		}
		EXPECT(ext4_release_inode(hold), EXT4_OK);
		CHECK(fs->info.free_inodes == free_inodes + 1);
	}
	EXPECT(ext4_sync(fs), EXT4_OK);
	memcpy(clean, device->stable, device->size);
	ext4_unmount(fs);
	CHECK(storage_recover(device, clean, true));
	CHECK(device->writes == 0 && device->live == 0);
	free(clean);
}

static void
guards(struct device *device)
{
	struct ext4_fs *fs;
	struct ext4_inode root;
	struct ext4_inode parent;
	struct ext4_inode nested;
	struct ext4_inode file;
	struct ext4_inode result;
	struct ext4_inode untouched;
	struct ext4_rename_entry source;
	struct ext4_rename_entry destination;
	struct ext4_rename_entry changed;
	struct ext4_timestamp time = { .seconds = 1700000050 };
	uint8_t *before = malloc(device->size);
	uint32_t writes;

	CHECK(before != NULL);
	device_reset(device, device->base);
	fs = mount_writer(device, &root);
	parent = create(fs, &root, "parent", TEST_DIRECTORY, 0);
	nested = create(fs, &parent, "nested", TEST_DIRECTORY, 0);
	file = create(fs, &root, "file", TEST_REGULAR, 's');
	memcpy(before, device->cache, device->size);
	writes = device->writes;
	source = entry(&root, "parent", &parent);
	destination = entry(&nested, "moved", NULL);
	memset(&result, 0xa5, sizeof(result));
	untouched = result;
	EXPECT(ext4_rename(fs, &source, &destination, 0, &time, &result), EXT4_INVALID_ARGUMENT);
	destination = entry(&parent, "nested", &nested);
	EXPECT(ext4_rename(fs, &source, &destination, EXT4_RENAME_EXCHANGE, &time, &result),
	    EXT4_INVALID_ARGUMENT);
	source = entry(&root, "file", &file);
	destination = entry(&root, "parent", &parent);
	EXPECT(ext4_rename(fs, &source, &destination, 0, &time, &result), EXT4_IS_DIRECTORY);
	destination = source;
	EXPECT(ext4_rename(fs, &source, &destination, EXT4_RENAME_NOREPLACE, &time, &result),
	    EXT4_EXISTS);
	EXPECT(ext4_rename(fs, &source, &destination, EXT4_RENAME_NOREPLACE | EXT4_RENAME_EXCHANGE,
		   &time, &result),
	    EXT4_INVALID_ARGUMENT);
	destination = entry(&root, "absent", NULL);
	EXPECT(ext4_rename(fs, &source, &destination, EXT4_RENAME_EXCHANGE, &time, &result),
	    EXT4_NOT_FOUND);
	changed = source;
	changed.generation++;
	EXPECT(ext4_rename(fs, &changed, &destination, 0, &time, &result), EXT4_STALE);
	changed = destination;
	changed.directory_generation++;
	EXPECT(ext4_rename(fs, &source, &changed, 0, &time, &result), EXT4_STALE);
	CHECK(memcmp(&result, &untouched, sizeof(result)) == 0);
	EXPECT(ext4_rename(fs, &source, &source, 0, &time, &result), EXT4_OK);
	CHECK(
	    result.number == file.number && result.change_time.seconds == file.change_time.seconds);
	CHECK(!fs->aborted && device->writes == writes &&
	    memcmp(before, device->cache, device->size) == 0);
	ext4_unmount(fs);
	CHECK(device->live == 0);
	free(before);
	printf("PASS rename no-op, flags, stale identities, types and directory cycles\n");
}

static void
arguments_and_aliases(struct device *device)
{
	struct ext4_fs *fs;
	struct ext4_inode root;
	struct ext4_inode file;
	struct ext4_inode alias;
	struct ext4_inode directory;
	struct ext4_inode populated;
	struct ext4_inode result;
	struct ext4_inode untouched;
	struct ext4_inode_hold *hold;
	struct ext4_rename_entry source;
	struct ext4_rename_entry destination;
	struct ext4_rename_entry changed;
	struct ext4_timestamp time = { .seconds = 1700000050 };
	struct ext4_timestamp invalid = { .seconds = 1700000050,
		.nanoseconds = EXT4_NANOSECONDS_PER_SECOND };
	const uint8_t *names[] = { NULL, (const uint8_t *)"", (const uint8_t *)".",
		(const uint8_t *)"..", (const uint8_t *)"a/b", (const uint8_t *)"a\0b",
		(const uint8_t *)"x" };
	size_t lengths[] = { 1, 0, 1, 2, 3, 3, SIZE_MAX };
	uint8_t *before = malloc(device->size);
	uint32_t writes;
	size_t index;
	enum ext4_result error;

	CHECK(before != NULL);
	device_reset(device, device->base);
	fs = mount_writer(device, &root);
	file = create(fs, &root, "file", TEST_REGULAR, 's');
	directory = create(fs, &root, "directory", TEST_DIRECTORY, 0);
	populated = create(fs, &root, "populated", TEST_DIRECTORY, 0);
	(void)create(fs, &populated, "inside", TEST_REGULAR, 't');
	EXPECT(ext4_link(fs, root.number, root.generation, (const uint8_t *)"alias", 5, file.number,
		   file.generation, &file.change_time, &alias),
	    EXT4_OK);
	file = alias;
	source = entry(&root, "file", &file);
	destination = entry(&root, "absent", NULL);
	memcpy(before, device->cache, device->size);
	writes = device->writes;
	memset(&result, 0xa5, sizeof(result));
	untouched = result;
	EXPECT(ext4_rename(NULL, &source, &destination, 0, &time, &result), EXT4_INVALID_ARGUMENT);
	EXPECT(ext4_rename(fs, NULL, &destination, 0, &time, &result), EXT4_INVALID_ARGUMENT);
	EXPECT(ext4_rename(fs, &source, NULL, 0, &time, &result), EXT4_INVALID_ARGUMENT);
	EXPECT(ext4_rename(fs, &source, &destination, 0, NULL, &result), EXT4_INVALID_ARGUMENT);
	EXPECT(ext4_rename(fs, &source, &destination, 0, &time, NULL), EXT4_INVALID_ARGUMENT);
	EXPECT(ext4_rename(fs, &source, &destination, UINT32_MAX, &time, &result),
	    EXT4_INVALID_ARGUMENT);
	EXPECT(ext4_rename(fs, &source, &destination, 0, &invalid, &result), EXT4_RANGE);
	for (index = 0; index < sizeof(names) / sizeof(names[0]); index++) {
		error = index == sizeof(names) / sizeof(names[0]) - 1 ? EXT4_NAME_TOO_LONG
								      : EXT4_INVALID_ARGUMENT;
		changed = source;
		changed.name = names[index];
		changed.name_length = lengths[index];
		EXPECT(ext4_rename(fs, &changed, &destination, 0, &time, &result), error);
		changed = destination;
		changed.name = names[index];
		changed.name_length = lengths[index];
		EXPECT(ext4_rename(fs, &source, &changed, 0, &time, &result), error);
	}
	changed = source;
	changed.inode = 0;
	EXPECT(ext4_rename(fs, &changed, &destination, 0, &time, &result), EXT4_INVALID_ARGUMENT);
	changed = destination;
	changed.generation = file.generation;
	EXPECT(ext4_rename(fs, &source, &changed, 0, &time, &result), EXT4_INVALID_ARGUMENT);
	changed = entry(&root, "alias", &file);
	changed.generation++;
	EXPECT(ext4_rename(fs, &source, &changed, 0, &time, &result), EXT4_STALE);
	changed = entry(&root, "alias", &directory);
	EXPECT(ext4_rename(fs, &source, &changed, 0, &time, &result), EXT4_STALE);
	changed = entry(&root, "alias", NULL);
	EXPECT(ext4_rename(fs, &source, &changed, 0, &time, &result), EXT4_STALE);
	changed = entry(&root, "absent", &file);
	EXPECT(ext4_rename(fs, &source, &changed, 0, &time, &result), EXT4_STALE);
	EXPECT(ext4_rename(fs, &changed, &destination, 0, &time, &result), EXT4_NOT_FOUND);
	changed = entry(&file, "absent", NULL);
	EXPECT(ext4_rename(fs, &source, &changed, 0, &time, &result), EXT4_NOT_DIRECTORY);
	source = entry(&root, "directory", &directory);
	destination = entry(&root, "file", &file);
	EXPECT(ext4_rename(fs, &source, &destination, 0, &time, &result), EXT4_NOT_DIRECTORY);
	destination = entry(&root, "populated", &populated);
	EXPECT(ext4_rename(fs, &source, &destination, 0, &time, &result), EXT4_NOT_EMPTY);
	CHECK(memcmp(&result, &untouched, sizeof(result)) == 0);
	source = entry(&root, "file", &file);
	destination = entry(&root, "alias", &alias);
	EXPECT(ext4_rename(fs, &source, &destination, 0, &time, &result), EXT4_OK);
	CHECK(result.number == file.number && result.links == 2 &&
	    result.change_time.seconds == file.change_time.seconds);
	EXPECT(
	    ext4_rename(fs, &source, &destination, EXT4_RENAME_EXCHANGE, &time, &result), EXT4_OK);
	CHECK(result.number == file.number && result.links == 2 &&
	    result.change_time.seconds == file.change_time.seconds);
	EXPECT(ext4_rename(fs, &source, &destination, EXT4_RENAME_NOREPLACE, &time, &result),
	    EXT4_EXISTS);
	CHECK(!fs->aborted && device->writes == writes &&
	    memcmp(before, device->cache, device->size) == 0);
	EXPECT(ext4_hold_inode(fs, file.number, file.generation, &hold), EXT4_OK);
	destination = entry(&root, "moved", NULL);
	EXPECT(
	    ext4_rename(fs, &source, &destination, EXT4_RENAME_NOREPLACE, &time, &result), EXT4_OK);
	EXPECT(ext4_refresh_inode(hold, &result), EXT4_OK);
	CHECK(result.links == 2 && result.change_time.seconds == time.seconds);
	contents(fs, &result, TEST_REGULAR, 's', 0);
	EXPECT(ext4_lookup(fs, &root, (const uint8_t *)"moved", 5, &result), EXT4_OK);
	CHECK(result.number == file.number && result.generation == file.generation);
	EXPECT(ext4_lookup(fs, &root, (const uint8_t *)"file", 4, &result), EXT4_NOT_FOUND);
	EXPECT(ext4_release_inode(hold), EXT4_OK);
	EXPECT(ext4_sync(fs), EXT4_OK);
	ext4_unmount(fs);
	EXPECT(ext4_mount(&device->environment, &fs), EXT4_OK);
	memcpy(before, device->cache, device->size);
	writes = device->writes;
	result = untouched;
	EXPECT(ext4_rename(fs, &source, &destination, 0, &time, &result), EXT4_READ_ONLY);
	CHECK(device->writes == writes && memcmp(before, device->cache, device->size) == 0 &&
	    memcmp(&result, &untouched, sizeof(result)) == 0);
	ext4_unmount(fs);
	CHECK(device->live == 0);
	free(before);
	printf("PASS rename arguments, nonempty replacement, hardlink aliases, held source and "
	       "read-only guards\n");
}

static struct ext4_dir_header_disk *
directory_entry(struct device *device, struct ext4_fs *fs, const struct ext4_inode *directory,
    const char *name, uint64_t *physical)
{
	struct ext4_dir_header_disk *entry;
	uint32_t offset;
	uint32_t length;
	uint32_t usable = device->block_size -
	    (device->metadata_checksum ? sizeof(struct ext4_dir_tail_disk) : 0);

	CHECK(directory->size == device->block_size);
	EXPECT(ext4_map_block(fs, directory, 0, physical), EXT4_OK);
	for (offset = 0; offset < usable; offset += length) {
		entry = (struct ext4_dir_header_disk *)(device->cache +
		    *physical * device->block_size + offset);
		length = ext4_directory_record_length(fs, entry);
		CHECK(length >= sizeof(*entry) && length <= usable - offset);
		if (ext4_le32(&entry->inode) != 0 && entry->name_length == strlen(name) &&
		    memcmp((uint8_t *)entry + sizeof(*entry), name, strlen(name)) == 0) {
			return entry;
		}
	}
	CHECK(false);
	return NULL;
}

static void
credit_guard(struct device *device)
{
	struct ext4_fs *fs;
	struct ext4_inode root;
	struct ext4_inode source;
	struct ext4_inode target;
	struct ext4_inode journal;
	struct ext4_inode result;
	struct ext4_inode untouched;
	struct ext4_rename_entry names[2];
	struct ext4_timestamp time = { .seconds = 1700000050 };
	struct ext4_jbd_super *super;
	uint64_t physical;
	uint8_t *before = malloc(device->size);

	CHECK(before != NULL);
	device_reset(device, device->base);
	fs = mount_writer(device, &root);
	source = create(fs, &root, "source", TEST_REGULAR, 's');
	target = create(fs, &root, "target", TEST_REGULAR, 't');
	names[0] = entry(&root, "source", &source);
	names[1] = entry(&root, "target", &target);
	EXPECT(ext4_get_inode(fs, fs->journal_inode, &journal), EXT4_OK);
	EXPECT(ext4_map_block(fs, &journal, 0, &physical), EXT4_OK);
	EXPECT(ext4_sync(fs), EXT4_OK);
	ext4_unmount(fs);
	device_reset(device, device->stable);
	super = (struct ext4_jbd_super *)(device->cache + physical * device->block_size);
	ext4_encode_be32(
	    &super->max_length, ext4_be32(&super->first) + 2 * TEST_RENAME_SMALL_CREDITS + 2);
	if (ext4_be32(&super->feature_incompat) & (EXT4_JBD_CSUM_V2 | EXT4_JBD_CSUM_V3)) {
		ext4_encode_be32(&super->checksum, 0);
		ext4_encode_be32(&super->checksum, ext4_crc32c(UINT32_MAX, super, sizeof(*super)));
	}
	memcpy(before, device->cache, device->size);
	fs = mount_writer(device, &root);
	CHECK(ext4_journal_credits(fs->journal) == TEST_RENAME_SMALL_CREDITS);
	memset(&result, 0xa5, sizeof(result));
	untouched = result;
	EXPECT(ext4_rename(fs, &names[0], &names[1], 0, &time, &result), EXT4_RANGE);
	CHECK(!fs->aborted && device->writes == 0 &&
	    memcmp(before, device->cache, device->size) == 0 &&
	    memcmp(&result, &untouched, sizeof(result)) == 0);
	ext4_unmount(fs);
	CHECK(device->live == 0);
	free(before);
	printf("PASS rename reserves restartable victim cleanup credits before replacing names\n");
}

static void
directory_checksum(struct device *device, struct ext4_fs *fs, const struct ext4_inode *directory,
    uint64_t physical)
{
	uint8_t *bytes = device->cache + physical * device->block_size;
	struct ext4_dir_tail_disk *tail;

	if (device->metadata_checksum) {
		tail = (struct ext4_dir_tail_disk *)(bytes + device->block_size - sizeof(*tail));
		ext4_encode32(&tail->checksum,
		    ext4_crc32c(
			ext4_inode_seed(fs, directory), bytes, device->block_size - sizeof(*tail)));
	}
}

enum malformed_rename {
	ANCESTRY_SELF,
	ANCESTRY_CYCLE,
	ANCESTRY_FILE,
	WRONG_SOURCE_DOTDOT,
	LATE_SOURCE_NAME,
	LATE_DESTINATION_NAME,
	DUPLICATE_SOURCE,
	SOURCE_PARENT_LINKS,
	DESTINATION_LINK_LIMIT,
	DESTINATION_UNKNOWN_LINKS,
	IMMUTABLE_SOURCE_PARENT,
	IMMUTABLE_DESTINATION_PARENT,
	SOURCE_DIRECTORY_CHECKSUM,
	MALFORMED_RENAME_COUNT
};

static void
malformed(struct device *device)
{
	struct ext4_fs *fs;
	struct ext4_inode root;
	struct ext4_inode parents[2];
	struct ext4_inode source;
	struct ext4_inode third;
	struct ext4_inode file;
	struct ext4_inode result;
	struct ext4_inode untouched;
	struct ext4_rename_entry names[2];
	struct ext4_inode_disk *disk;
	struct ext4_dir_header_disk *record;
	struct ext4_timestamp time = { .seconds = 1700000050 };
	uint8_t *before = malloc(device->size);
	uint64_t offset;
	uint64_t physical;
	uint32_t writes;
	unsigned int scenario;
	unsigned int index;
	unsigned int passed = 0;
	unsigned int skipped = 0;
	enum ext4_result expected;

	CHECK(before != NULL);
	for (scenario = 0; scenario < MALFORMED_RENAME_COUNT; scenario++) {
		device_reset(device, device->base);
		fs = mount_writer(device, &root);
		if (scenario == SOURCE_DIRECTORY_CHECKSUM && !device->metadata_checksum) {
			skipped++;
			ext4_unmount(fs);
			continue;
		}
		parents[0] = create(fs, &root, "left", TEST_DIRECTORY, 0);
		parents[1] = create(fs, &root, "right", TEST_DIRECTORY, 0);
		source = create(fs, &parents[0], "source", TEST_DIRECTORY, 0);
		third = create(fs, &root, "third", TEST_DIRECTORY, 0);
		file = create(fs, &root, "file", TEST_REGULAR, 'f');
		(void)create(fs, &parents[0], "future", TEST_REGULAR, 'f');
		(void)create(fs, &parents[1], "future", TEST_REGULAR, 'f');
		for (index = 0; index < 2; index++) {
			EXPECT(ext4_get_inode(fs, parents[index].number, &parents[index]), EXT4_OK);
		}
		names[0] = entry(&parents[0], "source", &source);
		names[1] = entry(&parents[1], "destination", NULL);
		expected = EXT4_CORRUPT;
		switch ((enum malformed_rename)scenario) {
		case ANCESTRY_SELF:
		case ANCESTRY_CYCLE:
		case ANCESTRY_FILE:
			record = directory_entry(device, fs, &parents[1], "..", &physical);
			ext4_encode32(&record->inode,
			    scenario == ANCESTRY_SELF	     ? parents[1].number
				: scenario == ANCESTRY_CYCLE ? third.number
							     : file.number);
			directory_checksum(device, fs, &parents[1], physical);
			if (scenario == ANCESTRY_CYCLE) {
				record = directory_entry(device, fs, &third, "..", &physical);
				ext4_encode32(&record->inode, parents[1].number);
				directory_checksum(device, fs, &third, physical);
			}
			break;
		case WRONG_SOURCE_DOTDOT:
			record = directory_entry(device, fs, &source, "..", &physical);
			ext4_encode32(&record->inode, parents[1].number);
			directory_checksum(device, fs, &source, physical);
			break;
		case LATE_SOURCE_NAME:
		case LATE_DESTINATION_NAME:
		case DUPLICATE_SOURCE:
			index = scenario == LATE_DESTINATION_NAME ? 1 : 0;
			record = directory_entry(device, fs, &parents[index], "future", &physical);
			if (scenario == DUPLICATE_SOURCE) {
				memcpy((uint8_t *)record + sizeof(*record), "source", 6);
			} else {
				*((uint8_t *)record + sizeof(*record)) = '/';
			}
			directory_checksum(device, fs, &parents[index], physical);
			break;
		case SOURCE_PARENT_LINKS:
		case DESTINATION_LINK_LIMIT:
		case DESTINATION_UNKNOWN_LINKS:
		case IMMUTABLE_SOURCE_PARENT:
		case IMMUTABLE_DESTINATION_PARENT:
			index =
			    scenario == SOURCE_PARENT_LINKS || scenario == IMMUTABLE_SOURCE_PARENT
			    ? 0
			    : 1;
			EXPECT(ext4_inode_location(fs, parents[index].number, &offset), EXT4_OK);
			disk = (struct ext4_inode_disk *)(device->cache + offset);
			if (scenario == IMMUTABLE_SOURCE_PARENT ||
			    scenario == IMMUTABLE_DESTINATION_PARENT) {
				ext4_encode32(
				    &disk->flags, parents[index].flags | EXT4_INODE_IMMUTABLE);
				expected = EXT4_UNSUPPORTED;
			} else {
				ext4_encode16(&disk->links,
				    scenario == SOURCE_PARENT_LINKS	     ? 2
					: scenario == DESTINATION_LINK_LIMIT ? EXT4_LINK_MAX
									     : 1);
				expected = scenario == SOURCE_PARENT_LINKS ? EXT4_CORRUPT
				    : scenario == DESTINATION_LINK_LIMIT   ? EXT4_TOO_MANY_LINKS
									   : EXT4_CORRUPT;
				if (scenario == DESTINATION_UNKNOWN_LINKS) {
					struct ext4_super_disk *super;

					/* A sentinel without DIR_NLINK is corrupt. Valid sentinel
					 * operations are exercised by directory link tests. */
					super = (struct ext4_super_disk *)(device->cache +
					    EXT4_SUPER_OFFSET);
					fs->info.feature_ro_compat &= ~EXT4_FEATURE_RO_DIR_NLINK;
					ext4_encode32(
					    &super->feature_ro_compat, fs->info.feature_ro_compat);
					if (device->metadata_checksum) {
						ext4_encode32(&super->checksum,
						    ext4_crc32c(UINT32_MAX, super,
							offsetof(
							    struct ext4_super_disk, checksum)));
					}
				}
			}
			ext4_inode_checksum_set(fs, parents[index].number, disk);
			break;
		case SOURCE_DIRECTORY_CHECKSUM:
			(void)directory_entry(device, fs, &source, "..", &physical);
			device->cache[physical * device->block_size + device->block_size - 1] ^= 1;
			break;
		case MALFORMED_RENAME_COUNT:
			CHECK(false);
		}
		memcpy(before, device->cache, device->size);
		writes = device->writes;
		memset(&result, 0xa5, sizeof(result));
		untouched = result;
		EXPECT(ext4_rename(fs, &names[0], &names[1], 0, &time, &result), expected);
		CHECK(!fs->aborted && device->writes == writes &&
		    memcmp(before, device->cache, device->size) == 0 &&
		    memcmp(&result, &untouched, sizeof(result)) == 0);
		ext4_unmount(fs);
		passed++;
	}
	CHECK(device->live == 0);
	free(before);
	printf("PASS malformed rename cases=%u skipped_absent_checksum=%u\n", passed, skipped);
}

static void
indexed_operations(struct device *device)
{
	struct ext4_fs *fs;
	struct ext4_inode root;
	struct ext4_inode directory;
	struct ext4_inode file;
	struct ext4_inode result;
	struct ext4_inode untouched;
	struct ext4_rename_entry source;
	struct ext4_rename_entry destination;
	struct ext4_timestamp time = { .seconds = 1700000050 };
	unsigned int operation;

	for (operation = 0; operation < 4; operation++) {
		device_reset(device, device->base);
		fs = mount_writer(device, &root);
		EXPECT(ext4_lookup(fs, &root, (const uint8_t *)"many", 4, &directory), EXT4_OK);
		EXPECT(ext4_lookup(fs, &root, (const uint8_t *)"hello.txt", 9, &file), EXT4_OK);
		CHECK(directory.flags & EXT4_INODE_INDEX);
		memset(&result, 0xa5, sizeof(result));
		untouched = result;
		if (operation == 0) {
			source = entry(&directory, "entry", &file);
			destination = entry(&root, "absent", NULL);
			EXPECT(ext4_rename(fs, &source, &destination, 0, &time, &result),
			    EXT4_NOT_FOUND);
			CHECK(device->writes == 0 &&
			    memcmp(&result, &untouched, sizeof(result)) == 0 &&
			    memcmp(device->cache, device->base, device->size) == 0);
		} else {
			source = operation == 3 ? entry(&root, "many", &directory)
						: entry(&root, "hello.txt", &file);
			destination = operation == 1 ? entry(&directory, "absent", NULL)
			    : operation == 2	     ? entry(&root, "many", &directory)
						     : entry(&root, "absent", NULL);
			EXPECT(ext4_rename(fs, &source, &destination,
				   operation == 2 ? EXT4_RENAME_EXCHANGE : 0, &time, &result),
			    EXT4_OK);
			EXPECT(ext4_lookup(fs, operation == 1 ? &directory : &root,
				   destination.name, destination.name_length, &result),
			    EXT4_OK);
			CHECK(result.number == source.inode);
			EXPECT(ext4_get_inode(fs, directory.number, &directory), EXT4_OK);
			CHECK(directory.flags & EXT4_INODE_INDEX);
			EXPECT(ext4_lookup(fs, &directory, (const uint8_t *)"..", 2, &result),
			    EXT4_OK);
			CHECK(result.number == root.number);
			EXPECT(ext4_sync(fs), EXT4_OK);
		}
		ext4_unmount(fs);
		CHECK(device->live == 0);
	}
	printf("PASS indexed rename insertion, exchange, directory move and missing source\n");
}

enum rename_operation {
	RENAME_MOVE_FILE,
	RENAME_MOVE_DIRECTORY,
	RENAME_REPLACE_FILE,
	RENAME_REPLACE_DIRECTORY,
	RENAME_REPLACE_SHORT_SYMLINK,
	RENAME_REPLACE_LONG_SYMLINK,
	RENAME_REPLACE_ALIAS,
	RENAME_EXCHANGE_MIXED,
	RENAME_REPLACE_HELD,
	RENAME_GROW_PARENT,
	RENAME_EXCHANGE_DIRECTORIES,
	RENAME_WHITEOUT_FILE,
	RENAME_WHITEOUT_DIRECTORY,
	RENAME_WHITEOUT_REPLACE,
	RENAME_WHITEOUT_GROW,
	RENAME_WHITEOUT_ATTRIBUTES,
	RENAME_WHITEOUT_HELD,
	RENAME_OPERATION_COUNT
};

struct rename_case {
	enum test_kind source;
	enum test_kind target;
	bool same_parent;
	bool replace;
	bool exchange;
	bool held;
	bool large;
	bool alias;
	bool grow;
	bool whiteout;
	bool attributes;
};

static const struct rename_case cases[RENAME_OPERATION_COUNT] = {
	[RENAME_MOVE_FILE] = { .source = TEST_REGULAR, .same_parent = true },
	[RENAME_MOVE_DIRECTORY] = { .source = TEST_DIRECTORY },
	[RENAME_REPLACE_FILE] = { .source = TEST_REGULAR,
	    .target = TEST_REGULAR,
	    .same_parent = true,
	    .replace = true,
	    .large = true },
	[RENAME_REPLACE_DIRECTORY] = { .source = TEST_DIRECTORY,
	    .target = TEST_DIRECTORY,
	    .replace = true },
	[RENAME_REPLACE_SHORT_SYMLINK] = { .source = TEST_INLINE_SYMLINK,
	    .target = TEST_INLINE_SYMLINK,
	    .replace = true },
	[RENAME_REPLACE_LONG_SYMLINK] = { .source = TEST_MAPPED_SYMLINK,
	    .target = TEST_MAPPED_SYMLINK,
	    .replace = true },
	[RENAME_REPLACE_ALIAS] = { .source = TEST_REGULAR,
	    .target = TEST_REGULAR,
	    .replace = true,
	    .large = true,
	    .alias = true },
	[RENAME_EXCHANGE_MIXED] = { .source = TEST_REGULAR,
	    .target = TEST_DIRECTORY,
	    .replace = true,
	    .exchange = true },
	[RENAME_REPLACE_HELD] = { .source = TEST_REGULAR,
	    .target = TEST_REGULAR,
	    .replace = true,
	    .held = true,
	    .large = true },
	[RENAME_GROW_PARENT] = { .source = TEST_REGULAR, .grow = true },
	[RENAME_EXCHANGE_DIRECTORIES] = { .source = TEST_DIRECTORY,
	    .target = TEST_DIRECTORY,
	    .replace = true,
	    .exchange = true },
	[RENAME_WHITEOUT_FILE] = { .source = TEST_REGULAR, .same_parent = true, .whiteout = true },
	[RENAME_WHITEOUT_DIRECTORY] = { .source = TEST_DIRECTORY, .whiteout = true },
	[RENAME_WHITEOUT_REPLACE] = { .source = TEST_REGULAR,
	    .target = TEST_REGULAR,
	    .replace = true,
	    .large = true,
	    .whiteout = true },
	[RENAME_WHITEOUT_GROW] = { .source = TEST_REGULAR, .grow = true, .whiteout = true },
	[RENAME_WHITEOUT_ATTRIBUTES] = { .source = TEST_REGULAR,
	    .whiteout = true,
	    .attributes = true },
	[RENAME_WHITEOUT_HELD] = { .source = TEST_REGULAR,
	    .target = TEST_REGULAR,
	    .replace = true,
	    .held = true,
	    .large = true,
	    .whiteout = true }
};

struct trace {
	uint32_t allocations;
	uint32_t reads;
	uint32_t events;
	uint32_t commit_event;
	bool committed;
};

static void
destination_name(const struct rename_case *test, char *name)
{
	if (test->grow) {
		memset(name, 'd', EXT4_NAME_MAX);
		name[EXT4_NAME_MAX] = '\0';
	} else {
		strcpy(name, "destination");
	}
}

static void
fill_directory(struct ext4_fs *fs, struct device *device, struct ext4_inode *directory,
    const struct ext4_inode *target, const struct ext4_timestamp *time)
{
	struct ext4_inode result;
	uint32_t minimum =
	    (sizeof(struct ext4_dir_header_disk) + EXT4_NAME_MAX + EXT4_DIRECTORY_ALIGNMENT - 1U) &
	    ~(EXT4_DIRECTORY_ALIGNMENT - 1U);
	uint32_t dot = (sizeof(struct ext4_dir_header_disk) + 2U + EXT4_DIRECTORY_ALIGNMENT - 1U) &
	    ~(EXT4_DIRECTORY_ALIGNMENT - 1U);
	uint32_t usable = device->block_size -
	    (device->metadata_checksum ? sizeof(struct ext4_dir_tail_disk) : 0);
	uint32_t count = (usable - 2U * dot) / minimum;
	uint32_t index;
	int length;
	char name[EXT4_NAME_MAX + 1];

	for (index = 0; index < count; index++) {
		length = snprintf(name, sizeof(name), "filler-%08u-", index);
		CHECK(length > 0 && (unsigned int)length < EXT4_NAME_MAX);
		memset(name + length, 'f', EXT4_NAME_MAX - (size_t)length);
		name[EXT4_NAME_MAX] = '\0';
		EXPECT(
		    ext4_link(fs, directory->number, directory->generation, (const uint8_t *)name,
			EXT4_NAME_MAX, target->number, target->generation, time, &result),
		    EXT4_OK);
	}
	EXPECT(ext4_get_inode(fs, directory->number, directory), EXT4_OK);
	CHECK(directory->size == device->block_size);
}

static void
space_guard(struct device *device)
{
	struct ext4_fs *fs;
	struct ext4_inode root;
	struct ext4_inode parents[2];
	struct ext4_inode source;
	struct ext4_inode filler;
	struct ext4_inode result;
	struct ext4_inode untouched;
	struct ext4_rename_entry names[2];
	struct ext4_timestamp time = { .seconds = 1700000050 };
	struct ext4_super_disk *super;
	uint8_t *before = malloc(device->size);
	char name[EXT4_NAME_MAX + 1];
	uint64_t free_blocks;
	uint32_t free_inodes;
	uint32_t writes;

	CHECK(before != NULL);
	device_reset(device, device->base);
	fs = mount_writer(device, &root);
	parents[0] = create(fs, &root, "left", TEST_DIRECTORY, 0);
	parents[1] = create(fs, &root, "right", TEST_DIRECTORY, 0);
	source = create(fs, &parents[0], "source", TEST_REGULAR, 's');
	filler = create(fs, &root, "filler", TEST_REGULAR, 'f');
	fill_directory(fs, device, &parents[1], &filler, &time);
	destination_name(&cases[RENAME_GROW_PARENT], name);
	names[0] = entry(&parents[0], "source", &source);
	names[1] = entry(&parents[1], name, NULL);
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
	writes = device->writes;
	memset(&result, 0xa5, sizeof(result));
	untouched = result;
	EXPECT(ext4_rename(fs, &names[0], &names[1], 0, &time, &result), EXT4_NO_SPACE);
	CHECK(!fs->aborted && device->writes == writes &&
	    memcmp(before, device->cache, device->size) == 0 &&
	    memcmp(&result, &untouched, sizeof(result)) == 0);
	names[1] = entry(&parents[1], "x", NULL);
	EXPECT(ext4_rename(fs, &names[0], &names[1], 0, &time, &result), EXT4_OK);
	CHECK(result.number == source.number && fs->info.free_blocks == free_blocks &&
	    fs->info.free_inodes == free_inodes);
	EXPECT(ext4_lookup(fs, &parents[0], (const uint8_t *)"source", 6, &result), EXT4_NOT_FOUND);
	EXPECT(ext4_lookup(fs, &parents[1], (const uint8_t *)"x", 1, &result), EXT4_OK);
	contents(fs, &result, TEST_REGULAR, 's', 0);
	EXPECT(ext4_get_inode(fs, parents[1].number, &result), EXT4_OK);
	CHECK(result.size == device->block_size);
	EXPECT(ext4_sync(fs), EXT4_OK);
	ext4_unmount(fs);
	CHECK(device->live == 0);
	free(before);
	printf("PASS rename preserves names on reserved-space exhaustion and reuses existing "
	       "record space\n");
}

static void
fault_fixture(struct device *device, enum rename_operation operation)
{
	const struct rename_case *test = &cases[operation];
	struct ext4_fs *fs;
	struct ext4_inode root;
	struct ext4_inode parents[2];
	struct ext4_inode objects[2];
	struct ext4_inode filler;
	struct ext4_inode result;
	struct ext4_inode_update update = attributes(TEST_REGULAR);
	uint8_t *bytes = malloc(device->block_size);
	uint32_t block;
	size_t completed;
	char name[EXT4_NAME_MAX + 1];

	CHECK(bytes != NULL);
	memset(bytes, 't', device->block_size);
	device_reset(device, device->base);
	fs = mount_writer(device, &root);
	parents[0] = create(fs, &root, "left", TEST_DIRECTORY, 0);
	parents[1] = test->same_parent ? parents[0] : create(fs, &root, "right", TEST_DIRECTORY, 0);
	objects[0] = create(fs, &parents[0], "source", test->source, 's');
	if (test->source == TEST_DIRECTORY) {
		(void)create(fs, &objects[0], "inside", TEST_REGULAR, 's');
	}
	destination_name(test, name);
	if (test->replace) {
		objects[1] = create(fs, &parents[1], name, test->target, 't');
		if (test->target == TEST_DIRECTORY && test->exchange) {
			(void)create(fs, &objects[1], "inside", TEST_REGULAR, 't');
		}
		update.fields =
		    EXT4_ATTR_PERMISSIONS | EXT4_ATTR_MODIFY_TIME | EXT4_ATTR_CHANGE_TIME;
		if (test->large) {
			for (block = 0; block < TEST_RENAME_DATA_BLOCKS; block++) {
				EXPECT(ext4_write(fs, objects[1].number, objects[1].generation,
					   (uint64_t)block * device->block_size, bytes,
					   device->block_size, &update, &completed),
				    EXT4_OK);
				CHECK(completed == device->block_size);
			}
		}
		if (test->alias) {
			EXPECT(ext4_link(fs, root.number, root.generation,
				   (const uint8_t *)"kept-name", 9, objects[1].number,
				   objects[1].generation, &update.change_time, &result),
			    EXT4_OK);
		}
	}
	if (test->grow) {
		filler = create(fs, &root, "filler", TEST_REGULAR, 'f');
		fill_directory(fs, device, &parents[1], &filler, &update.change_time);
	}
	EXPECT(ext4_sync(fs), EXT4_OK);
	ext4_unmount(fs);
	memcpy(device->base, device->stable, device->size);
	free(bytes);
}

static enum ext4_result
attempt(struct device *device, enum rename_operation operation, unsigned int fault, uint32_t point,
    unsigned int survival, bool partial, struct trace *trace)
{
	const struct rename_case *test = &cases[operation];
	struct ext4_fs *fs;
	struct ext4_inode root;
	struct ext4_inode parents[2];
	struct ext4_inode objects[2];
	struct ext4_inode result;
	struct ext4_inode untouched;
	struct ext4_inode_hold *hold = NULL;
	struct ext4_rename_entry names[2];
	struct ext4_timestamp time = { .seconds = 1700000050 };
	struct ext4_inode_update whiteout_update = attributes(TEST_REGULAR);
	struct ext4_xattr_change attribute;
	uint8_t value[500];
	uint32_t allocations;
	uint32_t reads;
	uint32_t events;
	size_t byte;
	char name[EXT4_NAME_MAX + 1];
	enum ext4_result error;

	fs = mount_writer(device, &root);
	EXPECT(ext4_lookup(fs, &root, (const uint8_t *)"left", 4, &parents[0]), EXT4_OK);
	if (test->same_parent) {
		parents[1] = parents[0];
	} else {
		EXPECT(ext4_lookup(fs, &root, (const uint8_t *)"right", 5, &parents[1]), EXT4_OK);
	}
	EXPECT(ext4_lookup(fs, &parents[0], (const uint8_t *)"source", 6, &objects[0]), EXT4_OK);
	destination_name(test, name);
	if (test->replace) {
		EXPECT(
		    ext4_lookup(fs, &parents[1], (const uint8_t *)name, strlen(name), &objects[1]),
		    EXT4_OK);
	}
	names[0] = entry(&parents[0], "source", &objects[0]);
	names[1] = entry(&parents[1], name, test->replace ? &objects[1] : NULL);
	if (test->held) {
		EXPECT(
		    ext4_hold_inode(fs, objects[1].number, objects[1].generation, &hold), EXT4_OK);
	}
	allocations = device->allocations;
	reads = device->reads;
	events = device->events;
	whiteout_update.permissions = 0;
	whiteout_update.access_time = time;
	whiteout_update.modify_time = time;
	whiteout_update.change_time = time;
	memset(&attribute, 0, sizeof(attribute));
	if (test->attributes) {
		for (byte = 0; byte < sizeof(value); byte++) {
			value[byte] = (uint8_t)(byte * 31U + 7U);
		}
		attribute.policy = EXT4_XATTR_CREATE;
		attribute.name_index = EXT4_XATTR_USER;
		attribute.name = (const uint8_t *)"whiteout";
		attribute.name_length = 8;
		attribute.value = value;
		attribute.value_size = sizeof(value);
		whiteout_update.fields |= EXT4_ATTR_XATTRS;
		whiteout_update.xattrs = &attribute;
		whiteout_update.xattr_count = 1;
	}
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
	error = test->whiteout
	    ? ext4_rename_whiteout(fs, &names[0], &names[1], 0, &whiteout_update, &time, &result)
	    : ext4_rename(fs, &names[0], &names[1], test->exchange ? EXT4_RENAME_EXCHANGE : 0,
		  &time, &result);
	if (error == EXT4_OK) {
		CHECK(result.number == objects[0].number &&
		    result.generation == objects[0].generation &&
		    result.links == objects[0].links && result.change_time.seconds == time.seconds);
		if (hold != NULL) {
			error = ext4_release_inode(hold);
		}
		if (error == EXT4_OK) {
			error = ext4_sync(fs);
		}
	} else {
		CHECK(memcmp(&result, &untouched, sizeof(result)) == 0);
	}
	trace->allocations = device->allocations - allocations;
	trace->reads = device->reads - reads;
	trace->events = device->events - events;
	trace->commit_event = device->commit_barrier - events;
	trace->committed = device->intent_durable;
	if (error != EXT4_OK) {
		if (device->writes != 0) {
			CHECK(fs->aborted);
			EXPECT(ext4_get_inode(fs, root.number, &result), EXT4_RECOVERY_REQUIRED);
		} else {
			CHECK(memcmp(device->cache, device->base, device->size) == 0);
			CHECK(!fs->aborted || (error == EXT4_IO && fs->journal->aborted));
		}
	}
	ext4_unmount(fs);
	CHECK(device->live == 0);
	return error;
}

static void
fault_cases(struct device *device, enum rename_operation operation, bool smoke, const char *exports,
    const char *path)
{
	struct trace baseline;
	struct trace trace;
	uint8_t *original = malloc(device->size);
	uint8_t *expected = malloc(device->size);
	uint32_t point;
	uint32_t limit;
	uint32_t recovered = 0;
	uint32_t torn = 0;
	unsigned int fault;
	unsigned int survival;
	unsigned int partial;
	char prefix[40];

	CHECK(original != NULL && expected != NULL);
	memcpy(original, device->base, device->size);
	fault_fixture(device, operation);
	CHECK(snprintf(prefix, sizeof(prefix), "rename-before-%u-", operation) > 0);
	storage_export(device, exports, path, prefix);
	device_reset(device, device->base);
	EXPECT(attempt(device, operation, 0, 0, 0, false, &baseline), EXT4_OK);
	memcpy(expected, device->stable, device->size);
	CHECK(snprintf(prefix, sizeof(prefix), "rename-atomic-%u-", operation) > 0);
	storage_export(device, exports, path, prefix);
	CHECK(storage_recover(device, expected, true));
	CHECK(device->writes == 0);
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
		printf("PASS rename faults operation=%u allocations=%u reads=%u cuts=%u "
		       "recovered=%u torn_super_fail_closed=%u\n",
		    operation, baseline.allocations, baseline.reads, baseline.events * 6, recovered,
		    torn);
	}
	if (exports != NULL) {
		CHECK(baseline.commit_event > 1 && baseline.commit_event < baseline.events);
		device_reset(device, device->base);
		EXPECT(attempt(device, operation, 3, baseline.commit_event + 1, 0, false, &trace),
		    EXT4_IO);
		CHECK(trace.committed);
		CHECK(snprintf(prefix, sizeof(prefix), "rename-pending-%u-", operation) > 0);
		storage_export(device, exports, path, prefix);
		CHECK(storage_recover(device, expected, true));
		device_reset(device, device->base);
		EXPECT(attempt(device, operation, 3, baseline.commit_event - 1, 0, false, &trace),
		    EXT4_IO);
		CHECK(!trace.committed);
		CHECK(snprintf(prefix, sizeof(prefix), "rename-uncommitted-%u-", operation) > 0);
		storage_export(device, exports, path, prefix);
		CHECK(storage_recover(device, expected, false));
	}
	memcpy(device->base, original, device->size);
	free(expected);
	free(original);
}

#include "whiteout.h"

int
main(int argc, char **argv)
{
	struct device device;
	const char *exports = NULL;
	unsigned int same_parent;
	unsigned int reverse;
	unsigned int kind;
	unsigned int target_kind;
	unsigned int operation;
	bool smoke = false;
	bool functional_only = false;
	bool indexed = false;
	bool whiteout = false;
	int argument = 1;

	while (argument < argc && argv[argument][0] == '-') {
		if (strcmp(argv[argument], "--smoke") == 0) {
			smoke = true;
		} else if (strcmp(argv[argument], "--functional-only") == 0) {
			functional_only = true;
		} else if (strcmp(argv[argument], "--indexed") == 0) {
			indexed = true;
		} else if (strcmp(argv[argument], "--whiteout") == 0) {
			whiteout = true;
		} else if (strcmp(argv[argument], "--export") == 0 && argument + 1 < argc) {
			exports = argv[++argument];
		} else {
			CHECK(false);
		}
		argument++;
	}
	CHECK(argument < argc);
	for (; argument < argc; argument++) {
		storage_open(&device, argv[argument]);
		if (whiteout) {
			whiteout_guards(&device);
			for (operation = RENAME_WHITEOUT_FILE; operation < RENAME_OPERATION_COUNT;
			    operation++) {
				fault_cases(&device, (enum rename_operation)operation, smoke,
				    exports, argv[argument]);
			}
			storage_close(&device);
			printf("PASS whiteout rename: %s\n", argv[argument]);
			continue;
		}
		if (indexed) {
			indexed_operations(&device);
			storage_close(&device);
			continue;
		}
		guards(&device);
		arguments_and_aliases(&device);
		credit_guard(&device);
		malformed(&device);
		space_guard(&device);
		functional_sequences = 0;
		for (same_parent = 0; same_parent < 2; same_parent++) {
			for (reverse = 0; reverse < 2; reverse++) {
				for (kind = TEST_REGULAR; kind <= TEST_MAPPED_SYMLINK; kind++) {
					for (target_kind = TEST_REGULAR;
					    target_kind <= TEST_MAPPED_SYMLINK; target_kind++) {
						if (target_kind == TEST_DIRECTORY) {
							continue;
						}
						sequence(&device, (enum test_kind)kind,
						    (enum test_kind)target_kind, true, true,
						    same_parent, reverse, false);
						if (kind != TEST_DIRECTORY && kind != target_kind) {
							sequence(&device, (enum test_kind)kind,
							    (enum test_kind)target_kind, true,
							    false, same_parent, reverse, false);
							sequence(&device, (enum test_kind)kind,
							    (enum test_kind)target_kind, true,
							    false, same_parent, reverse, true);
						}
					}
					sequence(&device, (enum test_kind)kind,
					    (enum test_kind)kind, false, false, same_parent,
					    reverse, false);
					sequence(&device, (enum test_kind)kind,
					    (enum test_kind)kind, true, false, same_parent, reverse,
					    false);
					sequence(&device, (enum test_kind)kind,
					    (enum test_kind)kind, true, false, same_parent, reverse,
					    true);
					sequence(&device, (enum test_kind)kind, TEST_DIRECTORY,
					    true, true, same_parent, reverse, false);
				}
			}
		}
		storage_export(&device, exports, argv[argument], "rename-exchanged-");
		if (!functional_only) {
			for (operation = 0; operation < RENAME_WHITEOUT_FILE; operation++) {
				fault_cases(&device, (enum rename_operation)operation, smoke,
				    exports, argv[argument]);
			}
		}
		storage_close(&device);
		printf("PASS rename functional matrix: %s sequences=%u\n", argv[argument],
		    functional_sequences);
	}
	return 0;
}

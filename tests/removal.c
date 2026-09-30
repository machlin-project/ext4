/* SPDX-License-Identifier: BSD-3-Clause */
#include "storage.h"
#include "allocate.h"

#define TEST_REMOVAL_DATA_BLOCKS 35U
#define TEST_REMOVAL_SMALL_CREDITS 4U

static struct ext4_inode_update
attributes(void)
{
	struct ext4_inode_update update;

	memset(&update, 0, sizeof(update));
	update.fields = EXT4_ATTR_PERMISSIONS | EXT4_ATTR_UID | EXT4_ATTR_GID |
	    EXT4_ATTR_ACCESS_TIME | EXT4_ATTR_MODIFY_TIME | EXT4_ATTR_CHANGE_TIME;
	update.permissions = 0640;
	update.uid = UINT32_MAX - 2U;
	update.gid = UINT32_C(0x81234567);
	update.access_time.seconds = 1700000000;
	update.modify_time.seconds = 1700000001;
	update.change_time.seconds = 1700000002;
	return update;
}

static struct ext4_inode_update
write_attributes(void)
{
	struct ext4_inode_update update = attributes();

	update.fields = EXT4_ATTR_PERMISSIONS | EXT4_ATTR_MODIFY_TIME | EXT4_ATTR_CHANGE_TIME;
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
create(struct ext4_fs *fs, const struct ext4_inode *parent, const char *name, bool directory)
{
	struct ext4_inode_update update = attributes();
	struct ext4_inode inode;

	EXPECT(directory
		? ext4_mkdir(fs, parent->number, parent->generation, (const uint8_t *)name,
		      strlen(name), &update, &update.change_time, &inode)
		: ext4_create(fs, parent->number, parent->generation, (const uint8_t *)name,
		      strlen(name), &update, &update.change_time, &inode),
	    EXT4_OK);
	return inode;
}

static enum ext4_result
remove_inode(struct ext4_fs *fs, const struct ext4_inode *parent, const char *name,
    const struct ext4_inode *inode, bool directory, struct ext4_inode *result)
{
	struct ext4_timestamp time = { .seconds = 1700000030 };

	return directory
	    ? ext4_rmdir(fs, parent->number, parent->generation, (const uint8_t *)name,
		  strlen(name), inode->number, inode->generation, &time, result)
	    : ext4_unlink(fs, parent->number, parent->generation, (const uint8_t *)name,
		  strlen(name), inode->number, inode->generation, &time, result);
}

static void
missing(struct ext4_fs *fs, const struct ext4_inode *parent, const char *name)
{
	struct ext4_inode inode;

	EXPECT(
	    ext4_lookup(fs, parent, (const uint8_t *)name, strlen(name), &inode), EXT4_NOT_FOUND);
}

static void
hold_faults(struct device *device)
{
	struct ext4_fs *fs;
	struct ext4_inode root;
	struct ext4_inode inode;
	struct ext4_inode_hold *hold = NULL;
	struct ext4_inode_hold *result;
	uint32_t allocations;
	uint32_t reads;
	uint32_t live;
	uint32_t count;
	uint32_t point;
	unsigned int existing;
	unsigned int fault;

	device_reset(device, device->base);
	fs = mount_writer(device, &root);
	EXPECT(ext4_lookup(fs, &root, (const uint8_t *)"hello.txt", 9, &inode), EXT4_OK);
	for (existing = 0; existing < 2; existing++) {
		if (existing) {
			EXPECT(ext4_hold_inode(fs, inode.number, inode.generation, &hold), EXT4_OK);
		}
		live = device->live;
		allocations = device->allocations;
		reads = device->reads;
		EXPECT(ext4_hold_inode(fs, inode.number, inode.generation, &result), EXT4_OK);
		allocations = device->allocations - allocations;
		reads = device->reads - reads;
		EXPECT(ext4_release_inode(result), EXT4_OK);
		CHECK(device->live == live);
		for (fault = 1; fault <= 2; fault++) {
			count = fault == 1 ? allocations : reads;
			for (point = 1; point <= count; point++) {
				result = NULL;
				if (fault == 1) {
					device->fail_allocation = device->allocations + point;
				} else {
					device->fail_read = device->reads + point;
				}
				EXPECT(ext4_hold_inode(fs, inode.number, inode.generation, &result),
				    fault == 1 ? EXT4_NO_MEMORY : EXT4_IO);
				device->fail_allocation = device->fail_read = 0;
				CHECK(result == NULL && device->live == live && !fs->aborted);
				CHECK(fs->hold_count == existing &&
				    (!existing || hold->references == 1));
			}
		}
	}
	CHECK(hold != NULL);
	hold->references = UINT32_MAX;
	result = NULL;
	EXPECT(ext4_hold_inode(fs, inode.number, inode.generation, &result), EXT4_RANGE);
	CHECK(result == NULL && hold->references == UINT32_MAX);
	hold->references = 1;
	EXPECT(ext4_release_inode(hold), EXT4_OK);
	CHECK(device->writes == 0 && memcmp(device->cache, device->base, device->size) == 0);
	ext4_unmount(fs);
	CHECK(device->live == 0);
	printf(
	    "PASS inode hold allocation/read failures, shared identity and reference overflow\n");
}

static void
held_inode_resolution(struct device *device)
{
	struct ext4_fs *fs;
	struct ext4_inode root;
	struct ext4_inode inode;
	struct ext4_inode refreshed;
	struct ext4_inode_hold *hold;
	struct ext4_inode_disk *disk;
	struct ext4_inode_update update = write_attributes();
	uint64_t offset;
	uint64_t result;
	uint32_t reads;
	uint32_t allocations;
	uint32_t writes;
	size_t completed;
	const uint8_t payload = 0x5a;

	device_reset(device, device->base);
	fs = mount_writer(device, &root);
	EXPECT(ext4_lookup(fs, &root, (const uint8_t *)"hello.txt", 9, &inode), EXT4_OK);
	EXPECT(ext4_inode_resolve(fs, inode.number, &offset), EXT4_OK);
	EXPECT(ext4_hold_inode(fs, inode.number, inode.generation, &hold), EXT4_OK);
	reads = device->reads;
	allocations = device->allocations;
	result = UINT64_MAX;
	EXPECT(ext4_inode_resolve_live(fs, inode.number, &result), EXT4_OK);
	CHECK(result == offset && device->reads == reads && device->allocations == allocations);
	/* Admission still checks the current record, not just the held identity. */
	writes = device->writes;
	disk = (struct ext4_inode_disk *)(device->cache + offset);
	ext4_encode32(&disk->generation, inode.generation + 1U);
	ext4_inode_checksum_set(fs, inode.number, disk);
	EXPECT(ext4_write(fs, inode.number, inode.generation, 0, &payload, 1, &update, &completed),
	    EXT4_STALE);
	CHECK(completed == 0 && device->writes == writes && !fs->aborted);
	ext4_encode32(&disk->generation, inode.generation);
	ext4_inode_checksum_set(fs, inode.number, disk);
	if (fs->metadata_checksum) {
		disk->checksum_lo.bytes[0] ^= 1U;
		EXPECT(ext4_write(
			   fs, inode.number, inode.generation, 0, &payload, 1, &update, &completed),
		    EXT4_CORRUPT);
		CHECK(completed == 0 && device->writes == writes && !fs->aborted);
		disk->checksum_lo.bytes[0] ^= 1U;
	}
	/* A failed explicit refresh revokes the allocation observation. Resolving
	 * again must reach the device instead of reviving the old address. */
	device->fail_read = device->reads + 1U;
	EXPECT(ext4_refresh_inode(hold, &refreshed), EXT4_IO);
	device->fail_read = device->reads + 1U;
	result = UINT64_MAX;
	EXPECT(ext4_inode_resolve_live(fs, inode.number, &result), EXT4_IO);
	CHECK(result == UINT64_MAX);
	device->fail_read = 0;
	EXPECT(ext4_refresh_inode(hold, &refreshed), EXT4_OK);
	fs->aborted = true;
	EXPECT(ext4_inode_resolve_live(fs, inode.number, &result), EXT4_RECOVERY_REQUIRED);
	CHECK(result == UINT64_MAX);
	fs->aborted = false;
	EXPECT(ext4_release_inode(hold), EXT4_OK);
	device->fail_read = device->reads + 1U;
	EXPECT(ext4_inode_resolve_live(fs, inode.number, &result), EXT4_IO);
	CHECK(result == UINT64_MAX);
	device->fail_read = 0;
	ext4_unmount(fs);
	CHECK(device->live == 0 && device->writes == writes);

	/* A read-only hold supplies no exclusive allocation guarantee. */
	EXPECT(ext4_mount(&device->environment, &fs), EXT4_OK);
	EXPECT(ext4_hold_inode(fs, inode.number, inode.generation, &hold), EXT4_OK);
	device->fail_read = device->reads + 1U;
	EXPECT(ext4_inode_resolve_live(fs, inode.number, &result), EXT4_IO);
	CHECK(result == UINT64_MAX);
	device->fail_read = 0;
	EXPECT(ext4_release_inode(hold), EXT4_OK);
	ext4_unmount(fs);
	CHECK(device->live == 0 && memcmp(device->cache, device->base, device->size) == 0);
	puts("PASS held inode location, current generation/checksum, refresh failure and read-only "
	     "guards");
}

static void
held_write_metadata(struct device *device)
{
	struct ext4_fs *fs;
	struct ext4_inode root;
	struct ext4_inode inode;
	struct ext4_inode_hold *hold = NULL;
	struct ext4_inode_update update = write_attributes();
	uint8_t *expected = malloc(device->size);
	const uint8_t payload[] = { 0x45, 0x58, 0x54, 0x34 };
	uint32_t reads[2];
	uint32_t allocations[2];
	uint32_t writes[2];
	uint32_t flushes[2];
	unsigned int held;
	size_t completed;

	CHECK(expected != NULL);
	for (held = 0; held < 2U; held++) {
		device_reset(device, device->base);
		fs = mount_writer(device, &root);
		EXPECT(ext4_lookup(fs, &root, (const uint8_t *)"hello.txt", 9, &inode), EXT4_OK);
		if (held != 0) {
			EXPECT(ext4_hold_inode(fs, inode.number, inode.generation, &hold), EXT4_OK);
		}
		reads[held] = device->reads;
		allocations[held] = device->allocations;
		writes[held] = device->writes;
		flushes[held] = device->events - device->writes;
		EXPECT(ext4_write(fs, inode.number, inode.generation, 0, payload, sizeof(payload),
			   &update, &completed),
		    EXT4_OK);
		CHECK(completed == sizeof(payload));
		reads[held] = device->reads - reads[held];
		allocations[held] = device->allocations - allocations[held];
		writes[held] = device->writes - writes[held];
		flushes[held] = device->events - device->writes - flushes[held];
		if (held != 0) {
			EXPECT(ext4_release_inode(hold), EXT4_OK);
		}
		EXPECT(ext4_sync(fs), EXT4_OK);
		ext4_unmount(fs);
		CHECK(
		    device->live == 0 && memcmp(device->cache, device->stable, device->size) == 0);
		if (held == 0) {
			memcpy(expected, device->cache, device->size);
		} else {
			CHECK(memcmp(expected, device->cache, device->size) == 0);
		}
	}
	CHECK(reads[1] + 2U == reads[0] && allocations[1] + 2U == allocations[0]);
	CHECK(writes[1] == writes[0] && flushes[1] == flushes[0]);
	free(expected);
	puts("PASS held writes save two metadata reads/allocations with identical durable image");
}

static void
guards(struct device *device)
{
	struct ext4_fs *fs;
	struct ext4_inode root;
	struct ext4_inode file;
	struct ext4_inode result;
	struct ext4_inode untouched;
	struct ext4_inode changed;
	struct ext4_timestamp time = { .seconds = 1700000030 };
	const uint8_t *names[] = { NULL, (const uint8_t *)"", (const uint8_t *)".",
		(const uint8_t *)"..", (const uint8_t *)"a/b", (const uint8_t *)"a\0b",
		(const uint8_t *)"x" };
	size_t lengths[] = { 1, 0, 1, 2, 3, 3, SIZE_MAX };
	uint8_t *before = malloc(device->size);
	uint32_t writes;
	size_t index;

	CHECK(before != NULL);
	device_reset(device, device->base);
	fs = mount_writer(device, &root);
	file = create(fs, &root, "victim", false);
	writes = device->writes;
	memcpy(before, device->cache, device->size);
	memset(&result, 0xa5, sizeof(result));
	untouched = result;
	for (index = 0; index < sizeof(names) / sizeof(names[0]); index++) {
		EXPECT(ext4_unlink(fs, root.number, root.generation, names[index], lengths[index],
			   file.number, file.generation, &time, &result),
		    index == sizeof(names) / sizeof(names[0]) - 1 ? EXT4_NAME_TOO_LONG
								  : EXT4_INVALID_ARGUMENT);
	}
	EXPECT(ext4_unlink(NULL, root.number, root.generation, (const uint8_t *)"victim", 6,
		   file.number, file.generation, &time, &result),
	    EXT4_INVALID_ARGUMENT);
	EXPECT(ext4_unlink(fs, root.number, root.generation, (const uint8_t *)"victim", 6,
		   file.number, file.generation, NULL, &result),
	    EXT4_INVALID_ARGUMENT);
	EXPECT(ext4_unlink(fs, root.number, root.generation, (const uint8_t *)"victim", 6,
		   file.number, file.generation, &time, NULL),
	    EXT4_INVALID_ARGUMENT);
	changed = root;
	changed.generation++;
	EXPECT(remove_inode(fs, &changed, "victim", &file, false, &result), EXT4_STALE);
	changed = file;
	changed.generation++;
	EXPECT(remove_inode(fs, &root, "victim", &changed, false, &result), EXT4_STALE);
	changed = file;
	changed.number++;
	EXPECT(remove_inode(fs, &root, "victim", &changed, false, &result), EXT4_STALE);
	EXPECT(remove_inode(fs, &root, "absent", &file, false, &result), EXT4_NOT_FOUND);
	EXPECT(remove_inode(fs, &file, "victim", &file, false, &result), EXT4_NOT_DIRECTORY);
	CHECK(device->writes == writes && memcmp(before, device->cache, device->size) == 0);
	CHECK(memcmp(&result, &untouched, sizeof(result)) == 0 && !fs->aborted);
	EXPECT(ext4_sync(fs), EXT4_OK);
	ext4_unmount(fs);
	writes = device->writes;
	EXPECT(ext4_mount(&device->environment, &fs), EXT4_OK);
	EXPECT(remove_inode(fs, &root, "victim", &file, false, &result), EXT4_READ_ONLY);
	CHECK(device->writes == writes && memcmp(&result, &untouched, sizeof(result)) == 0);
	ext4_unmount(fs);
	free(before);
	printf("PASS removal input, stale-identity and read-only guards without mutation\n");
}

static void
credit_guard(struct device *device)
{
	struct ext4_fs *fs;
	struct ext4_inode root;
	struct ext4_inode file;
	struct ext4_inode journal;
	struct ext4_inode result;
	struct ext4_inode untouched;
	struct ext4_jbd_super *super;
	uint64_t physical;
	uint8_t *before = malloc(device->size);

	CHECK(before != NULL);
	device_reset(device, device->base);
	fs = mount_writer(device, &root);
	file = create(fs, &root, "victim", false);
	EXPECT(ext4_get_inode(fs, fs->journal_inode, &journal), EXT4_OK);
	EXPECT(ext4_map_block(fs, &journal, 0, &physical), EXT4_OK);
	EXPECT(ext4_sync(fs), EXT4_OK);
	ext4_unmount(fs);
	device_reset(device, device->stable);
	super = (struct ext4_jbd_super *)(device->cache + physical * device->block_size);
	ext4_encode_be32(
	    &super->max_length, ext4_be32(&super->first) + 2 * TEST_REMOVAL_SMALL_CREDITS + 2);
	if (ext4_be32(&super->feature_incompat) & (EXT4_JBD_CSUM_V2 | EXT4_JBD_CSUM_V3)) {
		ext4_encode_be32(&super->checksum, 0);
		ext4_encode_be32(&super->checksum, ext4_crc32c(UINT32_MAX, super, sizeof(*super)));
	}
	memcpy(before, device->cache, device->size);
	fs = mount_writer(device, &root);
	CHECK(ext4_journal_credits(fs->journal) == TEST_REMOVAL_SMALL_CREDITS);
	memset(&result, 0xa5, sizeof(result));
	untouched = result;
	EXPECT(remove_inode(fs, &root, "victim", &file, false, &result), EXT4_RANGE);
	CHECK(memcmp(&result, &untouched, sizeof(result)) == 0 && !fs->aborted);
	CHECK(device->writes == 0 && memcmp(before, device->cache, device->size) == 0);
	ext4_unmount(fs);
	free(before);
	printf("PASS removal reserves restartable cleanup credits before namespace commit\n");
}

static void
held_files(struct device *device)
{
	struct ext4_fs *fs;
	struct ext4_fs *reader;
	struct ext4_inode root;
	struct ext4_inode file;
	struct ext4_inode other;
	struct ext4_inode replacement;
	struct ext4_inode result;
	struct ext4_inode_hold *hold;
	struct ext4_inode_hold *duplicate;
	struct ext4_inode_hold *other_hold;
	struct ext4_inode_update update = write_attributes();
	struct ext4_mapping mapping;
	uint8_t *bytes = malloc(device->block_size);
	uint8_t *observed = malloc(device->block_size);
	uint64_t free_blocks;
	uint32_t free_inodes;
	size_t completed;
	uint32_t block;

	CHECK(bytes != NULL && observed != NULL);
	memset(bytes, 0x5a, device->block_size);
	device_reset(device, device->base);
	fs = mount_writer(device, &root);
	free_blocks = fs->info.free_blocks;
	free_inodes = fs->info.free_inodes;
	file = create(fs, &root, "held-file", false);
	other = create(fs, &root, "other-file", false);
	EXPECT(ext4_hold_inode(fs, file.number, file.generation, &hold), EXT4_OK);
	EXPECT(ext4_hold_inode(fs, file.number, file.generation, &duplicate), EXT4_OK);
	CHECK(hold == duplicate && fs->hold_count == 1 && hold->references == 2);
	EXPECT(ext4_map_read_held(hold, 0, device->block_size, &mapping), EXT4_NOT_FOUND);
	EXPECT(ext4_hold_inode(fs, other.number, other.generation, &other_hold), EXT4_OK);
	for (block = 0; block < TEST_REMOVAL_DATA_BLOCKS; block++) {
		EXPECT(ext4_write(fs, file.number, file.generation,
			   (uint64_t)block * device->block_size, bytes, device->block_size, &update,
			   &completed),
		    EXT4_OK);
		CHECK(completed == device->block_size);
		EXPECT(ext4_read_held(hold, (uint64_t)block * device->block_size, observed,
			   device->block_size, &completed),
		    EXT4_OK);
		CHECK(completed == device->block_size && memcmp(bytes, observed, completed) == 0);
		EXPECT(ext4_map_read_held(hold, (uint64_t)block * device->block_size,
			   device->block_size, &mapping),
		    EXT4_OK);
		CHECK(!mapping.hole && mapping.length == device->block_size &&
		    mapping.device_offset <= device->size - device->block_size &&
		    memcmp(device->cache + mapping.device_offset, bytes, device->block_size) == 0);
	}
	EXPECT(remove_inode(fs, &root, "held-file", &file, false, &result), EXT4_OK);
	CHECK(result.links == 0 && hold->unlinked && fs->last_orphan == file.number);
	missing(fs, &root, "held-file");
	EXPECT(ext4_get_inode(fs, file.number, &result), EXT4_NOT_FOUND);
	EXPECT(ext4_read_held(hold, 0, observed, device->block_size, &completed), EXT4_OK);
	CHECK(completed == device->block_size && memcmp(bytes, observed, completed) == 0);
	EXPECT(ext4_refresh_inode(hold, &result), EXT4_OK);
	CHECK(result.links == 0 && result.size == TEST_REMOVAL_DATA_BLOCKS * device->block_size);
	EXPECT(ext4_read(fs, &result, 0, observed, device->block_size, &completed), EXT4_OK);
	CHECK(completed == device->block_size && memcmp(bytes, observed, completed) == 0);
	EXPECT(ext4_link(fs, root.number, root.generation, (const uint8_t *)"resurrect", 9,
		   file.number, file.generation, &update.change_time, &result),
	    EXT4_NOT_FOUND);
	EXPECT(remove_inode(fs, &root, "other-file", &other, false, &result), EXT4_OK);
	CHECK(fs->last_orphan == other.number);
	update = attributes();
	update.permissions = 0600;
	update.uid = 12345;
	update.gid = 23456;
	EXPECT(ext4_set_attributes(fs, file.number, file.generation, &update, &result), EXT4_OK);
	CHECK(result.links == 0 && result.uid == update.uid && result.gid == update.gid &&
	    (result.mode & ~EXT4_MODE_TYPE) == update.permissions);
	update = write_attributes();
	replacement = create(fs, &root, "replacement", false);
	CHECK(replacement.number != file.number && replacement.number != other.number);
	for (block = 0; block < TEST_REMOVAL_DATA_BLOCKS; block++) {
		EXPECT(ext4_write(fs, replacement.number, replacement.generation,
			   (uint64_t)block * device->block_size, bytes, device->block_size, &update,
			   &completed),
		    EXT4_OK);
	}
	EXPECT(ext4_truncate(fs, replacement.number, replacement.generation, 0, &update, &result),
	    EXT4_OK);
	CHECK(result.links == 1 && result.size == 0 && fs->last_orphan == other.number);
	EXPECT(remove_inode(fs, &root, "replacement", &replacement, false, &result), EXT4_OK);
	CHECK(fs->last_orphan == other.number);
	EXPECT(ext4_sync(fs), EXT4_OK);
	EXPECT(ext4_mount(&device->environment, &reader), EXT4_RECOVERY_REQUIRED);
	EXPECT(ext4_truncate(fs, file.number, file.generation, 1, &update, &result), EXT4_OK);
	CHECK(result.links == 0 && result.size == 1 && fs->last_orphan == other.number);
	EXPECT(ext4_read_held(hold, 0, observed, device->block_size, &completed), EXT4_OK);
	CHECK(completed == 1 && observed[0] == bytes[0]);
	EXPECT(ext4_map_read_held(hold, device->block_size, device->block_size, &mapping),
	    EXT4_NOT_FOUND);
	EXPECT(ext4_map_read_held(hold, 0, 2U * device->block_size, &mapping), EXT4_OK);
	CHECK(mapping.length == device->block_size && !mapping.hole);
	memset(bytes, 0x36, device->block_size);
	EXPECT(ext4_write(fs, file.number, file.generation, device->block_size + 7, bytes, 13,
		   &update, &completed),
	    EXT4_OK);
	CHECK(completed == 13);
	EXPECT(
	    ext4_read_held(duplicate, device->block_size + 7, observed, 13, &completed), EXT4_OK);
	CHECK(completed == 13 && memcmp(bytes, observed, completed) == 0);
	EXPECT(ext4_refresh_inode(hold, &result), EXT4_OK);
	EXPECT(ext4_read(fs, &result, 1, observed, device->block_size - 1, &completed), EXT4_OK);
	CHECK(completed == device->block_size - 1);
	for (block = 0; block < completed; block++) {
		CHECK(observed[block] == 0);
	}
	EXPECT(ext4_release_inode(duplicate), EXT4_OK);
	CHECK(hold->references == 1 && fs->info.free_inodes == free_inodes - 2);
	EXPECT(ext4_release_inode(hold), EXT4_OK);
	CHECK(fs->last_orphan == other.number && fs->info.free_inodes == free_inodes - 1);
	EXPECT(ext4_refresh_inode(other_hold, &result), EXT4_OK);
	CHECK(result.links == 0);
	EXPECT(ext4_release_inode(other_hold), EXT4_OK);
	CHECK(fs->last_orphan == 0 && fs->holds == NULL && fs->hold_count == 0);
	CHECK(fs->info.free_inodes == free_inodes && fs->info.free_blocks == free_blocks);
	replacement = create(fs, &root, "reuse", false);
	CHECK(replacement.number == file.number &&
	    replacement.generation == (file.generation == UINT32_MAX ? 1 : file.generation + 1));
	hold = NULL;
	EXPECT(ext4_hold_inode(fs, replacement.number, file.generation, &hold), EXT4_STALE);
	CHECK(hold == NULL);
	EXPECT(remove_inode(fs, &root, "reuse", &replacement, false, &result), EXT4_OK);
	EXPECT(ext4_sync(fs), EXT4_OK);
	ext4_unmount(fs);
	EXPECT(ext4_mount(&device->environment, &reader), EXT4_OK);
	ext4_unmount(reader);
	CHECK(device->live == 0);
	free(observed);
	free(bytes);
	printf("PASS held unlinked files: duplicate references, read/write/truncate, non-head "
	       "release\n");
}

static struct ext4_dir_header_disk *
find_entry(struct device *device, struct ext4_fs *fs, const struct ext4_inode *directory,
    const char *name, uint64_t *physical)
{
	struct ext4_dir_header_disk *entry;
	uint32_t logical;
	uint32_t offset;
	uint32_t length;
	uint32_t usable = device->block_size -
	    (device->metadata_checksum ? sizeof(struct ext4_dir_tail_disk) : 0);

	for (logical = 0; logical < directory->size / device->block_size; logical++) {
		EXPECT(ext4_map_block(fs, directory, logical, physical), EXT4_OK);
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
	}
	CHECK(false);
	return NULL;
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

static void
slot_name(char *name, uint32_t index)
{
	int length;

	length = snprintf(name, EXT4_NAME_MAX + 1, "slot-%08u", index);
	CHECK(length > 0 && (unsigned int)length < EXT4_NAME_MAX);
	memset(name + length, 'n', EXT4_NAME_MAX - (size_t)length);
	name[EXT4_NAME_MAX] = '\0';
}

static void
directory_slots(struct device *device)
{
	struct ext4_fs *fs;
	struct ext4_inode root;
	struct ext4_inode directory;
	struct ext4_inode target;
	struct ext4_inode result;
	struct ext4_dir_header_disk *entry;
	struct ext4_timestamp time = { .seconds = 1700000030 };
	struct ext4_super_disk *super;
	uint64_t physical;
	uint64_t free_blocks;
	uint32_t free_inodes;
	uint32_t count = 0;
	uint32_t first;
	uint32_t index;
	char name[EXT4_NAME_MAX + 1];

	device_reset(device, device->base);
	fs = mount_writer(device, &root);
	/* This case owns linear first-record deletion and reuse. Indexed
	 * creation and deletion have separate transition and graph tests. */
	super = (struct ext4_super_disk *)(device->cache + EXT4_SUPER_OFFSET);
	fs->info.feature_compat &= ~EXT4_FEATURE_COMPAT_DIR_INDEX;
	ext4_encode32(&super->feature_compat, fs->info.feature_compat);
	if (device->metadata_checksum) {
		ext4_encode32(&super->checksum,
		    ext4_crc32c(UINT32_MAX, super, offsetof(struct ext4_super_disk, checksum)));
	}
	free_blocks = fs->info.free_blocks;
	free_inodes = fs->info.free_inodes;
	directory = create(fs, &root, "slots", true);
	target = create(fs, &root, "slot-target", false);
	do {
		slot_name(name, count++);
		EXPECT(ext4_link(fs, directory.number, directory.generation, (const uint8_t *)name,
			   EXT4_NAME_MAX, target.number, target.generation, &time, &result),
		    EXT4_OK);
		EXPECT(ext4_get_inode(fs, directory.number, &directory), EXT4_OK);
		CHECK(count <= device->block_size / sizeof(struct ext4_dir_header_disk));
	} while (directory.size == device->block_size);
	CHECK(directory.size == 2U * device->block_size);
	first = count - 1;
	slot_name(name, count++);
	EXPECT(ext4_link(fs, directory.number, directory.generation, (const uint8_t *)name,
		   EXT4_NAME_MAX, target.number, target.generation, &time, &result),
	    EXT4_OK);
	slot_name(name, first);
	entry = find_entry(device, fs, &directory, name, &physical);
	CHECK((uint8_t *)entry == device->cache + physical * device->block_size);
	EXPECT(remove_inode(fs, &directory, name, &target, false, &result), EXT4_OK);
	CHECK(ext4_le32(&entry->inode) == 0 && entry->name_length == 0);
	missing(fs, &directory, name);
	slot_name(name, first + 1);
	EXPECT(ext4_lookup(fs, &directory, (const uint8_t *)name, EXT4_NAME_MAX, &result), EXT4_OK);
	CHECK(result.number == target.number);
	slot_name(name, first);
	EXPECT(ext4_link(fs, directory.number, directory.generation, (const uint8_t *)name,
		   EXT4_NAME_MAX, target.number, target.generation, &time, &result),
	    EXT4_OK);
	CHECK(ext4_le32(&entry->inode) == target.number);
	for (index = 0; index < count; index++) {
		slot_name(name, index);
		EXPECT(remove_inode(fs, &directory, name, &target, false, &result), EXT4_OK);
		CHECK(result.links == count - index);
	}
	EXPECT(ext4_get_inode(fs, directory.number, &result), EXT4_OK);
	CHECK(result.size == 2U * device->block_size);
	EXPECT(remove_inode(fs, &root, "slots", &directory, true, &result), EXT4_OK);
	EXPECT(remove_inode(fs, &root, "slot-target", &target, false, &result), EXT4_OK);
	CHECK(fs->info.free_blocks == free_blocks && fs->info.free_inodes == free_inodes);
	EXPECT(ext4_sync(fs), EXT4_OK);
	ext4_unmount(fs);
	CHECK(device->live == 0);
	printf("PASS first directory entry deletion, empty-slot reuse and multiblock rmdir\n");
}

enum malformed_removal {
	LATE_NAME_SLASH,
	DUPLICATE_NAME,
	WRONG_DOTDOT,
	IMMUTABLE_PARENT,
	IMMUTABLE_TARGET,
	PROTECTED_TARGET_MAP,
	WRONG_ENTRY_TYPE,
	DIRECTORY_CHECKSUM,
	MALFORMED_REMOVAL_COUNT
};

static void
malformed(struct device *device)
{
	struct ext4_fs *fs;
	struct ext4_inode root;
	struct ext4_inode file;
	struct ext4_inode journal;
	struct ext4_inode result;
	struct ext4_inode untouched;
	struct ext4_inode_disk *parent_disk;
	struct ext4_inode_disk *child_disk;
	struct ext4_dir_header_disk *entry;
	struct ext4_extent_disk *extent;
	struct ext4_inode_update update = write_attributes();
	uint8_t *before = malloc(device->size);
	uint8_t byte = 0x5a;
	uint64_t parent_offset;
	uint64_t child_offset;
	uint64_t physical;
	uint32_t writes;
	unsigned int scenario;
	unsigned int passed = 0;
	unsigned int skipped = 0;
	size_t completed;
	enum ext4_result expected;

	CHECK(before != NULL);
	for (scenario = 0; scenario < MALFORMED_REMOVAL_COUNT; scenario++) {
		device_reset(device, device->base);
		fs = mount_writer(device, &root);
		if ((scenario == DIRECTORY_CHECKSUM && !device->metadata_checksum) ||
		    (scenario == WRONG_ENTRY_TYPE &&
			!(fs->info.feature_incompat & EXT4_FEATURE_INCOMPAT_FILETYPE))) {
			skipped++;
			ext4_unmount(fs);
			continue;
		}
		file = create(fs, &root, "victim", scenario == WRONG_DOTDOT);
		(void)create(fs, &root, "future", false);
		if (scenario == PROTECTED_TARGET_MAP) {
			EXPECT(ext4_write(fs, file.number, file.generation, 0, &byte, 1, &update,
				   &completed),
			    EXT4_OK);
		}
		EXPECT(ext4_get_inode(fs, root.number, &root), EXT4_OK);
		EXPECT(ext4_get_inode(fs, file.number, &file), EXT4_OK);
		EXPECT(ext4_inode_location(fs, root.number, &parent_offset), EXT4_OK);
		EXPECT(ext4_inode_location(fs, file.number, &child_offset), EXT4_OK);
		parent_disk = (struct ext4_inode_disk *)(device->cache + parent_offset);
		child_disk = (struct ext4_inode_disk *)(device->cache + child_offset);
		expected = EXT4_CORRUPT;
		switch ((enum malformed_removal)scenario) {
		case LATE_NAME_SLASH:
		case DUPLICATE_NAME:
			entry = find_entry(device, fs, &root, "future", &physical);
			if (scenario == LATE_NAME_SLASH) {
				*((uint8_t *)entry + sizeof(*entry)) = '/';
			} else {
				memcpy((uint8_t *)entry + sizeof(*entry), "victim", 6);
			}
			directory_checksum(device, fs, &root, physical);
			break;
		case WRONG_DOTDOT:
			entry = find_entry(device, fs, &file, "..", &physical);
			ext4_encode32(&entry->inode, file.number);
			directory_checksum(device, fs, &file, physical);
			break;
		case IMMUTABLE_PARENT:
			ext4_encode32(&parent_disk->flags, root.flags | EXT4_INODE_IMMUTABLE);
			expected = EXT4_PERMISSION_DENIED;
			break;
		case IMMUTABLE_TARGET:
			ext4_encode32(&child_disk->flags, file.flags | EXT4_INODE_IMMUTABLE);
			expected = EXT4_PERMISSION_DENIED;
			break;
		case PROTECTED_TARGET_MAP:
			EXPECT(ext4_get_inode(fs, fs->journal_inode, &journal), EXT4_OK);
			EXPECT(ext4_map_block(fs, &journal, 0, &physical), EXT4_OK);
			if (file.flags & EXT4_INODE_EXTENTS) {
				extent = (struct ext4_extent_disk *)(child_disk->block_data +
				    sizeof(struct ext4_extent_header_disk));
				ext4_encode32(&extent->physical_lo, (uint32_t)physical);
				ext4_encode16(&extent->physical_hi, (uint16_t)(physical >> 32));
			} else {
				ext4_encode32(
				    (struct ext4_le32 *)child_disk->block_data, (uint32_t)physical);
			}
			break;
		case WRONG_ENTRY_TYPE:
			entry = find_entry(device, fs, &root, "victim", &physical);
			entry->type = EXT4_FT_DIRECTORY;
			directory_checksum(device, fs, &root, physical);
			break;
		case DIRECTORY_CHECKSUM:
			(void)find_entry(device, fs, &root, "victim", &physical);
			device->cache[physical * device->block_size + device->block_size - 1] ^= 1;
			break;
		case MALFORMED_REMOVAL_COUNT:
			CHECK(false);
		}
		ext4_inode_checksum_set(fs, root.number, parent_disk);
		ext4_inode_checksum_set(fs, file.number, child_disk);
		memcpy(before, device->cache, device->size);
		writes = device->writes;
		memset(&result, 0xa5, sizeof(result));
		untouched = result;
		EXPECT(remove_inode(fs, &root, "victim", &file, scenario == WRONG_DOTDOT, &result),
		    expected);
		CHECK(!fs->aborted && memcmp(&result, &untouched, sizeof(result)) == 0);
		CHECK(device->writes == writes && memcmp(before, device->cache, device->size) == 0);
		ext4_unmount(fs);
		passed++;
	}
	free(before);
	CHECK(device->live == 0);
	printf("PASS malformed removal cases=%u skipped_absent_features=%u\n", passed, skipped);
}

static void
directories_and_links(struct device *device, const char *exports, const char *path)
{
	struct ext4_fs *fs;
	struct ext4_inode root;
	struct ext4_inode directory;
	struct ext4_inode child;
	struct ext4_inode nested;
	struct ext4_inode result;
	struct ext4_inode_hold *hold;
	struct ext4_inode_update update = attributes();
	struct ext4_dir_entry entry;
	uint8_t target[EXT4_INODE_BLOCK_BYTES];
	uint8_t observed[EXT4_INODE_BLOCK_BYTES];
	uint64_t cookie = 0;
	uint32_t free_inodes;
	uint64_t free_blocks;
	unsigned int index;
	size_t completed;

	device_reset(device, device->base);
	fs = mount_writer(device, &root);
	free_inodes = fs->info.free_inodes;
	free_blocks = fs->info.free_blocks;
	directory = create(fs, &root, "removed-directory", true);
	child = create(fs, &directory, "child", false);
	nested = create(fs, &directory, "nested", true);
	EXPECT(remove_inode(fs, &root, "removed-directory", &directory, true, &result),
	    EXT4_NOT_EMPTY);
	EXPECT(remove_inode(fs, &root, "removed-directory", &directory, false, &result),
	    EXT4_IS_DIRECTORY);
	EXPECT(remove_inode(fs, &directory, "child", &child, true, &result), EXT4_NOT_DIRECTORY);
	EXPECT(remove_inode(fs, &directory, "nested", &nested, true, &result), EXT4_OK);
	EXPECT(remove_inode(fs, &directory, "child", &child, false, &result), EXT4_OK);
	EXPECT(ext4_hold_inode(fs, directory.number, directory.generation, &hold), EXT4_OK);
	EXPECT(remove_inode(fs, &root, "removed-directory", &directory, true, &result), EXT4_OK);
	CHECK(result.links == 0 && result.size == 0);
	EXPECT(ext4_refresh_inode(hold, &result), EXT4_OK);
	EXPECT(ext4_next_dir(fs, &result, &cookie, &entry), EXT4_NOT_FOUND);
	EXPECT(ext4_create(fs, result.number, result.generation, (const uint8_t *)"deleted", 7,
		   &update, &update.change_time, &child),
	    EXT4_NOT_FOUND);
	EXPECT(ext4_release_inode(hold), EXT4_OK);
	child = create(fs, &root, "first-name", false);
	EXPECT(ext4_link(fs, root.number, root.generation, (const uint8_t *)"second-name", 11,
		   child.number, child.generation, &update.change_time, &result),
	    EXT4_OK);
	EXPECT(remove_inode(fs, &root, "first-name", &child, false, &result), EXT4_OK);
	CHECK(result.links == 1 && fs->last_orphan == 0);
	EXPECT(remove_inode(fs, &root, "second-name", &child, false, &result), EXT4_OK);
	memset(target, 's', sizeof(target));
	for (index = 0; index < 2; index++) {
		EXPECT(ext4_symlink(fs, root.number, root.generation, (const uint8_t *)"link", 4,
			   target, sizeof(target) - index, &update, &update.change_time, &child),
		    EXT4_OK);
		EXPECT(ext4_hold_inode(fs, child.number, child.generation, &hold), EXT4_OK);
		EXPECT(remove_inode(fs, &root, "link", &child, false, &result), EXT4_OK);
		EXPECT(ext4_refresh_inode(hold, &result), EXT4_OK);
		CHECK(result.links == 0 && result.size == sizeof(target) - index);
		EXPECT(ext4_read(fs, &result, 0, observed, sizeof(observed), &completed), EXT4_OK);
		CHECK(completed == sizeof(target) - index &&
		    memcmp(target, observed, completed) == 0);
		EXPECT(ext4_sync(fs), EXT4_OK);
		EXPECT(ext4_release_inode(hold), EXT4_OK);
	}
	CHECK(fs->info.free_inodes == free_inodes && fs->info.free_blocks == free_blocks);
	EXPECT(ext4_get_inode(fs, root.number, &result), EXT4_OK);
	CHECK(result.links == root.links && result.change_time.seconds == 1700000030);
	EXPECT(ext4_sync(fs), EXT4_OK);
	ext4_unmount(fs);
	storage_export(device, exports, path, "removed-");
	CHECK(device->live == 0);
	printf("PASS remove directories, hardlink names and inline/mapped symlinks\n");
}

static void
held_crash(struct device *device)
{
	struct ext4_fs *fs;
	struct ext4_inode root;
	struct ext4_inode child;
	struct ext4_inode result;
	struct ext4_inode_hold *hold;
	uint8_t *expected = malloc(device->size);

	CHECK(expected != NULL);
	device_reset(device, device->base);
	fs = mount_writer(device, &root);
	child = create(fs, &root, "crash-held", false);
	EXPECT(remove_inode(fs, &root, "crash-held", &child, false, &result), EXT4_OK);
	EXPECT(ext4_sync(fs), EXT4_OK);
	memcpy(expected, device->stable, device->size);
	ext4_unmount(fs);
	device_reset(device, device->base);
	fs = mount_writer(device, &root);
	child = create(fs, &root, "crash-held", false);
	EXPECT(ext4_hold_inode(fs, child.number, child.generation, &hold), EXT4_OK);
	EXPECT(remove_inode(fs, &root, "crash-held", &child, false, &result), EXT4_OK);
	EXPECT(ext4_sync(fs), EXT4_OK);
	ext4_unmount(fs);
	CHECK(device->live == 0);
	CHECK(storage_recover(device, expected, true));
	free(expected);
	printf("PASS crash recovery reclaims committed open-unlinked inode\n");
}

enum removal_operation {
	REMOVE_ALIAS,
	REMOVE_FILE,
	REMOVE_DIRECTORY,
	REMOVE_SHORT_SYMLINK,
	REMOVE_LONG_SYMLINK
};

struct trace {
	uint32_t allocations;
	uint32_t reads;
	uint32_t events;
	uint32_t commit_event;
	bool committed;
};

static void
fault_fixture(struct device *device, enum removal_operation operation)
{
	struct ext4_fs *fs;
	struct ext4_inode root;
	struct ext4_inode inode;
	struct ext4_inode result;
	struct ext4_inode_update update = attributes();
	uint8_t *bytes = malloc(device->block_size);
	size_t completed;
	uint32_t block;

	CHECK(bytes != NULL);
	memset(bytes, 's', device->block_size);
	device_reset(device, device->base);
	fs = mount_writer(device, &root);
	if (operation >= REMOVE_SHORT_SYMLINK) {
		update.permissions = 0777;
		EXPECT(ext4_symlink(fs, root.number, root.generation, (const uint8_t *)"victim", 6,
			   bytes,
			   EXT4_INODE_BLOCK_BYTES - (operation == REMOVE_SHORT_SYMLINK ? 1U : 0U),
			   &update, &update.change_time, &inode),
		    EXT4_OK);
	} else {
		inode = create(fs, &root, "victim", operation == REMOVE_DIRECTORY);
		if (operation != REMOVE_DIRECTORY) {
			update = write_attributes();
			for (block = 0; block < TEST_REMOVAL_DATA_BLOCKS; block++) {
				EXPECT(ext4_write(fs, inode.number, inode.generation,
					   (uint64_t)block * device->block_size, bytes,
					   device->block_size, &update, &completed),
				    EXT4_OK);
				CHECK(completed == device->block_size);
			}
		}
		if (operation == REMOVE_ALIAS) {
			EXPECT(ext4_link(fs, root.number, root.generation,
				   (const uint8_t *)"kept-name", 9, inode.number, inode.generation,
				   &update.change_time, &result),
			    EXT4_OK);
		}
	}
	EXPECT(ext4_sync(fs), EXT4_OK);
	ext4_unmount(fs);
	memcpy(device->base, device->stable, device->size);
	free(bytes);
}

static enum ext4_result
attempt(struct device *device, enum removal_operation operation, unsigned int fault, uint32_t point,
    unsigned int survival, bool partial, struct trace *trace)
{
	struct ext4_fs *fs;
	struct ext4_inode root;
	struct ext4_inode inode;
	struct ext4_inode result;
	struct ext4_inode untouched;
	uint32_t allocations;
	uint32_t reads;
	uint32_t events;
	enum ext4_result error;

	fs = mount_writer(device, &root);
	EXPECT(ext4_lookup(fs, &root, (const uint8_t *)"victim", 6, &inode), EXT4_OK);
	allocations = device->allocations;
	reads = device->reads;
	events = device->events;
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
	error = remove_inode(fs, &root, "victim", &inode, operation == REMOVE_DIRECTORY, &result);
	if (error == EXT4_OK) {
		CHECK(result.links == (operation == REMOVE_ALIAS ? 1 : 0));
		error = ext4_sync(fs);
	} else {
		CHECK(memcmp(&result, &untouched, sizeof(result)) == 0);
	}
	trace->allocations = device->allocations - allocations;
	trace->reads = device->reads - reads;
	trace->events = device->events - events;
	trace->commit_event = device->commit_barrier;
	trace->committed = device->intent_durable;
	if (error != EXT4_OK) {
		if (device->writes != 0) {
			CHECK(fs->aborted);
			EXPECT(ext4_get_inode(fs, root.number, &result), EXT4_RECOVERY_REQUIRED);
		} else {
			CHECK(memcmp(device->cache, device->base, device->size) == 0);
			/* Commit first rereads the primary superblock. Its failure poisons
			 * the journal even though no write has yet been attempted. */
			CHECK(!fs->aborted || (error == EXT4_IO && fs->journal->aborted));
			if (fs->aborted) {
				EXPECT(ext4_get_inode(fs, root.number, &result),
				    EXT4_RECOVERY_REQUIRED);
			}
		}
	}
	ext4_unmount(fs);
	CHECK(device->live == 0);
	return error;
}

static void
fault_cases(struct device *device, enum removal_operation operation, bool smoke,
    const char *exports, const char *path)
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
	char prefix[32];

	CHECK(original != NULL && expected != NULL);
	memcpy(original, device->base, device->size);
	fault_fixture(device, operation);
	CHECK(snprintf(prefix, sizeof(prefix), "remove-before-%u-", operation) > 0);
	storage_export(device, exports, path, prefix);
	device_reset(device, device->base);
	EXPECT(attempt(device, operation, 0, 0, 0, false, &baseline), EXT4_OK);
	memcpy(expected, device->stable, device->size);
	CHECK(snprintf(prefix, sizeof(prefix), "remove-atomic-%u-", operation) > 0);
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
		printf("PASS removal faults operation=%u allocations=%u reads=%u cuts=%u "
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
		CHECK(snprintf(prefix, sizeof(prefix), "remove-pending-%u-", operation) > 0);
		storage_export(device, exports, path, prefix);
		CHECK(storage_recover(device, expected, true));
		device_reset(device, device->base);
		EXPECT(attempt(device, operation, 3, baseline.commit_event - 1, 0, false, &trace),
		    EXT4_IO);
		CHECK(!trace.committed);
		CHECK(snprintf(prefix, sizeof(prefix), "remove-uncommitted-%u-", operation) > 0);
		storage_export(device, exports, path, prefix);
		CHECK(storage_recover(device, expected, false));
	}
	memcpy(device->base, original, device->size);
	free(expected);
	free(original);
}

enum hold_operation { RELEASE_HEAD, RELEASE_NONHEAD, TRUNCATE_NONHEAD };

static enum ext4_result
release_attempt(struct device *device, enum hold_operation operation, unsigned int fault,
    uint32_t point, unsigned int survival, bool partial, struct trace *trace)
{
	struct ext4_fs *fs;
	struct ext4_inode root;
	struct ext4_inode victim;
	struct ext4_inode companion;
	struct ext4_inode result;
	struct ext4_inode untouched;
	struct ext4_inode_hold *hold;
	struct ext4_inode_hold *other = NULL;
	struct ext4_inode_update update = write_attributes();
	uint32_t allocations;
	uint32_t reads;
	uint32_t events;
	uint32_t writes;
	bool nonhead = operation != RELEASE_HEAD;
	bool closing = false;
	enum ext4_result error;

	fs = mount_writer(device, &root);
	EXPECT(ext4_lookup(fs, &root, (const uint8_t *)"victim", 6, &victim), EXT4_OK);
	EXPECT(ext4_hold_inode(fs, victim.number, victim.generation, &hold), EXT4_OK);
	EXPECT(remove_inode(fs, &root, "victim", &victim, false, &result), EXT4_OK);
	if (nonhead) {
		EXPECT(
		    ext4_lookup(fs, &root, (const uint8_t *)"companion", 9, &companion), EXT4_OK);
		EXPECT(
		    ext4_hold_inode(fs, companion.number, companion.generation, &other), EXT4_OK);
		EXPECT(remove_inode(fs, &root, "companion", &companion, false, &result), EXT4_OK);
		CHECK(fs->last_orphan == companion.number);
	}
	EXPECT(ext4_sync(fs), EXT4_OK);
	allocations = device->allocations;
	reads = device->reads;
	events = device->events;
	writes = device->writes;
	device->survival = survival;
	device->partial = partial;
	if (fault == 1) {
		device->fail_allocation = allocations + point;
	} else if (fault == 2) {
		device->fail_read = reads + point;
	} else if (fault == 3) {
		device->stop_at = events + point;
	}
	error = EXT4_OK;
	if (operation == TRUNCATE_NONHEAD) {
		memset(&result, 0xa5, sizeof(result));
		untouched = result;
		error = ext4_truncate(fs, victim.number, victim.generation, 0, &update, &result);
		if (error == EXT4_OK) {
			CHECK(result.size == 0 && result.links == 0 && result.blocks_512 == 0 &&
			    fs->last_orphan == companion.number);
		} else {
			CHECK(memcmp(&result, &untouched, sizeof(result)) == 0);
		}
	}
	if (error == EXT4_OK) {
		closing = true;
		error = ext4_release_inode(hold);
		CHECK(fs->hold_count == (nonhead ? 1U : 0U));
	}
	if (error == EXT4_OK && nonhead) {
		CHECK(fs->last_orphan == companion.number);
		error = ext4_release_inode(other);
	}
	if (error == EXT4_OK) {
		CHECK(fs->holds == NULL && fs->last_orphan == 0);
		error = ext4_sync(fs);
	}
	trace->allocations = device->allocations - allocations;
	trace->reads = device->reads - reads;
	trace->events = device->events - events;
	if (error != EXT4_OK) {
		if (closing || device->writes != writes) {
			CHECK(fs->aborted);
		}
		if (fs->aborted) {
			EXPECT(ext4_get_inode(fs, root.number, &result), EXT4_RECOVERY_REQUIRED);
		} else {
			device->fail_allocation = device->fail_read = 0;
			EXPECT(ext4_refresh_inode(hold, &result), EXT4_OK);
			CHECK(result.size == victim.size && result.links == 0);
		}
	}
	ext4_unmount(fs);
	CHECK(device->live == 0);
	return error;
}

static void
release_faults(struct device *device, enum hold_operation operation, bool smoke)
{
	struct ext4_fs *fs;
	struct ext4_inode root;
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

	CHECK(original != NULL && expected != NULL);
	memcpy(original, device->base, device->size);
	fault_fixture(device, REMOVE_FILE);
	if (operation != RELEASE_HEAD) {
		device_reset(device, device->base);
		fs = mount_writer(device, &root);
		(void)create(fs, &root, "companion", false);
		EXPECT(ext4_sync(fs), EXT4_OK);
		ext4_unmount(fs);
		memcpy(device->base, device->stable, device->size);
	}
	device_reset(device, device->base);
	EXPECT(release_attempt(device, operation, 0, 0, 0, false, &baseline), EXT4_OK);
	memcpy(expected, device->stable, device->size);
	if (!smoke) {
		for (fault = 1; fault <= 2; fault++) {
			limit = fault == 1 ? baseline.allocations : baseline.reads;
			for (point = 1; point <= limit; point++) {
				device_reset(device, device->base);
				EXPECT(release_attempt(
					   device, operation, fault, point, 0, false, &trace),
				    fault == 1 ? EXT4_NO_MEMORY : EXT4_IO);
				CHECK(storage_recover(device, expected, true));
			}
		}
		for (point = 1; point <= baseline.events; point++) {
			for (survival = 0; survival < 3; survival++) {
				for (partial = 0; partial < 2; partial++) {
					device_reset(device, device->base);
					EXPECT(release_attempt(device, operation, 3, point,
						   survival, partial != 0, &trace),
					    EXT4_IO);
					CHECK(device->off);
					if (storage_recover(device, expected, true)) {
						recovered++;
					} else {
						torn++;
					}
				}
			}
		}
		printf("PASS held inode faults operation=%u allocations=%u reads=%u cuts=%u "
		       "recovered=%u torn_super_fail_closed=%u\n",
		    operation, baseline.allocations, baseline.reads, baseline.events * 6, recovered,
		    torn);
	}
	memcpy(device->base, original, device->size);
	free(expected);
	free(original);
}

static void
indexed_guard(struct device *device)
{
	struct ext4_fs *fs;
	struct ext4_inode root;
	struct ext4_inode directory;
	struct ext4_inode file;
	struct ext4_inode result;
	struct ext4_inode untouched;

	fs = mount_writer(device, &root);
	EXPECT(ext4_lookup(fs, &root, (const uint8_t *)"many", 4, &directory), EXT4_OK);
	EXPECT(ext4_lookup(fs, &root, (const uint8_t *)"hello.txt", 9, &file), EXT4_OK);
	CHECK(directory.flags & EXT4_INODE_INDEX);
	memset(&result, 0xa5, sizeof(result));
	untouched = result;
	EXPECT(remove_inode(fs, &directory, "entry", &file, false, &result), EXT4_NOT_FOUND);
	EXPECT(remove_inode(fs, &root, "many", &directory, true, &result), EXT4_NOT_EMPTY);
	CHECK(memcmp(&result, &untouched, sizeof(result)) == 0);
	CHECK(device->writes == 0 && memcmp(device->cache, device->base, device->size) == 0);
	ext4_unmount(fs);
	printf(
	    "PASS indexed removal checks missing names and nonempty directories without writes\n");
}

int
main(int argc, char **argv)
{
	struct device device;
	const char *exports = NULL;
	bool smoke = false;
	bool indexed = false;
	unsigned int operation;
	int argument = 1;

	while (argument < argc && argv[argument][0] == '-') {
		if (strcmp(argv[argument], "--smoke") == 0) {
			smoke = true;
		} else if (strcmp(argv[argument], "--indexed") == 0) {
			indexed = true;
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
		if (indexed) {
			indexed_guard(&device);
			storage_close(&device);
			continue;
		}
		hold_faults(&device);
		held_inode_resolution(&device);
		held_write_metadata(&device);
		guards(&device);
		credit_guard(&device);
		malformed(&device);
		directory_slots(&device);
		held_files(&device);
		directories_and_links(&device, exports, argv[argument]);
		held_crash(&device);
		for (operation = REMOVE_ALIAS; operation <= REMOVE_LONG_SYMLINK; operation++) {
			fault_cases(&device, (enum removal_operation)operation, smoke, exports,
			    argv[argument]);
		}
		for (operation = RELEASE_HEAD; operation <= TRUNCATE_NONHEAD; operation++) {
			release_faults(&device, (enum hold_operation)operation, smoke);
		}
		storage_close(&device);
		printf("PASS removal: %s\n", argv[argument]);
	}
	return 0;
}

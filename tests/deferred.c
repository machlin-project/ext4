/* SPDX-License-Identifier: BSD-3-Clause */
#include "storage.h"

/* Deferred commit on e2fsprogs-authored images. A sequence of namespace, data,
 * attribute and truncation mutations joins one compound transaction: reads see it
 * before any device write, ext4_commit makes it durable at once, and a power cut at
 * every write or barrier of that commit recovers either none or all of it. With a
 * small compound, commits also happen when the next mutation would not fit, and a
 * power cut anywhere recovers exactly one of the images at those commit points. */

#define DEFERRED_SECONDS 1700010000
#define FILES 8U
/* One API call per step: the directory, a creation and a write per file, then a
 * rename, a link, an attribute change, a truncation and removal of the link, each a
 * single transaction, followed by two final removals that reclaim in further ones. */
#define SINGLE_STEPS (1U + 2U * FILES + 5U)
#define STEPS (SINGLE_STEPS + 2U)
#define LARGE_COMMIT_BLOCKS 256U
#define SMALL_COMMIT_BLOCKS 24U
#define NAME_BYTES 16U
#define PATTERN_SEED 0x5dU
/* Ordered overwrites: blocks of the fixture's payload replaced in place. */
#define OVERWRITE_FILE "payload.bin"
#define OVERWRITE_FIRST 2U
#define OVERWRITE_BLOCKS 4U
#define OVERWRITE_BYTE 0xa7U

static const struct ext4_timestamp deferred_time = { DEFERRED_SECONDS, 0 };
/* EXT4_WRITE_* flags of the current run. */
static uint32_t data_flags;
static const uint8_t directory_name[] = "deferred";
static const uint8_t attribute_name[] = "deferred";

static struct ext4_inode_update
creation(void)
{
	struct ext4_inode_update update = { 0 };

	update.fields = EXT4_ATTR_PERMISSIONS | EXT4_ATTR_UID | EXT4_ATTR_GID |
	    EXT4_ATTR_ACCESS_TIME | EXT4_ATTR_MODIFY_TIME | EXT4_ATTR_CHANGE_TIME |
	    EXT4_ATTR_XATTRS;
	update.permissions = 0644;
	update.access_time = deferred_time;
	update.modify_time = deferred_time;
	update.change_time = deferred_time;
	return update;
}

static struct ext4_inode_update
change(uint16_t permissions)
{
	struct ext4_inode_update update = { 0 };

	update.fields = EXT4_ATTR_PERMISSIONS | EXT4_ATTR_MODIFY_TIME | EXT4_ATTR_CHANGE_TIME |
	    EXT4_ATTR_XATTRS;
	update.permissions = permissions;
	update.modify_time = deferred_time;
	update.change_time = deferred_time;
	return update;
}

static void
file_name(char *name, uint32_t index)
{
	snprintf(name, NAME_BYTES, "f%u", index);
}

static void
pattern(uint8_t *data, size_t length, uint32_t index)
{
	size_t byte;

	for (byte = 0; byte < length; byte++) {
		data[byte] = (uint8_t)(PATTERN_SEED + index * 7U + byte % 251U);
	}
}

static enum ext4_result
lookup_name(struct ext4_fs *fs, const struct ext4_inode *directory, const char *name,
    struct ext4_inode *inode)
{
	return ext4_lookup(fs, directory, (const uint8_t *)name, strlen(name), inode);
}

/* Perform one step of the sequence; a power cut may end it with an error. */
static enum ext4_result
step(struct ext4_fs *fs, uint32_t index, uint32_t block_size)
{
	struct ext4_inode_update update = creation();
	struct ext4_xattr_change xattr = { EXT4_XATTR_SET, EXT4_XATTR_USER, attribute_name,
		sizeof(attribute_name) - 1U, "value", 5 };
	struct ext4_rename_entry from;
	struct ext4_rename_entry to;
	struct ext4_inode root;
	struct ext4_inode directory;
	struct ext4_inode inode;
	struct ext4_inode result;
	char name[NAME_BYTES];
	char other[NAME_BYTES];
	uint8_t *data;
	uint32_t file;
	size_t completed;
	size_t length;
	enum ext4_result error;

	error = ext4_get_inode(fs, EXT4_ROOT_INODE, &root);
	if (error != EXT4_OK) {
		return error;
	}
	if (index == 0) {
		update.permissions = 0755;
		return ext4_mkdir(fs, root.number, root.generation, directory_name,
		    sizeof(directory_name) - 1U, &update, &deferred_time, &directory);
	}
	error = ext4_lookup(fs, &root, directory_name, sizeof(directory_name) - 1U, &directory);
	if (error != EXT4_OK) {
		return error;
	}
	if (index <= 2U * FILES) {
		file = (index - 1U) / 2U;
		file_name(name, file);
		if (index % 2U != 0) {
			return ext4_create(fs, directory.number, directory.generation,
			    (const uint8_t *)name, strlen(name), &update, &deferred_time, &inode);
		}
		error = lookup_name(fs, &directory, name, &inode);
		if (error != EXT4_OK) {
			return error;
		}
		length = (size_t)(file + 1U) * block_size + file + 1U;
		data = malloc(length);
		CHECK(data != NULL);
		pattern(data, length, file);
		update = change(0644);
		error = ext4_write(
		    fs, inode.number, inode.generation, 0, data, length, &update, &completed);
		free(data);
		return error;
	}
	file = index - 2U * FILES;
	file_name(name, file == 1U ? 0 : file == 5U ? 1 : file == 6U ? 2 : file == 7U ? 5 : file);
	error = lookup_name(fs, &directory, name, &inode);
	if (error != EXT4_OK) {
		return error;
	}
	switch (file) {
	case 1:
		file_name(other, FILES);
		from = (struct ext4_rename_entry){ directory.number, directory.generation,
			(const uint8_t *)name, strlen(name), inode.number, inode.generation };
		to = (struct ext4_rename_entry){ directory.number, directory.generation,
			(const uint8_t *)other, strlen(other), 0, 0 };
		return ext4_rename(fs, &from, &to, 0, &deferred_time, &result);
	case 2:
		file_name(name, 1);
		error = lookup_name(fs, &directory, name, &inode);
		if (error != EXT4_OK) {
			return error;
		}
		return ext4_link(fs, root.number, root.generation, (const uint8_t *)"alias", 5,
		    inode.number, inode.generation, &deferred_time, &result);
	case 3:
		update = change(0600);
		update.xattrs = &xattr;
		update.xattr_count = 1;
		return ext4_set_attributes(fs, inode.number, inode.generation, &update, &result);
	case 4:
		update = change(0644);
		return ext4_truncate_atomic(
		    fs, inode.number, inode.generation, block_size, &update, &result);
	case 5:
		return ext4_unlink(fs, root.number, root.generation, (const uint8_t *)"alias", 5,
		    inode.number, inode.generation, &deferred_time, &result);
	default:
		return ext4_unlink(fs, directory.number, directory.generation,
		    (const uint8_t *)name, strlen(name), inode.number, inode.generation,
		    &deferred_time, &result);
	}
}

/* Reads of the pending state: names, data and attributes as the steps left them. */
static void
verify(struct ext4_fs *fs, uint32_t block_size)
{
	struct ext4_inode root;
	struct ext4_inode directory;
	struct ext4_inode inode;
	char name[NAME_BYTES];
	uint8_t value[8];
	uint8_t *expected;
	uint8_t *read_back;
	size_t completed;
	size_t length;
	size_t size;
	uint32_t index;

	EXPECT(ext4_get_inode(fs, EXT4_ROOT_INODE, &root), EXT4_OK);
	EXPECT(ext4_lookup(fs, &root, directory_name, sizeof(directory_name) - 1U, &directory),
	    EXT4_OK);
	for (index = 0; index <= FILES; index++) {
		file_name(name, index);
		if (index == 0 || index == 2 || index == 5) {
			EXPECT(lookup_name(fs, &directory, name, &inode), EXT4_NOT_FOUND);
			continue;
		}
		EXPECT(lookup_name(fs, &directory, name, &inode), EXT4_OK);
		/* The renamed first file keeps its contents under the last name. */
		length = index == FILES ? block_size + 1U
		    : index == 4	? block_size
					: (size_t)(index + 1U) * block_size + index + 1U;
		CHECK(inode.size == length);
		expected = malloc(length);
		read_back = malloc(length);
		CHECK(expected != NULL && read_back != NULL);
		pattern(expected, length, index == FILES ? 0 : index);
		EXPECT(ext4_read(fs, &inode, 0, read_back, length, &completed), EXT4_OK);
		CHECK(completed == length && memcmp(expected, read_back, length) == 0);
		free(expected);
		free(read_back);
	}
	EXPECT(ext4_lookup(fs, &root, (const uint8_t *)"alias", 5, &inode), EXT4_NOT_FOUND);
	file_name(name, 1);
	EXPECT(lookup_name(fs, &directory, name, &inode), EXT4_OK);
	CHECK(inode.links == 1);
	file_name(name, 3);
	EXPECT(lookup_name(fs, &directory, name, &inode), EXT4_OK);
	CHECK((inode.mode & 07777U) == 0600);
	EXPECT(ext4_get_xattr(fs, inode.number, inode.generation, EXT4_XATTR_USER, attribute_name,
		   sizeof(attribute_name) - 1U, value, sizeof(value), &size),
	    EXT4_OK);
	CHECK(size == 5 && memcmp(value, "value", 5) == 0);
}

static void
mount_deferred(struct device *device, uint32_t blocks, struct ext4_fs **fs)
{
	struct ext4_write_options options = { blocks, data_flags };

	EXPECT(ext4_mount_writable_with_options(
		   &device->environment, &device->writer, NULL, &options, fs),
	    EXT4_OK);
}

/* The image recovery produces from a durable state, which is what a power cut
 * leaves once recovery runs. */
static uint8_t *
recovered(struct device *scratch, const uint8_t *stable)
{
	struct ext4_recovery_report report;
	uint8_t *image = malloc(scratch->size);

	CHECK(image != NULL);
	device_reset(scratch, stable);
	EXPECT(ext4_recover(&scratch->environment, &scratch->writer, &report), EXT4_OK);
	memcpy(image, scratch->cache, scratch->size);
	return image;
}

/* Blocks an image's own bitmaps allocate. Ordered data may leave data of a
 * commit that did not become durable in blocks that stay free. */
static uint8_t *
allocated_blocks(struct device *scratch, const uint8_t *image)
{
	struct ext4_group group;
	struct ext4_fs *fs;
	uint8_t *allocated = calloc(scratch->blocks, 1);
	uint8_t *bitmap;
	uint64_t first;
	uint64_t block;
	uint32_t index;
	uint32_t bit;

	CHECK(allocated != NULL);
	device_reset(scratch, image);
	EXPECT(ext4_mount(&scratch->environment, &fs), EXT4_OK);
	bitmap = malloc(fs->info.block_size);
	CHECK(bitmap != NULL);
	for (block = 0; block < fs->first_data_block; block++) {
		allocated[block] = 1;
	}
	for (index = 0; index < fs->info.groups; index++) {
		EXPECT(ext4_group_get(fs, index, &group), EXT4_OK);
		if (group.flags & EXT4_GROUP_BLOCK_UNINIT) {
			continue;
		}
		EXPECT(ext4_block_read(fs, group.block_bitmap, bitmap), EXT4_OK);
		first = fs->first_data_block + (uint64_t)index * fs->blocks_per_group;
		for (bit = 0; bit < fs->clusters_per_group; bit++) {
			for (block = first + (uint64_t)bit * fs->cluster_blocks;
			    (bitmap[bit / EXT4_BITS_PER_BYTE] >> (bit % EXT4_BITS_PER_BYTE) & 1U) &&
			    block < first + (uint64_t)(bit + 1U) * fs->cluster_blocks &&
			    block < scratch->blocks;
			    block++) {
				allocated[block] = 1;
			}
		}
	}
	free(bitmap);
	ext4_unmount(fs);
	return allocated;
}

/* Under journaled data every block must match; under ordered data only those the
 * expected image allocates. */
static bool
equal(struct device *device, const uint8_t *expected, const uint8_t *allocated)
{
	uint32_t index;

	if (!(data_flags & EXT4_WRITE_ORDERED_DATA)) {
		return storage_equal(device, expected);
	}
	for (index = 0; index < device->blocks; index++) {
		if (!device->journal_blocks[index] && allocated[index] &&
		    memcmp(device->cache + (size_t)index * device->block_size,
			expected + (size_t)index * device->block_size, device->block_size) != 0) {
			return false;
		}
	}
	return true;
}

static bool
matches(struct device *device, uint8_t *const *images, uint8_t *const *maps, uint32_t count)
{
	uint32_t index;

	for (index = 0; index < count; index++) {
		if (equal(device, images[index], maps[index])) {
			return true;
		}
	}
	return false;
}

/* One compound: nothing reaches the device before the commit, and every cut of the
 * commit recovers none or all of the sequence. */
static void
atomic_commit(
    struct device *device, struct device *scratch, const char *exports, const char *source)
{
	struct ext4_recovery_report report;
	struct ext4_fs *fs;
	uint8_t *images[2];
	uint8_t *maps[2];
	uint32_t index;
	uint32_t events;
	uint32_t cut;
	uint32_t cuts = 0;
	uint32_t committed = 0;
	unsigned int survival;
	enum ext4_result error;

	device_reset(device, device->base);
	mount_deferred(device, LARGE_COMMIT_BLOCKS, &fs);
	for (index = 0; index < STEPS; index++) {
		EXPECT(step(fs, index, device->block_size), EXT4_OK);
	}
	/* Only ordered data reaches the device before the commit. */
	CHECK(device->writes == 0 || (data_flags & EXT4_WRITE_ORDERED_DATA));
	verify(fs, device->block_size);
	events = device->events;
	EXPECT(ext4_commit(fs), EXT4_OK);
	events = device->events - events;
	CHECK(events != 0);
	verify(fs, device->block_size);
	ext4_unmount(fs);
	images[0] = recovered(scratch, device->base);
	images[1] = recovered(scratch, device->stable);
	CHECK(memcmp(images[0], images[1], device->size) != 0);
	maps[0] = allocated_blocks(scratch, images[0]);
	maps[1] = allocated_blocks(scratch, images[1]);
	for (cut = 1; cut <= events; cut++) {
		for (survival = 0; survival < 3; survival++) {
			device_reset(device, device->base);
			mount_deferred(device, LARGE_COMMIT_BLOCKS, &fs);
			for (index = 0; index < STEPS; index++) {
				EXPECT(step(fs, index, device->block_size), EXT4_OK);
			}
			device->stop_at = device->events + cut;
			device->survival = survival;
			device->partial = (cut + survival) % 2 != 0;
			error = ext4_commit(fs);
			CHECK(error != EXT4_OK && device->off);
			ext4_unmount(fs);
			device_reset(device, device->stable);
			error = ext4_recover(&device->environment, &device->writer, &report);
			if (error == EXT4_CORRUPT) {
				/* A torn primary superblock fails closed without writes. */
				CHECK(device->metadata_checksum && device->writes == 0);
				continue;
			}
			EXPECT(error, EXT4_OK);
			CHECK(matches(device, images, maps, 2));
			committed += equal(device, images[1], maps[1]) ? 1U : 0U;
			cuts++;
		}
	}
	/* The final state is also what the owner reads back after recovery. */
	device_reset(device, images[1]);
	EXPECT(ext4_mount(&device->environment, &fs), EXT4_OK);
	verify(fs, device->block_size);
	ext4_unmount(fs);
	memcpy(device->stable, images[1], device->size);
	storage_export(device, exports, source, "deferred-");
	for (index = 0; index < 2; index++) {
		free(images[index]);
		free(maps[index]);
	}
	printf("PASS deferred atomic commit, %s data: %u steps, %u commit events, %u cuts, "
	       "%u committed\n",
	    data_flags ? "ordered" : "journaled", STEPS, events, cuts, committed);
}

/* A small compound commits whenever the next mutation would not fit; any cut
 * recovers exactly the state at one of those commits. */
static void
capacity_commits(struct device *device, struct device *scratch)
{
	struct ext4_recovery_report report;
	struct ext4_fs *fs;
	uint8_t *images[STEPS + 2U];
	uint8_t *maps[STEPS + 2U];
	uint32_t count = 0;
	uint32_t index;
	uint32_t before;
	uint32_t events;
	uint32_t cut;
	uint32_t cuts = 0;
	uint32_t index_matched;
	enum ext4_result error;

	device_reset(device, device->base);
	images[count++] = recovered(scratch, device->base);
	device_reset(device, device->base);
	mount_deferred(device, SMALL_COMMIT_BLOCKS, &fs);
	for (index = 0; index < SINGLE_STEPS; index++) {
		before = device->events;
		EXPECT(step(fs, index, device->block_size), EXT4_OK);
		if (device->events != before) {
			images[count++] = recovered(scratch, device->stable);
		}
	}
	EXPECT(ext4_commit(fs), EXT4_OK);
	images[count++] = recovered(scratch, device->stable);
	events = device->events;
	ext4_unmount(fs);
	CHECK(count > 3U);
	for (index = 0; index < count; index++) {
		maps[index] = allocated_blocks(scratch, images[index]);
	}
	for (cut = 1; cut <= events; cut++) {
		device_reset(device, device->base);
		mount_deferred(device, SMALL_COMMIT_BLOCKS, &fs);
		device->stop_at = cut;
		device->survival = cut % 3U;
		device->partial = cut % 2U != 0;
		error = EXT4_OK;
		for (index = 0; index < SINGLE_STEPS && error == EXT4_OK; index++) {
			error = step(fs, index, device->block_size);
		}
		if (error == EXT4_OK) {
			error = ext4_commit(fs);
		}
		CHECK(error != EXT4_OK && device->off);
		ext4_unmount(fs);
		device_reset(device, device->stable);
		error = ext4_recover(&device->environment, &device->writer, &report);
		if (error == EXT4_CORRUPT) {
			CHECK(device->metadata_checksum && device->writes == 0);
			continue;
		}
		EXPECT(error, EXT4_OK);
		index_matched = 0;
		while (index_matched < count &&
		    !equal(device, images[index_matched], maps[index_matched])) {
			index_matched++;
		}
		CHECK(index_matched < count);
		cuts++;
	}
	for (index = 0; index < count; index++) {
		free(images[index]);
		free(maps[index]);
	}
	printf("PASS deferred capacity commits, %s data: %u commit points, %u events, %u cuts\n",
	    data_flags ? "ordered" : "journaled", count, events, cuts);
}

/* Ordered data writes overwrites of existing blocks in place before the commit: after
 * any cut each overwritten block holds its old or new contents, never anything else,
 * the file keeps its size, and a durable commit implies the new contents. */
static void
ordered_overwrites(struct device *device)
{
	struct ext4_recovery_report report;
	struct ext4_inode_update update = change(0644);
	struct ext4_inode_update creation_update = creation();
	struct ext4_inode root;
	struct ext4_inode file;
	struct ext4_inode marker;
	struct ext4_fs *fs;
	uint8_t *original;
	uint8_t *contents;
	uint8_t *replacement;
	size_t completed;
	size_t length;
	size_t offset;
	size_t sector;
	uint32_t block;
	uint32_t events;
	uint32_t cut;
	uint32_t cuts = 0;
	uint32_t mixed = 0;
	bool committed;
	bool fresh;
	bool changed;
	enum ext4_result error;

	device_reset(device, device->base);
	EXPECT(ext4_mount(&device->environment, &fs), EXT4_OK);
	EXPECT(ext4_get_inode(fs, EXT4_ROOT_INODE, &root), EXT4_OK);
	EXPECT(
	    ext4_lookup(fs, &root, (const uint8_t *)OVERWRITE_FILE, strlen(OVERWRITE_FILE), &file),
	    EXT4_OK);
	length = (size_t)file.size;
	CHECK(length >= (OVERWRITE_FIRST + OVERWRITE_BLOCKS) * device->block_size);
	original = malloc(length);
	contents = malloc(length);
	replacement = malloc((size_t)OVERWRITE_BLOCKS * device->block_size);
	CHECK(original != NULL && contents != NULL && replacement != NULL);
	EXPECT(ext4_read(fs, &file, 0, original, length, &completed), EXT4_OK);
	CHECK(completed == length);
	ext4_unmount(fs);
	memset(replacement, OVERWRITE_BYTE, (size_t)OVERWRITE_BLOCKS * device->block_size);
	events = 0;
	for (cut = 0; cut == 0 || cut <= events; cut++) {
		device_reset(device, device->base);
		mount_deferred(device, LARGE_COMMIT_BLOCKS, &fs);
		/* Cuts cover the in-place data writes as well as the commit. */
		device->stop_at = cut;
		device->survival = cut % 3U;
		device->partial = cut % 2U != 0;
		error = ext4_write(fs, file.number, file.generation,
		    (uint64_t)OVERWRITE_FIRST * device->block_size, replacement,
		    (size_t)OVERWRITE_BLOCKS * device->block_size, &update, &completed);
		if (error == EXT4_OK) {
			error =
			    ext4_create(fs, root.number, root.generation, (const uint8_t *)"marker",
				6, &creation_update, &deferred_time, &marker);
		}
		if (error == EXT4_OK) {
			error = ext4_commit(fs);
		}
		if (cut == 0) {
			EXPECT(error, EXT4_OK);
			events = device->events;
			ext4_unmount(fs);
			continue;
		}
		CHECK(error != EXT4_OK && device->off);
		ext4_unmount(fs);
		committed = device->intent_durable;
		device_reset(device, device->stable);
		error = ext4_recover(&device->environment, &device->writer, &report);
		if (error == EXT4_CORRUPT) {
			CHECK(device->metadata_checksum && device->writes == 0);
			continue;
		}
		EXPECT(error, EXT4_OK);
		EXPECT(ext4_mount(&device->environment, &fs), EXT4_OK);
		EXPECT(ext4_get_inode(fs, EXT4_ROOT_INODE, &root), EXT4_OK);
		fresh = ext4_lookup(fs, &root, (const uint8_t *)"marker", 6, &marker) == EXT4_OK;
		CHECK(fresh || !committed);
		EXPECT(ext4_get_inode(fs, file.number, &file), EXT4_OK);
		CHECK(file.size == length);
		EXPECT(ext4_read(fs, &file, 0, contents, length, &completed), EXT4_OK);
		CHECK(completed == length);
		changed = false;
		for (block = 0; (size_t)block * device->block_size < length; block++) {
			offset = (size_t)block * device->block_size;
			if (block < OVERWRITE_FIRST ||
			    block >= OVERWRITE_FIRST + OVERWRITE_BLOCKS) {
				CHECK(memcmp(contents + offset, original + offset,
					  length - offset < device->block_size
					      ? length - offset
					      : device->block_size) == 0);
				continue;
			}
			/* Each sector of an overwritten block is old or new; torn blocks are
			 * possible, as with Linux's ordered data. A durable commit implies new. */
			for (sector = 0; sector < device->block_size; sector += EXT4_SECTOR_SIZE) {
				if (memcmp(contents + offset + sector,
					replacement + offset + sector -
					    (size_t)OVERWRITE_FIRST * device->block_size,
					EXT4_SECTOR_SIZE) == 0) {
					changed = true;
					continue;
				}
				CHECK(!fresh &&
				    memcmp(contents + offset + sector, original + offset + sector,
					EXT4_SECTOR_SIZE) == 0);
			}
		}
		mixed += changed && !fresh ? 1U : 0U;
		ext4_unmount(fs);
		cuts++;
	}
	free(replacement);
	free(contents);
	free(original);
	printf("PASS ordered overwrites: %u events, %u cuts, %u with new data before the "
	       "commit\n",
	    events, cuts, mixed);
}

int
main(int argc, char **argv)
{
	static struct device device;
	static struct device scratch;
	const char *exports = NULL;
	int first = 1;
	int index;

	if (argc >= 3 && strcmp(argv[1], "--export") == 0) {
		exports = argv[2];
		first = 3;
	}
	if (argc <= first) {
		fprintf(stderr, "usage: %s [--export DIRECTORY] IMAGE...\n", argv[0]);
		return 2;
	}
	for (index = first; index < argc; index++) {
		storage_open(&device, argv[index]);
		storage_open(&scratch, argv[index]);
		data_flags = 0;
		atomic_commit(&device, &scratch, exports, argv[index]);
		capacity_commits(&device, &scratch);
		data_flags = EXT4_WRITE_ORDERED_DATA;
		atomic_commit(&device, &scratch, NULL, argv[index]);
		capacity_commits(&device, &scratch);
		ordered_overwrites(&device);
		storage_close(&scratch);
		storage_close(&device);
		printf("PASS deferred commit: %s\n", argv[index]);
	}
	return 0;
}

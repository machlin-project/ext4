/* SPDX-License-Identifier: BSD-3-Clause */
#include "journal.h"
#include "image.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define VOLUME_SECONDS 1700000500
#define SEED_SPANS 5U
#define SPAN_BYTES 32U
#define VALUE_BYTES 300U
/* Complete each preceding barrier, then report an uncertain result to stop
 * before the commit record or before the first metadata checkpoint. */
#define CUT_BEFORE_COMMIT 3U
#define CUT_AFTER_COMMIT 4U

#define CHECK(expression)                                                                          \
	do {                                                                                       \
		if (!(expression)) {                                                               \
			fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #expression);           \
			exit(1);                                                                   \
		}                                                                                  \
	} while (0)
#define EXPECT(expression, expected) result_is((expression), (expected), #expression, __LINE__)

struct cut_writer {
	struct ext4_write_environment underlying;
	unsigned int flushes;
	unsigned int stop;
};

static void
result_is(enum ext4_result actual, enum ext4_result expected, const char *operation, int line)
{
	if (actual != expected) {
		fprintf(stderr, "%s:%d: %s: %s, expected %s\n", __FILE__, line, operation,
		    ext4_result_string(actual), ext4_result_string(expected));
		exit(1);
	}
}

static enum ext4_result
cut_write(void *context, uint64_t offset, const void *buffer, size_t length)
{
	struct cut_writer *cut = context;

	return cut->underlying.write(cut->underlying.context, offset, buffer, length);
}

static enum ext4_result
cut_flush(void *context)
{
	struct cut_writer *cut = context;
	enum ext4_result error;

	error = cut->underlying.flush(cut->underlying.context);
	cut->flushes++;
	return error == EXT4_OK && cut->flushes == cut->stop ? EXT4_IO : error;
}

static struct ext4_inode_update
attributes(bool create)
{
	struct ext4_inode_update update = { 0 };

	update.fields = EXT4_ATTR_PERMISSIONS | EXT4_ATTR_CHANGE_TIME | EXT4_ATTR_MODIFY_TIME |
	    EXT4_ATTR_XATTRS;
	if (create) {
		update.fields |= EXT4_ATTR_UID | EXT4_ATTR_GID | EXT4_ATTR_ACCESS_TIME;
	}
	update.permissions = 0640;
	update.uid = 70000;
	update.gid = 80000;
	update.change_time.seconds = VOLUME_SECONDS;
	update.modify_time.seconds = VOLUME_SECONDS;
	update.access_time.seconds = VOLUME_SECONDS;
	return update;
}

static struct ext4_inode
find(struct ext4_fs *fs, const struct ext4_inode *parent, const char *name)
{
	struct ext4_inode inode;

	EXPECT(ext4_lookup(fs, parent, (const uint8_t *)name, strlen(name), &inode), EXT4_OK);
	return inode;
}

static uint64_t
high_first(const struct ext4_fs *fs)
{
	return fs->first_data_block + (uint64_t)(fs->info.groups - 1U) * fs->blocks_per_group;
}

static void
read_span(struct ext4_fs *fs, struct ext4_posix_image *image, const struct ext4_inode *inode,
    uint64_t offset, uint8_t value, size_t length)
{
	struct ext4_mapping mapping;
	uint8_t bytes[64];
	uint8_t raw[64];
	size_t completed;
	size_t index;

	CHECK(length <= sizeof(bytes));
	EXPECT(ext4_read(fs, inode, offset, bytes, length, &completed), EXT4_OK);
	CHECK(completed == length);
	for (index = 0; index < length; index++) {
		CHECK(bytes[index] == value);
	}
	if (value != 0) {
		EXPECT(ext4_map_read(fs, inode, offset, length, &mapping), EXT4_OK);
		CHECK(!mapping.hole && mapping.length == length &&
		    mapping.device_offset / fs->info.block_size >= high_first(fs));
		EXPECT(image->environment.read(image, mapping.device_offset, raw, length), EXT4_OK);
		CHECK(memcmp(raw, bytes, length) == 0);
	}
}

static void
seed_checks(struct ext4_fs *fs, struct ext4_posix_image *image, const struct ext4_inode *directory)
{
	struct ext4_inode seed = find(fs, directory, "seed");
	uint8_t value[VALUE_BYTES];
	uint64_t cluster_bytes = (uint64_t)fs->cluster_blocks * fs->info.block_size;
	size_t size;
	unsigned int index;

	CHECK(seed.size == 8U * cluster_bytes + 7U + SPAN_BYTES && seed.generation == 123);
	for (index = 0; index < SEED_SPANS; index++) {
		read_span(fs, image, &seed, 2U * index * cluster_bytes + 7U, (uint8_t)('A' + index),
		    SPAN_BYTES);
	}
	read_span(fs, image, &seed, cluster_bytes + 13U, 0, SPAN_BYTES);
	EXPECT(ext4_get_xattr(fs, seed.number, seed.generation, EXT4_XATTR_USER,
		   (const uint8_t *)"large", 5, value, sizeof(value), &size),
	    EXT4_OK);
	CHECK(size == sizeof(value));
	for (index = 0; index < sizeof(value); index++) {
		CHECK(value[index] == (uint8_t)(0x90U + index % 23U));
	}
}

static struct ext4_inode
create(struct ext4_fs *fs, const struct ext4_inode *directory, bool interrupted)
{
	struct ext4_inode_update update = attributes(true);
	struct ext4_inode inode;
	struct ext4_inode before;
	struct ext4_xattr_change change = { EXT4_XATTR_CREATE, EXT4_XATTR_USER,
		(const uint8_t *)"large", 5, NULL, VALUE_BYTES };
	uint8_t value[VALUE_BYTES];

	memset(value, 'x', sizeof(value));
	change.value = value;
	update.xattrs = &change;
	update.xattr_count = 1;
	memset(&inode, 0xa5, sizeof(inode));
	before = inode;
	EXPECT(ext4_create(fs, directory->number, directory->generation, (const uint8_t *)"created",
		   7, &update, &update.change_time, &inode),
	    interrupted ? EXT4_IO : EXT4_OK);
	if (interrupted) {
		CHECK(memcmp(&inode, &before, sizeof(inode)) == 0);
	} else {
		CHECK((inode.number - 1U) / fs->inodes_per_group == fs->info.groups - 1U);
	}
	return inode;
}

static void
write_bytes(
    struct ext4_fs *fs, struct ext4_inode *inode, uint64_t offset, uint8_t value, size_t size)
{
	struct ext4_inode_update update = attributes(false);
	uint8_t bytes[64];
	size_t completed;

	CHECK(size <= sizeof(bytes));
	memset(bytes, value, size);
	EXPECT(ext4_write(
		   fs, inode->number, inode->generation, offset, bytes, size, &update, &completed),
	    EXT4_OK);
	CHECK(completed == size);
	EXPECT(ext4_get_inode(fs, inode->number, inode), EXT4_OK);
}

static void
mutate(struct ext4_fs *fs, struct ext4_posix_image *image, const struct ext4_inode *directory)
{
	struct ext4_inode inode = create(fs, directory, false);
	struct ext4_inode_update update = attributes(false);
	struct ext4_xattr_change change = { EXT4_XATTR_REPLACE, EXT4_XATTR_USER,
		(const uint8_t *)"large", 5, NULL, VALUE_BYTES };
	uint8_t value[VALUE_BYTES];
	uint64_t cluster_bytes = (uint64_t)fs->cluster_blocks * fs->info.block_size;
	uint64_t completed;
	unsigned int index;

	for (index = 0; index < SEED_SPANS; index++) {
		write_bytes(fs, &inode, 2U * index * cluster_bytes + 7U, (uint8_t)('K' + index),
		    SPAN_BYTES);
	}
	write_bytes(fs, &inode, 2U * cluster_bytes + fs->info.block_size + 13U, 'Z', 17);
	EXPECT(ext4_fallocate(fs, inode.number, inode.generation, 0, fs->info.block_size,
		   EXT4_FALLOC_KEEP_SIZE | EXT4_FALLOC_PUNCH_HOLE, &update, &completed),
	    EXT4_OK);
	CHECK(completed == fs->info.block_size);
	EXPECT(ext4_fallocate(fs, inode.number, inode.generation,
		   12U * cluster_bytes + fs->info.block_size, fs->info.block_size,
		   EXT4_FALLOC_KEEP_SIZE, &update, &completed),
	    EXT4_OK);
	CHECK(completed == fs->info.block_size);
	memset(value, 'y', sizeof(value));
	change.value = value;
	update.xattrs = &change;
	update.xattr_count = 1;
	EXPECT(ext4_set_attributes(fs, inode.number, inode.generation, &update, &inode), EXT4_OK);
	for (index = 1; index < SEED_SPANS; index++) {
		read_span(fs, image, &inode, 2U * index * cluster_bytes + 7U,
		    (uint8_t)('K' + index), SPAN_BYTES);
	}
	read_span(fs, image, &inode, 0, 0, SPAN_BYTES);
	read_span(fs, image, &inode, 2U * cluster_bytes + fs->info.block_size + 13U, 'Z', 17);
}

static void
reclaim(struct ext4_fs *fs, const struct ext4_inode *directory)
{
	struct ext4_inode inode = find(fs, directory, "created");
	struct ext4_inode result;
	struct ext4_inode_hold *hold;
	struct ext4_timestamp time = { VOLUME_SECONDS, 0 };
	uint64_t free_before = fs->info.free_blocks;
	uint64_t charged = inode.blocks_512 * EXT4_SECTOR_SIZE / fs->info.block_size;
	uint32_t free_inodes = fs->info.free_inodes;

	EXPECT(ext4_hold_inode(fs, inode.number, inode.generation, &hold), EXT4_OK);
	EXPECT(ext4_unlink(fs, directory->number, directory->generation, (const uint8_t *)"created",
		   7, inode.number, inode.generation, &time, &result),
	    EXT4_OK);
	EXPECT(ext4_refresh_inode(hold, &result), EXT4_OK);
	CHECK(result.links == 0 && result.size == inode.size &&
	    result.blocks_512 == inode.blocks_512);
	EXPECT(ext4_release_inode(hold), EXT4_OK);
	CHECK(fs->info.free_blocks == free_before + charged &&
	    fs->info.free_inodes == free_inodes + 1U);
}

int
main(int argc, char **argv)
{
	struct ext4_posix_image image;
	struct ext4_fs *fs;
	struct ext4_inode root;
	struct ext4_inode directory;
	struct cut_writer cut = { 0 };
	struct ext4_write_environment writer = { &cut, cut_write, cut_flush };
	bool read_only;
	bool interrupted;

	CHECK(argc == 3);
	read_only = strcmp(argv[1], "--read") == 0;
	interrupted =
	    strcmp(argv[1], "--before-commit") == 0 || strcmp(argv[1], "--after-commit") == 0;
	if (read_only) {
		EXPECT(ext4_posix_open(&image, argv[2]), EXT4_OK);
		EXPECT(ext4_mount(&image.environment, &fs), EXT4_OK);
	} else {
		EXPECT(ext4_posix_open_writable(&image, argv[2]), EXT4_OK);
		cut.underlying = image.writer;
		EXPECT(ext4_mount_writable(&image.environment, &writer, &fs), EXT4_OK);
	}
	CHECK(fs->info.blocks > (UINT64_C(1) << 31) && fs->cluster_blocks > 1);
	EXPECT(ext4_get_inode(fs, EXT4_ROOT_INODE, &root), EXT4_OK);
	directory = find(fs, &root, "upper");
	CHECK((directory.number - 1U) / fs->inodes_per_group == fs->info.groups - 1U);
	seed_checks(fs, &image, &directory);
	if (interrupted) {
		cut.flushes = 0;
		cut.stop =
		    strcmp(argv[1], "--before-commit") == 0 ? CUT_BEFORE_COMMIT : CUT_AFTER_COMMIT;
		(void)create(fs, &directory, true);
		CHECK(cut.flushes == cut.stop);
	} else if (!read_only) {
		if (strcmp(argv[1], "--mutate") == 0) {
			mutate(fs, &image, &directory);
		} else {
			CHECK(strcmp(argv[1], "--reclaim") == 0);
			reclaim(fs, &directory);
		}
		seed_checks(fs, &image, &directory);
		EXPECT(ext4_sync(fs), EXT4_OK);
	}
	ext4_unmount(fs);
	CHECK(image.live_allocations == 0 && (!read_only || image.write_calls == 0));
	printf("PASS %s: reads=%" PRIu64 " writes=%" PRIu64 " flushes=%" PRIu64
	       " allocations=%" PRIu64 "\n",
	    argv[1], image.read_calls, image.write_calls, image.flush_calls,
	    image.allocation_calls);
	ext4_posix_close(&image);
	return 0;
}

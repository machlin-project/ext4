/* SPDX-License-Identifier: BSD-3-Clause */
#include "storage.h"

#include <inttypes.h>

#define LARGE_FILE_SECONDS 1700000400
#define SIGNED_BYTE_BOUNDARY (UINT64_C(1) << 31)
#define UNSIGNED_BYTE_BOUNDARY (UINT64_C(1) << 32)
#define SPAN_CAPACITY 8U

struct span {
	uint64_t offset;
	size_t length;
	uint8_t byte;
};

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
	update.access_time.seconds = LARGE_FILE_SECONDS;
	update.modify_time.seconds = LARGE_FILE_SECONDS;
	update.change_time.seconds = LARGE_FILE_SECONDS;
	return update;
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

static size_t
seed_spans(struct span *spans, uint32_t block, uint64_t limit)
{
	size_t count = 3;
	uint64_t signed_logical = SIGNED_BYTE_BOUNDARY * block - 3U;

	spans[0] = (struct span){ 0, 7, 'A' };
	spans[1] = (struct span){ SIGNED_BYTE_BOUNDARY - 3U, 7, 'B' };
	spans[2] = (struct span){ UNSIGNED_BYTE_BOUNDARY - 3U, 7, 'C' };
	if (signed_logical + 7U < limit) {
		spans[count++] = (struct span){ signed_logical, 7, 'D' };
	}
	spans[count++] = (struct span){ limit - 1U, 1, 'Z' };
	return count;
}

static void
read_range(struct ext4_fs *fs, const struct ext4_inode *inode, uint64_t offset,
    const struct span *spans, size_t count)
{
	uint8_t observed[96];
	uint8_t expected[96] = { 0 };
	struct ext4_mapping mapping;
	size_t size;
	size_t completed;
	size_t index;
	size_t span;

	size = offset >= inode->size		      ? 0
	    : inode->size - offset < sizeof(observed) ? (size_t)(inode->size - offset)
						      : sizeof(observed);
	for (index = 0; index < size; index++) {
		for (span = 0; span < count; span++) {
			if (offset + index >= spans[span].offset &&
			    offset + index - spans[span].offset < spans[span].length) {
				expected[index] = spans[span].byte;
			}
		}
	}
	memset(observed, 0xa5, sizeof(observed));
	EXPECT(ext4_read(fs, inode, offset, observed, sizeof(observed), &completed), EXT4_OK);
	CHECK(completed == size && memcmp(observed, expected, size) == 0);
	for (index = size; index < sizeof(observed); index++) {
		CHECK(observed[index] == 0xa5);
	}
	if (size == 0) {
		EXPECT(ext4_map_read(fs, inode, offset, 1, &mapping), EXT4_NOT_FOUND);
	} else if (!(inode->flags & EXT4_INODE_INLINE_DATA)) {
		EXPECT(ext4_map_read(fs, inode, offset, size, &mapping), EXT4_OK);
		CHECK(mapping.length > 0 && mapping.length <= size);
		if (mapping.hole) {
			memset(observed, 0, mapping.length);
		} else {
			EXPECT(fs->environment.read(fs->environment.context, mapping.device_offset,
				   observed, mapping.length),
			    EXT4_OK);
		}
		CHECK(memcmp(observed, expected, mapping.length) == 0);
	}
}

static void
verify(struct ext4_fs *fs, const struct ext4_inode *inode, uint64_t limit, const struct span *spans,
    size_t count)
{
	size_t index;

	CHECK(inode->size == limit);
	for (index = 0; index < count; index++) {
		read_range(fs, inode, spans[index].offset > 11U ? spans[index].offset - 11U : 0,
		    spans, count);
	}
	read_range(fs, inode, 16U * fs->info.block_size + 17U, spans, count);
	read_range(fs, inode, limit / 3U, spans, count);
	read_range(
	    fs, inode, limit > fs->info.block_size ? limit - fs->info.block_size : 0, spans, count);
	read_range(fs, inode, limit, spans, count);
	read_range(fs, inode, UINT64_MAX, spans, count);
}

static void
write_span(struct ext4_fs *fs, struct ext4_inode *inode, const struct span *span)
{
	struct ext4_inode_update update = attributes(false);
	uint8_t bytes[64];
	size_t completed;

	CHECK(span->length <= sizeof(bytes));
	memset(bytes, span->byte, span->length);
	EXPECT(ext4_write(fs, inode->number, inode->generation, span->offset, bytes, span->length,
		   &update, &completed),
	    EXT4_OK);
	CHECK(completed == span->length);
	EXPECT(ext4_get_inode(fs, inode->number, inode), EXT4_OK);
}

static void
reject_growth(struct device *device, struct ext4_fs *fs, struct ext4_inode *inode, uint64_t limit)
{
	struct ext4_inode_update update = attributes(false);
	struct ext4_inode result;
	struct ext4_inode unchanged;
	uint8_t bytes[7] = { 1, 2, 3, 4, 5, 6, 7 };
	uint32_t writes = device->writes;
	uint64_t progress;
	size_t completed;

	memset(&result, 0xa5, sizeof(result));
	memcpy(&unchanged, &result, sizeof(result));
	EXPECT(
	    ext4_write(fs, inode->number, inode->generation, limit, bytes, 1, &update, &completed),
	    EXT4_RANGE);
	CHECK(completed == 0);
	EXPECT(ext4_write(fs, inode->number, inode->generation, UINT64_MAX - 2U, bytes,
		   sizeof(bytes), &update, &completed),
	    EXT4_RANGE);
	CHECK(completed == 0);
	EXPECT(ext4_truncate(fs, inode->number, inode->generation, limit + 1U, &update, &result),
	    EXT4_RANGE);
	CHECK(memcmp(&result, &unchanged, sizeof(result)) == 0);
	EXPECT(ext4_fallocate(fs, inode->number, inode->generation, limit, 1, EXT4_FALLOC_KEEP_SIZE,
		   &update, &progress),
	    EXT4_RANGE);
	CHECK(progress == 0 && device->writes == writes);
}

static struct ext4_fs *
checkpoint(struct device *device, struct ext4_fs *fs, const char *exports, const char *path,
    const char *label)
{
	EXPECT(ext4_sync(fs), EXT4_OK);
	ext4_unmount(fs);
	CHECK(device->live == 0);
	storage_export(device, exports, path, label);
	EXPECT(ext4_mount_writable(&device->environment, &device->writer, &fs), EXT4_OK);
	return fs;
}

static void
mutations(struct device *device, const char *exports, const char *path)
{
	struct ext4_fs *fs;
	struct ext4_inode seed;
	struct ext4_inode inode;
	struct ext4_inode root;
	struct ext4_inode_update create = attributes(true);
	struct ext4_inode_update update = attributes(false);
	struct ext4_timestamp time = { LARGE_FILE_SECONDS, 0 };
	struct span original[SPAN_CAPACITY] = { 0 };
	struct span spans[SPAN_CAPACITY];
	uint64_t limit;
	uint64_t free_blocks;
	uint64_t progress;
	uint64_t shortened = UNSIGNED_BYTE_BOUNDARY + 1U;
	size_t count;
	size_t current;
	size_t index;
	uint32_t block = device->block_size;

	EXPECT(ext4_mount_writable(&device->environment, &device->writer, &fs), EXT4_OK);
	seed = lookup(fs, "seed");
	limit = seed.size;
	CHECK(limit > shortened && limit % block == 0);
	count = seed_spans(original, block, limit);
	memcpy(spans, original, sizeof(spans));
	verify(fs, &seed, limit, original, count);
	EXPECT(ext4_get_inode(fs, EXT4_ROOT_INODE, &root), EXT4_OK);
	EXPECT(ext4_create(fs, root.number, root.generation, (const uint8_t *)"created", 7, &create,
		   &time, &inode),
	    EXT4_OK);
	free_blocks = fs->info.free_blocks;
	spans[0] = (struct span){ 0, 61, 0x71 };
	write_span(fs, &inode, &spans[0]);
	if (fs->info.feature_incompat & EXT4_FEATURE_INCOMPAT_INLINE_DATA) {
		CHECK(inode.flags & EXT4_INODE_INLINE_DATA);
	}
	/* The first expansion goes straight to the format ceiling. In particular,
	 * inline conversion must use the destination extent address space. */
	write_span(fs, &inode, &spans[count - 1U]);
	for (index = 1; index + 1 < count; index++) {
		write_span(fs, &inode, &spans[index]);
	}
	verify(fs, &inode, limit, spans, count);
	reject_growth(device, fs, &inode, limit);
	fs = checkpoint(device, fs, exports, path, "large-written-");
	inode = lookup(fs, "created");
	verify(fs, &inode, limit, spans, count);
	current = count;
	if (inode.flags & EXT4_INODE_EXTENTS) {
		EXPECT(ext4_fallocate(fs, inode.number, inode.generation, limit - 4U * block,
			   2U * block, EXT4_FALLOC_KEEP_SIZE, &update, &progress),
		    EXT4_OK);
		CHECK(progress == 2U * block);
		spans[current++] = (struct span){ limit - 3U * block + 9U, 1, 'R' };
		write_span(fs, &inode, &spans[current - 1U]);
	}
	EXPECT(ext4_fallocate(fs, inode.number, inode.generation, spans[1].offset, spans[1].length,
		   EXT4_FALLOC_KEEP_SIZE | EXT4_FALLOC_PUNCH_HOLE, &update, &progress),
	    EXT4_OK);
	CHECK(progress == spans[1].length);
	spans[1].byte = 0;
	EXPECT(ext4_get_inode(fs, inode.number, &inode), EXT4_OK);
	verify(fs, &inode, limit, spans, current);
	fs = checkpoint(device, fs, exports, path, "large-ranged-");
	inode = lookup(fs, "created");
	EXPECT(
	    ext4_truncate(fs, inode.number, inode.generation, shortened, &update, &inode), EXT4_OK);
	for (index = 0; index < current; index++) {
		if (spans[index].offset >= shortened) {
			spans[index].byte = 0;
		} else if (spans[index].length > shortened - spans[index].offset) {
			spans[index].length = (size_t)(shortened - spans[index].offset);
		}
	}
	verify(fs, &inode, shortened, spans, current);
	EXPECT(ext4_truncate(fs, inode.number, inode.generation, limit, &update, &inode), EXT4_OK);
	verify(fs, &inode, limit, spans, current);
	fs = checkpoint(device, fs, exports, path, "large-regrown-");
	inode = lookup(fs, "created");
	verify(fs, &inode, limit, spans, current);
	EXPECT(ext4_truncate(fs, inode.number, inode.generation, 0, &update, &inode), EXT4_OK);
	CHECK(inode.size == 0 && inode.blocks_512 == 0 && fs->info.free_blocks == free_blocks);
	fs = checkpoint(device, fs, exports, path, "large-reclaimed-");
	inode = lookup(fs, "created");
	CHECK(inode.size == 0 && inode.blocks_512 == 0 && fs->info.free_blocks == free_blocks);
	seed = lookup(fs, "seed");
	verify(fs, &seed, limit, original, count);
	EXPECT(ext4_sync(fs), EXT4_OK);
	ext4_unmount(fs);
	CHECK(device->live == 0);
	printf(
	    "PASS large-file reads, mutation and reclamation: limit=%" PRIu64 " %s\n", limit, path);
}

int
main(int argc, char **argv)
{
	struct device device;
	const char *exports = NULL;
	int argument = 1;

	CHECK(argc > 1);
	if (strcmp(argv[argument], "--export") == 0) {
		CHECK(argc >= 4);
		exports = argv[++argument];
		argument++;
	}
	for (; argument < argc; argument++) {
		storage_open(&device, argv[argument]);
		mutations(&device, exports, argv[argument]);
		storage_close(&device);
	}
	return 0;
}

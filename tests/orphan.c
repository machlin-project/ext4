/* SPDX-License-Identifier: BSD-3-Clause */
#include "allocate.h"
#include "image.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TEST_UNKNOWN_INODE_FLAG 0x80000000U
#define TEST_IMAGE_LIMIT (128U * 1024U * 1024U)
#define TEST_LINKED_ORPHANS 2U
#define TEST_PAYLOAD_SIZE 200000U

enum orphan_format { ORPHAN_LEGACY, ORPHAN_FILE, ORPHAN_MIXED };

#define CHECK(expression)                                                                          \
	do {                                                                                       \
		if (!(expression)) {                                                               \
			fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #expression);           \
			exit(1);                                                                   \
		}                                                                                  \
	} while (0)
#define EXPECT(expression, expected)                                                               \
	do {                                                                                       \
		enum ext4_result actual = (expression);                                            \
		if (actual != (expected)) {                                                        \
			fprintf(stderr, "%s:%d: %s: %s, expected %s\n", __FILE__, __LINE__,        \
			    #expression, ext4_result_string(actual),                               \
			    ext4_result_string(expected));                                         \
			exit(1);                                                                   \
		}                                                                                  \
	} while (0)

struct device {
	struct ext4_environment environment;
	struct ext4_write_environment writer;
	uint8_t *base;
	uint8_t *cache;
	uint8_t *stable;
	uint8_t *dirty;
	uint8_t *journal_blocks;
	size_t size;
	uint32_t block_size;
	uint32_t blocks;
	uint32_t events;
	uint32_t stop_at;
	uint32_t writes;
	uint32_t reads;
	uint32_t allocations;
	uint32_t fail_read;
	uint32_t fail_allocation;
	uint32_t live;
	unsigned int survival;
	bool partial;
	bool off;
	bool metadata_checksum;
	bool commit_written;
	bool intent_durable;
};

static void *
device_allocate(void *context, size_t size)
{
	struct device *device = context;
	void *buffer;

	if (++device->allocations == device->fail_allocation) {
		return NULL;
	}
	buffer = malloc(size);
	if (buffer != NULL) {
		device->live++;
	}
	return buffer;
}

static void
device_release(void *context, void *buffer, size_t size)
{
	struct device *device = context;

	(void)size;
	CHECK(buffer != NULL && device->live != 0);
	device->live--;
	free(buffer);
}

static enum ext4_result
device_read(void *context, uint64_t offset, void *buffer, size_t length)
{
	struct device *device = context;

	CHECK(offset <= device->size && length <= device->size - offset);
	if (++device->reads == device->fail_read || device->off) {
		return EXT4_IO;
	}
	memcpy(buffer, device->cache + offset, length);
	return EXT4_OK;
}

static void
device_persist(struct device *device, unsigned int survival)
{
	uint32_t index;

	for (index = 0; index < device->blocks; index++) {
		if (device->dirty[index] && (survival == 1 || (survival == 2 && (index & 1)))) {
			memcpy(device->stable + (size_t)index * device->block_size,
			    device->cache + (size_t)index * device->block_size, device->block_size);
		}
		device->dirty[index] = 0;
	}
	if (survival == 1 && device->commit_written) {
		device->intent_durable = true;
	}
}

static enum ext4_result
device_write(void *context, uint64_t offset, const void *buffer, size_t length)
{
	struct device *device = context;
	size_t partial;

	CHECK(!device->off && offset % device->block_size == 0 && length == device->block_size);
	CHECK(offset <= device->size && length <= device->size - offset);
	device->writes++;
	if (++device->events == device->stop_at) {
		if (device->partial) {
			partial = length / 2;
			if (offset ==
			    (EXT4_SUPER_OFFSET / device->block_size) * device->block_size) {
				partial = EXT4_SUPER_OFFSET % device->block_size + EXT4_SECTOR_SIZE;
			}
			memcpy(device->cache + offset, buffer, partial);
			device->dirty[offset / device->block_size] = 1;
		}
		device_persist(device, device->survival);
		device->off = true;
		return EXT4_IO;
	}
	memcpy(device->cache + offset, buffer, length);
	device->dirty[offset / device->block_size] = 1;
	if (device->journal_blocks[offset / device->block_size] &&
	    ext4_be32(&((const struct ext4_jbd_header *)buffer)->magic) == EXT4_JBD_MAGIC &&
	    ext4_be32(&((const struct ext4_jbd_header *)buffer)->type) == EXT4_JBD_COMMIT) {
		device->commit_written = true;
	}
	return EXT4_OK;
}

static enum ext4_result
device_flush(void *context)
{
	struct device *device = context;
	bool stop;

	CHECK(!device->off);
	stop = ++device->events == device->stop_at;
	device_persist(device, stop ? device->survival : 1);
	if (stop) {
		device->off = true;
		return EXT4_IO;
	}
	return EXT4_OK;
}

static void
device_reset(struct device *device, const uint8_t *source)
{
	CHECK(device->live == 0);
	memcpy(device->cache, source, device->size);
	if (source != device->stable) {
		memcpy(device->stable, source, device->size);
	}
	memset(device->dirty, 0, device->blocks);
	device->events = device->stop_at = device->writes = device->reads = 0;
	device->allocations = device->fail_read = device->fail_allocation = 0;
	device->off = false;
	device->partial = false;
	device->commit_written = false;
	device->intent_durable = false;
}

static void
super_checksum(struct device *device)
{
	struct ext4_super_disk *super =
	    (struct ext4_super_disk *)(device->cache + EXT4_SUPER_OFFSET);

	if (device->metadata_checksum) {
		ext4_encode32(&super->checksum,
		    ext4_crc32c(UINT32_MAX, super, offsetof(struct ext4_super_disk, checksum)));
	}
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

static void
orphan_block_checksum(
    struct device *device, struct ext4_fs *fs, const struct ext4_inode *file, uint64_t block)
{
	struct ext4_orphan_tail_disk *tail;
	struct ext4_block_number_disk address;
	uint8_t *buffer = device->cache + block * device->block_size;
	uint32_t checksum;

	if (!device->metadata_checksum) {
		return;
	}
	tail = (struct ext4_orphan_tail_disk *)(buffer + device->block_size - sizeof(*tail));
	ext4_encode32(&address.low, (uint32_t)block);
	ext4_encode32(&address.high, (uint32_t)(block >> 32));
	checksum = ext4_crc32c(ext4_inode_seed(fs, file), &address, sizeof(address));
	ext4_encode32(
	    &tail->checksum, ext4_crc32c(checksum, buffer, device->block_size - sizeof(*tail)));
}

static void
linked_fixture(struct device *device, enum orphan_format format)
{
	struct ext4_fs *fs;
	struct ext4_inode payload;
	struct ext4_inode hello;
	struct ext4_inode file;
	struct ext4_inode_disk *disk;
	struct ext4_super_disk *super =
	    (struct ext4_super_disk *)(device->cache + EXT4_SUPER_OFFSET);
	uint64_t offset;
	uint64_t block;
	uint32_t slots =
	    (device->block_size - sizeof(struct ext4_orphan_tail_disk)) / sizeof(struct ext4_le32);
	struct ext4_le32 *entries;

	EXPECT(ext4_mount(&device->environment, &fs), EXT4_OK);
	payload = lookup(fs, "payload.bin");
	hello = lookup(fs, "hello.txt");
	EXPECT(ext4_inode_location(fs, payload.number, &offset), EXT4_OK);
	disk = (struct ext4_inode_disk *)(device->cache + offset);
	ext4_encode32(&disk->size_lo, device->block_size + 7);
	ext4_encode32(&disk->deletion_time, format == ORPHAN_LEGACY ? hello.number : 0);
	ext4_inode_checksum_set(fs, payload.number, disk);
	EXPECT(ext4_inode_location(fs, hello.number, &offset), EXT4_OK);
	disk = (struct ext4_inode_disk *)(device->cache + offset);
	ext4_encode32(&disk->size_lo, 7);
	ext4_encode32(&disk->deletion_time, 0);
	ext4_inode_checksum_set(fs, hello.number, disk);
	ext4_encode32(&super->last_orphan, format == ORPHAN_FILE ? 0 : payload.number);
	if (format != ORPHAN_LEGACY) {
		CHECK(fs->orphan_file_inode != 0);
		EXPECT(ext4_get_inode(fs, fs->orphan_file_inode, &file), EXT4_OK);
		CHECK(file.size / device->block_size >= 2);
		if (format == ORPHAN_FILE) {
			EXPECT(ext4_map_block(fs, &file, 0, &block), EXT4_OK);
			entries = (struct ext4_le32 *)(device->cache + block * device->block_size);
			ext4_encode32(&entries[0], payload.number);
			orphan_block_checksum(device, fs, &file, block);
		}
		EXPECT(ext4_map_block(
			   fs, &file, (uint32_t)(file.size / device->block_size - 1), &block),
		    EXT4_OK);
		entries = (struct ext4_le32 *)(device->cache + block * device->block_size);
		ext4_encode32(&entries[slots - 1], hello.number);
		orphan_block_checksum(device, fs, &file, block);
	}
	if (fs->orphan_file_inode != 0) {
		ext4_encode32(&super->feature_ro_compat,
		    ext4_le32(&super->feature_ro_compat) | EXT4_FEATURE_RO_ORPHAN_PRESENT);
	}
	ext4_encode32(&super->feature_incompat,
	    ext4_le32(&super->feature_incompat) | EXT4_FEATURE_INCOMPAT_RECOVER);
	super_checksum(device);
	ext4_unmount(fs);
	memcpy(device->base, device->cache, device->size);
	device_reset(device, device->base);
}

static void
check_linked(struct device *device)
{
	struct ext4_fs *fs;
	struct ext4_inode payload;
	struct ext4_inode hello;
	struct ext4_inode alias;
	uint8_t *bytes = malloc(device->block_size + 7);
	uint64_t physical;
	size_t completed;
	size_t index;

	CHECK(bytes != NULL);
	EXPECT(ext4_mount(&device->environment, &fs), EXT4_OK);
	payload = lookup(fs, "payload.bin");
	hello = lookup(fs, "hello.txt");
	alias = lookup(fs, "hello-hardlink");
	CHECK(payload.size == device->block_size + 7 && payload.links == 1);
	CHECK(payload.blocks_512 == 2U * device->block_size / EXT4_SECTOR_SIZE);
	CHECK(
	    hello.size == 7 && hello.links == 2 && alias.number == hello.number && alias.size == 7);
	EXPECT(ext4_read(fs, &payload, 0, bytes, device->block_size + 7, &completed), EXT4_OK);
	CHECK(completed == device->block_size + 7);
	for (index = 0; index < completed; index++) {
		CHECK(bytes[index] == (uint8_t)(index * 17 + 23));
	}
	EXPECT(ext4_read(fs, &hello, 0, bytes, 7, &completed), EXT4_OK);
	CHECK(completed == 7 && memcmp(bytes, "Machlin", 7) == 0);
	EXPECT(ext4_map_block(fs, &payload, 1, &physical), EXT4_OK);
	for (index = 7; index < device->block_size; index++) {
		CHECK(device->cache[physical * device->block_size + index] == 0);
	}
	EXPECT(ext4_map_block(fs, &hello, 0, &physical), EXT4_OK);
	for (index = 7; index < device->block_size; index++) {
		CHECK(device->cache[physical * device->block_size + index] == 0);
	}
	ext4_unmount(fs);
	free(bytes);
}

static void
check_equal(struct device *device, const uint8_t *expected)
{
	uint32_t index;

	CHECK(device->live == 0 && memcmp(device->cache, device->stable, device->size) == 0);
	for (index = 0; index < device->blocks; index++) {
		if (!device->journal_blocks[index]) {
			CHECK(memcmp(device->cache + (size_t)index * device->block_size,
				  expected + (size_t)index * device->block_size,
				  device->block_size) == 0);
		}
	}
}

enum malformed_kind {
	HEAD_OUTSIDE_INODES,
	HEAD_ROOT,
	HEAD_JOURNAL,
	HEAD_FREE_INODE,
	NEXT_OUTSIDE_INODES,
	SELF_CYCLE,
	PAIR_CYCLE,
	TAIL_CYCLE,
	ZERO_MODE,
	LINKED_DIRECTORY,
	UNKNOWN_FLAGS_ORPHAN,
	INLINE_ORPHAN,
	EXTERNAL_XATTR,
	WRONG_BLOCK_COUNT,
	PROTECTED_DATA,
	BAD_INODE_CHECKSUM,
	MALFORMED_COUNT
};

static void
malformed_cases(struct device *device)
{
	struct ext4_fs *fs;
	struct ext4_inode payload;
	struct ext4_inode hello;
	struct ext4_inode_disk *disk;
	struct ext4_inode_disk *tail;
	struct ext4_super_disk *super;
	struct ext4_extent_header_disk *header;
	struct ext4_extent_disk *entry;
	struct ext4_recovery_report report;
	uint8_t *malformed = malloc(device->size);
	uint64_t offset;
	uint32_t block;
	unsigned int kind;
	unsigned int checked = 0;
	unsigned int skipped = 0;
	enum ext4_result expected;

	CHECK(malformed != NULL);
	for (kind = 0; kind < MALFORMED_COUNT; kind++) {
		if (kind == BAD_INODE_CHECKSUM && !device->metadata_checksum) {
			skipped++;
			continue;
		}
		device_reset(device, device->base);
		EXPECT(ext4_load(&device->environment, true, &fs), EXT4_OK);
		payload = lookup(fs, "payload.bin");
		hello = lookup(fs, "hello.txt");
		EXPECT(ext4_inode_location(fs, payload.number, &offset), EXT4_OK);
		disk = (struct ext4_inode_disk *)(device->cache + offset);
		EXPECT(ext4_inode_location(fs, hello.number, &offset), EXT4_OK);
		tail = (struct ext4_inode_disk *)(device->cache + offset);
		super = (struct ext4_super_disk *)(device->cache + EXT4_SUPER_OFFSET);
		expected = EXT4_CORRUPT;
		switch (kind) {
		case HEAD_OUTSIDE_INODES:
			ext4_encode32(&super->last_orphan, fs->info.inodes + 1);
			break;
		case HEAD_ROOT:
			ext4_encode32(&super->last_orphan, EXT4_ROOT_INODE);
			break;
		case HEAD_JOURNAL:
			ext4_encode32(&super->last_orphan, fs->journal_inode);
			break;
		case HEAD_FREE_INODE:
			ext4_encode32(&super->last_orphan, fs->info.inodes);
			break;
		case NEXT_OUTSIDE_INODES:
			ext4_encode32(&disk->deletion_time, fs->info.inodes + 1);
			break;
		case SELF_CYCLE:
			ext4_encode32(&disk->deletion_time, payload.number);
			break;
		case PAIR_CYCLE:
			ext4_encode32(&tail->deletion_time, payload.number);
			break;
		case TAIL_CYCLE:
			ext4_encode32(&tail->deletion_time, hello.number);
			break;
		case ZERO_MODE:
			ext4_encode16(&disk->mode, 0);
			break;
		case LINKED_DIRECTORY:
			ext4_encode16(&disk->mode, EXT4_MODE_DIRECTORY | 0700);
			expected = EXT4_UNSUPPORTED;
			break;
		case UNKNOWN_FLAGS_ORPHAN:
			ext4_encode32(&disk->flags, payload.flags | TEST_UNKNOWN_INODE_FLAG);
			expected = EXT4_UNSUPPORTED;
			break;
		case INLINE_ORPHAN:
			/* The feature is absent and the original block map is still set. */
			ext4_encode32(&disk->flags, payload.flags | EXT4_INODE_INLINE_DATA);
			expected = EXT4_CORRUPT;
			break;
		case EXTERNAL_XATTR:
			ext4_encode32(&disk->xattr_block_lo, fs->first_data_block + 1);
			break;
		case WRONG_BLOCK_COUNT:
			ext4_encode32(&disk->blocks_lo, ext4_le32(&disk->blocks_lo) + 1);
			break;
		case PROTECTED_DATA:
			for (block = 0; block < device->blocks && !device->journal_blocks[block];
			    block++) {
			}
			CHECK(block < device->blocks);
			if (payload.flags & EXT4_INODE_EXTENTS) {
				header = (struct ext4_extent_header_disk *)disk->block_data;
				CHECK(ext4_le16(&header->depth) == 0 &&
				    ext4_le16(&header->entries) != 0);
				entry = (struct ext4_extent_disk *)(header + 1);
				ext4_encode32(&entry->physical_lo, block);
				ext4_encode16(&entry->physical_hi, 0);
			} else {
				ext4_encode32((struct ext4_le32 *)disk->block_data, block);
			}
			break;
		case BAD_INODE_CHECKSUM:
			disk->uid_lo.bytes[0] ^= 1;
			break;
		default:
			CHECK(false);
		}
		if (kind != BAD_INODE_CHECKSUM) {
			ext4_inode_checksum_set(fs, payload.number, disk);
		}
		ext4_inode_checksum_set(fs, hello.number, tail);
		super_checksum(device);
		ext4_unmount(fs);
		memcpy(malformed, device->cache, device->size);
		device_reset(device, malformed);
		EXPECT(ext4_recover(&device->environment, &device->writer, &report), expected);
		CHECK(report.cleaned_orphans == 0 && report.orphan_transactions == 0);
		check_equal(device, malformed);
		checked++;
	}
	free(malformed);
	printf("PASS malformed orphan cases=%u; SKIP checksum-absent cases=%u\n", checked, skipped);
}

enum orphan_file_malformed {
	FILE_PRESENT_WITHOUT_FEATURE,
	FILE_ENTRIES_WITHOUT_PRESENT,
	FILE_INODE_ZERO,
	FILE_INODE_ZERO_RESERVED_BASE,
	FILE_INODE_ROOT,
	FILE_INODE_JOURNAL,
	FILE_INODE_OUTSIDE,
	FILE_INODE_FREE,
	FILE_SIZE_ZERO,
	FILE_SIZE_UNALIGNED,
	FILE_SIZE_LIMIT,
	FILE_LINK_COUNT,
	FILE_DELETION_TIME,
	FILE_WRONG_TYPE,
	FILE_IMMUTABLE,
	FILE_WRONG_BLOCK_COUNT,
	FILE_MAP_HOLE,
	FILE_MAP_ALIAS,
	FILE_MAP_PROTECTED,
	FILE_FIRST_MAGIC,
	FILE_LAST_MAGIC,
	FILE_FIRST_CHECKSUM,
	FILE_LAST_CHECKSUM,
	FILE_ADDRESS_CHECKSUM,
	FILE_GENERATION_CHECKSUM,
	FILE_ENTRY_OUTSIDE,
	FILE_ENTRY_ROOT,
	FILE_ENTRY_JOURNAL,
	FILE_ENTRY_SELF,
	FILE_ENTRY_FREE,
	FILE_ENTRY_DUPLICATE,
	FILE_ENTRY_CROSS_BLOCK_DUPLICATE,
	FILE_ENTRY_LEGACY_DUPLICATE,
	FILE_ENTRY_DELETION_TIME,
	FILE_LAST_ENTRY_TYPE,
	FILE_MALFORMED_COUNT
};

static void
orphan_file_malformed_cases(struct device *device)
{
	struct ext4_fs *fs;
	struct ext4_inode file;
	struct ext4_inode payload;
	struct ext4_inode hello;
	struct ext4_inode_disk *disk;
	struct ext4_inode_disk *payload_disk;
	struct ext4_inode_disk *hello_disk;
	struct ext4_super_disk *super;
	struct ext4_extent_header_disk *header;
	struct ext4_extent_disk *extent;
	struct ext4_le32 *first;
	struct ext4_le32 *last;
	struct ext4_orphan_tail_disk *first_tail;
	struct ext4_orphan_tail_disk *last_tail;
	struct ext4_recovery_report report;
	uint8_t *malformed = malloc(device->size);
	uint64_t offset;
	uint64_t first_block;
	uint64_t last_block;
	uint32_t slots =
	    (device->block_size - sizeof(struct ext4_orphan_tail_disk)) / sizeof(struct ext4_le32);
	unsigned int kind;
	unsigned int checked = 0;
	unsigned int skipped = 0;
	enum ext4_result expected;

	CHECK(malformed != NULL);
	for (kind = 0; kind < FILE_MALFORMED_COUNT; kind++) {
		if (kind >= FILE_FIRST_CHECKSUM && kind <= FILE_GENERATION_CHECKSUM &&
		    !device->metadata_checksum) {
			skipped++;
			continue;
		}
		device_reset(device, device->base);
		EXPECT(ext4_load(&device->environment, true, &fs), EXT4_OK);
		EXPECT(ext4_get_inode(fs, fs->orphan_file_inode, &file), EXT4_OK);
		payload = lookup(fs, "payload.bin");
		hello = lookup(fs, "hello.txt");
		EXPECT(ext4_inode_location(fs, file.number, &offset), EXT4_OK);
		disk = (struct ext4_inode_disk *)(device->cache + offset);
		EXPECT(ext4_inode_location(fs, payload.number, &offset), EXT4_OK);
		payload_disk = (struct ext4_inode_disk *)(device->cache + offset);
		EXPECT(ext4_inode_location(fs, hello.number, &offset), EXT4_OK);
		hello_disk = (struct ext4_inode_disk *)(device->cache + offset);
		EXPECT(ext4_map_block(fs, &file, 0, &first_block), EXT4_OK);
		EXPECT(ext4_map_block(
			   fs, &file, (uint32_t)(file.size / device->block_size - 1), &last_block),
		    EXT4_OK);
		first = (struct ext4_le32 *)(device->cache + first_block * device->block_size);
		last = (struct ext4_le32 *)(device->cache + last_block * device->block_size);
		first_tail = (struct ext4_orphan_tail_disk *)(first + slots);
		last_tail = (struct ext4_orphan_tail_disk *)(last + slots);
		super = (struct ext4_super_disk *)(device->cache + EXT4_SUPER_OFFSET);
		expected = EXT4_CORRUPT;
		switch (kind) {
		case FILE_PRESENT_WITHOUT_FEATURE:
			ext4_encode32(&super->feature_compat,
			    ext4_le32(&super->feature_compat) & ~EXT4_FEATURE_COMPAT_ORPHAN_FILE);
			break;
		case FILE_ENTRIES_WITHOUT_PRESENT:
			ext4_encode32(&super->feature_ro_compat,
			    ext4_le32(&super->feature_ro_compat) & ~EXT4_FEATURE_RO_ORPHAN_PRESENT);
			break;
		case FILE_INODE_ZERO:
			ext4_encode32(&super->orphan_file_inode, 0);
			break;
		case FILE_INODE_ZERO_RESERVED_BASE:
			ext4_encode32(&super->orphan_file_inode, 0);
			ext4_encode32(&super->first_inode, 0);
			break;
		case FILE_INODE_ROOT:
			ext4_encode32(&super->orphan_file_inode, EXT4_ROOT_INODE);
			break;
		case FILE_INODE_JOURNAL:
			ext4_encode32(&super->orphan_file_inode, fs->journal_inode);
			break;
		case FILE_INODE_OUTSIDE:
			ext4_encode32(&super->orphan_file_inode, fs->info.inodes + 1);
			break;
		case FILE_INODE_FREE:
			ext4_encode32(&super->orphan_file_inode, fs->info.inodes);
			break;
		case FILE_SIZE_ZERO:
			ext4_encode32(&disk->size_lo, 0);
			break;
		case FILE_SIZE_UNALIGNED:
			ext4_encode32(&disk->size_lo, (uint32_t)file.size - 1);
			break;
		case FILE_SIZE_LIMIT:
			ext4_encode32(&disk->size_lo,
			    (EXT4_ORPHAN_FILE_MAX_BLOCKS + 1U) * device->block_size);
			expected = EXT4_UNSUPPORTED;
			break;
		case FILE_LINK_COUNT:
			ext4_encode16(&disk->links, 2);
			break;
		case FILE_DELETION_TIME:
			ext4_encode32(&disk->deletion_time, payload.number);
			break;
		case FILE_WRONG_TYPE:
			ext4_encode16(&disk->mode, EXT4_MODE_DIRECTORY | 0700);
			break;
		case FILE_IMMUTABLE:
			ext4_encode32(&disk->flags, file.flags | EXT4_INODE_IMMUTABLE);
			expected = EXT4_UNSUPPORTED;
			break;
		case FILE_WRONG_BLOCK_COUNT:
			ext4_encode32(&disk->blocks_lo, ext4_le32(&disk->blocks_lo) + 1);
			break;
		case FILE_MAP_HOLE:
		case FILE_MAP_ALIAS:
		case FILE_MAP_PROTECTED:
			if (file.flags & EXT4_INODE_EXTENTS) {
				header = (struct ext4_extent_header_disk *)disk->block_data;
				CHECK(ext4_le16(&header->depth) == 0 &&
				    file.size == 4U * device->block_size);
				extent = (struct ext4_extent_disk *)(header + 1);
				if (kind == FILE_MAP_HOLE) {
					ext4_encode16(&header->entries, 0);
				} else if (kind == FILE_MAP_ALIAS) {
					ext4_encode16(&header->entries, 2);
					ext4_encode32(&extent[0].logical, 0);
					ext4_encode16(&extent[0].length, 2);
					ext4_encode16(&extent[0].physical_hi, 0);
					ext4_encode32(
					    &extent[0].physical_lo, (uint32_t)first_block);
					extent[1] = extent[0];
					ext4_encode32(&extent[1].logical, 2);
				} else {
					ext4_encode32(
					    &extent->physical_lo, fs->first_data_block + 1);
					ext4_encode16(&extent->physical_hi, 0);
				}
			} else {
				ext4_encode32((struct ext4_le32 *)disk->block_data,
				    kind == FILE_MAP_HOLE ? 0
					: kind == FILE_MAP_ALIAS
					? ext4_le32((struct ext4_le32 *)disk->block_data + 1)
					: fs->first_data_block + 1);
			}
			break;
		case FILE_FIRST_MAGIC:
			ext4_encode32(&first_tail->magic, 0);
			break;
		case FILE_LAST_MAGIC:
			ext4_encode32(&last_tail->magic, 0);
			break;
		case FILE_FIRST_CHECKSUM:
			first_tail->checksum.bytes[0] ^= 1;
			break;
		case FILE_LAST_CHECKSUM:
			last_tail->checksum.bytes[0] ^= 1;
			break;
		case FILE_ADDRESS_CHECKSUM:
			memcpy(last, first, device->block_size);
			break;
		case FILE_GENERATION_CHECKSUM:
			ext4_encode32(&disk->generation, file.generation + 1);
			break;
		case FILE_ENTRY_OUTSIDE:
			ext4_encode32(&first[0], fs->info.inodes + 1);
			break;
		case FILE_ENTRY_ROOT:
			ext4_encode32(&first[0], EXT4_ROOT_INODE);
			break;
		case FILE_ENTRY_JOURNAL:
			ext4_encode32(&first[0], fs->journal_inode);
			break;
		case FILE_ENTRY_SELF:
			ext4_encode32(&first[0], file.number);
			break;
		case FILE_ENTRY_FREE:
			ext4_encode32(&first[0], fs->info.inodes);
			break;
		case FILE_ENTRY_DUPLICATE:
			ext4_encode32(&first[1], payload.number);
			break;
		case FILE_ENTRY_CROSS_BLOCK_DUPLICATE:
			ext4_encode32(&last[0], payload.number);
			break;
		case FILE_ENTRY_LEGACY_DUPLICATE:
			ext4_encode32(&super->last_orphan, payload.number);
			break;
		case FILE_ENTRY_DELETION_TIME:
			ext4_encode32(&payload_disk->deletion_time, hello.number);
			break;
		case FILE_LAST_ENTRY_TYPE:
			ext4_encode16(&hello_disk->mode, EXT4_MODE_DIRECTORY | 0700);
			expected = EXT4_UNSUPPORTED;
			break;
		default:
			CHECK(false);
		}
		if (kind < FILE_FIRST_CHECKSUM || kind > FILE_GENERATION_CHECKSUM) {
			orphan_block_checksum(device, fs, &file, first_block);
			orphan_block_checksum(device, fs, &file, last_block);
		}
		ext4_inode_checksum_set(fs, file.number, disk);
		ext4_inode_checksum_set(fs, payload.number, payload_disk);
		ext4_inode_checksum_set(fs, hello.number, hello_disk);
		super_checksum(device);
		ext4_unmount(fs);
		memcpy(malformed, device->cache, device->size);
		device_reset(device, malformed);
		printf("CHECK orphan file malformed case=%u\n", kind);
		EXPECT(ext4_recover(&device->environment, &device->writer, &report), expected);
		CHECK(report.cleaned_orphans == 0 && report.orphan_transactions == 0);
		check_equal(device, malformed);
		checked++;
	}
	free(malformed);
	printf("PASS malformed orphan file cases=%u; SKIP checksum-absent cases=%u\n", checked,
	    skipped);
}

static void
recount_cases(struct device *device, const uint8_t *expected)
{
	struct ext4_fs *fs;
	struct ext4_super_disk *super;
	struct ext4_recovery_report report;
	uint32_t blocks;
	uint32_t inodes;
	unsigned int kind;
	uint8_t *pending = malloc(device->size);

	CHECK(pending != NULL);
	for (kind = 0; kind < 3; kind++) {
		device_reset(device, device->base);
		super = (struct ext4_super_disk *)(device->cache + EXT4_SUPER_OFFSET);
		blocks = ext4_le32(&super->free_blocks_lo);
		inodes = ext4_le32(&super->free_inodes);
		CHECK(blocks > 0 && inodes > 0);
		if (kind < 2) {
			ext4_encode32(&super->free_blocks_lo, kind == 0 ? blocks - 1 : blocks + 1);
			ext4_encode32(&super->free_inodes, kind == 0 ? inodes - 1 : inodes + 1);
		} else {
			ext4_encode32(&super->feature_incompat,
			    ext4_le32(&super->feature_incompat) & ~EXT4_FEATURE_INCOMPAT_RECOVER);
		}
		super_checksum(device);
		memcpy(pending, device->cache, device->size);
		device_reset(device, pending);
		if (kind == 2) {
			EXPECT(ext4_mount(&device->environment, &fs), EXT4_RECOVERY_REQUIRED);
			CHECK(fs == NULL && device->writes == 0);
		}
		EXPECT(ext4_recover(&device->environment, &device->writer, &report), EXT4_OK);
		CHECK(report.cleaned_orphans == TEST_LINKED_ORPHANS &&
		    report.accounting_updated == (kind < 2));
		check_equal(device, expected);
	}
	free(pending);
	puts("PASS stale accounting and orphan chain without recovery flag");
}

static void
resource_faults(struct device *device, const uint8_t *expected, uint32_t allocations,
    uint32_t reads, uint32_t mount_allocations, uint32_t mount_reads)
{
	struct ext4_recovery_report report;
	uint32_t kind;
	uint32_t index;
	uint32_t count;
	uint32_t repeated_end;
	uint32_t checked[2] = { 0, 0 };

	for (kind = 0; kind < 2; kind++) {
		count = kind == 0 ? allocations : reads;
		repeated_end = kind == 0 ? mount_allocations : mount_reads;
		for (index = 1; index <= count; index++) {
			/* The journal mapper repeats the same callback sequence for every
			 * journal block. Sample both ends of that phase, then inject at
			 * every callback in replay, recount and orphan cleanup. */
			if (index > 32 && index + 32 < repeated_end) {
				continue;
			}
			checked[kind]++;
			device_reset(device, device->base);
			if (kind == 0) {
				device->fail_allocation = index;
			} else {
				device->fail_read = index;
			}
			EXPECT(ext4_recover(&device->environment, &device->writer, &report),
			    kind == 0 ? EXT4_NO_MEMORY : EXT4_IO);
			CHECK(device->live == 0);
			device_reset(device, device->stable);
			EXPECT(
			    ext4_recover(&device->environment, &device->writer, &report), EXT4_OK);
			check_equal(device, expected);
		}
	}
	printf("PASS orphan resource faults: allocations=%u/%u reads=%u/%u (journal mapping "
	       "sampled)\n",
	    checked[0], allocations, checked[1], reads);
}

static void
crash_cases(struct device *device, const uint8_t *expected, uint32_t events)
{
	struct ext4_recovery_report report;
	struct ext4_super_disk *super;
	uint32_t event;
	uint32_t survival;
	uint32_t partial;
	uint32_t recovered = 0;
	uint32_t torn = 0;
	enum ext4_result error;

	for (event = 1; event <= events; event++) {
		for (survival = 0; survival < 3; survival++) {
			for (partial = 0; partial < 2; partial++) {
				device_reset(device, device->base);
				device->stop_at = event;
				device->survival = survival;
				device->partial = partial != 0;
				EXPECT(ext4_recover(&device->environment, &device->writer, &report),
				    EXT4_IO);
				CHECK(device->off && device->live == 0);
				device_reset(device, device->stable);
				error =
				    ext4_recover(&device->environment, &device->writer, &report);
				if (error == EXT4_CORRUPT) {
					super = (struct ext4_super_disk *)(device->cache +
					    EXT4_SUPER_OFFSET);
					CHECK(device->metadata_checksum && device->writes == 0 &&
					    ext4_le32(&super->checksum) !=
						ext4_crc32c(UINT32_MAX, super,
						    offsetof(struct ext4_super_disk, checksum)));
					torn++;
				} else {
					EXPECT(error, EXT4_OK);
					check_equal(device, expected);
					recovered++;
				}
				CHECK(device->live == 0);
			}
		}
	}
	printf("PASS orphan crash cuts: total=%u recovered=%u torn_super_fail_closed=%u\n",
	    events * 6, recovered, torn);
}

static void
export_image(struct device *device, const char *directory, const char *path, const char *prefix)
{
	char output[4096];
	const char *name = strrchr(path, '/');
	FILE *file;
	int length;

	if (directory == NULL) {
		return;
	}
	name = name == NULL ? path : name + 1;
	length = snprintf(output, sizeof(output), "%s/%s%s", directory, prefix, name);
	CHECK(length > 0 && (size_t)length < sizeof(output));
	file = fopen(output, "wbx");
	CHECK(file != NULL && fwrite(device->stable, 1, device->size, file) == device->size);
	CHECK(fclose(file) == 0);
}

static void
large_mapping(struct device *device, const uint8_t *clean, const char *exports, const char *path)
{
	struct ext4_fs *fs;
	struct ext4_inode inode;
	struct ext4_inode result;
	struct ext4_inode_update update;
	struct ext4_inode_disk *disk;
	struct ext4_super_disk *super;
	struct ext4_recovery_report report;
	uint64_t free_blocks;
	uint64_t logical;
	uint64_t offset;
	uint32_t per_block = device->block_size / sizeof(struct ext4_le32);
	uint32_t count = EXT4_TRANSACTION_MAX_BLOCKS + 1;
	uint32_t index;
	uint32_t writes;
	size_t completed;
	uint8_t marker;

	device_reset(device, clean);
	EXPECT(ext4_mount_writable(&device->environment, &device->writer, &fs), EXT4_OK);
	inode = lookup(fs, "empty");
	if (inode.flags & EXT4_INODE_EXTENTS) {
		ext4_unmount(fs);
		return;
	}
	CHECK(inode.size == 0 && inode.blocks_512 == 0);
	free_blocks = fs->info.free_blocks;
	memset(&update, 0, sizeof(update));
	update.fields = EXT4_ATTR_PERMISSIONS | EXT4_ATTR_MODIFY_TIME | EXT4_ATTR_CHANGE_TIME;
	update.permissions = 0600;
	update.modify_time.seconds = 1700000001;
	update.change_time.seconds = 1700000002;
	/* One data block per distinct indirect leaf forces more mapping nodes
	 * than an atomic transaction can enroll, without a large data fixture. */
	for (index = 0; index < count; index++) {
		logical = EXT4_DIRECT_BLOCKS + (uint64_t)per_block + (uint64_t)index * per_block;
		marker = (uint8_t)(index + 1);
		EXPECT(ext4_write(fs, inode.number, inode.generation,
			   logical * device->block_size + 3, &marker, 1, &update, &completed),
		    EXT4_OK);
		CHECK(completed == 1);
	}
	EXPECT(ext4_get_inode(fs, inode.number, &inode), EXT4_OK);
	CHECK(inode.blocks_512 / (device->block_size / EXT4_SECTOR_SIZE) > count * 2U);
	writes = device->writes;
	EXPECT(ext4_truncate_atomic(fs, inode.number, inode.generation, 0, &update, &result),
	    EXT4_RANGE);
	CHECK(device->writes == writes);
	EXPECT(ext4_sync(fs), EXT4_OK);
	EXPECT(ext4_inode_location(fs, inode.number, &offset), EXT4_OK);
	disk = (struct ext4_inode_disk *)(device->cache + offset);
	ext4_encode32(&disk->size_lo, 0);
	ext4_encode32(&disk->size_hi, 0);
	ext4_inode_checksum_set(fs, inode.number, disk);
	super = (struct ext4_super_disk *)(device->cache + EXT4_SUPER_OFFSET);
	ext4_encode32(&super->last_orphan, inode.number);
	ext4_encode32(&super->feature_incompat,
	    ext4_le32(&super->feature_incompat) | EXT4_FEATURE_INCOMPAT_RECOVER);
	super_checksum(device);
	ext4_unmount(fs);
	memcpy(device->stable, device->cache, device->size);
	device_reset(device, device->stable);
	export_image(device, exports, path, "large-pending-");
	EXPECT(ext4_recover(&device->environment, &device->writer, &report), EXT4_OK);
	CHECK(report.cleaned_orphans == 1 && report.orphan_transactions > 1);
	EXPECT(ext4_mount_writable(&device->environment, &device->writer, &fs), EXT4_OK);
	result = lookup(fs, "empty");
	CHECK(result.number == inode.number && result.generation == inode.generation &&
	    result.size == 0 && result.blocks_512 == 0 && fs->info.free_blocks == free_blocks);
	ext4_unmount(fs);
	export_image(device, exports, path, "large-clean-");
	printf("PASS large indirect orphan: %u sparse leaves, %u cleanup transactions\n", count,
	    report.orphan_transactions);
}

static void
live_prepare_large(struct device *device)
{
	struct ext4_fs *fs;
	struct ext4_inode inode;
	struct ext4_inode_update update;
	uint64_t logical;
	uint32_t per_block = device->block_size / sizeof(struct ext4_le32);
	uint32_t index;
	size_t completed;
	uint8_t marker;

	EXPECT(ext4_mount_writable(&device->environment, &device->writer, &fs), EXT4_OK);
	inode = lookup(fs, "empty");
	CHECK(!(inode.flags & EXT4_INODE_EXTENTS) && inode.size == 0);
	memset(&update, 0, sizeof(update));
	update.fields = EXT4_ATTR_PERMISSIONS | EXT4_ATTR_MODIFY_TIME | EXT4_ATTR_CHANGE_TIME;
	update.permissions = 06750;
	update.modify_time.seconds = 1700000001;
	update.change_time.seconds = 1700000002;
	for (index = 0; index <= EXT4_TRANSACTION_MAX_BLOCKS; index++) {
		logical = EXT4_DIRECT_BLOCKS + (uint64_t)per_block + (uint64_t)index * per_block;
		marker = (uint8_t)(index + 1);
		EXPECT(ext4_write(fs, inode.number, inode.generation,
			   logical * device->block_size + 3, &marker, 1, &update, &completed),
		    EXT4_OK);
		CHECK(completed == 1);
	}
	EXPECT(ext4_sync(fs), EXT4_OK);
	ext4_unmount(fs);
	memcpy(device->base, device->stable, device->size);
	device_reset(device, device->base);
}

struct live_trace {
	uint32_t allocations;
	uint32_t reads;
	uint32_t events;
	uint32_t transactions;
	bool intent_durable;
};

static enum ext4_result
live_attempt(struct device *device, const char *name, unsigned int fault, uint32_t position,
    unsigned int survival, bool partial, struct live_trace *trace)
{
	struct ext4_fs *fs;
	struct ext4_inode inode;
	struct ext4_inode result;
	struct ext4_inode sentinel;
	struct ext4_inode_update update;
	uint32_t sequence;
	enum ext4_result error;

	EXPECT(ext4_mount_writable(&device->environment, &device->writer, &fs), EXT4_OK);
	inode = lookup(fs, name);
	memset(&update, 0, sizeof(update));
	update.fields = EXT4_ATTR_PERMISSIONS | EXT4_ATTR_MODIFY_TIME | EXT4_ATTR_CHANGE_TIME;
	update.permissions = 0640;
	update.modify_time.seconds = 1700000301;
	update.change_time.seconds = 1700000302;
	memset(&result, 0xa5, sizeof(result));
	sentinel = result;
	device->allocations = device->reads = device->events = device->writes = 0;
	device->fail_allocation = fault == 1 ? position : 0;
	device->fail_read = fault == 2 ? position : 0;
	device->stop_at = fault == 3 ? position : 0;
	device->survival = survival;
	device->partial = partial;
	sequence = fs->journal->sequence;
	error = ext4_truncate(
	    fs, inode.number, inode.generation, device->block_size + 7, &update, &result);
	trace->allocations = device->allocations;
	trace->reads = device->reads;
	trace->events = device->events;
	trace->transactions = fs->journal->sequence - sequence;
	trace->intent_durable = device->intent_durable;
	device->fail_allocation = device->fail_read = device->stop_at = 0;
	if (error == EXT4_OK) {
		CHECK(result.number == inode.number && result.generation == inode.generation &&
		    result.size == device->block_size + 7 && result.links == inode.links &&
		    result.mode == (EXT4_MODE_REGULAR | update.permissions) &&
		    result.modify_time.seconds == update.modify_time.seconds &&
		    result.change_time.seconds == update.change_time.seconds && !fs->aborted &&
		    fs->last_orphan == 0);
		EXPECT(ext4_get_inode(fs, inode.number, &inode), EXT4_OK);
		CHECK(memcmp(&inode, &result, sizeof(inode)) == 0);
		EXPECT(ext4_sync(fs), EXT4_OK);
	} else {
		CHECK(memcmp(&result, &sentinel, sizeof(result)) == 0);
		if (fs->aborted) {
			EXPECT(ext4_get_inode(fs, inode.number, &result), EXT4_RECOVERY_REQUIRED);
			EXPECT(ext4_sync(fs), EXT4_RECOVERY_REQUIRED);
			EXPECT(
			    ext4_truncate(fs, inode.number, inode.generation, 0, &update, &result),
			    EXT4_RECOVERY_REQUIRED);
		} else {
			CHECK(device->writes == 0 &&
			    memcmp(device->cache, device->base, device->size) == 0);
			EXPECT(ext4_get_inode(fs, inode.number, &result), EXT4_OK);
		}
	}
	ext4_unmount(fs);
	CHECK(device->live == 0);
	return error;
}

static bool
live_equal(struct device *device, const uint8_t *expected)
{
	uint32_t index;

	for (index = 0; index < device->blocks; index++) {
		if (!device->journal_blocks[index] &&
		    memcmp(device->cache + (size_t)index * device->block_size,
			expected + (size_t)index * device->block_size, device->block_size) != 0) {
			return false;
		}
	}
	return true;
}

static bool
live_recover(struct device *device, const uint8_t *expected, bool intent_durable)
{
	struct ext4_recovery_report report;
	struct ext4_super_disk *super;
	enum ext4_result error;

	device_reset(device, device->stable);
	error = ext4_recover(&device->environment, &device->writer, &report);
	if (error == EXT4_CORRUPT) {
		super = (struct ext4_super_disk *)(device->cache + EXT4_SUPER_OFFSET);
		CHECK(device->metadata_checksum && device->writes == 0 &&
		    ext4_le32(&super->checksum) !=
			ext4_crc32c(UINT32_MAX, super, offsetof(struct ext4_super_disk, checksum)));
		return false;
	}
	EXPECT(error, EXT4_OK);
	CHECK(device->live == 0 && memcmp(device->cache, device->stable, device->size) == 0);
	CHECK(
	    live_equal(device, expected) || (!intent_durable && live_equal(device, device->base)));
	return true;
}

static void
live_contents(struct device *device, const char *name, bool zero)
{
	struct ext4_fs *fs;
	struct ext4_inode inode;
	uint8_t *bytes = malloc(device->block_size + 7);
	size_t index;
	size_t completed;

	CHECK(bytes != NULL);
	EXPECT(ext4_mount(&device->environment, &fs), EXT4_OK);
	inode = lookup(fs, name);
	EXPECT(ext4_read(fs, &inode, 0, bytes, device->block_size + 7, &completed), EXT4_OK);
	CHECK(completed == device->block_size + 7);
	for (index = 0; index < completed; index++) {
		CHECK(bytes[index] == (zero ? 0 : (uint8_t)(index * 17 + 23)));
	}
	ext4_unmount(fs);
	free(bytes);
}

static void
live_guards(struct device *device, const char *name)
{
	struct ext4_fs *fs;
	struct ext4_inode inode;
	struct ext4_inode result;
	struct ext4_inode_update update;
	uint32_t blocks;

	EXPECT(ext4_mount_writable(&device->environment, &device->writer, &fs), EXT4_OK);
	inode = lookup(fs, name);
	memset(&update, 0, sizeof(update));
	update.fields = EXT4_ATTR_PERMISSIONS | EXT4_ATTR_MODIFY_TIME | EXT4_ATTR_CHANGE_TIME;
	update.permissions = 0640;
	EXPECT(ext4_truncate(NULL, inode.number, inode.generation, 0, &update, &result),
	    EXT4_INVALID_ARGUMENT);
	EXPECT(ext4_truncate(fs, inode.number, inode.generation, 0, NULL, &result),
	    EXT4_INVALID_ARGUMENT);
	EXPECT(ext4_truncate(fs, inode.number, inode.generation, 0, &update, NULL),
	    EXT4_INVALID_ARGUMENT);
	EXPECT(
	    ext4_truncate(fs, inode.number, inode.generation + 1, 0, &update, &result), EXT4_STALE);
	EXPECT(ext4_truncate(fs, inode.number, inode.generation, UINT64_MAX, &update, &result),
	    EXT4_RANGE);
	blocks = fs->journal->blocks;
	fs->journal->blocks = fs->journal->first + 6;
	EXPECT(ext4_truncate(fs, inode.number, inode.generation, 0, &update, &result), EXT4_RANGE);
	fs->journal->blocks = blocks;
	CHECK(device->writes == 0 && memcmp(device->cache, device->base, device->size) == 0);
	ext4_unmount(fs);
}

static void
live_cases(struct device *device, const char *path, bool smoke, const char *exports, bool large)
{
	const char *base_name = strrchr(path, '/');
	const char *name;
	struct live_trace baseline;
	struct live_trace trace;
	uint8_t *expected = malloc(device->size);
	uint32_t index;
	uint32_t limit;
	uint32_t fault;
	uint32_t survival;
	uint32_t partial;
	uint32_t recovered = 0;
	uint32_t torn = 0;
	bool unwritten;

	base_name = base_name == NULL ? path : base_name + 1;
	unwritten = strncmp(base_name, "unwritten-", strlen("unwritten-")) == 0;
	name = large || unwritten ? "empty" : "payload.bin";
	CHECK(expected != NULL);
	if (large) {
		live_prepare_large(device);
	}
	export_image(device, exports, path, "before-");
	live_guards(device, name);
	device_reset(device, device->base);
	EXPECT(live_attempt(device, name, 0, 0, 0, false, &baseline), EXT4_OK);
	CHECK(baseline.transactions >= 1);
	if (large || unwritten ||
	    TEST_PAYLOAD_SIZE > (EXT4_ORPHAN_BATCH_BLOCKS + 2) * device->block_size) {
		CHECK(baseline.transactions > 1);
	}
	memcpy(expected, device->stable, device->size);
	live_contents(device, name, large || unwritten);
	export_image(device, exports, path, "after-");
	if (!smoke) {
		for (fault = 1; fault <= 2; fault++) {
			limit = fault == 1 ? baseline.allocations : baseline.reads;
			for (index = 1; index <= limit; index++) {
				device_reset(device, device->base);
				EXPECT(live_attempt(device, name, fault, index, 0, false, &trace),
				    fault == 1 ? EXT4_NO_MEMORY : EXT4_IO);
				CHECK(live_recover(device, expected, trace.intent_durable));
			}
		}
		for (index = 1; index <= baseline.events; index++) {
			for (survival = 0; survival < 3; survival++) {
				for (partial = 0; partial < 2; partial++) {
					device_reset(device, device->base);
					EXPECT(live_attempt(device, name, 3, index, survival,
						   partial != 0, &trace),
					    EXT4_IO);
					CHECK(device->off);
					if (live_recover(device, expected, trace.intent_durable)) {
						recovered++;
					} else {
						torn++;
					}
				}
			}
			/* A representative durable intent is also checked by independent
			 * tools. This is a stopped live call, not a synthesized list. */
			if (index == baseline.events / 2 && exports != NULL) {
				device_reset(device, device->base);
				EXPECT(live_attempt(device, name, 3, index, 0, false, &trace),
				    EXT4_IO);
				export_image(device, exports, path, "pending-");
			}
		}
		printf("PASS live truncate faults: allocations=%u reads=%u cuts=%u recovered=%u "
		       "torn_super_fail_closed=%u\n",
		    baseline.allocations, baseline.reads, baseline.events * 6, recovered, torn);
	} else if (exports != NULL) {
		device_reset(device, device->base);
		EXPECT(
		    live_attempt(device, name, 3, baseline.events / 2, 0, false, &trace), EXT4_IO);
		export_image(device, exports, path, "pending-");
		CHECK(live_recover(device, expected, trace.intent_durable));
	}
	free(expected);
	printf("PASS live truncate: %u transactions, %s\n", baseline.transactions, path);
}

static void
orphan_file_write_guards(struct device *device)
{
	struct ext4_fs *fs;
	struct ext4_inode file;
	struct ext4_inode payload;
	struct ext4_inode result;
	struct ext4_inode_disk *disk;
	struct ext4_inode_update update;
	struct ext4_recovery_report report;
	struct ext4_super_disk *super;
	struct ext4_le32 *entries;
	struct ext4_extent_header_disk *header;
	struct ext4_extent_disk *extent;
	uint64_t offset;
	uint64_t file_block;
	uint32_t index;
	size_t completed;
	uint8_t marker = 0xa5;

	EXPECT(ext4_mount_writable(&device->environment, &device->writer, &fs), EXT4_OK);
	CHECK(fs->orphan_file != NULL && fs->orphan_file->pending == 0);
	file = fs->orphan_file->inode;
	file_block = fs->orphan_file->blocks[0];
	memset(&update, 0, sizeof(update));
	update.fields = EXT4_ATTR_PERMISSIONS | EXT4_ATTR_MODIFY_TIME | EXT4_ATTR_CHANGE_TIME;
	update.permissions = 0600;
	update.modify_time.seconds = 1700000201;
	update.change_time.seconds = 1700000202;
	EXPECT(ext4_set_attributes(fs, file.number, file.generation, &update, &result),
	    EXT4_UNSUPPORTED);
	EXPECT(
	    ext4_truncate(fs, file.number, file.generation, 0, &update, &result), EXT4_UNSUPPORTED);
	EXPECT(ext4_write(fs, file.number, file.generation, 0, &marker, 1, &update, &completed),
	    EXT4_UNSUPPORTED);
	CHECK(completed == 0);
	for (index = 0; index < fs->orphan_file->block_count + fs->orphan_file->mapping_count;
	    index++) {
		CHECK(ext4_system_block(fs, fs->orphan_file->blocks[index]));
		EXPECT(ext4_data_block_valid(fs, fs->orphan_file->blocks[index]), EXT4_CORRUPT);
	}
	payload = lookup(fs, "payload.bin");
	EXPECT(ext4_inode_location(fs, payload.number, &offset), EXT4_OK);
	disk = (struct ext4_inode_disk *)(device->cache + offset);
	if (payload.flags & EXT4_INODE_EXTENTS) {
		header = (struct ext4_extent_header_disk *)disk->block_data;
		CHECK(ext4_le16(&header->depth) == 0 && ext4_le16(&header->entries) != 0);
		extent = (struct ext4_extent_disk *)(header + 1);
		ext4_encode32(&extent->physical_lo, (uint32_t)fs->orphan_file->blocks[0]);
		ext4_encode16(&extent->physical_hi, 0);
	} else {
		ext4_encode32(
		    (struct ext4_le32 *)disk->block_data, (uint32_t)fs->orphan_file->blocks[0]);
	}
	ext4_inode_checksum_set(fs, payload.number, disk);
	EXPECT(
	    ext4_write(fs, payload.number, payload.generation, 0, &marker, 1, &update, &completed),
	    EXT4_CORRUPT);
	CHECK(completed == 0);
	EXPECT(ext4_truncate_atomic(fs, payload.number, payload.generation, 0, &update, &result),
	    EXT4_CORRUPT);
	CHECK(device->writes == 0);
	ext4_unmount(fs);
	device_reset(device, device->base);
	/* A hidden pending slot is invalid even when the mount marker is clean. */
	EXPECT(ext4_mount(&device->environment, &fs), EXT4_OK);
	entries = (struct ext4_le32 *)(device->cache + file_block * device->block_size);
	ext4_encode32(entries, payload.number);
	orphan_block_checksum(device, fs, &file, file_block);
	ext4_unmount(fs);
	EXPECT(ext4_mount_writable(&device->environment, &device->writer, &fs), EXT4_CORRUPT);
	CHECK(fs == NULL && device->writes == 0 && device->live == 0);
	device_reset(device, device->base);
	/* Conversely, an empty file may carry only ORPHAN_PRESENT after a crash. */
	super = (struct ext4_super_disk *)(device->cache + EXT4_SUPER_OFFSET);
	ext4_encode32(&super->feature_ro_compat,
	    ext4_le32(&super->feature_ro_compat) | EXT4_FEATURE_RO_ORPHAN_PRESENT);
	super_checksum(device);
	memcpy(device->stable, device->cache, device->size);
	EXPECT(ext4_mount(&device->environment, &fs), EXT4_RECOVERY_REQUIRED);
	CHECK(fs == NULL && device->writes == 0);
	EXPECT(ext4_recover(&device->environment, &device->writer, &report), EXT4_OK);
	CHECK(report.cleaned_orphans == 0 && report.orphan_file_transfers == 0);
	check_equal(device, device->base);
	device_reset(device, device->base);
	puts("PASS orphan file inode and backing blocks excluded from ordinary mutations");
}

static void
test_image(const char *path, bool pending, bool smoke, const char *exports, bool live, bool large,
    enum orphan_format format)
{
	struct ext4_posix_image source;
	struct ext4_fs *fs;
	struct ext4_journal *journal;
	struct ext4_recovery_report report;
	struct device device;
	uint8_t *expected;
	uint32_t index;
	uint32_t block;
	uint32_t allocations;
	uint32_t reads;
	uint32_t events;
	uint32_t mount_allocations;
	uint32_t mount_reads;
	bool orphan_file;

	memset(&device, 0, sizeof(device));
	EXPECT(ext4_posix_open(&source, path), EXT4_OK);
	CHECK(source.environment.size_bytes <= TEST_IMAGE_LIMIT);
	device.size = (size_t)source.environment.size_bytes;
	device.base = malloc(device.size);
	device.cache = malloc(device.size);
	device.stable = malloc(device.size);
	expected = malloc(device.size);
	CHECK(device.base != NULL && device.cache != NULL && device.stable != NULL &&
	    expected != NULL);
	EXPECT(source.environment.read(source.environment.context, 0, device.base, device.size),
	    EXT4_OK);
	ext4_posix_close(&source);
	memcpy(device.cache, device.base, device.size);
	device.environment = (struct ext4_environment){ &device, device.size, device_read,
		device_allocate, device_release };
	device.writer = (struct ext4_write_environment){ &device, device_write, device_flush };
	EXPECT(ext4_load(&device.environment, true, &fs), EXT4_OK);
	device.block_size = fs->info.block_size;
	device.blocks = (uint32_t)(device.size / device.block_size);
	device.metadata_checksum = fs->metadata_checksum;
	orphan_file = fs->orphan_file_inode != 0;
	device.dirty = calloc(device.blocks, 1);
	device.journal_blocks = calloc(device.blocks, 1);
	CHECK(device.dirty != NULL && device.journal_blocks != NULL);
	EXPECT(ext4_journal_load(fs, &device.writer, &journal), EXT4_OK);
	mount_allocations = device.allocations;
	mount_reads = device.reads;
	for (index = 0; index < journal->run_count; index++) {
		for (block = 0; block < journal->runs[index].length; block++) {
			device.journal_blocks[journal->runs[index].physical + block] = 1;
		}
	}
	ext4_journal_close(journal);
	ext4_unmount(fs);
	device_reset(&device, device.base);
	if (orphan_file && !pending) {
		orphan_file_write_guards(&device);
	}
	if (live) {
		live_cases(&device, path, smoke, exports, large);
		goto finish;
	}
	if (!pending) {
		linked_fixture(&device, format);
	}
	export_image(&device, exports, path, "pending-");
	EXPECT(ext4_mount(&device.environment, &fs), EXT4_RECOVERY_REQUIRED);
	CHECK(fs == NULL && device.writes == 0 && device.live == 0);
	device_reset(&device, device.base);
	EXPECT(ext4_recover(&device.environment, &device.writer, &report), EXT4_OK);
	CHECK(report.cleaned_orphans == (pending ? 6U : TEST_LINKED_ORPHANS));
	CHECK(report.orphan_transactions >= report.cleaned_orphans);
	CHECK(report.orphan_file_transfers ==
	    (pending			     ? (orphan_file ? 6U : 0U)
		    : format == ORPHAN_FILE  ? TEST_LINKED_ORPHANS
		    : format == ORPHAN_MIXED ? 1U
					     : 0U));
	allocations = device.allocations;
	reads = device.reads;
	events = device.events;
	memcpy(expected, device.stable, device.size);
	check_equal(&device, expected);
	if (!pending) {
		check_linked(&device);
	}
	export_image(&device, exports, path, "clean-");
	EXPECT(ext4_recover(&device.environment, &device.writer, &report), EXT4_OK);
	CHECK(device.events == events && report.cleaned_orphans == 0);
	CHECK(memcmp(device.cache, expected, device.size) == 0);
	if (!smoke) {
		if (!pending) {
			if (format == ORPHAN_LEGACY) {
				malformed_cases(&device);
			} else if (format == ORPHAN_FILE) {
				orphan_file_malformed_cases(&device);
			}
			recount_cases(&device, expected);
		}
		resource_faults(
		    &device, expected, allocations, reads, mount_allocations, mount_reads);
		crash_cases(&device, expected, events);
	}
	if (!pending) {
		large_mapping(&device, expected, exports, path);
	}
finish:
	CHECK(device.live == 0);
	free(expected);
	free(device.base);
	free(device.cache);
	free(device.stable);
	free(device.dirty);
	free(device.journal_blocks);
	if (!live) {
		printf("PASS %s orphan recovery: %s\n",
		    pending ? "Linux unlinked" : "linked truncate", path);
	}
}

int
main(int argc, char **argv)
{
	const char *exports = NULL;
	bool pending = false;
	bool smoke = false;
	bool live = false;
	bool large = false;
	enum orphan_format format = ORPHAN_LEGACY;
	int index = 1;

	setvbuf(stdout, NULL, _IOLBF, 0);
	while (index < argc) {
		if (strcmp(argv[index], "--pending") == 0) {
			pending = true;
			index++;
		} else if (strcmp(argv[index], "--smoke") == 0) {
			smoke = true;
			index++;
		} else if (strcmp(argv[index], "--live") == 0) {
			live = true;
			index++;
		} else if (strcmp(argv[index], "--large") == 0) {
			large = true;
			index++;
		} else if (strcmp(argv[index], "--orphan-file") == 0) {
			format = ORPHAN_FILE;
			index++;
		} else if (strcmp(argv[index], "--mixed") == 0) {
			format = ORPHAN_MIXED;
			index++;
		} else if (strcmp(argv[index], "--export") == 0) {
			CHECK(index + 1 < argc);
			exports = argv[index + 1];
			index += 2;
		} else {
			break;
		}
	}
	CHECK(index < argc);
	CHECK((!live || !pending) && (!large || live));
	for (; index < argc; index++) {
		test_image(argv[index], pending, smoke, exports, live, large, format);
	}
	return 0;
}

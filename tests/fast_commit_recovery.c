/* SPDX-License-Identifier: BSD-3-Clause */
#include "journal.h"
#include "image.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define IMAGE_LIMIT (128U * 1024U * 1024U)
#define EVENT_LIMIT 8192U
#define DIRECTORY_DEPTH 16U
#define CHECK(expression)                                                                          \
	do {                                                                                       \
		if (!(expression)) {                                                               \
			fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #expression);           \
			exit(1);                                                                   \
		}                                                                                  \
	} while (0)
#define EXPECT(expression, expected) result_is((expression), (expected), #expression, __LINE__)

struct event {
	uint64_t offset;
	bool flush;
};

struct device {
	struct ext4_environment environment;
	struct ext4_write_environment writer;
	uint8_t *base;
	uint8_t *cache;
	uint8_t *stable;
	uint8_t *dirty;
	size_t size;
	uint32_t block_size;
	uint32_t blocks;
	uint32_t events;
	uint32_t stop_at;
	uint32_t live;
	uint32_t allocations;
	uint32_t reads;
	uint32_t fail_allocation;
	uint32_t fail_read;
	unsigned int survival;
	bool partial;
	bool off;
	struct event history[EVENT_LIMIT];
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

static void *
allocate(void *context, size_t bytes)
{
	struct device *device = context;
	void *result;

	if (++device->allocations == device->fail_allocation) {
		return NULL;
	}
	result = malloc(bytes);
	if (result != NULL) {
		device->live++;
	}
	return result;
}

static void
release(void *context, void *buffer, size_t bytes)
{
	struct device *device = context;

	(void)bytes;
	CHECK(device->live != 0 && buffer != NULL);
	device->live--;
	free(buffer);
}

static enum ext4_result
read_device(void *context, uint64_t offset, void *buffer, size_t length)
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
persist(struct device *device, unsigned int survival)
{
	uint32_t block;

	for (block = 0; block < device->blocks; block++) {
		if (device->dirty[block] && (survival == 1 || (survival == 2 && (block & 1)))) {
			memcpy(device->stable + (size_t)block * device->block_size,
			    device->cache + (size_t)block * device->block_size, device->block_size);
		}
		device->dirty[block] = 0;
	}
}

static bool
event(struct device *device, uint64_t offset, bool flush)
{
	CHECK(device->events < EVENT_LIMIT);
	device->history[device->events].offset = offset;
	device->history[device->events].flush = flush;
	return ++device->events == device->stop_at;
}

static enum ext4_result
write_device(void *context, uint64_t offset, const void *buffer, size_t length)
{
	struct device *device = context;
	size_t prefix;
	bool cut;

	CHECK(offset <= device->size && length <= device->size - offset);
	CHECK(offset % device->block_size == 0 && length == device->block_size);
	if (device->off) {
		return EXT4_IO;
	}
	cut = event(device, offset, false);
	if (!cut || device->partial) {
		prefix = cut ? length / 2U : length;
		if (cut && offset == EXT4_SUPER_OFFSET / device->block_size * device->block_size) {
			prefix = EXT4_SUPER_OFFSET % device->block_size + EXT4_SECTOR_SIZE;
		}
		memcpy(device->cache + offset, buffer, prefix);
		device->dirty[offset / device->block_size] = 1;
	}
	if (cut) {
		persist(device, device->survival);
		device->off = true;
		return EXT4_IO;
	}
	return EXT4_OK;
}

static enum ext4_result
flush_device(void *context)
{
	struct device *device = context;
	bool cut;

	if (device->off) {
		return EXT4_IO;
	}
	cut = event(device, 0, true);
	persist(device, cut ? device->survival : 1);
	device->off = cut;
	return cut ? EXT4_IO : EXT4_OK;
}

static void
reset(struct device *device)
{
	CHECK(device->live == 0);
	memcpy(device->cache, device->base, device->size);
	memcpy(device->stable, device->base, device->size);
	memset(device->dirty, 0, device->blocks);
	device->events = 0;
	device->stop_at = 0;
	device->allocations = 0;
	device->reads = 0;
	device->fail_allocation = 0;
	device->fail_read = 0;
	device->off = false;
	device->partial = false;
}

static void
reboot_device(struct device *device)
{
	CHECK(device->live == 0);
	memcpy(device->cache, device->stable, device->size);
	memset(device->dirty, 0, device->blocks);
	device->events = 0;
	device->stop_at = 0;
	device->allocations = 0;
	device->reads = 0;
	device->fail_allocation = 0;
	device->fail_read = 0;
	device->off = false;
	device->partial = false;
}

static bool
same_time(struct ext4_timestamp a, struct ext4_timestamp b)
{
	return a.seconds == b.seconds && a.nanoseconds == b.nanoseconds;
}

static void
compare_inode(struct ext4_fs *fs, struct ext4_fs *reference, const struct ext4_inode *inode,
    const struct ext4_inode *expected, unsigned int depth, uint64_t *directory_blocks,
    uint64_t *expected_directory_blocks)
{
	struct ext4_inode child;
	struct ext4_inode expected_child;
	struct ext4_dir_entry entry;
	uint8_t actual_data[4096];
	uint8_t expected_data[4096];
	uint64_t cookie = 0;
	uint64_t offset;
	size_t actual_length;
	size_t expected_length;
	size_t length;
	uint32_t actual_entries = 0;
	uint32_t expected_entries = 0;
	uint32_t sectors_per_block = fs->info.block_size / EXT4_SECTOR_SIZE;
	enum ext4_result error;

	CHECK(depth <= DIRECTORY_DEPTH);
	CHECK(inode->number == expected->number && inode->generation == expected->generation);
	CHECK(inode->mode == expected->mode && inode->links == expected->links);
	CHECK(inode->uid == expected->uid && inode->gid == expected->gid);
	if ((inode->mode & EXT4_MODE_TYPE) == EXT4_MODE_DIRECTORY) {
		CHECK(inode->blocks_512 % sectors_per_block == 0 &&
		    expected->blocks_512 % sectors_per_block == 0);
		*directory_blocks += inode->blocks_512 / sectors_per_block;
		*expected_directory_blocks += expected->blocks_512 / sectors_per_block;
		for (;;) {
			error = ext4_next_dir(reference, expected, &cookie, &entry);
			if (error == EXT4_NOT_FOUND) {
				break;
			}
			EXPECT(error, EXT4_OK);
			expected_entries++;
			EXPECT(
			    ext4_lookup(fs, inode, entry.name, entry.name_length, &child), EXT4_OK);
			EXPECT(ext4_get_inode(reference, entry.inode, &expected_child), EXT4_OK);
			CHECK(child.number == expected_child.number);
			if (entry.name[0] != '.' ||
			    (entry.name_length != 1 &&
				!(entry.name_length == 2 && entry.name[1] == '.'))) {
				compare_inode(fs, reference, &child, &expected_child, depth + 1U,
				    directory_blocks, expected_directory_blocks);
			}
		}
		cookie = 0;
		while ((error = ext4_next_dir(fs, inode, &cookie, &entry)) == EXT4_OK) {
			actual_entries++;
		}
		EXPECT(error, EXT4_NOT_FOUND);
		CHECK(actual_entries == expected_entries);
		/* Fast records do not provide parent replay timestamps or prescribe
		 * the allocator's directory block layout. Namespace is compared above. */
		return;
	}
	CHECK(inode->size == expected->size && inode->blocks_512 == expected->blocks_512);
	CHECK(inode->flags == expected->flags);
	CHECK(same_time(inode->change_time, expected->change_time));
	CHECK(same_time(inode->modify_time, expected->modify_time));
	CHECK(same_time(inode->birth_time, expected->birth_time));
	for (offset = 0; offset < inode->size; offset += length) {
		length = inode->size - offset < sizeof(actual_data) ? (size_t)(inode->size - offset)
								    : sizeof(actual_data);
		EXPECT(ext4_read(fs, inode, offset, actual_data, length, &actual_length), EXT4_OK);
		EXPECT(
		    ext4_read(reference, expected, offset, expected_data, length, &expected_length),
		    EXT4_OK);
		CHECK(actual_length == length && expected_length == length);
		CHECK(memcmp(actual_data, expected_data, length) == 0);
	}
}

static void
compare(struct device *device, struct ext4_fs *reference)
{
	struct ext4_fs *fs;
	struct ext4_info info;
	struct ext4_info expected;
	struct ext4_inode root;
	struct ext4_inode expected_root;
	struct ext4_recovery_report report;
	uint32_t events = device->events;
	uint64_t directory_blocks = 0;
	uint64_t expected_directory_blocks = 0;

	EXPECT(ext4_mount(&device->environment, &fs), EXT4_OK);
	ext4_get_info(fs, &info);
	ext4_get_info(reference, &expected);
	CHECK(info.block_size == expected.block_size && info.blocks == expected.blocks &&
	    info.free_inodes == expected.free_inodes);
	EXPECT(ext4_get_inode(fs, EXT4_ROOT_INODE, &root), EXT4_OK);
	EXPECT(ext4_get_inode(reference, EXT4_ROOT_INODE, &expected_root), EXT4_OK);
	compare_inode(
	    fs, reference, &root, &expected_root, 0, &directory_blocks, &expected_directory_blocks);
	/* Reconstructed directories can use a different index/tree layout. Every
	 * difference in free blocks must be charged to those directories; regular
	 * file allocation, metadata and bytes remain exact comparisons above. */
	CHECK(info.free_blocks + directory_blocks ==
	    expected.free_blocks + expected_directory_blocks);
	ext4_unmount(fs);
	EXPECT(ext4_recover(&device->environment, &device->writer, &report), EXT4_OK);
	CHECK(device->events == events && device->live == 0);
}

int
main(int argc, char **argv)
{
	struct ext4_posix_image source;
	struct ext4_posix_image oracle;
	struct ext4_fs *reference;
	struct ext4_recovery_report report;
	struct device *device;
	const struct ext4_super_disk *super;
	uint32_t operations;
	uint32_t allocations;
	uint32_t reads;
	uint32_t limit;
	uint32_t fault;
	uint32_t stop;
	uint32_t cuts = 0;
	uint32_t torn = 0;
	unsigned int survival;
	unsigned int partial;
	uint32_t checksum;
	bool damaged_super;
	bool faults;
	bool resources;
	enum ext4_result error;

	if (argc != 3 && argc != 4) {
		fprintf(stderr,
		    "usage: %s PENDING_IMAGE VERIFIED_REFERENCE_IMAGE [--faults|--resources]\n",
		    argv[0]);
		return 2;
	}
	faults = argc == 4 && strcmp(argv[3], "--faults") == 0;
	resources = argc == 4 && strcmp(argv[3], "--resources") == 0;
	CHECK(argc == 3 || faults || resources);
	EXPECT(ext4_posix_open(&source, argv[1]), EXT4_OK);
	EXPECT(ext4_posix_open(&oracle, argv[2]), EXT4_OK);
	EXPECT(ext4_mount(&oracle.environment, &reference), EXT4_OK);
	CHECK(source.environment.size_bytes <= IMAGE_LIMIT);
	device = calloc(1, sizeof(*device));
	CHECK(device != NULL);
	device->size = (size_t)source.environment.size_bytes;
	device->base = malloc(device->size);
	device->cache = malloc(device->size);
	device->stable = malloc(device->size);
	CHECK(device->base != NULL && device->cache != NULL && device->stable != NULL);
	EXPECT(source.environment.read(source.environment.context, 0, device->base, device->size),
	    EXT4_OK);
	super = (const struct ext4_super_disk *)(device->base + EXT4_SUPER_OFFSET);
	CHECK(ext4_le16(&super->magic) == EXT4_SUPER_MAGIC);
	device->block_size = EXT4_MIN_BLOCK_SIZE << ext4_le32(&super->log_block_size);
	device->blocks = (uint32_t)(device->size / device->block_size);
	device->dirty = calloc(device->blocks, 1);
	CHECK(device->dirty != NULL);
	device->environment.context = device;
	device->environment.size_bytes = device->size;
	device->environment.read = read_device;
	device->environment.allocate = allocate;
	device->environment.release = release;
	device->writer.context = device;
	device->writer.write = write_device;
	device->writer.flush = flush_device;
	reset(device);
	EXPECT(ext4_recover(&device->environment, &device->writer, &report), EXT4_OK);
	CHECK(report.fast_commits != 0 && device->live == 0);
	operations = device->events;
	allocations = device->allocations;
	reads = device->reads;
	compare(device, reference);
	printf("PASS %u fast commits match verified namespace, data and accounting; %u durability "
	       "events\n",
	    report.fast_commits, operations);
	if (resources) {
		for (fault = 0; fault < 2; fault++) {
			limit = fault == 0 ? allocations : reads;
			for (stop = 1; stop <= limit; stop++) {
				reset(device);
				device->fail_allocation = fault == 0 ? stop : 0;
				device->fail_read = fault == 0 ? 0 : stop;
				EXPECT(ext4_recover(&device->environment, &device->writer, &report),
				    fault == 0 ? EXT4_NO_MEMORY : EXT4_IO);
				CHECK(device->live == 0);
				reboot_device(device);
				EXPECT(ext4_recover(&device->environment, &device->writer, &report),
				    EXT4_OK);
				compare(device, reference);
			}
		}
		printf(
		    "PASS %u allocation and %u read failures; restart completes semantic replay\n",
		    allocations, reads);
	}
	if (faults) {
		for (survival = 0; survival < 3; survival++) {
			for (partial = 0; partial < 2; partial++) {
				for (stop = 1; stop <= operations; stop++) {
					reset(device);
					device->stop_at = stop;
					device->survival = survival;
					device->partial = partial != 0;
					EXPECT(ext4_recover(
						   &device->environment, &device->writer, &report),
					    EXT4_IO);
					CHECK(device->events == stop && device->live == 0);
					reboot_device(device);
					super = (const struct ext4_super_disk *)(device->cache +
					    EXT4_SUPER_OFFSET);
					checksum = ext4_crc32c(UINT32_MAX, super,
					    offsetof(struct ext4_super_disk, checksum));
					damaged_super = checksum != ext4_le32(&super->checksum);
					error = ext4_recover(
					    &device->environment, &device->writer, &report);
					if (damaged_super) {
						EXPECT(error, EXT4_CORRUPT);
						CHECK(device->events == 0);
						torn++;
					} else {
						if (error != EXT4_OK) {
							fprintf(stderr,
							    "retry cut=%u survival=%u partial=%u: "
							    "%s\n",
							    stop, survival, partial,
							    ext4_result_string(error));
						}
						EXPECT(error, EXT4_OK);
						compare(device, reference);
					}
					CHECK(device->live == 0);
					cuts++;
				}
			}
		}
		printf("PASS %u interrupted recoveries; %u torn primary superblocks rejected\n",
		    cuts, torn);
	}
	free(device->dirty);
	free(device->stable);
	free(device->cache);
	free(device->base);
	free(device);
	ext4_unmount(reference);
	CHECK(oracle.live_allocations == 0);
	ext4_posix_close(&source);
	ext4_posix_close(&oracle);
	return 0;
}

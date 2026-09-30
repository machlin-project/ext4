/* SPDX-License-Identifier: BSD-3-Clause */
#ifndef MACHLIN_EXT4_TEST_STORAGE_H
#define MACHLIN_EXT4_TEST_STORAGE_H

#include "journal.h"
#include "image.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TEST_IMAGE_LIMIT (128U * 1024U * 1024U)

/* Independent volatile/stable device model shared by namespace tests. Each
 * callback failure is observable; a crash keeps none/all/alternating blocks. */
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
	uint32_t commit_barrier;
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
		if (!device->intent_durable) {
			device->commit_barrier = device->events;
		}
		device->intent_durable = true;
	}
}

static enum ext4_result
device_write(void *context, uint64_t offset, const void *buffer, size_t length)
{
	struct device *device = context;
	size_t partial;

	CHECK(!device->off && offset % device->block_size == 0 && length != 0 &&
	    length % device->block_size == 0);
	CHECK(offset <= device->size && length <= device->size - offset);
	device->writes++;
	if (++device->events == device->stop_at) {
		if (device->partial) {
			partial = length / 2;
			if (length > device->block_size) {
				/* Tear inside a block after the batch's complete prefix. */
				partial += EXT4_SECTOR_SIZE;
			}
			if (offset ==
			    (EXT4_SUPER_OFFSET / device->block_size) * device->block_size) {
				partial = EXT4_SUPER_OFFSET % device->block_size + EXT4_SECTOR_SIZE;
			}
			memcpy(device->cache + offset, buffer, partial);
			memset(device->dirty + offset / device->block_size, 1,
			    (partial + device->block_size - 1U) / device->block_size);
		}
		device_persist(device, device->survival);
		device->off = true;
		return EXT4_IO;
	}
	memcpy(device->cache + offset, buffer, length);
	memset(device->dirty + offset / device->block_size, 1, length / device->block_size);
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
	device->commit_barrier = 0;
	device->allocations = device->fail_read = device->fail_allocation = 0;
	device->off = false;
	device->partial = false;
	device->commit_written = false;
	device->intent_durable = false;
}

static void
storage_open(struct device *device, const char *path)
{
	struct ext4_posix_image source;
	struct ext4_fs *fs;
	struct ext4_journal *journal;
	uint32_t index;
	uint32_t block;

	memset(device, 0, sizeof(*device));
	EXPECT(ext4_posix_open(&source, path), EXT4_OK);
	CHECK(source.environment.size_bytes <= TEST_IMAGE_LIMIT);
	device->size = (size_t)source.environment.size_bytes;
	device->base = malloc(device->size);
	device->cache = malloc(device->size);
	device->stable = malloc(device->size);
	CHECK(device->base != NULL && device->cache != NULL && device->stable != NULL);
	EXPECT(source.environment.read(source.environment.context, 0, device->base, device->size),
	    EXT4_OK);
	ext4_posix_close(&source);
	memcpy(device->cache, device->base, device->size);
	device->environment = (struct ext4_environment){ device, device->size, device_read,
		device_allocate, device_release };
	device->writer =
	    (struct ext4_write_environment){ device, device_write, device_flush, NULL };
	EXPECT(ext4_mount(&device->environment, &fs), EXT4_OK);
	device->block_size = fs->info.block_size;
	device->blocks = (uint32_t)(device->size / device->block_size);
	device->metadata_checksum = fs->metadata_checksum;
	device->dirty = calloc(device->blocks, 1);
	device->journal_blocks = calloc(device->blocks, 1);
	CHECK(device->dirty != NULL && device->journal_blocks != NULL);
	/* A volume without a journal has no blocks outside the image comparison. */
	if (fs->info.feature_compat & EXT4_FEATURE_COMPAT_HAS_JOURNAL) {
		EXPECT(ext4_journal_load(fs, &device->writer, &journal), EXT4_OK);
		for (index = 0; index < journal->run_count; index++) {
			for (block = 0; block < journal->runs[index].length; block++) {
				device->journal_blocks[journal->runs[index].physical + block] = 1;
			}
		}
		ext4_journal_close(journal);
	}
	ext4_unmount(fs);
	device_reset(device, device->base);
}

static void
storage_close(struct device *device)
{
	CHECK(device->live == 0);
	free(device->journal_blocks);
	free(device->dirty);
	free(device->stable);
	free(device->cache);
	free(device->base);
}

static bool
storage_equal(struct device *device, const uint8_t *expected)
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

static inline bool
storage_recover(struct device *device, const uint8_t *expected, bool committed)
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
	    storage_equal(device, expected) || (!committed && storage_equal(device, device->base)));
	return true;
}

static inline void
storage_export(struct device *device, const char *directory, const char *source, const char *prefix)
{
	const char *name = strrchr(source, '/');
	char path[4096];
	FILE *stream;
	int length;

	if (directory == NULL) {
		return;
	}
	name = name == NULL ? source : name + 1;
	length = snprintf(path, sizeof(path), "%s/%s%s", directory, prefix, name);
	CHECK(length > 0 && (size_t)length < sizeof(path));
	stream = fopen(path, "wbx");
	CHECK(stream != NULL);
	CHECK(fwrite(device->stable, 1, device->size, stream) == device->size);
	CHECK(fclose(stream) == 0);
}

#endif

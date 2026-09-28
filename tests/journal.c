/* SPDX-License-Identifier: BSD-3-Clause */
#include "journal.h"
#include "image.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TEST_TARGETS 2U
#define TEST_EVENTS 2048U
#define TEST_SECTOR_SIZE 512U
#define TEST_UNKNOWN_JOURNAL_FEATURE 0x80000000U
#define TEST_UNKNOWN_TAG_FLAG 0x8000U

#define CHECK(expression)                                                                          \
	do {                                                                                       \
		if (!(expression)) {                                                               \
			fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #expression);           \
			exit(1);                                                                   \
		}                                                                                  \
	} while (0)

#define EXPECT(expression, expected)                                                               \
	do {                                                                                       \
		enum ext4_result actual_result = (expression);                                     \
		if (actual_result != (expected)) {                                                 \
			fprintf(stderr, "%s:%d: %s: got %s, expected %s\n", __FILE__, __LINE__,    \
			    #expression, ext4_result_string(actual_result),                        \
			    ext4_result_string(expected));                                         \
			exit(1);                                                                   \
		}                                                                                  \
	} while (0)

enum survival { DROP_VOLATILE, PERSIST_VOLATILE, PERSIST_ALTERNATE, SURVIVAL_COUNT };

struct event {
	uint64_t offset;
	bool flush;
};

struct device {
	struct ext4_environment environment;
	struct ext4_write_environment writer;
	uint8_t *original;
	uint8_t *base;
	uint8_t *stable;
	uint8_t *cache;
	uint8_t *dirty;
	uint8_t *pending;
	uint64_t *journal_map;
	uint64_t target[TEST_TARGETS];
	uint64_t commit_offset;
	uint64_t live;
	uint64_t allocations;
	uint64_t fail_allocation;
	uint64_t reads;
	uint64_t fail_read;
	size_t size;
	uint32_t block_size;
	uint32_t blocks;
	uint32_t journal_blocks;
	uint32_t journal_first;
	uint32_t operations;
	uint32_t stop_at;
	uint32_t home_event;
	uint32_t writes;
	uint32_t profile;
	bool checksum_v1;
	struct event events[TEST_EVENTS];
	enum survival survival;
	bool partial;
	bool off;
};

static void *
device_allocate(void *context, size_t size)
{
	struct device *device = context;
	void *allocation;

	device->allocations++;
	if (device->allocations == device->fail_allocation) {
		return NULL;
	}
	allocation = malloc(size);
	if (allocation != NULL) {
		device->live++;
	}
	return allocation;
}

static void
device_release(void *context, void *allocation, size_t size)
{
	struct device *device = context;

	(void)size;
	CHECK(allocation != NULL && device->live != 0);
	device->live--;
	free(allocation);
}

static enum ext4_result
device_read(void *context, uint64_t offset, void *buffer, size_t length)
{
	struct device *device = context;

	device->reads++;
	CHECK(offset <= device->size && length <= device->size - offset);
	if (device->off || device->reads == device->fail_read) {
		return EXT4_IO;
	}
	memcpy(buffer, device->cache + offset, length);
	return EXT4_OK;
}

static void
device_persist(struct device *device, enum survival survival)
{
	uint32_t block;

	for (block = 0; block < device->blocks; block++) {
		if (device->dirty[block] &&
		    (survival == PERSIST_VOLATILE ||
			(survival == PERSIST_ALTERNATE && (block & 1)))) {
			memcpy(device->stable + (size_t)block * device->block_size,
			    device->cache + (size_t)block * device->block_size, device->block_size);
		}
		device->dirty[block] = 0;
	}
}

static bool
device_event(struct device *device, uint64_t offset, bool flush)
{
	CHECK(!device->off);
	CHECK(device->operations < TEST_EVENTS);
	device->events[device->operations].offset = offset;
	device->events[device->operations].flush = flush;
	device->operations++;
	return device->operations == device->stop_at;
}

static enum ext4_result
device_write(void *context, uint64_t offset, const void *buffer, size_t length)
{
	struct device *device = context;
	const struct ext4_jbd_header *header = buffer;
	const struct ext4_jbd_header *durable;
	uint32_t index;
	size_t partial;
	bool stop;

	CHECK(offset % device->block_size == 0 && length == device->block_size);
	CHECK(offset <= device->size && length <= device->size - offset);
	stop = device_event(device, offset, false);
	device->writes++;
	for (index = 0; index < TEST_TARGETS; index++) {
		if (offset == device->target[index] * device->block_size) {
			if (device->home_event == 0) {
				device->home_event = device->operations;
			}
			/* A real drive may persist a home write immediately. The commit
			 * must already be on stable media when that write is submitted. */
			durable = (const struct ext4_jbd_header *)(device->stable +
			    device->commit_offset);
			CHECK(ext4_be32(&durable->magic) == EXT4_JBD_MAGIC);
			CHECK(ext4_be32(&durable->type) == EXT4_JBD_COMMIT);
		}
	}
	if (stop) {
		if (device->partial) {
			partial = length / 2;
			if (offset ==
			    (EXT4_SUPER_OFFSET / device->block_size) * device->block_size) {
				partial = EXT4_SUPER_OFFSET % device->block_size + TEST_SECTOR_SIZE;
			}
			memcpy(device->cache + offset, buffer, partial);
			device->dirty[offset / device->block_size] = 1;
		}
		device_persist(device, device->survival);
		device->off = true;
		return EXT4_IO;
	}
	if (ext4_be32(&header->magic) == EXT4_JBD_MAGIC &&
	    ext4_be32(&header->type) == EXT4_JBD_COMMIT) {
		device->commit_offset = offset;
	}
	memcpy(device->cache + offset, buffer, length);
	device->dirty[offset / device->block_size] = 1;
	return EXT4_OK;
}

static enum ext4_result
device_flush(void *context)
{
	struct device *device = context;
	bool stop;

	stop = device_event(device, 0, true);
	device_persist(device, stop ? device->survival : PERSIST_VOLATILE);
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
	memcpy(device->stable, source, device->size);
	memcpy(device->cache, source, device->size);
	memset(device->dirty, 0, device->blocks);
	device->operations = 0;
	device->writes = 0;
	device->reads = 0;
	device->allocations = 0;
	device->fail_allocation = 0;
	device->fail_read = 0;
	device->stop_at = 0;
	device->home_event = 0;
	device->partial = false;
	device->off = false;
}

static void
device_power_on(struct device *device)
{
	CHECK(device->live == 0);
	memcpy(device->cache, device->stable, device->size);
	memset(device->dirty, 0, device->blocks);
	device->off = false;
	device->operations = 0;
	device->writes = 0;
	device->reads = 0;
	device->allocations = 0;
	device->fail_allocation = 0;
	device->fail_read = 0;
	device->stop_at = 0;
	device->partial = false;
}

static void
device_initialize(struct device *device, const char *path)
{
	struct ext4_posix_image image;
	struct ext4_fs *fs;
	struct ext4_journal *journal;
	struct ext4_inode root;
	struct ext4_inode file;
	uint32_t run;
	uint32_t block;
	uint32_t index;

	memset(device, 0, sizeof(*device));
	EXPECT(ext4_posix_open(&image, path), EXT4_OK);
	EXPECT(ext4_mount(&image.environment, &fs), EXT4_OK);
	device->size = (size_t)image.environment.size_bytes;
	device->block_size = fs->info.block_size;
	device->blocks = (uint32_t)(device->size / device->block_size);
	CHECK(device->size <= 128U * 1024U * 1024U);
	device->original = malloc(device->size);
	device->base = malloc(device->size);
	device->stable = malloc(device->size);
	device->cache = malloc(device->size);
	device->pending = malloc(device->size);
	device->dirty = calloc(device->blocks, 1);
	CHECK(device->original && device->base && device->stable && device->cache &&
	    device->pending && device->dirty);
	EXPECT(image.environment.read(&image, 0, device->original, device->size), EXT4_OK);
	EXPECT(ext4_get_inode(fs, EXT4_ROOT_INODE, &root), EXT4_OK);
	EXPECT(ext4_lookup(fs, &root, (const uint8_t *)"payload.bin", strlen("payload.bin"), &file),
	    EXT4_OK);
	for (index = 0; index < TEST_TARGETS; index++) {
		EXPECT(ext4_map_block(fs, &file, index, &device->target[index]), EXT4_OK);
		CHECK(device->target[index] != 0);
	}
	device->environment.context = device;
	device->environment.size_bytes = device->size;
	device->environment.read = device_read;
	device->environment.allocate = device_allocate;
	device->environment.release = device_release;
	device->writer.context = device;
	device->writer.write = device_write;
	device->writer.flush = device_flush;
	EXPECT(ext4_journal_open(fs, &device->writer, &journal), EXT4_OK);
	device->journal_blocks = journal->blocks;
	device->journal_first = journal->first;
	device->journal_map = calloc(journal->blocks, sizeof(*device->journal_map));
	CHECK(device->journal_map != NULL);
	for (run = 0; run < journal->run_count; run++) {
		for (block = 0; block < journal->runs[run].length; block++) {
			device->journal_map[journal->runs[run].logical + block] =
			    journal->runs[run].physical + block;
		}
	}
	ext4_journal_close(journal);
	ext4_unmount(fs);
	CHECK(image.live_allocations == 0);
	ext4_posix_close(&image);
}

static void
journal_super_checksum(struct ext4_jbd_super *super)
{
	uint32_t checksum;

	if (ext4_be32(&super->feature_incompat) & (EXT4_JBD_CSUM_V2 | EXT4_JBD_CSUM_V3)) {
		ext4_encode_be32(&super->checksum, 0);
		checksum = ext4_crc32c(UINT32_MAX, super, sizeof(*super));
		ext4_encode_be32(&super->checksum, checksum);
	}
}

static void
device_profile(struct device *device, uint32_t features, bool checksum_v1)
{
	struct ext4_jbd_super *super;

	memcpy(device->base, device->original, device->size);
	super =
	    (struct ext4_jbd_super *)(device->base + device->journal_map[0] * device->block_size);
	ext4_encode_be32(&super->feature_incompat, features);
	ext4_encode_be32(&super->feature_compat, checksum_v1 ? EXT4_JBD_COMPAT_CHECKSUM : 0);
	super->checksum_type = EXT4_JBD_CRC32C;
	/* Exercise unsigned transaction-ID wraparound as part of every profile. */
	ext4_encode_be32(&super->sequence, UINT32_MAX - 1);
	journal_super_checksum(super);
	device->profile = features;
	device->checksum_v1 = checksum_v1;
	device_reset(device, device->base);
}

static void
fill_target(void *buffer, uint32_t block_size, uint32_t index)
{
	memset(buffer, index == 0 ? 0x53 : 0xa7, block_size);
	if (index == 0) {
		ext4_encode_be32(buffer, EXT4_JBD_MAGIC);
	}
}

static enum ext4_result
run_transaction(struct device *device)
{
	struct ext4_fs *fs;
	struct ext4_journal *journal;
	struct ext4_transaction *transaction;
	void *buffer;
	void *same;
	uint32_t index;
	enum ext4_result error;

	EXPECT(ext4_mount(&device->environment, &fs), EXT4_OK);
	EXPECT(ext4_journal_open(fs, &device->writer, &journal), EXT4_OK);
	EXPECT(ext4_transaction_begin(journal, TEST_TARGETS, &transaction), EXT4_OK);
	for (index = 0; index < TEST_TARGETS; index++) {
		EXPECT(
		    ext4_transaction_buffer(transaction, device->target[index], &buffer), EXT4_OK);
		fill_target(buffer, device->block_size, index);
		EXPECT(ext4_transaction_buffer(transaction, device->target[index], &same), EXT4_OK);
		CHECK(buffer == same);
		CHECK(memcmp(device->cache + device->target[index] * device->block_size,
			  device->base + device->target[index] * device->block_size,
			  device->block_size) == 0);
	}
	error = ext4_transaction_commit(transaction);
	if (error == EXT4_OK) {
		error = ext4_journal_finish(journal);
	} else {
		EXPECT(ext4_transaction_begin(journal, 1, &transaction), EXT4_RECOVERY_REQUIRED);
		CHECK(transaction == NULL);
		EXPECT(ext4_journal_finish(journal), EXT4_IO);
	}
	ext4_journal_close(journal);
	ext4_unmount(fs);
	CHECK(device->live == 0);
	return error;
}

static bool
check_outcome(struct device *device)
{
	uint8_t *expected;
	uint32_t index;
	bool all_old = true;
	bool all_new = true;

	expected = malloc(device->block_size);
	CHECK(expected != NULL);
	for (index = 0; index < TEST_TARGETS; index++) {
		fill_target(expected, device->block_size, index);
		all_new &= memcmp(device->stable + device->target[index] * device->block_size,
			       expected, device->block_size) == 0;
		all_old &= memcmp(device->stable + device->target[index] * device->block_size,
			       device->base + device->target[index] * device->block_size,
			       device->block_size) == 0;
	}
	free(expected);
	CHECK(all_old || all_new);
	return all_new;
}

static void
check_clean(struct device *device)
{
	struct ext4_fs *fs;
	struct ext4_recovery_report report;
	uint32_t writes;

	EXPECT(ext4_mount(&device->environment, &fs), EXT4_OK);
	ext4_unmount(fs);
	writes = device->writes;
	EXPECT(ext4_recover(&device->environment, &device->writer, &report), EXT4_OK);
	CHECK(report.transactions == 0 && device->writes == writes);
	CHECK(device->live == 0);
}

static void
export_pending(struct device *device, const char *directory)
{
	char path[1024];
	FILE *file;
	const char *version = device->checksum_v1 ? "v1-" : "";
	int length;

	if (directory == NULL) {
		return;
	}
	length = snprintf(path, sizeof(path), "%s/writer-%s%u-%u.img", directory, version,
	    device->block_size, device->profile);
	CHECK(length > 0 && (size_t)length < sizeof(path));
	file = fopen(path, "wbx");
	CHECK(file != NULL);
	CHECK(fwrite(device->pending, 1, device->size, file) == device->size);
	CHECK(fclose(file) == 0);
	length = snprintf(path, sizeof(path), "%s/writer-%s%u-%u.json", directory, version,
	    device->block_size, device->profile);
	CHECK(length > 0 && (size_t)length < sizeof(path));
	file = fopen(path, "wx");
	CHECK(file != NULL);
	CHECK(
	    fprintf(file,
		"{\"block_size\":%u,\"features\":%u,\"checksum_v1\":%s,\"async_commit\":%s,"
		"\"targets\":[%llu,%llu]}\n",
		device->block_size, device->profile, device->checksum_v1 ? "true" : "false",
		device->profile & EXT4_JBD_ASYNC_COMMIT ? "true" : "false",
		(unsigned long long)device->target[0], (unsigned long long)device->target[1]) > 0);
	CHECK(fclose(file) == 0);
}

static void
test_power_loss(struct device *device, const char *directory, bool export_only)
{
	struct ext4_recovery_report report;
	struct ext4_fs *fs;
	struct event events[TEST_EVENTS];
	uint32_t operations;
	uint32_t home_event;
	uint32_t cut;
	uint32_t survival;
	uint32_t partial;
	uint32_t rejected_controls = 0;
	uint32_t recovered = 0;
	enum ext4_result error;

	device_reset(device, device->base);
	EXPECT(run_transaction(device), EXT4_OK);
	operations = device->operations;
	home_event = device->home_event;
	memcpy(events, device->events, sizeof(events));
	CHECK(check_outcome(device));
	check_clean(device);
	CHECK(home_event != 0);
	if (export_only) {
		device_reset(device, device->base);
		device->stop_at = home_event;
		device->survival = DROP_VOLATILE;
		EXPECT(run_transaction(device), EXT4_IO);
		device_power_on(device);
		memcpy(device->pending, device->stable, device->size);
		export_pending(device, directory);
		EXPECT(ext4_recover(&device->environment, &device->writer, &report), EXT4_OK);
		CHECK(report.transactions == 1 && check_outcome(device));
		check_clean(device);
		return;
	}
	for (survival = 0; survival < SURVIVAL_COUNT; survival++) {
		for (partial = 0; partial < 2; partial++) {
			for (cut = 1; cut <= operations; cut++) {
				device_reset(device, device->base);
				device->stop_at = cut;
				device->survival = (enum survival)survival;
				device->partial = partial != 0;
				EXPECT(run_transaction(device), EXT4_IO);
				CHECK(device->off && device->operations == cut);
				device_power_on(device);
				if (cut == home_event && survival == DROP_VOLATILE &&
				    partial == 0) {
					memcpy(device->pending, device->stable, device->size);
					export_pending(device, directory);
				}
				error =
				    ext4_recover(&device->environment, &device->writer, &report);
				if (error != EXT4_OK) {
					/* A torn primary superblock lacks its checksum. Recovery
					 * must fail closed, never guess or claim the disk clean. */
					CHECK(error == EXT4_CORRUPT && partial != 0 &&
					    !events[cut - 1].flush &&
					    events[cut - 1].offset ==
						(EXT4_SUPER_OFFSET / device->block_size) *
						    device->block_size);
					CHECK(device->writes == 0);
					EXPECT(ext4_mount(&device->environment, &fs), EXT4_CORRUPT);
					CHECK(fs == NULL && device->live == 0);
					rejected_controls++;
					continue;
				}
				if (cut >= home_event) {
					CHECK(check_outcome(device));
				} else {
					(void)check_outcome(device);
				}
				check_clean(device);
				recovered++;
			}
		}
	}
	printf("PASS power loss: block=%u features=%u cases=%u recovered=%u "
	       "torn-control-rejected=%u\n",
	    device->block_size, device->profile, operations * SURVIVAL_COUNT * 2, recovered,
	    rejected_controls);
}

static void
test_recovery_faults(struct device *device)
{
	struct ext4_recovery_report report;
	uint64_t allocations;
	uint64_t reads;
	uint64_t point;
	uint32_t operations;
	uint32_t cut;
	enum ext4_result error;

	device_reset(device, device->pending);
	EXPECT(ext4_recover(&device->environment, &device->writer, &report), EXT4_OK);
	CHECK(report.transactions == 1 && report.replayed_blocks == TEST_TARGETS);
	operations = device->operations;
	allocations = device->allocations;
	reads = device->reads;
	for (cut = 1; cut <= operations; cut++) {
		device_reset(device, device->pending);
		device->stop_at = cut;
		device->survival = PERSIST_ALTERNATE;
		EXPECT(ext4_recover(&device->environment, &device->writer, &report), EXT4_IO);
		CHECK(device->live == 0);
		device_power_on(device);
		EXPECT(ext4_recover(&device->environment, &device->writer, &report), EXT4_OK);
		CHECK(check_outcome(device));
		check_clean(device);
	}
	/* Cover initial ownership, mapper allocation, record allocation and final
	 * validation. Identical per-block mapper allocations use the same path. */
	for (point = 1; point <= allocations; point++) {
		if (point > 12 && point + 12 < allocations) {
			continue;
		}
		device_reset(device, device->pending);
		device->fail_allocation = point;
		error = ext4_recover(&device->environment, &device->writer, &report);
		CHECK(error == EXT4_NO_MEMORY && device->live == 0);
		device_power_on(device);
		EXPECT(ext4_recover(&device->environment, &device->writer, &report), EXT4_OK);
		CHECK(check_outcome(device));
	}
	for (point = 1; point <= reads; point++) {
		if (point > 8 && point + 16 < reads) {
			continue;
		}
		device_reset(device, device->pending);
		device->fail_read = point;
		EXPECT(ext4_recover(&device->environment, &device->writer, &report), EXT4_IO);
		CHECK(device->live == 0);
		device_power_on(device);
		EXPECT(ext4_recover(&device->environment, &device->writer, &report), EXT4_OK);
		CHECK(check_outcome(device));
	}
	printf("PASS recovery: interrupted replay, allocation/read failures, retry, resource "
	       "balance\n");
}

static void
test_corruption(struct device *device)
{
	struct ext4_recovery_report report;
	uint64_t offset;
	uint32_t logical;
	uint32_t index;

	if (!(device->profile & (EXT4_JBD_CSUM_V2 | EXT4_JBD_CSUM_V3))) {
		return;
	}
	for (index = 0; index < 2; index++) {
		device_reset(device, device->pending);
		logical = device->journal_first + index;
		offset = device->journal_map[logical] * device->block_size;
		/* Preserve the header so a valid commit must expose corruption. */
		device->cache[offset + sizeof(struct ext4_jbd_header) + 1] ^= 1;
		memcpy(device->stable, device->cache, device->size);
		EXPECT(ext4_recover(&device->environment, &device->writer, &report), EXT4_CORRUPT);
		CHECK(device->writes == 0 && device->live == 0);
	}
	device_reset(device, device->pending);
	offset = device->commit_offset + offsetof(struct ext4_jbd_commit, checksum);
	device->cache[offset] ^= 1;
	memcpy(device->stable, device->cache, device->size);
	EXPECT(ext4_recover(&device->environment, &device->writer, &report), EXT4_OK);
	CHECK(report.transactions == 0 && report.discarded_tail);
	CHECK(!check_outcome(device));
	check_clean(device);
	printf("PASS checksums: committed corruption rejected; incomplete commit discarded\n");
}

static void
test_checksum_v1(struct device *device)
{
	struct ext4_recovery_report report;
	struct ext4_jbd_commit *commit;
	uint64_t offset;
	uint32_t index;
	bool async = (device->profile & EXT4_JBD_ASYNC_COMMIT) != 0;

	if (!device->checksum_v1) {
		return;
	}
	for (index = 0; index < 6; index++) {
		device_reset(device, device->pending);
		commit = (struct ext4_jbd_commit *)(device->cache + device->commit_offset);
		CHECK(commit->checksum_type == EXT4_JBD_CRC32 &&
		    commit->checksum_size == sizeof(commit->checksum[0]));
		if (index < 2) {
			offset =
			    device->journal_map[device->journal_first + index] * device->block_size;
			device->cache[offset + device->block_size - 1U] ^= 1;
		} else if (index == 2) {
			commit->checksum[0].bytes[0] ^= 1;
		} else if (index == 3) {
			commit->checksum_type = EXT4_JBD_CRC32C;
		} else if (index == 4) {
			commit->checksum_size--;
		} else {
			commit->checksum_type = commit->checksum_size = 0;
			ext4_encode_be32(&commit->checksum[0], 0);
		}
		memcpy(device->stable, device->cache, device->size);
		EXPECT(ext4_recover(&device->environment, &device->writer, &report),
		    index == 5 || async ? EXT4_OK : EXT4_CORRUPT);
		CHECK(device->live == 0);
		if (index != 5 && !async) {
			CHECK(device->writes == 0);
		} else if (index != 5) {
			CHECK(report.transactions == 0 && report.discarded_tail &&
			    !check_outcome(device));
			check_clean(device);
		} else {
			CHECK(report.transactions == 1 && check_outcome(device));
			check_clean(device);
		}
	}
	puts(
	    "PASS transactional checksum: descriptor/data/commit corruption and legacy transition");
}

static void
test_malformed_records(struct device *device)
{
	struct ext4_recovery_report report;
	struct ext4_jbd_super *super;
	struct ext4_jbd_tag *tag;
	struct ext4_jbd_tag3 *tag3;
	struct ext4_jbd_commit *commit;
	struct ext4_be32 *tail;
	uint8_t *descriptor;
	size_t tag_size;
	size_t last_offset;
	uint32_t checksum;
	uint32_t flags;
	uint32_t variant;
	uint32_t block;
	bool checksum_enabled;

	checksum_enabled = (device->profile & (EXT4_JBD_CSUM_V2 | EXT4_JBD_CSUM_V3)) != 0;
	tag_size = (device->profile & EXT4_JBD_CSUM_V3) ? sizeof(struct ext4_jbd_tag3)
							: sizeof(struct ext4_jbd_tag) +
		((device->profile & EXT4_JBD_64BIT) ? sizeof(struct ext4_be32) : 0) +
		((device->profile & EXT4_JBD_CSUM_V2) ? sizeof(struct ext4_be16) : 0);
	last_offset = sizeof(struct ext4_jbd_header) + tag_size + EXT4_UUID_SIZE;
	for (variant = 0; variant < 4; variant++) {
		device_reset(device, device->pending);
		descriptor =
		    device->cache + device->journal_map[device->journal_first] * device->block_size;
		tag = (struct ext4_jbd_tag *)(descriptor + sizeof(struct ext4_jbd_header));
		tag3 = (struct ext4_jbd_tag3 *)tag;
		if (variant == 0) {
			ext4_encode_be32(&tag->block_lo, (uint32_t)device->journal_map[0]);
		} else if (variant == 1) {
			ext4_encode_be32(&tag->block_lo, device->blocks);
		} else if (device->profile & EXT4_JBD_CSUM_V3) {
			if (variant == 2) {
				tag3 = (struct ext4_jbd_tag3 *)(descriptor + last_offset);
			}
			flags = ext4_be32(&tag3->flags);
			ext4_encode_be32(&tag3->flags,
			    variant == 2 ? flags & ~EXT4_JBD_LAST_TAG
					 : flags | TEST_UNKNOWN_TAG_FLAG);
		} else {
			if (variant == 2) {
				tag = (struct ext4_jbd_tag *)(descriptor + last_offset);
			}
			flags = ext4_be16(&tag->flags);
			ext4_encode_be16(&tag->flags,
			    (uint16_t)(variant == 2 ? flags & ~EXT4_JBD_LAST_TAG
						    : flags | TEST_UNKNOWN_TAG_FLAG));
		}
		if (checksum_enabled) {
			super = (struct ext4_jbd_super *)(device->cache +
			    device->journal_map[0] * device->block_size);
			tail =
			    (struct ext4_be32 *)(descriptor + device->block_size - sizeof(*tail));
			ext4_encode_be32(tail, 0);
			checksum = ext4_crc32c(UINT32_MAX, super->uuid, sizeof(super->uuid));
			checksum = ext4_crc32c(checksum, descriptor, device->block_size);
			ext4_encode_be32(tail, checksum);
		}
		if (device->checksum_v1) {
			checksum = UINT32_MAX;
			for (block = 0; block <= TEST_TARGETS; block++) {
				checksum = ext4_crc32_be(checksum,
				    device->cache +
					device->journal_map[device->journal_first + block] *
					    device->block_size,
				    device->block_size);
			}
			commit = (struct ext4_jbd_commit *)(device->cache + device->commit_offset);
			ext4_encode_be32(&commit->checksum[0], checksum);
		}
		memcpy(device->stable, device->cache, device->size);
		EXPECT(ext4_recover(&device->environment, &device->writer, &report), EXT4_CORRUPT);
		CHECK(device->writes == 0 && device->live == 0);
	}
	for (variant = 0; variant < 6; variant++) {
		device_reset(device, device->pending);
		super = (struct ext4_jbd_super *)(device->cache +
		    device->journal_map[0] * device->block_size);
		if (variant == 0) {
			ext4_encode_be32(&super->feature_incompat,
			    device->profile | TEST_UNKNOWN_JOURNAL_FEATURE);
		} else if (variant == 1) {
			ext4_encode_be32(&super->max_length, device->journal_blocks + 1);
		} else if (variant == 2) {
			ext4_encode_be32(&super->start, device->journal_blocks);
		} else if (variant < 5) {
			ext4_encode_be32(&super->feature_compat, EXT4_JBD_COMPAT_CHECKSUM);
			ext4_encode_be32(&super->feature_incompat,
			    variant == 3 ? EXT4_JBD_CSUM_V2 : EXT4_JBD_CSUM_V3);
		} else {
			ext4_encode_be32(&super->feature_compat, 0);
			ext4_encode_be32(&super->feature_incompat, EXT4_JBD_ASYNC_COMMIT);
		}
		journal_super_checksum(super);
		memcpy(device->stable, device->cache, device->size);
		EXPECT(ext4_recover(&device->environment, &device->writer, &report),
		    variant == 0 || variant > 2 ? EXT4_UNSUPPORTED : EXT4_CORRUPT);
		CHECK(device->writes == 0 && device->live == 0);
	}
	puts("PASS valid-checksum malformed records: targets, missing last tag, flags, features, "
	     "geometry");
}

static void
test_wrap(struct device *device)
{
	struct ext4_recovery_report report;
	struct ext4_jbd_super *super;
	uint8_t *saved;
	uint32_t slots[4];
	uint32_t block;
	uint32_t index;

	device_reset(device, device->pending);
	saved = malloc((size_t)device->block_size * 4);
	CHECK(saved != NULL);
	for (index = 0; index < 4; index++) {
		memcpy(saved + (size_t)index * device->block_size,
		    device->cache +
			device->journal_map[device->journal_first + index] * device->block_size,
		    device->block_size);
	}
	for (block = device->journal_first; block < device->journal_blocks; block++) {
		memset(device->cache + device->journal_map[block] * device->block_size, 0,
		    device->block_size);
	}
	slots[0] = device->journal_blocks - 2;
	slots[1] = device->journal_blocks - 1;
	slots[2] = device->journal_first;
	slots[3] = device->journal_first + 1;
	for (index = 0; index < 4; index++) {
		memcpy(device->cache + device->journal_map[slots[index]] * device->block_size,
		    saved + (size_t)index * device->block_size, device->block_size);
	}
	free(saved);
	super =
	    (struct ext4_jbd_super *)(device->cache + device->journal_map[0] * device->block_size);
	ext4_encode_be32(&super->start, slots[0]);
	journal_super_checksum(super);
	device->commit_offset = device->journal_map[slots[3]] * device->block_size;
	memcpy(device->stable, device->cache, device->size);
	EXPECT(ext4_recover(&device->environment, &device->writer, &report), EXT4_OK);
	CHECK(report.transactions == 1 && check_outcome(device));
	check_clean(device);
	printf("PASS journal ring wrap and transaction sequence wrap\n");
}

static void
test_multiple_descriptors(struct device *device)
{
	struct ext4_fs *fs;
	struct ext4_journal *journal;
	struct ext4_transaction *transaction;
	struct ext4_recovery_report report;
	struct ext4_inode root;
	struct ext4_inode file;
	uint64_t targets[128];
	uint8_t *expected;
	void *buffer;
	uint32_t phase;
	uint32_t index;
	uint32_t home_event = 0;

	if (device->block_size != EXT4_MIN_BLOCK_SIZE) {
		return;
	}
	expected = malloc(device->block_size);
	CHECK(expected != NULL);
	for (phase = 0; phase < 2; phase++) {
		device_reset(device, device->base);
		if (phase != 0) {
			device->stop_at = home_event;
			device->survival = DROP_VOLATILE;
		}
		EXPECT(ext4_mount(&device->environment, &fs), EXT4_OK);
		EXPECT(ext4_get_inode(fs, EXT4_ROOT_INODE, &root), EXT4_OK);
		EXPECT(ext4_lookup(
			   fs, &root, (const uint8_t *)"payload.bin", strlen("payload.bin"), &file),
		    EXT4_OK);
		EXPECT(ext4_journal_open(fs, &device->writer, &journal), EXT4_OK);
		EXPECT(ext4_transaction_begin(journal, 128, &transaction), EXT4_OK);
		for (index = 0; index < 128; index++) {
			EXPECT(ext4_map_block(fs, &file, index, &targets[index]), EXT4_OK);
			CHECK(targets[index] != 0);
			EXPECT(
			    ext4_transaction_buffer(transaction, targets[index], &buffer), EXT4_OK);
			fill_target(buffer, device->block_size, index);
		}
		EXPECT(ext4_transaction_commit(transaction), phase == 0 ? EXT4_OK : EXT4_IO);
		if (phase == 0) {
			home_event = device->home_event;
			EXPECT(ext4_journal_finish(journal), EXT4_OK);
		}
		ext4_journal_close(journal);
		ext4_unmount(fs);
		if (phase != 0) {
			device_power_on(device);
			EXPECT(
			    ext4_recover(&device->environment, &device->writer, &report), EXT4_OK);
			CHECK(report.transactions == 1 && report.replayed_blocks == 128);
		}
		for (index = 0; index < 128; index++) {
			fill_target(expected, device->block_size, index);
			CHECK(memcmp(expected, device->stable + targets[index] * device->block_size,
				  device->block_size) == 0);
		}
		check_clean(device);
	}
	free(expected);
	printf("PASS multi-descriptor transaction: 128 snapshots, commit and interrupted "
	       "checkpoint\n");
}

static void
test_ownership(struct device *device)
{
	struct ext4_fs *fs;
	struct ext4_journal *journal;
	struct ext4_journal *other;
	struct ext4_transaction *transaction;
	struct ext4_transaction *second;
	void *buffer;

	device_reset(device, device->base);
	EXPECT(ext4_mount(&device->environment, &fs), EXT4_OK);
	EXPECT(ext4_journal_open(fs, &device->writer, &journal), EXT4_OK);
	EXPECT(ext4_journal_open(fs, &device->writer, &other), EXT4_INVALID_ARGUMENT);
	CHECK(other == NULL);
	EXPECT(ext4_transaction_begin(journal, 0, &transaction), EXT4_INVALID_ARGUMENT);
	EXPECT(ext4_transaction_begin(journal, EXT4_TRANSACTION_MAX_BLOCKS + 1, &transaction),
	    EXT4_INVALID_ARGUMENT);
	EXPECT(ext4_transaction_begin(journal, 1, &transaction), EXT4_OK);
	EXPECT(ext4_transaction_begin(journal, 1, &second), EXT4_INVALID_ARGUMENT);
	CHECK(second == NULL);
	EXPECT(ext4_journal_finish(journal), EXT4_INVALID_ARGUMENT);
	EXPECT(ext4_transaction_buffer(transaction, device->journal_map[0], &buffer),
	    EXT4_INVALID_ARGUMENT);
	EXPECT(ext4_transaction_buffer(transaction, fs->first_data_block, &buffer),
	    EXT4_INVALID_ARGUMENT);
	EXPECT(
	    ext4_transaction_buffer(transaction, fs->info.blocks, &buffer), EXT4_INVALID_ARGUMENT);
	EXPECT(ext4_transaction_buffer(transaction, device->target[0], &buffer), EXT4_OK);
	EXPECT(ext4_transaction_buffer(transaction, device->target[1], &buffer), EXT4_RANGE);
	ext4_transaction_cancel(transaction);
	EXPECT(ext4_transaction_begin(journal, 1, &transaction), EXT4_OK);
	EXPECT(ext4_transaction_commit(transaction), EXT4_OK);
	EXPECT(ext4_journal_finish(journal), EXT4_OK);
	ext4_journal_close(journal);
	ext4_unmount(fs);
	CHECK(device->writes == 0 && device->live == 0);
	printf("PASS transaction ownership, cancellation, credits and protected ranges\n");
}

#include "journal_revoke.h"
#include "journal_async.h"

int
main(int argc, char **argv)
{
	static struct device device;
	static const uint32_t profiles[] = { 0, EXT4_JBD_CSUM_V2, EXT4_JBD_CSUM_V3,
		EXT4_JBD_CSUM_V2 | EXT4_JBD_64BIT, EXT4_JBD_CSUM_V3 | EXT4_JBD_64BIT };
	static const uint32_t v1_profiles[] = { 0, EXT4_JBD_64BIT };
	const uint32_t *selected = profiles;
	const char *directory;
	size_t count = sizeof(profiles) / sizeof(profiles[0]);
	size_t index;
	bool checksum_v1 = false;
	bool export_only = false;
	bool async = false;
	int argument = 1;

	if (argument < argc && strcmp(argv[argument], "--checksum-v1") == 0) {
		checksum_v1 = true;
		selected = v1_profiles;
		count = sizeof(v1_profiles) / sizeof(v1_profiles[0]);
		argument++;
	}
	if (argument < argc && strcmp(argv[argument], "--async") == 0) {
		async = true;
		if (!checksum_v1) {
			selected++;
			count--;
		}
		argument++;
	}
	if (argument < argc && strcmp(argv[argument], "--export-only") == 0) {
		export_only = true;
		argument++;
	}
	if (argc < argument + 1 || argc > argument + 2 || (export_only && argc != argument + 2)) {
		fprintf(stderr,
		    "usage: %s [--checksum-v1] [--async] [--export-only] IMAGE "
		    "[NEW_EXPORT_DIRECTORY]\n",
		    argv[0]);
		return 2;
	}
	setvbuf(stdout, NULL, _IOLBF, 0);
	directory = argc == argument + 2 ? argv[argument + 1] : NULL;
	device_initialize(&device, argv[argument]);
	for (index = 0; index < count; index++) {
		device_profile(&device,
		    selected[index] | EXT4_JBD_REVOKE_FEATURE | (async ? EXT4_JBD_ASYNC_COMMIT : 0),
		    checksum_v1);
		test_power_loss(&device, directory, export_only);
		if (export_only) {
			continue;
		}
		test_ownership(&device);
		test_recovery_faults(&device);
		test_corruption(&device);
		test_checksum_v1(&device);
		test_malformed_records(&device);
		test_revoke_advertisement(&device);
		test_async_tail(&device);
		test_wrap(&device);
		test_multiple_descriptors(&device);
	}
	free(device.original);
	free(device.base);
	free(device.stable);
	free(device.cache);
	free(device.pending);
	free(device.dirty);
	free(device.journal_map);
	puts(export_only ? "PASS exported committed journals" : "PASS journal durability suite");
	return 0;
}

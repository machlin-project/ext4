/* SPDX-License-Identifier: BSD-3-Clause */
#include "journal.h"
#include "image.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DEVICE_LIMIT (128U * 1024U * 1024U)
#define TEST_SECONDS 1700000700
#define UNKNOWN_FEATURE 0x80000000U
#define CHECK(expression)                                                                          \
	do {                                                                                       \
		if (!(expression)) {                                                               \
			fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #expression);           \
			exit(1);                                                                   \
		}                                                                                  \
	} while (0)
#define EXPECT(expression, expected) result_is((expression), (expected), #expression, __LINE__)

struct pair;

struct device {
	struct pair *pair;
	uint8_t *base;
	uint8_t *cache;
	uint8_t *stable;
	uint8_t *dirty;
	size_t size;
	uint32_t blocks;
	uint32_t reads;
	uint32_t fail_read;
	unsigned int survival;
	bool log;
};

struct pair {
	struct device home;
	struct device log;
	struct ext4_environment environment;
	struct ext4_write_environment writer;
	struct ext4_journal_environment external;
	uint32_t block_size;
	uint32_t events;
	uint32_t writes;
	uint32_t stop_at;
	uint32_t allocations;
	uint32_t fail_allocation;
	uint32_t live;
	uint32_t commit_write;
	uint32_t commit_flush;
	bool partial;
	bool off;
};

static const uint8_t old_bytes[] = "Machlin ext4\n";
static const uint8_t new_bytes[] = "Journal ext4\n";

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
allocate(void *context, size_t size)
{
	struct pair *pair = context;
	void *buffer;

	if (++pair->allocations == pair->fail_allocation) {
		return NULL;
	}
	buffer = malloc(size);
	if (buffer != NULL) {
		pair->live++;
	}
	return buffer;
}

static void
release(void *context, void *buffer, size_t size)
{
	struct pair *pair = context;

	(void)size;
	CHECK(buffer != NULL && pair->live != 0);
	pair->live--;
	free(buffer);
}

static enum ext4_result
read_device(void *context, uint64_t offset, void *buffer, size_t length)
{
	struct device *device = context;

	CHECK(offset <= device->size && length <= device->size - offset);
	if (device->pair->off || ++device->reads == device->fail_read) {
		return EXT4_IO;
	}
	memcpy(buffer, device->cache + offset, length);
	return EXT4_OK;
}

static enum ext4_result
read_home(void *context, uint64_t offset, void *buffer, size_t length)
{
	struct pair *pair = context;

	return read_device(&pair->home, offset, buffer, length);
}

static void
persist(struct device *device, unsigned int survival)
{
	uint32_t block;
	uint32_t size = device->pair->block_size;

	for (block = 0; block < device->blocks; block++) {
		if (device->dirty[block] && (survival == 1 || (survival == 2 && (block & 1)))) {
			memcpy(device->stable + (size_t)block * size,
			    device->cache + (size_t)block * size, size);
		}
		device->dirty[block] = 0;
	}
}

static void
power_cut(struct pair *pair)
{
	persist(&pair->home, pair->home.survival);
	persist(&pair->log, pair->log.survival);
	pair->off = true;
}

static enum ext4_result
write_device(void *context, uint64_t offset, const void *buffer, size_t length)
{
	struct device *device = context;
	struct pair *pair = device->pair;
	const struct ext4_jbd_header *header = buffer;
	size_t copied;
	bool cut;

	CHECK(offset <= device->size && length <= device->size - offset);
	CHECK(length != 0 && length % pair->block_size == 0 && offset % pair->block_size == 0);
	if (pair->off) {
		return EXT4_IO;
	}
	pair->writes++;
	cut = ++pair->events == pair->stop_at;
	if (!cut || pair->partial) {
		copied = cut ? length / 2U + (length > pair->block_size ? EXT4_SECTOR_SIZE : 0U)
			     : length;
		memcpy(device->cache + offset, buffer, copied);
		memset(device->dirty + offset / pair->block_size, 1,
		    (copied + pair->block_size - 1U) / pair->block_size);
	}
	if (cut) {
		power_cut(pair);
		return EXT4_IO;
	}
	if (device->log && ext4_be32(&header->magic) == EXT4_JBD_MAGIC &&
	    ext4_be32(&header->type) == EXT4_JBD_COMMIT) {
		pair->commit_write = pair->events;
	}
	return EXT4_OK;
}

static enum ext4_result
flush_device(void *context)
{
	struct device *device = context;
	struct pair *pair = device->pair;

	if (pair->off) {
		return EXT4_IO;
	}
	if (++pair->events == pair->stop_at) {
		power_cut(pair);
		return EXT4_IO;
	}
	persist(device, 1);
	if (device->log && pair->commit_write != 0 && pair->commit_flush == 0) {
		pair->commit_flush = pair->events;
	}
	return EXT4_OK;
}

static void
device_load(struct device *device, struct pair *pair, const char *path, bool log)
{
	struct ext4_posix_image image;

	memset(device, 0, sizeof(*device));
	device->pair = pair;
	device->log = log;
	EXPECT(ext4_posix_open(&image, path), EXT4_OK);
	CHECK(image.environment.size_bytes <= DEVICE_LIMIT);
	device->size = (size_t)image.environment.size_bytes;
	device->base = malloc(device->size);
	device->cache = malloc(device->size);
	device->stable = malloc(device->size);
	CHECK(device->base != NULL && device->cache != NULL && device->stable != NULL);
	EXPECT(image.environment.read(image.environment.context, 0, device->base, device->size),
	    EXT4_OK);
	ext4_posix_close(&image);
	if (!log) {
		pair->block_size = EXT4_MIN_BLOCK_SIZE
		    << ext4_le32(
			   &((const struct ext4_super_disk *)(device->base + EXT4_SUPER_OFFSET))
			       ->log_block_size);
	}
	CHECK(device->size % pair->block_size == 0);
	device->blocks = (uint32_t)(device->size / pair->block_size);
	device->dirty = calloc(device->blocks, 1);
	CHECK(device->dirty != NULL);
}

static void
reset(struct pair *pair)
{
	struct device *devices[] = { &pair->home, &pair->log };
	unsigned int index;

	CHECK(pair->live == 0);
	for (index = 0; index < 2; index++) {
		memcpy(devices[index]->cache, devices[index]->base, devices[index]->size);
		memcpy(devices[index]->stable, devices[index]->base, devices[index]->size);
		memset(devices[index]->dirty, 0, devices[index]->blocks);
		devices[index]->reads = devices[index]->fail_read = devices[index]->survival = 0;
	}
	pair->events = pair->writes = pair->stop_at = 0;
	pair->allocations = pair->fail_allocation = 0;
	pair->commit_write = pair->commit_flush = 0;
	pair->off = pair->partial = false;
	pair->external.size_bytes = pair->log.size;
}

static struct ext4_inode
lookup(struct ext4_fs *fs)
{
	struct ext4_inode root;
	struct ext4_inode inode;

	EXPECT(ext4_get_inode(fs, EXT4_ROOT_INODE, &root), EXT4_OK);
	EXPECT(ext4_lookup(fs, &root, (const uint8_t *)"hello.txt", 9, &inode), EXT4_OK);
	return inode;
}

static enum ext4_result
mutate(struct pair *pair)
{
	struct ext4_fs *fs;
	struct ext4_inode inode;
	struct ext4_inode_update update = { 0 };
	struct ext4_xattr_change attribute = { 0 };
	uint8_t value[300];
	size_t completed;
	enum ext4_result error;

	EXPECT(ext4_mount_writable_with_journal(
		   &pair->environment, &pair->writer, &pair->external, &fs),
	    EXT4_OK);
	inode = lookup(fs);
	memset(value, 'X', sizeof(value));
	attribute.name_index = EXT4_XATTR_USER;
	attribute.name = (const uint8_t *)"transaction";
	attribute.name_length = 11;
	attribute.value = value;
	attribute.value_size = sizeof(value);
	update.fields = EXT4_ATTR_PERMISSIONS | EXT4_ATTR_MODIFY_TIME | EXT4_ATTR_CHANGE_TIME |
	    EXT4_ATTR_XATTRS;
	update.permissions = 0604;
	update.modify_time.seconds = update.change_time.seconds = TEST_SECONDS;
	update.xattrs = &attribute;
	update.xattr_count = 1;
	error = ext4_write(fs, inode.number, inode.generation, 0, new_bytes, sizeof(new_bytes) - 1U,
	    &update, &completed);
	CHECK(completed == (error == EXT4_OK ? sizeof(new_bytes) - 1U : 0));
	if (error == EXT4_OK) {
		error = ext4_sync(fs);
	} else {
		EXPECT(ext4_get_inode(fs, inode.number, &inode), EXT4_RECOVERY_REQUIRED);
	}
	ext4_unmount(fs);
	CHECK(pair->live == 0);
	return error;
}

static bool
verify(struct pair *pair)
{
	struct ext4_fs *fs;
	struct ext4_inode inode;
	uint8_t bytes[sizeof(new_bytes)];
	uint8_t value[300];
	size_t size;
	size_t index;
	bool changed;

	EXPECT(ext4_mount(&pair->environment, &fs), EXT4_OK);
	inode = lookup(fs);
	EXPECT(ext4_read(fs, &inode, 0, bytes, sizeof(bytes), &size), EXT4_OK);
	changed = size == sizeof(new_bytes) - 1U && memcmp(bytes, new_bytes, size) == 0;
	CHECK(changed || (size == sizeof(old_bytes) - 1U && memcmp(bytes, old_bytes, size) == 0));
	if (changed) {
		CHECK((inode.mode & EXT4_MODE_PERMISSIONS) == 0604);
		CHECK(inode.change_time.seconds == TEST_SECONDS &&
		    inode.modify_time.seconds == TEST_SECONDS);
		EXPECT(ext4_get_xattr(fs, inode.number, inode.generation, EXT4_XATTR_USER,
			   (const uint8_t *)"transaction", 11, value, sizeof(value), &size),
		    EXT4_OK);
		CHECK(size == sizeof(value));
		for (index = 0; index < sizeof(value); index++) {
			CHECK(value[index] == 'X');
		}
	} else {
		CHECK((inode.mode & EXT4_MODE_PERMISSIONS) == 0644);
		EXPECT(ext4_get_xattr(fs, inode.number, inode.generation, EXT4_XATTR_USER,
			   (const uint8_t *)"transaction", 11, value, sizeof(value), &size),
		    EXT4_NOT_FOUND);
	}
	ext4_unmount(fs);
	return changed;
}

static void
restart(struct pair *pair)
{
	CHECK(pair->live == 0);
	memcpy(pair->home.cache, pair->home.stable, pair->home.size);
	memcpy(pair->log.cache, pair->log.stable, pair->log.size);
	pair->off = false;
	pair->stop_at = 0;
	pair->partial = false;
	memset(pair->home.dirty, 0, pair->home.blocks);
	memset(pair->log.dirty, 0, pair->log.blocks);
	pair->home.fail_read = pair->log.fail_read = pair->fail_allocation = 0;
}

static void
export_pair(struct pair *pair, const char *prefix, const char *state)
{
	struct device *devices[] = { &pair->home, &pair->log };
	const char *extensions[] = { "img", "journal" };
	char path[4096];
	FILE *file;
	int length;
	unsigned int index;

	for (index = 0; index < 2; index++) {
		length = snprintf(path, sizeof(path), "%s-%s.%s", prefix, state, extensions[index]);
		CHECK(length > 0 && (size_t)length < sizeof(path));
		file = fopen(path, "wb");
		CHECK(file != NULL);
		CHECK(fwrite(devices[index]->stable, 1, devices[index]->size, file) ==
		    devices[index]->size);
		CHECK(fclose(file) == 0);
	}
}

static void
functional(struct pair *pair, const char *prefix)
{
	struct ext4_fs *fs;
	struct ext4_recovery_report report;
	uint32_t before;
	uint32_t after;
	uint32_t writes;
	unsigned int state;

	reset(pair);
	EXPECT(ext4_mount_writable(&pair->environment, &pair->writer, &fs), EXT4_UNSUPPORTED);
	EXPECT(
	    ext4_recover_with_journal(&pair->environment, &pair->writer, &pair->external, &report),
	    EXT4_OK);
	CHECK(pair->writes == 0 && !verify(pair));
	EXPECT(mutate(pair), EXT4_OK);
	CHECK(verify(pair));
	before = pair->commit_write;
	after = pair->commit_flush + 1U;
	CHECK(before != 0 && after > before);
	/* Device control bytes are not journal ring addresses or home blocks. */
	CHECK(memcmp(pair->log.base, pair->log.stable,
		  (EXT4_SUPER_OFFSET / pair->block_size + 1U) * pair->block_size) == 0);
	if (prefix != NULL) {
		export_pair(pair, prefix, "clean");
	}
	for (state = 0; state < 2; state++) {
		reset(pair);
		pair->stop_at = state == 0 ? before : after;
		EXPECT(mutate(pair), EXT4_IO);
		restart(pair);
		EXPECT(ext4_mount(&pair->environment, &fs), EXT4_RECOVERY_REQUIRED);
		if (prefix != NULL) {
			export_pair(pair, prefix, state == 0 ? "before" : "after");
		}
		EXPECT(ext4_recover_with_journal(
			   &pair->environment, &pair->writer, &pair->external, &report),
		    EXT4_OK);
		CHECK(report.transactions == state);
		CHECK(verify(pair) == (state != 0));
		writes = pair->writes;
		EXPECT(ext4_recover_with_journal(
			   &pair->environment, &pair->writer, &pair->external, &report),
		    EXT4_OK);
		CHECK(report.transactions == 0 && pair->writes == writes);
		if (prefix != NULL) {
			export_pair(
			    pair, prefix, state == 0 ? "before-recovered" : "after-recovered");
		}
	}
}

static void
repair_checksums(struct pair *pair)
{
	struct ext4_super_disk *super;
	struct ext4_jbd_super *journal;
	unsigned int device;
	uint8_t *cache;

	for (device = 0; device < 2; device++) {
		cache = device == 0 ? pair->home.cache : pair->log.cache;
		super = (struct ext4_super_disk *)(cache + EXT4_SUPER_OFFSET);
		if (ext4_le32(&super->feature_ro_compat) & EXT4_FEATURE_RO_METADATA_CSUM) {
			ext4_encode32(&super->checksum,
			    ext4_crc32c(
				UINT32_MAX, super, offsetof(struct ext4_super_disk, checksum)));
		}
	}
	journal = (struct ext4_jbd_super *)(pair->log.cache +
	    (EXT4_SUPER_OFFSET / pair->block_size + 1U) * pair->block_size);
	if (journal->checksum_type == EXT4_JBD_CRC32C) {
		ext4_encode_be32(&journal->checksum, 0);
		ext4_encode_be32(
		    &journal->checksum, ext4_crc32c(UINT32_MAX, journal, sizeof(*journal)));
	}
}

static void
malformed(struct pair *pair)
{
	struct ext4_fs *fs;
	struct ext4_super_disk *home;
	struct ext4_super_disk *device;
	struct ext4_jbd_super *journal;
	enum ext4_result expected;
	uint32_t test;
	uint32_t reads[2];
	uint32_t allocations;
	unsigned int index;

	for (test = 0; test < 16; test++) {
		reset(pair);
		home = (struct ext4_super_disk *)(pair->home.cache + EXT4_SUPER_OFFSET);
		device = (struct ext4_super_disk *)(pair->log.cache + EXT4_SUPER_OFFSET);
		journal = (struct ext4_jbd_super *)(pair->log.cache +
		    (EXT4_SUPER_OFFSET / pair->block_size + 1U) * pair->block_size);
		expected = EXT4_CORRUPT;
		switch (test) {
		case 0:
			home->journal_uuid[0] ^= 1;
			break;
		case 1:
			device->uuid[0] ^= 1;
			break;
		case 2:
			journal->uuid[0] ^= 1;
			break;
		case 3:
			journal->user_ids[0] ^= 1;
			break;
		case 4:
			ext4_encode_be32(&journal->users, 2);
			expected = EXT4_UNSUPPORTED;
			break;
		case 5:
			ext4_encode_be32(&journal->users, 0);
			expected = EXT4_UNSUPPORTED;
			break;
		case 6:
			ext4_encode_be32(
			    &journal->first, EXT4_SUPER_OFFSET / pair->block_size + 1U);
			break;
		case 7:
			ext4_encode_be32(&journal->max_length, pair->log.blocks + 1U);
			break;
		case 8:
			ext4_encode_be32(&journal->block_size, pair->block_size * 2U);
			break;
		case 9:
			ext4_encode_be32(&journal->feature_incompat, UNKNOWN_FEATURE);
			expected = EXT4_UNSUPPORTED;
			break;
		case 10:
			ext4_encode32(&device->feature_incompat, 0);
			expected = EXT4_UNSUPPORTED;
			break;
		case 11:
			ext4_encode32(&device->log_block_size, 31);
			break;
		case 12:
			pair->external.size_bytes--;
			break;
		case 13:
			ext4_encode_be32(&journal->start, 1);
			break;
		case 14:
			ext4_encode16(&device->magic, 0);
			break;
		case 15:
			ext4_encode32(&home->journal_inode, EXT4_ROOT_INODE);
			expected = EXT4_INVALID_ARGUMENT;
			break;
		}
		repair_checksums(pair);
		EXPECT(ext4_mount_writable_with_journal(
			   &pair->environment, &pair->writer, &pair->external, &fs),
		    expected);
		CHECK(fs == NULL && pair->live == 0 && pair->writes == 0);
	}
	reset(pair);
	EXPECT(ext4_mount_writable_with_journal(
		   &pair->environment, &pair->writer, &pair->external, &fs),
	    EXT4_OK);
	reads[0] = pair->home.reads;
	reads[1] = pair->log.reads;
	allocations = pair->allocations;
	ext4_unmount(fs);
	for (index = 0; index < 2; index++) {
		for (test = 1; test <= reads[index]; test++) {
			reset(pair);
			(index == 0 ? &pair->home : &pair->log)->fail_read = test;
			EXPECT(ext4_mount_writable_with_journal(
				   &pair->environment, &pair->writer, &pair->external, &fs),
			    EXT4_IO);
			CHECK(fs == NULL && pair->live == 0 && pair->writes == 0);
		}
	}
	for (test = 1; test <= allocations; test++) {
		reset(pair);
		pair->fail_allocation = test;
		EXPECT(ext4_mount_writable_with_journal(
			   &pair->environment, &pair->writer, &pair->external, &fs),
		    EXT4_NO_MEMORY);
		CHECK(fs == NULL && pair->live == 0 && pair->writes == 0);
	}
	printf("PASS 16 malformed associations, %u read failures, %u allocation failures\n",
	    reads[0] + reads[1], allocations);
}

static void
faults(struct pair *pair)
{
	struct ext4_recovery_report report;
	const struct ext4_super_disk *super;
	uint32_t events;
	uint32_t stop;
	uint32_t cuts = 0;
	uint32_t torn = 0;
	unsigned int home;
	unsigned int log;
	unsigned int partial;
	bool committed;
	enum ext4_result error;

	reset(pair);
	EXPECT(mutate(pair), EXT4_OK);
	events = pair->events;
	for (stop = 1; stop <= events; stop++) {
		for (home = 0; home < 3; home++) {
			for (log = 0; log < 3; log++) {
				for (partial = 0; partial < 2; partial++) {
					reset(pair);
					pair->stop_at = stop;
					pair->home.survival = home;
					pair->log.survival = log;
					pair->partial = partial != 0;
					EXPECT(mutate(pair), EXT4_IO);
					committed = pair->commit_flush != 0;
					restart(pair);
					error = ext4_recover_with_journal(&pair->environment,
					    &pair->writer, &pair->external, &report);
					if (error == EXT4_OK) {
						CHECK(verify(pair) || !committed);
					} else {
						super = (const struct ext4_super_disk
							*)(pair->home.cache + EXT4_SUPER_OFFSET);
						CHECK(error == EXT4_CORRUPT && partial != 0 &&
						    (ext4_le32(&super->feature_ro_compat) &
							EXT4_FEATURE_RO_METADATA_CSUM) &&
						    ext4_crc32c(UINT32_MAX, super,
							offsetof(
							    struct ext4_super_disk, checksum)) !=
							ext4_le32(&super->checksum));
						torn++;
					}
					CHECK(pair->live == 0);
					cuts++;
				}
			}
		}
	}
	printf(
	    "PASS %u cross-device power cuts; %u torn primary superblocks rejected\n", cuts, torn);
}

static void
recovery_faults(struct pair *pair)
{
	struct ext4_recovery_report report;
	struct device *devices[] = { &pair->home, &pair->log };
	uint8_t *original[] = { pair->home.base, pair->log.base };
	const struct ext4_super_disk *super;
	uint32_t after;
	uint32_t events;
	uint32_t reads[2];
	uint32_t allocations;
	uint32_t stop;
	uint32_t cuts = 0;
	uint32_t torn = 0;
	unsigned int index;
	unsigned int home;
	unsigned int log;
	unsigned int partial;
	enum ext4_result error;

	reset(pair);
	EXPECT(mutate(pair), EXT4_OK);
	after = pair->commit_flush + 1U;
	reset(pair);
	pair->stop_at = after;
	EXPECT(mutate(pair), EXT4_IO);
	for (index = 0; index < 2; index++) {
		devices[index]->base = malloc(devices[index]->size);
		CHECK(devices[index]->base != NULL);
		memcpy(devices[index]->base, devices[index]->stable, devices[index]->size);
	}
	reset(pair);
	EXPECT(
	    ext4_recover_with_journal(&pair->environment, &pair->writer, &pair->external, &report),
	    EXT4_OK);
	events = pair->events;
	reads[0] = pair->home.reads;
	reads[1] = pair->log.reads;
	allocations = pair->allocations;
	CHECK(report.transactions == 1 && verify(pair));
	for (stop = 1; stop <= events; stop++) {
		for (home = 0; home < 3; home++) {
			for (log = 0; log < 3; log++) {
				for (partial = 0; partial < 2; partial++) {
					reset(pair);
					pair->stop_at = stop;
					pair->home.survival = home;
					pair->log.survival = log;
					pair->partial = partial != 0;
					EXPECT(ext4_recover_with_journal(&pair->environment,
						   &pair->writer, &pair->external, &report),
					    EXT4_IO);
					restart(pair);
					error = ext4_recover_with_journal(&pair->environment,
					    &pair->writer, &pair->external, &report);
					if (error == EXT4_OK) {
						CHECK(verify(pair));
					} else {
						super = (const struct ext4_super_disk
							*)(pair->home.cache + EXT4_SUPER_OFFSET);
						CHECK(error == EXT4_CORRUPT && partial != 0 &&
						    (ext4_le32(&super->feature_ro_compat) &
							EXT4_FEATURE_RO_METADATA_CSUM) &&
						    ext4_crc32c(UINT32_MAX, super,
							offsetof(
							    struct ext4_super_disk, checksum)) !=
							ext4_le32(&super->checksum));
						torn++;
					}
					CHECK(pair->live == 0);
					cuts++;
				}
			}
		}
	}
	for (index = 0; index < 2; index++) {
		for (stop = 1; stop <= reads[index]; stop++) {
			reset(pair);
			devices[index]->fail_read = stop;
			EXPECT(ext4_recover_with_journal(
				   &pair->environment, &pair->writer, &pair->external, &report),
			    EXT4_IO);
			CHECK(pair->live == 0);
			restart(pair);
			EXPECT(ext4_recover_with_journal(
				   &pair->environment, &pair->writer, &pair->external, &report),
			    EXT4_OK);
			CHECK(verify(pair));
		}
	}
	for (stop = 1; stop <= allocations; stop++) {
		reset(pair);
		pair->fail_allocation = stop;
		EXPECT(ext4_recover_with_journal(
			   &pair->environment, &pair->writer, &pair->external, &report),
		    EXT4_NO_MEMORY);
		CHECK(pair->live == 0);
		restart(pair);
		EXPECT(ext4_recover_with_journal(
			   &pair->environment, &pair->writer, &pair->external, &report),
		    EXT4_OK);
		CHECK(verify(pair));
	}
	for (index = 0; index < 2; index++) {
		free(devices[index]->base);
		devices[index]->base = original[index];
	}
	printf("PASS %u interrupted recoveries; %u torn superblocks; %u read failures; %u "
	       "allocation failures\n",
	    cuts, torn, reads[0] + reads[1], allocations);
}

int
main(int argc, char **argv)
{
	struct pair pair = { 0 };
	struct device *device;
	const char *prefix = NULL;
	bool fault_test = false;
	bool linux_return = false;
	int first = 1;
	unsigned int index;

	if (argc == 4 && strcmp(argv[1], "--faults") == 0) {
		fault_test = true;
		first++;
	} else if (argc == 5 && strcmp(argv[1], "--export") == 0) {
		prefix = argv[4];
		first++;
	} else if (argc == 5 && strcmp(argv[1], "--return") == 0) {
		linux_return = true;
		prefix = argv[4];
		first++;
	} else if (argc != 3) {
		fprintf(stderr,
		    "usage: %s [--faults] FILESYSTEM JOURNAL | --export FILESYSTEM JOURNAL "
		    "PREFIX | --return FILESYSTEM JOURNAL PREFIX\n",
		    argv[0]);
		return 2;
	}
	device_load(&pair.home, &pair, argv[first], false);
	device_load(&pair.log, &pair, argv[first + 1], true);
	pair.environment =
	    (struct ext4_environment){ &pair, pair.home.size, read_home, allocate, release };
	pair.writer =
	    (struct ext4_write_environment){ &pair.home, write_device, flush_device, NULL };
	pair.external = (struct ext4_journal_environment){ &pair.log, pair.log.size, read_device,
		write_device, flush_device };
	if (linux_return) {
		reset(&pair);
		EXPECT(mutate(&pair), EXT4_OK);
		CHECK(verify(&pair));
		export_pair(&pair, prefix, "returned");
		puts("PASS core mutation of Linux-authored paired devices");
	} else if (fault_test) {
		faults(&pair);
		recovery_faults(&pair);
	} else {
		functional(&pair, prefix);
		if (prefix == NULL) {
			malformed(&pair);
		}
		puts("PASS external journal transactions, paired recovery and idempotence");
	}
	CHECK(pair.live == 0);
	for (index = 0; index < 2; index++) {
		device = index == 0 ? &pair.home : &pair.log;
		free(device->base);
		free(device->cache);
		free(device->stable);
		free(device->dirty);
	}
	return 0;
}

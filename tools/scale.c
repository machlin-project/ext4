/* SPDX-License-Identifier: BSD-3-Clause */
/* Scale measurements on an in-memory copy of an image: large sequential and
 * random writes, directory growth and lookup, allocation in fragmented free space,
 * large truncation and offline reclamation. Each workload prints one JSON object
 * with elapsed time, device reads, writes and flushes, allocator calls, the peak
 * of live core allocations, write amplification and the resulting extent count.
 * Device flushes cost nothing here, so times measure the core, not the medium.
 * The resulting image can be saved for a filesystem check. */
#include <ext4/ext4.h>

#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define SCALE_SECONDS 1700009000
#define MEBIBYTE (1024U * 1024U)
#define KIBIBYTE 1024U
#define NAME_BYTES 32U
#define DIRECTORY_BATCH 10000U
#define LOOKUP_SAMPLES 1000U
#define OVERWRITES 1000U
#define OVERWRITE_BYTES (64U * KIBIBYTE)
#define PIECE_BYTES (4U * KIBIBYTE)
#define FRAGMENTED_MEBIBYTES 64U
#define RANDOM_MULTIPLIER UINT64_C(6364136223846793005)
#define RANDOM_INCREMENT UINT64_C(1442695040888963407)
#define NANOSECONDS_PER_MILLISECOND 1000000.0

struct device {
	struct ext4_environment environment;
	struct ext4_write_environment writer;
	uint8_t *bytes;
	size_t size;
	uint64_t reads;
	uint64_t read_bytes;
	uint64_t writes;
	uint64_t write_bytes;
	uint64_t flushes;
	uint64_t allocations;
	uint64_t live_bytes;
	uint64_t peak_bytes;
};

struct sample {
	uint64_t started;
	uint64_t reads;
	uint64_t read_bytes;
	uint64_t writes;
	uint64_t write_bytes;
	uint64_t flushes;
	uint64_t allocations;
};

static const struct ext4_timestamp scale_time = { SCALE_SECONDS, 0 };
static uint64_t random_state = UINT64_C(0x853c49e6748fea9b);

static void
require(bool condition, const char *operation)
{
	if (!condition) {
		fprintf(stderr, "%s\n", operation);
		exit(EXIT_FAILURE);
	}
}

static void
expect(enum ext4_result result, const char *operation)
{
	if (result != EXT4_OK) {
		fprintf(stderr, "%s: %s\n", operation, ext4_result_string(result));
		exit(EXIT_FAILURE);
	}
}

static uint64_t
now(void)
{
	struct timespec time;

	require(clock_gettime(CLOCK_MONOTONIC, &time) == 0, "monotonic clock failed");
	return (uint64_t)time.tv_sec * UINT64_C(1000000000) + (uint64_t)time.tv_nsec;
}

static uint32_t
next_random(void)
{
	random_state = random_state * RANDOM_MULTIPLIER + RANDOM_INCREMENT;
	return (uint32_t)(random_state >> 33);
}

static enum ext4_result
device_read(void *context, uint64_t offset, void *buffer, size_t length)
{
	struct device *device = context;

	if (offset > device->size || length > device->size - offset) {
		return EXT4_IO;
	}
	device->reads++;
	device->read_bytes += length;
	memcpy(buffer, device->bytes + offset, length);
	return EXT4_OK;
}

static enum ext4_result
device_write(void *context, uint64_t offset, const void *buffer, size_t length)
{
	struct device *device = context;

	if (offset > device->size || length > device->size - offset) {
		return EXT4_IO;
	}
	device->writes++;
	device->write_bytes += length;
	memcpy(device->bytes + offset, buffer, length);
	return EXT4_OK;
}

static enum ext4_result
device_flush(void *context)
{
	((struct device *)context)->flushes++;
	return EXT4_OK;
}

static void *
device_allocate(void *context, size_t size)
{
	struct device *device = context;
	void *buffer = malloc(size);

	if (buffer != NULL) {
		device->allocations++;
		device->live_bytes += size;
		if (device->live_bytes > device->peak_bytes) {
			device->peak_bytes = device->live_bytes;
		}
	}
	return buffer;
}

static void
device_release(void *context, void *buffer, size_t size)
{
	struct device *device = context;

	device->live_bytes -= size;
	free(buffer);
}

static void
device_open(struct device *device, const char *path)
{
	FILE *stream = fopen(path, "rb");
	long size;

	memset(device, 0, sizeof(*device));
	require(stream != NULL && fseek(stream, 0, SEEK_END) == 0, "open image");
	size = ftell(stream);
	require(size > 0 && fseek(stream, 0, SEEK_SET) == 0, "size image");
	device->size = (size_t)size;
	device->bytes = malloc(device->size);
	require(device->bytes != NULL, "allocate image copy");
	require(
	    fread(device->bytes, 1, device->size, stream) == device->size && fclose(stream) == 0,
	    "read image");
	device->environment = (struct ext4_environment){ device, device->size, device_read,
		device_allocate, device_release };
	device->writer =
	    (struct ext4_write_environment){ device, device_write, device_flush, NULL };
}

static void
device_save(const struct device *device, const char *path)
{
	FILE *stream = fopen(path, "wb");

	require(stream != NULL, "create result image");
	require(
	    fwrite(device->bytes, 1, device->size, stream) == device->size && fclose(stream) == 0,
	    "write result image");
}

static void
begin(const struct device *device, struct sample *sample)
{
	sample->started = now();
	sample->reads = device->reads;
	sample->read_bytes = device->read_bytes;
	sample->writes = device->writes;
	sample->write_bytes = device->write_bytes;
	sample->flushes = device->flushes;
	sample->allocations = device->allocations;
}

/* Print one measurement; user_bytes is the payload the workload asked to store. */
static void
report(const char *workload, const char *extra, const struct device *device,
    const struct sample *sample, uint64_t operations, uint64_t user_bytes, uint64_t extents)
{
	uint64_t elapsed = now() - sample->started;
	uint64_t written = device->write_bytes - sample->write_bytes;

	printf("{\"workload\": \"%s\"%s, \"operations\": %" PRIu64 ", \"elapsed_ms\": %.3f, "
	       "\"reads\": %" PRIu64 ", \"read_bytes\": %" PRIu64 ", \"writes\": %" PRIu64
	       ", \"write_bytes\": %" PRIu64 ", \"flushes\": %" PRIu64 ", \"allocations\": %" PRIu64
	       ", \"peak_live_bytes\": %" PRIu64 ", \"user_bytes\": %" PRIu64
	       ", \"write_amplification\": %.3f, \"extents\": %" PRIu64 "}\n",
	    workload, extra, operations, (double)elapsed / NANOSECONDS_PER_MILLISECOND,
	    device->reads - sample->reads, device->read_bytes - sample->read_bytes,
	    device->writes - sample->writes, written, device->flushes - sample->flushes,
	    device->allocations - sample->allocations, device->peak_bytes, user_bytes,
	    user_bytes == 0 ? 0.0 : (double)written / (double)user_bytes, extents);
	fflush(stdout);
}

static struct ext4_inode_update
creation(uint16_t permissions)
{
	struct ext4_inode_update update = { 0 };

	update.fields = EXT4_ATTR_PERMISSIONS | EXT4_ATTR_UID | EXT4_ATTR_GID |
	    EXT4_ATTR_ACCESS_TIME | EXT4_ATTR_MODIFY_TIME | EXT4_ATTR_CHANGE_TIME |
	    EXT4_ATTR_XATTRS;
	update.permissions = permissions;
	update.access_time = scale_time;
	update.modify_time = scale_time;
	update.change_time = scale_time;
	return update;
}

static struct ext4_inode_update
data_update(void)
{
	struct ext4_inode_update update = { 0 };

	update.fields = EXT4_ATTR_PERMISSIONS | EXT4_ATTR_MODIFY_TIME | EXT4_ATTR_CHANGE_TIME |
	    EXT4_ATTR_XATTRS;
	update.permissions = 0644;
	update.modify_time = scale_time;
	update.change_time = scale_time;
	return update;
}

static struct ext4_inode
root(struct ext4_fs *fs)
{
	struct ext4_inode inode;

	expect(ext4_get_inode(fs, EXT4_ROOT_INODE, &inode), "read root");
	return inode;
}

static struct ext4_inode
make_directory(struct ext4_fs *fs, const struct ext4_inode *parent, const char *name)
{
	struct ext4_inode_update update = creation(0755);
	struct ext4_inode inode;

	expect(ext4_mkdir(fs, parent->number, parent->generation, (const uint8_t *)name,
		   strlen(name), &update, &scale_time, &inode),
	    "create directory");
	return inode;
}

static struct ext4_inode
make_file(struct ext4_fs *fs, const struct ext4_inode *parent, const char *name)
{
	struct ext4_inode_update update = creation(0644);
	struct ext4_inode inode;

	expect(ext4_create(fs, parent->number, parent->generation, (const uint8_t *)name,
		   strlen(name), &update, &scale_time, &inode),
	    "create file");
	return inode;
}

/* Count physically discontiguous runs of a file's data. */
static uint64_t
extents(struct ext4_fs *fs, uint32_t number)
{
	struct ext4_mapping mapping;
	struct ext4_inode inode;
	uint64_t offset = 0;
	uint64_t next = UINT64_MAX;
	uint64_t count = 0;

	expect(ext4_get_inode(fs, number, &inode), "read measured file");
	while (offset < inode.size) {
		expect(ext4_map_read(fs, &inode, offset, (size_t)(inode.size - offset), &mapping),
		    "map measured file");
		require(mapping.length != 0, "empty mapping");
		if (!mapping.hole && mapping.device_offset != next) {
			count++;
		}
		next = mapping.hole ? UINT64_MAX : mapping.device_offset + mapping.length;
		offset += mapping.length;
	}
	return count;
}

static void
fill(struct ext4_fs *fs, const struct ext4_inode *file, uint64_t size, size_t request,
    uint8_t *buffer)
{
	struct ext4_inode_update update = data_update();
	uint64_t offset;
	size_t length;
	size_t completed;

	for (offset = 0; offset < size; offset += length) {
		length = size - offset < request ? (size_t)(size - offset) : request;
		memset(buffer, (int)(offset / request) & 0xff, length);
		expect(ext4_write_partial(fs, file->number, file->generation, offset, buffer,
			   length, &update, &completed),
		    "write file data");
		require(completed == length, "short file write");
	}
}

static void
sequential(struct device *device, uint64_t mebibytes, size_t request)
{
	struct sample sample;
	struct ext4_inode directory;
	struct ext4_inode file;
	struct ext4_inode_update update = data_update();
	struct ext4_fs *fs;
	uint8_t *buffer = malloc(request);
	uint64_t size = mebibytes * MEBIBYTE;
	uint64_t blocks;
	uint64_t index;
	size_t completed;
	size_t unit;
	char extra[64];

	require(buffer != NULL, "allocate request buffer");
	expect(ext4_mount_writable(&device->environment, &device->writer, &fs), "mount");
	directory = root(fs);
	file = make_file(fs, &directory, "sequential");
	snprintf(extra, sizeof(extra), ", \"request_bytes\": %zu", request);
	begin(device, &sample);
	fill(fs, &file, size, request, buffer);
	expect(ext4_sync(fs), "sync");
	report("sequential-write", extra, device, &sample, size / request, size,
	    extents(fs, file.number));
	/* Random aligned overwrites of the same data, each one transaction. */
	unit = OVERWRITE_BYTES;
	blocks = size / unit;
	begin(device, &sample);
	for (index = 0; index < OVERWRITES; index++) {
		memset(buffer, (int)index & 0xff, unit);
		expect(ext4_write(fs, file.number, file.generation, (next_random() % blocks) * unit,
			   buffer, unit, &update, &completed),
		    "overwrite");
	}
	expect(ext4_sync(fs), "sync");
	report("random-overwrite", ", \"request_bytes\": 65536", device, &sample, OVERWRITES,
	    OVERWRITES * (uint64_t)unit, extents(fs, file.number));
	begin(device, &sample);
	expect(ext4_get_inode(fs, file.number, &file), "refresh file");
	expect(ext4_truncate(fs, file.number, file.generation, 0, &update, &file), "truncate");
	expect(ext4_sync(fs), "sync");
	report("truncate", "", device, &sample, 1, 0, 0);
	ext4_unmount(fs);
	require(device->live_bytes == 0, "leaked core allocations");
	free(buffer);
}

static void
directories(struct device *device, uint32_t entries)
{
	struct sample sample;
	struct sample batch;
	struct ext4_inode parent;
	struct ext4_inode directory;
	struct ext4_inode inode;
	struct ext4_fs *fs;
	char name[NAME_BYTES];
	char extra[64];
	uint32_t index;

	expect(ext4_mount_writable(&device->environment, &device->writer, &fs), "mount");
	parent = root(fs);
	directory = make_directory(fs, &parent, "many");
	begin(device, &sample);
	begin(device, &batch);
	for (index = 0; index < entries; index++) {
		snprintf(name, sizeof(name), "entry-%07u", index);
		make_file(fs, &directory, name);
		if ((index + 1U) % DIRECTORY_BATCH == 0) {
			snprintf(extra, sizeof(extra), ", \"entries_after\": %u", index + 1U);
			report(
			    "directory-create-batch", extra, device, &batch, DIRECTORY_BATCH, 0, 0);
			begin(device, &batch);
		}
	}
	expect(ext4_sync(fs), "sync");
	snprintf(extra, sizeof(extra), ", \"entries\": %u", entries);
	report("directory-create", extra, device, &sample, entries, 0, 0);
	parent = root(fs);
	expect(ext4_lookup(fs, &parent, (const uint8_t *)"many", 4, &directory), "find directory");
	begin(device, &sample);
	for (index = 0; index < LOOKUP_SAMPLES; index++) {
		snprintf(name, sizeof(name), "entry-%07u", next_random() % entries);
		expect(ext4_lookup(fs, &directory, (const uint8_t *)name, strlen(name), &inode),
		    "lookup");
	}
	report("directory-lookup", extra, device, &sample, LOOKUP_SAMPLES, 0, 0);
	ext4_unmount(fs);
	require(device->live_bytes == 0, "leaked core allocations");
}

/* Fill free space with small files, remove every other one, then allocate a
 * large file from the resulting fragments. */
static void
fragmented(struct device *device, uint32_t files, uint64_t mebibytes)
{
	struct sample sample;
	struct ext4_inode parent;
	struct ext4_inode directory;
	struct ext4_inode file;
	struct ext4_inode result;
	struct ext4_inode_update update = data_update();
	struct ext4_fs *fs;
	uint8_t *buffer = malloc(MEBIBYTE);
	char name[NAME_BYTES];
	char extra[96];
	uint32_t index;
	size_t completed;

	require(buffer != NULL, "allocate request buffer");
	expect(ext4_mount_writable(&device->environment, &device->writer, &fs), "mount");
	parent = root(fs);
	directory = make_directory(fs, &parent, "fragments");
	for (index = 0; index < files; index++) {
		snprintf(name, sizeof(name), "piece-%07u", index);
		file = make_file(fs, &directory, name);
		memset(buffer, (int)index & 0xff, PIECE_BYTES);
		expect(ext4_write(fs, file.number, file.generation, 0, buffer, PIECE_BYTES, &update,
			   &completed),
		    "write piece");
	}
	/* Creation grew and indexed the directory; lookups need its current map. */
	expect(ext4_get_inode(fs, directory.number, &directory), "refresh directory");
	for (index = 0; index < files; index += 2U) {
		snprintf(name, sizeof(name), "piece-%07u", index);
		expect(ext4_lookup(fs, &directory, (const uint8_t *)name, strlen(name), &file),
		    "find piece");
		expect(
		    ext4_unlink(fs, directory.number, directory.generation, (const uint8_t *)name,
			strlen(name), file.number, file.generation, &scale_time, &result),
		    "remove piece");
		expect(ext4_get_inode(fs, directory.number, &directory), "refresh directory");
	}
	parent = root(fs);
	file = make_file(fs, &parent, "after-fragmentation");
	snprintf(extra, sizeof(extra), ", \"removed_pieces\": %u", (files + 1U) / 2U);
	begin(device, &sample);
	fill(fs, &file, mebibytes * MEBIBYTE, MEBIBYTE, buffer);
	expect(ext4_sync(fs), "sync");
	report("fragmented-write", extra, device, &sample, mebibytes, mebibytes * MEBIBYTE,
	    extents(fs, file.number));
	ext4_unmount(fs);
	require(device->live_bytes == 0, "leaked core allocations");
	free(buffer);
}

/* A large file held open and unlinked at unmount is reclaimed by offline recovery. */
static void
reclamation(struct device *device, uint64_t mebibytes)
{
	struct sample sample;
	struct ext4_recovery_report recovery;
	struct ext4_inode_hold *hold;
	struct ext4_inode parent;
	struct ext4_inode file;
	struct ext4_inode result;
	struct ext4_fs *fs;
	uint8_t *buffer = malloc(MEBIBYTE);

	require(buffer != NULL, "allocate request buffer");
	expect(ext4_mount_writable(&device->environment, &device->writer, &fs), "mount");
	parent = root(fs);
	file = make_file(fs, &parent, "orphan");
	fill(fs, &file, mebibytes * MEBIBYTE, MEBIBYTE, buffer);
	expect(ext4_hold_inode(fs, file.number, file.generation, &hold), "hold");
	expect(ext4_unlink(fs, parent.number, parent.generation, (const uint8_t *)"orphan", 6,
		   file.number, file.generation, &scale_time, &result),
	    "unlink held file");
	expect(ext4_sync(fs), "sync");
	ext4_unmount(fs);
	begin(device, &sample);
	expect(ext4_recover(&device->environment, &device->writer, &recovery), "recover");
	report("orphan-reclamation", ", \"cleaned_orphans\": 1", device, &sample,
	    recovery.orphan_transactions, 0, 0);
	require(recovery.cleaned_orphans == 1, "orphan not reclaimed");
	free(buffer);
}

int
main(int argc, char **argv)
{
	static struct device device;

	if (argc != 4 && argc != 5) {
		fprintf(stderr,
		    "usage: %s IMAGE sequential|directory|fragmented|reclamation SIZE [RESULT]\n"
		    "SIZE is MiB, except entries for directory and files for fragmented.\n"
		    "RESULT receives the image after the workload.\n",
		    argv[0]);
		return 2;
	}
	device_open(&device, argv[1]);
	if (strcmp(argv[2], "sequential") == 0) {
		sequential(&device, strtoull(argv[3], NULL, 10), MEBIBYTE);
	} else if (strcmp(argv[2], "directory") == 0) {
		directories(&device, (uint32_t)strtoul(argv[3], NULL, 10));
	} else if (strcmp(argv[2], "fragmented") == 0) {
		fragmented(&device, (uint32_t)strtoul(argv[3], NULL, 10), FRAGMENTED_MEBIBYTES);
	} else if (strcmp(argv[2], "reclamation") == 0) {
		reclamation(&device, strtoull(argv[3], NULL, 10));
	} else {
		fprintf(stderr, "unknown workload %s\n", argv[2]);
		return 2;
	}
	if (argc == 5) {
		device_save(&device, argv[4]);
	}
	free(device.bytes);
	return 0;
}

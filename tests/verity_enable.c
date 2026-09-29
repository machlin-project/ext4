/* SPDX-License-Identifier: BSD-3-Clause */
#include "sha.h"
#include "storage.h"
#include "verity.h"

#include <inttypes.h>

/* Enabling fs-verity on e2fsprogs-authored volumes with the verity feature. Files
 * that are empty, one byte, one block, sparse, preallocated past EOF or large enough
 * for several tree levels become verity files under both hash algorithms, Merkle
 * blocks of the filesystem block size and 1 KiB, and salts. Their contents read back
 * verified, their digests measure, their times stay unchanged, and they refuse
 * further enabling and writes. Unsuitable files and parameters are refused, and a
 * volume without the feature refuses enabling. A power cut at every write or barrier
 * of a multi-transaction enable recovers either the original file, with its blocks
 * and free space, or the verity file. Every allocation and read failure of an enable,
 * and running out of space after part of the tree is written, leave the original
 * file after the rollback, or the verity file. With --export, the final image and a manifest
 * in the verity fixtures' format, extended by each file's parameters, go to the
 * directory for independent and Linux verification. */

#define VERITY_SECONDS 1700020000
#define NAME_BYTES 32U
#define PATTERN_SEED 0x3bU
#define SALT_BYTES 32U
#define SHORT_SALT_BYTES 16U
#define SALT_SEED 0x40U
#define SMALL_MERKLE 1024U
#define PREALLOCATED_BLOCKS 8U
#define CRASH_BYTES (4U * 1024U * 1024U + 1U)
#define FAULT_BYTES (300U * 1024U + 7U)
/* Merkle bytes one write transaction takes, as the core queues them. */
#define QUEUE_BYTES (128U * 1024U)
#define MANIFEST_LINE 512U

struct enable_case {
	const char *name;
	/* Size in filesystem blocks plus bytes; a hole covers blocks [hole, hole_end). */
	uint32_t blocks;
	uint32_t bytes;
	uint32_t hole;
	uint32_t hole_end;
	bool preallocate;
	/* Stored inline before enabling, on volumes with inline data only. */
	bool inline_data;
	uint32_t algorithm;
	/* Zero selects the filesystem block size. */
	uint32_t merkle;
	uint32_t salt_size;
};

static const struct enable_case cases[] = {
	{ "empty", 0, 0, 0, 0, false, false, EXT4_VERITY_HASH_SHA256, 0, 0 },
	{ "one", 0, 1, 0, 0, false, false, EXT4_VERITY_HASH_SHA256, 0, 0 },
	{ "block", 1, 0, 0, 0, false, false, EXT4_VERITY_HASH_SHA512, 0, SALT_BYTES },
	{ "holes", 5, 77, 1, 3, false, false, EXT4_VERITY_HASH_SHA256, 0, SHORT_SALT_BYTES },
	{ "preallocated", 2, 5, 0, 0, true, false, EXT4_VERITY_HASH_SHA256, 0, 0 },
	{ "levels", 0, 1536U * 1024U + 77U, 0, 0, false, false, EXT4_VERITY_HASH_SHA256,
	    SMALL_MERKLE, 0 },
	{ "sha512-levels", 0, 700U * 1024U, 0, 0, false, false, EXT4_VERITY_HASH_SHA512,
	    SMALL_MERKLE, SALT_BYTES },
	{ "inline", 0, 100, 0, 0, false, true, EXT4_VERITY_HASH_SHA256, 0, 0 },
};

#define CASE_COUNT (sizeof(cases) / sizeof(cases[0]))

static const struct ext4_timestamp verity_time = { VERITY_SECONDS, 0 };
static uint8_t salt[SALT_BYTES];

static struct ext4_inode_update
creation(void)
{
	struct ext4_inode_update update = { 0 };

	update.fields = EXT4_ATTR_PERMISSIONS | EXT4_ATTR_UID | EXT4_ATTR_GID |
	    EXT4_ATTR_ACCESS_TIME | EXT4_ATTR_MODIFY_TIME | EXT4_ATTR_CHANGE_TIME |
	    EXT4_ATTR_XATTRS;
	update.permissions = 0644;
	update.access_time = verity_time;
	update.modify_time = verity_time;
	update.change_time = verity_time;
	return update;
}

static struct ext4_inode_update
change(void)
{
	struct ext4_inode_update update = { 0 };

	/* An empty attribute batch admits the attributes a file already has. */
	update.fields = EXT4_ATTR_PERMISSIONS | EXT4_ATTR_MODIFY_TIME | EXT4_ATTR_CHANGE_TIME |
	    EXT4_ATTR_XATTRS;
	update.permissions = 0644;
	update.modify_time = verity_time;
	update.change_time = verity_time;
	return update;
}

static void
pattern(uint8_t *data, size_t length, uint32_t index, uint32_t hole, uint32_t hole_end,
    uint32_t block_size)
{
	size_t byte;

	for (byte = 0; byte < length; byte++) {
		data[byte] = (uint8_t)(PATTERN_SEED + index * 13U + byte % 251U + (byte >> 12));
		if (byte / block_size >= hole && byte / block_size < hole_end) {
			data[byte] = 0;
		}
	}
}

static uint64_t
case_size(const struct enable_case *item, uint32_t block_size)
{
	return (uint64_t)item->blocks * block_size + item->bytes;
}

static struct ext4_verity_parameters
case_parameters(const struct enable_case *item, uint32_t block_size)
{
	struct ext4_verity_parameters parameters = { 0 };

	parameters.hash_algorithm = item->algorithm;
	parameters.block_size = item->merkle == 0 ? block_size : item->merkle;
	parameters.salt = item->salt_size == 0 ? NULL : salt;
	parameters.salt_size = item->salt_size;
	return parameters;
}

static void
lookup(struct ext4_fs *fs, const char *name, struct ext4_inode *inode)
{
	struct ext4_inode root;

	EXPECT(ext4_get_inode(fs, EXT4_ROOT_INODE, &root), EXT4_OK);
	EXPECT(ext4_lookup(fs, &root, (const uint8_t *)name, strlen(name), inode), EXT4_OK);
}

static void
write_range(struct ext4_fs *fs, const struct ext4_inode *inode, const uint8_t *data, uint64_t from,
    uint64_t to)
{
	struct ext4_inode_update write = change();
	size_t completed;

	while (from < to) {
		completed = 0;
		EXPECT(ext4_write_partial(fs, inode->number, inode->generation, from, data + from,
			   (size_t)(to - from), &write, &completed),
		    EXT4_OK);
		CHECK(completed != 0);
		from += completed;
	}
}

/* Create a file with the pattern, leaving its hole unwritten. */
static void
create_file(struct ext4_fs *fs, const char *name, const uint8_t *data, uint64_t size, uint32_t hole,
    uint32_t hole_end, uint32_t block_size, struct ext4_inode *inode)
{
	struct ext4_inode_update update = creation();
	struct ext4_inode root;
	uint64_t skip_from = (uint64_t)hole * block_size;
	uint64_t skip_to = (uint64_t)hole_end * block_size;

	CHECK(skip_from <= skip_to && (skip_from == skip_to || skip_to < size));
	EXPECT(ext4_get_inode(fs, EXT4_ROOT_INODE, &root), EXT4_OK);
	EXPECT(ext4_create(fs, root.number, root.generation, (const uint8_t *)name, strlen(name),
		   &update, &verity_time, inode),
	    EXT4_OK);
	write_range(fs, inode, data, 0, skip_from < size ? skip_from : size);
	write_range(fs, inode, data, skip_to, size);
	lookup(fs, name, inode);
	CHECK(inode->size == size);
}

static void
digest_text(const uint8_t *digest, size_t size, char *text)
{
	size_t index;

	for (index = 0; index < size; index++) {
		snprintf(text + index * 2U, 3, "%02x", digest[index]);
	}
}

/* Enable one case and check the verity file; append its manifest line. */
static void
enable_case(struct ext4_fs *fs, uint32_t index, uint32_t block_size, FILE *manifest)
{
	const struct enable_case *item = &cases[index];
	struct ext4_verity_parameters parameters = case_parameters(item, block_size);
	struct ext4_inode_update write = change();
	struct ext4_sha256 context;
	struct ext4_inode inode;
	struct ext4_inode result;
	struct ext4_inode again;
	uint8_t digest[EXT4_VERITY_MAX_DIGEST];
	uint8_t content[EXT4_SHA256_DIGEST_SIZE];
	char text[2U * EXT4_VERITY_MAX_DIGEST + 1U];
	char content_text[2U * EXT4_SHA256_DIGEST_SIZE + 1U];
	char salt_text[2U * SALT_BYTES + 1U];
	uint64_t size = case_size(item, block_size);
	uint64_t completed_range;
	uint32_t algorithm;
	uint8_t *data;
	uint8_t *read_back;
	size_t digest_size;
	size_t completed;

	data = malloc((size_t)size + 1U);
	read_back = malloc((size_t)size + 1U);
	CHECK(data != NULL && read_back != NULL);
	pattern(data, (size_t)size, index, item->hole, item->hole_end, block_size);
	create_file(fs, item->name, data, size, item->hole, item->hole_end, block_size, &inode);
	CHECK(!item->inline_data || (inode.flags & EXT4_INODE_INLINE_DATA));
	if (item->preallocate) {
		EXPECT(ext4_fallocate(fs, inode.number, inode.generation, size,
			   (uint64_t)PREALLOCATED_BLOCKS * block_size, EXT4_FALLOC_KEEP_SIZE,
			   &write, &completed_range),
		    EXT4_OK);
		lookup(fs, item->name, &inode);
	}
	EXPECT(ext4_measure_verity(fs, &inode, &algorithm, digest, sizeof(digest), &digest_size),
	    EXT4_NOT_FOUND);
	EXPECT(
	    ext4_enable_verity(fs, inode.number, inode.generation, &parameters, &result), EXT4_OK);
	CHECK((result.flags & EXT4_INODE_VERITY) && (result.flags & EXT4_INODE_EXTENTS) &&
	    !(result.flags & EXT4_INODE_INLINE_DATA) && result.size == size &&
	    result.links == inode.links &&
	    memcmp(&result.modify_time, &inode.modify_time, sizeof(inode.modify_time)) == 0 &&
	    memcmp(&result.change_time, &inode.change_time, sizeof(inode.change_time)) == 0);
	lookup(fs, item->name, &again);
	CHECK(again.flags == result.flags && again.size == result.size &&
	    again.blocks_512 == result.blocks_512);
	EXPECT(ext4_read(fs, &result, 0, read_back, (size_t)size, &completed), EXT4_OK);
	CHECK(completed == size && memcmp(read_back, data, (size_t)size) == 0);
	EXPECT(ext4_measure_verity(fs, &result, &algorithm, digest, 1, &digest_size), EXT4_RANGE);
	CHECK(algorithm == item->algorithm);
	EXPECT(ext4_measure_verity(fs, &result, &algorithm, digest, sizeof(digest), &digest_size),
	    EXT4_OK);
	CHECK(digest_size == (item->algorithm == EXT4_VERITY_HASH_SHA256 ? 32U : 64U));
	EXPECT(ext4_enable_verity(fs, result.number, result.generation, &parameters, &again),
	    EXT4_EXISTS);
	EXPECT(ext4_write(fs, result.number, result.generation, 0, "x", 1, &write, &completed),
	    EXT4_PERMISSION_DENIED);
	ext4_sha256_init(&context);
	ext4_sha256_update(&context, data, (size_t)size);
	ext4_sha256_final(&context, content);
	digest_text(content, sizeof(content), content_text);
	digest_text(digest, digest_size, text);
	if (item->salt_size == 0) {
		snprintf(salt_text, sizeof(salt_text), "-");
	} else {
		digest_text(salt, item->salt_size, salt_text);
	}
	if (manifest != NULL) {
		fprintf(manifest, "good %s %" PRIu64 " %s %s - %u %u %s\n", item->name, size,
		    content_text, text, item->algorithm, parameters.block_size, salt_text);
	}
	free(read_back);
	free(data);
}

/* Unsuitable files and parameters are refused without change. */
static void
refusals(struct ext4_fs *fs, uint32_t block_size)
{
	struct ext4_verity_parameters parameters = case_parameters(&cases[1], block_size);
	struct ext4_verity_parameters invalid;
	struct ext4_inode_update update = creation();
	struct ext4_inode_update write = change();
	struct ext4_inode_hold *hold;
	struct ext4_inode root;
	struct ext4_inode inode;
	struct ext4_inode result;
	size_t completed;

	EXPECT(ext4_get_inode(fs, EXT4_ROOT_INODE, &root), EXT4_OK);
	EXPECT(ext4_mkdir(fs, root.number, root.generation, (const uint8_t *)"directory", 9,
		   &update, &verity_time, &inode),
	    EXT4_OK);
	EXPECT(ext4_enable_verity(fs, inode.number, inode.generation, &parameters, &result),
	    EXT4_IS_DIRECTORY);
	EXPECT(ext4_create(fs, root.number, root.generation, (const uint8_t *)"append", 6, &update,
		   &verity_time, &inode),
	    EXT4_OK);
	EXPECT(ext4_write(fs, inode.number, inode.generation, 0, "append", 6, &write, &completed),
	    EXT4_OK);
	EXPECT(ext4_set_inode_flags(fs, inode.number, inode.generation, EXT4_INODE_APPEND,
		   EXT4_INODE_APPEND, &verity_time, &inode),
	    EXT4_OK);
	EXPECT(ext4_enable_verity(fs, inode.number, inode.generation, &parameters, &result),
	    EXT4_PERMISSION_DENIED);
	EXPECT(ext4_set_inode_flags(
		   fs, inode.number, inode.generation, EXT4_INODE_APPEND, 0, &verity_time, &inode),
	    EXT4_OK);
	invalid = parameters;
	invalid.hash_algorithm = EXT4_VERITY_HASH_SHA512 + 1U;
	EXPECT(ext4_enable_verity(fs, inode.number, inode.generation, &invalid, &result),
	    EXT4_INVALID_ARGUMENT);
	invalid = parameters;
	invalid.block_size = SMALL_MERKLE / 2U;
	EXPECT(ext4_enable_verity(fs, inode.number, inode.generation, &invalid, &result),
	    EXT4_INVALID_ARGUMENT);
	invalid.block_size = block_size * 2U;
	EXPECT(ext4_enable_verity(fs, inode.number, inode.generation, &invalid, &result),
	    EXT4_INVALID_ARGUMENT);
	invalid.block_size = block_size - 1U;
	EXPECT(ext4_enable_verity(fs, inode.number, inode.generation, &invalid, &result),
	    EXT4_INVALID_ARGUMENT);
	invalid = parameters;
	invalid.salt = salt;
	invalid.salt_size = SALT_BYTES + 1U;
	EXPECT(ext4_enable_verity(fs, inode.number, inode.generation, &invalid, &result),
	    EXT4_INVALID_ARGUMENT);
	invalid.salt = NULL;
	invalid.salt_size = SHORT_SALT_BYTES;
	EXPECT(ext4_enable_verity(fs, inode.number, inode.generation, &invalid, &result),
	    EXT4_INVALID_ARGUMENT);
	EXPECT(ext4_enable_verity(fs, inode.number, inode.generation + 1U, &parameters, &result),
	    EXT4_STALE);
	/* An unlinked file held open cannot be enabled. */
	EXPECT(ext4_hold_inode(fs, inode.number, inode.generation, &hold), EXT4_OK);
	EXPECT(ext4_unlink(fs, root.number, root.generation, (const uint8_t *)"append", 6,
		   inode.number, inode.generation, &verity_time, &result),
	    EXT4_OK);
	EXPECT(ext4_enable_verity(fs, inode.number, inode.generation, &parameters, &result),
	    EXT4_INVALID_ARGUMENT);
	EXPECT(ext4_release_inode(hold), EXT4_OK);
}

static void
export_manifest(
    const char *directory, const char *source, char *path, size_t capacity, FILE **manifest)
{
	const char *name = strrchr(source, '/');
	const char *extension;
	int length;

	name = name == NULL ? source : name + 1;
	extension = strrchr(name, '.');
	length = snprintf(path, capacity, "%s/enable-%.*s.manifest", directory,
	    (int)(extension == NULL ? strlen(name) : (size_t)(extension - name)), name);
	CHECK(length > 0 && (size_t)length < capacity);
	*manifest = fopen(path, "wx");
	CHECK(*manifest != NULL);
}

/* The file after a failed enable: unchanged contents, no tree and its space returned. */
static void
check_original(struct ext4_fs *fs, const char *name, const uint8_t *data, uint64_t size,
    uint64_t blocks_512, uint64_t free_blocks)
{
	struct ext4_inode inode;
	uint8_t *read_back = malloc((size_t)size + 1U);
	size_t completed;

	CHECK(read_back != NULL);
	lookup(fs, name, &inode);
	CHECK(!(inode.flags & EXT4_INODE_VERITY) && inode.size == size &&
	    inode.blocks_512 == blocks_512 && fs->info.free_blocks == free_blocks &&
	    fs->last_orphan == 0);
	EXPECT(ext4_read(fs, &inode, 0, read_back, (size_t)size, &completed), EXT4_OK);
	CHECK(completed == size && memcmp(read_back, data, (size_t)size) == 0);
	free(read_back);
}

/* Fail each allocation and read of one enable in turn. */
static void
faults(struct device *device)
{
	struct ext4_verity_parameters parameters = { EXT4_VERITY_HASH_SHA256, SMALL_MERKLE, NULL,
		0 };
	struct ext4_recovery_report report;
	struct ext4_inode inode;
	struct ext4_inode result;
	struct ext4_fs *fs;
	uint8_t *data;
	uint8_t *before;
	uint64_t free_blocks;
	uint64_t blocks_512;
	uint32_t allocations;
	uint32_t reads;
	uint32_t fault;
	uint32_t original = 0;
	uint32_t enabled = 0;
	uint32_t recovered = 0;
	enum ext4_result error;

	data = malloc(FAULT_BYTES);
	before = malloc(device->size);
	CHECK(data != NULL && before != NULL);
	pattern(data, FAULT_BYTES, CASE_COUNT + 1U, 0, 0, device->block_size);
	device_reset(device, device->base);
	EXPECT(ext4_mount_writable(&device->environment, &device->writer, &fs), EXT4_OK);
	create_file(fs, "faults", data, FAULT_BYTES, 0, 0, device->block_size, &inode);
	EXPECT(ext4_sync(fs), EXT4_OK);
	free_blocks = fs->info.free_blocks;
	blocks_512 = inode.blocks_512;
	ext4_unmount(fs);
	memcpy(before, device->stable, device->size);
	EXPECT(ext4_mount_writable(&device->environment, &device->writer, &fs), EXT4_OK);
	allocations = device->allocations;
	reads = device->reads;
	EXPECT(
	    ext4_enable_verity(fs, inode.number, inode.generation, &parameters, &result), EXT4_OK);
	allocations = device->allocations - allocations;
	reads = device->reads - reads;
	ext4_unmount(fs);
	for (fault = 1; fault <= allocations + reads; fault++) {
		device_reset(device, before);
		EXPECT(ext4_mount_writable(&device->environment, &device->writer, &fs), EXT4_OK);
		if (fault <= allocations) {
			device->fail_allocation = device->allocations + fault;
		} else {
			device->fail_read = device->reads + fault - allocations;
		}
		error =
		    ext4_enable_verity(fs, inode.number, inode.generation, &parameters, &result);
		device->fail_allocation = 0;
		device->fail_read = 0;
		if (error == EXT4_OK) {
			CHECK(result.flags & EXT4_INODE_VERITY);
			ext4_unmount(fs);
			enabled++;
			continue;
		}
		CHECK(error == (fault <= allocations ? EXT4_NO_MEMORY : EXT4_IO));
		if (fs->aborted) {
			/* An uncertain commit leaves the device state for recovery. */
			ext4_unmount(fs);
			memcpy(device->stable, device->cache, device->size);
			EXPECT(
			    ext4_recover(&device->environment, &device->writer, &report), EXT4_OK);
			EXPECT(ext4_mount(&device->environment, &fs), EXT4_OK);
			lookup(fs, "faults", &result);
			if (!(result.flags & EXT4_INODE_VERITY)) {
				check_original(
				    fs, "faults", data, FAULT_BYTES, blocks_512, free_blocks);
			}
			ext4_unmount(fs);
			recovered++;
			continue;
		}
		EXPECT(ext4_sync(fs), EXT4_OK);
		ext4_unmount(fs);
		EXPECT(ext4_mount(&device->environment, &fs), EXT4_OK);
		check_original(fs, "faults", data, FAULT_BYTES, blocks_512, free_blocks);
		ext4_unmount(fs);
		original++;
	}
	free(before);
	free(data);
	printf("PASS verity enable faults: %u allocations, %u reads, %u rolled back, %u "
	       "recovered, %u enabled\n",
	    allocations, reads, original, recovered, enabled);
}

/* Leave room for one write transaction of the tree but not all of it: the enable
 * fails with NO_SPACE after writing part of the tree and returns the space. */
static void
no_space(struct device *device)
{
	struct ext4_verity_parameters parameters = { EXT4_VERITY_HASH_SHA256, SMALL_MERKLE, NULL,
		0 };
	struct ext4_inode_update update = creation();
	struct ext4_inode_update write = change();
	struct ext4_inode root;
	struct ext4_inode filler;
	struct ext4_inode inode;
	struct ext4_inode result;
	struct ext4_fs *fs;
	uint8_t *data;
	uint64_t offset = 0;
	uint64_t free_blocks;
	uint64_t blocks_512;
	uint32_t room = QUEUE_BYTES / device->block_size + QUEUE_BYTES / device->block_size / 4U;
	size_t chunk = (size_t)device->block_size * 64U;
	size_t completed;
	enum ext4_result error = EXT4_OK;

	data = malloc(device->size);
	CHECK(data != NULL);
	pattern(data, device->size, CASE_COUNT + 2U, 0, 0, device->block_size);
	device_reset(device, device->base);
	EXPECT(ext4_mount_writable(&device->environment, &device->writer, &fs), EXT4_OK);
	create_file(fs, "filler", data, (uint64_t)room * device->block_size, 0, 0,
	    device->block_size, &filler);
	EXPECT(ext4_get_inode(fs, EXT4_ROOT_INODE, &root), EXT4_OK);
	EXPECT(ext4_create(fs, root.number, root.generation, (const uint8_t *)"full", 4, &update,
		   &verity_time, &inode),
	    EXT4_OK);
	while (error == EXT4_OK) {
		completed = 0;
		error = ext4_write_partial(fs, inode.number, inode.generation, offset,
		    data + offset, chunk, &write, &completed);
		offset += completed;
	}
	EXPECT(error, EXT4_NO_SPACE);
	EXPECT(ext4_unlink(fs, root.number, root.generation, (const uint8_t *)"filler", 6,
		   filler.number, filler.generation, &verity_time, &result),
	    EXT4_OK);
	lookup(fs, "full", &inode);
	free_blocks = fs->info.free_blocks;
	blocks_512 = inode.blocks_512;
	CHECK(free_blocks >= room && inode.size == offset);
	EXPECT(ext4_enable_verity(fs, inode.number, inode.generation, &parameters, &result),
	    EXT4_NO_SPACE);
	check_original(fs, "full", data, offset, blocks_512, free_blocks);
	/* With the space of a truncated megabyte, the same file becomes a verity file. */
	offset -= 1024U * 1024U;
	EXPECT(ext4_truncate(fs, inode.number, inode.generation, offset, &write, &inode), EXT4_OK);
	EXPECT(
	    ext4_enable_verity(fs, inode.number, inode.generation, &parameters, &result), EXT4_OK);
	CHECK(result.flags & EXT4_INODE_VERITY);
	EXPECT(ext4_sync(fs), EXT4_OK);
	ext4_unmount(fs);
	free(data);
	printf("PASS verity enable without space: %" PRIu64 " bytes, %" PRIu64
	       " free blocks rolled back\n",
	    offset, free_blocks);
}

/* Cut every write or barrier of one multi-transaction enable. */
static void
power_cuts(struct device *device)
{
	struct ext4_verity_parameters parameters = { EXT4_VERITY_HASH_SHA256, SMALL_MERKLE, NULL,
		0 };
	struct ext4_recovery_report report;
	struct ext4_inode inode;
	struct ext4_inode result;
	struct ext4_fs *fs;
	uint8_t expected[EXT4_VERITY_MAX_DIGEST];
	uint8_t digest[EXT4_VERITY_MAX_DIGEST];
	uint8_t *data;
	uint8_t *read_back;
	uint8_t *before;
	uint64_t free_blocks;
	uint64_t blocks_512;
	uint32_t algorithm;
	uint32_t events;
	uint32_t cut;
	uint32_t original = 0;
	uint32_t enabled = 0;
	uint32_t torn = 0;
	size_t digest_size;
	size_t completed;
	enum ext4_result error;

	data = malloc(CRASH_BYTES);
	read_back = malloc(CRASH_BYTES);
	before = malloc(device->size);
	CHECK(data != NULL && read_back != NULL && before != NULL);
	pattern(data, CRASH_BYTES, CASE_COUNT, 0, 0, device->block_size);
	device_reset(device, device->base);
	EXPECT(ext4_mount_writable(&device->environment, &device->writer, &fs), EXT4_OK);
	create_file(fs, "crash", data, CRASH_BYTES, 0, 0, device->block_size, &inode);
	EXPECT(ext4_sync(fs), EXT4_OK);
	free_blocks = fs->info.free_blocks;
	blocks_512 = inode.blocks_512;
	ext4_unmount(fs);
	memcpy(before, device->stable, device->size);
	EXPECT(ext4_mount_writable(&device->environment, &device->writer, &fs), EXT4_OK);
	events = device->events;
	EXPECT(
	    ext4_enable_verity(fs, inode.number, inode.generation, &parameters, &result), EXT4_OK);
	events = device->events - events;
	EXPECT(
	    ext4_measure_verity(fs, &result, &algorithm, expected, sizeof(expected), &digest_size),
	    EXT4_OK);
	ext4_unmount(fs);
	for (cut = 1; cut <= events; cut++) {
		device_reset(device, before);
		EXPECT(ext4_mount_writable(&device->environment, &device->writer, &fs), EXT4_OK);
		device->stop_at = device->events + cut;
		device->survival = cut % 3U;
		device->partial = cut % 2U != 0;
		error =
		    ext4_enable_verity(fs, inode.number, inode.generation, &parameters, &result);
		CHECK(error != EXT4_OK && device->off);
		ext4_unmount(fs);
		device_reset(device, device->stable);
		error = ext4_recover(&device->environment, &device->writer, &report);
		if (error == EXT4_CORRUPT) {
			CHECK(device->metadata_checksum && device->writes == 0);
			torn++;
			continue;
		}
		EXPECT(error, EXT4_OK);
		EXPECT(ext4_mount(&device->environment, &fs), EXT4_OK);
		lookup(fs, "crash", &result);
		CHECK(result.size == CRASH_BYTES && fs->last_orphan == 0);
		EXPECT(ext4_read(fs, &result, 0, read_back, CRASH_BYTES, &completed), EXT4_OK);
		CHECK(completed == CRASH_BYTES && memcmp(read_back, data, CRASH_BYTES) == 0);
		if (result.flags & EXT4_INODE_VERITY) {
			EXPECT(ext4_measure_verity(
				   fs, &result, &algorithm, digest, sizeof(digest), &digest_size),
			    EXT4_OK);
			CHECK(memcmp(digest, expected, digest_size) == 0);
			enabled++;
		} else {
			/* Recovery removed the partial tree and its space. */
			CHECK(
			    result.blocks_512 == blocks_512 && fs->info.free_blocks == free_blocks);
			original++;
		}
		ext4_unmount(fs);
	}
	CHECK(original != 0 && enabled != 0);
	free(before);
	free(read_back);
	free(data);
	printf("PASS verity enable power cuts: %u events, %u original, %u enabled, %u torn "
	       "superblocks\n",
	    events, original, enabled, torn);
}

int
main(int argc, char **argv)
{
	static struct device device;
	struct ext4_verity_parameters parameters;
	struct ext4_inode inode;
	struct ext4_inode result;
	struct ext4_fs *fs;
	const char *exports = NULL;
	char path[4096];
	FILE *manifest = NULL;
	uint32_t index;
	int first = 1;
	int argument;

	if (argc >= 3 && strcmp(argv[1], "--export") == 0) {
		exports = argv[2];
		first = 3;
	}
	if (argc <= first) {
		fprintf(stderr, "usage: %s [--export DIRECTORY] IMAGE...\n", argv[0]);
		return 2;
	}
	for (index = 0; index < SALT_BYTES; index++) {
		salt[index] = (uint8_t)(SALT_SEED + index);
	}
	for (argument = first; argument < argc; argument++) {
		storage_open(&device, argv[argument]);
		device_reset(&device, device.base);
		EXPECT(ext4_mount_writable(&device.environment, &device.writer, &fs), EXT4_OK);
		if (!(fs->info.feature_ro_compat & EXT4_FEATURE_RO_VERITY)) {
			parameters = case_parameters(&cases[1], device.block_size);
			create_file(fs, "plain", (const uint8_t *)"plain", 5, 0, 0,
			    device.block_size, &inode);
			EXPECT(ext4_enable_verity(
				   fs, inode.number, inode.generation, &parameters, &result),
			    EXT4_UNSUPPORTED);
			ext4_unmount(fs);
			storage_close(&device);
			printf(
			    "PASS verity enable refused without the feature: %s\n", argv[argument]);
			continue;
		}
		if (exports != NULL) {
			export_manifest(exports, argv[argument], path, sizeof(path), &manifest);
			fprintf(manifest, "algorithm %u block %u\n", EXT4_VERITY_HASH_SHA256,
			    device.block_size);
		}
		for (index = 0; index < CASE_COUNT; index++) {
			if (cases[index].inline_data &&
			    !(fs->info.feature_incompat & EXT4_FEATURE_INCOMPAT_INLINE_DATA)) {
				continue;
			}
			enable_case(fs, index, device.block_size, manifest);
		}
		refusals(fs, device.block_size);
		EXPECT(ext4_sync(fs), EXT4_OK);
		ext4_unmount(fs);
		if (manifest != NULL) {
			CHECK(fclose(manifest) == 0);
			manifest = NULL;
			storage_export(&device, exports, argv[argument], "enable-");
		}
		power_cuts(&device);
		faults(&device);
		no_space(&device);
		storage_close(&device);
		printf("PASS verity enable: %s, %zu files\n", argv[argument], CASE_COUNT);
	}
	return 0;
}

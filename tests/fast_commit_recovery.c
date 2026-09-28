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

enum orphan_damage {
	ORPHAN_DUPLICATE,
	ORPHAN_LEGACY_DUPLICATE,
	ORPHAN_RESERVED,
	ORPHAN_OUT_OF_RANGE,
	ORPHAN_BAD_MAGIC,
	ORPHAN_BAD_CHECKSUM,
	ORPHAN_DAMAGE_COUNT
};

enum journal_map_damage {
	JOURNAL_MAP_HOLE,
	JOURNAL_MAP_DUPLICATE_DATA,
	JOURNAL_MAP_DATA_IS_NODE,
	JOURNAL_MAP_REUSED_NODE,
	JOURNAL_MAP_NODE_CYCLE,
	JOURNAL_MAP_DAMAGE_COUNT
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
compare_xattrs(struct ext4_fs *fs, struct ext4_fs *reference, const struct ext4_inode *inode,
    const struct ext4_inode *expected)
{
	struct ext4_xattr_key *keys;
	struct ext4_xattr_key *wanted;
	uint8_t *value;
	uint8_t *expected_value;
	size_t count;
	size_t expected_count;
	size_t size;
	size_t expected_size;
	size_t index;

	EXPECT(ext4_list_xattrs(fs, inode->number, inode->generation, NULL, 0, &count), EXT4_OK);
	EXPECT(ext4_list_xattrs(
		   reference, expected->number, expected->generation, NULL, 0, &expected_count),
	    EXT4_OK);
	CHECK(count == expected_count && count <= EXT4_XATTR_MAX_CHANGES);
	if (count == 0) {
		return;
	}
	keys = calloc(count, sizeof(*keys));
	wanted = calloc(count, sizeof(*wanted));
	CHECK(keys != NULL && wanted != NULL);
	EXPECT(
	    ext4_list_xattrs(fs, inode->number, inode->generation, keys, count, &count), EXT4_OK);
	EXPECT(ext4_list_xattrs(reference, expected->number, expected->generation, wanted,
		   expected_count, &expected_count),
	    EXT4_OK);
	CHECK(count == expected_count);
	for (index = 0; index < count; index++) {
		CHECK(keys[index].name_index == wanted[index].name_index &&
		    keys[index].name_length == wanted[index].name_length &&
		    keys[index].value_size == wanted[index].value_size &&
		    memcmp(keys[index].name, wanted[index].name, keys[index].name_length) == 0);
		size = keys[index].value_size;
		value = malloc(size == 0 ? 1U : size);
		expected_value = malloc(size == 0 ? 1U : size);
		CHECK(value != NULL && expected_value != NULL);
		EXPECT(ext4_get_xattr(fs, inode->number, inode->generation, keys[index].name_index,
			   keys[index].name, keys[index].name_length, value, size, &size),
		    EXT4_OK);
		EXPECT(ext4_get_xattr(reference, expected->number, expected->generation,
			   wanted[index].name_index, wanted[index].name, wanted[index].name_length,
			   expected_value, size, &expected_size),
		    EXT4_OK);
		CHECK(size == keys[index].value_size && size == expected_size &&
		    memcmp(value, expected_value, size) == 0);
		free(expected_value);
		free(value);
	}
	free(wanted);
	free(keys);
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
	compare_xattrs(fs, reference, inode, expected);
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
	CHECK(inode->device_major == expected->device_major &&
	    inode->device_minor == expected->device_minor);
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

static void
orphan_guards(struct device *device)
{
	struct ext4_fs *fs;
	struct ext4_inode file;
	struct ext4_recovery_report report;
	struct ext4_block_number_disk address;
	struct ext4_orphan_tail_disk *tail;
	struct ext4_le32 *entries;
	const struct ext4_super_disk *super;
	uint64_t block;
	uint32_t seed;
	uint32_t checksum;
	uint32_t index;
	unsigned int damage;

	reset(device);
	EXPECT(ext4_load(&device->environment, true, &fs), EXT4_OK);
	CHECK(fs->orphan_file_inode != 0 && fs->metadata_checksum);
	EXPECT(ext4_get_inode(fs, fs->orphan_file_inode, &file), EXT4_OK);
	EXPECT(ext4_map_block(fs, &file, 0, &block), EXT4_OK);
	CHECK(block != 0);
	ext4_encode32(&address.low, (uint32_t)block);
	ext4_encode32(&address.high, (uint32_t)(block >> 32));
	seed = ext4_crc32c(ext4_inode_seed(fs, &file), &address, sizeof(address));
	ext4_unmount(fs);
	for (damage = 0; damage < ORPHAN_DAMAGE_COUNT; damage++) {
		reset(device);
		super = (const struct ext4_super_disk *)(device->cache + EXT4_SUPER_OFFSET);
		entries = (struct ext4_le32 *)(device->cache + block * device->block_size);
		tail = (struct ext4_orphan_tail_disk *)((uint8_t *)entries + device->block_size -
		    sizeof(*tail));
		CHECK(ext4_le32(&entries[0]) != 0 && ext4_le32(&entries[1]) != 0);
		switch (damage) {
		case ORPHAN_DUPLICATE:
			ext4_encode32(&entries[1], ext4_le32(&entries[0]));
			break;
		case ORPHAN_LEGACY_DUPLICATE:
			CHECK(ext4_le32(&super->last_orphan) != 0);
			ext4_encode32(&entries[1], ext4_le32(&super->last_orphan));
			break;
		case ORPHAN_RESERVED:
			ext4_encode32(&entries[1], ext4_le32(&super->orphan_file_inode));
			break;
		case ORPHAN_OUT_OF_RANGE:
			ext4_encode32(&entries[1], ext4_le32(&super->inodes_count) + 1U);
			break;
		case ORPHAN_BAD_MAGIC:
			ext4_encode32(&tail->magic, EXT4_ORPHAN_MAGIC ^ 1U);
			break;
		case ORPHAN_BAD_CHECKSUM:
			break;
		}
		checksum = ext4_crc32c(seed, entries, device->block_size - sizeof(*tail));
		ext4_encode32(
		    &tail->checksum, damage == ORPHAN_BAD_CHECKSUM ? checksum ^ 1U : checksum);
		memcpy(device->stable, device->cache, device->size);
		EXPECT(ext4_recover(&device->environment, &device->writer, &report), EXT4_CORRUPT);
		CHECK(device->live == 0 && report.replayed_blocks == 0 && report.fast_commits == 0);
		for (index = 0; index < device->events; index++) {
			CHECK(device->history[index].flush);
		}
		CHECK(memcmp(device->cache, device->stable, device->size) == 0);
	}
	printf("PASS %u malformed orphan states rejected before fast-commit writes\n",
	    ORPHAN_DAMAGE_COUNT);
}

static void
journal_map_guards(struct device *device)
{
	struct ext4_fs *fs;
	struct ext4_inode inode;
	struct ext4_recovery_report report;
	const struct ext4_le32 *root;
	struct ext4_le32 *single;
	struct ext4_le32 *double_root;
	uint64_t single_block;
	uint64_t double_block;
	uint32_t middle = device->block_size / sizeof(*single) / 2U;
	uint32_t event_index;
	unsigned int damage;

	reset(device);
	EXPECT(ext4_load(&device->environment, true, &fs), EXT4_OK);
	EXPECT(ext4_get_inode(fs, fs->journal_inode, &inode), EXT4_OK);
	CHECK(!(inode.flags & EXT4_INODE_EXTENTS));
	root = (const struct ext4_le32 *)inode.block_data;
	single_block = ext4_le32(&root[EXT4_DIRECT_BLOCKS]);
	double_block = ext4_le32(&root[EXT4_DIRECT_BLOCKS + 1U]);
	CHECK(single_block != 0 && single_block < device->blocks && double_block != 0 &&
	    double_block < device->blocks);
	ext4_unmount(fs);
	for (damage = 0; damage < JOURNAL_MAP_DAMAGE_COUNT; damage++) {
		reset(device);
		single = (struct ext4_le32 *)(device->cache + single_block * device->block_size);
		double_root =
		    (struct ext4_le32 *)(device->cache + double_block * device->block_size);
		CHECK(ext4_le32(&single[middle]) != 0 && ext4_le32(&double_root[0]) != 0);
		switch (damage) {
		case JOURNAL_MAP_HOLE:
			ext4_encode32(&single[middle], 0);
			break;
		case JOURNAL_MAP_DUPLICATE_DATA:
			ext4_encode32(&single[middle], ext4_le32(&single[middle - 1U]));
			break;
		case JOURNAL_MAP_DATA_IS_NODE:
			ext4_encode32(&single[middle], (uint32_t)single_block);
			break;
		case JOURNAL_MAP_REUSED_NODE:
			ext4_encode32(&double_root[0], (uint32_t)single_block);
			break;
		case JOURNAL_MAP_NODE_CYCLE:
			ext4_encode32(&double_root[0], (uint32_t)double_block);
			break;
		}
		memcpy(device->stable, device->cache, device->size);
		EXPECT(ext4_recover(&device->environment, &device->writer, &report), EXT4_CORRUPT);
		CHECK(device->live == 0 && report.fast_commits == 0 && report.replayed_blocks == 0);
		for (event_index = 0; event_index < device->events; event_index++) {
			CHECK(device->history[event_index].flush);
		}
		CHECK(memcmp(device->stable, device->cache, device->size) == 0);
	}
	printf(
	    "PASS %u corrupt journal mappings rejected without writes\n", JOURNAL_MAP_DAMAGE_COUNT);
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
	bool orphans;
	bool journal_map;
	bool reject;
	enum ext4_result error;

	if (argc != 3 && argc != 4) {
		fprintf(stderr,
		    "usage: %s PENDING_IMAGE VERIFIED_REFERENCE_IMAGE "
		    "[--faults|--resources|--orphans|--journal-map|--reject]\n",
		    argv[0]);
		return 2;
	}
	faults = argc == 4 && strcmp(argv[3], "--faults") == 0;
	resources = argc == 4 && strcmp(argv[3], "--resources") == 0;
	orphans = argc == 4 && strcmp(argv[3], "--orphans") == 0;
	journal_map = argc == 4 && strcmp(argv[3], "--journal-map") == 0;
	reject = argc == 4 && strcmp(argv[3], "--reject") == 0;
	CHECK(argc == 3 || faults || resources || orphans || journal_map || reject);
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
	if (journal_map) {
		journal_map_guards(device);
		goto out;
	}
	if (reject) {
		EXPECT(ext4_recover(&device->environment, &device->writer, &report), EXT4_CORRUPT);
		CHECK(device->live == 0 && report.fast_commits == 0 && report.replayed_blocks == 0);
		for (stop = 0; stop < device->events; stop++) {
			CHECK(device->history[stop].flush);
		}
		CHECK(memcmp(device->base, device->cache, device->size) == 0);
		printf("PASS malformed inode payload rejected without home or journal writes\n");
		goto out;
	}
	EXPECT(ext4_recover(&device->environment, &device->writer, &report), EXT4_OK);
	CHECK(report.fast_commits != 0 && device->live == 0);
	operations = device->events;
	allocations = device->allocations;
	reads = device->reads;
	compare(device, reference);
	printf("PASS %u fast commits match verified namespace, data and accounting; %u durability "
	       "events; %u allocations, %u reads\n",
	    report.fast_commits, operations, allocations, reads);
	if (orphans) {
		orphan_guards(device);
	}
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
					damaged_super = (ext4_le32(&super->feature_ro_compat) &
							    EXT4_FEATURE_RO_METADATA_CSUM) &&
					    checksum != ext4_le32(&super->checksum);
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
out:
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

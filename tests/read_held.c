/* SPDX-License-Identifier: BSD-3-Clause */
#include "storage.h"

static void
check_mappings(struct device *device)
{
	struct ext4_fs *fs;
	struct ext4_inode root;
	struct ext4_inode inode;
	struct ext4_inode_hold *hold;
	struct ext4_mapping mapping;
	struct ext4_mapping before;
	uint8_t *expected;
	size_t completed;
	size_t length;
	size_t offset;
	size_t index;
	uint32_t reads;
	uint32_t allocations;
	uint32_t fault;
	unsigned int kind;

	EXPECT(ext4_mount(&device->environment, &fs), EXT4_OK);
	EXPECT(ext4_get_inode(fs, EXT4_ROOT_INODE, &root), EXT4_OK);
	EXPECT(ext4_lookup(fs, &root, (const uint8_t *)"payload.bin", 11, &inode), EXT4_OK);
	length = inode.size < 4U * device->block_size + 39U ? (size_t)inode.size
							    : 4U * device->block_size + 39U;
	CHECK(length != 0);
	expected = malloc(length);
	CHECK(expected != NULL);
	EXPECT(ext4_read(fs, &inode, 0, expected, length, &completed), EXT4_OK);
	CHECK(completed == length);
	EXPECT(ext4_hold_inode(fs, inode.number, inode.generation, &hold), EXT4_OK);
	device->allocations = device->reads = 0;
	EXPECT(ext4_map_read_held(hold, 0, length, &mapping), EXT4_OK);
	allocations = device->allocations;
	reads = device->reads;
	CHECK(allocations != 0 && reads != 0);
	device->allocations = 0;
	device->fail_allocation = 1;
	for (offset = 0; offset < length; offset += mapping.length) {
		EXPECT(ext4_map_read_held(hold, offset, length - offset, &mapping), EXT4_OK);
		CHECK(mapping.length != 0 && mapping.length <= length - offset);
		if (mapping.hole) {
			for (index = 0; index < mapping.length; index++) {
				CHECK(expected[offset + index] == 0);
			}
		} else {
			CHECK(mapping.device_offset <= device->size &&
			    mapping.length <= device->size - mapping.device_offset);
			CHECK(memcmp(device->cache + mapping.device_offset, expected + offset,
				  mapping.length) == 0);
		}
	}
	CHECK(device->allocations == 0);
	device->fail_allocation = 0;

	memset(&mapping, 0xa5, sizeof(mapping));
	memcpy(&before, &mapping, sizeof(before));
	EXPECT(ext4_map_read_held(NULL, 0, length, &mapping), EXT4_INVALID_ARGUMENT);
	EXPECT(ext4_map_read_held(hold, 0, 0, &mapping), EXT4_INVALID_ARGUMENT);
	EXPECT(ext4_map_read_held(hold, 0, length, NULL), EXT4_INVALID_ARGUMENT);
	EXPECT(ext4_map_read_held(hold, inode.size, length, &mapping), EXT4_NOT_FOUND);
	EXPECT(ext4_map_read_held(hold, UINT64_MAX, length, &mapping), EXT4_NOT_FOUND);
	fs->aborted = true;
	EXPECT(ext4_map_read_held(hold, 0, length, &mapping), EXT4_RECOVERY_REQUIRED);
	fs->aborted = false;
	CHECK(memcmp(&before, &mapping, sizeof(before)) == 0);
	for (kind = 0; kind < 2U; kind++) {
		for (fault = 1; fault <= (kind == 0 ? allocations : reads); fault++) {
			ext4_drop_read_cache(hold);
			device->allocations = device->reads = 0;
			device->fail_allocation = kind == 0 ? fault : 0;
			device->fail_read = kind == 1 ? fault : 0;
			EXPECT(ext4_map_read_held(hold, 0, length, &mapping),
			    kind == 0 ? EXT4_NO_MEMORY : EXT4_IO);
			CHECK(memcmp(&before, &mapping, sizeof(before)) == 0);
			device->fail_allocation = device->fail_read = 0;
		}
	}
	/* Recovery after a failed initialization shares state with copied reads. */
	EXPECT(ext4_read_held(hold, 0, expected, length, &completed), EXT4_OK);
	CHECK(completed == length);
	EXPECT(ext4_release_inode(hold), EXT4_OK);
	ext4_unmount(fs);
	CHECK(device->live == 0 && device->writes == 0);
	free(expected);
}

static void
check_reads(struct device *device)
{
	struct ext4_fs *fs;
	struct ext4_inode root;
	struct ext4_inode inode;
	struct ext4_inode_hold *hold;
	struct ext4_inode_hold *duplicate;
	uint8_t *expected;
	uint8_t *observed;
	size_t length;
	size_t completed;
	uint32_t live;
	uint32_t reads;
	uint32_t allocations;
	uint32_t fault;
	unsigned int kind;

	EXPECT(ext4_mount(&device->environment, &fs), EXT4_OK);
	EXPECT(ext4_get_inode(fs, EXT4_ROOT_INODE, &root), EXT4_OK);
	EXPECT(ext4_lookup(fs, &root, (const uint8_t *)"payload.bin", 11, &inode), EXT4_OK);
	length = 4U * device->block_size + 39U;
	if (length > inode.size) {
		length = (size_t)inode.size;
	}
	CHECK(length != 0);
	expected = malloc(length);
	observed = malloc(length);
	CHECK(expected != NULL && observed != NULL);
	EXPECT(ext4_read(fs, &inode, 0, expected, length, &completed), EXT4_OK);
	CHECK(completed == length);
	EXPECT(ext4_hold_inode(fs, inode.number, inode.generation, &hold), EXT4_OK);
	EXPECT(ext4_hold_inode(fs, inode.number, inode.generation, &duplicate), EXT4_OK);
	CHECK(hold == duplicate);
	live = device->live;
	device->allocations = device->reads = 0;
	EXPECT(ext4_read_held(hold, 0, observed, length, &completed), EXT4_OK);
	CHECK(completed == length && memcmp(observed, expected, length) == 0);
	allocations = device->allocations;
	reads = device->reads;
	CHECK(allocations != 0 && reads != 0 && hold->reader != NULL);
	device->allocations = 0;
	device->fail_allocation = 1;
	EXPECT(ext4_read_held(duplicate, 0, observed, length, &completed), EXT4_OK);
	CHECK(completed == length && memcmp(observed, expected, length) == 0 &&
	    device->allocations == 0);
	device->fail_allocation = 0;
	device->reads = 0;
	EXPECT(ext4_read_held(hold, inode.size, observed, length, &completed), EXT4_OK);
	CHECK(completed == 0 && device->reads == 0);
	EXPECT(ext4_read_held(hold, UINT64_MAX, observed, length, &completed), EXT4_OK);
	CHECK(completed == 0 && device->reads == 0);
	EXPECT(ext4_read_held(hold, 0, NULL, 1, &completed), EXT4_INVALID_ARGUMENT);
	CHECK(completed == 0);
	EXPECT(ext4_read_held(NULL, 0, observed, length, &completed), EXT4_INVALID_ARGUMENT);
	EXPECT(ext4_read_held(hold, 0, observed, length, NULL), EXT4_INVALID_ARGUMENT);

	for (kind = 0; kind < 2U; kind++) {
		for (fault = 1; fault <= (kind == 0 ? allocations : reads); fault++) {
			ext4_drop_read_cache(hold);
			CHECK(device->live == live);
			device->allocations = device->reads = 0;
			device->fail_allocation = kind == 0 ? fault : 0;
			device->fail_read = kind == 1 ? fault : 0;
			EXPECT(ext4_read_held(hold, 0, observed, length, &completed),
			    kind == 0 ? EXT4_NO_MEMORY : EXT4_IO);
			CHECK(completed <= length);
			device->fail_allocation = device->fail_read = 0;
			EXPECT(ext4_read_held(hold, 0, observed, length, &completed), EXT4_OK);
			CHECK(completed == length && memcmp(observed, expected, length) == 0);
		}
	}
	/* An explicit refresh forgets even an unchanged inode's external leaves.
	 * A failed refresh must not leave a cached snapshot usable afterward. */
	EXPECT(ext4_refresh_inode(hold, &inode), EXT4_OK);
	CHECK(hold->reader == NULL && device->live == live);
	EXPECT(ext4_read_held(hold, 0, observed, length, &completed), EXT4_OK);
	device->fail_read = device->reads + 1U;
	EXPECT(ext4_refresh_inode(hold, &inode), EXT4_IO);
	CHECK(hold->reader == NULL && device->live == live);
	device->fail_read = 0;
	EXPECT(ext4_read_held(hold, 0, observed, length, &completed), EXT4_OK);
	CHECK(completed == length && memcmp(observed, expected, length) == 0);
	/* The cached revision is zero: wrapping back to zero must still discard it. */
	fs->read_revision = UINT64_MAX;
	ext4_read_cache_invalidate(fs);
	CHECK(fs->read_revision == 0 && hold->reader == NULL && device->live == live);
	EXPECT(ext4_read_held(hold, 0, observed, length, &completed), EXT4_OK);
	fs->aborted = true;
	observed[0] = 0xa5;
	EXPECT(ext4_read_held(hold, 0, observed, length, &completed), EXT4_RECOVERY_REQUIRED);
	CHECK(completed == 0 && observed[0] == 0xa5);
	fs->aborted = false;
	EXPECT(ext4_release_inode(duplicate), EXT4_OK);
	CHECK(hold->references == 1 && hold->reader != NULL);
	EXPECT(ext4_release_inode(hold), EXT4_OK);
	CHECK(fs->holds == NULL);
	EXPECT(ext4_hold_inode(fs, inode.number, inode.generation, &hold), EXT4_OK);
	EXPECT(ext4_read_held(hold, 0, observed, length, &completed), EXT4_OK);
	/* Unmount also owns cached holds that the adapter has not released. */
	ext4_unmount(fs);
	CHECK(device->live == 0 && device->writes == 0);
	free(observed);
	free(expected);
}

int
main(int argc, char **argv)
{
	struct device device;
	int index;

	CHECK(argc > 1);
	for (index = 1; index < argc; index++) {
		storage_open(&device, argv[index]);
		check_reads(&device);
		check_mappings(&device);
		storage_close(&device);
		printf("PASS held read faults, refresh, EOF, revision wrap and lifetime: %s\n",
		    argv[index]);
	}
	return 0;
}

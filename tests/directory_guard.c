/* SPDX-License-Identifier: BSD-3-Clause */
#include "directory_write.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(condition) \
	do { \
		if (!(condition)) { \
			fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition); \
			exit(1); \
		} \
	} while (0)

/* Admission is deliberately tested before lower layers: none of these calls
 * may allocate, read a block, or reach a transaction for this unsupported format. */
static void *
refuse_allocation(void *opaque, size_t size)
{
	unsigned int *calls = opaque;

	(void)size;
	(*calls)++;
	return NULL;
}

static void
check_guard(uint32_t block_size)
{
	struct ext4_fs fs = { 0 };
	struct ext4_allocation allocation = { .fs = &fs };
	struct ext4_inode parent = { .mode = EXT4_MODE_DIRECTORY,
		.flags = EXT4_INODE_ENCRYPT | EXT4_INODE_CASEFOLD | EXT4_INODE_INLINE_DATA };
	uint8_t saved_parent[sizeof(parent)];
	struct ext4_inode_disk disk = { 0 };
	uint8_t saved_disk[sizeof(disk)];
	struct ext4_directory_slot slot = { 0 };
	struct ext4_directory_request request = { .directory = &parent };
	uint8_t saved_slot[sizeof(slot)];
	unsigned int allocations = 0;

	memcpy(saved_parent, &parent, sizeof(parent));
	memcpy(saved_disk, &disk, sizeof(disk));
	memcpy(saved_slot, &slot, sizeof(slot));
	fs.info.block_size = block_size;
	fs.info.feature_incompat = EXT4_FEATURE_INCOMPAT_CASEFOLD |
	    EXT4_FEATURE_INCOMPAT_ENCRYPT;
	fs.environment.context = &allocations;
	fs.environment.allocate = refuse_allocation;
	CHECK(ext4_directory_scan(&allocation, &parent, &disk, NULL,
	    EXT4_DIRECTORY_EMPTY, 0, &slot) == EXT4_UNSUPPORTED);
	CHECK(ext4_directory_scan(&allocation, &parent, &disk, &request,
	    EXT4_DIRECTORY_FIND, 2, &slot) == EXT4_UNSUPPORTED);
	CHECK(ext4_directory_scan(&allocation, &parent, &disk, &request,
	    EXT4_DIRECTORY_INSERT, 0, &slot) == EXT4_UNSUPPORTED);
	CHECK(ext4_directory_insert(&allocation, &parent, &disk, &slot, 3,
	    EXT4_FT_REGULAR, &request) == EXT4_UNSUPPORTED);
	CHECK(ext4_directory_initialize(&allocation, &parent, &disk, 2) == EXT4_UNSUPPORTED);
	CHECK(ext4_directory_remove(&allocation, &parent, &slot) == EXT4_UNSUPPORTED);
	CHECK(ext4_directory_replace(&allocation, &parent, &slot, 4,
	    EXT4_FT_DIRECTORY) == EXT4_UNSUPPORTED);
	CHECK(allocations == 0);
	CHECK(memcmp(&parent, saved_parent, sizeof(parent)) == 0);
	CHECK(memcmp(&disk, saved_disk, sizeof(disk)) == 0);
	CHECK(memcmp(&slot, saved_slot, sizeof(slot)) == 0);
	/* EMPTY validates layout without allocating a name-comparison buffer. */
	parent.flags = EXT4_INODE_CASEFOLD;
	CHECK(ext4_directory_scan(&allocation, &parent, &disk, NULL,
	    EXT4_DIRECTORY_EMPTY, 0, &slot) == EXT4_CORRUPT);
	CHECK(allocations == 0);
	CHECK(ext4_directory_request_open(&fs, &parent, (const uint8_t *)"name", 4,
	    EXT4_NAME_REQUIRE_KEY, NULL, &request) == EXT4_NO_MEMORY);
	CHECK(allocations == 1);
	ext4_directory_request_close(&fs, &request);
}

int
main(void)
{
	check_guard(1024);
	check_guard(4096);
	check_guard(65536);
	puts("inline encrypted casefold directory mutation admission: passed");
	return 0;
}

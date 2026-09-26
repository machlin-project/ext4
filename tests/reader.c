/* SPDX-License-Identifier: BSD-3-Clause */
#include <ext4/ext4.h>
#include "image.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned int failures;

#define CHECK(condition)                                                                           \
	do {                                                                                       \
		if (!(condition)) {                                                                \
			fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition);            \
			failures++;                                                                \
			goto out;                                                                  \
		}                                                                                  \
	} while (0)

static enum ext4_result
lookup(
    struct ext4_fs *fs, const struct ext4_inode *parent, const char *name, struct ext4_inode *inode)
{
	return ext4_lookup(fs, parent, (const uint8_t *)name, strlen(name), inode);
}

static void
check_contents(
    struct ext4_fs *fs, struct ext4_inode *inode, const uint8_t *expected, size_t expected_size)
{
	uint8_t *buffer = NULL;
	size_t completed;
	size_t offset;
	size_t chunk;

	CHECK(inode->size == expected_size);
	buffer = malloc(expected_size + 16);
	CHECK(buffer != NULL);
	memset(buffer, 0xa7, expected_size + 16);
	CHECK(ext4_read(fs, inode, 0, buffer, expected_size + 16, &completed) == EXT4_OK);
	CHECK(completed == expected_size);
	CHECK(memcmp(buffer, expected, expected_size) == 0);
	CHECK(buffer[expected_size] == 0xa7);
	CHECK(ext4_read(fs, inode, expected_size, buffer, 1, &completed) == EXT4_OK);
	CHECK(completed == 0);
	CHECK(ext4_read(fs, inode, UINT64_MAX, buffer, 1, &completed) == EXT4_OK);
	CHECK(completed == 0);
	/* Uneven requests deliberately cross both filesystem-block and VM-page
	 * boundaries, including short reads at EOF. */
	for (offset = 0; offset < expected_size; offset += 997) {
		chunk = expected_size - offset;
		if (chunk > 8193) {
			chunk = 8193;
		}
		CHECK(ext4_read(fs, inode, offset, buffer, chunk, &completed) == EXT4_OK);
		CHECK(completed == chunk);
		CHECK(memcmp(buffer, expected + offset, chunk) == 0);
	}
out:
	free(buffer);
}

static void
check_reader(const char *path)
{
	struct ext4_posix_image image;
	struct ext4_fs *fs = NULL;
	struct ext4_inode root;
	struct ext4_inode inode;
	struct ext4_inode other;
	struct ext4_inode directory;
	struct ext4_dir_entry entry;
	struct ext4_info info;
	uint8_t *expected = NULL;
	uint8_t long_link[100];
	bool entries[400] = { false };
	char name[32];
	uint64_t cookie;
	size_t index;
	size_t extent_index;
	unsigned int entry_index;
	unsigned int count = 0;
	enum ext4_result error;

	CHECK(ext4_posix_open(&image, path) == EXT4_OK);
	error = ext4_mount(&image.environment, &fs);
	if (error != EXT4_OK) {
		fprintf(stderr, "%s: mount: %s\n", path, ext4_result_string(error));
	}
	CHECK(error == EXT4_OK);
	ext4_get_info(fs, &info);
	CHECK(info.blocks * info.block_size == 64U * 1024U * 1024U);
	CHECK(info.block_size == 1024 || info.block_size == 4096);
	CHECK(ext4_get_inode(fs, EXT4_ROOT_INODE, &root) == EXT4_OK);
	CHECK(lookup(fs, &root, "hello.txt", &inode) == EXT4_OK);
	check_contents(fs, &inode, (const uint8_t *)"Machlin ext4\n", 13);
	CHECK(lookup(fs, &root, "hello-hardlink", &other) == EXT4_OK);
	CHECK(other.number == inode.number && inode.links == 2);
	CHECK(lookup(fs, &root, "empty", &inode) == EXT4_OK);
	check_contents(fs, &inode, (const uint8_t *)"", 0);
	CHECK(lookup(fs, &root, "hello-link", &inode) == EXT4_OK);
	CHECK(inode.fast_symlink);
	check_contents(fs, &inode, (const uint8_t *)"hello.txt", 9);
	CHECK(lookup(fs, &root, "long-link", &inode) == EXT4_OK);
	CHECK(!inode.fast_symlink);
	memset(long_link, 'L', sizeof(long_link));
	check_contents(fs, &inode, long_link, sizeof(long_link));
	CHECK(lookup(fs, &root, "nested", &directory) == EXT4_OK);
	CHECK(lookup(fs, &directory, "child.txt", &inode) == EXT4_OK);
	check_contents(fs, &inode, (const uint8_t *)"nested data\n", 12);
	CHECK(lookup(fs, &root, "payload.bin", &inode) == EXT4_OK);
	expected = malloc(2U * 1024U * 1024U);
	CHECK(expected != NULL);
	for (index = 0; index < 200000; index++) {
		expected[index] = (uint8_t)((index * 17 + 23) & 255U);
	}
	check_contents(fs, &inode, expected, 200000);
	CHECK(lookup(fs, &root, "sparse.bin", &inode) == EXT4_OK);
	memset(expected, 0, 2U * 1024U * 1024U);
	for (extent_index = 0; extent_index < 12; extent_index++) {
		memset(expected + extent_index * 65536, (int)extent_index + 1, 4096);
	}
	check_contents(fs, &inode, expected, 2U * 1024U * 1024U);
	CHECK(lookup(fs, &root, "many", &directory) == EXT4_OK);
	cookie = 0;
	while ((error = ext4_next_dir(fs, &directory, &cookie, &entry)) == EXT4_OK) {
		if (strcmp((const char *)entry.name, ".") == 0 ||
		    strcmp((const char *)entry.name, "..") == 0) {
			continue;
		}
		CHECK(sscanf((const char *)entry.name, "entry-%u", &entry_index) == 1);
		CHECK(entry_index < 400 && !entries[entry_index]);
		entries[entry_index] = true;
		count++;
	}
	CHECK(error == EXT4_NOT_FOUND && count == 400);
	for (entry_index = 0; entry_index < 400; entry_index += 37) {
		snprintf(name, sizeof(name), "entry-%04u", entry_index);
		CHECK(lookup(fs, &directory, name, &inode) == EXT4_OK);
		snprintf(name, sizeof(name), "%u\n", entry_index);
		check_contents(fs, &inode, (const uint8_t *)name, strlen(name));
	}
	CHECK(lookup(fs, &root, "no-such-entry", &inode) == EXT4_NOT_FOUND);
	cookie = 1;
	CHECK(ext4_next_dir(fs, &root, &cookie, &entry) == EXT4_CORRUPT);
	CHECK(ext4_get_inode(fs, 0, &inode) == EXT4_INVALID_ARGUMENT);
	CHECK(ext4_lookup(fs, &root, (const uint8_t *)"bad/name", 8, &inode) ==
	    EXT4_INVALID_ARGUMENT);
out:
	free(expected);
	ext4_unmount(fs);
	if (image.live_allocations != 0) {
		fprintf(stderr, "%s: leaked %llu allocations\n", path,
		    (unsigned long long)image.live_allocations);
		failures++;
	}
	ext4_posix_close(&image);
}

static void
check_mount_faults(const char *path)
{
	struct ext4_posix_image image;
	struct ext4_fs *fs = NULL;
	uint64_t allocation_count;
	uint64_t read_count;
	uint64_t fault;

	CHECK(ext4_posix_open(&image, path) == EXT4_OK);
	CHECK(ext4_mount(&image.environment, &fs) == EXT4_OK);
	allocation_count = image.allocation_calls;
	read_count = image.read_calls;
	ext4_unmount(fs);
	fs = NULL;
	CHECK(image.live_allocations == 0);
	for (fault = 1; fault <= allocation_count; fault++) {
		image.allocation_calls = 0;
		image.fail_allocation_at = fault;
		CHECK(ext4_mount(&image.environment, &fs) == EXT4_NO_MEMORY);
		CHECK(fs == NULL && image.live_allocations == 0);
	}
	image.fail_allocation_at = 0;
	for (fault = 1; fault <= read_count; fault++) {
		image.read_calls = 0;
		image.fail_read_at = fault;
		CHECK(ext4_mount(&image.environment, &fs) == EXT4_IO);
		CHECK(fs == NULL && image.live_allocations == 0);
	}
out:
	ext4_unmount(fs);
	ext4_posix_close(&image);
}

int
main(int argc, char **argv)
{
	int index;

	if (argc < 2) {
		fprintf(stderr, "usage: ext4-reader-test IMAGE...\n");
		return 2;
	}
	for (index = 1; index < argc; index++) {
		check_reader(argv[index]);
		check_mount_faults(argv[index]);
	}
	if (failures != 0) {
		fprintf(stderr, "FAIL: %u reader assertions\n", failures);
		return 1;
	}
	printf("PASS: image reading, links, sparse data, indexed directories and mount failures\n");
	return 0;
}

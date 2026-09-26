/* SPDX-License-Identifier: BSD-3-Clause */
#define _DARWIN_C_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/mount.h>
#include <unistd.h>

#define PAYLOAD_SIZE 200000U

#define CHECK(condition)                                                                           \
	do {                                                                                       \
		if (!(condition)) {                                                                \
			fprintf(stderr, "%s:%d: %s (errno=%d)\n", __FILE__, __LINE__, #condition,  \
			    errno);                                                                \
			failed = true;                                                             \
			goto out;                                                                  \
		}                                                                                  \
	} while (0)

int
main(int argc, char **argv)
{
	struct statfs filesystem;
	uint8_t *mapping = MAP_FAILED;
	char bytes[13];
	int directory = -1;
	int file = -1;
	bool failed = false;

	if (argc != 2 || geteuid() != 0) {
		fprintf(stderr, "usage (root): ext4-mounted-lifetime-test MOUNTPOINT\n");
		return 2;
	}
	CHECK(statfs(argv[1], &filesystem) == 0);
	CHECK(strcmp(filesystem.f_mntonname, argv[1]) == 0);
	CHECK((filesystem.f_flags & MNT_RDONLY) != 0);
	directory = open(argv[1], O_RDONLY | O_DIRECTORY);
	CHECK(directory >= 0);
	file = openat(directory, "hello.txt", O_RDONLY);
	CHECK(file >= 0);
	CHECK(close(directory) == 0);
	directory = -1;
	errno = 0;
	CHECK(unmount(argv[1], 0) == -1 && errno == EBUSY);
	CHECK(pread(file, bytes, sizeof(bytes), 0) == sizeof(bytes));
	CHECK(memcmp(bytes, "Machlin ext4\n", sizeof(bytes)) == 0);
	CHECK(close(file) == 0);
	file = -1;
	directory = open(argv[1], O_RDONLY | O_DIRECTORY);
	CHECK(directory >= 0);
	file = openat(directory, "payload.bin", O_RDONLY);
	CHECK(file >= 0);
	mapping = mmap(NULL, PAYLOAD_SIZE, PROT_READ, MAP_SHARED, file, 0);
	CHECK(mapping != MAP_FAILED);
	CHECK(close(file) == 0);
	file = -1;
	CHECK(close(directory) == 0);
	directory = -1;
	/* The mapping alone must keep the volume busy, before any page-in. */
	errno = 0;
	CHECK(unmount(argv[1], 0) == -1 && errno == EBUSY);
	CHECK(mapping[0] == 23U);
	CHECK(mapping[PAYLOAD_SIZE - 1] == (uint8_t)(((PAYLOAD_SIZE - 1) * 17U + 23U) & 255U));
	CHECK(munmap(mapping, PAYLOAD_SIZE) == 0);
	mapping = MAP_FAILED;
	CHECK(unmount(argv[1], 0) == 0);
	printf(
	    "PASS: open files and mappings retain the mount; final close/unmap permits unmount\n");
out:
	if (mapping != MAP_FAILED) {
		munmap(mapping, PAYLOAD_SIZE);
	}
	if (file >= 0) {
		close(file);
	}
	if (directory >= 0) {
		close(directory);
	}
	return failed ? 1 : 0;
}

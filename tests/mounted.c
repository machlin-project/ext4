/* SPDX-License-Identifier: BSD-3-Clause */
#define _DARWIN_C_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>

#define PAYLOAD_SIZE 200000U
#define SPARSE_SIZE (2U * 1024U * 1024U)
#define WORKER_COUNT 8U
#define DIRECTORY_COUNT 400U

static unsigned int failures;

#define CHECK(condition)                                                                           \
	do {                                                                                       \
		if (!(condition)) {                                                                \
			fprintf(stderr, "%s:%d: %s (errno=%d)\n", __FILE__, __LINE__, #condition,  \
			    errno);                                                                \
			failures++;                                                                \
			goto out;                                                                  \
		}                                                                                  \
	} while (0)

struct reader_worker {
	int root;
	unsigned int index;
	int error;
};

static uint8_t
payload_byte(size_t offset)
{
	return (uint8_t)((offset * 17U + 23U) & 255U);
}

static void *
read_worker(void *context)
{
	struct reader_worker *worker = context;
	uint8_t buffer[8193];
	size_t offset;
	size_t length;
	size_t index;
	unsigned int iteration;
	int fd = -1;
	ssize_t completed;

	for (iteration = 0; iteration < 64; iteration++) {
		fd = openat(worker->root, "payload.bin", O_RDONLY);
		if (fd < 0) {
			worker->error = errno;
			break;
		}
		offset = (iteration * 1009U + worker->index * 997U) % PAYLOAD_SIZE;
		length = PAYLOAD_SIZE - offset;
		if (length > sizeof(buffer)) {
			length = sizeof(buffer);
		}
		completed = pread(fd, buffer, length, (off_t)offset);
		if (completed != (ssize_t)length) {
			worker->error = EIO;
		}
		for (index = 0; index < length && worker->error == 0; index++) {
			if (buffer[index] != payload_byte(offset + index)) {
				worker->error = EIO;
			}
		}
		if (close(fd) != 0) {
			worker->error = errno;
		}
		fd = -1;
		if (worker->error != 0) {
			break;
		}
	}
	if (fd >= 0) {
		close(fd);
	}
	return NULL;
}

static void
check_mounted(const char *path)
{
	struct stat hello;
	struct stat hardlink;
	struct stat metadata;
	struct statvfs filesystem;
	struct dirent *entry;
	struct reader_worker workers[WORKER_COUNT];
	pthread_t threads[WORKER_COUNT];
	bool seen[DIRECTORY_COUNT] = { false };
	DIR *directory = NULL;
	uint8_t *mapping = MAP_FAILED;
	uint8_t *sparse = NULL;
	char link[128];
	char bytes[64];
	char trailing;
	unsigned int count = 0;
	unsigned int number;
	unsigned int started = 0;
	unsigned int joined = 0;
	size_t index;
	size_t extent;
	uint8_t expected;
	int root = -1;
	int fd = -1;
	int directory_fd = -1;

	root = open(path, O_RDONLY | O_DIRECTORY);
	CHECK(root >= 0);
	CHECK(fstatvfs(root, &filesystem) == 0);
	CHECK((filesystem.f_flag & ST_RDONLY) != 0);
	CHECK(fstatat(root, "hello.txt", &hello, 0) == 0);
	CHECK(fstatat(root, "hello-hardlink", &hardlink, 0) == 0);
	CHECK(S_ISREG(hello.st_mode) && hello.st_size == 13);
	CHECK(hello.st_ino == hardlink.st_ino && hello.st_nlink == 2);
	fd = openat(root, "hello-link", O_RDONLY);
	CHECK(fd >= 0);
	CHECK(read(fd, bytes, sizeof(bytes)) == 13);
	CHECK(memcmp(bytes, "Machlin ext4\n", 13) == 0);
	CHECK(read(fd, bytes, sizeof(bytes)) == 0);
	CHECK(close(fd) == 0);
	fd = -1;
	CHECK(fstatat(root, "hello-link", &metadata, AT_SYMLINK_NOFOLLOW) == 0);
	CHECK(S_ISLNK(metadata.st_mode));
	CHECK(readlinkat(root, "hello-link", link, sizeof(link)) == 9);
	CHECK(memcmp(link, "hello.txt", 9) == 0);
	CHECK(readlinkat(root, "long-link", link, sizeof(link)) == 100);
	for (index = 0; index < 100; index++) {
		CHECK(link[index] == 'L');
	}
	fd = openat(root, "nested/child.txt", O_RDONLY);
	CHECK(fd >= 0);
	CHECK(read(fd, bytes, sizeof(bytes)) == 12);
	CHECK(memcmp(bytes, "nested data\n", 12) == 0);
	CHECK(close(fd) == 0);
	fd = openat(root, "empty", O_RDONLY);
	CHECK(fd >= 0);
	CHECK(read(fd, bytes, sizeof(bytes)) == 0);
	CHECK(close(fd) == 0);
	fd = openat(root, "payload.bin", O_RDONLY);
	CHECK(fd >= 0);
	mapping = mmap(NULL, PAYLOAD_SIZE, PROT_READ, MAP_PRIVATE, fd, 0);
	CHECK(mapping != MAP_FAILED);
	for (index = 0; index < PAYLOAD_SIZE; index++) {
		CHECK(mapping[index] == payload_byte(index));
	}
	CHECK(munmap(mapping, PAYLOAD_SIZE) == 0);
	mapping = MAP_FAILED;
	CHECK(close(fd) == 0);
	fd = openat(root, "sparse.bin", O_RDONLY);
	CHECK(fd >= 0);
	CHECK(fstat(fd, &metadata) == 0);
	CHECK(metadata.st_size == SPARSE_SIZE && metadata.st_blocks * 512 < SPARSE_SIZE);
	sparse = malloc(SPARSE_SIZE);
	CHECK(sparse != NULL);
	CHECK(pread(fd, sparse, SPARSE_SIZE, 0) == SPARSE_SIZE);
	for (index = 0; index < SPARSE_SIZE; index++) {
		extent = index / 65536U;
		expected = extent < 12 && index % 65536U < 4096U ? (uint8_t)(extent + 1) : 0;
		CHECK(sparse[index] == expected);
	}
	CHECK(close(fd) == 0);
	fd = -1;
	directory_fd = openat(root, "many", O_RDONLY | O_DIRECTORY);
	CHECK(directory_fd >= 0);
	directory = fdopendir(directory_fd);
	CHECK(directory != NULL);
	directory_fd = -1;
	errno = 0;
	while ((entry = readdir(directory)) != NULL) {
		if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
			continue;
		}
		CHECK(sscanf(entry->d_name, "entry-%u%c", &number, &trailing) == 1);
		CHECK(number < DIRECTORY_COUNT && !seen[number]);
		seen[number] = true;
		count++;
	}
	CHECK(errno == 0 && count == DIRECTORY_COUNT);
	CHECK(closedir(directory) == 0);
	directory = NULL;
	for (index = 0; index < WORKER_COUNT; index++) {
		workers[index].root = root;
		workers[index].index = (unsigned int)index;
		workers[index].error = 0;
		CHECK(pthread_create(&threads[index], NULL, read_worker, &workers[index]) == 0);
		started++;
	}
	while (joined < started) {
		CHECK(pthread_join(threads[joined], NULL) == 0);
		joined++;
		CHECK(workers[joined - 1].error == 0);
	}
	errno = 0;
	fd = openat(root, "hello.txt", O_WRONLY);
	CHECK(fd == -1 && errno == EROFS);
	errno = 0;
	fd = openat(root, "must-not-be-created", O_CREAT | O_EXCL | O_WRONLY, 0600);
	CHECK(fd == -1 && errno == EROFS);
out:
	while (joined < started) {
		pthread_join(threads[joined], NULL);
		joined++;
	}
	if (directory != NULL) {
		closedir(directory);
	}
	if (directory_fd >= 0) {
		close(directory_fd);
	}
	if (fd >= 0) {
		close(fd);
	}
	if (root >= 0) {
		close(root);
	}
	if (mapping != MAP_FAILED) {
		munmap(mapping, PAYLOAD_SIZE);
	}
	free(sparse);
}

int
main(int argc, char **argv)
{
	if (argc != 2) {
		fprintf(stderr, "usage: ext4-mounted-test MOUNTPOINT\n");
		return 2;
	}
	check_mounted(argv[1]);
	if (failures != 0) {
		fprintf(stderr, "FAIL: %u mounted filesystem assertions\n", failures);
		return 1;
	}
	printf("PASS: mounted reads, metadata, directories, links, sparse data, mmap, concurrent "
	       "I/O and read-only enforcement\n");
	return 0;
}

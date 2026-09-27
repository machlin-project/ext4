/* SPDX-License-Identifier: BSD-3-Clause */
#define _DARWIN_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/types.h>
#include <unistd.h>

#define TARGET_COUNT 6U
#define WORKER_COUNT 4U
#define INLINE_CAPACITY 60U
#define SECTOR_SIZE 512U
#define TEST_UID (UINT32_MAX - 2U)
#define TEST_GID UINT32_C(0x81234567)

struct reader {
	int root;
	size_t block_size;
	bool passed;
};

static size_t
target_length(size_t block_size, unsigned int index)
{
	const size_t lengths[TARGET_COUNT] = { 1, INLINE_CAPACITY - 1, INLINE_CAPACITY,
		INLINE_CAPACITY + 1, block_size - 1, 15 };

	return lengths[index];
}

static void
target_bytes(uint8_t *bytes, size_t length, bool binary)
{
	size_t index;

	for (index = 0; index < length; index++) {
		bytes[index] = (uint8_t)(index % 17 == 16 ? '/' : 'a' + index % 26);
		if (binary && index % 7 == 0) {
			bytes[index] = (uint8_t)(0x80U + index % 128);
		}
	}
}

static bool
read_targets(int root, size_t block_size, uint8_t *expected, uint8_t *actual)
{
	char name[32];
	size_t capacities[] = { 1, INLINE_CAPACITY - 1, INLINE_CAPACITY, 1024, block_size + 13 };
	size_t length;
	size_t count;
	size_t capacity;
	unsigned int index;
	unsigned int alias;

	for (index = 0; index < TARGET_COUNT; index++) {
		length = target_length(block_size, index);
		target_bytes(expected, length, index == TARGET_COUNT - 1);
		for (alias = 0; alias < 2; alias++) {
			snprintf(name, sizeof(name), "symbolic-%s%u", alias ? "alias-" : "", index);
			for (capacity = 0; capacity < sizeof(capacities) / sizeof(capacities[0]);
			    capacity++) {
				count =
				    length < capacities[capacity] ? length : capacities[capacity];
				memset(actual, 0xa5, block_size + 14);
				if (readlinkat(root, name, (char *)actual, capacities[capacity]) !=
					(ssize_t)count ||
				    memcmp(actual, expected, count) != 0 || actual[count] != 0xa5) {
					fprintf(stderr,
					    "readlink mismatch: %s capacity=%zu errno=%d\n", name,
					    capacities[capacity], errno);
					return false;
				}
			}
		}
	}
	return true;
}

static void *
reader_thread(void *context)
{
	struct reader *reader = context;
	uint8_t *expected = malloc(reader->block_size);
	uint8_t *actual = malloc(reader->block_size + 14);
	unsigned int iteration;

	reader->passed = expected != NULL && actual != NULL;
	for (iteration = 0; iteration < 32 && reader->passed; iteration++) {
		reader->passed = read_targets(reader->root, reader->block_size, expected, actual);
	}
	free(actual);
	free(expected);
	return NULL;
}

#define CHECK(condition)                                                                           \
	do {                                                                                       \
		if (!(condition)) {                                                                \
			fprintf(stderr, "%s:%d: %s (errno=%d)\n", __FILE__, __LINE__, #condition,  \
			    errno);                                                                \
			goto out;                                                                  \
		}                                                                                  \
	} while (0)

int
main(int argc, char **argv)
{
	struct stat inode;
	struct stat alias;
	struct statvfs filesystem;
	struct reader readers[WORKER_COUNT];
	pthread_t threads[WORKER_COUNT];
	ino_t numbers[TARGET_COUNT];
	char name[32];
	char alias_name[32];
	char *end;
	unsigned long block_size;
	unsigned int index;
	unsigned int other;
	unsigned int started = 0;
	unsigned int joined = 0;
	int root = -1;
	int result = EXIT_FAILURE;

	if (argc != 3) {
		fprintf(stderr, "usage: %s READ_ONLY_MOUNTPOINT BLOCK_SIZE\n", argv[0]);
		return EXIT_FAILURE;
	}
	errno = 0;
	block_size = strtoul(argv[2], &end, 10);
	CHECK(errno == 0 && *argv[2] != 0 && *end == 0 && block_size >= 1024 &&
	    block_size <= 65536 && (block_size & (block_size - 1)) == 0);
	root = open(argv[1], O_RDONLY | O_DIRECTORY);
	CHECK(root >= 0 && fstatvfs(root, &filesystem) == 0);
	CHECK((filesystem.f_flag & ST_RDONLY) != 0);
	for (index = 0; index < TARGET_COUNT; index++) {
		snprintf(name, sizeof(name), "symbolic-%u", index);
		snprintf(alias_name, sizeof(alias_name), "symbolic-alias-%u", index);
		CHECK(fstatat(root, name, &inode, AT_SYMLINK_NOFOLLOW) == 0);
		CHECK(fstatat(root, alias_name, &alias, AT_SYMLINK_NOFOLLOW) == 0);
		CHECK(S_ISLNK(inode.st_mode) && (inode.st_mode & 07777) == 0777);
		CHECK(inode.st_uid == TEST_UID && inode.st_gid == TEST_GID && inode.st_nlink == 2);
		CHECK(inode.st_size == (off_t)target_length(block_size, index));
		CHECK(inode.st_blocks ==
		    (blkcnt_t)(inode.st_size < INLINE_CAPACITY ? 0 : block_size / SECTOR_SIZE));
		CHECK(alias.st_ino == inode.st_ino && alias.st_mode == inode.st_mode &&
		    alias.st_size == inode.st_size && alias.st_nlink == inode.st_nlink);
		for (other = 0; other < index; other++) {
			CHECK(numbers[other] != inode.st_ino);
		}
		numbers[index] = inode.st_ino;
	}
	for (index = 0; index < WORKER_COUNT; index++) {
		readers[index].root = root;
		readers[index].block_size = block_size;
		readers[index].passed = false;
		CHECK(pthread_create(&threads[index], NULL, reader_thread, &readers[index]) == 0);
		started++;
	}
	while (joined < started) {
		CHECK(pthread_join(threads[joined], NULL) == 0);
		joined++;
		CHECK(readers[joined - 1].passed);
	}
	CHECK(close(root) == 0);
	root = -1;
	printf("PASS mounted symbolic links: targets=%u aliases=%u concurrent_readlinks=%u "
	       "block_size=%lu\n",
	    TARGET_COUNT, TARGET_COUNT, WORKER_COUNT * 32 * TARGET_COUNT * 2 * 5, block_size);
	result = EXIT_SUCCESS;
out:
	while (joined < started) {
		pthread_join(threads[joined++], NULL);
	}
	if (root >= 0) {
		close(root);
	}
	return result;
}

/* SPDX-License-Identifier: BSD-3-Clause */
#define _DARWIN_C_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <unistd.h>

#define CACHE_ROUNDS 16U
#define REPLACE_ROUNDS 64U
#define READER_COUNT 3U
#define FINAL_PATTERN 101U
#define EPOCH_MAGIC UINT64_C(0x4d4143484c494e34)

struct epoch_header {
	uint64_t magic;
	uint32_t sequence;
	uint32_t reserved;
};

struct rendezvous {
	pthread_mutex_t mutex;
	pthread_cond_t condition;
	unsigned arrived;
	unsigned phase;
};

struct reader {
	struct rendezvous *gate;
	size_t page_size;
	int directory;
};

#define CHECK(condition)                                                                           \
	do {                                                                                       \
		if (!(condition)) {                                                                \
			fprintf(stderr, "%s:%d: %s (errno=%d: %s)\n", __FILE__, __LINE__,          \
			    #condition, errno, strerror(errno));                                   \
			exit(EXIT_FAILURE);                                                        \
		}                                                                                  \
	} while (0)

static void
fill_pattern(uint8_t *bytes, size_t length, unsigned sequence)
{
	size_t index;

	for (index = 0; index < length; index++) {
		bytes[index] = (uint8_t)((index * 17U + sequence * 29U) ^ (index >> 9));
	}
}

static size_t
epoch_size(size_t page_size, unsigned sequence)
{
	return (3U + sequence % 2U) * page_size + 17U;
}

static mode_t
epoch_mode(unsigned sequence)
{
	return sequence % 2U == 0 ? 0600 : 0640;
}

static void
fill_epoch(uint8_t *bytes, size_t length, unsigned sequence)
{
	struct epoch_header header = { .magic = EPOCH_MAGIC, .sequence = sequence };

	fill_pattern(bytes, length, sequence);
	memcpy(bytes, &header, sizeof(header));
}

static void
rendezvous(struct rendezvous *gate)
{
	unsigned phase;

	CHECK(pthread_mutex_lock(&gate->mutex) == 0);
	phase = gate->phase;
	gate->arrived++;
	if (gate->arrived == READER_COUNT + 1U) {
		gate->arrived = 0;
		gate->phase++;
		CHECK(pthread_cond_broadcast(&gate->condition) == 0);
	} else {
		while (phase == gate->phase) {
			CHECK(pthread_cond_wait(&gate->condition, &gate->mutex) == 0);
		}
	}
	CHECK(pthread_mutex_unlock(&gate->mutex) == 0);
}

static void
check_bytes(int file, uint8_t *scratch, const uint8_t *expected, size_t length)
{
	struct stat status;
	uint8_t extra;

	CHECK(fstat(file, &status) == 0 && status.st_size == (off_t)length);
	CHECK(pread(file, scratch, length, 0) == (ssize_t)length);
	CHECK(memcmp(scratch, expected, length) == 0);
	CHECK(pread(file, &extra, sizeof(extra), (off_t)length) == 0);
}

static void
check_coherence(int file, int alias, const uint8_t *mapping, const uint8_t *expected,
    uint8_t *scratch, size_t length, size_t mapping_size)
{
	size_t mapped = length < mapping_size ? length : mapping_size;

	check_bytes(file, scratch, expected, length);
	check_bytes(alias, scratch, expected, length);
	CHECK(memcmp(mapping, expected, mapped) == 0);
}

static void
check_cached_mutations(int directory, size_t page_size)
{
	size_t capacity = 7U * page_size + 31U;
	size_t length = 5U * page_size + 17U;
	size_t mapping_size = 4U * page_size;
	size_t shorter = 2U * page_size + 17U;
	uint8_t *expected = calloc(1, capacity);
	uint8_t *scratch = malloc(capacity);
	uint8_t *read_mapping;
	uint8_t *write_mapping;
	unsigned round;
	int file;
	int alias;

	CHECK(expected != NULL && scratch != NULL);
	fill_pattern(expected, length, 3U);
	file = openat(directory, "coherent", O_CREAT | O_EXCL | O_RDWR | O_NOFOLLOW, 0600);
	CHECK(file >= 0);
	CHECK(pwrite(file, expected, length, 0) == (ssize_t)length && fsync(file) == 0);
	alias = openat(directory, "coherent", O_RDONLY | O_NOFOLLOW);
	CHECK(alias >= 0);
	read_mapping = mmap(NULL, mapping_size, PROT_READ, MAP_SHARED, alias, 0);
	write_mapping = mmap(NULL, mapping_size, PROT_READ | PROT_WRITE, MAP_SHARED, file, 0);
	CHECK(read_mapping != MAP_FAILED && write_mapping != MAP_FAILED);
	check_coherence(file, alias, read_mapping, expected, scratch, length, mapping_size);
	for (round = 0; round < CACHE_ROUNDS; round++) {
		size_t grown = (round % 2U == 0 ? 5U : 7U) * page_size + 31U;

		CHECK(ftruncate(file, (off_t)grown) == 0);
		memset(expected + length, 0, grown - length);
		length = grown;
		fill_pattern(expected + page_size, page_size, 64U + round);
		CHECK(pwrite(file, expected + page_size, page_size, (off_t)page_size) ==
		    (ssize_t)page_size);
		CHECK(fsync(file) == 0);
		check_coherence(file, alias, read_mapping, expected, scratch, length, mapping_size);
		memset(write_mapping, (int)(0x80U + round), page_size);
		memset(expected, (int)(0x80U + round), page_size);
		CHECK(msync(write_mapping, mapping_size, MS_SYNC) == 0 && fsync(file) == 0);
		check_coherence(file, alias, read_mapping, expected, scratch, length, mapping_size);
		CHECK(ftruncate(file, (off_t)shorter) == 0);
		check_coherence(
		    file, alias, read_mapping, expected, scratch, shorter, mapping_size);
		length = 5U * page_size + 17U;
		CHECK(ftruncate(file, (off_t)length) == 0);
		memset(expected + shorter, 0, length - shorter);
		CHECK(fsync(file) == 0);
		check_coherence(file, alias, read_mapping, expected, scratch, length, mapping_size);
	}
	fill_pattern(expected, length, FINAL_PATTERN);
	CHECK(pwrite(file, expected, length, 0) == (ssize_t)length && fsync(file) == 0);
	check_coherence(file, alias, read_mapping, expected, scratch, length, mapping_size);
	CHECK(munmap(read_mapping, mapping_size) == 0 && munmap(write_mapping, mapping_size) == 0);
	CHECK(close(alias) == 0 && close(file) == 0);
	free(scratch);
	free(expected);
	printf("{\"phase\":\"cache\",\"rounds\":%u,\"page_size\":%zu,\"passed\":true}\n",
	    CACHE_ROUNDS, page_size);
	fflush(stdout);
}

static ino_t
publish_epoch(int directory, size_t page_size, unsigned sequence)
{
	size_t length = epoch_size(page_size, sequence);
	uint8_t *bytes = malloc(length);
	struct stat status;
	int file;

	CHECK(bytes != NULL);
	fill_epoch(bytes, length, sequence);
	file = openat(directory, "next", O_CREAT | O_EXCL | O_RDWR | O_NOFOLLOW, 0600);
	CHECK(file >= 0 && fchmod(file, epoch_mode(sequence)) == 0);
	CHECK(pwrite(file, bytes, length, 0) == (ssize_t)length && fsync(file) == 0);
	CHECK(fstat(file, &status) == 0);
	CHECK(renameat(directory, "next", directory, "current") == 0 && close(file) == 0);
	free(bytes);
	return status.st_ino;
}

static void
check_epoch(int file, uint8_t *scratch, uint8_t *expected, size_t page_size, unsigned sequence,
    nlink_t links)
{
	struct stat status;
	size_t length = epoch_size(page_size, sequence);

	fill_epoch(expected, length, sequence);
	check_bytes(file, scratch, expected, length);
	CHECK(fstat(file, &status) == 0 && status.st_nlink == links);
	CHECK((status.st_mode & 0777) == epoch_mode(sequence));
	CHECK(status.st_uid == geteuid());
}

static void *
read_replaced_epochs(void *context)
{
	struct reader *reader = context;
	size_t capacity = epoch_size(reader->page_size, 1U);
	uint8_t *scratch = malloc(capacity);
	uint8_t *expected = malloc(capacity);
	uint8_t *mapping;
	size_t length;
	unsigned round;
	int file;

	CHECK(scratch != NULL && expected != NULL);
	for (round = 1; round <= REPLACE_ROUNDS; round++) {
		file = openat(reader->directory, "current", O_RDONLY | O_NOFOLLOW);
		CHECK(file >= 0);
		length = epoch_size(reader->page_size, round - 1U);
		mapping = mmap(NULL, length, PROT_READ, MAP_SHARED, file, 0);
		CHECK(mapping != MAP_FAILED);
		/* Every reader owns the old inode before its name is replaced. */
		rendezvous(reader->gate);
		rendezvous(reader->gate);
		check_epoch(file, scratch, expected, reader->page_size, round - 1U, 0);
		CHECK(close(file) == 0);
		/* Fault the old mapping after the last descriptor owned by this reader
		 * closes. Neither the new name nor its data may replace this object. */
		CHECK(memcmp(mapping, expected, length) == 0);
		CHECK(munmap(mapping, length) == 0);
		file = openat(reader->directory, "current", O_RDONLY | O_NOFOLLOW);
		CHECK(file >= 0);
		check_epoch(file, scratch, expected, reader->page_size, round, 1);
		CHECK(close(file) == 0);
		rendezvous(reader->gate);
	}
	free(expected);
	free(scratch);
	return NULL;
}

static void
check_replacement_lifetime(int directory, size_t page_size)
{
	struct rendezvous gate = { .mutex = PTHREAD_MUTEX_INITIALIZER,
		.condition = PTHREAD_COND_INITIALIZER };
	struct reader reader = { .gate = &gate, .page_size = page_size, .directory = directory };
	pthread_t readers[READER_COUNT];
	ino_t previous;
	ino_t current;
	unsigned index;
	unsigned round;

	previous = publish_epoch(directory, page_size, 0);
	printf(
	    "{\"phase\":\"replacement-progress\",\"completed\":0,\"pid\":%ld}\n", (long)getpid());
	fflush(stdout);
	for (index = 0; index < READER_COUNT; index++) {
		CHECK(pthread_create(&readers[index], NULL, read_replaced_epochs, &reader) == 0);
	}
	for (round = 1; round <= REPLACE_ROUNDS; round++) {
		rendezvous(&gate);
		current = publish_epoch(directory, page_size, round);
		CHECK(current != previous);
		previous = current;
		rendezvous(&gate);
		rendezvous(&gate);
		if (round % 8U == 0) {
			printf("{\"phase\":\"replacement-progress\",\"completed\":%u}\n", round);
			fflush(stdout);
		}
	}
	for (index = 0; index < READER_COUNT; index++) {
		CHECK(pthread_join(readers[index], NULL) == 0);
	}
	CHECK(
	    pthread_cond_destroy(&gate.condition) == 0 && pthread_mutex_destroy(&gate.mutex) == 0);
	printf("{\"phase\":\"replacement\",\"rounds\":%u,\"readers\":%u,\"passed\":true}\n",
	    REPLACE_ROUNDS, READER_COUNT);
	fflush(stdout);
}

static void
check_persisted(int directory, size_t page_size)
{
	size_t length = 5U * page_size + 17U;
	uint8_t *expected = malloc(length);
	uint8_t *scratch = malloc(length);
	struct stat status;
	int file;

	CHECK(expected != NULL && scratch != NULL);
	fill_pattern(expected, length, FINAL_PATTERN);
	file = openat(directory, "coherent", O_RDONLY | O_NOFOLLOW);
	CHECK(file >= 0);
	check_bytes(file, scratch, expected, length);
	CHECK(fstat(file, &status) == 0 && status.st_nlink == 1 &&
	    (status.st_mode & 0777) == 0600 && status.st_uid == geteuid());
	CHECK(close(file) == 0);
	file = openat(directory, "current", O_RDONLY | O_NOFOLLOW);
	CHECK(file >= 0);
	check_epoch(file, scratch, expected, page_size, REPLACE_ROUNDS, 1);
	CHECK(close(file) == 0);
	CHECK(fstatat(directory, "next", &status, AT_SYMLINK_NOFOLLOW) == -1 && errno == ENOENT);
	free(scratch);
	free(expected);
	puts("{\"phase\":\"persisted\",\"passed\":true}");
}

int
main(int argc, char **argv)
{
	struct statfs filesystem;
	long native_page;
	size_t page_size;
	int root;
	int directory;
	int verify;

	CHECK(argc == 3 && (strcmp(argv[2], "write") == 0 || strcmp(argv[2], "verify") == 0));
	CHECK(geteuid() != 0);
	verify = strcmp(argv[2], "verify") == 0;
	native_page = sysconf(_SC_PAGESIZE);
	CHECK(native_page >= 4096 && native_page <= 65536);
	page_size = (size_t)native_page;
	root = open(argv[1], O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
	CHECK(root >= 0 && fstatfs(root, &filesystem) == 0);
	CHECK(strcmp(filesystem.f_fstypename, "machlinext4") == 0);
	CHECK((filesystem.f_flags & MNT_IGNORE_OWNERSHIP) == 0);
	CHECK(((filesystem.f_flags & MNT_RDONLY) != 0) == verify);
	if (!verify) {
		CHECK(mkdirat(root, "acceptance-cache", 0700) == 0);
	}
	directory = openat(root, "acceptance-cache", O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
	CHECK(directory >= 0);
	if (!verify) {
		check_cached_mutations(directory, page_size);
		check_replacement_lifetime(directory, page_size);
	}
	check_persisted(directory, page_size);
	CHECK(close(directory) == 0 && close(root) == 0);
	return EXIT_SUCCESS;
}

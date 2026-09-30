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
#include <sys/xattr.h>
#include <unistd.h>

#define DATA_SIZE 131073U
#define WORKER_COUNT 4U
#define WORKER_WRITES 8U
#define WORKER_STRIDE 4096U

struct writer {
	int fd;
	unsigned index;
	int error;
};

#define CHECK(condition)                                                                           \
	do {                                                                                       \
		if (!(condition)) {                                                                \
			fprintf(stderr, "%s:%d: %s (errno=%d: %s)\n", __FILE__, __LINE__,          \
			    #condition, errno, strerror(errno));                                   \
			exit(EXIT_FAILURE);                                                        \
		}                                                                                  \
	} while (0)

static uint8_t
expected_byte(size_t position)
{
	if (position < WORKER_COUNT * WORKER_WRITES * WORKER_STRIDE) {
		return (uint8_t)(position / (WORKER_WRITES * WORKER_STRIDE) + 1);
	}
	return (uint8_t)(position * 17 + 3);
}

static void *
write_worker(void *context)
{
	struct writer *worker = context;
	uint8_t buffer[WORKER_STRIDE];
	unsigned iteration;
	off_t offset;

	memset(buffer, (int)worker->index + 1, sizeof(buffer));
	for (iteration = 0; iteration < WORKER_WRITES; iteration++) {
		offset = (off_t)((worker->index * WORKER_WRITES + iteration) * WORKER_STRIDE);
		if (pwrite(worker->fd, buffer, sizeof(buffer), offset) != sizeof(buffer)) {
			worker->error = errno != 0 ? errno : EIO;
			break;
		}
	}
	return NULL;
}

static void
check_persisted(int directory)
{
	uint8_t *data = malloc(DATA_SIZE);
	char value[32];
	char link[32];
	struct stat status;
	struct stat alias;
	size_t index;
	int fd;

	CHECK(data != NULL);
	fd = openat(directory, "payload", O_RDONLY | O_NOFOLLOW);
	CHECK(fd >= 0);
	CHECK(fstat(fd, &status) == 0 && status.st_size == DATA_SIZE && status.st_nlink == 2);
	CHECK((status.st_mode & 0777) == 0640);
	CHECK(pread(fd, data, DATA_SIZE, 0) == DATA_SIZE);
	for (index = 0; index < DATA_SIZE; index++) {
		CHECK(data[index] == expected_byte(index));
	}
	CHECK(fgetxattr(fd, "org.machlin.acceptance", value, sizeof(value), 0, 0) == 9);
	CHECK(memcmp(value, "persisted", 9) == 0);
	CHECK(fstatat(directory, "hardlink", &alias, 0) == 0 && alias.st_ino == status.st_ino);
	CHECK(readlinkat(directory, "symlink", link, sizeof(link)) == 7);
	CHECK(memcmp(link, "payload", 7) == 0);
	CHECK(close(fd) == 0);
	free(data);
}

static void
check_write(int directory)
{
	uint8_t *data = malloc(DATA_SIZE);
	uint8_t zeros[4096];
	uint8_t *mapping;
	struct stat status;
	struct writer workers[WORKER_COUNT];
	pthread_t threads[WORKER_COUNT];
	size_t index;
	int fd;
	int removed;

	CHECK(data != NULL);
	for (index = 0; index < DATA_SIZE; index++) {
		data[index] = (uint8_t)(index * 17 + 3);
	}
	fd = openat(directory, "temporary", O_CREAT | O_EXCL | O_RDWR, 0600);
	CHECK(fd >= 0);
	CHECK(write(fd, data, DATA_SIZE) == DATA_SIZE);
	CHECK(fsync(fd) == 0);
	CHECK(ftruncate(fd, 1025) == 0);
	CHECK(ftruncate(fd, DATA_SIZE) == 0);
	memset(zeros, 0xa5, sizeof(zeros));
	CHECK(pread(fd, zeros, sizeof(zeros), 1025) == sizeof(zeros));
	for (index = 0; index < sizeof(zeros); index++) {
		CHECK(zeros[index] == 0);
	}
	CHECK(pwrite(fd, data, DATA_SIZE, 0) == DATA_SIZE);
	for (index = 0; index < WORKER_COUNT; index++) {
		workers[index] = (struct writer){ .fd = fd, .index = (unsigned)index };
		CHECK(pthread_create(&threads[index], NULL, write_worker, &workers[index]) == 0);
	}
	for (index = 0; index < WORKER_COUNT; index++) {
		CHECK(pthread_join(threads[index], NULL) == 0 && workers[index].error == 0);
	}
	mapping = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	CHECK(mapping != MAP_FAILED);
	mapping[0] = 99;
	CHECK(msync(mapping, 4096, MS_SYNC) == 0);
	CHECK(pread(fd, zeros, 1, 0) == 1 && zeros[0] == 99);
	mapping[0] = 1;
	CHECK(msync(mapping, 4096, MS_SYNC) == 0);
	CHECK(munmap(mapping, 4096) == 0);
	CHECK(fchmod(fd, 0640) == 0);
	CHECK(fsetxattr(fd, "org.machlin.acceptance", "persisted", 9, 0, XATTR_CREATE) == 0);
	errno = 0;
	CHECK(fsetxattr(fd, "org.machlin.acceptance", "again", 5, 0, XATTR_CREATE) == -1 &&
	    errno == EEXIST);
	CHECK(fsetxattr(fd, "org.machlin.transient", "x", 1, 0, 0) == 0);
	CHECK(fremovexattr(fd, "org.machlin.transient", 0) == 0);
	CHECK(fsync(fd) == 0);
	CHECK(close(fd) == 0);
	CHECK(renameat(directory, "temporary", directory, "payload") == 0);
	CHECK(linkat(directory, "payload", directory, "hardlink", 0) == 0);
	CHECK(symlinkat("payload", directory, "symlink") == 0);
	removed = openat(directory, "removed", O_CREAT | O_EXCL | O_RDWR, 0600);
	CHECK(removed >= 0);
	CHECK(write(removed, "old", 3) == 3);
	CHECK(unlinkat(directory, "removed", 0) == 0);
	CHECK(fstat(removed, &status) == 0 && status.st_nlink == 0);
	CHECK(pwrite(removed, "new bytes", 9, 0) == 9);
	CHECK(pread(removed, zeros, 9, 0) == 9 && memcmp(zeros, "new bytes", 9) == 0);
	CHECK(fsync(removed) == 0);
	CHECK(close(removed) == 0);
	CHECK(mkdirat(directory, "empty", 0700) == 0);
	CHECK(unlinkat(directory, "empty", AT_REMOVEDIR) == 0);
	CHECK(fsync(directory) == 0);
	free(data);
}

int
main(int argc, char **argv)
{
	struct statfs filesystem;
	int root;
	int directory;
	int verify;

	CHECK(argc == 3 && (strcmp(argv[2], "write") == 0 || strcmp(argv[2], "verify") == 0));
	verify = strcmp(argv[2], "verify") == 0;
	root = open(argv[1], O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
	CHECK(root >= 0);
	CHECK(fstatfs(root, &filesystem) == 0);
	CHECK(strcmp(filesystem.f_fstypename, "machlinext4") == 0);
	if (!verify) {
		CHECK((filesystem.f_flags & MNT_RDONLY) == 0);
		CHECK(mkdirat(root, "acceptance-write", 0700) == 0);
	}
	directory = openat(root, "acceptance-write", O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
	CHECK(directory >= 0);
	if (!verify) {
		check_write(directory);
	}
	check_persisted(directory);
	CHECK(close(directory) == 0 && close(root) == 0);
	puts(verify ? "PASS native write persistence after remount"
		    : "PASS native writes, growth, shrink, mmap, concurrent I/O, metadata, xattrs, "
		      "namespace and open-unlinked lifetime");
	return EXIT_SUCCESS;
}

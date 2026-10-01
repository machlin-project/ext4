/* SPDX-License-Identifier: BSD-3-Clause */
#define _DARWIN_C_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#define PAYLOAD_BYTES 200000U
#define WRITABLE_BYTE 0x35U
#define UNAVAILABLE_BYTE 0xa7U
#define CHECK(condition)                                                                           \
	do {                                                                                       \
		if (!(condition)) {                                                                \
			fprintf(stderr, "%s:%d: %s (errno=%d: %s)\n", __FILE__, __LINE__,          \
			    #condition, errno, strerror(errno));                                   \
			exit(EXIT_FAILURE);                                                        \
		}                                                                                  \
	} while (0)

static int
removed_error(ssize_t result, int error)
{
	return result == -1 &&
	    (error == EIO || error == ENXIO || error == ENODEV || error == EBADF ||
		error == ESTALE);
}

static uint8_t
expected_byte(size_t offset, int writable)
{
	return writable ? WRITABLE_BYTE : (uint8_t)(offset * 17U + 23U);
}

static int
open_file(int root, int writable)
{
	return openat(root, writable ? "removal-durable" : "payload.bin", O_RDONLY | O_NOFOLLOW);
}

static void
verify(int root, int writable)
{
	struct stat status;
	uint8_t data[4096];
	size_t offset;
	size_t length;
	size_t index;
	int fd = open_file(root, writable);

	CHECK(fd >= 0 && fstat(fd, &status) == 0 && status.st_size == PAYLOAD_BYTES);
	for (offset = 0; offset < PAYLOAD_BYTES; offset += length) {
		length = PAYLOAD_BYTES - offset;
		if (length > sizeof(data)) {
			length = sizeof(data);
		}
		CHECK(pread(fd, data, length, (off_t)offset) == (ssize_t)length);
		for (index = 0; index < length; index++) {
			CHECK(data[index] == expected_byte(offset + index, writable));
		}
	}
	CHECK(close(fd) == 0);
	puts("PASS: all acknowledged bytes survive forced image removal and remount");
}

static int
exercise_removal(int root, int writable)
{
	struct stat status;
	uint8_t *baseline = NULL;
	volatile uint8_t *mapping;
	uint8_t byte;
	ssize_t read_result;
	ssize_t write_result = -1;
	int read_error;
	int write_error = 0;
	int sync_result = 0;
	int sync_error = 0;
	int file_close;
	int file_close_error;
	int directory_close;
	int directory_close_error;
	int child_status;
	int passed;
	int fd;
	pid_t child;
	char command[32];

	if (writable) {
		fd = openat(root, "removal-durable", O_CREAT | O_EXCL | O_RDWR | O_NOFOLLOW, 0600);
		CHECK(fd >= 0);
		baseline = malloc(PAYLOAD_BYTES);
		CHECK(baseline != NULL);
		memset(baseline, WRITABLE_BYTE, PAYLOAD_BYTES);
		CHECK(pwrite(fd, baseline, PAYLOAD_BYTES, 0) == PAYLOAD_BYTES);
		CHECK(fsync(fd) == 0 && fsync(root) == 0);
		free(baseline);
	} else {
		fd = open_file(root, 0);
	}
	CHECK(fd >= 0 && fstat(fd, &status) == 0 && status.st_size == PAYLOAD_BYTES);
	CHECK(fcntl(fd, F_RDAHEAD, 0) == 0 && fcntl(fd, F_NOCACHE, 1) == 0);
	mapping = mmap(NULL, PAYLOAD_BYTES, PROT_READ, MAP_SHARED, fd, 0);
	CHECK(mapping != MAP_FAILED);
	CHECK(madvise((void *)mapping, PAYLOAD_BYTES, MADV_RANDOM) == 0);
	CHECK(mapping[0] == expected_byte(0, writable));
	printf("{\"phase\":\"ready\",\"pid\":%d,\"writable\":%s}\n", getpid(),
	    writable ? "true" : "false");
	CHECK(fflush(stdout) == 0);
	CHECK(fgets(command, sizeof(command), stdin) != NULL && strcmp(command, "removed\n") == 0);
	/* The controller force-detaches only this disposable image. This tests the
	 * forced-unmount lifetime, not sudden physical power loss or volatile media. */
	errno = 0;
	read_result = pread(fd, &byte, 1, PAYLOAD_BYTES - 1);
	read_error = errno;
	if (writable) {
		byte = UNAVAILABLE_BYTE;
		errno = 0;
		write_result = pwrite(fd, &byte, 1, PAYLOAD_BYTES - 1);
		write_error = errno;
		errno = 0;
		sync_result = fsync(fd);
		sync_error = errno;
	}
	child = fork();
	CHECK(child >= 0);
	if (child == 0) {
		uint8_t value;

		alarm(10);
		value = mapping[PAYLOAD_BYTES - 1];
		/* Correct resident data may remain accessible. An inaccessible page
		 * must signal SIGBUS; neither garbage nor SIGSEGV is acceptable. */
		_exit(value == expected_byte(PAYLOAD_BYTES - 1, writable) ? 0 : 1);
	}
	CHECK(waitpid(child, &child_status, 0) == child);
	CHECK(munmap((void *)mapping, PAYLOAD_BYTES) == 0);
	errno = 0;
	file_close = close(fd);
	file_close_error = errno;
	errno = 0;
	directory_close = close(root);
	directory_close_error = errno;
	passed = removed_error(read_result, read_error) &&
	    (!writable ||
		(removed_error(write_result, write_error) &&
		    (sync_result == 0 || removed_error(sync_result, sync_error)))) &&
	    ((WIFEXITED(child_status) && WEXITSTATUS(child_status) == 0) ||
		(WIFSIGNALED(child_status) && WTERMSIG(child_status) == SIGBUS)) &&
	    (file_close == 0 || removed_error(file_close, file_close_error)) &&
	    (directory_close == 0 || removed_error(directory_close, directory_close_error));
	printf("{\"phase\":\"finished\",\"read\":%lld,\"read_errno\":%d,"
	       "\"write\":%lld,\"write_errno\":%d,\"fsync\":%d,\"fsync_errno\":%d,"
	       "\"mmap_exit\":%d,\"mmap_signal\":%d,\"close\":%d,\"close_errno\":%d,"
	       "\"root_close\":%d,\"root_close_errno\":%d,\"passed\":%s}\n",
	    (long long)read_result, read_error, (long long)write_result, write_error, sync_result,
	    sync_error, WIFEXITED(child_status) ? WEXITSTATUS(child_status) : -1,
	    WIFSIGNALED(child_status) ? WTERMSIG(child_status) : 0, file_close, file_close_error,
	    directory_close, directory_close_error, passed ? "true" : "false");
	CHECK(fflush(stdout) == 0);
	return passed;
}

int
main(int argc, char **argv)
{
	struct statfs filesystem;
	int root;
	int writable;
	int checking;
	int passed;

	CHECK(argc == 3 && geteuid() != 0);
	writable =
	    strcmp(argv[2], "remove-readwrite") == 0 || strcmp(argv[2], "verify-written") == 0;
	checking =
	    strcmp(argv[2], "verify-readonly") == 0 || strcmp(argv[2], "verify-written") == 0;
	CHECK(writable || checking || strcmp(argv[2], "remove-readonly") == 0);
	CHECK(statfs(argv[1], &filesystem) == 0 && strcmp(filesystem.f_mntonname, argv[1]) == 0 &&
	    strcmp(filesystem.f_fstypename, "machlinext4") == 0);
	CHECK((filesystem.f_flags & MNT_IGNORE_OWNERSHIP) == 0);
	CHECK(((filesystem.f_flags & MNT_RDONLY) != 0) == (checking || !writable));
	root = open(argv[1], O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
	CHECK(root >= 0);
	if (checking) {
		verify(root, writable);
		CHECK(close(root) == 0);
		return 0;
	}
	passed = exercise_removal(root, writable);
	return passed ? 0 : 1;
}

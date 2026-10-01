/* SPDX-License-Identifier: BSD-3-Clause */
/* Reuse a full disposable pressure image to check native ENOSPC without
 * filling the entire device again. Only the final test-file range is changed. */
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <unistd.h>

enum {
	CapacityWriteBytes = 256 * 1024,
	CapacityReleaseBytes = 128 * 1024,
	CapacityMaxVolumeBytes = 256 * 1024 * 1024,
	CapacityPattern = 0x6d,
	CapacityMaxAttempts = 4
};

#define CHECK(condition)                                                                           \
	do {                                                                                       \
		if (!(condition)) {                                                                \
			fprintf(stderr, "%s:%d: %s (errno=%d)\n", __FILE__, __LINE__, #condition,  \
			    errno);                                                                \
			exit(EXIT_FAILURE);                                                        \
		}                                                                                  \
	} while (0)

/* The APFS control is restricted to a small, newly created disposable volume.
 * Allocation reserves can leave f_bavail nonzero even after a write hits ENOSPC. */
static int
prepare_control(int root, struct statfs *filesystem, uint8_t *buffer)
{
	off_t reported = 0;
	ssize_t amount;
	int file;

	CHECK(filesystem->f_blocks != 0 && filesystem->f_bsize != 0);
	CHECK(filesystem->f_blocks <= CapacityMaxVolumeBytes / filesystem->f_bsize);
	CHECK(mkdirat(root, "acceptance-write", 0700) == 0);
	file = openat(root, "acceptance-write/space-pressure", O_CREAT | O_EXCL | O_RDWR, 0600);
	CHECK(file >= 0 && fcntl(file, F_NOCACHE, 1) == 0);
	while (reported <= CapacityMaxVolumeBytes) {
		amount = pwrite(file, buffer, CapacityWriteBytes, reported);
		if (amount < 0) {
			CHECK(errno == ENOSPC);
			break;
		}
		CHECK(amount > 0 && amount <= CapacityWriteBytes);
		reported += amount;
	}
	CHECK(reported <= CapacityMaxVolumeBytes && fsync(file) == 0);
	return file;
}

static int
read_tail(int file, off_t start, off_t end, uint8_t *buffer, ssize_t *amount, int *error)
{
	off_t cursor;
	size_t count;
	size_t index;

	*amount = 0;
	*error = 0;
	if (end < start || end - start > CapacityMaxAttempts * CapacityWriteBytes) {
		return 0;
	}
	for (cursor = start; cursor < end; cursor += count) {
		count = (size_t)(end - cursor);
		if (count > CapacityWriteBytes) {
			count = CapacityWriteBytes;
		}
		*amount = pread(file, buffer, count, cursor);
		*error = *amount < 0 ? errno : 0;
		if (*amount != (ssize_t)count) {
			return 0;
		}
		for (index = 0; index < count; index++) {
			if (buffer[index] != CapacityPattern) {
				return 0;
			}
		}
	}
	return 1;
}

static off_t
parse_offset(const char *text)
{
	char *end;
	unsigned long long value;

	CHECK(text[0] >= '0' && text[0] <= '9');
	errno = 0;
	value = strtoull(text, &end, 10);
	CHECK(errno == 0 && *end == '\0' && value <= CapacityMaxVolumeBytes);
	return (off_t)value;
}

/* A fresh read-only mount must agree with the size observed by the writer.
 * Checking only the live size can silently skip a committed but hidden tail. */
static int
verify_remount(const char *mount, off_t start, off_t expected)
{
	struct statfs filesystem;
	struct stat status;
	uint8_t *buffer;
	ssize_t amount;
	int error;
	int root;
	int file;
	int readback;
	int eof;
	int passed;

	CHECK(geteuid() != 0 && expected >= start);
	root = open(mount, O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
	CHECK(root >= 0 && fstatfs(root, &filesystem) == 0);
	CHECK(strcmp(filesystem.f_fstypename, "machlinext4") == 0);
	CHECK((filesystem.f_flags & MNT_RDONLY) != 0);
	file = openat(root, "acceptance-write/space-pressure", O_RDONLY | O_NOFOLLOW);
	CHECK(file >= 0 && fstat(file, &status) == 0 && S_ISREG(status.st_mode));
	CHECK(status.st_uid == geteuid());
	buffer = malloc(CapacityWriteBytes);
	CHECK(buffer != NULL);
	readback = read_tail(file, start, status.st_size, buffer, &amount, &error);
	eof = pread(file, buffer, 1, status.st_size) == 0;
	passed = status.st_size == expected && readback && eof;
	printf("{\"expected_size\":%lld,\"size\":%lld,\"read_amount\":%lld,\"read_error\":%d,"
	       "\"readback\":%s,\"eof\":%s,\"passed\":%s}\n",
	    (long long)expected, (long long)status.st_size, (long long)amount, error,
	    readback ? "true" : "false", eof ? "true" : "false", passed ? "true" : "false");
	free(buffer);
	CHECK(close(file) == 0 && close(root) == 0);
	return passed ? EXIT_SUCCESS : EXIT_FAILURE;
}

int
main(int argc, char **argv)
{
	struct statfs filesystem;
	struct stat status;
	uint8_t *buffer;
	off_t start;
	off_t reported = 0;
	ssize_t amount = 0;
	long page_size;
	int root;
	int file;
	int uncached;
	int attempt;
	int write_error = 0;
	int sync_error;
	int stat_error;
	int close_error;
	int passed;
	int aligned;
	int control;
	int readback = 1;
	int read_error = 0;
	ssize_t read_amount = 0;

	CHECK(argc >= 3 && argc <= 5);
	if (strcmp(argv[2], "verify") == 0) {
		CHECK(argc == 5);
		return verify_remount(argv[1], parse_offset(argv[3]), parse_offset(argv[4]));
	}
	CHECK(strcmp(argv[2], "cached") == 0 || strcmp(argv[2], "uncached") == 0);
	CHECK(argc < 4 || strcmp(argv[3], "tail") == 0 || strcmp(argv[3], "aligned") == 0);
	CHECK(argc < 5 || strcmp(argv[4], "apfs") == 0);
	CHECK(geteuid() != 0);
	uncached = strcmp(argv[2], "uncached") == 0;
	aligned = argc >= 4 && strcmp(argv[3], "aligned") == 0;
	control = argc == 5;
	root = open(argv[1], O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
	CHECK(root >= 0 && fstatfs(root, &filesystem) == 0);
	CHECK(strcmp(filesystem.f_fstypename, control ? "apfs" : "machlinext4") == 0);
	CHECK((filesystem.f_flags & MNT_RDONLY) == 0);
	/* Spotlight and other native users can release a few metadata blocks while
	 * mounting. Bound the remaining headroom instead of assuming exact zero. */
	CHECK(filesystem.f_bsize != 0);
	CHECK(control || filesystem.f_bavail <= CapacityReleaseBytes / filesystem.f_bsize);
	buffer = malloc(CapacityWriteBytes);
	CHECK(buffer != NULL);
	memset(buffer, CapacityPattern, CapacityWriteBytes);
	file = control ? prepare_control(root, &filesystem, buffer)
		       : openat(root, "acceptance-write/space-pressure", O_RDWR | O_NOFOLLOW);
	CHECK(file >= 0 && fstat(file, &status) == 0 && S_ISREG(status.st_mode));
	CHECK(status.st_uid == geteuid() && status.st_size >= CapacityWriteBytes);
	start = status.st_size - CapacityReleaseBytes;
	page_size = sysconf(_SC_PAGESIZE);
	CHECK(page_size > 0);
	if (aligned) {
		start -= start % page_size;
	}
	CHECK(ftruncate(file, start) == 0 && fsync(file) == 0);
	CHECK(fcntl(file, F_NOCACHE, uncached) == 0);
	for (attempt = 0; attempt < CapacityMaxAttempts; attempt++) {
		amount = pwrite(file, buffer, CapacityWriteBytes, start + reported);
		write_error = amount < 0 ? errno : 0;
		if (amount <= 0) {
			break;
		}
		CHECK(amount <= CapacityWriteBytes);
		reported += amount;
	}
	sync_error = fsync(file) == 0 ? 0 : errno;
	stat_error = fstat(file, &status) == 0 ? 0 : errno;
	passed = amount < 0 && write_error == ENOSPC && sync_error == 0 && stat_error == 0 &&
	    status.st_size == start + reported;
	/* Inspect even an unreported prefix; a failed syscall need not undo earlier
	 * kernel subrequests. Keep the strict accounting verdict separate. */
	readback = stat_error == 0 &&
	    read_tail(file, start, status.st_size, buffer, &read_amount, &read_error);
	close_error = close(file) == 0 ? 0 : errno;
	passed = passed && close_error == 0;
	/* Cached writes may report success before allocation. ENOSPC at fsync is
	 * then an ordinary deferred error, not evidence of a broken write callback. */
	if (!uncached && sync_error == ENOSPC && stat_error == 0 &&
	    (write_error == 0 || write_error == ENOSPC) &&
	    (close_error == 0 || close_error == ENOSPC)) {
		passed = 1;
	}
	passed = passed && readback;
	printf("{\"filesystem\":\"%s\",\"aligned\":%s,\"uncached\":%s,\"start\":%lld,\"reported\":%"
	       "lld,\"write_error\":%d,"
	       "\"sync_error\":%d,\"stat_error\":%d,\"size\":%lld,\"close_error\":%d,"
	       "\"read_error\":%d,\"read_amount\":%lld,\"readback\":%s,\"passed\":%s}\n",
	    filesystem.f_fstypename, aligned ? "true" : "false", uncached ? "true" : "false",
	    (long long)start, (long long)reported, write_error, sync_error, stat_error,
	    stat_error == 0 ? (long long)status.st_size : -1LL, close_error, read_error,
	    (long long)read_amount, readback ? "true" : "false", passed ? "true" : "false");
	free(buffer);
	CHECK(close(root) == 0);
	return passed ? EXIT_SUCCESS : EXIT_FAILURE;
}

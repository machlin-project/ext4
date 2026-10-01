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

int
main(int argc, char **argv)
{
	struct statfs filesystem;
	struct stat status;
	uint8_t *buffer;
	off_t start;
	off_t reported = 0;
	off_t cursor;
	ssize_t amount = 0;
	size_t index;
	size_t count;
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
	if (stat_error != 0 || status.st_size < start ||
	    status.st_size - start > CapacityMaxAttempts * CapacityWriteBytes) {
		readback = 0;
	}
	for (cursor = start; readback && cursor < status.st_size; cursor += count) {
		count = (size_t)(status.st_size - cursor);
		if (count > CapacityWriteBytes) {
			count = CapacityWriteBytes;
		}
		read_amount = pread(file, buffer, count, cursor);
		read_error = read_amount < 0 ? errno : 0;
		readback = read_amount == (ssize_t)count;
		for (index = 0; readback && index < count; index++) {
			readback = buffer[index] == CapacityPattern;
		}
	}
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

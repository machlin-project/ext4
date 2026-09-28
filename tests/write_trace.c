/* SPDX-License-Identifier: BSD-3-Clause */
/* Test-only preload library: records every pwrite, write and fsync that another
 * program issues to one image, in order, so interrupted foreign recovery states can be
 * rebuilt from its exact write sequence. EXT4_WRITE_TRACE names the trace file and
 * EXT4_WRITE_TRACE_TARGET the traced image. Records are 'W', a little-endian
 * 64-bit offset and length and the data, or 'F' for a flush. */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#define TRACE_WRITE 'W'
#define TRACE_FLUSH 'F'

static int trace = -1;
static dev_t target_device;
static ino_t target_inode;
static int configured;

static void
configure(void)
{
	struct stat target;
	const char *path;
	const char *target_path;

	if (configured) {
		return;
	}
	configured = 1;
	path = getenv("EXT4_WRITE_TRACE");
	target_path = getenv("EXT4_WRITE_TRACE_TARGET");
	if (path == NULL || target_path == NULL || stat(target_path, &target) != 0) {
		return;
	}
	target_device = target.st_dev;
	target_inode = target.st_ino;
	trace = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_APPEND | O_CLOEXEC, 0600);
}

static int
traced(int fd)
{
	struct stat metadata;

	configure();
	return trace >= 0 && fd != trace && fstat(fd, &metadata) == 0 &&
	    metadata.st_dev == target_device && metadata.st_ino == target_inode;
}

static void
record(char kind, uint64_t offset, const void *buffer, uint64_t length)
{
	unsigned char header[1 + 2 * sizeof(uint64_t)];
	unsigned int index;

	header[0] = (unsigned char)kind;
	for (index = 0; index < sizeof(uint64_t); index++) {
		header[1 + index] = (unsigned char)(offset >> (8 * index));
		header[1 + sizeof(uint64_t) + index] = (unsigned char)(length >> (8 * index));
	}
	if (write(trace, header, sizeof(header)) != (ssize_t)sizeof(header) ||
	    (length != 0 && write(trace, buffer, (size_t)length) != (ssize_t)length)) {
		abort();
	}
}

#ifdef __APPLE__
static ssize_t
traced_pwrite(int fd, const void *buffer, size_t length, off_t offset)
{
	ssize_t written = pwrite(fd, buffer, length, offset);

	if (written > 0 && traced(fd)) {
		record(TRACE_WRITE, (uint64_t)offset, buffer, (uint64_t)written);
	}
	return written;
}

/* unix_io writes the superblock with lseek and write. */
static ssize_t
traced_write(int fd, const void *buffer, size_t length)
{
	off_t offset = traced(fd) ? lseek(fd, 0, SEEK_CUR) : -1;
	ssize_t written = write(fd, buffer, length);

	if (written > 0 && offset >= 0) {
		record(TRACE_WRITE, (uint64_t)offset, buffer, (uint64_t)written);
	}
	return written;
}

static int
traced_fsync(int fd)
{
	int result = fsync(fd);

	if (result == 0 && traced(fd)) {
		record(TRACE_FLUSH, 0, NULL, 0);
	}
	return result;
}

/* dyld replaces calls to the second pointer with the first in other images. */
__attribute__((used, section("__DATA,__interpose"))) static const struct {
	const void *replacement;
	const void *replaced;
} interposers[] = { { (const void *)traced_pwrite, (const void *)pwrite },
	{ (const void *)traced_write, (const void *)write },
	{ (const void *)traced_fsync, (const void *)fsync } };
#else
ssize_t
pwrite(int fd, const void *buffer, size_t length, off_t offset)
{
	ssize_t (*real)(int, const void *, size_t, off_t) =
	    (ssize_t (*)(int, const void *, size_t, off_t))dlsym(RTLD_NEXT, "pwrite");
	ssize_t written = real(fd, buffer, length, offset);

	if (written > 0 && traced(fd)) {
		record(TRACE_WRITE, (uint64_t)offset, buffer, (uint64_t)written);
	}
	return written;
}

ssize_t
pwrite64(int fd, const void *buffer, size_t length, off_t offset)
{
	return pwrite(fd, buffer, length, offset);
}

/* unix_io writes the superblock with lseek and write. */
ssize_t
write(int fd, const void *buffer, size_t length)
{
	ssize_t (*real)(int, const void *, size_t) =
	    (ssize_t (*)(int, const void *, size_t))dlsym(RTLD_NEXT, "write");
	off_t offset = fd != trace && traced(fd) ? lseek(fd, 0, SEEK_CUR) : -1;
	ssize_t written = real(fd, buffer, length);

	if (written > 0 && offset >= 0) {
		record(TRACE_WRITE, (uint64_t)offset, buffer, (uint64_t)written);
	}
	return written;
}

int
fsync(int fd)
{
	int (*real)(int) = (int (*)(int))dlsym(RTLD_NEXT, "fsync");
	int result = real(fd);

	if (result == 0 && traced(fd)) {
		record(TRACE_FLUSH, 0, NULL, 0);
	}
	return result;
}
#endif

/* SPDX-License-Identifier: BSD-3-Clause */
#include "image.h"

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/file.h>
#include <unistd.h>

static enum ext4_result
ext4_posix_read(void *context, uint64_t offset, void *buffer, size_t length)
{
	struct ext4_posix_image *image = context;
	uint8_t *bytes = buffer;
	size_t completed = 0;
	ssize_t count;

	image->read_calls++;
	if (image->fail_read_at != 0 && image->read_calls == image->fail_read_at) {
		return EXT4_IO;
	}
	if (offset > INT64_MAX || length > (uint64_t)INT64_MAX - offset) {
		return EXT4_IO;
	}
	while (completed < length) {
		count = pread(
		    image->fd, bytes + completed, length - completed, (off_t)(offset + completed));
		if (count < 0 && errno == EINTR) {
			continue;
		}
		if (count <= 0) {
			return EXT4_IO;
		}
		completed += (size_t)count;
	}
	return EXT4_OK;
}

static void *
ext4_posix_allocate(void *context, size_t size)
{
	struct ext4_posix_image *image = context;
	void *allocation;

	image->allocation_calls++;
	if (image->fail_allocation_at != 0 &&
	    image->allocation_calls == image->fail_allocation_at) {
		return NULL;
	}
	allocation = malloc(size);
	if (allocation != NULL) {
		image->live_allocations++;
	}
	return allocation;
}

static enum ext4_result
ext4_posix_write(void *context, uint64_t offset, const void *buffer, size_t length)
{
	struct ext4_posix_image *image = context;
	const uint8_t *bytes = buffer;
	size_t completed = 0;
	ssize_t count;

	image->write_calls++;
	if ((image->fail_write_at != 0 && image->write_calls == image->fail_write_at) ||
	    offset > image->environment.size_bytes ||
	    length > image->environment.size_bytes - offset || offset > INT64_MAX ||
	    length > (uint64_t)INT64_MAX - offset) {
		return EXT4_IO;
	}
	while (completed < length) {
		count = pwrite(
		    image->fd, bytes + completed, length - completed, (off_t)(offset + completed));
		if (count < 0 && errno == EINTR) {
			continue;
		}
		if (count <= 0) {
			return EXT4_IO;
		}
		completed += (size_t)count;
	}
	return EXT4_OK;
}

static enum ext4_result
ext4_posix_flush(void *context)
{
	struct ext4_posix_image *image = context;
	int result;

	image->flush_calls++;
	if (image->fail_flush_at != 0 && image->flush_calls == image->fail_flush_at) {
		return EXT4_IO;
	}
	do {
#if defined(__APPLE__)
		/* fsync alone does not request a device-cache barrier on macOS. */
		result = fcntl(image->fd, F_FULLFSYNC);
#else
		result = fsync(image->fd);
#endif
	} while (result != 0 && errno == EINTR);
	return result == 0 ? EXT4_OK : EXT4_IO;
}

static void
ext4_posix_release(void *context, void *allocation, size_t size)
{
	struct ext4_posix_image *image = context;

	(void)size;
	if (allocation != NULL) {
		image->live_allocations--;
	}
	free(allocation);
}

static enum ext4_result
ext4_posix_open_mode(struct ext4_posix_image *image, const char *path, bool writable)
{
	struct stat status;

	memset(image, 0, sizeof(*image));
	image->fd = open(path, (writable ? O_RDWR : O_RDONLY) | O_CLOEXEC);
	if (image->fd < 0) {
		return EXT4_IO;
	}
	if (fstat(image->fd, &status) != 0 || status.st_size < 0 || !S_ISREG(status.st_mode)) {
		ext4_posix_close(image);
		return EXT4_IO;
	}
	if (writable && flock(image->fd, LOCK_EX | LOCK_NB) != 0) {
		ext4_posix_close(image);
		return EXT4_IO;
	}
	image->environment.context = image;
	image->environment.size_bytes = (uint64_t)status.st_size;
	image->environment.read = ext4_posix_read;
	image->environment.allocate = ext4_posix_allocate;
	image->environment.release = ext4_posix_release;
	if (writable) {
		image->writer.context = image;
		image->writer.write = ext4_posix_write;
		image->writer.flush = ext4_posix_flush;
	}
	return EXT4_OK;
}

enum ext4_result
ext4_posix_open(struct ext4_posix_image *image, const char *path)
{
	return ext4_posix_open_mode(image, path, false);
}

enum ext4_result
ext4_posix_open_writable(struct ext4_posix_image *image, const char *path)
{
	return ext4_posix_open_mode(image, path, true);
}

void
ext4_posix_close(struct ext4_posix_image *image)
{
	if (image->fd >= 0) {
		close(image->fd);
		image->fd = -1;
	}
}

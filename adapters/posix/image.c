/* SPDX-License-Identifier: BSD-3-Clause */
#include "image.h"

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
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

enum ext4_result
ext4_posix_open(struct ext4_posix_image *image, const char *path)
{
	struct stat status;

	memset(image, 0, sizeof(*image));
	image->fd = open(path, O_RDONLY | O_CLOEXEC);
	if (image->fd < 0) {
		return EXT4_IO;
	}
	if (fstat(image->fd, &status) != 0 || status.st_size < 0 || !S_ISREG(status.st_mode)) {
		ext4_posix_close(image);
		return EXT4_IO;
	}
	image->environment.context = image;
	image->environment.size_bytes = (uint64_t)status.st_size;
	image->environment.read = ext4_posix_read;
	image->environment.allocate = ext4_posix_allocate;
	image->environment.release = ext4_posix_release;
	return EXT4_OK;
}

void
ext4_posix_close(struct ext4_posix_image *image)
{
	if (image->fd >= 0) {
		close(image->fd);
		image->fd = -1;
	}
}

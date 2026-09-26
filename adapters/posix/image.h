/* SPDX-License-Identifier: BSD-3-Clause */
#ifndef MACHLIN_EXT4_POSIX_IMAGE_H
#define MACHLIN_EXT4_POSIX_IMAGE_H

#include <ext4/ext4.h>

struct ext4_posix_image {
	int fd;
	struct ext4_environment environment;
	uint64_t read_calls;
	uint64_t allocation_calls;
	uint64_t fail_read_at;
	uint64_t fail_allocation_at;
	uint64_t live_allocations;
};

enum ext4_result ext4_posix_open(struct ext4_posix_image *image, const char *path);
void ext4_posix_close(struct ext4_posix_image *image);

#endif

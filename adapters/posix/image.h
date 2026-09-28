/* SPDX-License-Identifier: BSD-3-Clause */
#ifndef MACHLIN_EXT4_POSIX_IMAGE_H
#define MACHLIN_EXT4_POSIX_IMAGE_H

#include <ext4/ext4.h>

#define EXT4_POSIX_NODE_NAME_SIZE 65U

struct ext4_posix_image {
	int fd;
	struct ext4_environment environment;
	struct ext4_write_environment writer;
	/* Wall-clock sleeps, system entropy and this host's name for MMP volumes. */
	struct ext4_mmp_environment mmp;
	char node_name[EXT4_POSIX_NODE_NAME_SIZE];
	char device_name[EXT4_POSIX_NODE_NAME_SIZE];
	uint64_t read_calls;
	uint64_t write_calls;
	uint64_t flush_calls;
	uint64_t allocation_calls;
	uint64_t fail_read_at;
	uint64_t fail_write_at;
	uint64_t fail_flush_at;
	uint64_t fail_allocation_at;
	uint64_t live_allocations;
};

enum ext4_result ext4_posix_open(struct ext4_posix_image *image, const char *path);
/* Offline regular images only; obtains an exclusive advisory lock. */
enum ext4_result ext4_posix_open_writable(struct ext4_posix_image *image, const char *path);
void ext4_posix_close(struct ext4_posix_image *image);

#endif

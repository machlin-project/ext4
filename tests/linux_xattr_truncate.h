/* SPDX-License-Identifier: BSD-3-Clause */
#ifndef MACHLIN_EXT4_TEST_LINUX_XATTR_TRUNCATE_H
#define MACHLIN_EXT4_TEST_LINUX_XATTR_TRUNCATE_H

#include <sys/xattr.h>

#define TEST_XATTR_TRUNCATED_SIZE 17U
#define TEST_XATTR_CHANGED_SIZE 700U
#define TEST_XATTR_SHARED_SIZE 600U
#define TEST_XATTR_SECTOR_SIZE 512U

static void
check_xattr_truncate(uint32_t block_size)
{
	struct stat metadata;
	uint8_t value[TEST_XATTR_CHANGED_SIZE];
	uint8_t bytes[TEST_XATTR_TRUNCATED_SIZE];
	size_t index;
	int fd;

	fd = open("/mnt/block", O_RDONLY | O_CLOEXEC);
	require(fd >= 0, "open recovered attributed inode");
	require(fstat(fd, &metadata) == 0, "stat recovered attributed inode");
	require(metadata.st_size == TEST_XATTR_TRUNCATED_SIZE && metadata.st_nlink == 1 &&
		metadata.st_blocks == 2 * block_size / TEST_XATTR_SECTOR_SIZE &&
		(metadata.st_mode & 07777) == 0761 && metadata.st_uid == 501 &&
		metadata.st_gid == 20,
	    "verify linked truncate identity, size, mode and data/attribute accounting");
	require(fgetxattr(fd, "user.binary", value, sizeof(value)) == TEST_XATTR_CHANGED_SIZE,
	    "linked truncate recovery must retain external attributes");
	for (index = 0; index < sizeof(value); index++) {
		require(value[index] == (uint8_t)(index * 17U + 0x51U),
		    "verify recovered attribute value");
	}
	require(pread(fd, bytes, sizeof(bytes), 0) == sizeof(bytes), "read recovered prefix");
	for (index = 0; index < sizeof(bytes); index++) {
		require(bytes[index] == 0x5a, "verify recovered prefix bytes");
	}
	require(pread(fd, bytes, 1, TEST_XATTR_TRUNCATED_SIZE) == 0, "verify recovered EOF");
	require(close(fd) == 0, "close recovered file");
	fd = open("/mnt/shared", O_RDONLY | O_CLOEXEC);
	require(fd >= 0, "open original shared-attribute owner");
	require(fgetxattr(fd, "user.binary", value, sizeof(value)) == TEST_XATTR_SHARED_SIZE,
	    "verify original shared owner retains its value");
	for (index = 0; index < TEST_XATTR_SHARED_SIZE; index++) {
		require(value[index] == (uint8_t)(index * 29U + 0x83U),
		    "compare unchanged shared owner value");
	}
	require(close(fd) == 0, "close shared owner");
	require(umount("/mnt") == 0, "cleanly unmount recovered attributed filesystem");
	puts("LINUX_EXT4_XATTR_TRUNCATE_PASS");
	power_off(1);
}

#endif

/* SPDX-License-Identifier: BSD-3-Clause */
#ifndef MACHLIN_EXT4_TEST_LINUX_EA_INODE_H
#define MACHLIN_EXT4_TEST_LINUX_EA_INODE_H

#define LINUX_EA_FILE "/mnt/linux-ea-file"
#define LINUX_EA_DIRECTORY "/mnt/linux-ea-directory"
#define LINUX_EA_ORPHAN "/mnt/linux-ea-orphan"
#define LINUX_EA_SMALL 13U
#define LINUX_EA_DATA 17U

static void
check_ea_inodes(void)
{
	FILE *configuration;
	uint8_t *value = malloc(LINUX_XATTR_VALUE_LIMIT);
	uint32_t block_size;
	size_t index;
	int fd;
	int orphan;

	require(value != NULL, "allocate Linux EA_INODE value");
	configuration = fopen("/block-size", "r");
	require(configuration != NULL && fscanf(configuration, "%u", &block_size) == 1 &&
		fclose(configuration) == 0,
	    "read Linux EA_INODE block size");
	for (index = 0; index < LINUX_XATTR_VALUE_LIMIT; index++) {
		value[index] = (uint8_t)(index * 23U + 0x67U);
	}
	require(setxattr("/mnt/body", "user.maximum", value, LINUX_XATTR_VALUE_LIMIT, 0) == 0,
	    "Linux replace core EA_INODE value");
	require(
	    removexattr("/mnt/many", "user.value7") == 0, "Linux release core shared value entry");
	require(setxattr(
		    "/mnt/many", "user.value0", value, LINUX_XATTR_VALUE_LIMIT, XATTR_REPLACE) == 0,
	    "Linux replace shared-block EA_INODE value");
	umask(0);
	fd = open(LINUX_EA_FILE, O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
	require(fd >= 0 && write(fd, value, LINUX_EA_DATA) == LINUX_EA_DATA,
	    "create Linux EA_INODE file");
	require(fsetxattr(fd, "user.maximum", value, LINUX_XATTR_VALUE_LIMIT, XATTR_CREATE) == 0 &&
		fsetxattr(fd, "user.duplicate", value, LINUX_XATTR_VALUE_LIMIT, XATTR_CREATE) == 0,
	    "Linux share values across keys and owners");
	require(fsetxattr(fd, "user.small", value, LINUX_EA_SMALL, XATTR_CREATE) == 0 &&
		fsetxattr(fd, "user.empty", "", 0, XATTR_CREATE) == 0,
	    "Linux mix empty, inline and EA_INODE values");
	errno = 0;
	require(fsetxattr(fd, "user.maximum", value, 1, XATTR_CREATE) == -1 && errno == EEXIST,
	    "Linux EA_INODE create policy");
	errno = 0;
	require(fsetxattr(fd, "user.absent", value, 1, XATTR_REPLACE) == -1 && errno == ENODATA,
	    "Linux EA_INODE replace policy");
	require(fsync(fd) == 0 && close(fd) == 0, "persist Linux EA_INODE file");
	require(mkdir(LINUX_EA_DIRECTORY, 0750) == 0 &&
		setxattr(LINUX_EA_DIRECTORY, "user.medium", value, 3U * block_size + 7U, 0) == 0,
	    "Linux create attributed directory");
	orphan = open(LINUX_EA_ORPHAN, O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
	require(orphan >= 0 &&
		fsetxattr(orphan, "user.maximum", value, LINUX_XATTR_VALUE_LIMIT, 0) == 0 &&
		fsync(orphan) == 0 && unlink(LINUX_EA_ORPHAN) == 0,
	    "leave a committed Linux open-unlinked owner of a shared value");
	xattr_sync_path("/mnt/body");
	xattr_sync_path("/mnt/many");
	xattr_sync_path(LINUX_EA_DIRECTORY);
	xattr_sync_path("/mnt");
	free(value);
	puts("LINUX_EXT4_EA_INODE_PASS");
	puts("LINUX_EXT4_XATTR_PASS");
	puts("LINUX_EXT4_COMMITTED_RECOVERY_PENDING");
	/* Keep orphan open through the simulated power loss. */
	power_off(1);
}

#endif

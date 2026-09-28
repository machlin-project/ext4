/* SPDX-License-Identifier: BSD-3-Clause */
#ifndef MACHLIN_EXT4_TEST_LINUX_INLINE_H
#define MACHLIN_EXT4_TEST_LINUX_INLINE_H

static void
check_inline_data(void)
{
	uint8_t bytes[120];
	char name[64];

	size_t index;
	int fd;
	int orphan;

	for (index = 0; index < sizeof(bytes); index++) {
		bytes[index] = (uint8_t)(index * 23U + 0x67U);
	}
	fd = open("/mnt/file120", O_RDWR | O_CLOEXEC);
	require(fd >= 0 && pwrite(fd, bytes, 11, 57) == 11 &&
		fsetxattr(fd, "user.linux", bytes, 13, XATTR_CREATE) == 0 && fsync(fd) == 0 &&
		close(fd) == 0,
	    "Linux mutate core inline data across inode regions");
	umask(0);
	fd = open("/mnt/linux-inline", O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
	require(fd >= 0 && write(fd, bytes, 61) == 61 &&
		fsetxattr(fd, "user.small", bytes, 13, XATTR_CREATE) == 0 && fsync(fd) == 0 &&
		close(fd) == 0,
	    "Linux create inline file and user attribute");
	require(mkdir("/mnt/linux-inline-dir", 0750) == 0, "Linux create inline directory");
	for (index = 0; index < 6; index++) {
		require(snprintf(name, sizeof(name), "/mnt/linux-inline-dir/n%zu", index) > 0,
		    "format Linux inline child name");
		fd = open(name, O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC, 0640);
		require(fd >= 0 && write(fd, bytes, 3) == 3 && fsync(fd) == 0 && close(fd) == 0,
		    "Linux create children across both inline directory regions");
	}
	require(rename("/mnt/linux-inline-dir", "/mnt/linux-renamed-dir") == 0,
	    "Linux rename inline directory");
	orphan = open("/mnt/linux-inline-orphan", O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
	require(orphan >= 0 && write(orphan, bytes, sizeof(bytes)) == sizeof(bytes) &&
		fsync(orphan) == 0 && unlink("/mnt/linux-inline-orphan") == 0,
	    "leave committed Linux open-unlinked inline inode");
	xattr_sync_path("/mnt/linux-renamed-dir");
	xattr_sync_path("/mnt");
	puts("LINUX_EXT4_INLINE_PASS");
	puts("LINUX_EXT4_XATTR_PASS");
	puts("LINUX_EXT4_COMMITTED_RECOVERY_PENDING");
	/* Keep the inline orphan open through the simulated power loss. */
	power_off(1);
}

#endif

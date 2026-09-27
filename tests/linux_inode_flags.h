/* SPDX-License-Identifier: BSD-3-Clause */
#include <sys/ioctl.h>

/* Linux UAPI encodes long in these commands while copying a 32-bit flag word. */
#define TEST_FS_GETFLAGS _IOR('f', 1, long)
#define TEST_FS_SETFLAGS _IOW('f', 2, long)

static bool
namespace_flag_checks(void)
{
	FILE *input = fopen("/namespace-flags", "r");
	char relative[512];
	char path[sizeof(relative) + sizeof("/mnt")];
	struct stat metadata;
	unsigned int expected;
	unsigned long flags = 0;
	unsigned long changed;
	int file;
	int directory;
	int root;
	int child;
	int scanned;

	if (input == NULL) {
		require(errno == ENOENT, "open optional inode flag expectations");
		return false;
	}
	while ((scanned = fscanf(input, "%511s %x", relative, &expected)) == 2) {
		snprintf(path, sizeof(path), "/mnt%s", relative);
		file = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
		flags = 0;
		require(file >= 0 && ioctl(file, TEST_FS_GETFLAGS, &flags) == 0 &&
			flags == expected && close(file) == 0,
		    "Linux reads exact core-created inode flags");
	}
	require(scanned == EOF && fclose(input) == 0, "finish inode flag expectations");
	file = open("/mnt/block", O_RDONLY | O_CLOEXEC);
	flags = 0;
	require(file >= 0 && ioctl(file, TEST_FS_GETFLAGS, &flags) == 0,
	    "Linux reads protected file flags");
	if (flags & EXT4_INODE_RESTRICTED_FLAGS) {
		errno = 0;
		require(open("/mnt/block", O_WRONLY | O_CLOEXEC) == -1 && errno == EPERM,
		    "Linux rejects nonappend open on protected inode");
		errno = 0;
		require(truncate("/mnt/block", 0) == -1 && errno == EPERM,
		    "Linux rejects protected truncate");
		errno = 0;
		require(link("/mnt/block", "/mnt/flag-illegal") == -1 && errno == EPERM,
		    "Linux rejects a protected hardlink");
		errno = 0;
		require(
		    unlink("/mnt/block") == -1 && errno == EPERM, "Linux rejects protected unlink");
	}
	if (flags & EXT4_INODE_IMMUTABLE) {
		changed = flags ^ EXT4_INODE_NODUMP;
		errno = 0;
		require(ioctl(file, TEST_FS_SETFLAGS, &changed) == -1 && errno == EPERM,
		    "Linux requires clearing immutable before changing other flags");
	}
	if (stat("/mnt/flag-dir", &metadata) == 0) {
		errno = 0;
		require(open("/mnt/flag-dir/denied", O_WRONLY | O_CREAT | O_EXCL, 0600) == -1 &&
			errno == EPERM,
		    "Linux rejects creation in immutable directory");
		errno = 0;
		require(unlink("/mnt/flag-dir/child") == -1 && errno == EPERM,
		    "Linux rejects removal from immutable directory");
	} else {
		require(errno == ENOENT, "locate optional protected directory");
	}
	changed = flags & ~(unsigned long)EXT4_INODE_RESTRICTED_FLAGS;
	require(ioctl(file, TEST_FS_SETFLAGS, &changed) == 0 && close(file) == 0,
	    "Linux clears admitted protection flags");
	file = open("/mnt/block", O_WRONLY | O_APPEND | O_CLOEXEC);
	require(file >= 0 && write(file, "Z", 1) == 1 && fsync(file) == 0,
	    "Linux writes a distinct appended byte after clearing protection");
	changed = (flags & ~(unsigned long)EXT4_INODE_MODIFIABLE_FLAGS) | EXT4_INODE_IMMUTABLE |
	    EXT4_INODE_NODUMP | EXT4_INODE_NOATIME;
	require(
	    ioctl(file, TEST_FS_SETFLAGS, &changed) == 0 && fsync(file) == 0 && close(file) == 0,
	    "Linux commits new immutable flags after data");
	require(mkdir("/mnt/linux-flag-dir", 0700) == 0, "Linux creates flag inheritance parent");
	directory = open("/mnt/linux-flag-dir", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	flags = 0;
	require(directory >= 0 && ioctl(directory, TEST_FS_GETFLAGS, &flags) == 0,
	    "Linux reads new directory mapping flags");
	changed =
	    flags | EXT4_INODE_APPEND | EXT4_INODE_NODUMP | EXT4_INODE_NOATIME | EXT4_INODE_DIRSYNC;
	require(ioctl(directory, TEST_FS_SETFLAGS, &changed) == 0,
	    "Linux sets append-only directory flags");
	child = open("/mnt/linux-flag-dir/child", O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
	require(child >= 0 && write(child, "L", 1) == 1 && fsync(child) == 0 && close(child) == 0,
	    "Linux creates and writes inside append-only directory");
	errno = 0;
	require(unlink("/mnt/linux-flag-dir/child") == -1 && errno == EPERM,
	    "Linux rejects deletion from append-only directory");
	root = open("/mnt", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	require(root >= 0 && fsync(directory) == 0 && fsync(root) == 0,
	    "Linux commits inherited flags and both parent directories");
	puts("LINUX_EXT4_INODE_FLAGS_PASS");
	return true;
}

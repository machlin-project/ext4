/* SPDX-License-Identifier: BSD-3-Clause */
#define _GNU_SOURCE
#include "disk.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/reboot.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/utsname.h>
#include <sys/xattr.h>
#include <unistd.h>

#define NATIVE_UID 70003U
#define NATIVE_GID 80004U
#define NATIVE_MODE 0642U

static void
power_off(int passed)
{
	printf("LINUX_EXTERNAL_JOURNAL_RESULT=%s\n", passed ? "PASS" : "FAIL");
	fflush(stdout);
	fflush(stderr);
	reboot(RB_POWER_OFF);
	for (;;) {
		pause();
	}
}

static void
require(int condition, const char *operation)
{
	if (!condition) {
		fprintf(stderr, "%s: %s\n", operation, strerror(errno));
		power_off(0);
	}
}

static void
check_file(const char *contents, unsigned int value_byte, unsigned int mode, bool native)
{
	struct stat metadata;
	uint8_t bytes[300];
	size_t index;
	int fd;

	fd = open("/mnt/hello.txt", O_RDONLY | O_CLOEXEC);
	require(fd >= 0, "open paired-device file");
	require(
	    read(fd, bytes, sizeof(bytes)) == (ssize_t)strlen(contents), "read exact file size");
	require(memcmp(bytes, contents, strlen(contents)) == 0, "verify file contents");
	require(fstat(fd, &metadata) == 0, "stat paired-device file");
	require((metadata.st_mode & 07777U) == mode, "verify file mode");
	if (native) {
		require(metadata.st_uid == NATIVE_UID && metadata.st_gid == NATIVE_GID,
		    "verify native ownership");
	}
	require(fgetxattr(fd, "user.transaction", bytes, sizeof(bytes)) == (ssize_t)sizeof(bytes),
	    "read transactional attribute");
	for (index = 0; index < sizeof(bytes); index++) {
		require(bytes[index] == value_byte, "verify transactional attribute");
	}
	require(close(fd) == 0, "close verified file");
}

static void
mutate(void)
{
	const char contents[] = "Native ext4!\n";
	uint8_t value[300];
	uint8_t data[4096];
	int fd;
	int orphan;
	int directory;

	fd = open("/mnt/hello.txt", O_RDWR | O_CLOEXEC);
	require(fd >= 0, "open file for native mutation");
	require(pwrite(fd, contents, sizeof(contents) - 1U, 0) == (ssize_t)sizeof(contents) - 1,
	    "write native bytes");
	require(fchown(fd, NATIVE_UID, NATIVE_GID) == 0, "change native owner");
	require(fchmod(fd, NATIVE_MODE) == 0, "change native mode");
	memset(value, 'Y', sizeof(value));
	require(fsetxattr(fd, "user.transaction", value, sizeof(value), XATTR_REPLACE) == 0,
	    "replace native attribute");
	require(fsync(fd) == 0, "commit native data and metadata to external journal");
	require(close(fd) == 0, "close native file");
	orphan = open("/mnt/orphan", O_CREAT | O_EXCL | O_RDWR | O_CLOEXEC, 0600);
	require(orphan >= 0, "create external-journal orphan");
	memset(data, 'O', sizeof(data));
	require(write(orphan, data, sizeof(data)) == (ssize_t)sizeof(data), "allocate orphan data");
	require(fsetxattr(orphan, "user.orphan", value, sizeof(value), XATTR_CREATE) == 0,
	    "allocate orphan attribute");
	require(fsync(orphan) == 0, "commit orphan allocation");
	require(unlink("/mnt/orphan") == 0, "unlink open orphan");
	directory = open("/mnt", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	require(directory >= 0 && fsync(directory) == 0, "commit orphan removal");
	require(close(directory) == 0, "close parent directory");
	check_file(contents, 'Y', NATIVE_MODE, true);
	puts("LINUX_EXTERNAL_JOURNAL_MUTATION_PASS");
	/* Leave the descriptor open and both devices mounted to require recovery. */
	power_off(1);
}

int
main(void)
{
	const char *modules[] = { "virtio_blk", "crc32c_generic", "crc16", "mbcache", "jbd2",
		"ext4" };
	const char *mount_options = "data=ordered,journal_path=/dev/vdb";
	struct utsname identity;
	struct stat home;
	struct stat journal;
	FILE *config;
	char path[128];
	unsigned int phase = 0;
	unsigned int index;
	int fd;
	int result;

	setvbuf(stdout, NULL, _IOLBF, 0);
	require(uname(&identity) == 0, "uname");
	printf("LINUX_EXTERNAL_JOURNAL_KERNEL=%s %s %s\n", identity.sysname, identity.release,
	    identity.machine);
	require(mount("devtmpfs", "/dev", "devtmpfs", 0, NULL) == 0, "mount devtmpfs");
	for (index = 0; index < sizeof(modules) / sizeof(modules[0]); index++) {
		snprintf(path, sizeof(path), "/modules/%s.ko", modules[index]);
		fd = open(path, O_RDONLY | O_CLOEXEC);
		require(fd >= 0, "open matching module");
		result = (int)syscall(SYS_finit_module, fd, "", 0);
		require(result == 0 || errno == EEXIST, path);
		require(close(fd) == 0, "close module");
	}
	config = fopen("/phase", "r");
	require(config != NULL && fscanf(config, "%u", &phase) == 1, "read phase");
	require(fclose(config) == 0 && phase <= 3, "validate phase");
	require(stat("/dev/vda", &home) == 0 && stat("/dev/vdb", &journal) == 0,
	    "find both disposable block devices");
	require(
	    S_ISBLK(home.st_mode) && S_ISBLK(journal.st_mode) && home.st_rdev != journal.st_rdev,
	    "require distinct filesystem and journal devices");
	printf("LINUX_EXTERNAL_JOURNAL_DEVICES=%llu,%llu\n", (unsigned long long)home.st_rdev,
	    (unsigned long long)journal.st_rdev);
	printf("LINUX_EXTERNAL_JOURNAL_MOUNT_OPTIONS=%s\n", mount_options);
	require(mount("/dev/vda", "/mnt", "ext4", MS_NOATIME | MS_NOSUID | MS_NODEV,
		    mount_options) == 0,
	    "mount filesystem with external journal");
	if (phase == 1) {
		check_file("Native ext4!\n", 'Y', NATIVE_MODE, true);
		require(access("/mnt/orphan", F_OK) < 0 && errno == ENOENT,
		    "verify orphan namespace cleanup");
	} else {
		check_file("Journal ext4\n", 'X', 0604, phase == 3);
	}
	if (phase == 0) {
		mutate();
	}
	require(umount("/mnt") == 0, "cleanly unmount both devices");
	printf("LINUX_EXTERNAL_JOURNAL_PHASE=%u\n", phase);
	power_off(1);
	return 0;
}

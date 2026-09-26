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
#include <unistd.h>

#define TEST_UID 12345U
#define TEST_GID 23456U
#define TEST_MODE 0600U
#define TEST_LINUX_BYTE 0x6cU
#define TEST_JOURNAL_MAGIC 0xc03b3998U

static void
power_off(int passed)
{
	printf("LINUX_EXT4_PROBE_RESULT=%s\n", passed ? "PASS" : "FAIL");
	fflush(stdout);
	fflush(stderr);
	/* Deliberately omit sync and unmount: fsync below has committed the file
	 * transaction, and the next owner must recover this disposable image. */
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

static uint32_t
decode_le32(const struct ext4_le32 *field)
{
	return (uint32_t)field->bytes[0] | ((uint32_t)field->bytes[1] << 8) |
	    ((uint32_t)field->bytes[2] << 16) | ((uint32_t)field->bytes[3] << 24);
}

int
main(void)
{
	static const char *modules[] = { "virtio_blk", "crc32c_generic", "crc16", "mbcache", "jbd2",
		"ext4" };
	struct ext4_super_disk super;
	struct utsname identity;
	char module[128];
	FILE *configuration;
	uint8_t *buffer;
	uint8_t linux_byte = TEST_LINUX_BYTE;
	uint32_t block_size;
	uint32_t logarithm;
	uint32_t index;
	uint32_t position;
	uint8_t expected;
	int fd;
	int result;

	setvbuf(stdout, NULL, _IOLBF, 0);
	require(uname(&identity) == 0, "uname");
	printf(
	    "LINUX_EXT4_KERNEL=%s %s %s\n", identity.sysname, identity.release, identity.machine);
	require(mount("devtmpfs", "/dev", "devtmpfs", 0, NULL) == 0, "mount devtmpfs");
	for (index = 0; index < sizeof(modules) / sizeof(modules[0]); index++) {
		snprintf(module, sizeof(module), "/modules/%s.ko", modules[index]);
		fd = open(module, O_RDONLY | O_CLOEXEC);
		require(fd >= 0, "open module");
		result = (int)syscall(SYS_finit_module, fd, "", 0);
		require(result == 0 || errno == EEXIST, module);
		require(close(fd) == 0, "close module");
	}
	configuration = fopen("/block-size", "r");
	require(configuration != NULL, "open configuration");
	require(fscanf(configuration, "%u", &block_size) == 1, "read configuration");
	require(fclose(configuration) == 0, "close configuration");
	fd = open("/dev/vda", O_RDONLY | O_CLOEXEC);
	require(fd >= 0, "open disposable virtio device");
	require(pread(fd, &super, sizeof(super), EXT4_SUPER_OFFSET) == sizeof(super),
	    "read superblock");
	require(close(fd) == 0, "close device");
	logarithm = decode_le32(&super.log_block_size);
	require(logarithm <= 6 && (EXT4_MIN_BLOCK_SIZE << logarithm) == block_size,
	    "verify filesystem block size");
	require((decode_le32(&super.feature_incompat) & EXT4_FEATURE_INCOMPAT_RECOVER) != 0,
	    "verify journal requires recovery");
	require(mount("/dev/vda", "/mnt", "ext4", MS_NOATIME | MS_NOSUID | MS_NODEV,
		    "data=ordered") == 0,
	    "Linux ext4 mount and recovery");
	buffer = malloc((size_t)block_size * 2);
	require(buffer != NULL, "allocate comparison buffer");
	fd = open("/mnt/payload.bin", O_RDWR | O_CLOEXEC);
	require(fd >= 0, "open recovered payload");
	require(pread(fd, buffer, (size_t)block_size * 2, 0) == (ssize_t)block_size * 2,
	    "read recovered payload");
	for (position = 0; position < block_size * 2; position++) {
		expected = position < block_size ? 0x53 : 0xa7;
		if (position < 4) {
			expected = (uint8_t)(TEST_JOURNAL_MAGIC >> ((3U - position) * 8));
		}
		if (buffer[position] != expected) {
			fprintf(stderr, "incorrect payload byte at %u: got %u, expected %u\n",
			    position, buffer[position], expected);
			power_off(0);
		}
	}
	puts("LINUX_EXT4_REPLAY_PASS");
	/* Linux now authors a real inode transaction for the reverse roundtrip. */
	require(fchown(fd, TEST_UID, TEST_GID) == 0, "Linux chown");
	require(fchmod(fd, TEST_MODE) == 0, "Linux chmod");
	require(
	    pwrite(fd, &linux_byte, sizeof(linux_byte), 0) == sizeof(linux_byte), "Linux write");
	require(fsync(fd) == 0, "Linux fsync commit");
	puts("LINUX_EXT4_COMMITTED_RECOVERY_PENDING");
	free(buffer);
	power_off(1);
	return 1;
}

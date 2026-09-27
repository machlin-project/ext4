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

#ifdef EXT4_TEST_FILE_WRITES
static void
check_exported_files(uint32_t block_size, uint16_t inode_size)
{
	struct stat metadata;
	struct stat original;
	struct stat alias;
	uint8_t *bytes;
	uint8_t expected;
	size_t index;
	size_t start = block_size - 7;
	int fd;

	bytes = malloc(200000);
	require(bytes != NULL, "allocate whole-file comparison");
	fd = open("/mnt/payload.bin", O_RDONLY | O_CLOEXEC);
	require(fd >= 0, "open written payload");
	require(pread(fd, bytes, 200000, 0) == 200000, "read complete written payload");
	for (index = 0; index < 200000; index++) {
		expected = (uint8_t)(index * 17 + 23);
		if (index >= start && index - start < block_size + 23) {
			expected = (uint8_t)((index - start) * 29 + 7);
		}
		require(bytes[index] == expected, "compare native file write");
	}
	require(close(fd) == 0, "close written payload");
	free(bytes);
	require(stat("/mnt/metadata.txt", &metadata) == 0, "stat written metadata");
	require(metadata.st_uid == UINT32_MAX - 1U && metadata.st_gid == 0x81234567U &&
		(metadata.st_mode & 07777) == 0610,
	    "verify full-width owners and mode");
	require(metadata.st_atim.tv_sec == 0 && metadata.st_atim.tv_nsec == 0, "verify atime");
	if (inode_size == EXT4_INODE_BASE_SIZE) {
		require(metadata.st_mtim.tv_sec == 1700000001 && metadata.st_mtim.tv_nsec == 0 &&
			metadata.st_ctim.tv_sec == 1700000002 && metadata.st_ctim.tv_nsec == 0,
		    "verify legacy timestamps");
	} else {
		require(metadata.st_mtim.tv_sec == INT32_MIN &&
			metadata.st_mtim.tv_nsec == 123456789 &&
			metadata.st_ctim.tv_sec == INT64_C(15032385535) &&
			metadata.st_ctim.tv_nsec == 999999999,
		    "verify extended signed timestamps");
	}
	require(stat("/mnt/hello.txt", &original) == 0 && stat("/mnt/hello-hardlink", &alias) == 0,
	    "stat both hardlinks");
	require(original.st_ino == alias.st_ino && alias.st_nlink == 2 && alias.st_uid == 1111 &&
		alias.st_gid == 2222 && (alias.st_mode & 07777) == 0640,
	    "verify shared hardlink attributes");
	fd = open("/mnt/hello-hardlink", O_RDONLY | O_CLOEXEC);
	require(fd >= 0 && read(fd, &expected, 1) == 1 && expected == 'X', "verify hardlink write");
	require(close(fd) == 0, "close hardlink");
	puts("LINUX_EXT4_FILE_WRITE_PASS");
}
#endif

#ifdef EXT4_TEST_ALLOCATION
static uint64_t
check_allocated_file(uint32_t block_size)
{
	struct stat status;
	uint8_t *expected;
	uint8_t *observed;
	size_t capacity = (size_t)block_size * 724;
	size_t size = (size_t)block_size * 2 + 16;
	size_t offset;
	size_t index;
	uint32_t logical;
	int fd;

	expected = calloc(1, capacity);
	observed = malloc(capacity);
	require(expected != NULL && observed != NULL, "allocate sparse-file oracle");
	for (index = 0; index < block_size + 23; index++) {
		expected[block_size - 7 + index] = (uint8_t)(index * 29 + 7);
	}
	for (index = 0; index < 350; index++) {
		logical = 4 + (uint32_t)((index * 73) % 359) * 2;
		offset = (size_t)logical * block_size + index % 31;
		memset(expected + offset, (int)(index % 251 + 1), 17);
		if (size < offset + 17) {
			size = offset + 17;
		}
	}
	for (index = 0; index < block_size + 23; index++) {
		expected[block_size * 3 - 7 + index] = (uint8_t)(index * 29 + 7);
	}
	fd = open("/mnt/empty", O_RDONLY | O_CLOEXEC);
	require(fd >= 0 && fstat(fd, &status) == 0, "open/stat allocated file");
	require(status.st_size == (off_t)size, "verify grown size");
	require(pread(fd, observed, size, 0) == (ssize_t)size, "read grown file");
	require(memcmp(observed, expected, size) == 0, "compare allocated bytes and sparse gaps");
	require(close(fd) == 0, "close grown file");
	free(expected);
	free(observed);
	puts("LINUX_EXT4_ALLOCATION_PASS");
	return size;
}
#endif

#ifdef EXT4_TEST_TRUNCATE
static void
check_truncated_file(uint32_t block_size)
{
	struct stat status;
	uint8_t *observed;
	size_t size = (size_t)block_size * 9 + 13;
	size_t index;
	int fd;

	observed = malloc(size);
	require(observed != NULL, "allocate truncated-file oracle");
	fd = open("/mnt/empty", O_RDONLY | O_CLOEXEC);
	require(fd >= 0 && fstat(fd, &status) == 0, "open/stat truncated file");
	require(status.st_size == (off_t)size && status.st_blocks == block_size / EXT4_SECTOR_SIZE,
	    "verify truncated size and released mapping blocks");
	require(pread(fd, observed, size, 0) == (ssize_t)size, "read truncated file");
	for (index = 0; index < size; index++) {
		require(observed[index] == (index == (size_t)block_size * 4 + 7 ? 'T' : 0),
		    "verify zero growth and reused allocation");
	}
	require(close(fd) == 0, "close truncated file");
	free(observed);
	puts("LINUX_EXT4_TRUNCATE_PASS");
}
#endif

int
main(void)
{
	static const char *modules[] = { "virtio_blk", "crc32c_generic", "crc16", "mbcache", "jbd2",
		"ext4" };
	struct ext4_super_disk super;
	struct utsname identity;
	char module[128];
	FILE *configuration;
#if !defined(EXT4_TEST_FILE_WRITES) && !defined(EXT4_TEST_ALLOCATION) &&                           \
    !defined(EXT4_TEST_TRUNCATE)
	uint8_t *buffer;
	uint32_t position;
	uint8_t expected;
#endif
	off_t write_offset = 0;
	uint8_t linux_byte = TEST_LINUX_BYTE;
	uint32_t block_size;
	uint32_t logarithm;
	uint32_t index;
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
#if defined(EXT4_TEST_FILE_WRITES) || defined(EXT4_TEST_ALLOCATION) || defined(EXT4_TEST_TRUNCATE)
	require((decode_le32(&super.feature_incompat) & EXT4_FEATURE_INCOMPAT_RECOVER) == 0,
	    "verify cleanly finished writable filesystem");
#else
	require((decode_le32(&super.feature_incompat) & EXT4_FEATURE_INCOMPAT_RECOVER) != 0,
	    "verify journal requires recovery");
#endif
	require(mount("/dev/vda", "/mnt", "ext4", MS_NOATIME | MS_NOSUID | MS_NODEV,
		    "data=ordered") == 0,
	    "Linux ext4 mount and recovery");
#ifdef EXT4_TEST_FILE_WRITES
	check_exported_files(block_size,
	    (uint16_t)super.inode_size.bytes[0] | ((uint16_t)super.inode_size.bytes[1] << 8));
#endif

#ifdef EXT4_TEST_ALLOCATION
	write_offset = (off_t)(check_allocated_file(block_size) + block_size + 7);
	fd = open("/mnt/empty", O_RDWR | O_CLOEXEC);
#elif defined(EXT4_TEST_TRUNCATE)
	check_truncated_file(block_size);
	write_offset = (off_t)block_size * 10 + 7;
	fd = open("/mnt/empty", O_RDWR | O_CLOEXEC);
#else
	fd = open("/mnt/payload.bin", O_RDWR | O_CLOEXEC);
#endif
	require(fd >= 0, "open recovered payload");
#if !defined(EXT4_TEST_FILE_WRITES) && !defined(EXT4_TEST_ALLOCATION) &&                           \
    !defined(EXT4_TEST_TRUNCATE)
	buffer = malloc((size_t)block_size * 2);
	require(buffer != NULL, "allocate comparison buffer");
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
	free(buffer);
#endif
	/* Linux now authors a real inode transaction for the reverse roundtrip. */
	require(fchown(fd, TEST_UID, TEST_GID) == 0, "Linux chown");
	require(fchmod(fd, TEST_MODE) == 0, "Linux chmod");
#ifdef EXT4_TEST_TRUNCATE
	require(ftruncate(fd, (off_t)block_size * 3 + 9) == 0, "Linux shrink and release");
	require(ftruncate(fd, (off_t)block_size * 12 + 17) == 0, "Linux sparse growth");
#endif
	require(pwrite(fd, &linux_byte, sizeof(linux_byte), write_offset) == sizeof(linux_byte),
	    "Linux write");
	require(fsync(fd) == 0, "Linux fsync commit");
	puts("LINUX_EXT4_COMMITTED_RECOVERY_PENDING");
	power_off(1);
	return 1;
}

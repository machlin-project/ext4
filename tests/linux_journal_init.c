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
#include <time.h>
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
	/* Journal-write callers deliberately omit sync and unmount after fsync;
	 * recovery-only callers may explicitly unmount before reaching this point. */
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

static void
exercise_journal_revoke(void)
{
	int directory;

	require(mkdir("/mnt/checksum-revoke", 0700) == 0, "create journal revoke directory");
	directory = open("/mnt/checksum-revoke", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	require(directory >= 0, "open journal revoke directory");
	require(fsync(directory) == 0, "commit directory before releasing its block");
	require(close(directory) == 0, "close journal revoke directory");
	require(rmdir("/mnt/checksum-revoke") == 0, "release committed directory block");
	directory = open("/mnt", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	require(directory >= 0, "open parent after directory release");
	require(fsync(directory) == 0, "commit directory block revoke");
	require(close(directory) == 0, "close directory release parent");
	puts("LINUX_EXT4_CHECKSUM_REVOKE_PASS");
}

#ifdef EXT4_TEST_NAMESPACE
#include "linux_namespace.h"
#endif
#ifdef EXT4_TEST_XATTR_TRUNCATE
#include "linux_xattr_truncate.h"
#endif
#ifdef EXT4_TEST_XATTRS
#include "linux_xattrs.h"
#endif

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

#ifdef EXT4_TEST_LIVE_TRUNCATE
static int
check_live_truncated_file(uint32_t block_size, bool recovering,
    const struct timespec *recovery_start, const struct timespec *recovery_end)
{
	struct stat status;
	FILE *configuration;
	uint8_t *bytes;
	uint8_t expected;
	unsigned int empty;
	unsigned int zero;
	unsigned int blocks;
	size_t size = (size_t)block_size + 7;
	size_t index;
	int fd;
	bool captured;
	bool refreshed;

	configuration = fopen("/live-truncate", "r");
	require(configuration != NULL, "open live truncate configuration");
	require(fscanf(configuration, "%u %u %u", &empty, &zero, &blocks) == 3,
	    "read live truncate configuration");
	require(fclose(configuration) == 0, "close live truncate configuration");
	fd = open(empty ? "/mnt/empty" : "/mnt/payload.bin", O_RDWR | O_CLOEXEC);
	require(fd >= 0 && fstat(fd, &status) == 0, "open/stat live truncated file");
	printf("LINUX_EXT4_LIVE_STATE size=%lld blocks=%lld mode=%o mtime=%lld.%09ld "
	       "ctime=%lld.%09ld recovering=%u\n",
	    (long long)status.st_size, (long long)status.st_blocks,
	    (unsigned int)(status.st_mode & 07777), (long long)status.st_mtim.tv_sec,
	    status.st_mtim.tv_nsec, (long long)status.st_ctim.tv_sec, status.st_ctim.tv_nsec,
	    recovering ? 1U : 0U);
	printf("LINUX_EXT4_RECOVERY_WINDOW start=%lld end=%lld\n",
	    (long long)recovery_start->tv_sec, (long long)recovery_end->tv_sec);
	require(status.st_size == (off_t)size && status.st_blocks == blocks &&
		(status.st_mode & 07777) == 0640,
	    "verify live truncate allocation and permissions");
	captured = status.st_mtim.tv_sec == 1700000301 && status.st_mtim.tv_nsec == 0 &&
	    status.st_ctim.tv_sec == 1700000302 && status.st_ctim.tv_nsec == 0;
	/* Linux orphan cleanup refreshes both timestamps. Bound that transition
	 * to the actual mount interval, including legacy inode second precision. */
	refreshed = recovering && status.st_mtim.tv_sec == status.st_ctim.tv_sec &&
	    status.st_mtim.tv_nsec == status.st_ctim.tv_nsec &&
	    status.st_mtim.tv_sec >= recovery_start->tv_sec &&
	    status.st_mtim.tv_sec <= recovery_end->tv_sec && status.st_mtim.tv_nsec >= 0 &&
	    status.st_mtim.tv_nsec < EXT4_NANOSECONDS_PER_SECOND;
	require(captured || refreshed, "verify captured or Linux recovery timestamps");
	bytes = malloc(size);
	require(bytes != NULL, "allocate live truncate comparison");
	require(pread(fd, bytes, size, 0) == (ssize_t)size, "read live truncated file");
	for (index = 0; index < size; index++) {
		expected = zero ? 0 : (uint8_t)(index * 17 + 23);
		require(bytes[index] == expected, "verify live truncate retained bytes");
	}
	free(bytes);
	puts("LINUX_EXT4_LIVE_TRUNCATE_PASS");
	return fd;
}
#endif

#ifdef EXT4_TEST_ORPHANS
static void
create_orphans(uint32_t block_size, bool extents)
{
	static const char *paths[] = { "/mnt/orphan-regular", "/mnt/orphan-sparse",
		"/mnt/orphan-directory", "/mnt/orphan-short-link", "/mnt/orphan-long-link",
		"/mnt/orphan-fifo" };
	struct stat status;
	uint8_t *bytes;
	char target[257];
	uint64_t logical[6];
	uint64_t per_block = block_size / sizeof(struct ext4_le32);
	size_t length = (size_t)block_size * 3 + 37;
	size_t index;
	int descriptors[6];
	int root;

	bytes = malloc(length);
	require(bytes != NULL, "allocate orphan payload");
	for (index = 0; index < length; index++) {
		bytes[index] = (uint8_t)(index * 43 + 91);
	}
	descriptors[0] = open(paths[0], O_CREAT | O_EXCL | O_RDWR | O_CLOEXEC, 0640);
	require(descriptors[0] >= 0, "create regular orphan");
	require(write(descriptors[0], bytes, length) == (ssize_t)length, "write regular orphan");
	require(fsync(descriptors[0]) == 0, "commit regular orphan data");
	descriptors[1] = open(paths[1], O_CREAT | O_EXCL | O_RDWR | O_CLOEXEC, 0600);
	require(descriptors[1] >= 0, "create sparse orphan");
	logical[0] = 0;
	logical[1] = extents ? 7 : EXT4_DIRECT_BLOCKS - 1;
	logical[2] = extents ? 19 : EXT4_DIRECT_BLOCKS;
	logical[3] = extents ? 47 : EXT4_DIRECT_BLOCKS + per_block;
	logical[4] = extents ? 97 : EXT4_DIRECT_BLOCKS + per_block + per_block * per_block;
	logical[5] = extents ? 193 : logical[4] + per_block * per_block * per_block - 1;
	for (index = 0; index < sizeof(logical) / sizeof(logical[0]); index++) {
		require(pwrite(descriptors[1], bytes, 17,
			    (off_t)(logical[index] * block_size + 5)) == 17,
		    "write sparse orphan mapping");
		/* Separate durable writes prevent delayed-allocation merging from
		 * removing the intended sparse mapping boundaries. */
		require(fsync(descriptors[1]) == 0, "commit sparse orphan data");
	}
	require(mkdir(paths[2], 0700) == 0, "create orphan directory");
	descriptors[2] = open(paths[2], O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	require(symlink("target", paths[3]) == 0, "create short orphan symlink");
	descriptors[3] = open(paths[3], O_PATH | O_NOFOLLOW | O_CLOEXEC);
	memset(target, 'L', sizeof(target) - 1);
	target[sizeof(target) - 1] = 0;
	require(symlink(target, paths[4]) == 0, "create long orphan symlink");
	descriptors[4] = open(paths[4], O_PATH | O_NOFOLLOW | O_CLOEXEC);
	require(mkfifo(paths[5], 0600) == 0, "create orphan FIFO");
	descriptors[5] = open(paths[5], O_PATH | O_NOFOLLOW | O_CLOEXEC);
	for (index = 0; index < sizeof(descriptors) / sizeof(descriptors[0]); index++) {
		require(descriptors[index] >= 0, "retain orphan descriptor");
		require((index == 2 ? rmdir(paths[index]) : unlink(paths[index])) == 0,
		    "remove last namespace link");
		require(fstat(descriptors[index], &status) == 0 && status.st_nlink == 0,
		    "verify open unlinked inode");
		printf("LINUX_EXT4_ORPHAN index=%zu inode=%llu mode=%o size=%llu blocks=%llu\n",
		    index, (unsigned long long)status.st_ino, (unsigned int)status.st_mode,
		    (unsigned long long)status.st_size, (unsigned long long)status.st_blocks);
	}
	root = open("/mnt", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	require(root >= 0 && fsync(root) == 0, "commit orphan directory removals");
	require(syncfs(descriptors[0]) == 0, "commit live orphan list");
	free(bytes);
	puts("LINUX_EXT4_ORPHANS_PENDING");
	puts("LINUX_EXT4_COMMITTED_RECOVERY_PENDING");
	/* Leave every descriptor open until power-off, so ordinary last-close
	 * cleanup cannot remove the recovery work that this fixture exercises. */
	power_off(1);
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
#ifdef EXT4_TEST_LIVE_TRUNCATE
	struct timespec recovery_start;
	struct timespec recovery_end;
#endif
#if !defined(EXT4_TEST_FILE_WRITES) && !defined(EXT4_TEST_ALLOCATION) &&                           \
    !defined(EXT4_TEST_TRUNCATE) && !defined(EXT4_TEST_LIVE_TRUNCATE)
	uint8_t *buffer;
	uint32_t position;
	uint8_t expected;
#endif
	off_t write_offset = 0;
	const char *mount_options = "data=ordered";
	uint8_t linux_byte = TEST_LINUX_BYTE;
	uint32_t block_size;
	uint32_t logarithm;
	uint32_t index;
	int fd;
	int result;
	int checksum_v1;
	int async_commit;
	int no_delalloc;

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
#if defined(EXT4_TEST_FILE_WRITES) || defined(EXT4_TEST_ALLOCATION) ||                             \
    defined(EXT4_TEST_TRUNCATE) || defined(EXT4_TEST_ORPHANS)
	require((decode_le32(&super.feature_incompat) & EXT4_FEATURE_INCOMPAT_RECOVER) == 0,
	    "verify cleanly finished writable filesystem");
#elif !defined(EXT4_TEST_LIVE_TRUNCATE) && !defined(EXT4_TEST_NAMESPACE) &&                        \
    !defined(EXT4_TEST_XATTR_TRUNCATE) && !defined(EXT4_TEST_XATTRS)
	require((decode_le32(&super.feature_incompat) & EXT4_FEATURE_INCOMPAT_RECOVER) != 0,
	    "verify journal requires recovery");
#endif
#ifdef EXT4_TEST_LIVE_TRUNCATE
	require(
	    clock_gettime(CLOCK_REALTIME_COARSE, &recovery_start) == 0, "read recovery start time");
#endif
	checksum_v1 = access("/journal-checksum-v1", F_OK) == 0;
	async_commit = access("/journal-async-commit", F_OK) == 0;
	no_delalloc = access("/no-delalloc", F_OK) == 0;
	if (no_delalloc) {
		mount_options = "data=ordered,nodelalloc";
	}
	if (checksum_v1) {
		require(
		    (decode_le32(&super.feature_ro_compat) & EXT4_FEATURE_RO_METADATA_CSUM) == 0,
		    "verify native checksum v1 filesystem profile");
		mount_options = no_delalloc ? "data=ordered,journal_checksum,nodelalloc"
					    : "data=ordered,journal_checksum";
	}
	if (async_commit) {
		/* Linux omits revoke records in data=journal mode. Use writeback
		 * here and fsync the changed file before stopping the guest. */
		mount_options = no_delalloc ? "data=writeback,journal_async_commit,nodelalloc"
					    : "data=writeback,journal_async_commit";
	}
	printf("LINUX_EXT4_MOUNT_OPTIONS=%s\n", mount_options);
	require(mount("/dev/vda", "/mnt", "ext4", MS_NOATIME | MS_NOSUID | MS_NODEV,
		    mount_options) == 0,
	    "Linux ext4 mount and recovery");
#ifdef EXT4_TEST_LIVE_TRUNCATE
	require(clock_gettime(CLOCK_REALTIME, &recovery_end) == 0, "read recovery end time");
#endif
#ifdef EXT4_TEST_ORPHANS
	create_orphans(block_size,
	    (decode_le32(&super.feature_incompat) & EXT4_FEATURE_INCOMPAT_EXTENTS) != 0);
	return 1;
#endif
#ifdef EXT4_TEST_NAMESPACE
	check_namespace(block_size);
	return 1;
#endif
#ifdef EXT4_TEST_XATTR_TRUNCATE
	check_xattr_truncate(block_size);
	return 1;
#endif
#ifdef EXT4_TEST_XATTRS
	check_xattrs();
	return 1;
#endif
#ifdef EXT4_TEST_FILE_WRITES
	check_exported_files(block_size,
	    (uint16_t)super.inode_size.bytes[0] | ((uint16_t)super.inode_size.bytes[1] << 8));
#endif

#ifdef EXT4_TEST_LIVE_TRUNCATE
	fd = check_live_truncated_file(block_size,
	    (decode_le32(&super.feature_incompat) & EXT4_FEATURE_INCOMPAT_RECOVER) != 0 ||
		decode_le32(&super.last_orphan) != 0,
	    &recovery_start, &recovery_end);
	write_offset = (off_t)block_size * 4 + 7;
#elif defined(EXT4_TEST_ALLOCATION)
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
    !defined(EXT4_TEST_TRUNCATE) && !defined(EXT4_TEST_LIVE_TRUNCATE)
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
	if (checksum_v1 || async_commit) {
		exercise_journal_revoke();
	}
	require(fchown(fd, TEST_UID, TEST_GID) == 0, "Linux chown");
	require(fchmod(fd, TEST_MODE) == 0, "Linux chmod");
#ifdef EXT4_TEST_TRUNCATE
	require(ftruncate(fd, (off_t)block_size * 3 + 9) == 0, "Linux shrink and release");
	require(ftruncate(fd, (off_t)block_size * 12 + 17) == 0, "Linux sparse growth");
#endif
#ifdef EXT4_TEST_LIVE_TRUNCATE
	require(ftruncate(fd, (off_t)block_size * 6 + 13) == 0, "Linux grow truncated file");
#endif
	require(pwrite(fd, &linux_byte, sizeof(linux_byte), write_offset) == sizeof(linux_byte),
	    "Linux write");
	require(fsync(fd) == 0, "Linux fsync commit");
	puts("LINUX_EXT4_COMMITTED_RECOVERY_PENDING");
	power_off(1);
	return 1;
}

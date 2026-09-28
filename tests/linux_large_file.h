/* SPDX-License-Identifier: BSD-3-Clause */
#ifndef MACHLIN_EXT4_TEST_LINUX_LARGE_FILE_H
#define MACHLIN_EXT4_TEST_LINUX_LARGE_FILE_H

#include <sys/ioctl.h>

#define LINUX_LARGE_BYTE_BOUNDARY (UINT64_C(1) << 32)
/* Linux UAPI encodes long while copying a 32-bit inode flag word. */
#define LINUX_LARGE_GETFLAGS _IOR('f', 1, long)

static void
large_file_checks(void)
{
	struct stat status;
	FILE *list;
	char path[128];
	char source[128];
	uint8_t *expected;
	uint8_t *observed;
	unsigned long long number;
	unsigned long long size;
	unsigned long long sectors;
	unsigned long long offset;
	unsigned int mode;
	unsigned int uid;
	unsigned int gid;
	size_t length;
	int fd;
	int fields;

	list = fopen("/large-file-inodes", "r");
	require(list != NULL, "open large-file metadata expectations");
	while ((fields = fscanf(list, "%127s %llu %llu %llu %o %u %u", path, &number, &size,
		    &sectors, &mode, &uid, &gid)) != EOF) {
		require(fields == 7 && stat(path, &status) == 0 &&
			(unsigned long long)status.st_ino == number &&
			(unsigned long long)status.st_size == size &&
			(unsigned long long)status.st_blocks == sectors &&
			(status.st_mode & 07777U) == mode && S_ISREG(status.st_mode) &&
			status.st_uid == uid && status.st_gid == gid,
		    "Linux exact large-file metadata");
	}
	require(fclose(list) == 0, "close large-file metadata expectations");
	list = fopen("/large-file-slices", "r");
	require(list != NULL, "open large-file bounded data expectations");
	while ((fields = fscanf(list, "%127s %llu %127s", path, &offset, source)) != EOF) {
		require(fields == 3, "parse large-file bounded slice");
		expected = xattr_load(source, &length);
		observed = malloc(length);
		require(observed != NULL, "allocate large-file bounded read");
		fd = open(path, O_RDONLY | O_CLOEXEC);
		require(fd >= 0 && pread(fd, observed, length, (off_t)offset) == (ssize_t)length &&
			memcmp(observed, expected, length) == 0 && close(fd) == 0,
		    "Linux exact high-offset bytes and holes");
		free(observed);
		free(expected);
	}
	require(fclose(list) == 0, "close large-file bounded data expectations");
}

static void
large_file_write(int fd, uint64_t offset, uint8_t value, size_t length)
{
	uint8_t bytes[64];
	ssize_t written;

	require(length <= sizeof(bytes), "bound large-file write");
	memset(bytes, value, length);
	written = pwrite(fd, bytes, length, (off_t)offset);
	if (written != (ssize_t)length) {
		fprintf(stderr, "Large-file write offset=%llu length=%zu completed=%lld\n",
		    (unsigned long long)offset, length, (long long)written);
	}
	require(written == (ssize_t)length, "Linux write at large logical offset");
}

static void
check_large_files(void)
{
	FILE *configuration;
	struct stat status;
	struct stat before;
	unsigned long long limit = 0;
	unsigned long flags = 0;
	unsigned int block = 0;
	int verify_only = 0;
	int inline_profile = 0;
	int fd;
	int orphan;
	uint8_t byte = 'Q';

	configuration = fopen("/large-file-geometry", "r");
	require(configuration != NULL &&
		fscanf(configuration, "%u %llu %d %d", &block, &limit, &verify_only,
		    &inline_profile) == 4 &&
		fclose(configuration) == 0 && block >= 1024 && block <= 4096 &&
		limit > LINUX_LARGE_BYTE_BOUNDARY && limit % block == 0,
	    "read large-file roundtrip geometry");
	large_file_checks();
	if (verify_only) {
		require(umount("/mnt") == 0, "unmount verified large files");
		puts("LINUX_EXT4_LARGE_FILE_RETURN_PASS");
		puts("LINUX_EXT4_XATTR_RETURN_PASS");
		power_off(1);
	}
	fd = open("/mnt/created", O_RDWR | O_CLOEXEC);
	require(fd >= 0, "open core-authored large file");
	errno = 0;
	require(ftruncate(fd, (off_t)(limit + 1U)) == -1 && errno == EFBIG,
	    "Linux agrees with the format size ceiling");
	errno = 0;
	require(pwrite(fd, &byte, 1, (off_t)limit) == -1 && errno == EFBIG,
	    "Linux rejects a write beyond the format ceiling");
	large_file_write(fd, LINUX_LARGE_BYTE_BOUNDARY - 3U, 'L', 7);
	require(ftruncate(fd, (off_t)(limit - block)) == 0 && ftruncate(fd, (off_t)limit) == 0,
	    "Linux truncates and regrows at the address ceiling");
	large_file_write(fd, limit - 1U, 'N', 1);
	require(fsync(fd) == 0 && close(fd) == 0, "commit Linux high-offset overwrite");
	umask(0);
	fd = open("/mnt/linux-large", O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC, 0640);
	require(fd >= 0, "create Linux large-file origin");
	large_file_write(fd, 0, 0x64, 61);
	if (inline_profile) {
		/* The pinned Linux write check applies its indirect limit before
		 * inline conversion. Record that behavior, then convert at a small
		 * offset before exercising the valid extent address space. */
		require(ioctl(fd, LINUX_LARGE_GETFLAGS, &flags) == 0 &&
			(flags & EXT4_INODE_INLINE_DATA) && !(flags & EXT4_INODE_EXTENTS) &&
			fstat(fd, &before) == 0 && before.st_size == 61,
		    "capture Linux inline state before rejected growth");
		errno = 0;
		require(pwrite(fd, &byte, 1, (off_t)(limit - 1U)) == -1 && errno == EFBIG,
		    "Linux inline high-offset rejection before conversion");
		/* Linux reports virtual sectors for inline data through stat, even
		 * when the on-disk allocation charge is zero. Compare unchanged
		 * observed accounting across the rejected operation. */
		require(fstat(fd, &status) == 0 && status.st_size == before.st_size &&
			status.st_blocks == before.st_blocks,
		    "Linux rejected inline growth preserves size and allocation");
		large_file_write(fd, block, 0, 1);
		/* Delayed allocation completes this conversion during writeback. */
		require(fsync(fd) == 0 && ftruncate(fd, 61) == 0 && fsync(fd) == 0 &&
			ioctl(fd, LINUX_LARGE_GETFLAGS, &flags) == 0 &&
			(flags & EXT4_INODE_EXTENTS) && !(flags & EXT4_INODE_INLINE_DATA),
		    "convert native inline storage before extent-limit write");
		puts("LINUX_EXT4_INLINE_HIGH_OFFSET_REJECTED_BEFORE_CONVERSION");
	}
	large_file_write(fd, limit - 1U, 'X', 1);
	large_file_write(fd, LINUX_LARGE_BYTE_BOUNDARY - 3U, 'V', 7);
	require(fsync(fd) == 0 && close(fd) == 0, "commit Linux sparse creation");
	orphan = open("/mnt/large-orphan", O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
	require(orphan >= 0, "create high-offset Linux orphan");
	large_file_write(orphan, LINUX_LARGE_BYTE_BOUNDARY - 3U, 'O', 7);
	require(fsync(orphan) == 0, "complete native orphan mapping before extent-limit growth");
	large_file_write(orphan, limit - 1U, 'O', 1);
	require(fsync(orphan) == 0 && unlink("/mnt/large-orphan") == 0,
	    "commit high-offset open-unlinked Linux inode");
	xattr_sync_path("/mnt");
	puts("LINUX_EXT4_LARGE_FILE_PASS");
	puts("LINUX_EXT4_XATTR_PASS");
	puts("LINUX_EXT4_COMMITTED_RECOVERY_PENDING");
	power_off(1);
}

#endif

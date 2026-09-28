/* SPDX-License-Identifier: BSD-3-Clause */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/reboot.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/syscall.h>
#include <sys/sysmacros.h>
#include <sys/utsname.h>
#include <sys/xattr.h>
#include <unistd.h>

#define CREATED_FILES 32U
#define CREATED_MODE 0604U
#define SPARSE_BLOCK 8U
#define SPARSE_OFFSET 11U
#define TAIL_BYTES 513U
#define LONG_FILES 12U
#define LONG_NAME_BYTES 230U
#define SCATTER_EXTENTS 32U
#define RESERVED_START 100U
#define RESERVED_BLOCKS 8U
#define INITIALIZED_BLOCK 102U
#define INITIALIZED_OFFSET 7U
#define SYMLINK_INLINE_BYTES 60U
#define ATTRIBUTE_VALUE_MAX 65536U
#define ATTRIBUTE_COUNT 8U
#define ATTRIBUTE_SURVIVORS 6U
#define PHASE_PROTOCOL_READBACK 2U
#define PHASE_INDIRECT_READBACK 3U
#define PHASE_LARGE_PREFIX_READBACK 4U
#define LARGE_PREFIX_FILES_1K 256U
#define LARGE_PREFIX_FILES_4K 1024U
#define PROTOCOL_SOURCE_BLOCKS 5U
#define DIRECT_BLOCKS 12U
#define INDIRECT_POINTER_BYTES 4U

static const struct {
	const char *path;
	mode_t mode;
	unsigned int major;
	unsigned int minor;
} special_nodes[] = {
	{ "/mnt/character-legacy", S_IFCHR, 255, 255 },
	{ "/mnt/character-wide", S_IFCHR, 256, 256 },
	{ "/mnt/character-zero", S_IFCHR, 0, 0 },
	{ "/mnt/block-wide", S_IFBLK, 4095, 1048575 },
	{ "/mnt/fifo", S_IFIFO, 0, 0 },
	{ "/mnt/socket", S_IFSOCK, 0, 0 },
};

static const char *link_names[] = { "/mnt/link-short", "/mnt/link-59", "/mnt/link-60" };

static void
power_off(int passed)
{
	printf("LINUX_FAST_COMMIT_RESULT=%s\n", passed ? "PASS" : "FAIL");
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
small_file_name(char *path, size_t size, unsigned int index)
{
	int length;

	length = snprintf(path, size, "/mnt/item-%02u", index);
	require(length > 0 && (size_t)length < size, "form fixture filename");
}

static size_t
small_file_data(char *bytes, size_t size, unsigned int index)
{
	int length;

	length = snprintf(bytes, size, "Native fast commit file %02u\n", index);
	require(length > 0 && (size_t)length < size, "form fixture contents");
	return (size_t)length;
}

static void
long_file_name(char *path, size_t size, unsigned int index)
{
	int prefix;
	size_t length;

	prefix = snprintf(path, size, "/mnt/new-dir/entry-%02u-", index);
	require(prefix > 0 && (size_t)prefix + LONG_NAME_BYTES < size, "form long pathname");
	length = (size_t)prefix - strlen("/mnt/new-dir/");
	memset(path + prefix, 'a' + (int)(index % 26U), LONG_NAME_BYTES - length);
	path[(size_t)prefix + LONG_NAME_BYTES - length] = 0;
}

static void
verify_extra(unsigned int block_size)
{
	struct stat metadata;
	uint8_t bytes[4096];
	uint8_t expected;
	char path[512];
	size_t position;
	size_t length;
	off_t offset;
	off_t size = (off_t)INITIALIZED_BLOCK * block_size + INITIALIZED_OFFSET + 1U;
	unsigned int index;
	int fd;

	require(access("/mnt/victim", F_OK) < 0 && errno == ENOENT, "verify final unlink");
	require(access("/mnt/open-unlinked", F_OK) < 0 && errno == ENOENT,
	    "verify checkpointed orphan has no name");
	require(stat("/mnt/new-dir", &metadata) == 0 && S_ISDIR(metadata.st_mode) &&
		metadata.st_nlink == 2,
	    "verify created directory");
	for (index = 0; index < LONG_FILES; index++) {
		long_file_name(path, sizeof(path), index);
		fd = open(path, O_RDONLY | O_CLOEXEC);
		require(fd >= 0 && read(fd, bytes, sizeof(bytes)) == 1 && bytes[0] == 'a' + index,
		    "verify long-name created file");
		require(close(fd) == 0, "close long-name file");
	}
	fd = open("/mnt/ranges", O_RDONLY | O_CLOEXEC);
	require(fd >= 0 && fstat(fd, &metadata) == 0 && metadata.st_size == size,
	    "open fragmented and preallocated file");
	for (offset = 0; offset < size; offset += (off_t)length) {
		length = (uint64_t)(size - offset) < sizeof(bytes) ? (size_t)(size - offset)
								   : sizeof(bytes);
		require(read(fd, bytes, length) == (ssize_t)length, "read fragmented file");
		for (position = 0; position < length; position++) {
			expected = 0;
			if ((uint64_t)offset + position ==
			    (uint64_t)INITIALIZED_BLOCK * block_size + INITIALIZED_OFFSET) {
				expected = 'E';
			} else if (((uint64_t)offset + position) % (2U * block_size) == 0) {
				index = (unsigned int)(((uint64_t)offset + position) /
				    (2U * block_size));
				if (index < SCATTER_EXTENTS && index != 4U) {
					expected = (uint8_t)('D' + index % 26U);
				}
			}
			require(bytes[position] == expected,
			    "verify holes, unwritten data and initialized byte");
		}
	}
	require(close(fd) == 0, "close fragmented file");
}

static void
verify(unsigned int block_size)
{
	struct stat metadata;
	struct stat alias;
	uint8_t *bytes;
	uint8_t expected;
	char path[128];
	char contents[64];
	size_t size = (size_t)SPARSE_BLOCK * block_size + SPARSE_OFFSET + TAIL_BYTES;
	size_t offset;
	size_t length;
	unsigned int index;
	int fd;

	bytes = malloc(size);
	require(bytes != NULL, "allocate native verification buffer");
	fd = open("/mnt/hello.txt", O_RDONLY | O_CLOEXEC);
	require(fd >= 0 && fstat(fd, &metadata) == 0, "open sparse fast-commit file");
	require(metadata.st_size == (off_t)size && (metadata.st_mode & 07777) == 0640,
	    "verify sparse size and permissions");
	require(read(fd, bytes, size) == (ssize_t)size, "read sparse fast-commit data");
	for (offset = 0; offset < size; offset++) {
		if (offset < block_size + 3U) {
			expected = 'B';
		} else if (offset >= 2U * block_size && offset < 3U * block_size) {
			expected = 0;
		} else if (offset < 3U * block_size + 17U) {
			expected = 'A';
		} else if (offset < (size_t)SPARSE_BLOCK * block_size + SPARSE_OFFSET) {
			expected = 0;
		} else {
			expected = 'C';
		}
		require(bytes[offset] == expected, "verify fast-commit range and sparse gap");
	}
	require(close(fd) == 0, "close sparse file");
	for (index = 0; index < CREATED_FILES; index++) {
		small_file_name(path, sizeof(path), index);
		if (index == 0) {
			require(access(path, F_OK) < 0 && errno == ENOENT,
			    "verify renamed source is absent");
			strcpy(path, "/mnt/moved");
		}
		length = small_file_data(contents, sizeof(contents), index);
		fd = open(path, O_RDONLY | O_CLOEXEC);
		require(fd >= 0 && fstat(fd, &metadata) == 0, "open replayed created file");
		require(
		    metadata.st_size == (off_t)length && (metadata.st_mode & 07777) == CREATED_MODE,
		    "verify replayed inode fields");
		require(read(fd, bytes, size) == (ssize_t)length &&
			memcmp(bytes, contents, length) == 0,
		    "verify replayed created data");
		require(metadata.st_nlink == (index == 0 ? 2U : 1U), "verify final link counts");
		if (index == 0) {
			require(stat("/mnt/alias", &alias) == 0 && alias.st_ino == metadata.st_ino,
			    "verify retained hardlink identity");
		}
		require(close(fd) == 0, "close replayed created file");
	}
	free(bytes);
	verify_extra(block_size);
}

static void
special_target(char *target, unsigned int index)
{
	size_t length;

	if (index == 0) {
		strcpy(target, "hello.txt");
		return;
	}
	length = index == 1 ? SYMLINK_INLINE_BYTES - 1U : SYMLINK_INLINE_BYTES;
	memset(target, index == 1 ? 'a' : 'b', length);
	target[length] = 0;
}

static void
verify_specials(const char *short_target)
{
	struct stat metadata;
	char target[SYMLINK_INLINE_BYTES + 1U];
	char actual[sizeof(target)];
	size_t length;
	unsigned int index;

	for (index = 0; index < sizeof(link_names) / sizeof(link_names[0]); index++) {
		special_target(target, index);
		if (index == 0) {
			strcpy(target, short_target);
		}
		length = strlen(target);
		require(lstat(link_names[index], &metadata) == 0 && S_ISLNK(metadata.st_mode) &&
			metadata.st_size == (off_t)length &&
			readlink(link_names[index], actual, sizeof(actual)) == (ssize_t)length &&
			memcmp(actual, target, length) == 0,
		    "verify exact symlink target");
	}
	for (index = 0; index < sizeof(special_nodes) / sizeof(special_nodes[0]); index++) {
		require(lstat(special_nodes[index].path, &metadata) == 0 &&
			metadata.st_mode == (special_nodes[index].mode | 0640) &&
			metadata.st_nlink == 1 && metadata.st_size == 0 &&
			metadata.st_blocks == 0 &&
			major(metadata.st_rdev) == special_nodes[index].major &&
			minor(metadata.st_rdev) == special_nodes[index].minor,
		    "verify special inode and device identity");
	}
	puts("LINUX_FAST_COMMIT_SPECIALS_PASS");
}

static void
mutate_specials(void)
{
	char target[SYMLINK_INLINE_BYTES + 1U];
	unsigned int index;
	int fd;

	for (index = 0; index < sizeof(link_names) / sizeof(link_names[0]); index++) {
		special_target(target, index);
		require(symlink(target, link_names[index]) == 0, "create symlink");
	}
	for (index = 0; index < sizeof(special_nodes) / sizeof(special_nodes[0]); index++) {
		require(mknod(special_nodes[index].path, special_nodes[index].mode | 0640,
			    makedev(special_nodes[index].major, special_nodes[index].minor)) == 0,
		    "create special inode");
	}
	/* The pinned Linux reference forces a full commit for these inode types.
	 * Its fsync must supersede the earlier fast prefix before the crash. */
	fd = open("/mnt/hello.txt", O_RDWR | O_CLOEXEC);
	require(fd >= 0 && pwrite(fd, "B", 1, 0) == 1 && fsync(fd) == 0 && close(fd) == 0,
	    "commit queued symlinks and special inodes");
	verify_specials("hello.txt");
}

static void
mutate_extra(unsigned int block_size)
{
	char path[512];
	uint8_t byte;
	unsigned int index;
	int descriptors[LONG_FILES];
	int fd;

	fd = open("/mnt/victim", O_RDWR | O_CLOEXEC);
	require(fd >= 0 && unlink("/mnt/victim") == 0 && fsync(fd) == 0,
	    "fast commit final unlink while the inode is open");
	require(close(fd) == 0, "close unlinked inode");
	require(mkdir("/mnt/new-dir", 0750) == 0, "create logged directory");
	for (index = 0; index < LONG_FILES; index++) {
		long_file_name(path, sizeof(path), index);
		descriptors[index] =
		    open(path, O_CREAT | O_EXCL | O_RDWR | O_CLOEXEC, CREATED_MODE);
		byte = (uint8_t)('a' + index);
		require(descriptors[index] >= 0 && write(descriptors[index], &byte, 1) == 1,
		    "create queued long-name file");
	}
	/* Several queued creates force records to cross journal block boundaries. */
	for (index = 0; index < LONG_FILES; index++) {
		require(fsync(descriptors[index]) == 0 && close(descriptors[index]) == 0,
		    "commit queued long-name files");
	}
	fd = open("/mnt/ranges", O_CREAT | O_EXCL | O_RDWR | O_CLOEXEC, CREATED_MODE);
	require(fd >= 0, "create fragmented file");
	for (index = 0; index < SCATTER_EXTENTS; index++) {
		byte = (uint8_t)('D' + index % 26U);
		require(pwrite(fd, &byte, 1, (off_t)index * 2U * block_size) == 1,
		    "create separate extent");
	}
	require(fsync(fd) == 0, "commit extent tree split");
	require(fallocate(fd, FALLOC_FL_PUNCH_HOLE | FALLOC_FL_KEEP_SIZE, (off_t)8U * block_size,
		    block_size) == 0,
	    "punch fragmented extent");
	require(fallocate(fd, FALLOC_FL_KEEP_SIZE, (off_t)RESERVED_START * block_size,
		    (off_t)RESERVED_BLOCKS * block_size) == 0 &&
		fsync(fd) == 0,
	    "commit unwritten extents");
	require(
	    pwrite(fd, "E", 1, (off_t)INITIALIZED_BLOCK * block_size + INITIALIZED_OFFSET) == 1 &&
		fsync(fd) == 0,
	    "commit partial unwritten initialization");
	require(close(fd) == 0, "close fragmented file");
}

static void
mutate(unsigned int block_size, unsigned int modern_orphans, unsigned int special_files)
{
	struct stat held_metadata;
	uint8_t *bytes;
	char path[128];
	char contents[64];
	char line[256];
	FILE *stats;
	size_t length;
	unsigned long commits = 0;
	unsigned long ineligible = 0;
	unsigned int index;
	int fd;
	int held = -1;

	bytes = malloc(block_size + 3U);
	require(bytes != NULL, "allocate native mutation buffer");
	fd = open("/mnt/victim", O_CREAT | O_EXCL | O_RDWR | O_CLOEXEC, CREATED_MODE);
	memset(bytes, 'V', block_size + 3U);
	require(fd >= 0 && write(fd, bytes, block_size + 3U) == (ssize_t)block_size + 3U &&
		close(fd) == 0,
	    "create checkpointed unlink target");
	if (modern_orphans) {
		held =
		    open("/mnt/open-unlinked", O_CREAT | O_EXCL | O_RDWR | O_CLOEXEC, CREATED_MODE);
		require(held >= 0 &&
			write(held, bytes, block_size + 3U) == (ssize_t)block_size + 3 &&
			unlink("/mnt/open-unlinked") == 0,
		    "create checkpointed open-unlinked orphan");
	}
	fd = open("/mnt/hello.txt", O_RDWR | O_CLOEXEC);
	require(fd >= 0, "open initial file");
	/* Finish one ordinary transaction before asking Linux for fast commits. */
	require(pwrite(fd, "A", 1, 0) == 1 && syncfs(fd) == 0, "checkpoint initial transaction");
	if (held >= 0) {
		require(fstat(held, &held_metadata) == 0 && held_metadata.st_nlink == 0 &&
			held_metadata.st_blocks != 0,
		    "retain allocated orphan through checkpoint and crash");
		printf("LINUX_FAST_COMMIT_HELD_ORPHAN=%llu\n",
		    (unsigned long long)held_metadata.st_ino);
	}
	memset(bytes, 'B', block_size + 3U);
	require(pwrite(fd, bytes, block_size + 3U, 0) == (ssize_t)block_size + 3,
	    "overwrite range for fast commit");
	require(fsync(fd) == 0, "commit overwritten range");
	require(fallocate(fd, FALLOC_FL_PUNCH_HOLE | FALLOC_FL_KEEP_SIZE, (off_t)2U * block_size,
		    block_size) == 0 &&
		fsync(fd) == 0,
	    "commit removal of an allocated range");
	memset(bytes, 'C', TAIL_BYTES);
	require(pwrite(fd, bytes, TAIL_BYTES, (off_t)SPARSE_BLOCK * block_size + SPARSE_OFFSET) ==
		TAIL_BYTES,
	    "write sparse appended range");
	require(fchmod(fd, 0640) == 0 && fsync(fd) == 0, "commit sparse range and metadata");
	require(close(fd) == 0, "close changed file");
	for (index = 0; index < CREATED_FILES; index++) {
		small_file_name(path, sizeof(path), index);
		length = small_file_data(contents, sizeof(contents), index);
		fd = open(path, O_CREAT | O_EXCL | O_RDWR | O_CLOEXEC, CREATED_MODE);
		require(fd >= 0 && write(fd, contents, length) == (ssize_t)length,
		    "create fast-commit file");
		require(fsync(fd) == 0, "commit created file");
		if (index == 0) {
			require(link(path, "/mnt/alias") == 0 && fsync(fd) == 0, "commit hardlink");
			require(rename(path, "/mnt/moved") == 0 && fsync(fd) == 0,
			    "commit same-directory rename");
		}
		require(close(fd) == 0, "close created file");
	}
	free(bytes);
	mutate_extra(block_size);
	if (special_files) {
		mutate_specials();
	}
	verify(block_size);
	stats = fopen("/proc/fs/ext4/vda/fc_info", "r");
	require(stats != NULL, "open native fast-commit statistics");
	puts("LINUX_FAST_COMMIT_STATS_BEGIN");
	while (fgets(line, sizeof(line), stats) != NULL) {
		fputs(line, stdout);
		if (strstr(line, " commits\n") != NULL) {
			require(sscanf(line, "%lu commits", &commits) == 1,
			    "decode native fast-commit count");
		} else if (strstr(line, " ineligible\n") != NULL) {
			require(sscanf(line, "%lu ineligible", &ineligible) == 1,
			    "decode native full-commit fallback count");
		}
	}
	require(fclose(stats) == 0 && commits != 0, "require actual native fast commits");
	require(ineligible == (special_files ? 1UL : 0UL), "verify expected full-commit fallback");
	puts("LINUX_FAST_COMMIT_STATS_END");
	/* Keep the mount dirty and held open-unlinked descriptor alive; the next
	 * owner must recover the journal and reclaim its checkpointed orphan. */
	power_off(1);
}

static void
verify_uniform_file(const char *path, size_t size, mode_t mode, uint8_t value)
{
	struct stat metadata;
	uint8_t bytes[4096];
	size_t offset;
	size_t length;
	size_t position;
	int fd;

	fd = open(path, O_RDONLY | O_CLOEXEC);
	require(fd >= 0 && fstat(fd, &metadata) == 0 && metadata.st_mode == (S_IFREG | mode) &&
		metadata.st_nlink == 1 && metadata.st_size == (off_t)size,
	    "verify protocol file metadata");
	for (offset = 0; offset < size; offset += length) {
		length = size - offset < sizeof(bytes) ? size - offset : sizeof(bytes);
		require(read(fd, bytes, length) == (ssize_t)length, "read protocol file");
		for (position = 0; position < length; position++) {
			require(bytes[position] == value, "verify every protocol file byte");
		}
	}
	require(read(fd, bytes, 1) == 0 && close(fd) == 0, "verify protocol EOF and close");
}

static void
verify_indirect(unsigned int block_size)
{
	const char *removed[] = { "/mnt/hello.txt", "/mnt/victim", "/mnt/final-delete",
		"/mnt/orphan-held", "/mnt/legacy" };
	struct stat metadata;
	struct stat alias;
	uint8_t bytes[4096];
	uint8_t expected;
	char path[512];
	unsigned int per_block = block_size / INDIRECT_POINTER_BYTES;
	unsigned int blocks = DIRECT_BLOCKS + per_block + 2U;
	unsigned int logical;
	unsigned int index;
	size_t size = (size_t)blocks * block_size;
	size_t position;
	int fd;

	for (index = 0; index < sizeof(removed) / sizeof(removed[0]); index++) {
		require(access(removed[index], F_OK) < 0 && errno == ENOENT,
		    "verify old indirect generation and orphan names are absent");
	}
	require(stat("/mnt/new-dir", &metadata) == 0 && S_ISDIR(metadata.st_mode) &&
		metadata.st_nlink == 2,
	    "verify indirect directory");
	fd = open("/mnt/renamed", O_RDONLY | O_CLOEXEC);
	require(fd >= 0 && fstat(fd, &metadata) == 0 && metadata.st_mode == (S_IFREG | 0640) &&
		metadata.st_size == (off_t)size && metadata.st_nlink == 2 &&
		stat("/mnt/alias", &alias) == 0 && metadata.st_ino == alias.st_ino,
	    "verify indirect sparse inode and hardlink");
	for (logical = 0; logical < blocks; logical++) {
		expected = logical == 0 || logical == DIRECT_BLOCKS - 1U ||
			logical == DIRECT_BLOCKS || logical == DIRECT_BLOCKS + per_block - 1U ||
			logical >= blocks - 2U
		    ? 'A'
		    : 0;
		require(read(fd, bytes, block_size) == (ssize_t)block_size,
		    "read direct, single and double-indirect data and holes");
		for (position = 0; position < block_size; position++) {
			require(bytes[position] == expected, "verify indirect logical mapping");
		}
	}
	require(read(fd, bytes, 1) == 0 && close(fd) == 0, "verify indirect EOF and close");
	for (index = 0; index < LONG_FILES; index++) {
		long_file_name(path, sizeof(path), index);
		size =
		    index == 0 ? (DIRECT_BLOCKS + 1U) * block_size + 17U : block_size + 17U + index;
		verify_uniform_file(path, size, CREATED_MODE, (uint8_t)('a' + index));
	}
	verify_uniform_file("/mnt/reused", block_size + 37U, 0640, 'R');
	verify_uniform_file("/mnt/orphan-truncate", block_size + 13U, 0640, 'O');
	verify_specials("renamed");
	puts("LINUX_FAST_COMMIT_INDIRECT_PASS");
}

static void
verify_large_prefix(unsigned int block_size)
{
	struct stat metadata;
	struct stat alias;
	uint8_t bytes[4096];
	uint8_t expected;
	char path[512];
	unsigned int files = block_size == 1024 ? LARGE_PREFIX_FILES_1K : LARGE_PREFIX_FILES_4K;
	unsigned int index;
	unsigned int logical;
	size_t position;
	int fd;

	require(access("/mnt/hello.txt", F_OK) < 0 && errno == ENOENT,
	    "verify old large-prefix name is absent");
	require(stat("/mnt/new-dir", &metadata) == 0 && S_ISDIR(metadata.st_mode) &&
		metadata.st_nlink == 2,
	    "verify large-prefix directory");
	fd = open("/mnt/renamed", O_RDONLY | O_CLOEXEC);
	require(fd >= 0 && fstat(fd, &metadata) == 0 && metadata.st_mode == (S_IFREG | 0640) &&
		metadata.st_size == (off_t)PROTOCOL_SOURCE_BLOCKS * block_size &&
		metadata.st_nlink == 2 && stat("/mnt/alias", &alias) == 0 &&
		metadata.st_ino == alias.st_ino,
	    "verify large-prefix sparse inode and hardlink");
	for (logical = 0; logical < PROTOCOL_SOURCE_BLOCKS; logical++) {
		expected = logical == 1 ? 0 : 'A';
		require(read(fd, bytes, block_size) == (ssize_t)block_size,
		    "read large-prefix sparse file");
		for (position = 0; position < block_size; position++) {
			require(bytes[position] == expected, "verify large-prefix sparse data");
		}
	}
	require(read(fd, bytes, 1) == 0 && close(fd) == 0, "verify large-prefix EOF and close");
	for (index = 0; index < files; index++) {
		long_file_name(path, sizeof(path), index);
		verify_uniform_file(
		    path, block_size + 17U + index, CREATED_MODE, (uint8_t)('a' + index % 26U));
	}
	printf("LINUX_FAST_COMMIT_LARGE_PREFIX_FILES=%u\n", files);
	puts("LINUX_FAST_COMMIT_LARGE_PREFIX_PASS");
}

static void
verify_xattr_reuse(unsigned int block_size)
{
	const char *replacements[] = { "/mnt/reused", "/mnt/reused-shared" };
	const char *removed[] = { "/mnt/victim", "/mnt/victim-shared", "/mnt/final-delete",
		"/mnt/orphan-held", "/mnt/legacy" };
	const char survivor_data[] = "surviving attribute owner";
	struct stat metadata;
	uint8_t *bytes;
	char key[32];
	size_t size;
	size_t position;
	unsigned int index;
	int fd;

	bytes = malloc(ATTRIBUTE_VALUE_MAX);
	require(bytes != NULL, "allocate native attribute comparison");
	for (index = 0; index < sizeof(removed) / sizeof(removed[0]); index++) {
		require(access(removed[index], F_OK) < 0 && errno == ENOENT,
		    "verify old generation and orphan names are absent");
	}
	for (index = 0; index < sizeof(replacements) / sizeof(replacements[0]); index++) {
		fd = open(replacements[index], O_RDONLY | O_CLOEXEC);
		size = block_size + 37U;
		require(fd >= 0 && fstat(fd, &metadata) == 0 &&
			metadata.st_mode == (S_IFREG | 0640) && metadata.st_nlink == 1 &&
			metadata.st_size == (off_t)size && flistxattr(fd, NULL, 0) == 0 &&
			read(fd, bytes, size + 1U) == (ssize_t)size,
		    "verify reused inode metadata and absence of old attributes");
		for (position = 0; position < size; position++) {
			require(bytes[position] == 'R', "verify reused inode data");
		}
		require(close(fd) == 0, "close reused inode");
	}
	fd = open("/mnt/keep-xattrs", O_RDONLY | O_CLOEXEC);
	require(fd >= 0 && fstat(fd, &metadata) == 0 && metadata.st_mode == (S_IFREG | 0640) &&
		metadata.st_nlink == 1 && metadata.st_size == (off_t)sizeof(survivor_data) - 1 &&
		read(fd, bytes, sizeof(survivor_data)) == (ssize_t)sizeof(survivor_data) - 1 &&
		memcmp(bytes, survivor_data, sizeof(survivor_data) - 1U) == 0,
	    "verify surviving attribute owner");
	require(flistxattr(fd, NULL, 0) == (ssize_t)(ATTRIBUTE_SURVIVORS * sizeof("user.value0")),
	    "verify exact surviving attribute name-list size");
	for (index = 0; index < ATTRIBUTE_COUNT; index++) {
		require(snprintf(key, sizeof(key), "user.value%u", index) > 0,
		    "form attribute fixture key");
		if (index == 1 || index == 2) {
			require(
			    fgetxattr(fd, key, bytes, ATTRIBUTE_VALUE_MAX) < 0 && errno == ENODATA,
			    "verify private values were not transferred to the survivor");
			continue;
		}
		size = index == 0 ? ATTRIBUTE_VALUE_MAX : 3U * block_size + 7U;
		require(fgetxattr(fd, key, bytes, ATTRIBUTE_VALUE_MAX) == (ssize_t)size,
		    "read surviving shared value through Linux");
		for (position = 0; position < size; position++) {
			require(bytes[position] == (uint8_t)(position * 17U + index * 31U),
			    "verify every surviving attribute byte");
		}
	}
	require(close(fd) == 0, "close surviving attribute owner");
	free(bytes);
	puts("LINUX_FAST_COMMIT_XATTR_REUSE_PASS");
}

int
main(void)
{
	const char *modules[] = { "virtio_blk", "crc32c_generic", "crc16", "mbcache", "jbd2",
		"ext4" };
	/* The filesystem feature enables fast commits; Linux has no fast_commit
	 * mount parameter. The delayed full-commit timer preserves capture inputs. */
	const char *options = "data=ordered,commit=600";
	struct utsname identity;
	struct statfs geometry;
	FILE *config;
	char path[128];
	unsigned int phase = 0;
	unsigned int modern_orphans = 0;
	unsigned int special_files = 0;
	unsigned int index;
	int fd;
	int result;

	setvbuf(stdout, NULL, _IOLBF, 0);
	umask(0);
	require(uname(&identity) == 0, "uname");
	printf("LINUX_FAST_COMMIT_KERNEL=%s %s %s\n", identity.sysname, identity.release,
	    identity.machine);
	require(mount("devtmpfs", "/dev", "devtmpfs", 0, NULL) == 0, "mount devtmpfs");
	require(mount("proc", "/proc", "proc", 0, NULL) == 0, "mount guest procfs");
	for (index = 0; index < sizeof(modules) / sizeof(modules[0]); index++) {
		snprintf(path, sizeof(path), "/modules/%s.ko", modules[index]);
		fd = open(path, O_RDONLY | O_CLOEXEC);
		require(fd >= 0, "open matching module");
		result = (int)syscall(SYS_finit_module, fd, "", 0);
		require(result == 0 || errno == EEXIST, path);
		require(close(fd) == 0, "close module");
	}
	config = fopen("/phase", "r");
	require(config != NULL && fscanf(config, "%u", &phase) == 1, "read fixture phase");
	require(
	    fclose(config) == 0 && phase <= PHASE_LARGE_PREFIX_READBACK, "validate fixture phase");
	config = fopen("/modern-orphans", "r");
	require(config != NULL && fscanf(config, "%u", &modern_orphans) == 1,
	    "read orphan fixture mode");
	require(fclose(config) == 0 && modern_orphans <= 1, "validate orphan fixture mode");
	config = fopen("/special-files", "r");
	require(config != NULL && fscanf(config, "%u", &special_files) == 1,
	    "read special-file fixture mode");
	require(fclose(config) == 0 && special_files <= 1, "validate special-file fixture mode");
	printf("LINUX_FAST_COMMIT_MOUNT_OPTIONS=%s\n", options);
	require(mount("/dev/vda", "/mnt", "ext4", MS_NOATIME | MS_NOSUID | MS_NODEV, options) == 0,
	    "mount fast-commit filesystem");
	require(statfs("/mnt", &geometry) == 0 &&
		(geometry.f_bsize == 1024 || geometry.f_bsize == 4096),
	    "require fixture block size");
	if (phase == 0) {
		mutate((unsigned int)geometry.f_bsize, modern_orphans, special_files);
	}
	if (phase == PHASE_PROTOCOL_READBACK) {
		verify_xattr_reuse((unsigned int)geometry.f_bsize);
	} else if (phase == PHASE_INDIRECT_READBACK) {
		verify_indirect((unsigned int)geometry.f_bsize);
	} else if (phase == PHASE_LARGE_PREFIX_READBACK) {
		verify_large_prefix((unsigned int)geometry.f_bsize);
	} else {
		verify((unsigned int)geometry.f_bsize);
		if (special_files) {
			verify_specials("hello.txt");
		}
	}
	require(umount("/mnt") == 0, "cleanly unmount recovered filesystem");
	puts(phase >= PHASE_PROTOCOL_READBACK ? "LINUX_FAST_COMMIT_FIXTURE_READBACK_PASS"
					      : "LINUX_FAST_COMMIT_REPLAY_PASS");
	power_off(1);
	return 0;
}

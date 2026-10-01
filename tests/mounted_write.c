/* SPDX-License-Identifier: BSD-3-Clause */
#define _DARWIN_C_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/attr.h>
#include <sys/mman.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/xattr.h>
#include <unistd.h>

#define DATA_SIZE 131073U
#define WORKER_COUNT 4U
#define WORKER_WRITES 8U
#define WORKER_STRIDE 4096U

struct writer {
	int fd;
	unsigned index;
	int error;
};

struct volume_name_change {
	attrreference_t reference;
	char name[64];
};

struct volume_name_result {
	uint32_t length;
	attrreference_t reference;
	char name[256];
};

#define CHECK(condition)                                                                           \
	do {                                                                                       \
		if (!(condition)) {                                                                \
			fprintf(stderr, "%s:%d: %s (errno=%d: %s)\n", __FILE__, __LINE__,          \
			    #condition, errno, strerror(errno));                                   \
			exit(EXIT_FAILURE);                                                        \
		}                                                                                  \
	} while (0)

static uint8_t
expected_byte(size_t position)
{
	if (position < WORKER_COUNT * WORKER_WRITES * WORKER_STRIDE) {
		return (uint8_t)(position / (WORKER_WRITES * WORKER_STRIDE) + 1);
	}
	return (uint8_t)(position * 17 + 3);
}

static void *
write_worker(void *context)
{
	struct writer *worker = context;
	uint8_t buffer[WORKER_STRIDE];
	unsigned iteration;
	off_t offset;

	memset(buffer, (int)worker->index + 1, sizeof(buffer));
	for (iteration = 0; iteration < WORKER_WRITES; iteration++) {
		offset = (off_t)((worker->index * WORKER_WRITES + iteration) * WORKER_STRIDE);
		if (pwrite(worker->fd, buffer, sizeof(buffer), offset) != sizeof(buffer)) {
			worker->error = errno != 0 ? errno : EIO;
			break;
		}
	}
	return NULL;
}

static void
check_persisted(int directory)
{
	uint8_t *data = malloc(DATA_SIZE);
	char value[32];
	char link[32];
	struct stat status;
	struct stat alias;
	size_t index;
	int fd;

	CHECK(data != NULL);
	fd = openat(directory, "payload", O_RDONLY | O_NOFOLLOW);
	CHECK(fd >= 0);
	CHECK(fstat(fd, &status) == 0 && status.st_size == DATA_SIZE && status.st_nlink == 2);
	CHECK((status.st_mode & 0777) == 0640);
	CHECK(pread(fd, data, DATA_SIZE, 0) == DATA_SIZE);
	for (index = 0; index < DATA_SIZE; index++) {
		CHECK(data[index] == expected_byte(index));
	}
	CHECK(fgetxattr(fd, "org.machlin.acceptance", value, sizeof(value), 0, 0) == 9);
	CHECK(memcmp(value, "persisted", 9) == 0);
	CHECK(fstatat(directory, "hardlink", &alias, 0) == 0 && alias.st_ino == status.st_ino);
	CHECK(readlinkat(directory, "symlink", link, sizeof(link)) == 7);
	CHECK(memcmp(link, "payload", 7) == 0);
	CHECK(close(fd) == 0);
	free(data);
}

static void
check_special_files(int directory)
{
	struct sockaddr_un address = { .sun_len = sizeof(address), .sun_family = AF_UNIX };
	char bytes[8];
	int reader;
	int writer;
	int bound;
	int client;
	int previous;
	int fifo_supported = 0;
	int socket_supported = 0;

	if (mkfifoat(directory, "fifo", 0600) == 0) {
		fifo_supported = 1;
		reader = openat(directory, "fifo", O_RDONLY | O_NONBLOCK | O_NOFOLLOW);
		CHECK(reader >= 0);
		writer = openat(directory, "fifo", O_WRONLY | O_NONBLOCK | O_NOFOLLOW);
		CHECK(writer >= 0);
		CHECK(write(writer, "pipe", 4) == 4);
		CHECK(read(reader, bytes, sizeof(bytes)) == 4 && memcmp(bytes, "pipe", 4) == 0);
		CHECK(close(reader) == 0 && close(writer) == 0);
		CHECK(unlinkat(directory, "fifo", 0) == 0);
	} else {
		CHECK(errno == ENOTSUP);
	}
	previous = open(".", O_RDONLY | O_DIRECTORY);
	CHECK(previous >= 0 && fchdir(directory) == 0);
	strlcpy(address.sun_path, "socket", sizeof(address.sun_path));
	bound = socket(AF_UNIX, SOCK_DGRAM, 0);
	client = socket(AF_UNIX, SOCK_DGRAM, 0);
	CHECK(bound >= 0 && client >= 0);
	if (bind(bound, (const struct sockaddr *)&address, sizeof(address)) == 0) {
		socket_supported = 1;
		CHECK(connect(client, (const struct sockaddr *)&address, sizeof(address)) == 0);
		CHECK(send(client, "socket", 6, 0) == 6);
		CHECK(recv(bound, bytes, sizeof(bytes), MSG_DONTWAIT) == 6 &&
		    memcmp(bytes, "socket", 6) == 0);
		CHECK(unlinkat(directory, "socket", 0) == 0);
	} else {
		CHECK(errno == ENOTSUP);
	}
	CHECK(close(bound) == 0 && close(client) == 0);
	CHECK(fchdir(previous) == 0 && close(previous) == 0);
	/* Unsupported special creation is reported separately, never as a passed feature. */
	printf("{\"fifo\":\"%s\",\"socket\":\"%s\"}\n", fifo_supported ? "passed" : "unsupported",
	    socket_supported ? "passed" : "unsupported");
}

static void
check_write(int directory)
{
	uint8_t *data = malloc(DATA_SIZE);
	uint8_t zeros[4096];
	uint8_t *mapping;
	struct stat status;
	struct writer workers[WORKER_COUNT];
	pthread_t threads[WORKER_COUNT];
	size_t index;
	int fd;
	int removed;

	CHECK(data != NULL);
	for (index = 0; index < DATA_SIZE; index++) {
		data[index] = (uint8_t)(index * 17 + 3);
	}
	fd = openat(directory, "temporary", O_CREAT | O_EXCL | O_RDWR, 0600);
	CHECK(fd >= 0);
	CHECK(write(fd, data, DATA_SIZE) == DATA_SIZE);
	CHECK(fsync(fd) == 0);
	CHECK(ftruncate(fd, 1025) == 0);
	CHECK(ftruncate(fd, DATA_SIZE) == 0);
	memset(zeros, 0xa5, sizeof(zeros));
	CHECK(pread(fd, zeros, sizeof(zeros), 1025) == sizeof(zeros));
	for (index = 0; index < sizeof(zeros); index++) {
		CHECK(zeros[index] == 0);
	}
	CHECK(pwrite(fd, data, DATA_SIZE, 0) == DATA_SIZE);
	for (index = 0; index < WORKER_COUNT; index++) {
		workers[index] = (struct writer){ .fd = fd, .index = (unsigned)index };
		CHECK(pthread_create(&threads[index], NULL, write_worker, &workers[index]) == 0);
	}
	for (index = 0; index < WORKER_COUNT; index++) {
		CHECK(pthread_join(threads[index], NULL) == 0 && workers[index].error == 0);
	}
	mapping = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	CHECK(mapping != MAP_FAILED);
	mapping[0] = 99;
	CHECK(msync(mapping, 4096, MS_SYNC) == 0);
	CHECK(pread(fd, zeros, 1, 0) == 1 && zeros[0] == 99);
	mapping[0] = 1;
	CHECK(msync(mapping, 4096, MS_SYNC) == 0);
	CHECK(munmap(mapping, 4096) == 0);
	CHECK(fchmod(fd, 0640) == 0);
	CHECK(fsetxattr(fd, "org.machlin.acceptance", "persisted", 9, 0, XATTR_CREATE) == 0);
	errno = 0;
	CHECK(fsetxattr(fd, "org.machlin.acceptance", "again", 5, 0, XATTR_CREATE) == -1 &&
	    errno == EEXIST);
	CHECK(fsetxattr(fd, "org.machlin.transient", "x", 1, 0, 0) == 0);
	CHECK(fremovexattr(fd, "org.machlin.transient", 0) == 0);
	CHECK(fsync(fd) == 0);
	CHECK(close(fd) == 0);
	CHECK(renameat(directory, "temporary", directory, "payload") == 0);
	CHECK(linkat(directory, "payload", directory, "hardlink", 0) == 0);
	CHECK(symlinkat("payload", directory, "symlink") == 0);
	removed = openat(directory, "removed", O_CREAT | O_EXCL | O_RDWR, 0600);
	CHECK(removed >= 0);
	CHECK(write(removed, "old", 3) == 3);
	CHECK(unlinkat(directory, "removed", 0) == 0);
	CHECK(fstat(removed, &status) == 0 && status.st_nlink == 0);
	CHECK(pwrite(removed, "new bytes", 9, 0) == 9);
	CHECK(pread(removed, zeros, 9, 0) == 9 && memcmp(zeros, "new bytes", 9) == 0);
	CHECK(fsync(removed) == 0);
	CHECK(close(removed) == 0);
	CHECK(mkdirat(directory, "empty", 0700) == 0);
	CHECK(unlinkat(directory, "empty", AT_REMOVEDIR) == 0);
	CHECK(fsync(directory) == 0);
	free(data);
}

static void
check_permissions(int root, int directory)
{
	struct statfs filesystem;
	struct stat status;
	int fd;

	CHECK(geteuid() != 0);
	CHECK(fstatfs(root, &filesystem) == 0 && (filesystem.f_flags & MNT_IGNORE_OWNERSHIP) == 0);
	CHECK(fstatat(root, "foreign-owned", &status, AT_SYMLINK_NOFOLLOW) == 0);
	CHECK(status.st_uid != geteuid() && (status.st_mode & 0777) == 0600);
	errno = 0;
	CHECK(openat(root, "foreign-owned", O_RDONLY | O_NOFOLLOW) == -1 && errno == EACCES);
	errno = 0;
	CHECK(openat(root, "foreign-owned", O_WRONLY | O_NOFOLLOW) == -1 && errno == EACCES);
	errno = 0;
	CHECK(fchmodat(root, "foreign-owned", 0666, 0) == -1 && errno == EPERM);
	CHECK(fstatat(root, "sticky", &status, AT_SYMLINK_NOFOLLOW) == 0);
	CHECK(status.st_uid != geteuid() && (status.st_mode & 07777) == 01777);
	CHECK(fstatat(root, "sticky/foreign-owned", &status, AT_SYMLINK_NOFOLLOW) == 0);
	CHECK(status.st_uid != geteuid());
	errno = 0;
	/* POSIX permits either errno for a sticky-directory ownership denial. */
	CHECK(
	    unlinkat(root, "sticky/foreign-owned", 0) == -1 && (errno == EPERM || errno == EACCES));
	CHECK(fstatat(root, "sticky/foreign-owned", &status, AT_SYMLINK_NOFOLLOW) == 0);
	fd = openat(directory, "payload", O_RDWR | O_NOFOLLOW);
	CHECK(fd >= 0);
	CHECK(fstat(fd, &status) == 0 && status.st_uid == geteuid());
	errno = 0;
	CHECK(fchown(fd, geteuid() + 1, (gid_t)-1) == -1 && errno == EPERM);
	errno = 0;
	CHECK(fchflags(fd, SF_IMMUTABLE) == -1 && errno == EPERM);
	CHECK(fchflags(fd, UF_NODUMP) == 0);
	CHECK(fstat(fd, &status) == 0 && (status.st_flags & UF_NODUMP) != 0);
	CHECK(fchflags(fd, 0) == 0);
	CHECK(fchmod(fd, 0000) == 0);
	errno = 0;
	CHECK(openat(directory, "payload", O_RDONLY | O_NOFOLLOW) == -1 && errno == EACCES);
	CHECK(fchmod(fd, 0640) == 0 && fsync(fd) == 0 && close(fd) == 0);
	puts("PASS native ownership, mode denial, sticky directories, chown/system-flag denial, "
	     "and nodump");
}

static void
check_set_id(int directory)
{
	struct statfs filesystem;
	struct stat status;
	mode_t live_mode;
	mode_t reopened_mode;
	uint8_t byte = 1;
	int fd;

	CHECK(fstatfs(directory, &filesystem) == 0);
	fd = openat(directory, "payload", O_RDWR | O_NOFOLLOW);
	CHECK(fd >= 0 && fchmod(fd, 06740) == 0);
	CHECK(pwrite(fd, &byte, 1, 0) == 1 && fsync(fd) == 0);
	errno = 0;
	CHECK(fstat(fd, &status) == 0);
	live_mode = status.st_mode & 07777;
	CHECK(close(fd) == 0);
	fd = openat(directory, "payload", O_RDWR | O_NOFOLLOW);
	CHECK(fd >= 0 && fstat(fd, &status) == 0);
	reopened_mode = status.st_mode & 07777;
	/* Restore the ordinary I/O fixture before recording an independent failure. */
	CHECK(fchmod(fd, 0640) == 0 && fsync(fd) == 0 && close(fd) == 0);
	printf("{\"live_mode\":\"%04o\",\"reopened_mode\":\"%04o\",\"nosuid\":%s}\n", live_mode,
	    reopened_mode, (filesystem.f_flags & MNT_NOSUID) != 0 ? "true" : "false");
	CHECK((live_mode & (S_ISUID | S_ISGID)) == 0);
	CHECK((reopened_mode & (S_ISUID | S_ISGID)) == 0);
}

static void
check_space(int directory)
{
	const size_t chunk = 256U * 1024U;
	struct statfs filesystem;
	struct stat status;
	fstore_t reservation = { .fst_posmode = F_PEOFPOSMODE, .fst_length = 65536 };
	uint8_t *buffer = malloc(chunk);
	uint64_t limit;
	off_t total = 0;
	off_t offset;
	ssize_t amount;
	size_t index;
	int fd;
	int full = 0;

	CHECK(buffer != NULL && fstatfs(directory, &filesystem) == 0);
	limit = filesystem.f_blocks * filesystem.f_bsize;
	CHECK(limit > 0 && limit <= 256U * 1024U * 1024U);
	fd = openat(directory, "space-pressure", O_CREAT | O_EXCL | O_RDWR, 0600);
	CHECK(fd >= 0);
	CHECK(fcntl(fd, F_PREALLOCATE, &reservation) == 0 && reservation.fst_bytesalloc >= 65536);
	CHECK(fstat(fd, &status) == 0 && status.st_size == 0 && status.st_blocks * 512 >= 65536);
	CHECK(ftruncate(fd, 16384) == 0);
	CHECK(pread(fd, buffer, 16384, 0) == 16384);
	for (index = 0; index < 16384; index++) {
		CHECK(buffer[index] == 0);
	}
	CHECK(ftruncate(fd, 0) == 0);
	CHECK(fcntl(fd, F_NOCACHE, 1) == 0);
	fprintf(stderr, "pressure: preallocation and zero exposure passed; filling %llu bytes\n",
	    (unsigned long long)limit);
	memset(buffer, 0x6d, chunk);
	while ((uint64_t)total <= limit) {
		amount = pwrite(fd, buffer, chunk, total);
		if (amount < 0) {
			CHECK(errno == ENOSPC);
			full = 1;
			break;
		}
		CHECK(amount > 0 && (size_t)amount <= chunk);
		total += amount;
		if (total % (1024U * 1024U) == 0 || (size_t)amount != chunk) {
			fprintf(stderr, "pressure: committed %lld bytes\n", (long long)total);
		}
	}
	fprintf(
	    stderr, "pressure: ENOSPC after %lld bytes, syncing and verifying\n", (long long)total);
	CHECK(full && total > 0 && fsync(fd) == 0);
	CHECK(fstat(fd, &status) == 0 && status.st_size == total);
	for (offset = 0; offset < total; offset += amount) {
		amount = pread(fd, buffer,
		    total - offset < (off_t)chunk ? (size_t)(total - offset) : chunk, offset);
		CHECK(amount > 0);
		for (index = 0; index < (size_t)amount; index++) {
			CHECK(buffer[index] == 0x6d);
		}
	}
	CHECK(ftruncate(fd, 0) == 0);
	CHECK(pwrite(fd, "space recovered", 15, 0) == 15 && fsync(fd) == 0);
	CHECK(close(fd) == 0 && unlinkat(directory, "space-pressure", 0) == 0);
	CHECK(fsync(directory) == 0);
	free(buffer);
	puts("PASS native preallocation, zero exposure, ENOSPC prefix readback and space reuse");
}

static int
set_volume_name(int root, const char *name)
{
	struct attrlist request = { .bitmapcount = ATTR_BIT_MAP_COUNT,
		.volattr = ATTR_VOL_INFO | ATTR_VOL_NAME };
	struct volume_name_change change = { 0 };
	size_t length = strlen(name) + 1;

	CHECK(length <= sizeof(change.name));
	change.reference.attr_dataoffset = offsetof(struct volume_name_change, name);
	change.reference.attr_length = (uint32_t)length;
	memcpy(change.name, name, length);
	/* Darwin's volume-name unpacker requires space beyond the referenced bytes. */
	return fsetattrlist(root, &request, &change, sizeof(change), 0);
}

static void
check_volume_name(int root, const char *expected)
{
	struct attrlist request = { .bitmapcount = ATTR_BIT_MAP_COUNT,
		.volattr = ATTR_VOL_INFO | ATTR_VOL_NAME };
	struct volume_name_result result = { 0 };
	size_t offset;

	CHECK(fgetattrlist(root, &request, &result, sizeof(result), 0) == 0);
	CHECK(result.length <= sizeof(result) && result.reference.attr_dataoffset >= 0);
	offset = offsetof(struct volume_name_result, reference) + result.reference.attr_dataoffset;
	CHECK(offset <= result.length && result.reference.attr_length <= result.length - offset);
	CHECK(result.reference.attr_length == strlen(expected) + 1);
	CHECK(memcmp((const char *)&result + offset, expected, result.reference.attr_length) == 0);
}

static void
check_volume_rename(int root, int readonly)
{
	const char *name = "Machlin writable"; /* Exactly the ext4 label's 16-byte limit. */

	if (readonly) {
		check_volume_name(root, name);
		CHECK(set_volume_name(root, "denied") == -1 && errno == EROFS);
	} else {
		CHECK(set_volume_name(root, "ext4") == 0);
		check_volume_name(root, "ext4");
		CHECK(set_volume_name(root, name) == 0);
		check_volume_name(root, name);
		CHECK(set_volume_name(root, "12345678901234567") == -1 &&
		    (errno == EINVAL || errno == ENAMETOOLONG));
		check_volume_name(root, name);
	}
	puts(readonly ? "PASS native volume label persistence and read-only denial"
		      : "PASS native volume rename, exact label limit and oversized rejection");
}

static void
check_seek_regions(int directory)
{
	/* Page-aligned regions keep the expected extents independent of writeback
	 * page size on either filesystem block size. The final block has a short EOF. */
	const size_t unit = 64U * 1024U;
	uint8_t *data = malloc(unit);
	int fd = openat(directory, "seek-regions", O_CREAT | O_EXCL | O_RDWR | O_NOFOLLOW, 0600);

	CHECK(data != NULL && fd >= 0);
	memset(data, 0x5a, unit);
	CHECK(ftruncate(fd, 6U * unit + 17U) == 0);
	CHECK(pwrite(fd, data, unit, unit) == (ssize_t)unit);
	CHECK(pwrite(fd, data, unit, 3U * unit) == (ssize_t)unit);
	CHECK(pwrite(fd, data, 17, 6U * unit) == 17);
	CHECK(fsync(fd) == 0);
	CHECK(lseek(fd, 0, SEEK_HOLE) == 0);
	CHECK(lseek(fd, 1, SEEK_DATA) == (off_t)unit);
	CHECK(lseek(fd, unit + 17U, SEEK_DATA) == (off_t)unit + 17);
	CHECK(lseek(fd, unit + 17U, SEEK_HOLE) == (off_t)(2U * unit));
	CHECK(lseek(fd, 2U * unit, SEEK_DATA) == (off_t)(3U * unit));
	CHECK(lseek(fd, 3U * unit, SEEK_HOLE) == (off_t)(4U * unit));
	CHECK(lseek(fd, 4U * unit, SEEK_DATA) == (off_t)(6U * unit));
	CHECK(lseek(fd, 6U * unit, SEEK_HOLE) == (off_t)(6U * unit + 17U));
	CHECK(lseek(fd, 6U * unit + 17U, SEEK_DATA) == -1 && errno == ENXIO);
	CHECK(lseek(fd, 6U * unit + 17U, SEEK_HOLE) == -1 && errno == ENXIO);
	CHECK(lseek(fd, -1, SEEK_DATA) == -1 && errno == EINVAL);
	/* A seek must also see a buffered write that has not had an explicit fsync. */
	CHECK(pwrite(fd, data, unit, 4U * unit) == (ssize_t)unit);
	CHECK(lseek(fd, 4U * unit, SEEK_DATA) == (off_t)(4U * unit));
	CHECK(ftruncate(fd, unit) == 0);
	CHECK(lseek(fd, 0, SEEK_DATA) == -1 && errno == ENXIO);
	CHECK(lseek(fd, 0, SEEK_HOLE) == 0);
	CHECK(close(fd) == 0 && unlinkat(directory, "seek-regions", 0) == 0);
	free(data);
	puts("PASS native sparse seek, partial EOF, buffered writes and truncate invalidation");
}

int
main(int argc, char **argv)
{
	struct statfs filesystem;
	int root;
	int directory;
	int verify;

	CHECK(argc == 3 &&
	    (strcmp(argv[2], "write") == 0 || strcmp(argv[2], "verify") == 0 ||
		strcmp(argv[2], "special") == 0 || strcmp(argv[2], "policy") == 0 ||
		strcmp(argv[2], "setid") == 0 || strcmp(argv[2], "pressure") == 0 ||
		strcmp(argv[2], "seek") == 0 || strcmp(argv[2], "rename") == 0 ||
		strcmp(argv[2], "rename-verify") == 0));
	verify = strcmp(argv[2], "verify") == 0;
	root = open(argv[1], O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
	CHECK(root >= 0);
	CHECK(fstatfs(root, &filesystem) == 0);
	CHECK(strcmp(filesystem.f_fstypename, "machlinext4") == 0);
	if (strcmp(argv[2], "rename") == 0 || strcmp(argv[2], "rename-verify") == 0) {
		check_volume_rename(root, (filesystem.f_flags & MNT_RDONLY) != 0);
		CHECK(close(root) == 0);
		return EXIT_SUCCESS;
	}
	if (strcmp(argv[2], "special") == 0 || strcmp(argv[2], "policy") == 0 ||
	    strcmp(argv[2], "setid") == 0 || strcmp(argv[2], "pressure") == 0 ||
	    strcmp(argv[2], "seek") == 0) {
		CHECK((filesystem.f_flags & MNT_RDONLY) == 0);
		directory = openat(root, "acceptance-write", O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
		CHECK(directory >= 0);
		if (strcmp(argv[2], "special") == 0) {
			check_special_files(directory);
		} else if (strcmp(argv[2], "policy") == 0) {
			check_permissions(root, directory);
		} else if (strcmp(argv[2], "setid") == 0) {
			check_set_id(directory);
		} else if (strcmp(argv[2], "seek") == 0) {
			check_seek_regions(directory);
		} else {
			check_space(directory);
		}
		CHECK(close(directory) == 0 && close(root) == 0);
		return EXIT_SUCCESS;
	}
	if (!verify) {
		CHECK((filesystem.f_flags & MNT_RDONLY) == 0);
		CHECK(mkdirat(root, "acceptance-write", 0700) == 0);
	}
	directory = openat(root, "acceptance-write", O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
	CHECK(directory >= 0);
	if (!verify) {
		check_write(directory);
	}
	check_persisted(directory);
	CHECK(close(directory) == 0 && close(root) == 0);
	puts(verify ? "PASS native write persistence after remount"
		    : "PASS native writes, growth, shrink, mmap, concurrent I/O, metadata, xattrs, "
		      "namespace and open-unlinked lifetime");
	return EXIT_SUCCESS;
}

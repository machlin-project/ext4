/* SPDX-License-Identifier: BSD-3-Clause */
#ifndef MACHLIN_EXT4_TEST_LINUX_XATTRS_H
#define MACHLIN_EXT4_TEST_LINUX_XATTRS_H

#include <grp.h>
#include <stdbool.h>
#include <sys/wait.h>
#include <sys/xattr.h>

#define LINUX_XATTR_CASES 128U
#define LINUX_XATTR_VALUE_LIMIT 65536U
#define LINUX_XATTR_DATA_LIMIT 1048576U
#define LINUX_XATTR_FILE "/mnt/linux-xattr-file"
#define LINUX_XATTR_DIRECTORY "/mnt/linux-xattr-directory"
#define LINUX_XATTR_INHERITED "/mnt/directory/linux-inherited"
#define LINUX_XATTR_NAMED_USER 70000U
#define LINUX_XATTR_UNRELATED_GROUP 90000U
#define LINUX_XATTR_PATH_CHARS 4095
#define LINUX_XATTR_STRING_(value) #value
#define LINUX_XATTR_STRING(value) LINUX_XATTR_STRING_(value)
#define LINUX_XATTR_PATH_SCAN "%" LINUX_XATTR_STRING(LINUX_XATTR_PATH_CHARS) "s"

struct linux_xattr_value {
	char path[LINUX_XATTR_PATH_CHARS + 1];
	char name[256];
	uint8_t *value;
	size_t size;
	bool denied;
};

static uint8_t *
xattr_load(const char *path, size_t *size)
{
	FILE *input;
	uint8_t *value;
	long length;

	input = fopen(path, "rb");
	require(input != NULL && fseek(input, 0, SEEK_END) == 0, "open expected attribute/data");
	length = ftell(input);
	require(length >= 0 && length <= LINUX_XATTR_DATA_LIMIT && fseek(input, 0, SEEK_SET) == 0,
	    "bound expected attribute/data");
	*size = (size_t)length;
	value = malloc(*size + 1);
	require(value != NULL && fread(value, 1, *size, input) == *size,
	    "load expected attribute/data");
	require(fclose(input) == 0, "close expected attribute/data");
	return value;
}

static void
xattr_check_value(const char *path, const struct linux_xattr_value *item)
{
	uint8_t *value;
	ssize_t size;

	errno = 0;
	size = lgetxattr(path, item->name, NULL, 0);
	if (item->denied) {
		/* Linux keeps raw user.* storage on a symlink but hides value access. */
		require(size == -1 && errno == ENODATA, "Linux user attribute symlink policy");
		return;
	}
	require(size >= 0 && (size_t)size == item->size, "Linux attribute length query");
	value = malloc(item->size + 1);
	require(value != NULL, "allocate Linux attribute comparison");
	require(lgetxattr(path, item->name, value, item->size) == size &&
		memcmp(value, item->value, item->size) == 0,
	    "Linux exact attribute bytes");
	if (item->size > 1) {
		errno = 0;
		require(lgetxattr(path, item->name, value, item->size - 1) == -1 && errno == ERANGE,
		    "Linux short attribute buffer");
	}
	free(value);
}

static void
xattr_check_list(const char *path, const char *relative, size_t expected,
    const struct linux_xattr_value *values, size_t count)
{
	char *names;
	bool *seen;
	ssize_t size;
	size_t offset = 0;
	size_t length;
	size_t index;
	size_t matched;
	size_t found = 0;

	size = llistxattr(path, NULL, 0);
	require(size >= 0 && size <= LINUX_XATTR_VALUE_LIMIT, "Linux attribute list length");
	names = malloc((size_t)size + 1);
	seen = calloc(count + 1, sizeof(*seen));
	require(names != NULL && seen != NULL, "allocate attribute list");
	require(llistxattr(path, names, (size_t)size) == size, "Linux complete attribute list");
	while (offset < (size_t)size) {
		length = strnlen(names + offset, (size_t)size - offset);
		require(length != 0 && length < (size_t)size - offset, "bounded Linux list entry");
		matched = 0;
		for (index = 0; index < count; index++) {
			if (strcmp(relative, values[index].path) == 0 &&
			    strcmp(names + offset, values[index].name) == 0) {
				require(!seen[index], "no duplicated Linux attribute names");
				seen[index] = true;
				matched++;
			}
		}
		require(matched == 1, "Linux attribute name belongs to the complete expected set");
		found++;
		offset += length + 1;
	}
	require(found == expected, "Linux attribute list count");
	free(seen);
	free(names);
}

static void
xattr_check_all(void)
{
	struct linux_xattr_value *values;
	struct linux_xattr_value *item;
	struct stat metadata;
	FILE *input;
	char relative[LINUX_XATTR_PATH_CHARS + 1];
	char path[LINUX_XATTR_PATH_CHARS + sizeof("/mnt")];
	char source[128];
	char hex[511];
	uint8_t *expected;
	uint8_t *actual;
	unsigned long long number;
	unsigned long long file_size;
	unsigned int mode;
	unsigned int uid;
	unsigned int gid;
	unsigned int byte;
	size_t count = 0;
	size_t index;
	size_t character;
	size_t attributes;
	size_t size;
	int fields;
	int flag;
	int fd;

	values = calloc(LINUX_XATTR_CASES, sizeof(*values));
	require(values != NULL, "allocate independent Linux attribute expectations");
	input = fopen("/xattr-values", "r");
	require(input != NULL, "open attribute value manifest");
	for (;;) {
		require(count < LINUX_XATTR_CASES, "bound Linux attribute cases");
		item = &values[count];
		fields = fscanf(
		    input, LINUX_XATTR_PATH_SCAN " %510s %127s %d", item->path, hex, source, &flag);
		if (fields == EOF) {
			break;
		}
		require(fields == 4 && strlen(hex) % 2 == 0, "parse Linux attribute case");
		for (character = 0; character < strlen(hex) / 2; character++) {
			require(sscanf(hex + character * 2, "%2x", &byte) == 1 && byte != 0,
			    "decode Linux attribute name");
			item->name[character] = (char)byte;
		}
		item->value = xattr_load(source, &item->size);
		require(item->size <= LINUX_XATTR_VALUE_LIMIT, "bound Linux attribute value");
		item->denied = flag != 0;
		snprintf(path, sizeof(path), "/mnt%s", item->path);
		xattr_check_value(path, item);
		count++;
	}
	require(fclose(input) == 0, "close attribute value manifest");
	input = fopen("/xattr-inodes", "r");
	require(input != NULL, "open attributed inode manifest");
	while ((fields = fscanf(input, LINUX_XATTR_PATH_SCAN " %llu %o %u %u %llu %zu", relative,
		    &number, &mode, &uid, &gid, &file_size, &attributes)) != EOF) {
		require(fields == 7, "parse attributed inode identity");
		snprintf(path, sizeof(path), "/mnt%s", relative);
		require(lstat(path, &metadata) == 0 && metadata.st_ino == number &&
			metadata.st_mode == mode && metadata.st_uid == uid &&
			metadata.st_gid == gid && metadata.st_size >= 0 &&
			(uint64_t)metadata.st_size == file_size,
		    "Linux attributed inode identity, mode, owners and size");
		xattr_check_list(path, relative, attributes, values, count);
	}
	require(fclose(input) == 0, "close attributed inode manifest");
	input = fopen("/xattr-data", "r");
	require(input != NULL, "open attributed contents manifest");
	while ((fields = fscanf(
		    input, LINUX_XATTR_PATH_SCAN " %127s %d", relative, source, &flag)) != EOF) {
		require(fields == 3, "parse attributed contents case");
		expected = xattr_load(source, &size);
		actual = malloc(size + 1);
		require(actual != NULL, "allocate complete contents comparison");
		snprintf(path, sizeof(path), "/mnt%s", relative);
		if (flag) {
			require(readlink(path, (char *)actual, size + 1) == (ssize_t)size,
			    "Linux exact attributed symlink length");
		} else {
			fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
			require(fd >= 0 && pread(fd, actual, size, 0) == (ssize_t)size,
			    "Linux read complete attributed file");
			require(pread(fd, actual + size, 1, (off_t)size) == 0,
			    "Linux attributed file EOF");
			require(close(fd) == 0, "close attributed file");
		}
		require(memcmp(actual, expected, size) == 0, "Linux exact attributed contents");
		free(actual);
		free(expected);
	}
	require(fclose(input) == 0, "close attributed contents manifest");
	for (index = 0; index < count; index++) {
		free(values[index].value);
	}
	free(values);
}

static void
xattr_sync_path(const char *path)
{
	int fd;

	fd = open(path, O_RDONLY | O_CLOEXEC);
	require(fd >= 0 && fsync(fd) == 0 && close(fd) == 0, "persist Linux attribute owner");
}

static void
xattr_check_acl_access(void)
{
	uint8_t value;
	pid_t child;
	int status;
	int fd;

	child = fork();
	require(child >= 0, "fork ACL visibility check");
	if (child == 0) {
		if (setgroups(0, NULL) != 0 || setgid(LINUX_XATTR_UNRELATED_GROUP) != 0 ||
		    setuid(LINUX_XATTR_NAMED_USER) != 0) {
			_exit(1);
		}
		fd = open(LINUX_XATTR_FILE, O_RDONLY | O_CLOEXEC);
		if (fd < 0 || read(fd, &value, 1) != 1 || close(fd) != 0) {
			_exit(2);
		}
		errno = 0;
		if (open(LINUX_XATTR_FILE, O_WRONLY | O_CLOEXEC) != -1 || errno != EACCES) {
			_exit(3);
		}
		errno = 0;
		if (setxattr(LINUX_XATTR_FILE, "user.denied", &value, 1, 0) != -1 ||
		    errno != EACCES) {
			_exit(4);
		}
		errno = 0;
		if (getxattr(LINUX_XATTR_FILE, "trusted.marker", &value, 1) != -1 ||
		    errno != ENODATA) {
			_exit(5);
		}
		_exit(0);
	}
	require(
	    waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0,
	    "Linux named ACL user, mask and trusted namespace restrictions");
}

#include "linux_ea_inode.h"
#include "linux_inline.h"
#include "linux_cluster.h"
#include "linux_large_file.h"
#include "linux_large_volume.h"

static void
check_xattrs(void)
{
	FILE *configuration;
	uint8_t bytes[650];
	uint8_t *acl;
	uint8_t *capability;
	size_t acl_size;
	size_t capability_size;
	size_t index;
	int verify_only = 0;
	int fd;
	int result;

	if (access("/large-volume-geometry", F_OK) == 0) {
		check_large_volumes();
	}
	if (access("/large-file-geometry", F_OK) == 0) {
		check_large_files();
	}
	xattr_check_all();
	configuration = fopen("/xattr-options", "r");
	require(configuration != NULL && fscanf(configuration, "%d", &verify_only) == 1 &&
		fclose(configuration) == 0,
	    "read attribute roundtrip mode");
	if (verify_only) {
		require(umount("/mnt") == 0, "cleanly unmount returned attributes");
		puts("LINUX_EXT4_XATTR_RETURN_PASS");
		power_off(1);
	}
	if (access("/ea-inode", F_OK) == 0) {
		check_ea_inodes();
	}
	if (access("/inline-data", F_OK) == 0) {
		check_inline_data();
	}
	if (access("/cluster-geometry", F_OK) == 0) {
		check_clusters();
	}
	for (index = 0; index < sizeof(bytes); index++) {
		bytes[index] = (uint8_t)(index * 23U + 0x67U);
	}
	acl = xattr_load("/linux-acl", &acl_size);
	capability = xattr_load("/linux-capability", &capability_size);
	require(setxattr("/mnt/block", "user.binary", bytes, sizeof(bytes), 0) == 0,
	    "Linux replace or create external attribute with shared-block COW");
	errno = 0;
	result = removexattr("/mnt/shared", "user.binary");
	require(
	    result == 0 || (result == -1 && errno == ENODATA), "Linux release shared attribute");
	require(chmod("/mnt/many", 0640) == 0, "Linux chmod updates compact ACL and mode together");
	umask(0);
	fd = open(LINUX_XATTR_FILE, O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
	require(fd >= 0 && write(fd, bytes, 17) == 17, "create Linux attributed file contents");
	require(fchown(fd, 12345, 23456) == 0, "set Linux attribute owner identity");
	require(fsetxattr(fd, "user.binary", bytes, 600, XATTR_CREATE) == 0,
	    "Linux create external attribute");
	errno = 0;
	require(fsetxattr(fd, "user.binary", bytes, 600, XATTR_CREATE) == -1 && errno == EEXIST,
	    "Linux create policy rejects an existing key");
	errno = 0;
	require(fsetxattr(fd, "user.absent", bytes, 1, XATTR_REPLACE) == -1 && errno == ENODATA,
	    "Linux replace policy rejects a missing key");
	require(fsetxattr(fd, "user.empty", "", 0, XATTR_CREATE) == 0,
	    "Linux empty value remains an attribute");
	require(fsetxattr(fd, "trusted.marker", bytes, 19, XATTR_CREATE) == 0,
	    "Linux create trusted attribute");
	require(fsetxattr(fd, "system.posix_acl_access", acl, acl_size, 0) == 0,
	    "Linux convert userspace ACL and update permission bits");
	require(fsetxattr(fd, "security.capability", capability, capability_size, 0) == 0,
	    "Linux create valid file capability metadata");
	require(fsync(fd) == 0 && close(fd) == 0, "commit Linux file attributes");
	xattr_check_acl_access();
	require(mkdir(LINUX_XATTR_DIRECTORY, 0750) == 0, "create Linux default-ACL directory");
	require(setxattr(LINUX_XATTR_DIRECTORY, "system.posix_acl_default", acl, acl_size, 0) == 0,
	    "Linux create default ACL storage");
	fd = open(LINUX_XATTR_INHERITED, O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC, 0640);
	require(fd >= 0 && fsync(fd) == 0 && close(fd) == 0,
	    "Linux inherit a core-preserved default ACL");
	free(capability);
	free(acl);
	xattr_sync_path("/mnt/block");
	xattr_sync_path("/mnt/shared");
	xattr_sync_path("/mnt/many");
	xattr_sync_path(LINUX_XATTR_DIRECTORY);
	xattr_sync_path("/mnt/directory");
	xattr_sync_path("/mnt");
	puts("LINUX_EXT4_XATTR_PASS");
	puts("LINUX_EXT4_COMMITTED_RECOVERY_PENDING");
	power_off(1);
}

#endif

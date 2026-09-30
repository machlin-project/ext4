/* SPDX-License-Identifier: BSD-3-Clause */
#define _DARWIN_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "linux_sha256.h"

#define MANIFEST_LINE 2048U
#define FILE_BUFFER 8191U

/* The encryption fixtures use relative, whitespace-free paths. Never interpret
 * an absolute path or a parent traversal from a malformed manifest. */
static bool
relative_path(const char *path)
{
	const char *component = path;
	const char *end;
	size_t length;

	while (*component != 0) {
		end = strchr(component, '/');
		length = end == NULL ? strlen(component) : (size_t)(end - component);
		if (length == 0 || (length == 1 && component[0] == '.') ||
		    (length == 2 && component[0] == '.' && component[1] == '.')) {
			return false;
		}
		if (end == NULL) {
			return true;
		}
		component = end + 1;
	}
	return false;
}

static bool
check_file(int root, const char *path, unsigned long long size, const char *expected)
{
	struct sha256 context;
	uint8_t bytes[FILE_BUFFER];
	char digest[2U * SHA256_BYTES + 1U];
	unsigned long long completed = 0;
	ssize_t count;
	int fd;
	bool valid;

	fd = openat(root, path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
	if (fd < 0) {
		return false;
	}
	sha256_init(&context);
	for (;;) {
		count = read(fd, bytes, sizeof(bytes));
		if (count < 0 && errno == EINTR) {
			continue;
		}
		if (count <= 0 || (unsigned long long)count > size - completed) {
			break;
		}
		sha256_update(&context, bytes, (size_t)count);
		completed += (unsigned long long)count;
	}
	valid = count == 0 && completed == size;
	if (close(fd) != 0) {
		valid = false;
	}
	sha256_final(&context, digest);
	return valid && strcmp(digest, expected) == 0;
}

static bool
check_entry(int root, const char *line)
{
	struct stat metadata;
	char kind[16];
	char path[1024];
	char value[1024];
	char expected[2U * SHA256_BYTES + 1U];
	char target[1024];
	char extra;
	char *end;
	unsigned long long size;
	ssize_t count;
	int fields;
	bool valid = false;

	fields = sscanf(line, "%15s %1023s %1023s %64s %c", kind, path, value, expected, &extra);
	if (fields < 2 || !relative_path(path)) {
		fprintf(stderr, "FAIL: malformed manifest entry\n");
		return false;
	}
	if (fstatat(root, path, &metadata, AT_SYMLINK_NOFOLLOW) != 0) {
		fprintf(stderr, "FAIL: %s: %s\n", path, strerror(errno));
		return false;
	}
	if (strcmp(kind, "file") == 0 && fields == 4 && S_ISREG(metadata.st_mode)) {
		errno = 0;
		size = strtoull(value, &end, 10);
		valid = value[0] >= '0' && value[0] <= '9' && *end == 0 && errno == 0 &&
		    metadata.st_size >= 0 && (unsigned long long)metadata.st_size == size &&
		    strlen(expected) == 2U * SHA256_BYTES && check_file(root, path, size, expected);
	} else if (strcmp(kind, "symlink") == 0 && fields == 3 && S_ISLNK(metadata.st_mode)) {
		count = readlinkat(root, path, target, sizeof(target));
		valid = count >= 0 && (size_t)count == strlen(value) &&
		    memcmp(target, value, (size_t)count) == 0;
	} else if (fields == 2) {
		valid = (strcmp(kind, "directory") == 0 && S_ISDIR(metadata.st_mode)) ||
		    (strcmp(kind, "fifo") == 0 && S_ISFIFO(metadata.st_mode));
	}
	printf("%s: %s %s\n", valid ? "PASS" : "FAIL", kind, path);
	return valid;
}

int
main(int argc, char **argv)
{
	FILE *manifest;
	char line[MANIFEST_LINE];
	unsigned int checked = 0;
	unsigned int failed = 0;
	int root;

	if (argc != 3) {
		fprintf(stderr, "usage: ext4-mounted-manifest-test MOUNTPOINT MANIFEST\n");
		return 2;
	}
	root = open(argv[1], O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	manifest = fopen(argv[2], "r");
	if (root < 0 || manifest == NULL) {
		fprintf(stderr, "Cannot open mounted tree or manifest: %s\n", strerror(errno));
		if (root >= 0) {
			close(root);
		}
		if (manifest != NULL) {
			fclose(manifest);
		}
		return 1;
	}
	while (fgets(line, sizeof(line), manifest) != NULL) {
		if (strchr(line, '\n') == NULL) {
			fprintf(stderr, "FAIL: unterminated or oversized manifest entry\n");
			failed++;
			break;
		}
		if (!check_entry(root, line)) {
			failed++;
		}
		checked++;
	}
	if (ferror(manifest)) {
		failed++;
	}
	if (fclose(manifest) != 0) {
		failed++;
	}
	if (close(root) != 0) {
		failed++;
	}
	printf("Mounted manifest: %u checked, %u failed\n", checked, failed);
	return checked != 0 && failed == 0 ? 0 : 1;
}

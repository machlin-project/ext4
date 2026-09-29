/* SPDX-License-Identifier: BSD-3-Clause */
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/reboot.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/utsname.h>
#include <unistd.h>

#include "linux_sha256.h"

/* Guest probe for a sustained-operation export, mounted read-only. Without the key
 * it lists each encrypted directory the manifest names by its no-key path and
 * reports every name. It then adds the Linux probe's fscrypt master key and checks
 * every object: file contents by size and SHA-256, symlink targets in hex, types, and
 * verity digests by FS_IOC_MEASURE_VERITY. Paths are walked one component at a time,
 * so their length is not limited by PATH_MAX. */

#define MANIFEST_LINE 65536U
#define READ_BYTES 65536U
#define KEY_BYTES 64U
#define KEY_MULTIPLIER 7U
#define KEY_OFFSET 3U
#define KEY_IDENTIFIER_BYTES 16U
#define FSCRYPT_KEY_SPEC_TYPE_IDENTIFIER 2U
#define FSCRYPT_IOCTL_TYPE 'f'
#define FSCRYPT_ADD_KEY_NUMBER 23
#define FSVERITY_MEASURE_NUMBER 134
#define DIGEST_MAX 64U

/* Linux UAPI <linux/fscrypt.h> and <linux/fsverity.h> layouts; the musl sysroot has
 * no kernel headers. */
struct fscrypt_key_specifier {
	uint32_t type;
	uint32_t reserved;

	union {
		uint8_t reserved_space[32];
		uint8_t identifier[KEY_IDENTIFIER_BYTES];
	} u;
};

struct fscrypt_add_key_header {
	struct fscrypt_key_specifier key_spec;
	uint32_t raw_size;
	uint32_t key_id;
	uint32_t reserved[8];
};

struct fscrypt_add_key_argument {
	struct fscrypt_add_key_header header;
	uint8_t raw[KEY_BYTES];
};

struct fsverity_digest_header {
	uint16_t digest_algorithm;
	uint16_t digest_size;
};

#define FS_IOC_ADD_ENCRYPTION_KEY                                                                  \
	_IOWR(FSCRYPT_IOCTL_TYPE, FSCRYPT_ADD_KEY_NUMBER, struct fscrypt_add_key_header)
#define FS_IOC_MEASURE_VERITY                                                                      \
	_IOWR(FSCRYPT_IOCTL_TYPE, FSVERITY_MEASURE_NUMBER, struct fsverity_digest_header)

static char line[MANIFEST_LINE];
static uint8_t bytes[READ_BYTES];

static void
power_off(int passed)
{
	printf("LINUX_SUSTAINED_RESULT=%s\n", passed ? "PASS" : "FAIL");
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

/* Open the directory holding a path's last component, one component at a time,
 * and return that component. */
static int
open_parent(char *path, char **last)
{
	char *slash;
	int directory = open("/mnt", O_PATH | O_DIRECTORY | O_CLOEXEC);
	int next;

	require(directory >= 0, "open mount");
	while ((slash = strchr(path, '/')) != NULL) {
		*slash = 0;
		next = openat(directory, path, O_PATH | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
		require(next >= 0, path);
		require(close(directory) == 0, "close path component");
		directory = next;
		path = slash + 1;
	}
	*last = path;
	return directory;
}

/* The next space-separated field of a manifest line. */
static char *
field(char **cursor)
{
	char *start = *cursor;
	char *end;

	require(start != NULL && *start != 0, "parse manifest field");
	end = strchr(start, ' ');
	if (end == NULL) {
		end = start + strlen(start);
		*cursor = NULL;
	} else {
		*end = 0;
		*cursor = end + 1;
	}
	return start;
}

static unsigned int
list_nokey(char *number, char *path)
{
	struct dirent *entry;
	struct stat metadata;
	unsigned int names = 0;
	char *last;
	int parent = open_parent(path, &last);
	int directory = openat(parent, last, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
	DIR *stream;

	require(directory >= 0, "open encrypted directory without the key");
	stream = fdopendir(directory);
	require(stream != NULL, "list encrypted directory");
	errno = 0;
	while ((entry = readdir(stream)) != NULL) {
		if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
			continue;
		}
		printf("LINUX_SUSTAINED_NOKEY=%s %s\n", number, entry->d_name);
		require(fstatat(directory, entry->d_name, &metadata, AT_SYMLINK_NOFOLLOW) == 0,
		    "look up no-key name");
		names++;
		errno = 0;
	}
	require(errno == 0 && closedir(stream) == 0 && close(parent) == 0,
	    "finish encrypted directory");
	return names;
}

static void
add_key(void)
{
	struct fscrypt_add_key_argument argument;
	unsigned int index;
	int root = open("/mnt", O_RDONLY | O_DIRECTORY | O_CLOEXEC);

	require(root >= 0, "open filesystem root");
	memset(&argument, 0, sizeof(argument));
	argument.header.key_spec.type = FSCRYPT_KEY_SPEC_TYPE_IDENTIFIER;
	argument.header.raw_size = KEY_BYTES;
	for (index = 0; index < KEY_BYTES; index++) {
		argument.raw[index] = (uint8_t)(index * KEY_MULTIPLIER + KEY_OFFSET);
	}
	require(ioctl(root, FS_IOC_ADD_ENCRYPTION_KEY, &argument) == 0 && close(root) == 0,
	    "add fscrypt master key");
}

static void
check_file(int parent, const char *name, char **cursor)
{
	char *size = field(cursor);
	char *expected = field(cursor);
	struct sha256 context;
	char digest[2U * SHA256_BYTES + 1U];
	unsigned long long total = 0;
	ssize_t count;
	int fd = openat(parent, name, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);

	require(fd >= 0, "open file");
	sha256_init(&context);
	while ((count = read(fd, bytes, sizeof(bytes))) > 0) {
		sha256_update(&context, bytes, (size_t)count);
		total += (unsigned long long)count;
	}
	require(count == 0 && close(fd) == 0, "read file");
	sha256_final(&context, digest);
	require(total == strtoull(size, NULL, 10) && strcmp(digest, expected) == 0,
	    "compare file contents");
}

static void
check_symlink(int parent, const char *name, const char *expected)
{
	char text[3];
	ssize_t count = readlinkat(parent, name, (char *)bytes, sizeof(bytes));
	ssize_t index;

	require(count > 0 && (size_t)count * 2U == strlen(expected), "read symlink");
	for (index = 0; index < count; index++) {
		snprintf(text, sizeof(text), "%02x", bytes[index]);
		require(memcmp(text, expected + 2 * index, 2) == 0, "compare symlink target");
	}
}

static void
check_verity(int parent, const char *name, const char *expected)
{
	struct {
		struct fsverity_digest_header header;
		uint8_t digest[DIGEST_MAX];
	} buffer;

	char text[2U * DIGEST_MAX + 1U];
	unsigned int index;
	int fd = openat(parent, name, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);

	require(fd >= 0, "open verity file");
	memset(&buffer, 0, sizeof(buffer));
	buffer.header.digest_size = DIGEST_MAX;
	require(ioctl(fd, FS_IOC_MEASURE_VERITY, &buffer) == 0 && close(fd) == 0,
	    "measure verity file");
	for (index = 0; index < buffer.header.digest_size; index++) {
		snprintf(text + index * 2U, 3, "%02x", buffer.digest[index]);
	}
	require(strcmp(text, expected) == 0, "compare verity digest");
}

static unsigned int
check_objects(FILE *manifest, unsigned int *verity)
{
	struct stat metadata;
	unsigned int objects = 0;
	char *cursor;
	char *kind;
	char *last;
	int parent;

	while (fgets(line, sizeof(line), manifest) != NULL) {
		require(strchr(line, '\n') != NULL, "manifest line length");
		*strchr(line, '\n') = 0;
		cursor = line;
		kind = field(&cursor);
		if (strcmp(kind, "nokeydir") == 0) {
			continue;
		}
		parent = open_parent(field(&cursor), &last);
		require(
		    fstatat(parent, last, &metadata, AT_SYMLINK_NOFOLLOW) == 0, "look up object");
		if (strcmp(kind, "file") == 0) {
			require(S_ISREG(metadata.st_mode), "file type");
			check_file(parent, last, &cursor);
		} else if (strcmp(kind, "symlink") == 0) {
			require(S_ISLNK(metadata.st_mode), "symlink type");
			check_symlink(parent, last, field(&cursor));
		} else if (strcmp(kind, "verity") == 0) {
			check_verity(parent, last, field(&cursor));
			(*verity)++;
			objects--;
		} else if (strcmp(kind, "directory") == 0) {
			require(S_ISDIR(metadata.st_mode), "directory type");
		} else if (strcmp(kind, "fifo") == 0) {
			require(S_ISFIFO(metadata.st_mode), "fifo type");
		} else {
			require(strcmp(kind, "device") == 0 && S_ISCHR(metadata.st_mode),
			    "device type");
		}
		require(close(parent) == 0, "close parent");
		objects++;
	}
	return objects;
}

int
main(void)
{
	const char *modules[] = { "virtio_blk", "crc32c_generic", "crc16", "mbcache", "jbd2",
		"ext4" };
	struct utsname identity;
	char path[64];
	char *cursor;
	char *number;
	unsigned int names = 0;
	unsigned int objects;
	unsigned int verity = 0;
	unsigned int index;
	FILE *manifest;
	int fd;
	int result;

	setvbuf(stdout, NULL, _IOLBF, 0);
	require(uname(&identity) == 0, "uname");
	printf("LINUX_SUSTAINED_KERNEL=%s %s %s\n", identity.sysname, identity.release,
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
	require(mount("/dev/vda", "/mnt", "ext4", MS_RDONLY, NULL) == 0, "mount read-only");
	manifest = fopen("/manifest", "r");
	require(manifest != NULL, "open manifest");
	while (fgets(line, sizeof(line), manifest) != NULL) {
		require(strchr(line, '\n') != NULL, "manifest line length");
		*strchr(line, '\n') = 0;
		cursor = line;
		if (strcmp(field(&cursor), "nokeydir") == 0) {
			number = field(&cursor);
			names += list_nokey(number, field(&cursor));
		}
	}
	printf("LINUX_SUSTAINED_NOKEY_NAMES=%u\n", names);
	add_key();
	rewind(manifest);
	objects = check_objects(manifest, &verity);
	require(fclose(manifest) == 0 && objects != 0, "complete manifest");
	printf("LINUX_SUSTAINED_OBJECTS=%u\nLINUX_SUSTAINED_VERITY=%u\n", objects, verity);
	puts("LINUX_SUSTAINED_VERIFIED");
	require(umount("/mnt") == 0, "unmount");
	power_off(1);
	return 0;
}

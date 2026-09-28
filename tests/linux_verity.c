/* SPDX-License-Identifier: BSD-3-Clause */
#define _GNU_SOURCE
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
#include <sys/statfs.h>
#include <sys/syscall.h>
#include <sys/utsname.h>
#include <unistd.h>

/* Guest probe: Linux reads independently authored fs-verity files read-only.
 * Valid files must measure to the manifest's digest and read completely.
 * Damaged data or tree blocks must fail with EIO; invalid descriptors must fail
 * to open or read. */

#define MANIFEST_LINE 512U
#define DIGEST_MAX 64U
#define BLOCK_MAX 65536U
#define FSVERITY_IOCTL_TYPE 'f'
#define FSVERITY_MEASURE_NUMBER 134

/* Linux UAPI <linux/fsverity.h> header; the musl sysroot has no kernel headers. */
struct fsverity_digest_header {
	uint16_t digest_algorithm;
	uint16_t digest_size;
};

#define FS_IOC_MEASURE_VERITY                                                                      \
	_IOWR(FSVERITY_IOCTL_TYPE, FSVERITY_MEASURE_NUMBER, struct fsverity_digest_header)

static void
power_off(int passed)
{
	printf("LINUX_VERITY_RESULT=%s\n", passed ? "PASS" : "FAIL");
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
measure(int fd, const char *expected)
{
	struct {
		struct fsverity_digest_header header;
		uint8_t digest[DIGEST_MAX];
	} buffer;

	char text[2U * DIGEST_MAX + 1U];
	unsigned int index;

	memset(&buffer, 0, sizeof(buffer));
	buffer.header.digest_size = DIGEST_MAX;
	require(ioctl(fd, FS_IOC_MEASURE_VERITY, &buffer) == 0, "measure verity file");
	for (index = 0; index < buffer.header.digest_size; index++) {
		snprintf(text + index * 2U, 3, "%02x", buffer.digest[index]);
	}
	require(strcmp(text, expected) == 0, "compare Linux verity digest");
}

static int
read_at(int fd, off_t offset, uint8_t *bytes, size_t length)
{
	ssize_t result = pread(fd, bytes, length, offset);

	return result < 0 ? -errno : 0;
}

int
main(void)
{
	const char *modules[] = { "virtio_blk", "crc32c_generic", "crc16", "mbcache", "jbd2",
		"ext4" };
	static uint8_t block[BLOCK_MAX];
	struct utsname identity;
	struct stat metadata;
	char line[MANIFEST_LINE];
	char path[128];
	char kind[16];
	char name[64];
	char sha256[80];
	char digest[2U * DIGEST_MAX + 1U];
	char failure[16];
	unsigned long long size;
	unsigned int algorithm;
	unsigned int block_size;
	unsigned int index;
	unsigned int checked = 0;
	off_t offset;
	ssize_t count;
	FILE *manifest;
	int fd;
	int result;

	setvbuf(stdout, NULL, _IOLBF, 0);
	require(uname(&identity) == 0, "uname");
	printf(
	    "LINUX_VERITY_KERNEL=%s %s %s\n", identity.sysname, identity.release, identity.machine);
	require(mount("devtmpfs", "/dev", "devtmpfs", 0, NULL) == 0, "mount devtmpfs");
	for (index = 0; index < sizeof(modules) / sizeof(modules[0]); index++) {
		snprintf(path, sizeof(path), "/modules/%s.ko", modules[index]);
		fd = open(path, O_RDONLY | O_CLOEXEC);
		require(fd >= 0, "open matching module");
		result = (int)syscall(SYS_finit_module, fd, "", 0);
		require(result == 0 || errno == EEXIST, path);
		require(close(fd) == 0, "close module");
	}
	require(mount("/dev/vda", "/mnt", "ext4", MS_RDONLY | MS_NOATIME, NULL) == 0,
	    "mount verity filesystem read-only");
	manifest = fopen("/manifest", "r");
	require(manifest != NULL && fgets(line, sizeof(line), manifest) != NULL, "read manifest");
	require(sscanf(line, "algorithm %u block %u", &algorithm, &block_size) == 2 &&
		block_size <= BLOCK_MAX,
	    "parse manifest header");
	while (fgets(line, sizeof(line), manifest) != NULL) {
		require(sscanf(line, "%15s %63s %llu %79s %128s %15s", kind, name, &size, sha256,
			    digest, failure) == 6,
		    "parse manifest entry");
		snprintf(path, sizeof(path), "/mnt/%s", name);
		fd = open(path, O_RDONLY | O_CLOEXEC);
		if (strcmp(kind, "version") == 0 || strcmp(kind, "size") == 0) {
			/* Invalid descriptors fail at open, or at the latest on read. */
			require(
			    fd < 0 || read_at(fd, 0, block, 1) < 0, "reject invalid descriptor");
			if (fd >= 0) {
				close(fd);
			}
			printf("LINUX_VERITY_REJECTED=%s\n", name);
			checked++;
			continue;
		}
		require(fd >= 0 && fstat(fd, &metadata) == 0 && metadata.st_size == (off_t)size,
		    "open verity file");
		if (strcmp(kind, "good") == 0) {
			measure(fd, digest);
			for (offset = 0; offset < (off_t)size; offset += count) {
				count = pread(fd, block, block_size, offset);
				require(count > 0, "read verified file");
			}
			require(offset == (off_t)size, "read complete verified file");
			printf("LINUX_VERITY_VERIFIED=%s\n", name);
		} else if (strcmp(kind, "root") == 0) {
			require(read_at(fd, 0, block, 1) == -EIO, "reject damaged root hash");
			printf("LINUX_VERITY_REJECTED=%s\n", name);
		} else {
			offset = (off_t)strtoull(failure, NULL, 10) * block_size;
			require(read_at(fd, offset, block, 1) == -EIO, "reject damaged block");
			require(
			    read_at(fd, (off_t)size - 1, block, 1) == 0, "read undamaged block");
			printf("LINUX_VERITY_REJECTED=%s\n", name);
		}
		require(close(fd) == 0, "close verity file");
		checked++;
	}
	require(fclose(manifest) == 0 && checked != 0, "complete manifest");
	require(umount("/mnt") == 0, "cleanly unmount verity filesystem");
	printf("LINUX_VERITY_FILES=%u\n", checked);
	power_off(1);
	return 0;
}

/* SPDX-License-Identifier: BSD-3-Clause */
#define _GNU_SOURCE
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

/* Guest probe: Linux acquires multi-mount protection on volumes the core released
 * or left pending, and releases it again. /phase selects the case: 0 writes a file
 * after the core released the volume, 1 requires the file the core wrote after
 * Linux, and 2 takes over a volume the core left with a pending journal. */

#define PHASE_RELEASED 0U
#define PHASE_CONTINUED 1U
#define PHASE_PENDING 2U
#define LINUX_FILE "/mnt/linux-mmp"
#define CORE_FILE "/mnt/core-after-linux"
#define PAYLOAD "written by Linux after acquiring multi-mount protection\n"
#define PATH_BYTES 64U

static void
power_off(int passed)
{
	printf("LINUX_MMP_RESULT=%s\n", passed ? "PASS" : "FAIL");
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

int
main(void)
{
	static const char *const modules[] = { "virtio_blk", "crc32c_generic", "crc16", "mbcache",
		"jbd2", "ext4" };
	struct utsname identity;
	struct timespec before;
	struct timespec after;
	struct stat metadata;
	char path[PATH_BYTES];
	unsigned int phase = PHASE_PENDING + 1U;
	unsigned int index;
	FILE *config;
	ssize_t written;
	int fd;
	int result;

	setvbuf(stdout, NULL, _IOLBF, 0);
	require(uname(&identity) == 0, "uname");
	printf("LINUX_MMP_KERNEL=%s %s %s\n", identity.sysname, identity.release, identity.machine);
	printf("LINUX_MMP_NODE=%s\n", identity.nodename);
	require(mount("devtmpfs", "/dev", "devtmpfs", 0, NULL) == 0, "mount devtmpfs");
	for (index = 0; index < sizeof(modules) / sizeof(modules[0]); index++) {
		snprintf(path, sizeof(path), "/modules/%s.ko", modules[index]);
		fd = open(path, O_RDONLY | O_CLOEXEC);
		require(fd >= 0, "open matching module");
		result = (int)syscall(SYS_finit_module, fd, "", 0);
		require(result == 0 || errno == EEXIST, path);
		require(close(fd) == 0, "close module");
	}
	config = fopen("/phase", "r");
	require(config != NULL && fscanf(config, "%u", &phase) == 1 && fclose(config) == 0 &&
		phase <= PHASE_PENDING,
	    "read probe phase");
	/* Linux waits at least two check intervals before it owns the volume. */
	require(clock_gettime(CLOCK_MONOTONIC, &before) == 0, "read clock");
	require(mount("/dev/vda", "/mnt", "ext4", MS_NOATIME, NULL) == 0,
	    "acquire multi-mount protection");
	require(clock_gettime(CLOCK_MONOTONIC, &after) == 0, "read clock");
	printf("LINUX_MMP_MOUNT_SECONDS=%lld\n", (long long)(after.tv_sec - before.tv_sec));
	if (phase == PHASE_RELEASED) {
		fd = open(LINUX_FILE, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0644);
		require(fd >= 0, "create file after acquisition");
		written = write(fd, PAYLOAD, strlen(PAYLOAD));
		require(written == (ssize_t)strlen(PAYLOAD) && fsync(fd) == 0 && close(fd) == 0,
		    "write file after acquisition");
	} else if (phase == PHASE_CONTINUED) {
		require(stat(LINUX_FILE, &metadata) == 0 && metadata.st_size == strlen(PAYLOAD),
		    "find Linux file");
		require(stat(CORE_FILE, &metadata) == 0, "find file written by the core");
	}
	require(umount("/mnt") == 0, "release multi-mount protection");
	power_off(1);
	return 0;
}

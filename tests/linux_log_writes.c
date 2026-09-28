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
#include <sys/syscall.h>
#include <sys/utsname.h>
#include <unistd.h>

/* Guest probe: Linux recovers the pending volume on /dev/vda through a dm-log-writes
 * target whose log is /dev/vdb. Mounting replays the journal, including fast
 * commits; the probe then unmounts and removes the target, which completes the log
 * of every write, flush and FUA write Linux issued, in completion order. */

#define PATH_BYTES 128U
#define KERNEL_RECORD_BYTES 1024U
#define SECTOR_BYTES 512U
#define TRACE_NAME "trace"
#define TRACE_DEVICE "/dev/dm-0"
#define TRACE_PARAMETERS "/dev/vda /dev/vdb"
#define TRACE_TARGET "log-writes"

/* Linux UAPI <linux/dm-ioctl.h>; the musl sysroot lacks it. */
#define DM_IOCTL_TYPE 0xfdU
#define DM_VERSION_MAJOR 4U
#define DM_NAME_BYTES 128U
#define DM_UUID_BYTES 129U
#define DM_TYPE_NAME_BYTES 16U
#define DM_IOCTL_BYTES 312U
#define DM_TARGET_SPEC_BYTES 40U
#define DM_DEV_CREATE_COMMAND 3U
#define DM_DEV_REMOVE_COMMAND 4U
#define DM_DEV_SUSPEND_COMMAND 6U
#define DM_TABLE_LOAD_COMMAND 9U
#define DM_COMMAND(command) _IOWR(DM_IOCTL_TYPE, command, struct dm_ioctl)

struct dm_ioctl {
	uint32_t version[3];
	uint32_t data_size;
	uint32_t data_start;
	uint32_t target_count;
	int32_t open_count;
	uint32_t flags;
	uint32_t event_nr;
	uint32_t padding;
	uint64_t dev;
	char name[DM_NAME_BYTES];
	char uuid[DM_UUID_BYTES];
	char data[7];
};

struct dm_target_spec {
	uint64_t sector_start;
	uint64_t length;
	int32_t status;
	uint32_t next;
	char target_type[DM_TYPE_NAME_BYTES];
};

struct dm_table {
	struct dm_ioctl header;
	struct dm_target_spec target;
	char parameters[sizeof(TRACE_PARAMETERS)];
};

_Static_assert(sizeof(struct dm_ioctl) == DM_IOCTL_BYTES, "device-mapper ioctl ABI");
_Static_assert(sizeof(struct dm_target_spec) == DM_TARGET_SPEC_BYTES, "target ABI");

static void
power_off(int passed)
{
	printf("LINUX_LOG_WRITES_RESULT=%s\n", passed ? "PASS" : "FAIL");
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
dm_header(struct dm_ioctl *header, uint32_t size)
{
	memset(header, 0, sizeof(*header));
	header->version[0] = DM_VERSION_MAJOR;
	header->data_size = size;
	header->data_start = sizeof(*header);
	strncpy(header->name, TRACE_NAME, sizeof(header->name) - 1U);
}

/* Print the kernel's filesystem, journal and device-mapper messages. */
static void
kernel_messages(void)
{
	char record[KERNEL_RECORD_BYTES];
	const char *message;
	ssize_t length;
	int fd = open("/dev/kmsg", O_RDONLY | O_NONBLOCK | O_CLOEXEC);

	require(fd >= 0, "open kernel log");
	for (;;) {
		length = read(fd, record, sizeof(record) - 1U);
		if (length < 0 && errno == EPIPE) {
			continue;
		}
		if (length <= 0) {
			break;
		}
		record[length] = '\0';
		if (strstr(record, "EXT4-fs") != NULL || strstr(record, "JBD2") != NULL ||
		    strstr(record, "device-mapper") != NULL) {
			record[strcspn(record, "\n")] = '\0';
			message = strchr(record, ';');
			printf("LINUX_LOG_WRITES_KERNEL_LOG=%s\n",
			    message != NULL ? message + 1 : record);
		}
	}
	require(close(fd) == 0, "close kernel log");
}

int
main(void)
{
	static const char *const modules[] = { "virtio_blk", "crc32c_generic", "crc16", "mbcache",
		"jbd2", "ext4", "dm-mod", "dm-log-writes" };
	struct dm_ioctl header;
	struct dm_table table;
	struct utsname identity;
	struct stat metadata;
	char path[PATH_BYTES];
	uint64_t bytes = 0;
	unsigned int index;
	int control;
	int fd;
	int result;

	setvbuf(stdout, NULL, _IOLBF, 0);
	require(uname(&identity) == 0, "uname");
	printf("LINUX_LOG_WRITES_KERNEL=%s %s %s\n", identity.sysname, identity.release,
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
	fd = open("/dev/vda", O_RDONLY | O_CLOEXEC);
	require(fd >= 0 && ioctl(fd, BLKGETSIZE64, &bytes) == 0 && bytes % SECTOR_BYTES == 0 &&
		close(fd) == 0,
	    "size recovered device");
	printf("LINUX_LOG_WRITES_DEVICE_BYTES=%llu\n", (unsigned long long)bytes);
	control = open("/dev/mapper/control", O_RDWR | O_CLOEXEC);
	require(control >= 0, "open device-mapper control");
	dm_header(&header, sizeof(header));
	require(
	    ioctl(control, DM_COMMAND(DM_DEV_CREATE_COMMAND), &header) == 0, "create trace device");
	memset(&table, 0, sizeof(table));
	dm_header(&table.header, sizeof(table));
	table.header.target_count = 1;
	table.target.length = bytes / SECTOR_BYTES;
	strncpy(table.target.target_type, TRACE_TARGET, sizeof(table.target.target_type) - 1U);
	memcpy(table.parameters, TRACE_PARAMETERS, sizeof(TRACE_PARAMETERS));
	require(ioctl(control, DM_COMMAND(DM_TABLE_LOAD_COMMAND), &table) == 0,
	    "load log-writes table");
	dm_header(&header, sizeof(header));
	require(ioctl(control, DM_COMMAND(DM_DEV_SUSPEND_COMMAND), &header) == 0,
	    "activate trace device");
	require(
	    stat(TRACE_DEVICE, &metadata) == 0 && S_ISBLK(metadata.st_mode), "find trace device");
	/* Mounting recovers the journal and replays its fast commits. */
	require(mount(TRACE_DEVICE, "/mnt", "ext4", 0, NULL) == 0, "mount pending filesystem");
	printf("LINUX_LOG_WRITES_MOUNTED=1\n");
	require(umount("/mnt") == 0, "cleanly unmount recovered filesystem");
	/* Removing the target logs its remaining writes and the final log header. */
	dm_header(&header, sizeof(header));
	require(
	    ioctl(control, DM_COMMAND(DM_DEV_REMOVE_COMMAND), &header) == 0, "remove trace device");
	require(close(control) == 0, "close device-mapper control");
	for (index = 0; index < 2U; index++) {
		fd = open(index == 0 ? "/dev/vda" : "/dev/vdb", O_RDONLY | O_CLOEXEC);
		require(fd >= 0 && fsync(fd) == 0 && close(fd) == 0, "flush virtual disk");
	}
	kernel_messages();
	power_off(1);
	return 0;
}

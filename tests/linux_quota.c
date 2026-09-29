/* SPDX-License-Identifier: BSD-3-Clause */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <grp.h>
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
#include <sys/wait.h>
#include <unistd.h>

/* Guest probe: Linux reads the usage of quota files written by the core through
 * quotactl. The mutation phase also removes and creates files, moves owners and
 * changes a project, so the kernel frees and reuses quota entries and blocks
 * before the core continues from its result. The enforcement phase mounts with
 * limits enabled and reports where the kernel refuses an unprivileged owner.
 * /phase holds "PHASE PROJECT". */

#define PHASE_REPORT 0U
#define PHASE_MUTATE 1U
#define PHASE_ENFORCE 2U
#define QUOTA_TYPES 3U
#define REMOVED_FIRST 2030U
#define REMOVED_LAST 2039U
#define LINUX_FIRST_OWNER 6000U
#define LINUX_OWNERS 5U
#define LINUX_GROUP 6100U
#define FAR_OWNER 0x01000000U
#define TOUCHED_OWNER 2012U
#define TOUCHED_GROUP 3000U
#define PROJECT_FILE_OWNER 2015U
#define LINUX_PROJECT 77U
#define BLOCK_BYTES 4096U
/* The owner whose limits the core's enforcement test wrote, and its file. */
#define ENFORCED_OWNER 1000U
#define ENFORCED_GROUP 100U
#define ENFORCED_FILE "/mnt/e2"
#define ENFORCED_DIRECTORY "/mnt/enforced"
#define ENFORCED_ATTEMPTS 16U
#define PATH_BYTES 128U

/* Linux UAPI <linux/quota.h> and <linux/fs.h>; the musl sysroot lacks them. */
#define Q_GETNEXTQUOTA 0x800009U
#define QUOTA_COMMAND(command, type) (((command) << 8) | ((type) & 0x00ffU))
#define FS_IOC_FSGETXATTR _IOR('X', 31, struct fsxattr)
#define FS_IOC_FSSETXATTR _IOW('X', 32, struct fsxattr)

struct next_quota {
	uint64_t space_hard;
	uint64_t space_soft;
	uint64_t space;
	uint64_t inode_hard;
	uint64_t inode_soft;
	uint64_t inodes;
	uint64_t space_time;
	uint64_t inode_time;
	uint32_t valid;
	uint32_t id;
};

struct fsxattr {
	uint32_t flags;
	uint32_t extent_size;
	uint32_t extents;
	uint32_t project;
	uint32_t cow_extent_size;
	uint8_t padding[8];
};

static void
power_off(int passed)
{
	printf("LINUX_QUOTA_RESULT=%s\n", passed ? "PASS" : "FAIL");
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

/* Print every ID with usage, as the kernel's quota tree reader sees it. */
static void
report(unsigned int types)
{
	struct next_quota quota;
	unsigned int type;
	uint32_t id;
	unsigned int count = 0;

	for (type = 0; type < types; type++) {
		id = 0;
		for (;;) {
			memset(&quota, 0, sizeof(quota));
			if (syscall(SYS_quotactl, QUOTA_COMMAND(Q_GETNEXTQUOTA, type), "/dev/vda",
				id, &quota) != 0) {
				require(errno == ENOENT, "query next quota");
				break;
			}
			if (quota.space != 0 || quota.inodes != 0) {
				printf("LINUX_QUOTA=%u %u %llu %llu\n", type, quota.id,
				    (unsigned long long)quota.space,
				    (unsigned long long)quota.inodes);
				count++;
			}
			/* ID 0xffffffff is invalid, so the last query starts below it. */
			if (quota.id >= UINT32_MAX - 1U) {
				break;
			}
			id = quota.id + 1U;
		}
	}
	printf("LINUX_QUOTA_ENTRIES=%u\n", count);
}

static void
create_owned(const char *path, uint32_t uid, uint32_t gid, unsigned int blocks)
{
	static uint8_t block[BLOCK_BYTES];
	unsigned int index;
	int fd;

	memset(block, (int)(uid & 0x7fU), sizeof(block));
	fd = open(path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0644);
	require(fd >= 0 && fchown(fd, uid, gid) == 0, "create owned file");
	for (index = 0; index < blocks; index++) {
		require(
		    write(fd, block, sizeof(block)) == (ssize_t)sizeof(block), "write owned file");
	}
	require(fsync(fd) == 0 && close(fd) == 0, "persist owned file");
}

static void
mutate(unsigned int project)
{
	struct fsxattr attributes;
	char path[PATH_BYTES];
	uint32_t owner;
	int fd;

	/* Removing every file of an owner releases its entry in the kernel. */
	for (owner = REMOVED_FIRST; owner <= REMOVED_LAST; owner++) {
		snprintf(path, sizeof(path), "/mnt/q/f%u", owner);
		require(unlink(path) == 0, "remove core-created file");
	}
	/* A distant ID gains and loses usage, so its tree path is freed. */
	snprintf(path, sizeof(path), "/mnt/q/f%u", TOUCHED_OWNER);
	require(chown(path, FAR_OWNER, FAR_OWNER) == 0, "move file to a distant owner");
	require(chown(path, TOUCHED_OWNER, TOUCHED_GROUP) == 0, "restore file owner");
	for (owner = 0; owner < LINUX_OWNERS; owner++) {
		snprintf(path, sizeof(path), "/mnt/q/linux%u", owner);
		create_owned(path, LINUX_FIRST_OWNER + owner, LINUX_GROUP, owner + 1U);
	}
	if (project) {
		snprintf(path, sizeof(path), "/mnt/q/f%u", PROJECT_FILE_OWNER);
		fd = open(path, O_RDONLY | O_CLOEXEC);
		require(fd >= 0 && ioctl(fd, FS_IOC_FSGETXATTR, &attributes) == 0,
		    "read project attributes");
		attributes.project = LINUX_PROJECT;
		require(ioctl(fd, FS_IOC_FSSETXATTR, &attributes) == 0 && close(fd) == 0,
		    "change project");
	}
	sync();
}

/* Linux enforces limits for a process without CAP_SYS_RESOURCE: an unprivileged
 * child appends to the owner's file until the kernel refuses, then creates one
 * inode beyond the owner's inode limit. */
static void
enforce(void)
{
	static uint8_t block[BLOCK_BYTES];
	size_t written = 0;
	unsigned int attempt;
	int status;
	int fd;
	pid_t child;

	require(mkdir(ENFORCED_DIRECTORY, 0755) == 0 &&
		chown(ENFORCED_DIRECTORY, ENFORCED_OWNER, ENFORCED_GROUP) == 0,
	    "create the owner's directory");
	child = fork();
	require(child >= 0, "fork unprivileged owner");
	if (child == 0) {
		require(setgroups(0, NULL) == 0 && setgid(ENFORCED_GROUP) == 0 &&
			setuid(ENFORCED_OWNER) == 0,
		    "drop privileges");
		fd = open(ENFORCED_FILE, O_WRONLY | O_APPEND | O_CLOEXEC);
		require(fd >= 0, "open the owner's file");
		memset(block, 'L', sizeof(block));
		for (attempt = 0; attempt < ENFORCED_ATTEMPTS; attempt++) {
			if (write(fd, block, sizeof(block)) != (ssize_t)sizeof(block)) {
				break;
			}
			written += sizeof(block);
		}
		printf("LINUX_QUOTA_ENFORCE_BYTES=%zu\n", written);
		printf("LINUX_QUOTA_ENFORCE_WRITE_ERROR=%d\n", errno);
		require(fsync(fd) == 0 && close(fd) == 0, "persist the owner's file");
		fd = open(
		    ENFORCED_DIRECTORY "/beyond", O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0644);
		printf("LINUX_QUOTA_ENFORCE_CREATE_ERROR=%d\n", fd < 0 ? errno : 0);
		fflush(stdout);
		_exit(0);
	}
	require(
	    waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0,
	    "wait for unprivileged owner");
	sync();
}

int
main(void)
{
	static const char *const modules[] = { "virtio_blk", "crc32c_generic", "crc16", "mbcache",
		"jbd2", "ext4", "quota_tree", "quota_v2" };
	struct utsname identity;
	char path[PATH_BYTES];
	unsigned int phase = PHASE_MUTATE + 1U;
	unsigned int project = 2;
	unsigned int index;
	FILE *config;
	int fd;
	int result;

	setvbuf(stdout, NULL, _IOLBF, 0);
	require(uname(&identity) == 0, "uname");
	printf(
	    "LINUX_QUOTA_KERNEL=%s %s %s\n", identity.sysname, identity.release, identity.machine);
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
	require(config != NULL && fscanf(config, "%u %u", &phase, &project) == 2 &&
		fclose(config) == 0 && phase <= PHASE_ENFORCE && project <= 1U,
	    "read probe phase");
	/* Quota tracking starts with a writable mount of a QUOTA volume; the quota
	 * mount options also enforce limits. */
	require(mount("/dev/vda", "/mnt", "ext4", MS_NOATIME,
		    phase != PHASE_ENFORCE ? NULL
			: project	   ? "usrquota,grpquota,prjquota"
					   : "usrquota,grpquota") == 0,
	    "mount quota filesystem");
	if (phase == PHASE_ENFORCE) {
		enforce();
	}
	if (phase == PHASE_MUTATE) {
		mutate(project);
		require(umount("/mnt") == 0, "unmount after Linux changes");
		require(mount("/dev/vda", "/mnt", "ext4", MS_NOATIME, NULL) == 0,
		    "remount quota filesystem");
	}
	report(project ? QUOTA_TYPES : QUOTA_TYPES - 1U);
	require(umount("/mnt") == 0, "cleanly unmount quota filesystem");
	power_off(1);
	return 0;
}

/* SPDX-License-Identifier: BSD-3-Clause */
#ifndef MACHLIN_EXT4_TEST_LINUX_LARGE_VOLUME_H
#define MACHLIN_EXT4_TEST_LINUX_LARGE_VOLUME_H

static void
check_large_volumes(void)
{
	FILE *configuration;
	uint8_t value[300];
	unsigned long long cluster = 0;
	unsigned int block = 0;
	int verify_only = 0;
	int fd;
	int orphan;

	configuration = fopen("/large-volume-geometry", "r");
	require(configuration != NULL &&
		fscanf(configuration, "%u %llu %d", &block, &cluster, &verify_only) == 3 &&
		fclose(configuration) == 0 && block >= 1024 && block <= 4096 && cluster == 1048576,
	    "read high physical-address geometry");
	large_file_checks();
	xattr_check_all();
	if (verify_only) {
		require(umount("/mnt") == 0, "unmount verified high-address volume");
		puts("LINUX_EXT4_LARGE_VOLUME_RETURN_PASS");
		puts("LINUX_EXT4_XATTR_RETURN_PASS");
		power_off(1);
	}
	fd = open("/mnt/upper/created", O_RDWR | O_CLOEXEC);
	require(fd >= 0, "open core-authored high-address inode");
	large_file_write(fd, 4U * cluster + 7U, 'P', 32);
	require(fsync(fd) == 0 && close(fd) == 0, "commit Linux high-address overwrite");
	umask(0);
	fd = open("/mnt/upper/native", O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC, 0640);
	require(fd >= 0, "create Linux high-address file");
	large_file_write(fd, 0, 'n', 61);
	large_file_write(fd, 3U * cluster + 11U, 'v', 31);
	memset(value, 'm', sizeof(value));
	require(fsetxattr(fd, "user.native", value, sizeof(value), XATTR_CREATE) == 0 &&
		fsync(fd) == 0 && close(fd) == 0,
	    "commit Linux high-address data and external attribute");
	orphan = open("/mnt/upper/orphan", O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
	require(orphan >= 0, "create high-address Linux orphan");
	large_file_write(orphan, 2U * cluster + 13U, 'o', 31);
	require(fsetxattr(orphan, "user.native", value, sizeof(value), XATTR_CREATE) == 0 &&
		fsync(orphan) == 0 && unlink("/mnt/upper/orphan") == 0,
	    "commit high-address open-unlinked Linux inode");
	xattr_sync_path("/mnt/upper");
	puts("LINUX_EXT4_LARGE_VOLUME_PASS");
	puts("LINUX_EXT4_XATTR_PASS");
	puts("LINUX_EXT4_COMMITTED_RECOVERY_PENDING");
	power_off(1);
}

#endif

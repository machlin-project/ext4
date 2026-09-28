/* SPDX-License-Identifier: BSD-3-Clause */
#ifndef MACHLIN_EXT4_TEST_LINUX_CLUSTER_H
#define MACHLIN_EXT4_TEST_LINUX_CLUSTER_H

static void
check_clusters(void)
{
	FILE *configuration;
	uint8_t *bytes;
	unsigned int block = 0;
	unsigned int ratio = 0;
	size_t cluster;
	size_t size;
	size_t index;
	int fd;
	int orphan;

	configuration = fopen("/cluster-geometry", "r");
	require(configuration != NULL && fscanf(configuration, "%u %u", &block, &ratio) == 2 &&
		fclose(configuration) == 0 && block >= 1024 && block <= 4096 && ratio > 1 &&
		ratio <= 16,
	    "read Linux clustered fixture geometry");
	cluster = (size_t)block * ratio;
	size = 3U * cluster + 17U;
	bytes = malloc(size);
	require(bytes != NULL, "allocate Linux clustered data");
	for (index = 0; index < size; index++) {
		bytes[index] = (uint8_t)(index * 23U + 0x67U);
	}
	fd = open("/mnt/sparse", O_RDWR | O_CLOEXEC);
	require(fd >= 0 &&
		fallocate(fd, FALLOC_FL_KEEP_SIZE | FALLOC_FL_PUNCH_HOLE, (off_t)(3U * cluster),
		    (off_t)cluster) == 0 &&
		pwrite(fd, bytes, block, (off_t)(2U * cluster + 2U * block)) == block &&
		fsync(fd) == 0 && close(fd) == 0,
	    "Linux release and reuse clustered sparse mappings");
	umask(0);
	fd = open("/mnt/linux-cluster", O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
	require(fd >= 0 &&
		write(fd, bytes, 2U * cluster + block) == (ssize_t)(2U * cluster + block) &&
		fsetxattr(fd, "user.large", bytes, block / 2U, XATTR_CREATE) == 0 &&
		fallocate(fd, FALLOC_FL_KEEP_SIZE, (off_t)(5U * cluster + 1U), block) == 0 &&
		fsync(fd) == 0 && close(fd) == 0,
	    "Linux create data, attribute and unwritten clusters");
	require(mkdir("/mnt/linux-cluster-dir", 0750) == 0, "Linux create clustered directory");
	fd = open("/mnt/linux-cluster-dir/child", O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC, 0640);
	require(fd >= 0 && write(fd, bytes, 61) == 61 && fsync(fd) == 0 && close(fd) == 0,
	    "Linux create clustered directory child");
	orphan = open("/mnt/linux-cluster-orphan", O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
	require(orphan >= 0 && write(orphan, bytes, size) == (ssize_t)size &&
		fallocate(orphan, FALLOC_FL_KEEP_SIZE | FALLOC_FL_PUNCH_HOLE, (off_t)cluster,
		    (off_t)cluster) == 0 &&
		fsync(orphan) == 0 && unlink("/mnt/linux-cluster-orphan") == 0,
	    "leave committed Linux orphan with partial cluster ownership");
	xattr_sync_path("/mnt/linux-cluster-dir");
	xattr_sync_path("/mnt");
	free(bytes);
	puts("LINUX_EXT4_CLUSTER_PASS");
	puts("LINUX_EXT4_XATTR_PASS");
	puts("LINUX_EXT4_COMMITTED_RECOVERY_PENDING");
	/* Keep the fragmented orphan open until simulated power loss. */
	power_off(1);
}

#endif

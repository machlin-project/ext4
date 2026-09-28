/* SPDX-License-Identifier: BSD-3-Clause */
#include "storage.h"

/* Multi-mount protection against a modeled second host. Sleeps advance a virtual
 * clock and may let the other host change the MMP block; every MMP write by the
 * core is recorded with its sequence. */

#define MMP_SECONDS 1700004000
#define MMP_NODE "machlin-test"
#define MMP_DEVICE "test-device"
#define FOREIGN_NODE "other-host"
#define FOREIGN_SEQUENCE 0x00123456U
#define RANDOM_SEQUENCE 0x2468ace0U
#define HISTORY_LIMIT 64U

enum foreign_action { FOREIGN_NONE, FOREIGN_ADVANCE, FOREIGN_CLAIM };

struct host {
	struct device *device;
	struct ext4_mmp_environment environment;
	struct ext4_write_environment writer;
	uint64_t mmp_offset;
	uint32_t checksum_seed;
	bool checksum;
	int64_t clock;
	uint32_t sleeps;
	uint32_t slept;
	uint32_t random;
	uint32_t act_on_sleep;
	uint32_t fail_sleep;
	enum foreign_action action;
	uint32_t history[HISTORY_LIMIT];
	uint32_t history_count;
};

static struct ext4_mmp_disk *
disk_mmp(struct host *host, uint8_t *image)
{
	return (struct ext4_mmp_disk *)(image + host->mmp_offset);
}

static void
seal(struct host *host, struct ext4_mmp_disk *mmp)
{
	if (host->checksum) {
		ext4_encode32(&mmp->checksum,
		    ext4_crc32c(
			host->checksum_seed, mmp, offsetof(struct ext4_mmp_disk, checksum)));
	}
}

/* The other host writes straight to stable media, bypassing this owner. */
static void
foreign_write(struct host *host, uint32_t sequence)
{
	struct ext4_mmp_disk *mmp = disk_mmp(host, host->device->cache);

	ext4_encode32(&mmp->sequence, sequence);
	memset(mmp->node_name, 0, sizeof(mmp->node_name));
	memcpy(mmp->node_name, FOREIGN_NODE, strlen(FOREIGN_NODE));
	seal(host, mmp);
	memcpy(host->device->stable + host->mmp_offset, mmp, sizeof(*mmp));
}

static uint32_t
on_disk_sequence(struct host *host)
{
	return ext4_le32(&disk_mmp(host, host->device->cache)->sequence);
}

static enum ext4_result
host_sleep(void *context, uint32_t seconds)
{
	struct host *host = context;
	uint32_t sequence;

	host->sleeps++;
	if (host->sleeps == host->fail_sleep) {
		return EXT4_IO;
	}
	host->slept += seconds;
	host->clock += seconds;
	if (host->sleeps == host->act_on_sleep) {
		sequence = on_disk_sequence(host);
		foreign_write(
		    host, host->action == FOREIGN_ADVANCE ? sequence + 1U : FOREIGN_SEQUENCE + 7U);
	}
	return EXT4_OK;
}

static uint32_t
host_random(void *context)
{
	return ((struct host *)context)->random;
}

static int64_t
host_now(void *context)
{
	return ((struct host *)context)->clock;
}

static enum ext4_result
host_write(void *context, uint64_t offset, const void *buffer, size_t length)
{
	struct host *host = context;

	if (offset == host->mmp_offset) {
		CHECK(host->history_count < HISTORY_LIMIT);
		host->history[host->history_count++] =
		    ext4_le32(&((const struct ext4_mmp_disk *)buffer)->sequence);
	}
	return device_write(host->device, offset, buffer, length);
}

static enum ext4_result
host_flush(void *context)
{
	return device_flush(((struct host *)context)->device);
}

static void
host_reset(struct host *host, const uint8_t *image)
{
	device_reset(host->device, image);
	host->clock = MMP_SECONDS;
	host->sleeps = host->slept = host->act_on_sleep = host->fail_sleep = 0;
	host->action = FOREIGN_NONE;
	host->random = RANDOM_SEQUENCE;
	host->history_count = 0;
}

static uint32_t
expected_wait(struct host *host, uint32_t interval)
{
	uint32_t check = interval * EXT4_MMP_CHECK_MULTIPLIER;
	uint32_t recorded = ext4_le16(&disk_mmp(host, host->device->cache)->check_interval);

	if (recorded > check) {
		check = recorded;
	}
	if (check < EXT4_MMP_MIN_CHECK_INTERVAL) {
		check = EXT4_MMP_MIN_CHECK_INTERVAL;
	}
	if (check > EXT4_MMP_MAX_CHECK_INTERVAL) {
		check = EXT4_MMP_MAX_CHECK_INTERVAL;
	}
	return 2U * check + 1U < check + EXT4_MMP_WAIT_LIMIT ? 2U * check + 1U
							     : check + EXT4_MMP_WAIT_LIMIT;
}

static struct ext4_inode_update
creation(void)
{
	struct ext4_inode_update update = { 0 };

	update.fields = EXT4_ATTR_PERMISSIONS | EXT4_ATTR_UID | EXT4_ATTR_GID |
	    EXT4_ATTR_ACCESS_TIME | EXT4_ATTR_MODIFY_TIME | EXT4_ATTR_CHANGE_TIME |
	    EXT4_ATTR_XATTRS;
	update.permissions = 0644;
	update.access_time.seconds = MMP_SECONDS;
	update.modify_time.seconds = MMP_SECONDS;
	update.change_time.seconds = MMP_SECONDS;
	return update;
}

static enum ext4_result
create(struct ext4_fs *fs, const char *name, struct ext4_inode *result)
{
	struct ext4_inode_update update = creation();
	struct ext4_timestamp time = { MMP_SECONDS, 0 };
	struct ext4_inode root;
	enum ext4_result error;

	error = ext4_get_inode(fs, EXT4_ROOT_INODE, &root);
	if (error != EXT4_OK) {
		return error;
	}
	return ext4_create(fs, EXT4_ROOT_INODE, root.generation, (const uint8_t *)name,
	    strlen(name), &update, &time, result);
}

static struct ext4_fs *
mount_protected(struct host *host)
{
	struct ext4_fs *fs;

	EXPECT(ext4_mount_writable(&host->device->environment, &host->writer, &fs), EXT4_OK);
	return fs;
}

static void
owned_lifecycle(struct host *host, const uint8_t *image, uint32_t interval, const char *exports,
    const char *source)
{
	struct ext4_fs *fs;
	struct ext4_info info;
	struct ext4_inode inode;
	struct ext4_inode root;
	uint32_t wait;
	uint32_t sequence = RANDOM_SEQUENCE % (EXT4_MMP_SEQ_MAX + 1U);

	host_reset(host, image);
	wait = expected_wait(host, interval);
	fs = mount_protected(host);
	ext4_get_info(fs, &info);
	CHECK(info.mmp_interval == interval && host->sleeps == 1 && host->slept == wait);
	CHECK(host->history_count == 2 && host->history[0] == sequence &&
	    host->history[1] == sequence + 1U && on_disk_sequence(host) == sequence + 1U);
	CHECK(memcmp(disk_mmp(host, host->device->cache)->node_name, MMP_NODE, sizeof(MMP_NODE)) ==
	    0);
	/* A fresh sequence needs no refresh before the first mutation. */
	EXPECT(create(fs, "first", &inode), EXT4_OK);
	CHECK(host->history_count == 2);
	host->clock += interval;
	EXPECT(create(fs, "second", &inode), EXT4_OK);
	CHECK(host->history_count == 3 && host->history[2] == sequence + 2U);
	EXPECT(ext4_mmp_update(fs), EXT4_OK);
	CHECK(host->history_count == 4 && on_disk_sequence(host) == sequence + 3U);
	EXPECT(ext4_sync(fs), EXT4_OK);
	EXPECT(ext4_mmp_release(fs), EXT4_OK);
	CHECK(on_disk_sequence(host) == EXT4_MMP_SEQ_CLEAN && host->history_count == 5);
	EXPECT(create(fs, "late", &inode), EXT4_READ_ONLY);
	EXPECT(ext4_mmp_update(fs), EXT4_READ_ONLY);
	EXPECT(ext4_sync(fs), EXT4_READ_ONLY);
	CHECK(host->history_count == 5);
	ext4_unmount(fs);
	CHECK(host->device->live == 0);
	EXPECT(ext4_mount(&host->device->environment, &fs), EXT4_OK);
	EXPECT(ext4_get_inode(fs, EXT4_ROOT_INODE, &root), EXT4_OK);
	EXPECT(ext4_lookup(fs, &root, (const uint8_t *)"first", 5, &inode), EXT4_OK);
	EXPECT(ext4_lookup(fs, &root, (const uint8_t *)"second", 6, &inode), EXT4_OK);
	ext4_unmount(fs);
	memcpy(host->device->stable, host->device->cache, host->device->size);
	storage_export(host->device, exports, source, "mmp-released-");
	puts("PASS clean acquisition, refresh, guard and release");
}

static void
wrap(struct host *host, const uint8_t *image)
{
	struct ext4_fs *fs;

	host_reset(host, image);
	host->random = EXT4_MMP_SEQ_MAX;
	fs = mount_protected(host);
	CHECK(host->history[0] == EXT4_MMP_SEQ_MAX && on_disk_sequence(host) == 1U);
	EXPECT(ext4_mmp_update(fs), EXT4_OK);
	CHECK(on_disk_sequence(host) == 2U);
	EXPECT(ext4_mmp_release(fs), EXT4_OK);
	ext4_unmount(fs);
	puts("PASS sequence wraps after the largest active value");
}

static void
contention(struct host *host, const uint8_t *image, uint32_t interval)
{
	struct ext4_fs *fs;
	struct ext4_inode inode;
	struct ext4_mmp_disk *mmp;
	uint32_t writes;

	/* An offline checker owns the device; no wait and no write. */
	host_reset(host, image);
	foreign_write(host, EXT4_MMP_SEQ_FSCK);
	EXPECT(ext4_mount_writable(&host->device->environment, &host->writer, &fs), EXT4_BUSY);
	CHECK(host->sleeps == 0 && host->device->writes == 0 && host->device->live == 0);

	/* A stale owner stopped updating; both waits pass. */
	host_reset(host, image);
	foreign_write(host, FOREIGN_SEQUENCE);
	fs = mount_protected(host);
	CHECK(host->sleeps == 2 && host->slept == 2U * expected_wait(host, interval));
	EXPECT(ext4_mmp_release(fs), EXT4_OK);
	ext4_unmount(fs);

	/* An active owner advances during the first wait. */
	host_reset(host, image);
	foreign_write(host, FOREIGN_SEQUENCE);
	host->action = FOREIGN_ADVANCE;
	host->act_on_sleep = 1;
	EXPECT(ext4_mount_writable(&host->device->environment, &host->writer, &fs), EXT4_BUSY);
	CHECK(host->sleeps == 1 && host->device->writes == 0 && host->device->live == 0);

	/* Another host claims the device during this owner's confirmation wait. */
	host_reset(host, image);
	host->action = FOREIGN_CLAIM;
	host->act_on_sleep = 1;
	EXPECT(ext4_mount_writable(&host->device->environment, &host->writer, &fs), EXT4_BUSY);
	CHECK(host->sleeps == 1 && host->history_count == 1 && host->device->writes == 1 &&
	    on_disk_sequence(host) == FOREIGN_SEQUENCE + 7U && host->device->live == 0);

	/* An interrupted wait fails the mount after publishing only the sequence. */
	host_reset(host, image);
	host->fail_sleep = 1;
	EXPECT(ext4_mount_writable(&host->device->environment, &host->writer, &fs), EXT4_IO);
	CHECK(host->device->writes == 1 && host->device->live == 0);

	/* A heartbeat that finds another owner poisons the instance without writes. */
	host_reset(host, image);
	fs = mount_protected(host);
	foreign_write(host, FOREIGN_SEQUENCE);
	writes = host->device->writes;
	EXPECT(ext4_mmp_update(fs), EXT4_BUSY);
	CHECK(fs->aborted && host->device->writes == writes);
	CHECK(create(fs, "after", &inode) != EXT4_OK && host->device->writes == writes);
	/* A poisoned owner cannot inspect the device again or publish CLEAN. */
	EXPECT(ext4_mmp_release(fs), EXT4_RECOVERY_REQUIRED);
	CHECK(host->device->writes == writes && on_disk_sequence(host) == FOREIGN_SEQUENCE);
	ext4_unmount(fs);

	/* A stale owner's first mutation checks the device before any write. */
	host_reset(host, image);
	fs = mount_protected(host);
	host->clock += interval;
	foreign_write(host, FOREIGN_SEQUENCE);
	writes = host->device->writes;
	EXPECT(create(fs, "after", &inode), EXT4_BUSY);
	CHECK(fs->aborted && host->device->writes == writes);
	EXPECT(ext4_sync(fs), EXT4_RECOVERY_REQUIRED);
	CHECK(host->device->writes == writes);
	ext4_unmount(fs);

	/* The largest recorded check interval bounds the wait. */
	host_reset(host, image);
	mmp = disk_mmp(host, host->device->cache);
	ext4_encode16(&mmp->check_interval, 1000);
	seal(host, mmp);
	memcpy(host->device->stable + host->mmp_offset, mmp, sizeof(*mmp));
	fs = mount_protected(host);
	CHECK(host->slept == EXT4_MMP_MAX_CHECK_INTERVAL + EXT4_MMP_WAIT_LIMIT);
	EXPECT(ext4_mmp_release(fs), EXT4_OK);
	ext4_unmount(fs);
	puts("PASS checker, stale, active, contested, interrupted and stolen ownership");
}

static void
refusals(struct host *host, const uint8_t *image)
{
	struct ext4_write_environment writer = host->writer;
	struct ext4_mmp_disk *mmp;
	struct ext4_fs *fs;

	host_reset(host, image);
	writer.mmp = NULL;
	EXPECT(ext4_mount_writable(&host->device->environment, &writer, &fs), EXT4_UNSUPPORTED);
	CHECK(host->device->writes == 0 && host->device->live == 0);
	EXPECT(ext4_mount(&host->device->environment, &fs), EXT4_OK);
	ext4_unmount(fs);

	host_reset(host, image);
	mmp = disk_mmp(host, host->device->cache);
	ext4_encode32(&mmp->magic, EXT4_MMP_MAGIC + 1U);
	seal(host, mmp);
	EXPECT(ext4_mount_writable(&host->device->environment, &host->writer, &fs), EXT4_CORRUPT);
	CHECK(host->device->writes == 0 && host->sleeps == 0 && host->device->live == 0);
	/* Read-only mounts neither read nor enforce the MMP block. */
	EXPECT(ext4_mount(&host->device->environment, &fs), EXT4_OK);
	ext4_unmount(fs);
	if (host->checksum) {
		host_reset(host, image);
		mmp = disk_mmp(host, host->device->cache);
		mmp->node_name[0] ^= 1;
		EXPECT(ext4_mount_writable(&host->device->environment, &host->writer, &fs),
		    EXT4_CORRUPT);
		CHECK(host->device->writes == 0 && host->device->live == 0);
	}
	puts("PASS missing services and malformed MMP blocks reject before writes");
}

static void
recovery(struct host *host, const uint8_t *image, const char *exports, const char *source)
{
	struct ext4_write_environment writer = host->writer;
	struct ext4_recovery_report report;
	struct ext4_inode inode;
	struct ext4_inode root;
	struct ext4_fs *fs;
	uint32_t commit;

	/* Leave a durable, uncheckpointed create for offline recovery. */
	host_reset(host, image);
	fs = mount_protected(host);
	host->device->survival = 1;
	EXPECT(create(fs, "recovered", &inode), EXT4_OK);
	commit = host->device->commit_barrier;
	ext4_unmount(fs);
	CHECK(commit != 0);
	host_reset(host, image);
	fs = mount_protected(host);
	/* Stop at the first home write after the durable commit barrier. */
	host->device->stop_at = commit + 1U;
	host->device->survival = 1;
	CHECK(create(fs, "recovered", &inode) == EXT4_IO && host->device->intent_durable);
	ext4_unmount(fs);
	device_reset(host->device, host->device->stable);
	memcpy(host->device->base, host->device->cache, host->device->size);
	storage_export(host->device, exports, source, "mmp-pending-");

	writer.mmp = NULL;
	EXPECT(ext4_recover(&host->device->environment, &writer, &report), EXT4_UNSUPPORTED);
	CHECK(host->device->writes == 0 && host->device->live == 0);
	host_reset(host, host->device->base);
	foreign_write(host, EXT4_MMP_SEQ_FSCK);
	EXPECT(ext4_recover(&host->device->environment, &host->writer, &report), EXT4_BUSY);
	CHECK(host->device->writes == 0 && host->device->live == 0);

	host_reset(host, host->device->base);
	EXPECT(ext4_recover(&host->device->environment, &host->writer, &report), EXT4_OK);
	CHECK(report.transactions == 1 && host->history_count == 3 &&
	    host->history[1] == EXT4_MMP_SEQ_FSCK && host->history[2] == EXT4_MMP_SEQ_CLEAN);
	EXPECT(ext4_mount(&host->device->environment, &fs), EXT4_OK);
	EXPECT(ext4_get_inode(fs, EXT4_ROOT_INODE, &root), EXT4_OK);
	EXPECT(ext4_lookup(fs, &root, (const uint8_t *)"recovered", 9, &inode), EXT4_OK);
	ext4_unmount(fs);
	puts("PASS offline recovery holds the checker sequence and releases it clean");
}

/* Acquire a volume another host released with the POSIX adapter's wall-clock
 * sleeps, entropy and host name, write one file, then release it clean. */
static void
continuation(const char *path)
{
	struct ext4_posix_image image;
	struct ext4_inode_update update = { 0 };
	struct ext4_timestamp now = { MMP_SECONDS, 0 };
	struct ext4_inode root;
	struct ext4_inode inode;
	struct ext4_fs *fs;

	update.fields = EXT4_ATTR_PERMISSIONS | EXT4_ATTR_UID | EXT4_ATTR_GID |
	    EXT4_ATTR_ACCESS_TIME | EXT4_ATTR_MODIFY_TIME | EXT4_ATTR_CHANGE_TIME |
	    EXT4_ATTR_XATTRS;
	update.permissions = 0644;
	update.access_time = now;
	update.modify_time = now;
	update.change_time = now;
	EXPECT(ext4_posix_open_writable(&image, path), EXT4_OK);
	EXPECT(ext4_mount_writable(&image.environment, &image.writer, &fs), EXT4_OK);
	EXPECT(ext4_get_inode(fs, EXT4_ROOT_INODE, &root), EXT4_OK);
	EXPECT(ext4_lookup(fs, &root, (const uint8_t *)"linux-mmp", 9, &inode), EXT4_OK);
	EXPECT(ext4_create(fs, root.number, root.generation, (const uint8_t *)"core-after-linux",
		   16, &update, &now, &inode),
	    EXT4_OK);
	EXPECT(ext4_sync(fs), EXT4_OK);
	EXPECT(ext4_mmp_release(fs), EXT4_OK);
	ext4_unmount(fs);
	CHECK(image.live_allocations == 0);
	ext4_posix_close(&image);
	printf("PASS multi-mount protection acquired after Linux by %s\n", image.node_name);
}

int
main(int argc, char **argv)
{
	static struct device device;
	struct host host;
	struct ext4_fs *fs;
	uint8_t *image;
	uint32_t interval;
	const char *exports = argc == 3 ? argv[2] : NULL;

	if (argc == 3 && strcmp(argv[1], "--continue") == 0) {
		continuation(argv[2]);
		return 0;
	}
	if (argc != 2 && argc != 3) {
		fprintf(stderr, "usage: %s [--continue] MMP_IMAGE [EXPORT_DIRECTORY]\n", argv[0]);
		return 2;
	}
	storage_open(&device, argv[1]);
	memset(&host, 0, sizeof(host));
	host.device = &device;
	EXPECT(ext4_mount(&device.environment, &fs), EXT4_OK);
	CHECK(fs->info.feature_incompat & EXT4_FEATURE_INCOMPAT_MMP);
	host.mmp_offset = fs->mmp_block * fs->info.block_size;
	host.checksum = fs->metadata_checksum;
	host.checksum_seed = fs->checksum_seed;
	interval = fs->mmp_interval;
	ext4_unmount(fs);
	host.environment = (struct ext4_mmp_environment){ &host, host_sleep, host_random, host_now,
		MMP_NODE, MMP_DEVICE };
	host.writer =
	    (struct ext4_write_environment){ &host, host_write, host_flush, &host.environment };
	image = malloc(device.size);
	CHECK(image != NULL);
	memcpy(image, device.base, device.size);
	owned_lifecycle(&host, image, interval, exports, argv[1]);
	wrap(&host, image);
	contention(&host, image, interval);
	refusals(&host, image);
	recovery(&host, image, exports, argv[1]);
	free(image);
	storage_close(&device);
	return 0;
}

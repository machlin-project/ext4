/* SPDX-License-Identifier: BSD-3-Clause */
#ifndef MACHLIN_EXT4_TEST_XATTR_RELEASE_FAULTS_H
#define MACHLIN_EXT4_TEST_XATTR_RELEASE_FAULTS_H

enum release_kind {
	RELEASE_SHARED,
	RELEASE_FAST,
	RELEASE_MAPPED,
	RELEASE_DIRECTORY,
	RELEASE_LARGE,
	RELEASE_KIND_COUNT
};

static const char *const release_names[] = { "shared", "fast", "mapped", "directory", "large" };
static const char *const release_paths[] = { "block", "symlink", "mapped-symlink", "directory",
	"block" };

static enum ext4_result
release_attempt(struct device *device, enum release_kind kind, unsigned int fault, uint32_t point,
    unsigned int survival, bool partial, struct lifetime_trace *trace, uint8_t *pending)
{
	struct ext4_fs *fs;
	struct ext4_inode root;
	struct ext4_inode inode;
	struct ext4_inode other;
	struct ext4_inode result;
	struct ext4_inode_hold *hold;
	struct ext4_inode_hold *duplicate;
	struct ext4_inode_hold *other_hold;
	uint64_t attribute;
	uint64_t released_blocks;
	uint64_t free_blocks;
	uint32_t free_inodes;
	uint32_t live;
	uint32_t allocations;
	uint32_t reads;
	uint32_t events;
	enum ext4_result error;

	fs = mount_writer(device, &root);
	inode = lookup(fs, &root, release_paths[kind]);
	other = lookup(fs, &root, "plain");
	CHECK(other.blocks_512 == 0);
	attribute = attribute_block(device, fs, &inode);
	released_blocks = inode.blocks_512 / (device->block_size / EXT4_SECTOR_SIZE);
	if (attribute != 0 && references(device, attribute) > 1) {
		released_blocks--;
	}
	free_blocks = fs->info.free_blocks;
	free_inodes = fs->info.free_inodes;
	EXPECT(ext4_hold_inode(fs, inode.number, inode.generation, &hold), EXT4_OK);
	EXPECT(ext4_hold_inode(fs, inode.number, inode.generation, &duplicate), EXT4_OK);
	EXPECT(ext4_hold_inode(fs, other.number, other.generation, &other_hold), EXT4_OK);
	CHECK(duplicate == hold && hold->references == 2);
	remove_inode(fs, &root, release_paths[kind], &inode);
	remove_inode(fs, &root, "plain", &other);
	CHECK(fs->last_orphan == other.number && hold->unlinked && other_hold->unlinked);
	/* Namespace deletion already committed. A crash during either final release
	 * must finish both deletions even if no cleanup transaction has committed. */
	if (pending != NULL) {
		memcpy(pending, device->stable, device->size);
	}
	events = device->events;
	EXPECT(ext4_release_inode(duplicate), EXT4_OK);
	CHECK(hold->references == 1 && device->events == events);
	live = device->live;
	allocations = device->allocations;
	reads = device->reads;
	device->commit_written = false;
	device->intent_durable = false;
	device->commit_barrier = 0;
	device->survival = survival;
	device->partial = partial;
	if (fault == 1) {
		device->fail_allocation = allocations + point;
	} else if (fault == 2) {
		device->fail_read = reads + point;
	} else if (fault == 3) {
		device->stop_at = events + point;
	}
	error = ext4_release_inode(hold);
	trace->allocations = device->allocations - allocations;
	trace->reads = device->reads - reads;
	trace->events = device->events - events;
	trace->commit_event = device->commit_barrier == 0 ? 0 : device->commit_barrier - events;
	trace->committed = device->intent_durable;
	/* The last reference is consumed even when cleanup cannot finish. No caller
	 * may retry a freed hold, and an aborted owner must reject future reads. */
	CHECK(fs->hold_count == 1 && device->live + 1 == live && fs->holds == other_hold);
	device->fail_allocation = device->fail_read = device->stop_at = 0;
	if (error != EXT4_OK) {
		CHECK(fs->aborted);
		EXPECT(ext4_get_inode(fs, EXT4_ROOT_INODE, &result), EXT4_RECOVERY_REQUIRED);
		EXPECT(ext4_release_inode(other_hold), EXT4_RECOVERY_REQUIRED);
	} else {
		CHECK(!fs->aborted && fs->last_orphan == other.number &&
		    fs->info.free_inodes == free_inodes + 1 &&
		    fs->info.free_blocks == free_blocks + released_blocks);
		EXPECT(ext4_release_inode(other_hold), EXT4_OK);
		CHECK(fs->last_orphan == 0 && fs->info.free_inodes == free_inodes + 2);
		EXPECT(ext4_sync(fs), EXT4_OK);
	}
	CHECK(fs->hold_count == 0);
	ext4_unmount(fs);
	CHECK(device->live == 0);
	return error;
}

static void
release_faults(struct device *device, enum release_kind kind, bool smoke, const char *exports,
    const char *path)
{
	struct lifetime_trace baseline;
	struct lifetime_trace trace;
	uint8_t *original = device->base;
	uint8_t *seed = malloc(device->size);
	uint8_t *expected = malloc(device->size);
	uint8_t *pending = malloc(device->size);
	uint32_t point;
	uint32_t limit;
	uint32_t recovered = 0;
	uint32_t torn = 0;
	unsigned int fault;
	unsigned int survival;
	unsigned int partial;
	char prefix[80];

	CHECK(seed != NULL && expected != NULL && pending != NULL);
	printf("RUN xattr release faults kind=%s\n", release_names[kind]);
	CHECK(fflush(stdout) == 0);
	lifetime_seed(device, kind == RELEASE_LARGE ? LIFETIME_TRUNCATE : LIFETIME_WRITE, seed);
	device->base = seed;
	device_reset(device, seed);
	EXPECT(release_attempt(device, kind, 0, 0, 0, false, &baseline, pending), EXT4_OK);
	memcpy(expected, device->stable, device->size);
	CHECK(snprintf(prefix, sizeof(prefix), "xattr-release-%s-", release_names[kind]) > 0);
	storage_export(device, exports, path, prefix);
	CHECK(storage_recover(device, expected, true) && device->writes == 0);
	if (!smoke) {
		for (fault = 1; fault <= 2; fault++) {
			limit = fault == 1 ? baseline.allocations : baseline.reads;
			for (point = 1; point <= limit; point++) {
				device_reset(device, seed);
				EXPECT(release_attempt(
					   device, kind, fault, point, 0, false, &trace, NULL),
				    fault == 1 ? EXT4_NO_MEMORY : EXT4_IO);
				CHECK(storage_recover(device, expected, true));
			}
		}
		for (point = 1; point <= baseline.events; point++) {
			for (survival = 0; survival < 3; survival++) {
				for (partial = 0; partial < 2; partial++) {
					device_reset(device, seed);
					EXPECT(release_attempt(device, kind, 3, point, survival,
						   partial != 0, &trace, NULL),
					    EXT4_IO);
					CHECK(device->off);
					if (storage_recover(device, expected, true)) {
						recovered++;
					} else {
						torn++;
					}
				}
			}
		}
		printf("PASS xattr release faults kind=%s allocations=%u reads=%u cuts=%u "
		       "recovered=%u torn_super_fail_closed=%u\n",
		    release_names[kind], baseline.allocations, baseline.reads, baseline.events * 6,
		    recovered, torn);
	}
	if (exports != NULL) {
		device_reset(device, pending);
		CHECK(snprintf(prefix, sizeof(prefix), "xattr-release-pending-%s-",
			  release_names[kind]) > 0);
		storage_export(device, exports, path, prefix);
		CHECK(storage_recover(device, expected, true));
		CHECK(baseline.commit_event > 1 && baseline.commit_event < baseline.events);
		for (fault = 0; fault < 2; fault++) {
			device_reset(device, seed);
			point = fault == 0 ? baseline.commit_event - 1 : baseline.commit_event + 1;
			EXPECT(release_attempt(device, kind, 3, point, 0, false, &trace, NULL),
			    EXT4_IO);
			CHECK(trace.committed == (fault != 0));
			CHECK(
			    snprintf(prefix, sizeof(prefix), "xattr-release-%s-%s-",
				fault == 0 ? "uncommitted" : "committed", release_names[kind]) > 0);
			storage_export(device, exports, path, prefix);
			CHECK(storage_recover(device, expected, true));
		}
	}
	device->base = original;
	device_reset(device, original);
	free(pending);
	free(expected);
	free(seed);
}

#endif

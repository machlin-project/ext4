/* SPDX-License-Identifier: BSD-3-Clause */
#ifndef MACHLIN_EXT4_TEST_XATTR_LIFETIME_FAULTS_H
#define MACHLIN_EXT4_TEST_XATTR_LIFETIME_FAULTS_H

enum lifetime_operation {
	LIFETIME_CREATE_FILE,
	LIFETIME_CREATE_DIRECTORY,
	LIFETIME_CREATE_FAST,
	LIFETIME_CREATE_MAPPED,
	LIFETIME_WRITE,
	LIFETIME_TRUNCATE,
	LIFETIME_UNLINK_SHARED,
	LIFETIME_UNLINK_FAST,
	LIFETIME_UNLINK_MAPPED,
	LIFETIME_RMDIR,
	LIFETIME_RENAME,
	LIFETIME_OPERATION_COUNT
};

static const char *const lifetime_operation_names[] = { "create-file", "create-directory",
	"create-fast", "create-mapped", "write", "truncate", "unlink-shared", "unlink-fast",
	"unlink-mapped", "rmdir", "rename" };

struct lifetime_trace {
	uint32_t allocations;
	uint32_t reads;
	uint32_t events;
	uint32_t commit_event;
	bool committed;
};

static void
lifetime_seed(struct device *device, enum lifetime_operation operation, uint8_t *seed)
{
	struct ext4_fs *fs;
	struct ext4_inode root;
	struct ext4_inode inode;
	struct ext4_inode_update update = attributes(false, NULL, 0);
	uint8_t *bytes;
	uint32_t block;
	size_t completed;

	device_reset(device, device->base);
	if (operation == LIFETIME_TRUNCATE) {
		bytes = malloc(device->block_size);
		CHECK(bytes != NULL);
		memset(bytes, 0x5a, device->block_size);
		fs = mount_writer(device, &root);
		inode = lookup(fs, &root, "block");
		for (block = 0; block < LIFETIME_DATA_BLOCKS; block++) {
			EXPECT(ext4_write(fs, inode.number, inode.generation,
				   (uint64_t)block * device->block_size, bytes, device->block_size,
				   &update, &completed),
			    EXT4_OK);
			CHECK(completed == device->block_size);
		}
		EXPECT(ext4_sync(fs), EXT4_OK);
		ext4_unmount(fs);
		free(bytes);
	}
	memcpy(seed, device->stable, device->size);
}

static enum ext4_result
lifetime_attempt(struct device *device, enum lifetime_operation operation, unsigned int fault,
    uint32_t point, unsigned int survival, bool partial, struct lifetime_trace *trace)
{
	struct ext4_fs *fs;
	struct ext4_inode root;
	struct ext4_inode inode;
	struct ext4_inode victim;
	struct ext4_inode parent;
	struct ext4_inode result;
	struct ext4_inode untouched;
	struct ext4_rename_entry from;
	struct ext4_rename_entry to;
	struct ext4_xattr_change attribute;
	struct ext4_inode_update update;
	uint8_t bytes[XATTR_CHANGED_BYTES];
	uint8_t observed[CHILD_FAST_BYTES];
	uint64_t free_blocks;
	uint32_t free_inodes;
	uint32_t live;
	uint32_t allocations;
	uint32_t reads;
	uint32_t events;
	size_t completed = SIZE_MAX;
	const char *name = "block";
	enum ext4_result error;

	fs = mount_writer(device, &root);
	if (operation == LIFETIME_UNLINK_FAST) {
		name = "symlink";
	} else if (operation == LIFETIME_UNLINK_MAPPED) {
		name = "mapped-symlink";
	} else if (operation == LIFETIME_RMDIR) {
		name = "directory";
	}
	inode = lookup(fs, &root, name);
	parent = lookup(fs, &root, "directory");
	victim = lookup(fs, &root, "symlink");
	from = rename_entry(&root, "block", &inode);
	to = rename_entry(&root, "symlink", &victim);
	pattern(bytes, sizeof(bytes), true);
	attribute =
	    change(operation <= LIFETIME_CREATE_MAPPED ? EXT4_XATTR_CREATE : EXT4_XATTR_REPLACE,
		EXT4_XATTR_USER, "binary", bytes, sizeof(bytes));
	update = attributes(operation <= LIFETIME_CREATE_MAPPED, &attribute, 1);
	free_blocks = fs->info.free_blocks;
	free_inodes = fs->info.free_inodes;
	live = device->live;
	allocations = device->allocations;
	reads = device->reads;
	events = device->events;
	memset(&untouched, 0xa5, sizeof(untouched));
	result = untouched;
	device->survival = survival;
	device->partial = partial;
	if (fault == 1) {
		device->fail_allocation = allocations + point;
	} else if (fault == 2) {
		device->fail_read = reads + point;
	} else if (fault == 3) {
		device->stop_at = events + point;
	}
	if (operation <= LIFETIME_CREATE_MAPPED) {
		error = create_child(fs, &parent, (enum child_kind)operation, &update, &result);
	} else if (operation == LIFETIME_WRITE) {
		error = ext4_write(fs, inode.number, inode.generation, device->block_size + 7,
		    bytes, sizeof(observed), &update, &completed);
		CHECK(completed == (error == EXT4_OK ? sizeof(observed) : 0));
	} else if (operation == LIFETIME_TRUNCATE) {
		error = ext4_truncate(fs, inode.number, inode.generation, 17, &update, &result);
	} else if (operation == LIFETIME_RENAME) {
		error = ext4_rename(fs, &from, &to, 0, &update.change_time, &result);
	} else if (operation == LIFETIME_RMDIR) {
		error = ext4_rmdir(fs, root.number, root.generation, (const uint8_t *)name,
		    strlen(name), inode.number, inode.generation, &update.change_time, &result);
	} else {
		error = ext4_unlink(fs, root.number, root.generation, (const uint8_t *)name,
		    strlen(name), inode.number, inode.generation, &update.change_time, &result);
	}
	if (error != EXT4_OK) {
		CHECK(memcmp(&result, &untouched, sizeof(result)) == 0 && device->live == live);
		if (device->writes == 0) {
			/* A journal read error can poison a commit before its first write.
			 * Private snapshots and allocation totals must still be unchanged. */
			CHECK(fs->info.free_blocks == free_blocks &&
			    fs->info.free_inodes == free_inodes &&
			    memcmp(device->cache, device->base, device->size) == 0 &&
			    memcmp(device->stable, device->base, device->size) == 0);
		}
	} else {
		error = ext4_sync(fs);
	}
	trace->allocations = device->allocations - allocations;
	trace->reads = device->reads - reads;
	trace->events = device->events - events;
	trace->commit_event = device->commit_barrier - events;
	trace->committed = device->intent_durable;
	if (error == EXT4_OK) {
		if (operation <= LIFETIME_CREATE_MAPPED) {
			value_is(fs, &result, EXT4_XATTR_USER, "binary", bytes, sizeof(bytes));
		} else if (operation == LIFETIME_WRITE || operation == LIFETIME_TRUNCATE) {
			EXPECT(ext4_get_inode(fs, inode.number, &result), EXT4_OK);
			value_is(fs, &result, EXT4_XATTR_USER, "binary", bytes, sizeof(bytes));
			if (operation == LIFETIME_WRITE) {
				EXPECT(ext4_read(fs, &result, device->block_size + 7, observed,
					   sizeof(observed), &completed),
				    EXT4_OK);
				CHECK(completed == sizeof(observed) &&
				    memcmp(bytes, observed, completed) == 0);
			} else {
				CHECK(result.size == 17 && fs->last_orphan == 0);
			}
		}
	} else {
		CHECK(device->writes == 0 || fs->aborted);
		if (fs->aborted) {
			EXPECT(ext4_get_inode(fs, inode.number, &result), EXT4_RECOVERY_REQUIRED);
		}
	}
	ext4_unmount(fs);
	CHECK(device->live == 0);
	return error;
}

static void
lifetime_faults(struct device *device, enum lifetime_operation operation, bool smoke,
    const char *exports, const char *path)
{
	struct lifetime_trace baseline;
	struct lifetime_trace trace;
	uint8_t *original = device->base;
	uint8_t *seed = malloc(device->size);
	uint8_t *expected = malloc(device->size);
	uint32_t point;
	uint32_t limit;
	uint32_t recovered = 0;
	uint32_t torn = 0;
	unsigned int fault;
	unsigned int survival;
	unsigned int partial;
	char prefix[80];

	CHECK(seed != NULL && expected != NULL);
	printf("RUN xattr lifetime faults operation=%s\n", lifetime_operation_names[operation]);
	CHECK(fflush(stdout) == 0);
	lifetime_seed(device, operation, seed);
	device->base = seed;
	device_reset(device, seed);
	EXPECT(lifetime_attempt(device, operation, 0, 0, 0, false, &baseline), EXT4_OK);
	memcpy(expected, device->stable, device->size);
	CHECK(snprintf(prefix, sizeof(prefix), "xattr-lifetime-%s-",
		  lifetime_operation_names[operation]) > 0);
	storage_export(device, exports, path, prefix);
	CHECK(storage_recover(device, expected, true) && device->writes == 0);
	if (!smoke) {
		for (fault = 1; fault <= 2; fault++) {
			limit = fault == 1 ? baseline.allocations : baseline.reads;
			for (point = 1; point <= limit; point++) {
				device_reset(device, seed);
				EXPECT(lifetime_attempt(
					   device, operation, fault, point, 0, false, &trace),
				    fault == 1 ? EXT4_NO_MEMORY : EXT4_IO);
				CHECK(storage_recover(device, expected, trace.committed));
			}
		}
		for (point = 1; point <= baseline.events; point++) {
			for (survival = 0; survival < 3; survival++) {
				for (partial = 0; partial < 2; partial++) {
					device_reset(device, seed);
					EXPECT(lifetime_attempt(device, operation, 3, point,
						   survival, partial != 0, &trace),
					    EXT4_IO);
					CHECK(device->off);
					if (storage_recover(device, expected, trace.committed)) {
						recovered++;
					} else {
						torn++;
					}
				}
			}
		}
		printf("PASS xattr lifetime faults operation=%s allocations=%u reads=%u cuts=%u "
		       "recovered=%u torn_super_fail_closed=%u\n",
		    lifetime_operation_names[operation], baseline.allocations, baseline.reads,
		    baseline.events * 6, recovered, torn);
	}
	if (exports != NULL) {
		device_reset(device, seed);
		CHECK(snprintf(prefix, sizeof(prefix), "xattr-lifetime-before-%s-",
			  lifetime_operation_names[operation]) > 0);
		storage_export(device, exports, path, prefix);
		CHECK(baseline.commit_event > 1 && baseline.commit_event < baseline.events);
		device_reset(device, seed);
		EXPECT(lifetime_attempt(
			   device, operation, 3, baseline.commit_event + 1, 0, false, &trace),
		    EXT4_IO);
		CHECK(trace.committed);
		CHECK(snprintf(prefix, sizeof(prefix), "xattr-lifetime-pending-%s-",
			  lifetime_operation_names[operation]) > 0);
		storage_export(device, exports, path, prefix);
		CHECK(storage_recover(device, expected, true));
		device_reset(device, seed);
		EXPECT(lifetime_attempt(
			   device, operation, 3, baseline.commit_event - 1, 0, false, &trace),
		    EXT4_IO);
		CHECK(!trace.committed);
		CHECK(snprintf(prefix, sizeof(prefix), "xattr-lifetime-uncommitted-%s-",
			  lifetime_operation_names[operation]) > 0);
		storage_export(device, exports, path, prefix);
		CHECK(storage_recover(device, expected, false));
	}
	device->base = original;
	device_reset(device, original);
	free(expected);
	free(seed);
}

#endif

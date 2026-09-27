/* SPDX-License-Identifier: BSD-3-Clause */
#ifndef MACHLIN_EXT4_TEST_XATTR_ENABLE_FAULTS_H
#define MACHLIN_EXT4_TEST_XATTR_ENABLE_FAULTS_H

#define ENABLE_BODY_BYTES 3U

enum enable_operation {
	ENABLE_CREATE_FILE,
	ENABLE_CREATE_DIRECTORY,
	ENABLE_CREATE_FAST,
	ENABLE_CREATE_MAPPED,
	ENABLE_SET_BODY,
	ENABLE_SET_EXTERNAL,
	ENABLE_WRITE,
	ENABLE_TRUNCATE,
	ENABLE_OPERATION_COUNT
};

static const char *const enable_names[] = { "create-file", "create-directory", "create-fast",
	"create-mapped", "set-body", "set-external", "write", "truncate" };

static enum ext4_result
enable_attempt(struct device *device, enum enable_operation operation, unsigned int fault,
    uint32_t point, unsigned int survival, bool partial, struct lifetime_trace *trace)
{
	struct ext4_fs *fs;
	struct ext4_inode root;
	struct ext4_inode parent;
	struct ext4_inode inode;
	struct ext4_inode result;
	struct ext4_inode untouched;
	struct ext4_inode_update update;
	struct ext4_xattr_change attribute;
	uint8_t bytes[XATTR_CHANGED_BYTES];
	uint64_t free_blocks;
	uint32_t free_inodes;
	uint32_t feature_compat;
	uint32_t allocations;
	uint32_t reads;
	uint32_t events;
	uint32_t live;
	size_t length = operation == ENABLE_SET_BODY ? ENABLE_BODY_BYTES : sizeof(bytes);
	size_t completed = SIZE_MAX;
	enum ext4_result error;

	fs = mount_writer(device, &root);
	parent = lookup(fs, &root, "directory");
	inode = lookup(fs, &root, "plain");
	feature_compat = fs->info.feature_compat;
	CHECK(!(feature_compat & EXT4_FEATURE_COMPAT_EXT_ATTR));
	pattern(bytes, sizeof(bytes), true);
	attribute = change(EXT4_XATTR_CREATE, EXT4_XATTR_USER, "binary", bytes, length);
	update = attributes(operation <= ENABLE_CREATE_MAPPED, &attribute, 1);
	memset(&untouched, 0xa5, sizeof(untouched));
	result = untouched;
	free_blocks = fs->info.free_blocks;
	free_inodes = fs->info.free_inodes;
	allocations = device->allocations;
	reads = device->reads;
	events = device->events;
	live = device->live;
	device->survival = survival;
	device->partial = partial;
	if (fault == 1) {
		device->fail_allocation = allocations + point;
	} else if (fault == 2) {
		device->fail_read = reads + point;
	} else if (fault == 3) {
		device->stop_at = events + point;
	}
	if (operation <= ENABLE_CREATE_MAPPED) {
		error = create_child(fs, &parent, (enum child_kind)operation, &update, &result);
	} else if (operation == ENABLE_WRITE) {
		error = ext4_write(fs, inode.number, inode.generation, device->block_size + 7,
		    bytes, CHILD_FAST_BYTES, &update, &completed);
		CHECK(completed == (error == EXT4_OK ? CHILD_FAST_BYTES : 0));
	} else if (operation == ENABLE_TRUNCATE) {
		error = ext4_truncate(fs, inode.number, inode.generation, 17, &update, &result);
	} else {
		error = ext4_set_attributes(fs, inode.number, inode.generation, &update, &result);
	}
	CHECK(device->live == live);
	if (error != EXT4_OK) {
		CHECK(memcmp(&result, &untouched, sizeof(result)) == 0 &&
		    fs->info.feature_compat == feature_compat);
		if (device->writes == 0) {
			CHECK(fs->info.free_blocks == free_blocks &&
			    fs->info.free_inodes == free_inodes &&
			    memcmp(device->cache, device->base, device->size) == 0 &&
			    memcmp(device->stable, device->base, device->size) == 0);
		}
	} else {
		CHECK(fs->info.feature_compat == (feature_compat | EXT4_FEATURE_COMPAT_EXT_ATTR));
		error = ext4_sync(fs);
	}
	trace->allocations = device->allocations - allocations;
	trace->reads = device->reads - reads;
	trace->events = device->events - events;
	trace->commit_event = device->commit_barrier == 0 ? 0 : device->commit_barrier - events;
	trace->committed = device->intent_durable;
	if (error == EXT4_OK) {
		if (operation == ENABLE_WRITE) {
			EXPECT(ext4_get_inode(fs, inode.number, &result), EXT4_OK);
		}
		value_is(fs, &result, EXT4_XATTR_USER, "binary", bytes, length);
	} else if (fs->aborted) {
		EXPECT(ext4_get_inode(fs, inode.number, &result), EXT4_RECOVERY_REQUIRED);
	}
	ext4_unmount(fs);
	CHECK(device->live == 0);
	return error;
}

static void
enable_feature_guards(struct device *device)
{
	struct ext4_fs *fs;
	struct ext4_inode root;
	struct ext4_transaction *transaction;
	struct ext4_super_disk *super;
	struct lifetime_trace trace;
	uint8_t *with_attributes = malloc(device->size);
	uint8_t *pending = malloc(device->size);
	uint32_t feature;
	uint32_t cut;
	unsigned int variant;
	unsigned int attempt;
	enum ext4_result error;

	CHECK(with_attributes != NULL && pending != NULL);
	device_reset(device, device->base);
	EXPECT(enable_attempt(device, ENABLE_SET_EXTERNAL, 0, 0, 0, false, &trace), EXT4_OK);
	memcpy(with_attributes, device->stable, device->size);
	for (variant = 0; variant < 2; variant++) {
		cut = 0;
		for (attempt = 0; attempt < 2; attempt++) {
			device_reset(device, variant == 0 ? with_attributes : device->base);
			fs = mount_writer(device, &root);
			EXPECT(ext4_transaction_begin(fs->journal, 1, &transaction), EXT4_OK);
			EXPECT(ext4_transaction_super(transaction, &super), EXT4_OK);
			feature = variant == 0 ? EXT4_FEATURE_COMPAT_EXT_ATTR
					       : EXT4_FEATURE_COMPAT_DIR_INDEX;
			CHECK(ext4_le32(&super->feature_compat) & feature);
			ext4_encode32(
			    &super->feature_compat, ext4_le32(&super->feature_compat) & ~feature);
			/* Commit a validly checksummed but forbidden format transition.
			 * The first run locates the commit boundary; only the second run
			 * leaves the pending log for the recovery admission check. */
			device->stop_at = cut;
			error = ext4_transaction_commit(transaction);
			EXPECT(error, attempt == 0 ? EXT4_OK : EXT4_IO);
			if (attempt == 0) {
				CHECK(device->commit_barrier != 0);
				cut = device->commit_barrier + 1;
			} else {
				CHECK(device->intent_durable);
			}
			ext4_unmount(fs);
			CHECK(device->live == 0);
		}
		memcpy(pending, device->stable, device->size);
		for (attempt = 0; attempt < 2; attempt++) {
			device_reset(device, device->stable);
			EXPECT(ext4_recover(&device->environment, &device->writer, NULL),
			    EXT4_UNSUPPORTED);
			CHECK(device->writes == 0 && device->live == 0 &&
			    memcmp(pending, device->stable, device->size) == 0 &&
			    memcmp(pending, device->cache, device->size) == 0);
		}
	}
	device_reset(device, device->base);
	free(pending);
	free(with_attributes);
	puts("PASS recovery rejects clearing EXT_ATTR and unrelated feature transitions without "
	     "writes");
}

static void
enable_faults(struct device *device, enum enable_operation operation, bool smoke,
    const char *exports, const char *path)
{
	struct lifetime_trace baseline;
	struct lifetime_trace trace;
	uint8_t *expected = malloc(device->size);
	uint32_t point;
	uint32_t limit;
	uint32_t recovered = 0;
	uint32_t torn = 0;
	unsigned int fault;
	unsigned int survival;
	unsigned int partial;
	char prefix[80];

	CHECK(expected != NULL);
	printf("RUN xattr enable faults operation=%s\n", enable_names[operation]);
	CHECK(fflush(stdout) == 0);
	device_reset(device, device->base);
	EXPECT(enable_attempt(device, operation, 0, 0, 0, false, &baseline), EXT4_OK);
	memcpy(expected, device->stable, device->size);
	CHECK(snprintf(prefix, sizeof(prefix), "xattr-enable-%s-", enable_names[operation]) > 0);
	storage_export(device, exports, path, prefix);
	CHECK(storage_recover(device, expected, true) && device->writes == 0);
	if (!smoke) {
		for (fault = 1; fault <= 2; fault++) {
			limit = fault == 1 ? baseline.allocations : baseline.reads;
			for (point = 1; point <= limit; point++) {
				device_reset(device, device->base);
				EXPECT(enable_attempt(
					   device, operation, fault, point, 0, false, &trace),
				    fault == 1 ? EXT4_NO_MEMORY : EXT4_IO);
				CHECK(storage_recover(device, expected, trace.committed));
			}
		}
		for (point = 1; point <= baseline.events; point++) {
			for (survival = 0; survival < 3; survival++) {
				for (partial = 0; partial < 2; partial++) {
					device_reset(device, device->base);
					EXPECT(enable_attempt(device, operation, 3, point, survival,
						   partial != 0, &trace),
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
		printf("PASS xattr enable faults operation=%s allocations=%u reads=%u cuts=%u "
		       "recovered=%u torn_super_fail_closed=%u\n",
		    enable_names[operation], baseline.allocations, baseline.reads,
		    baseline.events * 6, recovered, torn);
	}
	if (exports != NULL) {
		CHECK(baseline.commit_event > 1 && baseline.commit_event < baseline.events);
		for (fault = 0; fault < 2; fault++) {
			device_reset(device, device->base);
			point = fault == 0 ? baseline.commit_event - 1 : baseline.commit_event + 1;
			EXPECT(
			    enable_attempt(device, operation, 3, point, 0, false, &trace), EXT4_IO);
			CHECK(trace.committed == (fault != 0));
			CHECK(snprintf(prefix, sizeof(prefix), "xattr-enable-%s-%s-",
				  fault == 0 ? "uncommitted" : "pending",
				  enable_names[operation]) > 0);
			storage_export(device, exports, path, prefix);
			CHECK(storage_recover(device, expected, trace.committed));
		}
	}
	device_reset(device, device->base);
	free(expected);
}

#endif

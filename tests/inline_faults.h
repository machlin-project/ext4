/* SPDX-License-Identifier: BSD-3-Clause */
#ifndef MACHLIN_EXT4_TEST_INLINE_FAULTS_H
#define MACHLIN_EXT4_TEST_INLINE_FAULTS_H

enum inline_operation {
	INLINE_OVERWRITE,
	INLINE_GROW,
	INLINE_CONVERT,
	INLINE_ADD,
	INLINE_DIRECTORY_GROW,
	INLINE_ATTRIBUTES,
	INLINE_UNLINK,
	INLINE_OPERATIONS
};

struct inline_trace {
	uint32_t allocations;
	uint32_t reads;
	uint32_t events;
	uint32_t commit_event;
	bool committed;
};

static enum ext4_result
fault_attempt(struct device *device, enum inline_operation operation, unsigned int fault,
    uint32_t point, unsigned int survival, bool partial, struct inline_trace *trace)
{
	struct ext4_fs *fs = mount_writer(device);
	struct ext4_inode inode = lookup(fs, operation == INLINE_GROW ? "file1" : "file120");
	struct ext4_inode directory = lookup(fs, "empty");
	struct ext4_inode root;
	struct ext4_inode result;
	struct ext4_inode untouched;
	struct ext4_inode_update update = attributes(false);
	struct ext4_inode_update create = attributes(true);
	struct ext4_timestamp time = { INLINE_TEST_SECONDS, 0 };
	struct ext4_xattr_change changes[2] = { 0 };
	uint8_t *bytes = malloc(device->block_size);
	uint8_t name[EXT4_NAME_MAX];
	uint64_t free_blocks = fs->info.free_blocks;
	uint32_t free_inodes = fs->info.free_inodes;
	uint32_t allocations;
	uint32_t reads;
	uint32_t events;
	uint32_t live;
	size_t completed = 0;
	enum ext4_result error;

	CHECK(bytes != NULL);
	pattern(bytes, device->block_size);
	if (operation == INLINE_OVERWRITE) {
		memset(bytes, 0xe4, 31);
	}
	memset(name, 'x', sizeof(name));
	memset(&untouched, 0xa5, sizeof(untouched));
	result = untouched;
	EXPECT(ext4_get_inode(fs, EXT4_ROOT_INODE, &root), EXT4_OK);
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
	switch (operation) {
	case INLINE_OVERWRITE:
	case INLINE_GROW:
	case INLINE_CONVERT:
		error = ext4_write(fs, inode.number, inode.generation,
		    operation == INLINE_CONVERT ? device->block_size + 7U : 0, bytes,
		    operation == INLINE_OVERWRITE ? 31 : 120, &update, &completed);
		if (error != EXT4_OK) {
			CHECK(completed == 0);
		}
		break;
	case INLINE_ADD:
	case INLINE_DIRECTORY_GROW:
		error = ext4_create(fs, directory.number, directory.generation, name,
		    operation == INLINE_ADD ? 3 : sizeof(name), &create, &time, &result);
		break;
	case INLINE_ATTRIBUTES:
		changes[0].policy = changes[1].policy = EXT4_XATTR_CREATE;
		changes[0].name_index = changes[1].name_index = EXT4_XATTR_USER;
		changes[0].name = (const uint8_t *)"large";
		changes[1].name = (const uint8_t *)"small";
		changes[0].name_length = changes[1].name_length = 5;
		changes[0].value = changes[1].value = bytes;
		changes[0].value_size = device->block_size - 120;
		changes[1].value_size = 52;
		update.xattrs = changes;
		update.xattr_count = 2;
		error = ext4_set_attributes(fs, inode.number, inode.generation, &update, &result);
		break;
	case INLINE_UNLINK:
		error = ext4_unlink(fs, root.number, root.generation, (const uint8_t *)"file120", 7,
		    inode.number, inode.generation, &time, &result);
		break;
	default:
		error = EXT4_INVALID_ARGUMENT;
		break;
	}
	if (error != EXT4_OK) {
		CHECK(device->live == live && memcmp(&result, &untouched, sizeof(result)) == 0);
		if (device->writes == 0) {
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
	if (error != EXT4_OK && device->writes != 0) {
		CHECK(fs->aborted);
	}
	ext4_unmount(fs);
	CHECK(device->live == 0);
	free(bytes);
	return error;
}

static void
mutation_faults(struct device *device, const char *exports, const char *path, bool smoke)
{
	static const char *const labels[] = { "overwrite", "grow", "convert", "add",
		"directory-grow", "attributes", "unlink" };
	struct inline_trace baseline;
	struct inline_trace trace;
	uint8_t *expected = malloc(device->size);
	uint32_t point;
	uint32_t limit;
	uint32_t recovered;
	uint32_t torn;
	unsigned int fault;
	unsigned int survival;
	unsigned int partial;
	char prefix[96];
	enum inline_operation operation;

	CHECK(expected != NULL);
	for (operation = INLINE_OVERWRITE; operation < INLINE_OPERATIONS; operation++) {
		device_reset(device, device->base);
		CHECK(snprintf(prefix, sizeof(prefix), "inline-fault-before-%s-",
			  labels[operation]) > 0);
		storage_export(device, exports, path, prefix);
		EXPECT(fault_attempt(device, operation, 0, 0, 0, false, &baseline), EXT4_OK);
		memcpy(expected, device->stable, device->size);
		CHECK(snprintf(
			  prefix, sizeof(prefix), "inline-fault-after-%s-", labels[operation]) > 0);
		storage_export(device, exports, path, prefix);
		recovered = torn = 0;
		for (fault = 1; !smoke && fault <= 2; fault++) {
			limit = fault == 1 ? baseline.allocations : baseline.reads;
			for (point = 1; point <= limit; point++) {
				device_reset(device, device->base);
				EXPECT(fault_attempt(
					   device, operation, fault, point, 0, false, &trace),
				    fault == 1 ? EXT4_NO_MEMORY : EXT4_IO);
				CHECK(storage_recover(device, expected, trace.committed));
			}
		}
		for (point = 1; !smoke && point <= baseline.events; point++) {
			for (survival = 0; survival < 3; survival++) {
				for (partial = 0; partial < 2; partial++) {
					device_reset(device, device->base);
					EXPECT(fault_attempt(device, operation, 3, point, survival,
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
		if (exports != NULL) {
			CHECK(baseline.commit_event > 1 && baseline.commit_event < baseline.events);
			device_reset(device, device->base);
			EXPECT(fault_attempt(device, operation, 3, baseline.commit_event + 1, 0,
				   false, &trace),
			    EXT4_IO);
			CHECK(trace.committed);
			CHECK(snprintf(prefix, sizeof(prefix), "inline-fault-pending-%s-",
				  labels[operation]) > 0);
			storage_export(device, exports, path, prefix);
			CHECK(storage_recover(device, expected, true));
			device_reset(device, device->base);
			EXPECT(fault_attempt(device, operation, 3, baseline.commit_event - 1, 1,
				   false, &trace),
			    EXT4_IO);
			CHECK(!trace.committed);
			CHECK(snprintf(prefix, sizeof(prefix), "inline-fault-uncommitted-%s-",
				  labels[operation]) > 0);
			storage_export(device, exports, path, prefix);
			CHECK(storage_recover(device, expected, false));
		}
		printf("PASS inline faults operation=%s smoke=%u allocations=%u reads=%u cuts=%u "
		       "recovered=%u torn_super_fail_closed=%u\n",
		    labels[operation], smoke, baseline.allocations, baseline.reads,
		    smoke ? 0 : baseline.events * 6, recovered, torn);
	}
	free(expected);
}

#endif

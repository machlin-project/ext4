/* SPDX-License-Identifier: BSD-3-Clause */
#ifndef MACHLIN_EXT4_TEST_EA_INODE_FAULTS_H
#define MACHLIN_EXT4_TEST_EA_INODE_FAULTS_H

enum ea_fault_operation { EA_CREATE, EA_REPLACE, EA_REMOVE, EA_COPY, EA_FAULT_OPERATIONS };

struct ea_trace {
	uint32_t allocations;
	uint32_t reads;
	uint32_t events;
	uint32_t commit_event;
	bool committed;
};

static enum ext4_result
fault_attempt(struct device *device, enum ea_fault_operation operation, const uint8_t *bytes,
    unsigned int fault, uint32_t point, unsigned int survival, bool partial, struct ea_trace *trace)
{
	struct ext4_fs *fs = mount_writer(device);
	struct ext4_inode inode = lookup(fs,
	    operation == EA_CREATE     ? "plain"
		: operation == EA_COPY ? "alias"
				       : "body");
	struct ext4_inode untouched;
	struct ext4_inode result;
	struct ext4_xattr_change edit;
	struct ext4_inode_update update;
	uint64_t free_blocks = fs->info.free_blocks;
	uint32_t free_inodes = fs->info.free_inodes;
	uint32_t allocations = device->allocations;
	uint32_t reads = device->reads;
	uint32_t events = device->events;
	uint32_t live = device->live;
	enum ext4_result error;

	edit = change(operation == EA_CREATE ? EXT4_XATTR_CREATE : EXT4_XATTR_REPLACE, "maximum",
	    bytes, EXT4_XATTR_VALUE_MAX);
	if (operation == EA_REMOVE || operation == EA_COPY) {
		edit =
		    change(EXT4_XATTR_REMOVE, operation == EA_COPY ? "value7" : "maximum", NULL, 0);
	}
	update = attributes(&edit, 1, false);
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
	error = ext4_set_attributes(fs, inode.number, inode.generation, &update, &result);
	if (error != EXT4_OK) {
		CHECK(memcmp(&result, &untouched, sizeof(result)) == 0 && device->live == live);
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
		EXPECT(ext4_get_inode(fs, inode.number, &result), EXT4_RECOVERY_REQUIRED);
	}
	ext4_unmount(fs);
	CHECK(device->live == 0);
	return error;
}

static void
mutation_faults(struct device *device, const char *exports, const char *path)
{
	static const char *const labels[] = { "create", "replace", "remove", "copy" };
	struct ext4_fs *fs;
	struct ext4_inode source;
	struct ext4_inode alias;
	struct ea_trace baseline;
	struct ea_trace trace;
	uint8_t *original = malloc(device->size);
	uint8_t *expected = malloc(device->size);
	uint8_t *bytes = malloc(EXT4_XATTR_VALUE_MAX);
	uint32_t point;
	uint32_t limit;
	uint32_t recovered;
	uint32_t torn;
	unsigned int fault;
	unsigned int survival;
	unsigned int partial;
	char prefix[64];
	enum ea_fault_operation operation;

	CHECK(original != NULL && expected != NULL && bytes != NULL);
	memcpy(original, device->base, device->size);
	pattern(bytes, EXT4_XATTR_VALUE_MAX, true);
	for (operation = EA_CREATE; operation < EA_FAULT_OPERATIONS; operation++) {
		memcpy(device->base, original, device->size);
		device_reset(device, device->base);
		if (operation == EA_COPY) {
			fs = mount_writer(device);
			source = lookup(fs, "many");
			alias = lookup(fs, "alias");
			clone_attributes(device, fs, &source, &alias);
			ext4_unmount(fs);
			memcpy(device->base, device->cache, device->size);
		}
		device_reset(device, device->base);
		EXPECT(fault_attempt(device, operation, bytes, 0, 0, 0, false, &baseline), EXT4_OK);
		memcpy(expected, device->stable, device->size);
		CHECK(
		    snprintf(prefix, sizeof(prefix), "ea-fault-after-%s-", labels[operation]) > 0);
		storage_export(device, exports, path, prefix);
		recovered = 0;
		torn = 0;
		for (fault = 1; fault <= 2; fault++) {
			limit = fault == 1 ? baseline.allocations : baseline.reads;
			for (point = 1; point <= limit; point++) {
				device_reset(device, device->base);
				EXPECT(fault_attempt(device, operation, bytes, fault, point, 0,
					   false, &trace),
				    fault == 1 ? EXT4_NO_MEMORY : EXT4_IO);
				CHECK(storage_recover(device, expected, trace.committed));
			}
		}
		for (point = 1; point <= baseline.events; point++) {
			for (survival = 0; survival < 3; survival++) {
				for (partial = 0; partial < 2; partial++) {
					device_reset(device, device->base);
					EXPECT(fault_attempt(device, operation, bytes, 3, point,
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
		if (exports != NULL) {
			CHECK(baseline.commit_event > 1 && baseline.commit_event < baseline.events);
			device_reset(device, device->base);
			EXPECT(fault_attempt(device, operation, bytes, 3, baseline.commit_event + 1,
				   0, false, &trace),
			    EXT4_IO);
			CHECK(trace.committed);
			CHECK(snprintf(prefix, sizeof(prefix), "ea-fault-pending-%s-",
				  labels[operation]) > 0);
			storage_export(device, exports, path, prefix);
			CHECK(storage_recover(device, expected, true));
			device_reset(device, device->base);
			EXPECT(fault_attempt(device, operation, bytes, 3, baseline.commit_event - 1,
				   1, false, &trace),
			    EXT4_IO);
			CHECK(!trace.committed);
			CHECK(snprintf(prefix, sizeof(prefix), "ea-fault-uncommitted-%s-",
				  labels[operation]) > 0);
			storage_export(device, exports, path, prefix);
			CHECK(storage_recover(device, expected, false));
		}
		printf("PASS EA_INODE faults operation=%s allocations=%u reads=%u cuts=%u "
		       "recovered=%u torn_super_fail_closed=%u\n",
		    labels[operation], baseline.allocations, baseline.reads, baseline.events * 6,
		    recovered, torn);
	}
	memcpy(device->base, original, device->size);
	free(bytes);
	free(expected);
	free(original);
}

#endif

/* SPDX-License-Identifier: BSD-3-Clause */
#ifndef MACHLIN_EXT4_TEST_DIRECTORY_LINKS_H
#define MACHLIN_EXT4_TEST_DIRECTORY_LINKS_H

static void
directory_links_model(struct device *device)
{
	struct ext4_fs *fs;
	struct ext4_inode parent;
	struct ext4_inode hello;
	struct ext4_inode_disk *disk;
	struct ext4_super_disk *super;
	struct trace trace;
	uint64_t offset;
	uint32_t point;

	point = prepare_index_creation(device);
	device_reset(device, device->base);
	EXPECT(split_attempt(device, point, 0, 0, 0, false, &trace), EXT4_OK);
	fs = mount_index(device, &parent, &hello);
	CHECK(parent.flags & EXT4_INODE_INDEX);
	EXPECT(ext4_inode_location(fs, parent.number, &offset), EXT4_OK);
	disk = (struct ext4_inode_disk *)(device->cache + offset);
	ext4_encode16(&disk->links, EXT4_LINK_MAX);
	ext4_inode_checksum_set(fs, parent.number, disk);
	super = (struct ext4_super_disk *)(device->cache + EXT4_SUPER_OFFSET);
	ext4_encode32(&super->feature_ro_compat,
	    ext4_le32(&super->feature_ro_compat) & ~EXT4_FEATURE_RO_DIR_NLINK);
	if (device->metadata_checksum) {
		ext4_encode32(&super->checksum,
		    ext4_crc32c(UINT32_MAX, super, offsetof(struct ext4_super_disk, checksum)));
	}
	ext4_unmount(fs);
	/* Only the boundary counter is modeled. This compact state is never
	 * exported or presented as an independently clean filesystem. */
	memcpy(device->base, device->cache, device->size);
	device_reset(device, device->base);
}

static enum ext4_result
directory_links_attempt(struct device *device, unsigned int fault, uint32_t point,
    unsigned int survival, bool partial, struct trace *trace)
{
	struct ext4_fs *fs;
	struct ext4_inode parent;
	struct ext4_inode hello;
	struct ext4_inode result;
	struct ext4_inode untouched;
	struct ext4_inode_update update = attributes();
	uint32_t allocations;
	uint32_t reads;
	uint32_t events;
	enum ext4_result error;

	fs = mount_index(device, &parent, &hello);
	CHECK(parent.links == EXT4_LINK_MAX && (parent.flags & EXT4_INODE_INDEX));
	allocations = device->allocations;
	reads = device->reads;
	events = device->events;
	memset(&result, 0xa5, sizeof(result));
	untouched = result;
	device->survival = survival;
	device->partial = partial;
	if (fault == 1) {
		device->fail_allocation = allocations + point;
	} else if (fault == 2) {
		device->fail_read = reads + point;
	} else if (fault == 3) {
		device->stop_at = events + point;
	}
	error = ext4_mkdir(fs, parent.number, parent.generation, (const uint8_t *)"overflow", 8,
	    &update, &mutation_time, &result);
	if (error == EXT4_OK) {
		CHECK(
		    result.links == 2 && (fs->info.feature_ro_compat & EXT4_FEATURE_RO_DIR_NLINK));
		error = ext4_sync(fs);
	} else {
		CHECK(memcmp(&result, &untouched, sizeof(result)) == 0);
	}
	trace->allocations = device->allocations - allocations;
	trace->reads = device->reads - reads;
	trace->events = device->events - events;
	trace->commit_event = device->commit_barrier - events;
	trace->committed = device->intent_durable;
	if (error != EXT4_OK) {
		if (device->writes != 0) {
			CHECK(fs->aborted);
		} else {
			CHECK(memcmp(device->cache, device->base, device->size) == 0);
			CHECK(!(fs->info.feature_ro_compat & EXT4_FEATURE_RO_DIR_NLINK));
		}
	}
	ext4_unmount(fs);
	CHECK(device->live == 0);
	return error;
}

static void
directory_links_sequence(struct device *device, bool model)
{
	struct ext4_fs *fs;
	struct ext4_inode root;
	struct ext4_inode parent;
	struct ext4_inode peer;
	struct ext4_inode hello;
	struct ext4_inode child;
	struct ext4_inode extra;
	struct ext4_inode result;
	struct ext4_inode_disk *disk;
	struct ext4_inode_update update = attributes();
	struct ext4_rename_entry source;
	struct ext4_rename_entry destination;
	uint64_t offset;
	uint16_t peer_links;

	fs = mount_index(device, &parent, &hello);
	CHECK(parent.links == 1);
	EXPECT(ext4_get_inode(fs, EXT4_ROOT_INODE, &root), EXT4_OK);
	peer = lookup(fs, &root, "peer");
	peer_links = peer.links;
	child = lookup(fs, &parent, "child");
	move(fs, &parent, (const uint8_t *)"child", &peer, (const uint8_t *)"moved", 0);
	EXPECT(ext4_get_inode(fs, peer.number, &peer), EXT4_OK);
	EXPECT(ext4_get_inode(fs, parent.number, &parent), EXT4_OK);
	CHECK(peer.links == peer_links + 1 && parent.links == 1);
	move(fs, &peer, (const uint8_t *)"moved", &parent, (const uint8_t *)"child", 0);
	EXPECT(ext4_get_inode(fs, peer.number, &peer), EXT4_OK);
	CHECK(peer.links == peer_links);
	EXPECT(ext4_mkdir(fs, parent.number, parent.generation, (const uint8_t *)"extra", 5,
		   &update, &mutation_time, &extra),
	    EXT4_OK);
	move(fs, &parent, (const uint8_t *)"extra", &peer, (const uint8_t *)"child",
	    EXT4_RENAME_EXCHANGE);
	EXPECT(ext4_get_inode(fs, parent.number, &parent), EXT4_OK);
	CHECK(parent.links == 1);
	result = lookup(fs, &parent, "extra");
	if (model) {
		EXPECT(ext4_inode_location(fs, result.number, &offset), EXT4_OK);
		disk = (struct ext4_inode_disk *)(device->cache + offset);
		ext4_encode16(&disk->links, 1);
		ext4_inode_checksum_set(fs, result.number, disk);
	}
	source = rename_entry(&parent, (const uint8_t *)"child", &child);
	destination = rename_entry(&parent, (const uint8_t *)"extra", &result);
	EXPECT(ext4_rename(fs, &source, &destination, 0, &mutation_time, &result), EXT4_OK);
	child = lookup(fs, &parent, "extra");
	if (model) {
		EXPECT(ext4_inode_location(fs, child.number, &offset), EXT4_OK);
		disk = (struct ext4_inode_disk *)(device->cache + offset);
		ext4_encode16(&disk->links, 1);
		ext4_inode_checksum_set(fs, child.number, disk);
	}
	EXPECT(ext4_rmdir(fs, parent.number, parent.generation, (const uint8_t *)"extra", 5,
		   child.number, child.generation, &mutation_time, &result),
	    EXT4_OK);
	EXPECT(ext4_get_inode(fs, parent.number, &parent), EXT4_OK);
	CHECK(parent.links == 1);
	EXPECT(ext4_sync(fs), EXT4_OK);
	ext4_unmount(fs);
	CHECK(device->live == 0);
	puts("PASS DIR_NLINK mkdir, cross-parent move, exchange, replacement and rmdir");
}

static void
directory_links(struct device *device, const char *path, const char *exports, bool real_image)
{
	struct trace baseline;
	struct trace trace;
	uint8_t *original = malloc(device->size);
	uint8_t *expected = malloc(device->size);
	uint32_t limit;
	uint32_t point;
	uint32_t recovered = 0;
	uint32_t torn = 0;
	unsigned int fault;
	unsigned int survival;
	unsigned int partial;

	CHECK(original != NULL && expected != NULL && (real_image || exports == NULL));
	memcpy(original, device->base, device->size);
	if (!real_image) {
		directory_links_model(device);
	}
	device_reset(device, device->base);
	storage_export(device, exports, path, "links-before-");
	EXPECT(directory_links_attempt(device, 0, 0, 0, false, &baseline), EXT4_OK);
	memcpy(expected, device->stable, device->size);
	storage_export(device, exports, path, "links-atomic-");
	directory_links_sequence(device, !real_image);
	if (!real_image) {
		for (fault = 1; fault <= 2; fault++) {
			limit = fault == 1 ? baseline.allocations : baseline.reads;
			for (point = 1; point <= limit; point++) {
				device_reset(device, device->base);
				EXPECT(
				    directory_links_attempt(device, fault, point, 0, false, &trace),
				    fault == 1 ? EXT4_NO_MEMORY : EXT4_IO);
				CHECK(storage_recover(device, expected, trace.committed));
			}
		}
		for (point = 1; point <= baseline.events; point++) {
			for (survival = 0; survival < 3; survival++) {
				for (partial = 0; partial < 2; partial++) {
					device_reset(device, device->base);
					EXPECT(directory_links_attempt(device, 3, point, survival,
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
		printf(
		    "PASS modeled DIR_NLINK boundary allocations=%u reads=%u cuts=%u recovered=%u "
		    "torn_super_fail_closed=%u\n",
		    baseline.allocations, baseline.reads, baseline.events * 6, recovered, torn);
	}
	if (exports != NULL) {
		CHECK(baseline.commit_event > 1 && baseline.commit_event < baseline.events);
		device_reset(device, device->base);
		EXPECT(
		    directory_links_attempt(device, 3, baseline.commit_event + 1, 0, false, &trace),
		    EXT4_IO);
		CHECK(trace.committed);
		storage_export(device, exports, path, "links-pending-");
		CHECK(storage_recover(device, expected, true));
		device_reset(device, device->base);
		EXPECT(
		    directory_links_attempt(device, 3, baseline.commit_event - 1, 0, false, &trace),
		    EXT4_IO);
		CHECK(!trace.committed);
		storage_export(device, exports, path, "links-uncommitted-");
		CHECK(storage_recover(device, expected, false));
	}
	memcpy(device->base, original, device->size);
	free(expected);
	free(original);
}

#endif

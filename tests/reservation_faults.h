/* SPDX-License-Identifier: BSD-3-Clause */
/* Publish the old boundary's merge and the new boundary's split atomically. */

static bool
reservation_recover(struct device *device, bool committed)
{
	struct ext4_recovery_report report;
	struct ext4_super_disk *super;
	struct ext4_inode inode;
	struct ext4_fs *fs;
	size_t old_size = (EXT4_ORPHAN_BATCH_BLOCKS + 2U) * device->block_size + 8U;
	size_t new_size = (2U * EXT4_ORPHAN_BATCH_BLOCKS + 6U) * device->block_size + 8U;
	uint8_t *expected = calloc(new_size, 1);
	bool updated;
	enum ext4_result error;

	CHECK(expected != NULL);
	device_reset(device, device->stable);
	error = ext4_recover(&device->environment, &device->writer, &report);
	if (error == EXT4_CORRUPT) {
		super = (struct ext4_super_disk *)(device->cache + EXT4_SUPER_OFFSET);
		CHECK(device->metadata_checksum && device->writes == 0 &&
		    ext4_le32(&super->checksum) !=
			ext4_crc32c(UINT32_MAX, super, offsetof(struct ext4_super_disk, checksum)));
		free(expected);
		return false;
	}
	EXPECT(error, EXT4_OK);
	fs = mount_file(device, &inode);
	updated = inode.size == new_size;
	CHECK((updated || inode.size == old_size) && (!committed || updated));
	expected[7] = 0xa1;
	expected[old_size - 1U] = 0xd1;
	if (updated) {
		expected[new_size - 1U] = 0xe3;
	}
	contents(fs, &inode, expected, updated ? new_size : old_size);
	CHECK(fs->info.free_blocks == 0 && inode.mode == (EXT4_MODE_REGULAR | 0640) &&
	    inode.links == 1 &&
	    inode.blocks_512 ==
		3U * (2U * EXT4_ORPHAN_BATCH_BLOCKS + 5U) *
		    (device->block_size / EXT4_SECTOR_SIZE));
	CHECK(inode.modify_time.seconds == RANGE_SECONDS + (updated ? 19 : 9) &&
	    inode.change_time.seconds == RANGE_SECONDS + (updated ? 20 : 10));
	ext4_unmount(fs);
	CHECK(device->live == 0 && memcmp(device->cache, device->stable, device->size) == 0);
	free(expected);
	return true;
}

static void
reservation_faults(struct device *device, const char *exports, const char *path)
{
	struct ext4_inode inode;
	struct ext4_fs *fs;
	struct ext4_inode_update update = attributes(NULL);
	struct capacity_trace trace;
	uint8_t *original = device->base;
	uint8_t *prepared = malloc(device->size);
	uint8_t value = 0xa1;
	size_t offset = (2U * EXT4_ORPHAN_BATCH_BLOCKS + 6U) * device->block_size + 7U;
	size_t written;
	uint32_t phase;
	uint32_t position;
	uint32_t count;
	uint32_t allocations;
	uint32_t reads;
	uint32_t events;
	uint32_t commits;
	uint32_t barrier;
	uint32_t partial;
	uint32_t survival;
	uint32_t cuts = 0;
	uint32_t recovered = 0;
	bool committed;
	enum ext4_result error;

	CHECK(prepared != NULL);
	reservation_seed(device, false, &inode);
	fs = mount_file(device, &inode);
	update.modify_time.seconds += 9;
	update.change_time.seconds += 9;
	EXPECT(
	    ext4_write_partial(fs, inode.number, inode.generation, 7, &value, 1, &update, &written),
	    EXT4_OK);
	value = 0xd1;
	EXPECT(ext4_write_partial(fs, inode.number, inode.generation,
		   (EXT4_ORPHAN_BATCH_BLOCKS + 2U) * device->block_size + 7U, &value, 1, &update,
		   &written),
	    EXT4_OK);
	EXPECT(ext4_sync(fs), EXT4_OK);
	ext4_unmount(fs);
	memcpy(prepared, device->cache, device->size);
	device->base = prepared;
	device_reset(device, prepared);
	storage_export(device, exports, path, "reservation-before-");
	update.modify_time.seconds += 10;
	update.change_time.seconds += 10;
	value = 0xe3;
	fs = mount_file(device, &inode);
	trace = (struct capacity_trace){ .device = device };
	fs->journal->writer =
	    (struct ext4_write_environment){ &trace, capacity_trace_write, capacity_trace_flush };
	device->allocations = device->reads = 0;
	EXPECT(ext4_write_partial(
		   fs, inode.number, inode.generation, offset, &value, 1, &update, &written),
	    EXT4_OK);
	CHECK(written == 1 && trace.commits >= 4 && trace.durable_commits == trace.commits);
	allocations = device->allocations;
	reads = device->reads;
	events = device->events;
	commits = trace.commits;
	barrier = trace.last_barrier;
	EXPECT(ext4_sync(fs), EXT4_OK);
	storage_export(device, exports, path, "reservation-after-");
	ext4_unmount(fs);
	CHECK(reservation_recover(device, true));
	for (phase = 0; exports == NULL && phase < 2; phase++) {
		count = phase == 0 ? allocations : reads;
		for (position = 1; position <= count; position++) {
			device_reset(device, prepared);
			fs = mount_file(device, &inode);
			trace = (struct capacity_trace){ .device = device };
			fs->journal->writer = (struct ext4_write_environment){ &trace,
				capacity_trace_write, capacity_trace_flush };
			device->allocations = device->reads = 0;
			device->fail_allocation = phase == 0 ? position : 0;
			device->fail_read = phase == 1 ? position : 0;
			error = ext4_write_partial(fs, inode.number, inode.generation, offset,
			    &value, 1, &update, &written);
			EXPECT(error, phase == 0 ? EXT4_NO_MEMORY : EXT4_IO);
			CHECK(written == 0);
			ext4_unmount(fs);
			CHECK(reservation_recover(device, trace.durable_commits == commits));
		}
	}
	for (position = 1; position <= events; position++) {
		if (exports != NULL && position != barrier && position != barrier + 1U) {
			continue;
		}
		for (partial = 0; partial < (exports == NULL ? 2U : 1U); partial++) {
			for (survival = 0; survival < (exports == NULL ? 3U : 1U); survival++) {
				device_reset(device, prepared);
				fs = mount_file(device, &inode);
				trace = (struct capacity_trace){ .device = device };
				fs->journal->writer = (struct ext4_write_environment){ &trace,
					capacity_trace_write, capacity_trace_flush };
				device->stop_at = position;
				device->partial = partial != 0;
				device->survival = survival;
				EXPECT(ext4_write_partial(fs, inode.number, inode.generation,
					   offset, &value, 1, &update, &written),
				    EXT4_IO);
				CHECK(written == 0 && fs->aborted && device->off);
				committed = trace.durable_commits == commits;
				ext4_unmount(fs);
				storage_export(device, exports, path,
				    committed ? "reservation-pending-"
					      : "reservation-uncommitted-");
				recovered += reservation_recover(device, committed) ? 1U : 0U;
				cuts++;
			}
		}
	}
	device->base = original;
	free(prepared);
	printf("PASS reserved EOF-boundary faults: allocations=%u reads=%u commits=%u cuts=%u "
	       "recovered=%u torn_super_fail_closed=%u\n",
	    allocations, reads, commits, cuts, recovered, cuts - recovered);
}

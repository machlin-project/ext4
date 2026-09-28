/* SPDX-License-Identifier: BSD-3-Clause */
/* Reserve future EOF-boundary metadata before exhausting the remaining blocks. */

static uint32_t
reservation_blocks(const struct device *device, bool external)
{
	/* The 4 KiB fixture has only 975 data blocks available to release. */
	return external ? (device->block_size == EXT4_MIN_BLOCK_SIZE ? 3U : 2U)
			: 2U * EXT4_ORPHAN_BATCH_BLOCKS + 5U;
}

static void
reservation_seed(struct device *device, bool external, struct ext4_inode *inode)
{
	struct ext4_fs *fs = mount_file(device, inode);
	struct ext4_inode root;
	struct ext4_inode filler;
	struct ext4_inode_update update = attributes(NULL);
	uint32_t blocks = reservation_blocks(device, external);
	/* Root promotion separates one physical run with its metadata block. */
	uint32_t runs = external ? (device->block_size - sizeof(struct ext4_extent_header_disk)) /
		    sizeof(struct ext4_extent_disk) -
		2U
				 : 3U;
	uint64_t owned = (uint64_t)runs * blocks + (external ? 1U : 0U);
	uint64_t completed;
	uint32_t index;

	CHECK(fs->info.free_blocks == 0 && inode->size == 0);
	EXPECT(ext4_get_inode(fs, EXT4_ROOT_INODE, &root), EXT4_OK);
	EXPECT(ext4_lookup(fs, &root, (const uint8_t *)"filler", 6, &filler), EXT4_OK);
	EXPECT(ext4_fallocate(fs, filler.number, filler.generation, 0, owned * device->block_size,
		   EXT4_FALLOC_KEEP_SIZE | EXT4_FALLOC_PUNCH_HOLE, &update, &completed),
	    EXT4_OK);
	CHECK(completed == owned * device->block_size && fs->info.free_blocks == owned);
	for (index = 0; index < runs; index++) {
		EXPECT(ext4_fallocate(fs, inode->number, inode->generation,
			   (uint64_t)index * (blocks + 1U) * device->block_size,
			   (uint64_t)blocks * device->block_size, EXT4_FALLOC_KEEP_SIZE, &update,
			   &completed),
		    EXT4_OK);
		CHECK(completed == (uint64_t)blocks * device->block_size);
	}
	EXPECT(ext4_get_inode(fs, inode->number, inode), EXT4_OK);
	CHECK(inode->size == 0 && fs->info.free_blocks == 0 &&
	    inode->blocks_512 == owned * (device->block_size / EXT4_SECTOR_SIZE));
	EXPECT(ext4_sync(fs), EXT4_OK);
	ext4_unmount(fs);
	capacity_poison(device, inode);
}

static void
reservation_writes(struct device *device, bool external, const char *exports, const char *path)
{
	struct ext4_inode inode;
	struct ext4_fs *fs;
	struct ext4_inode_update update = attributes(NULL);
	uint32_t blocks = reservation_blocks(device, external);
	uint32_t runs = external ? (device->block_size - sizeof(struct ext4_extent_header_disk)) /
		    sizeof(struct ext4_extent_disk) -
		2U
				 : 3U;
	size_t length = (size_t)runs * (blocks + 1U) * device->block_size;
	uint8_t *expected = calloc(length, 1);
	uint64_t owned;
	size_t offset;
	size_t written;
	uint32_t index;
	uint32_t part;
	uint8_t value;

	CHECK(expected != NULL);
	reservation_seed(device, external, &inode);
	storage_export(device, exports, path, "reservation-reserved-");
	fs = mount_file(device, &inode);
	owned = inode.blocks_512;
	update.modify_time.seconds += 9;
	update.change_time.seconds += 9;
	for (index = 0; index < runs; index++) {
		for (part = 0; part < 2; part++) {
			offset = ((size_t)index * (blocks + 1U) + (part ? blocks / 2U : 0)) *
				device->block_size +
			    7U;
			value = (uint8_t)((part ? 0xd1U : 0xa1U) + index);
			EXPECT(ext4_write_partial(fs, inode.number, inode.generation, offset,
				   &value, 1, &update, &written),
			    EXT4_OK);
			CHECK(written == 1 && !fs->aborted && fs->info.free_blocks == 0);
			expected[offset] = value;
			contents(fs, &inode, expected, offset + 1U);
			CHECK(inode.blocks_512 == owned &&
			    inode.modify_time.seconds == RANGE_SECONDS + 9 &&
			    inode.change_time.seconds == RANGE_SECONDS + 10);
		}
	}
	EXPECT(ext4_sync(fs), EXT4_OK);
	storage_export(device, exports, path, "reservation-written-");
	ext4_unmount(fs);
	free(expected);
	puts("PASS reserved mapping capacity: partial EOF, repeated growth and extent transitions "
	     "on a full disk");
}

static void
reservation_reduced_capacity(struct device *device, bool external)
{
	struct ext4_inode inode;
	struct ext4_fs *fs;
	struct ext4_inode_disk *disk;
	struct ext4_extent_header_disk *header;
	const struct ext4_extent_index_disk *child;
	struct ext4_le32 *tail;
	struct ext4_inode_update update = attributes(NULL);
	uint64_t location;
	uint64_t block;
	uint64_t completed;
	size_t tail_offset;
	size_t written;
	uint8_t expected[8] = { 0 };

	reservation_seed(device, external, &inode);
	fs = mount_file(device, &inode);
	EXPECT(ext4_inode_location(fs, inode.number, &location), EXT4_OK);
	disk = (struct ext4_inode_disk *)(device->cache + location);
	header = (struct ext4_extent_header_disk *)disk->block_data;
	if (external) {
		CHECK(ext4_le16(&header->depth) == 1 && ext4_le16(&header->entries) == 1);
		child = (const struct ext4_extent_index_disk *)(header + 1);
		block = ext4_le32(&child->child_lo) | (uint64_t)ext4_le16(&child->child_hi) << 32;
		header =
		    (struct ext4_extent_header_disk *)(device->cache + block * device->block_size);
	}
	/* A valid imported leaf may advertise only its used entries, leaving
	 * physical room which reservation can recover without another block. */
	ext4_encode16(&header->maximum, ext4_le16(&header->entries));
	if (external && fs->metadata_checksum) {
		tail_offset =
		    sizeof(*header) + ext4_le16(&header->maximum) * sizeof(struct ext4_extent_disk);
		tail = (struct ext4_le32 *)((uint8_t *)header + tail_offset);
		ext4_encode32(tail, ext4_crc32c(ext4_inode_seed(fs, &inode), header, tail_offset));
	} else if (!external) {
		ext4_inode_checksum_set(fs, inode.number, disk);
	}
	memcpy(device->stable, device->cache, device->size);
	ext4_unmount(fs);
	fs = mount_file(device, &inode);
	EXPECT(ext4_fallocate(fs, inode.number, inode.generation, 0, device->block_size,
		   EXT4_FALLOC_KEEP_SIZE, &update, &completed),
	    EXT4_OK);
	CHECK(completed == device->block_size && fs->info.free_blocks == 0);
	expected[7] = 0xa1;
	EXPECT(ext4_write_partial(
		   fs, inode.number, inode.generation, 7, &expected[7], 1, &update, &written),
	    EXT4_OK);
	CHECK(written == 1 && fs->info.free_blocks == 0);
	contents(fs, &inode, expected, sizeof(expected));
	EXPECT(ext4_sync(fs), EXT4_OK);
	ext4_unmount(fs);
	puts("PASS reduced leaf capacity expands without allocating or making an empty child");
}

static void
reservation_partial_growth(struct device *device)
{
	struct ext4_inode inode;
	struct ext4_inode root;
	struct ext4_inode filler;
	struct ext4_fs *fs;
	struct ext4_inode_update update = attributes(NULL);
	uint32_t blocks = reservation_blocks(device, false);
	size_t length = ((size_t)2U * (blocks + 1U) + blocks / 2U) * device->block_size + 8U;
	uint8_t *expected = calloc(length, 1);
	uint64_t completed;
	size_t written;
	uint8_t value = 0xa1;

	CHECK(expected != NULL);
	reservation_seed(device, false, &inode);
	fs = mount_file(device, &inode);
	EXPECT(ext4_get_inode(fs, EXT4_ROOT_INODE, &root), EXT4_OK);
	EXPECT(ext4_lookup(fs, &root, (const uint8_t *)"filler", 6, &filler), EXT4_OK);
	EXPECT(ext4_fallocate(fs, filler.number, filler.generation,
		   (uint64_t)3U * blocks * device->block_size, device->block_size,
		   EXT4_FALLOC_KEEP_SIZE | EXT4_FALLOC_PUNCH_HOLE, &update, &completed),
	    EXT4_OK);
	CHECK(completed == device->block_size && fs->info.free_blocks == 1);
	/* A new run needs both data and metadata. The requested final EOF must
	 * not let an earlier checkpoint consume another reservation's slot. */
	EXPECT(ext4_fallocate(fs, inode.number, inode.generation,
		   (uint64_t)blocks * device->block_size,
		   (uint64_t)(2U * blocks + 3U) * device->block_size, 0, &update, &completed),
	    EXT4_NO_SPACE);
	CHECK(completed == 0 && fs->info.free_blocks == 1 && !fs->aborted);
	EXPECT(ext4_get_inode(fs, inode.number, &inode), EXT4_OK);
	CHECK(inode.size == 0);
	/* Another file consumes the final block; the original reservation still
	 * has enough mapping capacity to publish a partial initialized prefix. */
	EXPECT(ext4_write_partial(
		   fs, filler.number, filler.generation, 0, &value, 1, &update, &written),
	    EXT4_OK);
	CHECK(written == 1 && fs->info.free_blocks == 0);
	EXPECT(ext4_write_partial(
		   fs, inode.number, inode.generation, length - 1U, &value, 1, &update, &written),
	    EXT4_OK);
	CHECK(written == 1 && fs->info.free_blocks == 0);
	expected[length - 1U] = value;
	contents(fs, &inode, expected, length);
	EXPECT(ext4_sync(fs), EXT4_OK);
	ext4_unmount(fs);
	free(expected);
	puts("PASS failed growing reservation preserves earlier KEEP_SIZE mapping capacity");
}

static void
reservation_contract(struct device *device)
{
	struct ext4_inode inode;
	struct ext4_inode result;
	struct ext4_fs *fs;
	struct ext4_inode_update update = attributes(NULL);
	uint32_t blocks = 2U * EXT4_ORPHAN_BATCH_BLOCKS + 5U;
	size_t length = (size_t)3U * (blocks + 1U) * device->block_size;
	uint8_t *expected = calloc(length, 1);
	size_t offset = (size_t)(blocks / 2U) * device->block_size + 7U;
	size_t written;
	uint64_t completed;
	uint64_t owned;
	uint8_t value = 0xa1;

	CHECK(expected != NULL);
	reservation_seed(device, false, &inode);
	fs = mount_file(device, &inode);
	owned = inode.blocks_512;
	EXPECT(ext4_write_partial(
		   fs, inode.number, inode.generation, offset, &value, 1, &update, &written),
	    EXT4_OK);
	CHECK(written == 1);
	expected[offset] = value;
	offset += (size_t)(blocks + 1U) * device->block_size;
	EXPECT(ext4_truncate(fs, inode.number, inode.generation, offset + 1U, &update, &result),
	    EXT4_OK);
	contents(fs, &inode, expected, offset + 1U);
	/* A size-only growth can move EOF beyond the old split. Re-reservation
	 * and later writes must still find and reclaim that leaf record. */
	EXPECT(
	    ext4_fallocate(fs, inode.number, inode.generation,
		(uint64_t)(blocks + 1U) * device->block_size, (uint64_t)blocks * device->block_size,
		EXT4_FALLOC_KEEP_SIZE, &update, &completed),
	    EXT4_OK);
	CHECK(completed == (uint64_t)blocks * device->block_size);
	offset += 2U * device->block_size;
	value = 0xd1;
	EXPECT(ext4_write_partial(
		   fs, inode.number, inode.generation, offset, &value, 1, &update, &written),
	    EXT4_OK);
	CHECK(written == 1);
	expected[offset] = value;
	contents(fs, &inode, expected, offset + 1U);
	CHECK(fs->info.free_blocks == 0 && inode.blocks_512 == owned);
	/* A middle punch may spend its released data block on a mapping node.
	 * The untouched reservations must retain their future-write capacity. */
	EXPECT(ext4_fallocate(fs, inode.number, inode.generation, device->block_size,
		   device->block_size, EXT4_FALLOC_KEEP_SIZE | EXT4_FALLOC_PUNCH_HOLE, &update,
		   &completed),
	    EXT4_OK);
	CHECK(completed == device->block_size && fs->info.free_blocks == 0);
	offset = ((size_t)2U * (blocks + 1U) + blocks / 2U) * device->block_size + 7U;
	value = 0xe3;
	EXPECT(ext4_write_partial(
		   fs, inode.number, inode.generation, offset, &value, 1, &update, &written),
	    EXT4_OK);
	CHECK(written == 1);
	expected[offset] = value;
	contents(fs, &inode, expected, offset + 1U);
	CHECK(inode.blocks_512 == owned && fs->info.free_blocks == 0);
	EXPECT(ext4_truncate(fs, inode.number, inode.generation, 0, &update, &result), EXT4_OK);
	CHECK(result.size == 0 && result.blocks_512 == 0 &&
	    fs->info.free_blocks == owned / (device->block_size / EXT4_SECTOR_SIZE));
	EXPECT(ext4_sync(fs), EXT4_OK);
	ext4_unmount(fs);
	free(expected);
	puts("PASS reservation capacity survives size-only growth, re-reservation, punching and "
	     "final release");
}

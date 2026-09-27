/* SPDX-License-Identifier: BSD-3-Clause */
/* Packing limits, inode ownership and mutation with completely full bitmaps. */

static void
reject_unchanged(struct device *device, struct ext4_fs *fs, const struct ext4_inode *inode,
    const struct ext4_inode_update *update, enum ext4_result expected)
{
	struct ext4_inode result;
	struct ext4_inode untouched;
	uint8_t *before = malloc(device->size);
	uint64_t free_blocks = fs->info.free_blocks;
	uint32_t feature_compat = fs->info.feature_compat;
	uint32_t live = device->live;
	uint32_t writes = device->writes;
	uint32_t events = device->events;

	CHECK(before != NULL);
	memcpy(before, device->cache, device->size);
	memset(&untouched, 0xa5, sizeof(untouched));
	result = untouched;
	EXPECT(
	    ext4_set_attributes(fs, inode->number, inode->generation, update, &result), expected);
	CHECK(memcmp(&result, &untouched, sizeof(result)) == 0 &&
	    memcmp(before, device->cache, device->size) == 0 &&
	    fs->info.free_blocks == free_blocks && fs->info.feature_compat == feature_compat &&
	    device->live == live && device->writes == writes && device->events == events &&
	    !fs->aborted);
	free(before);
}

static void
capacity_cases(struct device *device, const char *exports, const char *path)
{
	struct ext4_fs *fs;
	struct ext4_inode inode;
	struct ext4_inode result;
	struct ext4_inode_update update;
	struct ext4_xattr_change batch[2];
	uint8_t *bytes = malloc(device->block_size);
	size_t size;
	uint64_t block;
	uint64_t free_blocks;

	CHECK(bytes != NULL);
	pattern(bytes, device->block_size, true);
	device_reset(device, device->base);
	fs = mount_writer(device);
	inode = lookup(fs, "capacity");
	block = attribute_block(device, fs, &inode);
	free_blocks = fs->info.free_blocks;
	EXPECT(ext4_get_xattr(fs, inode.number, inode.generation, EXT4_XATTR_USER,
		   (const uint8_t *)"full", 4, NULL, 0, &size),
	    EXT4_OK);
	batch[0] = change(EXT4_XATTR_REPLACE, EXT4_XATTR_USER, "full", bytes, size);
	update = attributes(batch, 1);
	EXPECT(ext4_set_attributes(fs, inode.number, inode.generation, &update, &result), EXT4_OK);
	CHECK(attribute_block(device, fs, &result) == block &&
	    result.blocks_512 == inode.blocks_512 && fs->info.free_blocks == free_blocks);
	value_is(fs, &result, EXT4_XATTR_USER, "full", bytes, size);
	batch[0].value_size += EXT4_XATTR_ALIGNMENT;
	reject_unchanged(device, fs, &inode, &update, EXT4_NO_SPACE);
	batch[0].value_size = size;
	batch[1] = change(EXT4_XATTR_CREATE, EXT4_XATTR_USER, "tiny", "abc", 3);
	update.xattr_count = 2;
	if (fs->inode_size == EXT4_INODE_BASE_SIZE) {
		reject_unchanged(device, fs, &inode, &update, EXT4_NO_SPACE);
	} else {
		EXPECT(ext4_set_attributes(fs, inode.number, inode.generation, &update, &result),
		    EXT4_OK);
		value_is(fs, &result, EXT4_XATTR_USER, "tiny", "abc", 3);
		CHECK(attribute_block(device, fs, &result) == block &&
		    fs->info.free_blocks == free_blocks);
	}
	EXPECT(ext4_sync(fs), EXT4_OK);
	ext4_unmount(fs);
	storage_export(device, exports, path, "xattr-capacity-");
	free(bytes);
	puts("PASS xattr maximum value, inode-body overflow and unchanged ENOSPC");
}

static void
packing_case(struct device *device, const char *exports, const char *path)
{
	struct ext4_fs *fs;
	struct ext4_inode inode;
	struct ext4_inode result;
	struct ext4_inode_disk *disk;
	struct ext4_xattr_change batch[4];
	struct ext4_inode_update update;
	uint8_t *bytes = malloc(device->block_size);
	size_t body;
	size_t capacity;
	size_t large;
	size_t index;
	uint64_t free_blocks;

	CHECK(bytes != NULL);
	pattern(bytes, device->block_size, true);
	device_reset(device, device->base);
	fs = mount_writer(device);
	if (fs->inode_size == EXT4_INODE_BASE_SIZE) {
		ext4_unmount(fs);
		free(bytes);
		puts("SKIP two-region xattr packing: 128-byte inodes have no attribute body");
		return;
	}
	inode = lookup(fs, "plain");
	disk = record(device, fs, &inode);
	body = fs->inode_size - EXT4_INODE_BASE_SIZE - ext4_le16(&disk->extra_size) -
	    2 * sizeof(struct ext4_le32);
	capacity =
	    device->block_size - sizeof(struct ext4_xattr_header_disk) - sizeof(struct ext4_le32);
	/* Three small records cost body-36, body/2 and body/2. Only the last
	 * two together leave enough external space for the fourth record. */
	CHECK(body >= 88 && body % (2 * EXT4_XATTR_ALIGNMENT) == 0);
	large = body - 36;
	batch[0] = change(EXT4_XATTR_CREATE, EXT4_XATTR_USER, "a", bytes,
	    large - sizeof(struct ext4_xattr_entry_disk) - EXT4_XATTR_ALIGNMENT);
	batch[1] = change(EXT4_XATTR_CREATE, EXT4_XATTR_USER, "b", bytes,
	    body / 2 - sizeof(struct ext4_xattr_entry_disk) - EXT4_XATTR_ALIGNMENT);
	batch[2] = batch[1];
	batch[2].name = (const uint8_t *)"c";
	batch[3] = change(EXT4_XATTR_CREATE, EXT4_XATTR_USER, "large", bytes,
	    capacity - large - sizeof(struct ext4_xattr_entry_disk) - 2 * EXT4_XATTR_ALIGNMENT);
	update = attributes(batch, 4);
	free_blocks = fs->info.free_blocks;
	EXPECT(ext4_set_attributes(fs, inode.number, inode.generation, &update, &result), EXT4_OK);
	CHECK(fs->info.free_blocks == free_blocks - 1 &&
	    result.blocks_512 == device->block_size / EXT4_SECTOR_SIZE);
	count_is(fs, &result, 4);
	for (index = 0; index < 4; index++) {
		value_is(fs, &result, EXT4_XATTR_USER, (const char *)batch[index].name, bytes,
		    batch[index].value_size);
		batch[index].policy = EXT4_XATTR_REPLACE;
	}
	batch[0].value_size += EXT4_XATTR_ALIGNMENT;
	reject_unchanged(device, fs, &inode, &update, EXT4_NO_SPACE);
	EXPECT(ext4_sync(fs), EXT4_OK);
	ext4_unmount(fs);
	storage_export(device, exports, path, "xattr-packed-");
	free(bytes);
	puts("PASS xattr two-region exact packing where a greedy placement cannot fit");
}

static void
ownership_cases(struct device *device)
{
	struct ext4_fs *fs;
	struct ext4_inode inode;
	struct ext4_inode_disk *disk;
	struct ext4_inode_update update;
	struct ext4_xattr_change item;
	struct ext4_xattr_header_disk *header;
	struct ext4_block_number_disk address;
	struct ext4_group group;
	uint64_t block;
	uint32_t checksum;
	unsigned int damage;

	item = change(EXT4_XATTR_CREATE, EXT4_XATTR_USER, "new", "abc", 3);
	update = attributes(&item, 1);
	for (damage = 0; damage < 5; damage++) {
		device_reset(device, device->base);
		fs = mount_writer(device);
		inode = lookup(fs, damage == 1 ? "mapped-symlink" : "block");
		disk = record(device, fs, &inode);
		block = attribute_block(device, fs, &inode);
		switch (damage) {
		case 0:
			ext4_encode16(&disk->mode, EXT4_MODE_CHARACTER | EXT4_MODE_FIFO | 0600);
			break;
		case 1:
			EXPECT(ext4_map_block(fs, &inode, 0, &block), EXT4_OK);
			ext4_encode32(&disk->xattr_block_lo, (uint32_t)block);
			ext4_encode16(&disk->xattr_block_hi, (uint16_t)(block >> 32));
			break;
		case 2:
			header = (struct ext4_xattr_header_disk *)(device->cache +
			    block * device->block_size);
			ext4_encode32(&header->references, 0);
			if (fs->metadata_checksum) {
				ext4_encode32(&header->checksum, 0);
				ext4_encode32(&address.low, (uint32_t)block);
				ext4_encode32(&address.high, (uint32_t)(block >> 32));
				checksum =
				    ext4_crc32c(fs->checksum_seed, &address, sizeof(address));
				ext4_encode32(&header->checksum,
				    ext4_crc32c(checksum, header, device->block_size));
			}
			break;
		case 3:
			EXPECT(ext4_group_get(fs, 0, &group), EXT4_OK);
			ext4_encode32(&disk->xattr_block_lo, (uint32_t)group.inode_table);
			ext4_encode16(&disk->xattr_block_hi, (uint16_t)(group.inode_table >> 32));
			break;
		case 4:
			ext4_encode32(&disk->blocks_lo,
			    ext4_le32(&disk->blocks_lo) + device->block_size / EXT4_SECTOR_SIZE);
			break;
		}
		ext4_inode_checksum_set(fs, inode.number, disk);
		reject_unchanged(device, fs, &inode, &update, EXT4_CORRUPT);
		ext4_unmount(fs);
	}
	device_reset(device, device->base);
	fs = mount_writer(device);
	inode = lookup(fs, "block");
	item = change(EXT4_XATTR_REPLACE, EXT4_XATTR_USER, "binary", "abc", 3);
	update.change_time.nanoseconds = EXT4_NANOSECONDS_PER_SECOND;
	reject_unchanged(device, fs, &inode, &update, EXT4_RANGE);
	ext4_unmount(fs);
	puts("PASS xattr ownership: corrupt type, map alias, refcount, protected block, accounting "
	     "and post-allocation timestamp rejection");
}

static void
full_space(struct device *device, const char *exports, const char *path)
{
	struct ext4_fs *fs;
	struct ext4_inode inode;
	struct ext4_inode shared;
	struct ext4_inode result;
	struct ext4_inode_update update;
	struct ext4_xattr_change item;
	uint8_t *bytes = malloc(device->block_size);
	uint8_t original[XATTR_BINARY_BYTES];
	uint8_t *before = malloc(device->block_size);
	uint64_t block;
	size_t size;

	CHECK(bytes != NULL && before != NULL);
	pattern(bytes, device->block_size, true);
	pattern(original, sizeof(original), false);
	device_reset(device, device->base);
	fs = mount_writer(device);
	CHECK(fs->info.free_blocks == 0);
	inode = lookup(fs, "block");
	shared = lookup(fs, "shared");
	block = attribute_block(device, fs, &inode);
	memcpy(before, device->cache + block * device->block_size, device->block_size);
	item = change(EXT4_XATTR_REPLACE, EXT4_XATTR_USER, "binary", original, sizeof(original));
	update = attributes(&item, 1);
	EXPECT(ext4_set_attributes(fs, inode.number, inode.generation, &update, &result), EXT4_OK);
	CHECK(memcmp(before, device->cache + block * device->block_size, device->block_size) == 0);
	item = change(EXT4_XATTR_CREATE, EXT4_XATTR_USER, "tiny", "abc", 3);
	if (fs->inode_size == EXT4_INODE_BASE_SIZE) {
		reject_unchanged(device, fs, &inode, &update, EXT4_NO_SPACE);
	} else {
		EXPECT(ext4_set_attributes(fs, inode.number, inode.generation, &update, &result),
		    EXT4_OK);
		CHECK(memcmp(before, device->cache + block * device->block_size,
			  device->block_size) == 0);
	}
	item =
	    change(EXT4_XATTR_REPLACE, EXT4_XATTR_USER, "binary", bytes, XATTR_REPLACEMENT_BYTES);
	reject_unchanged(device, fs, &inode, &update, EXT4_NO_SPACE);
	CHECK(references(device, block) == 2 && fs->info.free_blocks == 0);
	value_is(fs, &shared, EXT4_XATTR_USER, "binary", original, sizeof(original));
	inode = lookup(fs, "plain");
	item.policy = EXT4_XATTR_CREATE;
	reject_unchanged(device, fs, &inode, &update, EXT4_NO_SPACE);
	item = change(EXT4_XATTR_CREATE, EXT4_XATTR_USER, "tiny", "abc", 3);
	if (fs->inode_size == EXT4_INODE_BASE_SIZE) {
		reject_unchanged(device, fs, &inode, &update, EXT4_NO_SPACE);
	} else {
		EXPECT(ext4_set_attributes(fs, inode.number, inode.generation, &update, &result),
		    EXT4_OK);
	}
	inode = lookup(fs, "capacity");
	EXPECT(ext4_get_xattr(fs, inode.number, inode.generation, EXT4_XATTR_USER,
		   (const uint8_t *)"full", 4, NULL, 0, &size),
	    EXT4_OK);
	item = change(EXT4_XATTR_REPLACE, EXT4_XATTR_USER, "full", bytes, size);
	EXPECT(ext4_set_attributes(fs, inode.number, inode.generation, &update, &result), EXT4_OK);
	inode = lookup(fs, "mapped-symlink");
	item =
	    change(EXT4_XATTR_REPLACE, EXT4_XATTR_USER, "binary", bytes, XATTR_REPLACEMENT_BYTES);
	EXPECT(ext4_set_attributes(fs, inode.number, inode.generation, &update, &result), EXT4_OK);
	CHECK(fs->info.free_blocks == 0);
	inode = lookup(fs, "symlink");
	item = change(EXT4_XATTR_REMOVE, EXT4_XATTR_USER, "binary", NULL, 0);
	EXPECT(ext4_set_attributes(fs, inode.number, inode.generation, &update, &result), EXT4_OK);
	CHECK(result.fast_symlink && result.blocks_512 == 0 && fs->info.free_blocks == 1);
	inode = lookup(fs, "block");
	item =
	    change(EXT4_XATTR_REPLACE, EXT4_XATTR_USER, "binary", bytes, XATTR_REPLACEMENT_BYTES);
	EXPECT(ext4_set_attributes(fs, inode.number, inode.generation, &update, &result), EXT4_OK);
	CHECK(fs->info.free_blocks == 0 && references(device, block) == 1 &&
	    attribute_block(device, fs, &result) != block);
	value_is(fs, &result, EXT4_XATTR_USER, "binary", bytes, XATTR_REPLACEMENT_BYTES);
	value_is(fs, &shared, EXT4_XATTR_USER, "binary", original, sizeof(original));
	item = change(EXT4_XATTR_REMOVE, EXT4_XATTR_USER, "binary", NULL, 0);
	EXPECT(ext4_set_attributes(fs, inode.number, inode.generation, &update, &result), EXT4_OK);
	EXPECT(
	    ext4_set_attributes(fs, shared.number, shared.generation, &update, &result), EXT4_OK);
	CHECK(result.blocks_512 == 0 && fs->info.free_blocks == 2);
	EXPECT(ext4_sync(fs), EXT4_OK);
	ext4_unmount(fs);
	storage_export(device, exports, path, "xattr-full-written-");
	free(before);
	free(bytes);
	puts("PASS full disk xattrs: unchanged/body-only writes, COW rollback, in-place "
	     "replacement and freed-block reuse");
}

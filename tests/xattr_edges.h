/* SPDX-License-Identifier: BSD-3-Clause */
/* Inode-body validation and value/name representations independent of policy. */

enum xattr_body_damage {
	XATTR_BODY_NO_TERMINATOR,
	XATTR_BODY_NAME_OVERFLOW,
	XATTR_BODY_VALUE_OVERLAP,
	XATTR_BODY_DUPLICATE_EXTERNAL,
	XATTR_BODY_DAMAGE_COUNT
};

static void
body_corruption(struct device *device, struct ext4_fs *fs)
{
	struct ext4_inode inode = lookup(fs, "/body");
	struct ext4_inode external = lookup(fs, "/block");
	struct ext4_inode_disk *disk;
	struct ext4_inode_disk *external_disk;
	struct ext4_xattr_header_disk *header;
	struct ext4_xattr_entry_disk *entry;
	uint8_t *output = malloc(XATTR_TEST_CASES * sizeof(struct ext4_xattr_key));
	uint64_t inode_offset;
	uint64_t external_offset;
	uint64_t block;
	size_t body;
	size_t length;
	size_t returned;
	uint32_t live = device->live;
	unsigned int list;
	enum xattr_body_damage damage;

	CHECK(output != NULL);
	if (fs->inode_size == EXT4_INODE_BASE_SIZE) {
		free(output);
		puts("SKIP xattr inode-body guards: 128-byte inodes have no attribute body");
		return;
	}
	EXPECT(ext4_inode_location(fs, inode.number, &inode_offset), EXT4_OK);
	EXPECT(ext4_inode_location(fs, external.number, &external_offset), EXT4_OK);
	for (damage = XATTR_BODY_NO_TERMINATOR; damage < XATTR_BODY_DAMAGE_COUNT; damage++) {
		memcpy(device->cache, device->base, device->size);
		disk = (struct ext4_inode_disk *)(device->cache + inode_offset);
		body = EXT4_INODE_BASE_SIZE + ext4_le16(&disk->extra_size);
		CHECK(ext4_le32((const struct ext4_le32 *)((uint8_t *)disk + body)) ==
		    EXT4_XATTR_MAGIC);
		entry = (struct ext4_xattr_entry_disk *)((uint8_t *)disk + body +
		    sizeof(struct ext4_le32));
		switch (damage) {
		case XATTR_BODY_NO_TERMINATOR:
			body = fs->inode_size - sizeof(struct ext4_le32);
			ext4_encode16(&disk->extra_size, (uint16_t)(body - EXT4_INODE_BASE_SIZE));
			ext4_encode32(
			    (struct ext4_le32 *)((uint8_t *)disk + body), EXT4_XATTR_MAGIC);
			break;
		case XATTR_BODY_NAME_OVERFLOW:
			body = fs->inode_size - sizeof(struct ext4_le32) - sizeof(*entry) -
			    EXT4_XATTR_ALIGNMENT;
			ext4_encode16(&disk->extra_size, (uint16_t)(body - EXT4_INODE_BASE_SIZE));
			memset((uint8_t *)disk + body, 0, fs->inode_size - body);
			ext4_encode32(
			    (struct ext4_le32 *)((uint8_t *)disk + body), EXT4_XATTR_MAGIC);
			entry = (struct ext4_xattr_entry_disk *)((uint8_t *)disk + body +
			    sizeof(struct ext4_le32));
			entry->name_index = EXT4_XATTR_USER;
			entry->name_length = EXT4_NAME_MAX;
			break;
		case XATTR_BODY_VALUE_OVERLAP:
			ext4_encode16(&entry->value_offset, 0);
			ext4_encode32(&entry->value_size, 1);
			ext4_encode32(&entry->hash, 0);
			break;
		case XATTR_BODY_DUPLICATE_EXTERNAL:
			external_disk = (struct ext4_inode_disk *)(device->cache + external_offset);
			disk->xattr_block_lo = external_disk->xattr_block_lo;
			disk->xattr_block_hi = external_disk->xattr_block_hi;
			disk->blocks_lo = external_disk->blocks_lo;
			disk->blocks_hi = external_disk->blocks_hi;
			block = ext4_le32(&disk->xattr_block_lo) |
			    ((uint64_t)ext4_le16(&disk->xattr_block_hi) << 32);
			header = (struct ext4_xattr_header_disk *)(device->cache +
			    block * fs->info.block_size);
			ext4_encode32(&header->references, 3);
			block_checksum(fs, block, header);
			entry->name_index = EXT4_XATTR_USER;
			entry->name_length = 6;
			memcpy(entry + 1, "binary", entry->name_length);
			length = (sizeof(*entry) + entry->name_length + EXT4_XATTR_ALIGNMENT - 1) &
			    ~(size_t)(EXT4_XATTR_ALIGNMENT - 1);
			memset((uint8_t *)entry + length, 0, sizeof(struct ext4_le32));
			ext4_encode32(&entry->hash, 0);
			break;
		case XATTR_BODY_DAMAGE_COUNT:
			break;
		}
		ext4_inode_checksum_set(fs, inode.number, disk);
		for (list = 0; list < 2; list++) {
			returned = SIZE_MAX;
			memset(output, 0xa5, XATTR_TEST_CASES * sizeof(struct ext4_xattr_key));
			EXPECT(inspect(fs, &inode, list != 0, output, &returned), EXT4_CORRUPT);
			CHECK(returned == SIZE_MAX && device->live == live && !fs->aborted);
			sentinel(output, XATTR_TEST_CASES * sizeof(struct ext4_xattr_key));
		}
	}
	memcpy(device->cache, device->base, device->size);
	free(output);
	printf("PASS xattr inode-body guards: %u cases reject get/list before output\n",
	    XATTR_BODY_DAMAGE_COUNT);
}

static void
positive_edges(struct device *device, struct ext4_fs *fs, const uint8_t *expected,
    const char *exports, const char *path)
{
	struct ext4_inode inode = lookup(fs, "/block");
	struct ext4_inode_disk *disk;
	struct ext4_xattr_header_disk *header;
	struct ext4_xattr_entry_disk *entry;
	struct ext4_xattr_entry_disk *second;
	struct ext4_xattr_key keys[2];
	uint8_t value[XATTR_TEST_BINARY_BYTES];
	uint64_t offset;
	uint64_t block;
	size_t length;
	size_t returned;

	EXPECT(ext4_inode_location(fs, inode.number, &offset), EXT4_OK);
	disk = (struct ext4_inode_disk *)(device->cache + offset);
	block =
	    ext4_le32(&disk->xattr_block_lo) | ((uint64_t)ext4_le16(&disk->xattr_block_hi) << 32);
	header = (struct ext4_xattr_header_disk *)(device->cache + block * fs->info.block_size);
	entry = (struct ext4_xattr_entry_disk *)(header + 1);
	CHECK(entry->name_length == 6 && ext4_le32(&entry->value_size) == XATTR_TEST_BINARY_BYTES);
	length = (sizeof(*entry) + entry->name_length + EXT4_XATTR_ALIGNMENT - 1) &
	    ~(size_t)(EXT4_XATTR_ALIGNMENT - 1);
	ext4_encode32(&entry->value_size, XATTR_TEST_SHARED_VALUE_BYTES);
	ext4_encode32(&entry->hash, 0);
	second = (struct ext4_xattr_entry_disk *)((uint8_t *)entry + length);
	memcpy(second, entry, length);
	memcpy(second + 1, "second", 6);
	memset((uint8_t *)second + length, 0, sizeof(struct ext4_le32));
	ext4_encode32(&header->hash, 0);
	block_checksum(fs, block, header);
	EXPECT(ext4_get_xattr(fs, inode.number, inode.generation, EXT4_XATTR_USER,
		   (const uint8_t *)"second", 6, value, sizeof(value), &returned),
	    EXT4_OK);
	CHECK(returned == XATTR_TEST_SHARED_VALUE_BYTES && memcmp(value, expected, returned) == 0);
	EXPECT(ext4_list_xattrs(fs, inode.number, inode.generation, keys, 2, &returned), EXT4_OK);
	CHECK(returned == 2 && keys[0].value_size == XATTR_TEST_SHARED_VALUE_BYTES &&
	    keys[1].value_size == XATTR_TEST_SHARED_VALUE_BYTES &&
	    memcmp(keys[0].name, "binary", 6) == 0 && memcmp(keys[1].name, "second", 6) == 0);
	if (exports != NULL) {
		memcpy(device->stable, device->cache, device->size);
		storage_export(device, exports, path, "xattr-shared-values-");
	}
	memcpy(device->cache, device->base, device->size);
	/* An unknown namespace is opaque storage; visibility remains adapter policy. */
	entry->name_index = XATTR_TEST_UNKNOWN_NAMESPACE;
	block_checksum(fs, block, header);
	EXPECT(ext4_get_xattr(fs, inode.number, inode.generation, XATTR_TEST_UNKNOWN_NAMESPACE,
		   (const uint8_t *)"binary", 6, value, sizeof(value), &returned),
	    EXT4_OK);
	CHECK(returned == XATTR_TEST_BINARY_BYTES && memcmp(value, expected, returned) == 0);
	EXPECT(ext4_list_xattrs(fs, inode.number, inode.generation, keys, 2, &returned), EXT4_OK);
	CHECK(returned == 1 && keys[0].name_index == XATTR_TEST_UNKNOWN_NAMESPACE &&
	    keys[0].name_length == 6);
	if (exports != NULL) {
		memcpy(device->stable, device->cache, device->size);
		storage_export(device, exports, path, "xattr-unknown-namespace-");
	}
	memcpy(device->cache, device->base, device->size);
	memcpy(device->stable, device->base, device->size);
	puts("PASS xattr identical value ranges and opaque unknown namespace");
}

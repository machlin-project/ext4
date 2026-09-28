/* SPDX-License-Identifier: BSD-3-Clause */
#ifndef MACHLIN_EXT4_TEST_CLUSTER_VALUES_H
#define MACHLIN_EXT4_TEST_CLUSTER_VALUES_H

static void
private_values(struct device *device, const char *exports, const char *path)
{
	struct ext4_fs *fs;
	struct ext4_inode inode;
	struct ext4_inode result;
	struct ext4_inode_update update = attributes(false);
	struct ext4_xattr_change change = { 0 };
	uint8_t *bytes;
	uint8_t *observed;
	uint64_t blocks;
	uint64_t sectors;
	uint32_t inodes;
	size_t cluster;
	size_t size;
	size_t returned;

	device_reset(device, device->base);
	fs = mount_writer(device);
	CHECK(fs->info.feature_incompat & EXT4_FEATURE_INCOMPAT_EA_INODE);
	cluster = (size_t)fs->cluster_blocks * device->block_size;
	size = 2U * cluster + 37U;
	bytes = malloc(size);
	observed = malloc(size + 1U);
	CHECK(bytes != NULL && observed != NULL);
	pattern(bytes, size);
	memset(observed, 0x5a, size + 1U);
	blocks = fs->info.free_blocks;
	inodes = fs->info.free_inodes;
	inode = lookup(fs, "file");
	sectors = inode.blocks_512;
	change.policy = EXT4_XATTR_CREATE;
	change.name_index = EXT4_XATTR_USER;
	change.name = (const uint8_t *)"large";
	change.name_length = 5;
	change.value = bytes;
	change.value_size = size;
	update.xattrs = &change;
	update.xattr_count = 1;
	EXPECT(ext4_set_attributes(fs, inode.number, inode.generation, &update, &result), EXT4_OK);
	CHECK(fs->info.free_blocks == blocks - 3U * fs->cluster_blocks &&
	    fs->info.free_inodes == inodes - 1U &&
	    result.blocks_512 == sectors + 3U * cluster / EXT4_SECTOR_SIZE);
	EXPECT(ext4_get_xattr(fs, inode.number, inode.generation, EXT4_XATTR_USER,
		   (const uint8_t *)"large", 5, observed, size + 1U, &returned),
	    EXT4_OK);
	CHECK(returned == size && memcmp(bytes, observed, size) == 0 && observed[size] == 0x5a);
	EXPECT(ext4_sync(fs), EXT4_OK);
	storage_export(device, exports, path, "cluster-values-");
	change.policy = EXT4_XATTR_REPLACE;
	change.value_size = 7;
	EXPECT(ext4_set_attributes(fs, inode.number, inode.generation, &update, &result), EXT4_OK);
	CHECK(fs->info.free_blocks == blocks && fs->info.free_inodes == inodes &&
	    result.blocks_512 == sectors);
	EXPECT(ext4_get_xattr(fs, inode.number, inode.generation, EXT4_XATTR_USER,
		   (const uint8_t *)"large", 5, observed, size + 1U, &returned),
	    EXT4_OK);
	CHECK(returned == 7 && memcmp(bytes, observed, 7) == 0);
	finish(device, fs, exports, path, "cluster-values-reclaimed-");
	free(observed);
	free(bytes);
	puts("PASS cluster private values: whole-value bytes, logical charges and staged "
	     "reclamation");
}

#endif

/* SPDX-License-Identifier: BSD-3-Clause */
#ifndef MACHLIN_EXT4_TEST_CLUSTER_CORRUPTION_H
#define MACHLIN_EXT4_TEST_CLUSTER_CORRUPTION_H

static void
corruption(struct device *device)
{
	struct ext4_fs *fs;
	struct ext4_inode inode;
	struct ext4_inode_disk *disk;
	struct ext4_extent_header_disk *header;
	struct ext4_extent_disk *extents;
	struct ext4_inode_update update = attributes(false);
	uint8_t byte;
	uint64_t location;
	uint32_t physical;
	uint16_t length;
	uint16_t split;
	unsigned int mode;
	size_t completed;

	for (mode = 0; mode < 5; mode++) {
		device_reset(device, device->base);
		fs = mount_writer(device);
		inode = lookup(fs, "file");
		EXPECT(ext4_inode_location(fs, inode.number, &location), EXT4_OK);
		disk = (struct ext4_inode_disk *)(device->cache + location);
		header = (struct ext4_extent_header_disk *)disk->block_data;
		extents = (struct ext4_extent_disk *)(header + 1);
		CHECK(ext4_le16(&header->depth) == 0 && ext4_le16(&header->entries) == 1);
		physical = ext4_le32(&extents[0].physical_lo);
		length = ext4_le16(&extents[0].length);
		switch (mode) {
		case 0:
			ext4_encode32(&extents[0].physical_lo, physical + 1U);
			break;
		case 1:
		case 2:
			split = mode == 1 ? 1 : (uint16_t)fs->cluster_blocks;
			CHECK(length > split);
			extents[1] = extents[0];
			ext4_encode16(&header->entries, 2);
			ext4_encode16(&extents[0].length, split);
			ext4_encode32(&extents[1].logical, split);
			ext4_encode16(&extents[1].length, length - split);
			ext4_encode32(&extents[1].physical_lo,
			    mode == 1 ? physical + fs->cluster_blocks + split : physical);
			break;
		case 3:
			ext4_encode32(&disk->blocks_lo, ext4_le32(&disk->blocks_lo) - 1U);
			break;
		case 4:
			ext4_encode32(&disk->xattr_block_lo, physical + 1U);
			break;
		}
		ext4_inode_checksum_set(fs, inode.number, disk);
		EXPECT(ext4_get_inode(fs, inode.number, &inode), EXT4_OK);
		byte = 0x5a;
		if (mode == 0) {
			EXPECT(ext4_read(fs, &inode, 0, &byte, 1, &completed), EXT4_CORRUPT);
			CHECK(completed == 0 && byte == 0x5a);
		}
		EXPECT(ext4_write(
			   fs, inode.number, inode.generation, 0, &byte, 1, &update, &completed),
		    EXT4_CORRUPT);
		CHECK(completed == 0 && device->writes == 0);
		ext4_unmount(fs);
		CHECK(device->live == 0);
	}
	puts("PASS cluster corruption: alignment, conflicting backing, duplicate cluster, "
	     "sector accounting and attribute alias reject before writes");
}

static void
geometry_corruption(struct device *device)
{
	struct ext4_fs *fs;
	struct ext4_super_disk *super;
	uint32_t block_log;
	unsigned int mode;
	enum ext4_result expected;

	for (mode = 0; mode < 6; mode++) {
		device_reset(device, device->base);
		super = (struct ext4_super_disk *)(device->cache + EXT4_SUPER_OFFSET);
		block_log = ext4_le32(&super->log_block_size);
		expected = EXT4_CORRUPT;
		switch (mode) {
		case 0:
			ext4_encode32(&super->log_cluster_size, block_log + 16U);
			expected = EXT4_UNSUPPORTED;
			break;
		case 1:
			ext4_encode32(&super->clusters_per_group, 0);
			break;
		case 2:
			ext4_encode32(
			    &super->blocks_per_group, ext4_le32(&super->blocks_per_group) + 1U);
			break;
		case 3:
			ext4_encode32(&super->feature_ro_compat,
			    ext4_le32(&super->feature_ro_compat) & ~EXT4_FEATURE_RO_BIGALLOC);
			break;
		case 4:
			ext4_encode32(&super->feature_incompat,
			    ext4_le32(&super->feature_incompat) & ~EXT4_FEATURE_INCOMPAT_EXTENTS);
			break;
		case 5:
			ext4_encode32(
			    &super->free_blocks_lo, ext4_le32(&super->free_blocks_lo) - 1U);
			break;
		}
		if (device->metadata_checksum) {
			ext4_encode32(&super->checksum,
			    ext4_crc32c(
				UINT32_MAX, super, offsetof(struct ext4_super_disk, checksum)));
		}
		EXPECT(ext4_mount(&device->environment, &fs), expected);
		CHECK(fs == NULL && device->live == 0 && device->writes == 0);
	}
	puts("PASS cluster geometry: six checksummed invalid geometries reject mounting");
}

#endif

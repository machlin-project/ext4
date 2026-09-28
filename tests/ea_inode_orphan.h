/* SPDX-License-Identifier: BSD-3-Clause */
#ifndef MACHLIN_EXT4_TEST_EA_INODE_ORPHAN_H
#define MACHLIN_EXT4_TEST_EA_INODE_ORPHAN_H

static void
value_orphan(struct device *device, const char *exports, const char *path)
{
	struct ext4_fs *fs;
	struct ext4_inode owner;
	struct ext4_inode result;
	struct ext4_inode_disk *value;
	struct ext4_inode_disk *parent;
	struct ext4_super_disk *super;
	struct ext4_recovery_report report;
	struct ext4_xattr_change edit = change(EXT4_XATTR_REMOVE, "maximum", NULL, 0);
	struct ext4_inode_update update = attributes(&edit, 1, false);
	uint8_t *expected = malloc(device->size);
	uint64_t location;
	uint64_t attribute_block;
	uint32_t number;
	uint16_t inode_size;

	CHECK(expected != NULL);
	device_reset(device, device->base);
	fs = mount_writer(device);
	owner = lookup(fs, "body");
	number = value_number(fs, &owner, "maximum");
	CHECK(number != 0);
	inode_size = fs->inode_size;
	EXPECT(ext4_inode_location(fs, owner.number, &location), EXT4_OK);
	EXPECT(ext4_set_attributes(fs, owner.number, owner.generation, &update, &result), EXT4_OK);
	EXPECT(ext4_sync(fs), EXT4_OK);
	ext4_unmount(fs);
	memcpy(expected, device->stable, device->size);
	device_reset(device, device->base);
	EXPECT(ext4_mount(&device->environment, &fs), EXT4_OK);
	parent = record(device, fs, owner.number);
	attribute_block = ext4_le32(&parent->xattr_block_lo) |
	    ((uint64_t)ext4_le16(&parent->xattr_block_hi) << 32);
	/* Retain the value's original allocation after its owner drops the entry,
	 * as Linux can do when the zero-reference inode is on the orphan chain. */
	memcpy(device->cache + location, expected + location, inode_size);
	if (attribute_block != 0) {
		CHECK(attribute_block ==
		    (ext4_le32(&parent->xattr_block_lo) |
			((uint64_t)ext4_le16(&parent->xattr_block_hi) << 32)));
		memcpy(device->cache + attribute_block * device->block_size,
		    expected + attribute_block * device->block_size, device->block_size);
	}
	value = record(device, fs, number);
	ext4_encode16(&value->links, 0);
	ext4_encode32(&value->version_lo, 0);
	ext4_encode32(&value->change_time, 0);
	ext4_encode32(&value->deletion_time, 0);
	ext4_inode_checksum_set(fs, number, value);
	super = (struct ext4_super_disk *)(device->cache + EXT4_SUPER_OFFSET);
	ext4_encode32(&super->last_orphan, number);
	ext4_encode32(&super->feature_incompat,
	    ext4_le32(&super->feature_incompat) | EXT4_FEATURE_INCOMPAT_RECOVER);
	if (fs->metadata_checksum) {
		ext4_encode32(&super->checksum,
		    ext4_crc32c(UINT32_MAX, super, offsetof(struct ext4_super_disk, checksum)));
	}
	ext4_unmount(fs);
	memcpy(device->stable, device->cache, device->size);
	storage_export(device, exports, path, "ea-value-orphan-pending-");
	EXPECT(ext4_recover(&device->environment, &device->writer, &report), EXT4_OK);
	CHECK(report.cleaned_orphans == 1 && report.orphan_transactions != 0 && device->live == 0 &&
	    storage_equal(device, expected));
	storage_export(device, exports, path, "ea-value-orphan-clean-");
	free(expected);
	puts("PASS EA_INODE zero-reference private orphan recovery and exact allocation "
	     "reclamation");
}

#endif

/* SPDX-License-Identifier: BSD-3-Clause */
#ifndef MACHLIN_EXT4_TEST_INLINE_LIFETIME_H
#define MACHLIN_EXT4_TEST_INLINE_LIFETIME_H

static void
creation_lifetime(struct device *device, const char *exports, const char *path, bool full)
{
	struct ext4_fs *fs;
	struct ext4_inode parent;
	struct ext4_inode file;
	struct ext4_inode directory;
	struct ext4_inode root;
	struct ext4_inode result;
	struct ext4_inode_hold *hold;
	struct ext4_inode_update create = attributes(true);
	struct ext4_inode_update update = attributes(false);
	struct ext4_timestamp time = { INLINE_TEST_SECONDS, 0 };
	uint8_t bytes[120];
	uint8_t long_name[EXT4_NAME_MAX];
	uint64_t free_blocks;
	uint32_t free_inodes;
	uint32_t writes;
	size_t completed;

	device_reset(device, device->base);
	fs = mount_writer(device);
	free_blocks = fs->info.free_blocks;
	free_inodes = fs->info.free_inodes;
	CHECK(!full || free_blocks == 0);
	parent = lookup(fs, "empty");
	pattern(bytes, sizeof(bytes));
	EXPECT(ext4_create(fs, parent.number, parent.generation, (const uint8_t *)"file", 4,
		   &create, &time, &file),
	    EXT4_OK);
	CHECK(file.flags & EXT4_INODE_INLINE_DATA);
	EXPECT(ext4_write(
		   fs, file.number, file.generation, 0, bytes, sizeof(bytes), &update, &completed),
	    EXT4_OK);
	file = find(fs, &parent, "file");
	CHECK(file.flags & EXT4_INODE_INLINE_DATA);
	bytes_are(fs, &file, bytes, sizeof(bytes));
	EXPECT(ext4_mkdir(fs, parent.number, parent.generation, (const uint8_t *)"dir", 3, &create,
		   &time, &directory),
	    EXT4_OK);
	CHECK(directory.flags & EXT4_INODE_INLINE_DATA);
	result = find(fs, &directory, "..");
	CHECK(result.number == parent.number);
	CHECK(fs->info.free_blocks == free_blocks);
	if (full) {
		memset(long_name, 'x', sizeof(long_name));
		writes = device->writes;
		EXPECT(ext4_create(fs, parent.number, parent.generation, long_name,
			   sizeof(long_name), &create, &time, &result),
		    EXT4_NO_SPACE);
		CHECK(device->writes == writes && fs->info.free_blocks == 0);
	}
	EXPECT(ext4_sync(fs), EXT4_OK);
	storage_export(device, exports, path, "inline-new-");
	EXPECT(ext4_hold_inode(fs, file.number, file.generation, &hold), EXT4_OK);
	EXPECT(ext4_unlink(fs, parent.number, parent.generation, (const uint8_t *)"file", 4,
		   file.number, file.generation, &time, &result),
	    EXT4_OK);
	EXPECT(ext4_refresh_inode(hold, &file), EXT4_OK);
	CHECK(file.links == 0 && (file.flags & EXT4_INODE_INLINE_DATA));
	bytes_are(fs, &file, bytes, sizeof(bytes));
	EXPECT(ext4_truncate(fs, file.number, file.generation, 1, &update, &result), EXT4_OK);
	EXPECT(
	    ext4_write(fs, file.number, file.generation, 119, bytes + 119, 1, &update, &completed),
	    EXT4_OK);
	memset(bytes + 1, 0, 118);
	EXPECT(ext4_refresh_inode(hold, &file), EXT4_OK);
	bytes_are(fs, &file, bytes, sizeof(bytes));
	EXPECT(ext4_release_inode(hold), EXT4_OK);
	EXPECT(ext4_rmdir(fs, parent.number, parent.generation, (const uint8_t *)"dir", 3,
		   directory.number, directory.generation, &time, &result),
	    EXT4_OK);
	CHECK(fs->info.free_blocks == free_blocks && fs->info.free_inodes == free_inodes);
	EXPECT(ext4_get_inode(fs, EXT4_ROOT_INODE, &root), EXT4_OK);
	file = lookup(fs, "file1");
	EXPECT(ext4_unlink(fs, root.number, root.generation, (const uint8_t *)"file1", 5,
		   file.number, file.generation, &time, &result),
	    EXT4_OK);
	CHECK(fs->info.free_blocks == free_blocks && fs->info.free_inodes == free_inodes + 1);
	finish(device, fs, exports, path, "inline-lifetime-");
	printf("PASS inline creation and open-unlinked lifetime; full=%u\n", full);
}

#endif

/* SPDX-License-Identifier: BSD-3-Clause */
static void
whiteout_guards(struct device *device)
{
	struct ext4_fs *fs;
	struct ext4_inode root;
	struct ext4_inode directory;
	struct ext4_inode file;
	struct ext4_inode other;
	struct ext4_inode result;
	struct ext4_inode untouched;
	struct ext4_inode_update update = attributes(TEST_REGULAR);
	struct ext4_rename_entry source;
	struct ext4_rename_entry destination;
	struct ext4_rename_entry alias;
	uint8_t *before = malloc(device->size);
	uint32_t writes;
	uint32_t free_inodes;

	CHECK(before != NULL);
	device_reset(device, device->base);
	fs = mount_writer(device, &root);
	directory = create(fs, &root, "whiteout-dir", TEST_DIRECTORY, 0);
	file = create(fs, &directory, "file", TEST_REGULAR, 's');
	other = create(fs, &directory, "other", TEST_REGULAR, 't');
	EXPECT(ext4_link(fs, directory.number, directory.generation, (const uint8_t *)"alias", 5,
		   file.number, file.generation, &update.change_time, &result),
	    EXT4_OK);
	source = entry(&directory, "file", &file);
	destination = entry(&directory, "other", &other);
	alias = entry(&directory, "alias", &file);
	EXPECT(ext4_sync(fs), EXT4_OK);
	writes = device->writes;
	free_inodes = fs->info.free_inodes;
	memcpy(before, device->cache, device->size);
	memset(&result, 0x5a, sizeof(result));
	untouched = result;
	EXPECT(
	    ext4_rename_whiteout(fs, &source, &destination, 0, NULL, &update.change_time, &result),
	    EXT4_INVALID_ARGUMENT);
	EXPECT(ext4_rename_whiteout(
		   fs, &source, &destination, 0, &update, &update.change_time, &result),
	    EXT4_INVALID_ARGUMENT);
	update.permissions = 0;
	EXPECT(ext4_rename_whiteout(fs, &source, &destination, EXT4_RENAME_EXCHANGE, &update,
		   &update.change_time, &result),
	    EXT4_INVALID_ARGUMENT);
	EXPECT(ext4_rename_whiteout(fs, &source, &destination, EXT4_RENAME_NOREPLACE, &update,
		   &update.change_time, &result),
	    EXT4_EXISTS);
	CHECK(memcmp(&result, &untouched, sizeof(result)) == 0);
	EXPECT(ext4_rename_whiteout(fs, &source, &alias, 0, &update, &update.change_time, &result),
	    EXT4_OK);
	CHECK(result.number == file.number && result.links == 2);
	CHECK(fs->info.free_inodes == free_inodes && device->writes == writes &&
	    memcmp(before, device->cache, device->size) == 0);
	destination = entry(&directory, "new", NULL);
	EXPECT(ext4_rename_whiteout(fs, &source, &destination, EXT4_RENAME_NOREPLACE, &update,
		   &update.change_time, &result),
	    EXT4_OK);
	CHECK(fs->info.free_inodes == free_inodes - 1);
	EXPECT(ext4_get_inode(fs, directory.number, &directory), EXT4_OK);
	EXPECT(ext4_lookup(fs, &directory, (const uint8_t *)"file", 4, &result), EXT4_OK);
	CHECK(result.mode == EXT4_MODE_CHARACTER && result.device_major == 0 &&
	    result.device_minor == 0 && result.links == 1 && result.blocks_512 == 0 &&
	    result.size == 0 && result.uid == update.uid && result.gid == update.gid);
	EXPECT(ext4_unlink(fs, directory.number, directory.generation, (const uint8_t *)"file", 4,
		   result.number, result.generation, &update.change_time, &other),
	    EXT4_OK);
	CHECK(fs->info.free_inodes == free_inodes);
	EXPECT(ext4_sync(fs), EXT4_OK);
	ext4_unmount(fs);
	CHECK(device->live == 0);
	free(before);
	printf("PASS whiteout arguments, no-replace, aliases and removal\n");
}

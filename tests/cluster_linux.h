/* SPDX-License-Identifier: BSD-3-Clause */
#ifndef MACHLIN_EXT4_TEST_CLUSTER_LINUX_H
#define MACHLIN_EXT4_TEST_CLUSTER_LINUX_H

static void
linux_return(struct device *device, const char *output)
{
	struct ext4_fs *fs;
	struct ext4_inode root;
	struct ext4_inode file;
	struct ext4_inode directory;
	struct ext4_inode child;
	struct ext4_inode result;
	struct ext4_inode_update update = attributes(false);
	struct ext4_inode_update create = attributes(true);
	struct ext4_timestamp time = { CLUSTER_TEST_SECONDS, 0 };
	struct ext4_xattr_change changes[2] = { 0 };
	uint8_t *bytes;
	uint8_t *value;
	uint64_t progress;
	size_t cluster;
	size_t size;
	size_t index;
	size_t completed;
	size_t value_size;
	FILE *stream;

	device_reset(device, device->base);
	fs = mount_writer(device);
	cluster = (size_t)fs->cluster_blocks * device->block_size;
	size = 2U * cluster + device->block_size;
	bytes = malloc(size);
	value = malloc(device->block_size);
	CHECK(bytes != NULL && value != NULL);
	for (index = 0; index < size; index++) {
		bytes[index] = (uint8_t)(index * 23U + 0x67U);
	}
	EXPECT(ext4_get_inode(fs, EXT4_ROOT_INODE, &root), EXT4_OK);
	file = find(fs, &root, "linux-cluster");
	bytes_are(fs, &file, bytes, size);
	EXPECT(ext4_get_xattr(fs, file.number, file.generation, EXT4_XATTR_USER,
		   (const uint8_t *)"large", 5, value, device->block_size, &value_size),
	    EXT4_OK);
	CHECK(value_size == device->block_size / 2U && memcmp(value, bytes, value_size) == 0);
	CHECK(file.blocks_512 == 5U * cluster / EXT4_SECTOR_SIZE);
	EXPECT(ext4_truncate(
		   fs, file.number, file.generation, device->block_size + 5U, &update, &result),
	    EXT4_OK);
	memset(bytes + device->block_size + 5U, 0, size - device->block_size - 5U);
	memset(value, 0xd7, 17);
	EXPECT(ext4_write(
		   fs, file.number, file.generation, cluster + 3U, value, 17, &update, &completed),
	    EXT4_OK);
	CHECK(completed == 17);
	memcpy(bytes + cluster + 3U, value, 17);
	EXPECT(ext4_fallocate(fs, file.number, file.generation, 0, device->block_size,
		   EXT4_FALLOC_KEEP_SIZE | EXT4_FALLOC_PUNCH_HOLE, &update, &progress),
	    EXT4_OK);
	CHECK(progress == device->block_size);
	memset(bytes, 0, device->block_size);
	for (index = 0; index < 13; index++) {
		value[index] = (uint8_t)(index * 31U + 0x49U);
	}
	changes[0].policy = EXT4_XATTR_REMOVE;
	changes[0].name_index = changes[1].name_index = EXT4_XATTR_USER;
	changes[0].name = (const uint8_t *)"large";
	changes[0].name_length = 5;
	changes[1].policy = EXT4_XATTR_CREATE;
	changes[1].name = (const uint8_t *)"return";
	changes[1].name_length = 6;
	changes[1].value = value;
	changes[1].value_size = 13;
	update.fields |= EXT4_ATTR_UID | EXT4_ATTR_GID;
	update.uid = 54321;
	update.gid = 65432;
	update.xattrs = changes;
	update.xattr_count = 2;
	EXPECT(ext4_set_attributes(fs, file.number, file.generation, &update, &result), EXT4_OK);
	bytes_are(fs, &result, bytes, cluster + 20U);
	CHECK(result.blocks_512 == 2U * cluster / EXT4_SECTOR_SIZE);
	directory = find(fs, &root, "linux-cluster-dir");
	child = find(fs, &directory, "child");
	EXPECT(ext4_unlink(fs, directory.number, directory.generation, (const uint8_t *)"child", 5,
		   child.number, child.generation, &time, &result),
	    EXT4_OK);
	EXPECT(ext4_rmdir(fs, root.number, root.generation, (const uint8_t *)"linux-cluster-dir",
		   17, directory.number, directory.generation, &time, &result),
	    EXT4_OK);
	EXPECT(ext4_create(fs, root.number, root.generation, (const uint8_t *)"core-cluster", 12,
		   &create, &time, &file),
	    EXT4_OK);
	memset(bytes, 0xd7, cluster + 7U);
	update = attributes(false);
	EXPECT(ext4_write(
		   fs, file.number, file.generation, 0, bytes, cluster + 7U, &update, &completed),
	    EXT4_OK);
	CHECK(completed == cluster + 7U);
	file = find(fs, &root, "core-cluster");
	bytes_are(fs, &file, bytes, cluster + 7U);
	CHECK(file.blocks_512 == 2U * cluster / EXT4_SECTOR_SIZE);
	EXPECT(ext4_sync(fs), EXT4_OK);
	ext4_unmount(fs);
	CHECK(device->live == 0);
	stream = fopen(output, "wb");
	CHECK(stream != NULL && fwrite(device->stable, 1, device->size, stream) == device->size);
	CHECK(fclose(stream) == 0);
	free(value);
	free(bytes);
	puts("PASS Linux clustered read, truncate, hole reuse, attributes and return");
}

#endif

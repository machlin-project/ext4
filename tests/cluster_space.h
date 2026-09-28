/* SPDX-License-Identifier: BSD-3-Clause */
#ifndef MACHLIN_EXT4_TEST_CLUSTER_SPACE_H
#define MACHLIN_EXT4_TEST_CLUSTER_SPACE_H

static void
full_space(struct device *device, const char *exports, const char *path)
{
	struct ext4_fs *fs;
	struct ext4_inode inode;
	struct ext4_inode_update update = attributes(false);
	uint8_t *bytes;
	uint8_t *expected;
	uint64_t progress;
	uint64_t sectors;
	uint32_t writes;
	uint32_t ratio;
	size_t cluster;
	size_t size;
	size_t completed;

	device_reset(device, device->base);
	fs = mount_writer(device);
	CHECK(fs->info.free_blocks == 0);
	ratio = fs->cluster_blocks;
	cluster = (size_t)ratio * device->block_size;
	size = 9U * cluster + device->block_size;
	bytes = malloc(device->block_size);
	expected = malloc(size);
	CHECK(bytes != NULL && expected != NULL);
	pattern(bytes, device->block_size);
	inode = lookup(fs, "sparse");
	sectors = inode.blocks_512;
	EXPECT(ext4_read(fs, &inode, 0, expected, size, &completed), EXT4_OK);
	CHECK(completed == size);
	writes = device->writes;
	EXPECT(ext4_write(fs, inode.number, inode.generation, 2U * cluster + device->block_size,
		   bytes, device->block_size, &update, &completed),
	    EXT4_NO_SPACE);
	CHECK(completed == 0 && device->writes == writes && fs->info.free_blocks == 0);
	EXPECT(ext4_fallocate(fs, inode.number, inode.generation, 2U * cluster + device->block_size,
		   device->block_size, EXT4_FALLOC_KEEP_SIZE, &update, &progress),
	    EXT4_NO_SPACE);
	CHECK(progress == 0 && device->writes == writes && fs->info.free_blocks == 0);
	EXPECT(ext4_write(fs, inode.number, inode.generation, 2U * device->block_size, bytes,
		   device->block_size, &update, &completed),
	    EXT4_OK);
	CHECK(completed == device->block_size && fs->info.free_blocks == 0);
	memcpy(expected + 2U * device->block_size, bytes, device->block_size);
	EXPECT(ext4_fallocate(fs, inode.number, inode.generation, 0, device->block_size,
		   EXT4_FALLOC_KEEP_SIZE | EXT4_FALLOC_PUNCH_HOLE, &update, &progress),
	    EXT4_OK);
	CHECK(progress == device->block_size && fs->info.free_blocks == 0);
	memset(expected, 0, device->block_size);
	EXPECT(ext4_fallocate(fs, inode.number, inode.generation, 9U * cluster, device->block_size,
		   EXT4_FALLOC_KEEP_SIZE | EXT4_FALLOC_PUNCH_HOLE, &update, &progress),
	    EXT4_OK);
	CHECK(progress == device->block_size && fs->info.free_blocks == ratio);
	memset(expected + 9U * cluster, 0, device->block_size);
	EXPECT(ext4_fallocate(fs, inode.number, inode.generation, 2U * cluster + device->block_size,
		   device->block_size, EXT4_FALLOC_KEEP_SIZE, &update, &progress),
	    EXT4_OK);
	CHECK(progress == device->block_size && fs->info.free_blocks == 0);
	inode = lookup(fs, "sparse");
	CHECK(inode.blocks_512 == sectors);
	bytes_are(fs, &inode, expected, size);
	memset(bytes, 0xd7, device->block_size);
	EXPECT(ext4_write(fs, inode.number, inode.generation, 2U * cluster + device->block_size,
		   bytes, device->block_size, &update, &completed),
	    EXT4_OK);
	CHECK(completed == device->block_size && fs->info.free_blocks == 0);
	memcpy(expected + 2U * cluster + device->block_size, bytes, device->block_size);
	EXPECT(
	    ext4_write(fs, inode.number, inode.generation, 2U * cluster + 2U * device->block_size,
		bytes, device->block_size, &update, &completed),
	    EXT4_OK);
	CHECK(completed == device->block_size && fs->info.free_blocks == 0);
	memcpy(expected + 2U * cluster + 2U * device->block_size, bytes, device->block_size);
	inode = lookup(fs, "sparse");
	CHECK(inode.blocks_512 == sectors);
	bytes_are(fs, &inode, expected, size);
	finish(device, fs, exports, path, "cluster-full-sparse-");
	free(expected);
	free(bytes);
	puts("PASS cluster exhaustion: existing backing, final-reference release, unwritten "
	     "reuse and adjacent holes at zero free clusters");
}

#endif

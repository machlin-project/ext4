/* SPDX-License-Identifier: BSD-3-Clause */
#ifndef MACHLIN_EXT4_TEST_INLINE_LINUX_H
#define MACHLIN_EXT4_TEST_INLINE_LINUX_H

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
	struct ext4_timestamp time = { INLINE_TEST_SECONDS, 0 };
	struct ext4_rename_entry source = { 0 };
	struct ext4_rename_entry target = { 0 };
	struct ext4_xattr_change change = { 0 };
	uint8_t bytes[120];
	uint8_t value[13];
	char name[16];
	FILE *stream;

	size_t index;
	size_t completed;
	size_t size;

	device_reset(device, device->base);
	fs = mount_writer(device);
	EXPECT(ext4_get_inode(fs, EXT4_ROOT_INODE, &root), EXT4_OK);
	file = find(fs, &root, "linux-inline");
	CHECK(file.flags & EXT4_INODE_INLINE_DATA);
	for (index = 0; index < sizeof(bytes); index++) {
		bytes[index] = (uint8_t)(index * 23U + 0x67U);
	}
	bytes_are(fs, &file, bytes, 61);
	EXPECT(ext4_get_xattr(fs, file.number, file.generation, EXT4_XATTR_USER,
		   (const uint8_t *)"small", 5, value, sizeof(value), &size),
	    EXT4_OK);
	CHECK(size == sizeof(value) && memcmp(value, bytes, sizeof(value)) == 0);
	directory = find(fs, &root, "linux-renamed-dir");
	CHECK(directory.flags & EXT4_INODE_INLINE_DATA);
	child = find(fs, &directory, "..");
	CHECK(child.number == root.number);
	for (index = 0; index < 6; index++) {
		CHECK(snprintf(name, sizeof(name), "n%zu", index) > 0);
		child = find(fs, &directory, name);
		bytes_are(fs, &child, bytes, 3);
	}
	EXPECT(ext4_truncate(fs, file.number, file.generation, 1, &update, &result), EXT4_OK);
	memset(bytes + 1, 0, sizeof(bytes) - 1);
	bytes[119] = 0xd7;
	EXPECT(
	    ext4_write(fs, file.number, file.generation, 119, bytes + 119, 1, &update, &completed),
	    EXT4_OK);
	CHECK(completed == 1);
	file = find(fs, &root, "linux-inline");
	bytes_are(fs, &file, bytes, sizeof(bytes));
	for (index = 0; index < sizeof(value); index++) {
		value[index] = (uint8_t)(index * 31U + 0x49U);
	}
	change.name_index = EXT4_XATTR_USER;
	change.name = (const uint8_t *)"return";
	change.name_length = 6;
	change.value = value;
	change.value_size = sizeof(value);
	update.fields |= EXT4_ATTR_UID | EXT4_ATTR_GID;
	update.uid = 54321;
	update.gid = 65432;
	update.xattrs = &change;
	update.xattr_count = 1;
	EXPECT(ext4_set_attributes(fs, file.number, file.generation, &update, &result), EXT4_OK);
	child = find(fs, &directory, "n2");
	EXPECT(ext4_unlink(fs, directory.number, directory.generation, (const uint8_t *)"n2", 2,
		   child.number, child.generation, &time, &result),
	    EXT4_OK);
	source.directory = root.number;
	source.directory_generation = root.generation;
	source.name = (const uint8_t *)"linux-renamed-dir";
	source.name_length = 17;
	source.inode = directory.number;
	source.generation = directory.generation;
	target.directory = root.number;
	target.directory_generation = root.generation;
	target.name = (const uint8_t *)"core-return-dir";
	target.name_length = 15;
	EXPECT(ext4_rename(fs, &source, &target, 0, &time, &result), EXT4_OK);
	EXPECT(ext4_create(fs, root.number, root.generation, (const uint8_t *)"core-inline", 11,
		   &create, &time, &file),
	    EXT4_OK);
	memset(bytes, 0xd7, sizeof(bytes));
	EXPECT(ext4_write(
		   fs, file.number, file.generation, 0, bytes, sizeof(bytes), &create, &completed),
	    EXT4_OK);
	CHECK(completed == sizeof(bytes));
	file = find(fs, &root, "core-inline");
	CHECK(file.flags & EXT4_INODE_INLINE_DATA);
	bytes_are(fs, &file, bytes, sizeof(bytes));
	EXPECT(ext4_sync(fs), EXT4_OK);
	ext4_unmount(fs);
	CHECK(device->live == 0);
	stream = fopen(output, "wb");
	CHECK(stream != NULL && fwrite(device->stable, 1, device->size, stream) == device->size);
	CHECK(fclose(stream) == 0);
	puts("PASS Linux inline data read, mutation and return");
}

#endif

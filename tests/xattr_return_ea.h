/* SPDX-License-Identifier: BSD-3-Clause */
#ifndef MACHLIN_EXT4_TEST_XATTR_RETURN_EA_H
#define MACHLIN_EXT4_TEST_XATTR_RETURN_EA_H

static void
return_linux_ea_values(struct device *device, struct ext4_fs *fs, const char *output)
{
	uint8_t *value = malloc(EXT4_XATTR_VALUE_MAX);
	struct ext4_inode inode;
	struct ext4_inode result;
	struct ext4_xattr_change changes[] = {
		{ EXT4_XATTR_REPLACE, EXT4_XATTR_USER, (const uint8_t *)"maximum", 7, value,
		    EXT4_XATTR_VALUE_MAX - 3U },
		{ EXT4_XATTR_REMOVE, EXT4_XATTR_USER, (const uint8_t *)"duplicate", 9, NULL, 0 },
		{ EXT4_XATTR_REMOVE, EXT4_XATTR_USER, (const uint8_t *)"empty", 5, NULL, 0 },
		{ EXT4_XATTR_CREATE, EXT4_XATTR_USER, (const uint8_t *)"return", 6, value, 13 },
	};
	struct ext4_inode_update update = {
		.fields = EXT4_ATTR_PERMISSIONS | EXT4_ATTR_UID | EXT4_ATTR_GID |
		    EXT4_ATTR_CHANGE_TIME | EXT4_ATTR_XATTRS,
		.permissions = 0640,
		.uid = 54321,
		.gid = 65432,
		.change_time = { .seconds = 1700000090 },
		.xattrs = changes,
		.xattr_count = sizeof(changes) / sizeof(changes[0]),
	};
	FILE *stream;
	size_t index;

	CHECK(value != NULL);
	for (index = 0; index < EXT4_XATTR_VALUE_MAX; index++) {
		value[index] = (uint8_t)(index * 31U + 0x49U);
	}
	inode = lookup(fs, "/linux-ea-file");
	EXPECT(ext4_set_attributes(fs, inode.number, inode.generation, &update, &result), EXT4_OK);
	CHECK(result.number == inode.number && result.generation == inode.generation &&
	    result.uid == update.uid && result.gid == update.gid && result.size == inode.size);
	EXPECT(ext4_sync(fs), EXT4_OK);
	CHECK(memcmp(device->cache, device->stable, device->size) == 0);
	stream = fopen(output, "wbx");
	CHECK(stream != NULL && fwrite(device->stable, 1, device->size, stream) == device->size);
	CHECK(fclose(stream) == 0);
	free(value);
}

#endif

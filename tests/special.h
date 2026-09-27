/* SPDX-License-Identifier: BSD-3-Clause */
static const struct ext4_special_file special_files[] = {
	{ .type = EXT4_FT_CHARACTER, .device_major = 255, .device_minor = 255 },
	{ .type = EXT4_FT_BLOCK,
	    .device_major = EXT4_DEVICE_MAJOR_MAX,
	    .device_minor = EXT4_DEVICE_MINOR_MAX },
	{ .type = EXT4_FT_FIFO }, { .type = EXT4_FT_SOCKET }, { .type = EXT4_FT_CHARACTER },
	{ .type = EXT4_FT_CHARACTER, .device_major = 256, .device_minor = 256 }
};

static uint16_t
special_mode(enum ext4_file_type type)
{
	switch (type) {
	case EXT4_FT_CHARACTER:
		return EXT4_MODE_CHARACTER;
	case EXT4_FT_BLOCK:
		return EXT4_MODE_BLOCK;
	case EXT4_FT_FIFO:
		return EXT4_MODE_FIFO;
	case EXT4_FT_SOCKET:
		return EXT4_MODE_SOCKET;
	default:
		CHECK(false);
		return 0;
	}
}

static void
special_identity(const struct ext4_inode *inode, const struct ext4_special_file *special)
{
	CHECK((inode->mode & EXT4_MODE_TYPE) == special_mode(special->type));
	CHECK(inode->device_major == special->device_major &&
	    inode->device_minor == special->device_minor);
	CHECK(inode->size == 0 && !(inode->flags & EXT4_INODE_EXTENTS));
}

static void
special_cases(struct device *device, bool indexed)
{
	struct ext4_fs *fs;
	struct ext4_inode parent;
	struct ext4_inode inode;
	struct ext4_inode victim;
	struct ext4_inode result;
	struct ext4_inode forged;
	struct ext4_inode_hold *hold;
	struct ext4_inode_update update;
	struct ext4_inode_update mutation;
	struct ext4_rename_entry source;
	struct ext4_rename_entry destination;
	struct ext4_xattr_change attribute;
	struct ext4_mapping mapping;
	uint8_t value[500];
	uint8_t observed[sizeof(value)];
	uint64_t free_blocks;
	uint64_t parent_blocks;
	uint32_t free_inodes;
	uint32_t reads;
	size_t index;
	size_t byte;
	size_t completed;
	const uint8_t *name = (const uint8_t *)"special";
	const uint8_t *alias = (const uint8_t *)"special-alias";
	const uint8_t *replacement = (const uint8_t *)"special-target";

	device_reset(device, device->base);
	fs = mount_writer(device);
	parent = root_inode(fs);
	if (indexed) {
		parent = lookup(fs, &parent, "many");
		CHECK(parent.flags & EXT4_INODE_INDEX);
	}
	for (byte = 0; byte < sizeof(value); byte++) {
		value[byte] = (uint8_t)(byte * 31U + 7U);
	}
	memset(&attribute, 0, sizeof(attribute));
	attribute.policy = EXT4_XATTR_CREATE;
	attribute.name_index = EXT4_XATTR_USER;
	attribute.name = (const uint8_t *)"device";
	attribute.name_length = 6;
	attribute.value = value;
	attribute.value_size = sizeof(value);
	for (index = 0; index < sizeof(special_files) / sizeof(special_files[0]); index++) {
		free_blocks = fs->info.free_blocks;
		free_inodes = fs->info.free_inodes;
		parent_blocks = parent.blocks_512;
		update = create_attributes(fs);
		update.fields |= EXT4_ATTR_XATTRS;
		update.xattrs = &attribute;
		update.xattr_count = 1;
		EXPECT(ext4_mknod(fs, parent.number, parent.generation, name, 7,
			   &special_files[index], &update, &update.change_time, &inode),
		    EXT4_OK);
		check_created(fs, &inode, special_mode(special_files[index].type));
		special_identity(&inode, &special_files[index]);
		CHECK(inode.links == 1);
		EXPECT(ext4_get_xattr(fs, inode.number, inode.generation, EXT4_XATTR_USER,
			   attribute.name, attribute.name_length, observed, sizeof(observed),
			   &completed),
		    EXT4_OK);
		CHECK(completed == sizeof(value) && memcmp(value, observed, sizeof(value)) == 0);
		forged = inode;
		forged.size = device->block_size;
		reads = device->reads;
		EXPECT(ext4_read(fs, &forged, 0, observed, sizeof(observed), &completed),
		    EXT4_UNSUPPORTED);
		CHECK(completed == 0 && device->reads == reads);
		EXPECT(ext4_map_read(fs, &forged, 0, sizeof(observed), &mapping),
		    EXT4_INVALID_ARGUMENT);
		mutation = update;
		mutation.fields = EXT4_ATTR_PERMISSIONS | EXT4_ATTR_MODIFY_TIME |
		    EXT4_ATTR_CHANGE_TIME | EXT4_ATTR_XATTRS;
		mutation.xattr_count = 0;
		EXPECT(ext4_write(
			   fs, inode.number, inode.generation, 0, value, 1, &mutation, &completed),
		    EXT4_UNSUPPORTED);
		EXPECT(ext4_truncate(fs, inode.number, inode.generation, 1, &mutation, &result),
		    EXT4_UNSUPPORTED);
		EXPECT(ext4_link(fs, parent.number, parent.generation, alias, 13, inode.number,
			   inode.generation, &update.change_time, &result),
		    EXT4_OK);
		CHECK(result.links == 2);
		special_identity(&result, &special_files[index]);
		update.xattr_count = 0;
		EXPECT(ext4_mknod(fs, parent.number, parent.generation, replacement, 14,
			   &special_files[CREATE_WHITEOUT_NODE - CREATE_CHARACTER], &update,
			   &update.change_time, &victim),
		    EXT4_OK);
		memset(&source, 0, sizeof(source));
		source.directory = parent.number;
		source.directory_generation = parent.generation;
		source.inode = inode.number;
		source.generation = inode.generation;
		source.name = name;
		source.name_length = 7;
		destination = source;
		destination.name = replacement;
		destination.name_length = 14;
		destination.inode = victim.number;
		destination.generation = victim.generation;
		EXPECT(ext4_rename(fs, &source, &destination, 0, &update.change_time, &result),
		    EXT4_OK);
		special_identity(&result, &special_files[index]);
		EXPECT(ext4_get_inode(fs, victim.number, &victim), EXT4_NOT_FOUND);
		EXPECT(ext4_hold_inode(fs, inode.number, inode.generation, &hold), EXT4_OK);
		EXPECT(ext4_unlink(fs, parent.number, parent.generation, replacement, 14,
			   inode.number, inode.generation, &update.change_time, &result),
		    EXT4_OK);
		CHECK(result.links == 1);
		EXPECT(ext4_unlink(fs, parent.number, parent.generation, alias, 13, inode.number,
			   inode.generation, &update.change_time, &result),
		    EXT4_OK);
		EXPECT(ext4_refresh_inode(hold, &result), EXT4_OK);
		CHECK(result.links == 0);
		special_identity(&result, &special_files[index]);
		EXPECT(ext4_get_xattr(fs, result.number, result.generation, EXT4_XATTR_USER,
			   attribute.name, attribute.name_length, observed, sizeof(observed),
			   &completed),
		    EXT4_OK);
		CHECK(completed == sizeof(value) && memcmp(value, observed, sizeof(value)) == 0);
		EXPECT(ext4_release_inode(hold), EXT4_OK);
		EXPECT(ext4_get_inode(fs, parent.number, &parent), EXT4_OK);
		CHECK(fs->info.free_blocks +
			    (parent.blocks_512 - parent_blocks) /
				(device->block_size / EXT4_SECTOR_SIZE) ==
			free_blocks &&
		    fs->info.free_inodes == free_inodes);
		EXPECT(ext4_create(fs, parent.number, parent.generation, name, 7, &update,
			   &update.change_time, &result),
		    EXT4_OK);
		CHECK(result.device_major == 0 && result.device_minor == 0);
		EXPECT(ext4_unlink(fs, parent.number, parent.generation, name, 7, result.number,
			   result.generation, &update.change_time, &inode),
		    EXT4_OK);
		EXPECT(ext4_sync(fs), EXT4_OK);
	}
	ext4_unmount(fs);
	CHECK(device->live == 0);
	printf("PASS special inode identity, attributes, replacement and held lifetime\n");
}

static void
special_read_returned(struct device *device)
{
	const char *names[] = { "linux-device", "linux-device-renamed", "linux-fifo",
		"linux-socket" };
	const struct ext4_special_file expected[] = { { .type = EXT4_FT_CHARACTER },
		{ .type = EXT4_FT_CHARACTER,
		    .device_major = EXT4_DEVICE_MAJOR_MAX,
		    .device_minor = EXT4_DEVICE_MINOR_MAX },
		{ .type = EXT4_FT_FIFO }, { .type = EXT4_FT_SOCKET } };
	struct ext4_fs *fs;
	struct ext4_inode root;
	struct ext4_inode inode;
	size_t index;

	device_reset(device, device->base);
	EXPECT(ext4_mount(&device->environment, &fs), EXT4_OK);
	root = root_inode(fs);
	for (index = 0; index < sizeof(expected) / sizeof(expected[0]); index++) {
		inode = lookup(fs, &root, names[index]);
		special_identity(&inode, &expected[index]);
		CHECK(inode.links == 1 && inode.blocks_512 == 0);
	}
	ext4_unmount(fs);
	CHECK(device->writes == 0 && device->live == 0 &&
	    memcmp(device->cache, device->base, device->size) == 0);
	printf("PASS portable reads of Linux-created devices, whiteout, FIFO and socket\n");
}

static void
special_guards(struct device *device)
{
	struct ext4_fs *fs;
	struct ext4_inode root;
	struct ext4_inode result;
	struct ext4_inode untouched;
	struct ext4_inode_update update;
	struct ext4_special_file special = { .type = EXT4_FT_CHARACTER };
	unsigned int type;

	device_reset(device, device->base);
	fs = mount_writer(device);
	root = root_inode(fs);
	update = create_attributes(fs);
	memset(&result, 0x5a, sizeof(result));
	untouched = result;
	EXPECT(ext4_mknod(fs, root.number, root.generation, (const uint8_t *)"invalid", 7, NULL,
		   &update, &update.change_time, &result),
	    EXT4_INVALID_ARGUMENT);
	for (type = EXT4_FT_UNKNOWN; type <= EXT4_FT_SYMLINK; type++) {
		if (type >= EXT4_FT_CHARACTER && type <= EXT4_FT_SOCKET) {
			continue;
		}
		special.type = (enum ext4_file_type)type;
		EXPECT(ext4_mknod(fs, root.number, root.generation, (const uint8_t *)"invalid", 7,
			   &special, &update, &update.change_time, &result),
		    EXT4_INVALID_ARGUMENT);
	}
	special.type = EXT4_FT_CHARACTER;
	special.device_major = EXT4_DEVICE_MAJOR_MAX + 1;
	EXPECT(ext4_mknod(fs, root.number, root.generation, (const uint8_t *)"invalid", 7, &special,
		   &update, &update.change_time, &result),
	    EXT4_RANGE);
	special.device_major = 0;
	special.device_minor = EXT4_DEVICE_MINOR_MAX + 1;
	EXPECT(ext4_mknod(fs, root.number, root.generation, (const uint8_t *)"invalid", 7, &special,
		   &update, &update.change_time, &result),
	    EXT4_RANGE);
	special.type = EXT4_FT_FIFO;
	special.device_minor = 1;
	EXPECT(ext4_mknod(fs, root.number, root.generation, (const uint8_t *)"invalid", 7, &special,
		   &update, &update.change_time, &result),
	    EXT4_INVALID_ARGUMENT);
	special.type = EXT4_FT_SOCKET;
	EXPECT(ext4_mknod(fs, root.number, root.generation, (const uint8_t *)"invalid", 7, &special,
		   &update, &update.change_time, &result),
	    EXT4_INVALID_ARGUMENT);
	CHECK(memcmp(&result, &untouched, sizeof(result)) == 0 && device->writes == 0 &&
	    memcmp(device->cache, device->base, device->size) == 0);
	ext4_unmount(fs);
	CHECK(device->live == 0);
}

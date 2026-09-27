/* SPDX-License-Identifier: BSD-3-Clause */
#include "storage.h"

#define XATTR_VALUE_BYTES 600U
#define XATTR_CHANGED_BYTES 700U
#define XATTR_ACL_LIMIT 64U
#define LIFETIME_DATA_BLOCKS 40U
#define LIFETIME_SECONDS 1700000080
#define LIFETIME_SMALL_CREDITS 7U
#define CHILD_FAST_BYTES 13U
#define CHILD_MAPPED_BYTES 100U

enum child_kind { CHILD_FILE, CHILD_DIRECTORY, CHILD_FAST_SYMLINK, CHILD_MAPPED_SYMLINK };

static const char *const child_names[] = { "child-file", "child-directory", "child-fast",
	"child-mapped" };
static const char security_value[] = "system_u:object_r:tmp_t:s0";

static struct ext4_fs *
mount_writer(struct device *device, struct ext4_inode *root)
{
	struct ext4_fs *fs;

	EXPECT(ext4_mount_writable(&device->environment, &device->writer, &fs), EXT4_OK);
	EXPECT(ext4_get_inode(fs, EXT4_ROOT_INODE, root), EXT4_OK);
	return fs;
}

static struct ext4_inode
lookup(struct ext4_fs *fs, const struct ext4_inode *parent, const char *name)
{
	struct ext4_inode inode;

	EXPECT(ext4_lookup(fs, parent, (const uint8_t *)name, strlen(name), &inode), EXT4_OK);
	return inode;
}

static struct ext4_xattr_change
change(
    enum ext4_xattr_policy policy, uint8_t index, const char *name, const void *value, size_t size)
{
	struct ext4_xattr_change result = { 0 };

	result.policy = policy;
	result.name_index = index;
	result.name = (const uint8_t *)name;
	result.name_length = strlen(name);
	result.value = value;
	result.value_size = size;
	return result;
}

static struct ext4_inode_update
attributes(bool create, const struct ext4_xattr_change *changes, size_t count)
{
	struct ext4_inode_update update = { 0 };

	update.fields = EXT4_ATTR_PERMISSIONS | EXT4_ATTR_MODIFY_TIME | EXT4_ATTR_CHANGE_TIME |
	    EXT4_ATTR_XATTRS;
	if (create) {
		update.fields |= EXT4_ATTR_UID | EXT4_ATTR_GID | EXT4_ATTR_ACCESS_TIME;
	}
	update.permissions = 0761;
	update.uid = 12345;
	update.gid = 23456;
	update.access_time.seconds = LIFETIME_SECONDS;
	update.modify_time.seconds = LIFETIME_SECONDS;
	update.change_time.seconds = LIFETIME_SECONDS;
	update.xattrs = changes;
	update.xattr_count = count;
	return update;
}

static void
pattern(uint8_t *bytes, size_t size, bool changed)
{
	size_t index;

	for (index = 0; index < size; index++) {
		bytes[index] = (uint8_t)(index * (changed ? 17U : 29U) + (changed ? 0x51U : 0x83U));
	}
}

static void
value_is(struct ext4_fs *fs, const struct ext4_inode *inode, uint8_t index, const char *name,
    const void *expected, size_t size)
{
	uint8_t *bytes = malloc(size + 1);
	size_t returned = SIZE_MAX;

	CHECK(bytes != NULL);
	memset(bytes, 0xa5, size + 1);
	EXPECT(ext4_get_xattr(fs, inode->number, inode->generation, index, (const uint8_t *)name,
		   strlen(name), bytes, size, &returned),
	    EXT4_OK);
	CHECK(returned == size && memcmp(bytes, expected, size) == 0 && bytes[size] == 0xa5);
	free(bytes);
}

static uint64_t
attribute_block(struct device *device, struct ext4_fs *fs, const struct ext4_inode *inode)
{
	struct ext4_inode_disk *disk;
	uint64_t offset;

	EXPECT(ext4_inode_location(fs, inode->number, &offset), EXT4_OK);
	disk = (struct ext4_inode_disk *)(device->cache + offset);
	return ext4_le32(&disk->xattr_block_lo) |
	    ((uint64_t)ext4_le16(&disk->xattr_block_hi) << 32);
}

static uint32_t
references(struct device *device, uint64_t block)
{
	struct ext4_xattr_header_disk *header;

	CHECK(block != 0 && block < device->blocks);
	header = (struct ext4_xattr_header_disk *)(device->cache + block * device->block_size);
	return ext4_le32(&header->references);
}

static void
remove_inode(struct ext4_fs *fs, const struct ext4_inode *parent, const char *name,
    const struct ext4_inode *inode)
{
	struct ext4_timestamp time = { LIFETIME_SECONDS, 0 };
	struct ext4_inode result;

	if ((inode->mode & EXT4_MODE_TYPE) == EXT4_MODE_DIRECTORY) {
		EXPECT(ext4_rmdir(fs, parent->number, parent->generation, (const uint8_t *)name,
			   strlen(name), inode->number, inode->generation, &time, &result),
		    EXT4_OK);
	} else {
		EXPECT(ext4_unlink(fs, parent->number, parent->generation, (const uint8_t *)name,
			   strlen(name), inode->number, inode->generation, &time, &result),
		    EXT4_OK);
	}
}

static enum ext4_result
create_child(struct ext4_fs *fs, const struct ext4_inode *parent, enum child_kind kind,
    const struct ext4_inode_update *update, struct ext4_inode *result)
{
	uint8_t target[CHILD_MAPPED_BYTES];
	const char *name = child_names[kind];

	memset(target, 'q', sizeof(target));
	if (kind == CHILD_FILE) {
		return ext4_create(fs, parent->number, parent->generation, (const uint8_t *)name,
		    strlen(name), update, &update->change_time, result);
	}
	if (kind == CHILD_DIRECTORY) {
		return ext4_mkdir(fs, parent->number, parent->generation, (const uint8_t *)name,
		    strlen(name), update, &update->change_time, result);
	}
	return ext4_symlink(fs, parent->number, parent->generation, (const uint8_t *)name,
	    strlen(name), target, kind == CHILD_FAST_SYMLINK ? CHILD_FAST_BYTES : sizeof(target),
	    update, &update->change_time, result);
}

static void
creation(struct device *device, const char *exports, const char *path)
{
	struct ext4_fs *fs;
	struct ext4_inode root;
	struct ext4_inode parent;
	struct ext4_inode children[4];
	struct ext4_inode result;
	struct ext4_inode untouched;
	struct ext4_inode_update update;
	struct ext4_xattr_change changes[4];
	uint8_t bytes[XATTR_VALUE_BYTES];
	uint8_t acl[XATTR_ACL_LIMIT];
	uint8_t observed[CHILD_MAPPED_BYTES];
	uint64_t free_blocks;
	uint32_t free_inodes;
	size_t acl_size;
	size_t completed;
	enum child_kind kind;

	device_reset(device, device->base);
	fs = mount_writer(device, &root);
	parent = lookup(fs, &root, "directory");
	free_blocks = fs->info.free_blocks;
	free_inodes = fs->info.free_inodes;
	EXPECT(ext4_get_xattr(fs, parent.number, parent.generation, EXT4_XATTR_POSIX_ACL_DEFAULT,
		   NULL, 0, acl, sizeof(acl), &acl_size),
	    EXT4_OK);
	pattern(bytes, sizeof(bytes), true);
	changes[0] = change(EXT4_XATTR_CREATE, EXT4_XATTR_USER, "binary", bytes, sizeof(bytes));
	changes[1] = change(EXT4_XATTR_CREATE, EXT4_XATTR_SECURITY, "selinux", security_value,
	    sizeof(security_value));
	changes[2] = change(EXT4_XATTR_CREATE, EXT4_XATTR_POSIX_ACL_ACCESS, "", acl, acl_size);
	changes[3] = change(EXT4_XATTR_CREATE, EXT4_XATTR_POSIX_ACL_DEFAULT, "", acl, acl_size);
	update = attributes(true, NULL, 0);
	update.fields &= ~(uint32_t)EXT4_ATTR_XATTRS;
	memset(&untouched, 0xa5, sizeof(untouched));
	for (kind = CHILD_FILE; kind <= CHILD_MAPPED_SYMLINK; kind++) {
		result = untouched;
		EXPECT(create_child(fs, &parent, kind, &update, &result), EXT4_INVALID_ARGUMENT);
		CHECK(memcmp(&result, &untouched, sizeof(result)) == 0 && device->writes == 0 &&
		    memcmp(device->cache, device->base, device->size) == 0);
	}
	for (kind = CHILD_FILE; kind <= CHILD_MAPPED_SYMLINK; kind++) {
		update = attributes(true, changes,
		    kind == CHILD_DIRECTORY  ? 4
			: kind == CHILD_FILE ? 3
					     : 2);
		if (kind >= CHILD_FAST_SYMLINK) {
			update.permissions = 0777;
		}
		EXPECT(create_child(fs, &parent, kind, &update, &children[kind]), EXT4_OK);
		result = lookup(fs, &parent, child_names[kind]);
		CHECK(result.number == children[kind].number && result.uid == update.uid &&
		    result.gid == update.gid &&
		    (result.mode & EXT4_MODE_PERMISSIONS) == update.permissions);
		value_is(fs, &result, EXT4_XATTR_USER, "binary", bytes, sizeof(bytes));
		value_is(fs, &result, EXT4_XATTR_SECURITY, "selinux", security_value,
		    sizeof(security_value));
		CHECK(attribute_block(device, fs, &result) != 0 &&
		    references(device, attribute_block(device, fs, &result)) == 1);
		if (kind <= CHILD_DIRECTORY) {
			value_is(fs, &result, EXT4_XATTR_POSIX_ACL_ACCESS, "", acl, acl_size);
		} else {
			CHECK(result.fast_symlink == (kind == CHILD_FAST_SYMLINK));
			EXPECT(ext4_read(fs, &result, 0, observed, sizeof(observed), &completed),
			    EXT4_OK);
			CHECK(completed ==
			    (kind == CHILD_FAST_SYMLINK ? CHILD_FAST_BYTES : sizeof(observed)));
			memset(bytes, 'q', completed);
			CHECK(memcmp(bytes, observed, completed) == 0);
			pattern(bytes, sizeof(bytes), true);
		}
	}
	EXPECT(ext4_sync(fs), EXT4_OK);
	storage_export(device, exports, path, "xattr-created-");
	EXPECT(ext4_link(fs, root.number, root.generation, (const uint8_t *)"child-alias", 11,
		   children[CHILD_FILE].number, children[CHILD_FILE].generation,
		   &update.change_time, &result),
	    EXT4_OK);
	CHECK(result.links == 2);
	remove_inode(fs, &root, "child-alias", &result);
	for (kind = CHILD_FILE; kind <= CHILD_MAPPED_SYMLINK; kind++) {
		remove_inode(fs, &parent, child_names[kind], &children[kind]);
	}
	CHECK(fs->info.free_blocks == free_blocks && fs->info.free_inodes == free_inodes);
	update = attributes(true, NULL, 0);
	EXPECT(create_child(fs, &parent, CHILD_FILE, &update, &result), EXT4_OK);
	EXPECT(
	    ext4_list_xattrs(fs, result.number, result.generation, NULL, 0, &completed), EXT4_OK);
	CHECK(completed == 0);
	remove_inode(fs, &parent, child_names[CHILD_FILE], &result);
	EXPECT(ext4_sync(fs), EXT4_OK);
	CHECK(fs->info.free_blocks == free_blocks && fs->info.free_inodes == free_inodes);
	ext4_unmount(fs);
	storage_export(device, exports, path, "xattr-created-removed-");
	puts("PASS attributed creation: admitted inheritance, file/directory/symlink metadata, "
	     "hard links and complete reclamation");
}

static void
held_shared(struct device *device, const char *exports, const char *path)
{
	struct ext4_fs *fs;
	struct ext4_inode root;
	struct ext4_inode inode;
	struct ext4_inode shared;
	struct ext4_inode other;
	struct ext4_inode result;
	struct ext4_inode_hold *hold;
	struct ext4_inode_hold *duplicate;
	struct ext4_inode_hold *other_hold;
	struct ext4_inode_update update = attributes(false, NULL, 0);
	struct ext4_xattr_change replacement;
	uint8_t original[XATTR_VALUE_BYTES];
	uint8_t changed[XATTR_CHANGED_BYTES];
	uint8_t *bytes = malloc(device->block_size);
	uint8_t *pending = malloc(device->size);
	uint8_t *expected = malloc(device->size);
	uint64_t shared_block;
	uint64_t free_blocks;
	uint32_t free_inodes;
	uint32_t block;
	size_t completed;

	CHECK(bytes != NULL && pending != NULL && expected != NULL);
	device_reset(device, device->base);
	fs = mount_writer(device, &root);
	inode = lookup(fs, &root, "block");
	shared = lookup(fs, &root, "shared");
	other = lookup(fs, &root, "plain");
	free_blocks = fs->info.free_blocks;
	free_inodes = fs->info.free_inodes;
	shared_block = attribute_block(device, fs, &inode);
	CHECK(shared_block == attribute_block(device, fs, &shared) &&
	    references(device, shared_block) == 2);
	EXPECT(ext4_hold_inode(fs, inode.number, inode.generation, &hold), EXT4_OK);
	EXPECT(ext4_hold_inode(fs, inode.number, inode.generation, &duplicate), EXT4_OK);
	EXPECT(ext4_hold_inode(fs, other.number, other.generation, &other_hold), EXT4_OK);
	CHECK(hold == duplicate && hold->references == 2);
	pattern(original, sizeof(original), false);
	pattern(changed, sizeof(changed), true);
	memset(bytes, 0x5a, device->block_size);
	for (block = 0; block < LIFETIME_DATA_BLOCKS; block++) {
		EXPECT(ext4_write(fs, inode.number, inode.generation,
			   (uint64_t)block * device->block_size, bytes, device->block_size, &update,
			   &completed),
		    EXT4_OK);
		CHECK(completed == device->block_size);
	}
	remove_inode(fs, &root, "block", &inode);
	remove_inode(fs, &root, "plain", &other);
	CHECK(fs->last_orphan == other.number && references(device, shared_block) == 2);
	value_is(fs, &inode, EXT4_XATTR_USER, "binary", original, sizeof(original));
	replacement =
	    change(EXT4_XATTR_REPLACE, EXT4_XATTR_USER, "binary", changed, sizeof(changed));
	update = attributes(false, &replacement, 1);
	update.fields |= EXT4_ATTR_UID | EXT4_ATTR_GID;
	EXPECT(ext4_set_attributes(fs, inode.number, inode.generation, &update, &result), EXT4_OK);
	CHECK(result.links == 0 && result.uid == update.uid && result.gid == update.gid &&
	    attribute_block(device, fs, &result) != shared_block &&
	    references(device, shared_block) == 1);
	value_is(fs, &result, EXT4_XATTR_USER, "binary", changed, sizeof(changed));
	value_is(fs, &shared, EXT4_XATTR_USER, "binary", original, sizeof(original));
	update = attributes(false, NULL, 0);
	EXPECT(ext4_truncate(fs, inode.number, inode.generation, 17, &update, &result), EXT4_OK);
	CHECK(result.links == 0 && result.size == 17 && fs->last_orphan == other.number);
	value_is(fs, &result, EXT4_XATTR_USER, "binary", changed, sizeof(changed));
	replacement = change(EXT4_XATTR_REMOVE, EXT4_XATTR_USER, "binary", NULL, 0);
	update = attributes(false, &replacement, 1);
	EXPECT(ext4_write(fs, inode.number, inode.generation, device->block_size + 7, bytes, 1,
		   &update, &completed),
	    EXT4_OK);
	CHECK(completed == 1);
	EXPECT(ext4_refresh_inode(hold, &result), EXT4_OK);
	CHECK(result.size == device->block_size + 8 && attribute_block(device, fs, &result) == 0);
	EXPECT(ext4_read(fs, &result, 17, bytes, device->block_size - 17, &completed), EXT4_OK);
	CHECK(completed == device->block_size - 17);
	for (block = 0; block < completed; block++) {
		CHECK(bytes[block] == 0);
	}
	EXPECT(
	    ext4_list_xattrs(fs, result.number, result.generation, NULL, 0, &completed), EXT4_OK);
	CHECK(completed == 0 && fs->info.free_inodes == free_inodes);
	/* Preserve a crash with two held orphan entries. Offline cleanup must agree
	 * byte-for-byte with the live owner even though it visits them in reverse order. */
	memcpy(pending, device->stable, device->size);
	EXPECT(ext4_release_inode(duplicate), EXT4_OK);
	CHECK(hold->references == 1 && fs->info.free_inodes == free_inodes);
	EXPECT(ext4_release_inode(hold), EXT4_OK);
	CHECK(fs->last_orphan == other.number && fs->info.free_inodes == free_inodes + 1);
	EXPECT(ext4_release_inode(other_hold), EXT4_OK);
	CHECK(fs->last_orphan == 0 && fs->hold_count == 0 &&
	    fs->info.free_inodes == free_inodes + 2 && fs->info.free_blocks == free_blocks);
	value_is(fs, &shared, EXT4_XATTR_USER, "binary", original, sizeof(original));
	EXPECT(ext4_sync(fs), EXT4_OK);
	ext4_unmount(fs);
	memcpy(expected, device->stable, device->size);
	storage_export(device, exports, path, "xattr-held-released-");
	device_reset(device, pending);
	storage_export(device, exports, path, "xattr-held-pending-");
	CHECK(storage_recover(device, expected, true));
	storage_export(device, exports, path, "xattr-held-recovered-");
	free(expected);
	free(pending);
	free(bytes);
	puts("PASS attributed held files: shared block copy, data/attribute transitions, bounded "
	     "shrink, nonhead final release and offline recovery");
}

static void
credit_guard(struct device *device)
{
	struct ext4_fs *fs;
	struct ext4_inode root;
	struct ext4_inode inode;
	struct ext4_inode journal;
	struct ext4_inode result;
	struct ext4_inode untouched;
	struct ext4_inode_hold *hold;
	struct ext4_jbd_super *super;
	struct ext4_xattr_change attribute;
	struct ext4_inode_update update;
	uint8_t value[XATTR_VALUE_BYTES];
	uint8_t *before = malloc(device->size);
	uint64_t physical;
	uint64_t free_blocks;
	uint32_t writes;
	uint32_t live;
	size_t completed;

	CHECK(before != NULL);
	device_reset(device, device->base);
	fs = mount_writer(device, &root);
	EXPECT(ext4_get_inode(fs, fs->journal_inode, &journal), EXT4_OK);
	EXPECT(ext4_map_block(fs, &journal, 0, &physical), EXT4_OK);
	ext4_unmount(fs);
	super = (struct ext4_jbd_super *)(device->cache + physical * device->block_size);
	ext4_encode_be32(
	    &super->max_length, ext4_be32(&super->first) + 2 * LIFETIME_SMALL_CREDITS + 2);
	if (ext4_be32(&super->feature_incompat) & (EXT4_JBD_CSUM_V2 | EXT4_JBD_CSUM_V3)) {
		ext4_encode_be32(&super->checksum, 0);
		ext4_encode_be32(&super->checksum, ext4_crc32c(UINT32_MAX, super, sizeof(*super)));
	}
	memcpy(device->stable, device->cache, device->size);
	fs = mount_writer(device, &root);
	CHECK(ext4_journal_credits(fs->journal) == LIFETIME_SMALL_CREDITS);
	inode = lookup(fs, &root, "plain");
	pattern(value, sizeof(value), true);
	attribute = change(EXT4_XATTR_CREATE, EXT4_XATTR_USER, "binary", value, sizeof(value));
	update = attributes(false, &attribute, 1);
	/* The small journal fits both operations while this inode is linked. */
	EXPECT(ext4_set_attributes(fs, inode.number, inode.generation, &update, &result), EXT4_OK);
	attribute = change(EXT4_XATTR_REMOVE, EXT4_XATTR_USER, "binary", NULL, 0);
	EXPECT(ext4_set_attributes(fs, inode.number, inode.generation, &update, &result), EXT4_OK);
	update = attributes(false, NULL, 0);
	EXPECT(ext4_write(fs, inode.number, inode.generation, 0, value, 1, &update, &completed),
	    EXT4_OK);
	CHECK(completed == 1);
	EXPECT(
	    ext4_truncate_atomic(fs, inode.number, inode.generation, 0, &update, &result), EXT4_OK);
	EXPECT(ext4_hold_inode(fs, inode.number, inode.generation, &hold), EXT4_OK);
	remove_inode(fs, &root, "plain", &inode);
	CHECK(fs->last_orphan == inode.number);
	writes = device->writes;
	live = device->live;
	free_blocks = fs->info.free_blocks;
	memcpy(before, device->cache, device->size);
	memset(&untouched, 0xa5, sizeof(untouched));
	result = untouched;
	attribute = change(EXT4_XATTR_CREATE, EXT4_XATTR_USER, "binary", value, sizeof(value));
	update = attributes(false, &attribute, 1);
	EXPECT(
	    ext4_set_attributes(fs, inode.number, inode.generation, &update, &result), EXT4_RANGE);
	CHECK(memcmp(&result, &untouched, sizeof(result)) == 0);
	update = attributes(false, NULL, 0);
	completed = SIZE_MAX;
	EXPECT(ext4_write(fs, inode.number, inode.generation, 0, value, 1, &update, &completed),
	    EXT4_RANGE);
	CHECK(completed == 0 && device->writes == writes && device->live == live &&
	    fs->info.free_blocks == free_blocks && !fs->aborted &&
	    memcmp(device->cache, before, device->size) == 0 &&
	    memcmp(device->stable, before, device->size) == 0);
	EXPECT(ext4_release_inode(hold), EXT4_OK);
	EXPECT(ext4_sync(fs), EXT4_OK);
	ext4_unmount(fs);
	free(before);
	puts("PASS held-file growth reserves future attribute/data reclamation before writes");
}

static struct ext4_rename_entry
rename_entry(const struct ext4_inode *parent, const char *name, const struct ext4_inode *inode)
{
	struct ext4_rename_entry entry = { 0 };

	entry.directory = parent->number;
	entry.directory_generation = parent->generation;
	entry.name = (const uint8_t *)name;
	entry.name_length = strlen(name);
	entry.inode = inode->number;
	entry.generation = inode->generation;
	return entry;
}

static void
replacement(struct device *device, const char *exports, const char *path)
{
	struct ext4_fs *fs;
	struct ext4_inode root;
	struct ext4_inode source;
	struct ext4_inode victim;
	struct ext4_inode result;
	struct ext4_inode_hold *hold;
	struct ext4_rename_entry from;
	struct ext4_rename_entry to;
	struct ext4_xattr_change attribute;
	struct ext4_inode_update update;
	uint8_t value[XATTR_VALUE_BYTES];
	uint8_t original[XATTR_VALUE_BYTES];
	uint8_t acl[XATTR_ACL_LIMIT];
	uint64_t free_blocks;
	uint64_t source_block;
	uint32_t free_inodes;
	uint32_t source_references;
	size_t acl_size;
	const char *source_name;
	char prefix[64];
	enum child_kind kind;

	pattern(value, sizeof(value), true);
	pattern(original, sizeof(original), false);
	attribute = change(EXT4_XATTR_CREATE, EXT4_XATTR_USER, "binary", value, sizeof(value));
	update = attributes(true, &attribute, 1);
	for (kind = CHILD_FILE; kind <= CHILD_MAPPED_SYMLINK; kind++) {
		device_reset(device, device->base);
		fs = mount_writer(device, &root);
		free_blocks = fs->info.free_blocks;
		free_inodes = fs->info.free_inodes;
		source_name = kind == CHILD_DIRECTORY ? "directory" : "block";
		source = lookup(fs, &root, source_name);
		source_block = attribute_block(device, fs, &source);
		source_references = source_block == 0 ? 0 : references(device, source_block);
		acl_size = 0;
		if (kind == CHILD_DIRECTORY) {
			EXPECT(
			    ext4_get_xattr(fs, source.number, source.generation,
				EXT4_XATTR_POSIX_ACL_DEFAULT, NULL, 0, acl, sizeof(acl), &acl_size),
			    EXT4_OK);
		}
		EXPECT(create_child(fs, &root, kind, &update, &victim), EXT4_OK);
		EXPECT(ext4_hold_inode(fs, victim.number, victim.generation, &hold), EXT4_OK);
		from = rename_entry(&root, source_name, &source);
		to = rename_entry(&root, child_names[kind], &victim);
		EXPECT(ext4_rename(fs, &from, &to, 0, &update.change_time, &result), EXT4_OK);
		CHECK(result.number == source.number &&
		    attribute_block(device, fs, &result) == source_block &&
		    (source_block == 0 || references(device, source_block) == source_references));
		result = lookup(fs, &root, child_names[kind]);
		CHECK(result.number == source.number && result.generation == source.generation);
		EXPECT(ext4_refresh_inode(hold, &result), EXT4_OK);
		CHECK(result.links == 0);
		value_is(fs, &result, EXT4_XATTR_USER, "binary", value, sizeof(value));
		if (kind == CHILD_DIRECTORY) {
			value_is(fs, &source, EXT4_XATTR_POSIX_ACL_ACCESS, "", acl, acl_size);
			value_is(fs, &source, EXT4_XATTR_POSIX_ACL_DEFAULT, "", acl, acl_size);
		} else {
			value_is(
			    fs, &source, EXT4_XATTR_USER, "binary", original, sizeof(original));
		}
		EXPECT(ext4_release_inode(hold), EXT4_OK);
		CHECK(fs->last_orphan == 0 && fs->info.free_inodes == free_inodes &&
		    fs->info.free_blocks == free_blocks &&
		    (source_block == 0 || references(device, source_block) == source_references));
		EXPECT(ext4_sync(fs), EXT4_OK);
		ext4_unmount(fs);
		CHECK(snprintf(prefix, sizeof(prefix), "xattr-renamed-%s-", child_names[kind]) > 0);
		storage_export(device, exports, path, prefix);
	}
	puts("PASS attributed rename replacement: held file/directory/symlinks and unchanged "
	     "source sharing");
}

#include "xattr_lifetime_faults.h"
#include "xattr_release_faults.h"
#include "xattr_enable_faults.h"

int
main(int argc, char **argv)
{
	struct device device;
	const char *exports = NULL;
	bool smoke = false;
	bool release_only = false;
	bool enable_only = false;
	enum lifetime_operation operation;
	enum release_kind kind;
	enum enable_operation enable;
	int argument = 1;

	CHECK(argc >= 2);
	while (argument < argc && argv[argument][0] == '-') {
		if (strcmp(argv[argument], "--smoke") == 0) {
			smoke = true;
			argument++;
		} else if (strcmp(argv[argument], "--release") == 0) {
			release_only = true;
			argument++;
		} else if (strcmp(argv[argument], "--enable") == 0) {
			enable_only = true;
			argument++;
		} else {
			CHECK(strcmp(argv[argument], "--export") == 0 && argument + 2 < argc);
			exports = argv[argument + 1];
			argument += 2;
		}
	}
	CHECK(argument < argc && !(release_only && enable_only));
	for (; argument < argc; argument++) {
		storage_open(&device, argv[argument]);
		if (enable_only) {
			enable_feature_guards(&device);
			for (enable = ENABLE_CREATE_FILE; enable < ENABLE_OPERATION_COUNT;
			    enable++) {
				enable_faults(&device, enable, smoke, exports, argv[argument]);
			}
			storage_close(&device);
			printf("PASS first xattr transactions: %s\n", argv[argument]);
			continue;
		}
		if (!release_only) {
			creation(&device, exports, argv[argument]);
			held_shared(&device, exports, argv[argument]);
			credit_guard(&device);
			replacement(&device, exports, argv[argument]);
			for (operation = LIFETIME_CREATE_FILE; operation < LIFETIME_OPERATION_COUNT;
			    operation++) {
				lifetime_faults(&device, operation, smoke, exports, argv[argument]);
			}
		}
		for (kind = RELEASE_SHARED; kind < RELEASE_KIND_COUNT; kind++) {
			release_faults(&device, kind, smoke, exports, argv[argument]);
		}
		storage_close(&device);
		printf("PASS xattr mutation lifetime: %s\n", argv[argument]);
	}
	return 0;
}

/* SPDX-License-Identifier: BSD-3-Clause */
#include "storage.h"

/* Quota accounting on e2fsprogs-authored volumes. Creation, writes, truncation,
 * ownership changes, attributes, held and final removal and replacement must keep
 * every user and group entry equal to the charged inodes; a reader written here
 * walks the quota trees independently of the core. An optional export receives
 * the image for strict e2fsck, which recomputes all usage. --faults cuts power at
 * every event of operations that grow the quota trees and requires atomicity. */

#define QUOTA_SECONDS 1700008000
#define NEW_OWNERS 40U
#define FIRST_OWNER 2000U
#define FIRST_GROUP 3000U
#define OWNER_GROUPS 3U
#define EXISTING_OWNER 1000U
#define EXISTING_GROUP 100U
#define NAME_BYTES 32U
#define LARGE_VALUE 3000U
#define EXTERNAL_VALUE 300U
#define PROJECT 42U
#define MOVED_PROJECT 7U
#define PROJECT_OWNER 5000U
#define CONTINUED_OWNER 7000U
#define ORPHAN_OWNER 8000U
#define CONTINUED_OWNERS 30U
/* The project Linux assigns in its mutation phase. */
#define LINUX_PROJECT 77U
/* Enforcement: alice may add this many blocks before her soft and hard limits,
 * and hold this many inodes in all. */
#define SOFT_BLOCKS 3U
#define HARD_BLOCKS 6U
#define INODE_SOFT 2U
#define INODE_HARD 3U
#define WRITE_BLOCKS 8U
#define EXEMPT_BLOCKS 10U
#define LIMITED_PROJECT 9U
#define QUOTA_LIMIT_BLOCK 1024U

static int64_t quota_clock = QUOTA_SECONDS;

static const struct ext4_timestamp quota_time = { QUOTA_SECONDS, 0 };
/* IDs whose index paths diverge at every tree level. */
static const uint32_t far_owners[] = { 0x00010000U, 0x01000000U, 0xfffffffeU };

struct usage {
	uint64_t space;
	uint64_t inodes;
	bool present;
};

static struct ext4_inode_update
creation(uint32_t uid, uint32_t gid, uint16_t permissions)
{
	struct ext4_inode_update update = { 0 };

	update.fields = EXT4_ATTR_PERMISSIONS | EXT4_ATTR_UID | EXT4_ATTR_GID |
	    EXT4_ATTR_ACCESS_TIME | EXT4_ATTR_MODIFY_TIME | EXT4_ATTR_CHANGE_TIME |
	    EXT4_ATTR_XATTRS;
	update.uid = uid;
	update.gid = gid;
	update.permissions = permissions;
	update.access_time = quota_time;
	update.modify_time = quota_time;
	update.change_time = quota_time;
	return update;
}

static struct ext4_inode_update
data_update(const struct ext4_inode *inode)
{
	struct ext4_inode_update update = { 0 };

	update.fields = EXT4_ATTR_PERMISSIONS | EXT4_ATTR_MODIFY_TIME | EXT4_ATTR_CHANGE_TIME |
	    EXT4_ATTR_XATTRS;
	update.permissions = (uint16_t)(inode->mode & 07777U);
	update.modify_time = quota_time;
	update.change_time = quota_time;
	return update;
}

static bool
zero(const void *buffer, size_t length)
{
	const uint8_t *bytes = buffer;
	size_t index;

	for (index = 0; index < length; index++) {
		if (bytes[index] != 0) {
			return false;
		}
	}
	return true;
}

/* Walk one quota file's tree through ordinary reads of its inode. */
/* Walk the quota tree independently of the core. Returns whether id has an entry,
 * its contents and its byte offset within the quota file. */
static bool
quota_find(struct ext4_fs *fs, uint32_t type, uint32_t id, struct ext4_quota_entry_disk *result,
    uint64_t *offset)
{
	const struct ext4_quota_entry_disk *entry;
	const struct ext4_le32 *references;
	struct ext4_inode inode;
	uint8_t *file;
	size_t completed;
	size_t position;
	uint32_t block = EXT4_QUOTA_TREE_ROOT;
	uint32_t depth;
	uint32_t slot;
	uint32_t shift;
	bool found = false;

	CHECK(fs->quota_inodes[type] != 0);
	EXPECT(ext4_get_inode(fs, fs->quota_inodes[type], &inode), EXT4_OK);
	file = malloc((size_t)inode.size);
	CHECK(file != NULL);
	EXPECT(ext4_read(fs, &inode, 0, file, (size_t)inode.size, &completed), EXT4_OK);
	CHECK(completed == inode.size);
	for (depth = 0; depth < EXT4_QUOTA_TREE_DEPTH; depth++) {
		shift = (EXT4_QUOTA_TREE_DEPTH - depth - 1U) * EXT4_QUOTA_ID_BITS_PER_LEVEL;
		CHECK(((uint64_t)block + 1U) * EXT4_QUOTA_BLOCK_SIZE <= inode.size);
		references =
		    (const struct ext4_le32 *)(file + (size_t)block * EXT4_QUOTA_BLOCK_SIZE);
		block = ext4_le32(&references[(id >> shift) % EXT4_QUOTA_TREE_FANOUT]);
		if (block == 0) {
			free(file);
			return false;
		}
	}
	CHECK(((uint64_t)block + 1U) * EXT4_QUOTA_BLOCK_SIZE <= inode.size);
	for (slot = 0; slot < EXT4_QUOTA_LEAF_ENTRIES && !found; slot++) {
		position = (size_t)block * EXT4_QUOTA_BLOCK_SIZE +
		    sizeof(struct ext4_quota_leaf_disk) + (size_t)slot * sizeof(*entry);
		entry = (const struct ext4_quota_entry_disk *)(file + position);
		/* An all-zero entry is free; used empty entries carry a marker. */
		if (!zero(entry, sizeof(*entry)) && ext4_le32(&entry->id) == id) {
			memcpy(result, entry, sizeof(*result));
			*offset = position;
			found = true;
		}
	}
	CHECK(found);
	free(file);
	return true;
}

static uint64_t
le64(const struct ext4_le32 value[2])
{
	return (uint64_t)ext4_le32(&value[0]) | (uint64_t)ext4_le32(&value[1]) << 32;
}

static struct usage
quota_usage(struct ext4_fs *fs, uint32_t type, uint32_t id)
{
	struct ext4_quota_entry_disk entry;
	struct usage result = { 0 };
	uint64_t offset;

	if (quota_find(fs, type, id, &entry, &offset)) {
		result.space = le64(entry.space);
		result.inodes = le64(entry.inodes);
		result.present = true;
	}
	return result;
}

static void
expect_usage(struct ext4_fs *fs, uint32_t type, uint32_t id, uint64_t space, uint64_t inodes)
{
	struct usage usage = quota_usage(fs, type, id);

	if ((space != 0 || inodes != 0) && !usage.present) {
		fprintf(stderr, "type %u id %u has no quota entry\n", type, id);
		exit(1);
	}
	if (usage.space != space || usage.inodes != inodes) {
		fprintf(stderr, "type %u id %u: %llu bytes %llu inodes, expected %llu and %llu\n",
		    type, id, (unsigned long long)usage.space, (unsigned long long)usage.inodes,
		    (unsigned long long)space, (unsigned long long)inodes);
		exit(1);
	}
}

static struct ext4_inode
lookup(struct ext4_fs *fs, const struct ext4_inode *directory, const char *name)
{
	struct ext4_inode inode;

	EXPECT(ext4_lookup(fs, directory, (const uint8_t *)name, strlen(name), &inode), EXT4_OK);
	return inode;
}

static struct ext4_inode
create_file(struct ext4_fs *fs, const struct ext4_inode *directory, const char *name, uint32_t uid,
    uint32_t gid, uint32_t blocks)
{
	struct ext4_inode_update update = creation(uid, gid, 0644);
	struct ext4_inode inode;
	uint8_t *data;
	size_t completed;
	size_t length = (size_t)blocks * fs->info.block_size;

	EXPECT(ext4_create(fs, directory->number, directory->generation, (const uint8_t *)name,
		   strlen(name), &update, &quota_time, &inode),
	    EXT4_OK);
	if (length != 0) {
		data = malloc(length);
		CHECK(data != NULL);
		memset(data, (int)(uid & 0x7fU), length);
		update = data_update(&inode);
		EXPECT(ext4_write(fs, inode.number, inode.generation, 0, data, length, &update,
			   &completed),
		    EXT4_OK);
		CHECK(completed == length);
		free(data);
		EXPECT(ext4_get_inode(fs, inode.number, &inode), EXT4_OK);
	}
	return inode;
}

static void
set_owner(struct ext4_fs *fs, struct ext4_inode *inode, uint32_t uid, uint32_t gid)
{
	struct ext4_inode_update update = { 0 };

	update.fields = EXT4_ATTR_PERMISSIONS | EXT4_ATTR_UID | EXT4_ATTR_GID |
	    EXT4_ATTR_CHANGE_TIME | EXT4_ATTR_XATTRS;
	update.permissions = (uint16_t)(inode->mode & 07777U);
	update.uid = uid;
	update.gid = gid;
	update.change_time = quota_time;
	EXPECT(ext4_set_attributes(fs, inode->number, inode->generation, &update, inode), EXT4_OK);
}

static void
set_value(struct ext4_fs *fs, struct ext4_inode *inode, size_t size)
{
	struct ext4_xattr_change change = { EXT4_XATTR_SET, EXT4_XATTR_USER,
		(const uint8_t *)"quota", 5, NULL, size };
	struct ext4_inode_update update = { 0 };
	uint8_t *value = malloc(size);

	CHECK(value != NULL);
	memset(value, 'v', size);
	change.value = value;
	update.fields = EXT4_ATTR_CHANGE_TIME | EXT4_ATTR_XATTRS;
	update.change_time = quota_time;
	update.xattrs = &change;
	update.xattr_count = 1;
	EXPECT(ext4_set_attributes(fs, inode->number, inode->generation, &update, inode), EXT4_OK);
	free(value);
}

static void
expect_inode(struct ext4_fs *fs, const struct ext4_inode *inode, uint64_t inodes)
{
	expect_usage(fs, 0, inode->uid, inode->blocks_512 * EXT4_SECTOR_SIZE, inodes);
}

/* Project IDs: an explicit change moves usage; a PROJINHERIT directory passes
 * its project to new objects and refuses links or renames from other projects. */
static void
projects(struct ext4_fs *fs, const struct ext4_inode *root, const struct ext4_inode *directory,
    struct ext4_inode *moved)
{
	struct ext4_inode_update update = creation(PROJECT_OWNER, PROJECT_OWNER, 0755);
	struct ext4_inode project;
	struct ext4_inode inherited;
	struct ext4_inode nested;
	struct ext4_inode result;
	struct ext4_rename_entry from;
	struct ext4_rename_entry to;
	struct usage before;

	if (!(fs->info.feature_ro_compat & EXT4_FEATURE_RO_PROJECT)) {
		EXPECT(ext4_set_project(
			   fs, moved->number, moved->generation, PROJECT, &quota_time, &result),
		    EXT4_UNSUPPORTED);
		EXPECT(
		    ext4_set_project(fs, moved->number, moved->generation, 0, &quota_time, &result),
		    EXT4_OK);
		return;
	}
	before = quota_usage(fs, 2, 0);
	EXPECT(ext4_set_project(
		   fs, moved->number, moved->generation, MOVED_PROJECT, &quota_time, moved),
	    EXT4_OK);
	CHECK(moved->project == MOVED_PROJECT);
	expect_usage(fs, 2, MOVED_PROJECT, moved->blocks_512 * EXT4_SECTOR_SIZE, 1);
	expect_usage(
	    fs, 2, 0, before.space - moved->blocks_512 * EXT4_SECTOR_SIZE, before.inodes - 1U);
	EXPECT(ext4_mkdir(fs, root->number, root->generation, (const uint8_t *)"p", 1, &update,
		   &quota_time, &project),
	    EXT4_OK);
	EXPECT(ext4_set_project(
		   fs, project.number, project.generation, PROJECT, &quota_time, &project),
	    EXT4_OK);
	EXPECT(ext4_set_inode_flags(fs, project.number, project.generation, EXT4_INODE_PROJINHERIT,
		   EXT4_INODE_PROJINHERIT, &quota_time, &project),
	    EXT4_OK);
	inherited = create_file(fs, &project, "inherited", PROJECT_OWNER, PROJECT_OWNER, 2);
	CHECK(inherited.project == PROJECT && !(inherited.flags & EXT4_INODE_PROJINHERIT));
	EXPECT(ext4_mkdir(fs, project.number, project.generation, (const uint8_t *)"nested", 6,
		   &update, &quota_time, &nested),
	    EXT4_OK);
	CHECK(nested.project == PROJECT && (nested.flags & EXT4_INODE_PROJINHERIT));
	EXPECT(ext4_get_inode(fs, project.number, &project), EXT4_OK);
	expect_usage(fs, 2, PROJECT,
	    (project.blocks_512 + inherited.blocks_512 + nested.blocks_512) * EXT4_SECTOR_SIZE, 3);
	/* Another project's object cannot enter; a project member can leave. */
	EXPECT(ext4_link(fs, project.number, project.generation, (const uint8_t *)"alien", 5,
		   moved->number, moved->generation, &quota_time, &result),
	    EXT4_CROSS_PROJECT);
	from = (struct ext4_rename_entry){ directory->number, directory->generation,
		(const uint8_t *)"f2010", 5, moved->number, moved->generation };
	to = (struct ext4_rename_entry){ project.number, project.generation,
		(const uint8_t *)"alien", 5, 0, 0 };
	EXPECT(ext4_rename(fs, &from, &to, 0, &quota_time, &result), EXT4_CROSS_PROJECT);
	from = (struct ext4_rename_entry){ project.number, project.generation,
		(const uint8_t *)"inherited", 9, inherited.number, inherited.generation };
	to = (struct ext4_rename_entry){ directory->number, directory->generation,
		(const uint8_t *)"left", 4, 0, 0 };
	EXPECT(ext4_rename(fs, &from, &to, 0, &quota_time, &result), EXT4_OK);
	CHECK(result.project == PROJECT);
	EXPECT(ext4_link(fs, directory->number, directory->generation, (const uint8_t *)"member", 6,
		   nested.number, nested.generation, &quota_time, &result),
	    EXT4_IS_DIRECTORY);
	expect_usage(fs, 2, PROJECT,
	    (project.blocks_512 + inherited.blocks_512 + nested.blocks_512) * EXT4_SECTOR_SIZE, 3);
}

/* An unlinked file still held at unmount stays charged as an orphan; offline
 * recovery reclaims it and releases its usage in the same transactions. */
static void
orphan_cleanup(struct device *device)
{
	struct ext4_recovery_report report;
	struct ext4_inode_hold *hold;
	struct ext4_inode root;
	struct ext4_inode inode;
	struct ext4_inode result;
	struct ext4_fs *fs;

	EXPECT(ext4_mount_writable(&device->environment, &device->writer, &fs), EXT4_OK);
	EXPECT(ext4_get_inode(fs, EXT4_ROOT_INODE, &root), EXT4_OK);
	inode = create_file(fs, &root, "orphan", ORPHAN_OWNER, ORPHAN_OWNER, 3);
	EXPECT(ext4_hold_inode(fs, inode.number, inode.generation, &hold), EXT4_OK);
	EXPECT(ext4_unlink(fs, root.number, root.generation, (const uint8_t *)"orphan", 6,
		   inode.number, inode.generation, &quota_time, &result),
	    EXT4_OK);
	expect_inode(fs, &inode, 1);
	EXPECT(ext4_sync(fs), EXT4_OK);
	/* Unmount invalidates the hold without reclaiming the inode. */
	ext4_unmount(fs);
	EXPECT(ext4_recover(&device->environment, &device->writer, &report), EXT4_OK);
	CHECK(report.cleaned_orphans == 1);
	EXPECT(ext4_mount(&device->environment, &fs), EXT4_OK);
	expect_usage(fs, 0, ORPHAN_OWNER, 0, 0);
	expect_usage(fs, 1, ORPHAN_OWNER, 0, 0);
	ext4_unmount(fs);
}

static void
scenario(struct device *device, const char *exports, const char *source)
{
	static struct ext4_inode files[NEW_OWNERS];
	struct ext4_inode_update update = creation(0, 0, 0755);
	struct ext4_inode_hold *hold;
	struct ext4_inode root;
	struct ext4_inode owners;
	struct ext4_inode alice;
	struct ext4_inode directory;
	struct ext4_inode far;
	struct ext4_inode other;
	struct ext4_inode result;
	struct ext4_rename_entry from;
	struct ext4_rename_entry to;
	struct usage before;
	struct usage group_before;
	struct ext4_fs *fs;
	char name[NAME_BYTES];
	uint32_t index;
	bool value_inodes;
	bool project_volume;

	EXPECT(ext4_mount_writable(&device->environment, &device->writer, &fs), EXT4_OK);
	CHECK(fs->quota_active);
	value_inodes = (fs->info.feature_incompat & EXT4_FEATURE_INCOMPAT_EA_INODE) != 0;
	project_volume = (fs->info.feature_ro_compat & EXT4_FEATURE_RO_PROJECT) != 0;
	EXPECT(ext4_get_inode(fs, EXT4_ROOT_INODE, &root), EXT4_OK);
	owners = lookup(fs, &root, "owners");
	alice = lookup(fs, &owners, "alice");
	before = quota_usage(fs, 0, EXISTING_OWNER);
	group_before = quota_usage(fs, 1, EXISTING_GROUP);
	CHECK(before.present && before.inodes == 1 &&
	    before.space == alice.blocks_512 * EXT4_SECTOR_SIZE);
	EXPECT(ext4_mkdir(fs, root.number, root.generation, (const uint8_t *)"q", 1, &update,
		   &quota_time, &directory),
	    EXT4_OK);
	/* Forty new owners fill several leaves and share index blocks. */
	for (index = 0; index < NEW_OWNERS; index++) {
		snprintf(name, sizeof(name), "f%u", FIRST_OWNER + index);
		files[index] = create_file(fs, &directory, name, FIRST_OWNER + index,
		    FIRST_GROUP + index % OWNER_GROUPS, index % 4U + 1U);
		expect_inode(fs, &files[index], 1);
	}
	for (index = 0; index < sizeof(far_owners) / sizeof(far_owners[0]); index++) {
		snprintf(name, sizeof(name), "far%u", index);
		far = create_file(fs, &directory, name, far_owners[index], far_owners[index], 1);
		expect_inode(fs, &far, 1);
		expect_usage(fs, 1, far_owners[index], far.blocks_512 * EXT4_SECTOR_SIZE, 1);
	}
	/* Ownership moves the complete charge between entries. */
	set_owner(fs, &files[0], EXISTING_OWNER, EXISTING_GROUP);
	expect_usage(fs, 0, FIRST_OWNER, 0, 0);
	expect_usage(fs, 0, EXISTING_OWNER, before.space + files[0].blocks_512 * EXT4_SECTOR_SIZE,
	    before.inodes + 1U);
	expect_usage(fs, 1, EXISTING_GROUP,
	    group_before.space + files[0].blocks_512 * EXT4_SECTOR_SIZE, group_before.inodes + 1U);
	update = data_update(&files[1]);
	EXPECT(ext4_truncate(fs, files[1].number, files[1].generation, 0, &update, &files[1]),
	    EXT4_OK);
	expect_inode(fs, &files[1], 1);
	CHECK(files[1].blocks_512 == 0);
	/* An external attribute block adds to i_blocks; a value inode adds one inode. */
	set_value(fs, &files[2], EXTERNAL_VALUE);
	expect_inode(fs, &files[2], 1);
	if (value_inodes) {
		set_value(fs, &files[3], LARGE_VALUE);
		expect_inode(fs, &files[3], 2);
	}
	/* A held unlinked inode stays charged until its final release. */
	EXPECT(ext4_hold_inode(fs, files[4].number, files[4].generation, &hold), EXT4_OK);
	EXPECT(ext4_unlink(fs, directory.number, directory.generation, (const uint8_t *)"f2004", 5,
		   files[4].number, files[4].generation, &quota_time, &result),
	    EXT4_OK);
	expect_inode(fs, &files[4], 1);
	EXPECT(ext4_release_inode(hold), EXT4_OK);
	expect_usage(fs, 0, files[4].uid, 0, 0);
	/* Replacement releases the victim's charge. */
	from = (struct ext4_rename_entry){ directory.number, directory.generation,
		(const uint8_t *)"f2005", 5, files[5].number, files[5].generation };
	to = (struct ext4_rename_entry){ directory.number, directory.generation,
		(const uint8_t *)"f2006", 5, files[6].number, files[6].generation };
	EXPECT(ext4_rename(fs, &from, &to, 0, &quota_time, &result), EXT4_OK);
	expect_usage(fs, 0, files[6].uid, 0, 0);
	expect_inode(fs, &files[5], 1);
	update = creation(FIRST_OWNER + 7U, FIRST_GROUP, 0755);
	EXPECT(ext4_mkdir(fs, directory.number, directory.generation, (const uint8_t *)"d", 1,
		   &update, &quota_time, &other),
	    EXT4_OK);
	expect_usage(fs, 0, FIRST_OWNER + 7U,
	    (files[7].blocks_512 + other.blocks_512) * EXT4_SECTOR_SIZE, 2);
	update = creation(FIRST_OWNER + 8U, FIRST_GROUP, 0777);
	EXPECT(ext4_symlink(fs, directory.number, directory.generation, (const uint8_t *)"s", 1,
		   (const uint8_t *)"f2007", 5, &update, &quota_time, &other),
	    EXT4_OK);
	expect_usage(fs, 0, FIRST_OWNER + 8U,
	    (files[8].blocks_512 + other.blocks_512) * EXT4_SECTOR_SIZE, 2);
	directory = lookup(fs, &root, "q");
	far = lookup(fs, &directory, "far1");
	EXPECT(ext4_unlink(fs, directory.number, directory.generation, (const uint8_t *)"far1", 4,
		   far.number, far.generation, &quota_time, &result),
	    EXT4_OK);
	expect_usage(fs, 0, far_owners[1], 0, 0);
	expect_usage(fs, 1, far_owners[1], 0, 0);
	projects(fs, &root, &directory, &files[10]);
	EXPECT(ext4_sync(fs), EXT4_OK);
	ext4_unmount(fs);
	CHECK(device->live == 0);
	orphan_cleanup(device);
	/* Usage persists across a remount, including the grown trees. */
	EXPECT(ext4_mount(&device->environment, &fs), EXT4_OK);
	for (index = 9; index < NEW_OWNERS; index++) {
		expect_inode(fs, &files[index], 1);
	}
	expect_usage(fs, 0, far_owners[2], far.blocks_512 * EXT4_SECTOR_SIZE, 1);
	ext4_unmount(fs);
	memcpy(device->stable, device->cache, device->size);
	storage_export(device, exports, source, "quota-mutated-");
	printf("PASS quota accounting: %u new owners, %zu far IDs, ownership, truncation, "
	       "attributes%s, held removal, replacement, orphan recovery and %s\n",
	    NEW_OWNERS, sizeof(far_owners) / sizeof(far_owners[0]),
	    value_inodes ? ", value inodes" : "",
	    project_volume ? "project inheritance" : "project zero only");
}

/* Resume on an image Linux changed: new owners reuse the entries and blocks the
 * kernel freed, and a distant ID whose index path Linux released returns. */
static void
continuation(struct device *device, const char *exports, const char *source)
{
	struct ext4_inode root;
	struct ext4_inode directory;
	struct ext4_inode inode;
	struct ext4_inode other;
	struct ext4_inode result;
	struct ext4_fs *fs;
	char name[NAME_BYTES];
	uint32_t index;

	EXPECT(ext4_mount_writable(&device->environment, &device->writer, &fs), EXT4_OK);
	CHECK(fs->quota_active);
	EXPECT(ext4_get_inode(fs, EXT4_ROOT_INODE, &root), EXT4_OK);
	directory = lookup(fs, &root, "q");
	for (index = 0; index < CONTINUED_OWNERS; index++) {
		snprintf(name, sizeof(name), "c%u", CONTINUED_OWNER + index);
		inode = create_file(fs, &directory, name, CONTINUED_OWNER + index, CONTINUED_OWNER,
		    index % 3U + 1U);
		expect_inode(fs, &inode, 1);
	}
	/* New names can index the directory; lookups need its current map. */
	directory = lookup(fs, &root, "q");
	inode = lookup(fs, &directory, "f2020");
	set_owner(fs, &inode, far_owners[1], far_owners[1]);
	expect_usage(fs, 0, far_owners[1], inode.blocks_512 * EXT4_SECTOR_SIZE, 1);
	expect_usage(fs, 1, far_owners[1], inode.blocks_512 * EXT4_SECTOR_SIZE, 1);
	inode = lookup(fs, &directory, "linux0");
	EXPECT(ext4_unlink(fs, directory.number, directory.generation, (const uint8_t *)"linux0", 6,
		   inode.number, inode.generation, &quota_time, &result),
	    EXT4_OK);
	expect_usage(fs, 0, inode.uid, 0, 0);
	if (fs->info.feature_ro_compat & EXT4_FEATURE_RO_PROJECT) {
		other = lookup(fs, &directory, "f2015");
		CHECK(other.project == LINUX_PROJECT);
		inode = lookup(fs, &directory, "f2016");
		EXPECT(ext4_set_project(
			   fs, inode.number, inode.generation, LINUX_PROJECT, &quota_time, &inode),
		    EXT4_OK);
		expect_usage(fs, 2, LINUX_PROJECT,
		    (inode.blocks_512 + other.blocks_512) * EXT4_SECTOR_SIZE, 2);
	}
	EXPECT(ext4_sync(fs), EXT4_OK);
	ext4_unmount(fs);
	CHECK(device->live == 0);
	memcpy(device->stable, device->cache, device->size);
	storage_export(device, exports, source, "quota-continued-");
	printf("PASS quota continuation: %u owners reuse freed entries after Linux\n",
	    CONTINUED_OWNERS);
}

/* One operation per fault run: create a directory for new IDs, which grows both
 * trees, or move an existing file to a new owner. */
static enum ext4_result
fault_operation(struct ext4_fs *fs, unsigned int operation, struct ext4_inode *result)
{
	struct ext4_inode_update update = creation(0x02000000U, 0x02000000U, 0755);
	struct ext4_inode root;
	struct ext4_inode owners;
	struct ext4_inode alice;
	enum ext4_result error;

	error = ext4_get_inode(fs, EXT4_ROOT_INODE, &root);
	if (error != EXT4_OK) {
		return error;
	}
	if (operation == 0) {
		return ext4_mkdir(fs, root.number, root.generation, (const uint8_t *)"grown", 5,
		    &update, &quota_time, result);
	}
	error = ext4_lookup(fs, &root, (const uint8_t *)"owners", 6, &owners);
	if (error == EXT4_OK) {
		error = ext4_lookup(fs, &owners, (const uint8_t *)"alice", 5, &alice);
	}
	if (error != EXT4_OK) {
		return error;
	}
	update.fields = EXT4_ATTR_PERMISSIONS | EXT4_ATTR_UID | EXT4_ATTR_GID |
	    EXT4_ATTR_CHANGE_TIME | EXT4_ATTR_XATTRS;
	update.permissions = (uint16_t)(alice.mode & 07777U);
	update.uid = 0x03000000U;
	update.gid = 0x03000000U;
	return ext4_set_attributes(fs, alice.number, alice.generation, &update, result);
}

static void
faults(struct device *device, const char *exports, const char *source)
{
	static const char *const prefixes[] = { "quota-fault-mkdir-", "quota-fault-owner-" };
	struct ext4_inode result;
	struct ext4_fs *fs;
	uint8_t *expected = malloc(device->size);
	uint32_t events;
	uint32_t allocations;
	uint32_t reads;
	uint32_t position;
	uint32_t count;
	uint32_t cuts = 0;
	uint32_t recovered = 0;
	unsigned int operation;
	unsigned int phase;
	unsigned int partial;
	unsigned int survival;
	bool committed;

	CHECK(expected != NULL);
	for (operation = 0; operation < 2; operation++) {
		device_reset(device, device->base);
		EXPECT(ext4_mount_writable(&device->environment, &device->writer, &fs), EXT4_OK);
		device->reads = device->allocations = 0;
		EXPECT(fault_operation(fs, operation, &result), EXT4_OK);
		allocations = device->allocations;
		reads = device->reads;
		events = device->events;
		EXPECT(ext4_sync(fs), EXT4_OK);
		memcpy(expected, device->stable, device->size);
		ext4_unmount(fs);
		storage_export(device, exports, source, prefixes[operation]);
		for (phase = 0; phase < 2; phase++) {
			count = phase == 0 ? allocations : reads;
			for (position = 1; position <= count; position++) {
				device_reset(device, device->base);
				EXPECT(
				    ext4_mount_writable(&device->environment, &device->writer, &fs),
				    EXT4_OK);
				device->reads = device->allocations = 0;
				device->fail_allocation = phase == 0 ? position : 0;
				device->fail_read = phase == 1 ? position : 0;
				EXPECT(fault_operation(fs, operation, &result),
				    phase == 0 ? EXT4_NO_MEMORY : EXT4_IO);
				ext4_unmount(fs);
				CHECK(storage_recover(device, device->base, true));
			}
		}
		for (position = 1; position <= events; position++) {
			for (partial = 0; partial < 2; partial++) {
				for (survival = 0; survival < 3; survival++) {
					device_reset(device, device->base);
					EXPECT(ext4_mount_writable(
						   &device->environment, &device->writer, &fs),
					    EXT4_OK);
					device->stop_at = position;
					device->partial = partial != 0;
					device->survival = survival;
					EXPECT(fault_operation(fs, operation, &result), EXT4_IO);
					committed = device->intent_durable;
					ext4_unmount(fs);
					cuts++;
					recovered +=
					    storage_recover(device, expected, committed) ? 1U : 0U;
				}
			}
		}
	}
	free(expected);
	printf("PASS quota faults: cuts=%u recovered=%u\n", cuts, recovered);
}

static int64_t
clock_now(void *context)
{
	(void)context;
	return quota_clock;
}

/* Write limits into id's entry while no owner is mounted, as setquota does. The
 * entry lives in ordinary quota-file data. */
static void
set_limits(struct device *device, uint32_t type, uint32_t id, uint64_t space_hard,
    uint64_t space_soft, uint64_t inode_hard, uint64_t inode_soft)
{
	struct ext4_quota_entry_disk entry;
	struct ext4_mapping mapping;
	struct ext4_inode inode;
	struct ext4_fs *fs;
	uint64_t offset;

	EXPECT(ext4_mount(&device->environment, &fs), EXT4_OK);
	CHECK(quota_find(fs, type, id, &entry, &offset));
	EXPECT(ext4_get_inode(fs, fs->quota_inodes[type], &inode), EXT4_OK);
	EXPECT(ext4_map_read(fs, &inode, offset, sizeof(entry), &mapping), EXT4_OK);
	CHECK(!mapping.hole && mapping.length >= sizeof(entry));
	ext4_unmount(fs);
	ext4_encode32(&entry.space_hard[0], (uint32_t)space_hard);
	ext4_encode32(&entry.space_hard[1], (uint32_t)(space_hard >> 32));
	ext4_encode32(&entry.space_soft[0], (uint32_t)space_soft);
	ext4_encode32(&entry.space_soft[1], (uint32_t)(space_soft >> 32));
	ext4_encode32(&entry.inode_hard[0], (uint32_t)inode_hard);
	ext4_encode32(&entry.inode_hard[1], (uint32_t)(inode_hard >> 32));
	ext4_encode32(&entry.inode_soft[0], (uint32_t)inode_soft);
	ext4_encode32(&entry.inode_soft[1], (uint32_t)(inode_soft >> 32));
	memcpy(device->cache + mapping.device_offset, &entry, sizeof(entry));
	memcpy(device->stable + mapping.device_offset, &entry, sizeof(entry));
}

static struct ext4_quota_entry_disk
quota_entry(struct ext4_fs *fs, uint32_t type, uint32_t id)
{
	struct ext4_quota_entry_disk entry;
	uint64_t offset;

	CHECK(quota_find(fs, type, id, &entry, &offset));
	return entry;
}

static enum ext4_result
write_blocks(struct ext4_fs *fs, struct ext4_inode *inode, uint32_t first, uint32_t blocks,
    bool partial, size_t *completed)
{
	struct ext4_inode_update update = data_update(inode);
	size_t length = (size_t)blocks * fs->info.block_size;
	uint8_t *data = malloc(length);
	enum ext4_result error;

	CHECK(data != NULL);
	memset(data, 'q', length);
	error = partial
	    ? ext4_write_partial(fs, inode->number, inode->generation,
		  (uint64_t)first * fs->info.block_size, data, length, &update, completed)
	    : ext4_write(fs, inode->number, inode->generation,
		  (uint64_t)first * fs->info.block_size, data, length, &update, completed);
	free(data);
	EXPECT(ext4_get_inode(fs, inode->number, inode), EXT4_OK);
	return error;
}

/* Limits enforced by the adapter's policy: hard limits refuse without writes and
 * keep the owner usable, partial writes stop at the limit, soft limits start and
 * end grace periods, and exemption and decreases always proceed. */
static void
enforcement(struct device *device, const char *exports, const char *source)
{
	struct ext4_quota_policy policy = { NULL, EXT4_QUOTA_ENFORCE_USER, clock_now };
	struct ext4_inode_update update = creation(EXISTING_OWNER, EXISTING_GROUP, 0644);
	struct ext4_quota_entry_disk entry;
	struct ext4_inode_update truncation;
	struct ext4_inode root;
	struct ext4_inode owners;
	struct ext4_inode alice;
	struct ext4_inode limited;
	struct ext4_inode other;
	struct ext4_inode result;
	struct usage start;
	struct usage now;
	struct ext4_fs *fs;
	uint64_t block;
	uint64_t grace;
	size_t completed;

	EXPECT(ext4_mount(&device->environment, &fs), EXT4_OK);
	EXPECT(ext4_get_inode(fs, EXT4_ROOT_INODE, &root), EXT4_OK);
	owners = lookup(fs, &root, "owners");
	alice = lookup(fs, &owners, "alice");
	start = quota_usage(fs, 0, EXISTING_OWNER);
	block = fs->info.block_size;
	CHECK(start.present && start.inodes == 1 &&
	    start.space == alice.blocks_512 * EXT4_SECTOR_SIZE &&
	    start.space % QUOTA_LIMIT_BLOCK == 0);
	CHECK(fs->quota_inodes[2] != 0);
	ext4_unmount(fs);
	set_limits(device, 0, EXISTING_OWNER,
	    (start.space + HARD_BLOCKS * block) / QUOTA_LIMIT_BLOCK,
	    (start.space + SOFT_BLOCKS * block) / QUOTA_LIMIT_BLOCK, INODE_HARD, INODE_SOFT);
	EXPECT(ext4_mount_writable(&device->environment, &device->writer, &fs), EXT4_OK);
	EXPECT(ext4_get_inode(fs, EXT4_ROOT_INODE, &root), EXT4_OK);
	/* Policy validation. */
	policy.now = NULL;
	EXPECT(ext4_quota_policy_set(fs, &policy), EXT4_INVALID_ARGUMENT);
	policy.now = clock_now;
	policy.types = 0x8U;
	EXPECT(ext4_quota_policy_set(fs, &policy), EXT4_INVALID_ARGUMENT);
	policy.types = EXT4_QUOTA_ENFORCE_USER | EXT4_QUOTA_ENFORCE_PROJECT;
	EXPECT(ext4_quota_policy_set(fs, &policy), EXT4_OK);
	/* Without enforcement nothing is refused; with it, a hard limit refuses an
	 * atomic write that would cross it, without changing anything. */
	EXPECT(ext4_create(fs, root.number, root.generation, (const uint8_t *)"e1", 2, &update,
		   &quota_time, &limited),
	    EXT4_OK);
	expect_usage(fs, 0, EXISTING_OWNER, start.space, 2);
	EXPECT(write_blocks(fs, &limited, 0, WRITE_BLOCKS, false, &completed), EXT4_QUOTA_EXCEEDED);
	CHECK(!fs->aborted && completed == 0 && limited.size == 0);
	expect_usage(fs, 0, EXISTING_OWNER, start.space, 2);
	{
		struct ext4_inode_update request_update = data_update(&limited);
		const size_t length = 1024U * 1024U;
		uint8_t *data = malloc(length);
		uint8_t *before = malloc(device->size);
		uint32_t writes = device->writes;

		CHECK(data != NULL && before != NULL);
		memset(data, 'q', length);
		memcpy(before, device->cache, device->size);
		EXPECT(ext4_write_request(fs, limited.number, limited.generation, 0, data, length,
			   &request_update, &completed),
		    EXT4_QUOTA_EXCEEDED);
		CHECK(completed == 0 && !fs->aborted && device->writes == writes);
		CHECK(memcmp(before, device->cache, device->size) == 0);
		expect_usage(fs, 0, EXISTING_OWNER, start.space, 2);
		free(before);
		free(data);
	}
	/* A partial write keeps the prefix that fits the hard limit. */
	EXPECT(write_blocks(fs, &limited, 0, WRITE_BLOCKS, true, &completed), EXT4_QUOTA_EXCEEDED);
	CHECK(completed == HARD_BLOCKS * block && limited.size == completed);
	now = quota_usage(fs, 0, EXISTING_OWNER);
	CHECK(now.space == start.space + HARD_BLOCKS * block);
	/* Crossing the soft limit started its grace period from the file's grace time. */
	entry = quota_entry(fs, 0, EXISTING_OWNER);
	grace = le64(entry.space_time) - (uint64_t)quota_clock;
	CHECK(le64(entry.space_time) > (uint64_t)quota_clock && grace != 0);
	/* Falling back within the soft limit ends it. */
	truncation = data_update(&limited);
	EXPECT(ext4_truncate(
		   fs, limited.number, limited.generation, 2U * block, &truncation, &limited),
	    EXT4_OK);
	CHECK(le64(quota_entry(fs, 0, EXISTING_OWNER).space_time) == 0);
	/* Beyond the soft limit again, an expired grace period refuses more. */
	EXPECT(write_blocks(fs, &limited, 2, 3, false, &completed), EXT4_OK);
	entry = quota_entry(fs, 0, EXISTING_OWNER);
	CHECK(le64(entry.space_time) == (uint64_t)quota_clock + grace);
	quota_clock += (int64_t)grace;
	EXPECT(write_blocks(fs, &limited, 5, 1, false, &completed), EXT4_QUOTA_EXCEEDED);
	/* Exemption lifts limits for the owner's privileged operations only. */
	ext4_quota_exempt(fs, true);
	EXPECT(write_blocks(fs, &limited, 5, EXEMPT_BLOCKS, false, &completed), EXT4_OK);
	ext4_quota_exempt(fs, false);
	EXPECT(write_blocks(fs, &limited, 5 + EXEMPT_BLOCKS, 1, false, &completed),
	    EXT4_QUOTA_EXCEEDED);
	/* Inode limits: the third inode fits, a fourth does not. */
	EXPECT(ext4_create(fs, root.number, root.generation, (const uint8_t *)"e2", 2, &update,
		   &quota_time, &other),
	    EXT4_OK);
	EXPECT(ext4_create(fs, root.number, root.generation, (const uint8_t *)"e3", 2, &update,
		   &quota_time, &result),
	    EXT4_QUOTA_EXCEEDED);
	CHECK(!fs->aborted);
	EXPECT(ext4_lookup(fs, &root, (const uint8_t *)"e3", 2, &result), EXT4_NOT_FOUND);
	expect_usage(fs, 0, EXISTING_OWNER, quota_usage(fs, 0, EXISTING_OWNER).space, INODE_HARD);
	/* Decreases always proceed and end the grace periods they fall below. */
	EXPECT(ext4_unlink(fs, root.number, root.generation, (const uint8_t *)"e1", 2,
		   limited.number, limited.generation, &quota_time, &result),
	    EXT4_OK);
	now = quota_usage(fs, 0, EXISTING_OWNER);
	CHECK(now.space == start.space && now.inodes == 2);
	CHECK(le64(quota_entry(fs, 0, EXISTING_OWNER).space_time) == 0);
	/* Moving usage into a project at its hard limit is refused. */
	EXPECT(ext4_sync(fs), EXT4_OK);
	ext4_unmount(fs);
	EXPECT(ext4_mount(&device->environment, &fs), EXT4_OK);
	now = quota_usage(fs, 2, LIMITED_PROJECT);
	ext4_unmount(fs);
	set_limits(device, 2, LIMITED_PROJECT, now.space / QUOTA_LIMIT_BLOCK, 0, 0, 0);
	EXPECT(ext4_mount_writable(&device->environment, &device->writer, &fs), EXT4_OK);
	EXPECT(ext4_quota_policy_set(fs, &policy), EXT4_OK);
	EXPECT(ext4_get_inode(fs, EXT4_ROOT_INODE, &root), EXT4_OK);
	other = lookup(fs, &root, "e2");
	EXPECT(write_blocks(fs, &other, 0, 1, false, &completed), EXT4_OK);
	EXPECT(ext4_set_project(
		   fs, other.number, other.generation, LIMITED_PROJECT, &quota_time, &result),
	    EXT4_QUOTA_EXCEEDED);
	EXPECT(ext4_get_inode(fs, other.number, &result), EXT4_OK);
	CHECK(result.project == 0 && !fs->aborted);
	expect_usage(fs, 2, LIMITED_PROJECT, now.space, now.inodes);
	/* Without a policy the same move is only accounted. */
	EXPECT(ext4_quota_policy_set(fs, NULL), EXT4_OK);
	EXPECT(ext4_set_project(
		   fs, other.number, other.generation, LIMITED_PROJECT, &quota_time, &result),
	    EXT4_OK);
	CHECK(result.project == LIMITED_PROJECT);
	EXPECT(
	    ext4_set_project(fs, other.number, other.generation, 0, &quota_time, &result), EXT4_OK);
	expect_usage(fs, 2, LIMITED_PROJECT, now.space, now.inodes);
	EXPECT(ext4_sync(fs), EXT4_OK);
	ext4_unmount(fs);
	memcpy(device->stable, device->cache, device->size);
	storage_export(device, exports, source, "quota-enforced-");
	printf("PASS quota enforcement: hard, partial, soft grace start/expiry/end, exemption, "
	       "inode and project limits, grace %llu s\n",
	    (unsigned long long)grace);
}

int
main(int argc, char **argv)
{
	static struct device device;

	if (argc == 4 && strcmp(argv[1], "--continue") == 0) {
		storage_open(&device, argv[2]);
		continuation(&device, argv[3], argv[2]);
		storage_close(&device);
		return 0;
	}
	if ((argc == 3 || argc == 4) && strcmp(argv[1], "--enforce") == 0) {
		storage_open(&device, argv[2]);
		enforcement(&device, argc == 4 ? argv[3] : NULL, argv[2]);
		storage_close(&device);
		return 0;
	}
	if ((argc == 3 || argc == 4) && strcmp(argv[1], "--faults") == 0) {
		storage_open(&device, argv[2]);
		faults(&device, argc == 4 ? argv[3] : NULL, argv[2]);
		storage_close(&device);
		return 0;
	}
	if (argc != 2 && argc != 3) {
		fprintf(stderr,
		    "usage: %s [--faults | --enforce | --continue] QUOTA_IMAGE "
		    "[EXPORT_DIRECTORY]\n",
		    argv[0]);
		return 2;
	}
	storage_open(&device, argv[1]);
	scenario(&device, argc == 3 ? argv[2] : NULL, argv[1]);
	storage_close(&device);
	return 0;
}

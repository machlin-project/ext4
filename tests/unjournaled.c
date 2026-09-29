/* SPDX-License-Identifier: BSD-3-Clause */
#include "sha.h"
#include "storage.h"

#include <inttypes.h>

/* Writes to volumes without a journal under EXT4_WRITE_UNJOURNALED. Misuse of the
 * option and writable mounts without it are refused. The superblock's valid state
 * is cleared before the first change after a mount or a sync and set by sync, and a
 * volume left in use mounts and recovers only with CHECK_REQUIRED. A power cut at
 * every write or barrier of each operation leaves the old volume, when nothing
 * reached the device, or one that needs e2fsck, which recovery refuses to change.
 * With --export, one image per such cut and a manifest of the files synced before
 * every operation go to the directory, for tests/check_unjournaled.py to repair
 * with e2fsck and compare. A journaled image given after --journaled must refuse the
 * option. */

#define UNJOURNALED_SECONDS 1700030000
#define PERMISSIONS 0640U
#define DIRECTORY_PERMISSIONS 0750U
#define KEEP_A_BLOCKS 3U
#define WORK_BIG_BLOCKS 8U
#define WORK_VICTIM_BLOCKS 2U
#define NEW_FILE_BLOCKS 6U
#define TRUNCATED_TAIL 7U
#define ATTRIBUTE_BYTES 200U
#define FAST_TARGET "keep-target"
#define PATTERN_SEED 0x5aU
#define MISUSED_FLAG 0x80U
#define MISUSED_BLOCKS 16U

enum operation {
	OPERATION_CREATE,
	OPERATION_MKDIR,
	OPERATION_RENAME,
	OPERATION_TRUNCATE,
	OPERATION_UNLINK,
	OPERATION_ATTRIBUTE,
	OPERATION_SYMLINK,
	OPERATION_COUNT
};

static const char *const operation_names[OPERATION_COUNT] = { "create", "mkdir", "rename",
	"truncate", "unlink", "attribute", "symlink" };

static const struct ext4_timestamp unjournaled_time = { UNJOURNALED_SECONDS, 0 };

static struct ext4_inode_update
creation(uint16_t permissions)
{
	struct ext4_inode_update update = { 0 };

	update.fields = EXT4_ATTR_PERMISSIONS | EXT4_ATTR_UID | EXT4_ATTR_GID |
	    EXT4_ATTR_ACCESS_TIME | EXT4_ATTR_MODIFY_TIME | EXT4_ATTR_CHANGE_TIME |
	    EXT4_ATTR_XATTRS;
	update.permissions = permissions;
	update.access_time = unjournaled_time;
	update.modify_time = unjournaled_time;
	update.change_time = unjournaled_time;
	return update;
}

static struct ext4_inode_update
modification(const struct ext4_inode *inode)
{
	struct ext4_inode_update update = { 0 };

	update.fields = EXT4_ATTR_PERMISSIONS | EXT4_ATTR_MODIFY_TIME | EXT4_ATTR_CHANGE_TIME |
	    EXT4_ATTR_XATTRS;
	update.permissions = (uint16_t)(inode->mode & 07777U);
	update.modify_time = unjournaled_time;
	update.change_time = unjournaled_time;
	return update;
}

static void
pattern(uint8_t *buffer, size_t length, unsigned int seed)
{
	size_t index;

	for (index = 0; index < length; index++) {
		buffer[index] = (uint8_t)(seed * PATTERN_SEED + index * 31U + (index >> 8));
	}
}

static struct ext4_fs *
mount_unjournaled(struct device *device)
{
	struct ext4_write_options options = { 0, EXT4_WRITE_UNJOURNALED, 0 };
	struct ext4_fs *fs;

	EXPECT(ext4_mount_writable_with_options(
		   &device->environment, &device->writer, NULL, &options, &fs),
	    EXT4_OK);
	return fs;
}

static bool
volume_valid(const struct device *device)
{
	const struct ext4_super_disk *super =
	    (const struct ext4_super_disk *)(device->cache + EXT4_SUPER_OFFSET);

	return (ext4_le16(&super->state) & EXT4_VALID_FS) != 0;
}

static struct ext4_inode
find(struct ext4_fs *fs, uint32_t parent, const char *name)
{
	struct ext4_inode directory;
	struct ext4_inode inode;

	EXPECT(ext4_get_inode(fs, parent, &directory), EXT4_OK);
	EXPECT(ext4_lookup(fs, &directory, (const uint8_t *)name, strlen(name), &inode), EXT4_OK);
	return inode;
}

static struct ext4_inode
make_file(struct ext4_fs *fs, uint32_t parent, const char *name, size_t length, unsigned int seed)
{
	struct ext4_inode_update update = creation(PERMISSIONS);
	struct ext4_inode directory;
	struct ext4_inode inode;
	uint8_t *bytes = malloc(length == 0 ? 1U : length);
	size_t completed;

	CHECK(bytes != NULL);
	pattern(bytes, length, seed);
	EXPECT(ext4_get_inode(fs, parent, &directory), EXT4_OK);
	EXPECT(ext4_create(fs, directory.number, directory.generation, (const uint8_t *)name,
		   strlen(name), &update, &unjournaled_time, &inode),
	    EXT4_OK);
	update = modification(&inode);
	EXPECT(
	    ext4_write(fs, inode.number, inode.generation, 0, bytes, length, &update, &completed),
	    EXT4_OK);
	free(bytes);
	return find(fs, parent, name);
}

static struct ext4_inode
make_directory(struct ext4_fs *fs, uint32_t parent, const char *name)
{
	struct ext4_inode_update update = creation(DIRECTORY_PERMISSIONS);
	struct ext4_inode directory;
	struct ext4_inode inode;

	EXPECT(ext4_get_inode(fs, parent, &directory), EXT4_OK);
	EXPECT(ext4_mkdir(fs, directory.number, directory.generation, (const uint8_t *)name,
		   strlen(name), &update, &unjournaled_time, &inode),
	    EXT4_OK);
	return inode;
}

/* Misuse of the option, and writable mounts without it, change nothing. */
static void
refusals(struct device *device, const char *journaled)
{
	struct ext4_write_options options[] = { { MISUSED_BLOCKS, EXT4_WRITE_UNJOURNALED, 0 },
		{ 0, EXT4_WRITE_UNJOURNALED, MISUSED_BLOCKS }, { 0, MISUSED_FLAG, 0 } };
	struct ext4_journal_environment resource = { 0 };
	struct ext4_write_options unjournaled = { 0, EXT4_WRITE_UNJOURNALED, 0 };
	static struct device other;
	struct ext4_fs *fs;
	unsigned int index;

	device_reset(device, device->base);
	EXPECT(ext4_mount_writable(&device->environment, &device->writer, &fs), EXT4_UNSUPPORTED);
	for (index = 0; index < sizeof(options) / sizeof(options[0]); index++) {
		EXPECT(ext4_mount_writable_with_options(
			   &device->environment, &device->writer, NULL, &options[index], &fs),
		    EXT4_INVALID_ARGUMENT);
	}
	EXPECT(ext4_mount_writable_with_options(
		   &device->environment, &device->writer, &resource, &unjournaled, &fs),
	    EXT4_INVALID_ARGUMENT);
	CHECK(device->writes == 0 && device->live == 0);
	if (journaled != NULL) {
		storage_open(&other, journaled);
		EXPECT(ext4_mount_writable_with_options(
			   &other.environment, &other.writer, NULL, &unjournaled, &fs),
		    EXT4_INVALID_ARGUMENT);
		CHECK(other.writes == 0 && other.live == 0);
		storage_close(&other);
	}
	puts("PASS unjournaled writes need the option, and the option needs a volume without a "
	     "journal");
}

/* The valid state is cleared before the first change after a mount or a sync and
 * set by sync; a volume left in use needs e2fsck. */
static void
lifecycle(struct device *device)
{
	struct ext4_recovery_report report;
	struct ext4_inode_update update;
	struct ext4_inode inode;
	struct ext4_fs *fs;
	struct ext4_fs *other;
	uint8_t bytes[EXT4_MIN_BLOCK_SIZE];
	uint32_t writes;
	size_t completed;

	device_reset(device, device->base);
	fs = mount_unjournaled(device);
	CHECK(device->writes == 0 && volume_valid(device));
	inode = make_file(fs, EXT4_ROOT_INODE, "first", sizeof(bytes), 1);
	CHECK(!volume_valid(device));
	EXPECT(ext4_mount(&device->environment, &other), EXT4_CHECK_REQUIRED);
	writes = device->writes;
	EXPECT(ext4_recover(&device->environment, &device->writer, &report), EXT4_CHECK_REQUIRED);
	CHECK(device->writes == writes);
	EXPECT(ext4_commit(fs), EXT4_OK);
	CHECK(!volume_valid(device));
	EXPECT(ext4_sync(fs), EXT4_OK);
	CHECK(volume_valid(device));
	writes = device->writes;
	EXPECT(ext4_sync(fs), EXT4_OK);
	CHECK(device->writes == writes);
	EXPECT(ext4_mount(&device->environment, &other), EXT4_OK);
	inode = find(other, EXT4_ROOT_INODE, "first");
	EXPECT(ext4_read(other, &inode, 0, bytes, sizeof(bytes), &completed), EXT4_OK);
	CHECK(completed == sizeof(bytes));
	ext4_unmount(other);
	/* A change after the sync marks the volume in use again; an unmount without
	 * sync leaves it that way. */
	update = modification(&inode);
	pattern(bytes, sizeof(bytes), 2);
	EXPECT(ext4_write(fs, inode.number, inode.generation, 0, bytes, sizeof(bytes), &update,
		   &completed),
	    EXT4_OK);
	CHECK(!volume_valid(device));
	ext4_unmount(fs);
	EXPECT(ext4_mount(&device->environment, &other), EXT4_CHECK_REQUIRED);
	CHECK(device->live == 0);
	puts("PASS unjournaled writes mark the volume in use until sync");
}

/* keep/ holds files that no operation touches; work/ holds their targets. */
static void
prepare(struct device *device)
{
	struct ext4_inode_update update = creation(PERMISSIONS);
	struct ext4_inode keep;
	struct ext4_inode work;
	struct ext4_inode result;
	struct ext4_fs *fs;

	device_reset(device, device->base);
	fs = mount_unjournaled(device);
	keep = make_directory(fs, EXT4_ROOT_INODE, "keep");
	work = make_directory(fs, EXT4_ROOT_INODE, "work");
	make_file(fs, keep.number, "a", KEEP_A_BLOCKS * device->block_size + 11U, 1);
	make_file(fs, keep.number, "b", device->block_size, 2);
	EXPECT(ext4_symlink(fs, keep.number, keep.generation, (const uint8_t *)"fast", 4,
		   (const uint8_t *)FAST_TARGET, strlen(FAST_TARGET), &update, &unjournaled_time,
		   &result),
	    EXT4_OK);
	make_file(fs, work.number, "big", WORK_BIG_BLOCKS * device->block_size, 3);
	make_file(fs, work.number, "victim", WORK_VICTIM_BLOCKS * device->block_size, 4);
	make_directory(fs, work.number, "sub");
	EXPECT(ext4_sync(fs), EXT4_OK);
	ext4_unmount(fs);
	CHECK(volume_valid(device) && device->live == 0);
}

static enum ext4_result
operate(struct ext4_fs *fs, uint32_t block_size, enum operation operation)
{
	struct ext4_inode_update update = creation(PERMISSIONS);
	struct ext4_xattr_change change = { EXT4_XATTR_SET, EXT4_XATTR_USER, (const uint8_t *)"k",
		1, NULL, ATTRIBUTE_BYTES };
	struct ext4_inode work = find(fs, EXT4_ROOT_INODE, "work");
	struct ext4_inode sub = find(fs, work.number, "sub");
	struct ext4_inode big = find(fs, work.number, "big");
	struct ext4_inode victim = find(fs, work.number, "victim");
	struct ext4_rename_entry from = { work.number, work.generation, (const uint8_t *)"big", 3,
		big.number, big.generation };
	struct ext4_rename_entry to = { sub.number, sub.generation, (const uint8_t *)"moved", 5, 0,
		0 };
	struct ext4_inode result;
	uint8_t *bytes = malloc((size_t)NEW_FILE_BLOCKS * block_size);
	size_t completed;
	size_t index;
	enum ext4_result error;

	CHECK(bytes != NULL);
	pattern(bytes, (size_t)NEW_FILE_BLOCKS * block_size, 5);
	switch (operation) {
	case OPERATION_CREATE:
		error = ext4_create(fs, work.number, work.generation, (const uint8_t *)"new", 3,
		    &update, &unjournaled_time, &result);
		if (error == EXT4_OK) {
			update = modification(&result);
			error = ext4_write(fs, result.number, result.generation, 0, bytes,
			    (size_t)NEW_FILE_BLOCKS * block_size, &update, &completed);
		}
		break;
	case OPERATION_MKDIR:
		update.permissions = DIRECTORY_PERMISSIONS;
		error = ext4_mkdir(fs, work.number, work.generation, (const uint8_t *)"newdir", 6,
		    &update, &unjournaled_time, &result);
		break;
	case OPERATION_RENAME:
		error = ext4_rename(fs, &from, &to, 0, &unjournaled_time, &result);
		break;
	case OPERATION_TRUNCATE:
		update = modification(&big);
		error = ext4_truncate(
		    fs, big.number, big.generation, block_size + TRUNCATED_TAIL, &update, &result);
		break;
	case OPERATION_UNLINK:
		error = ext4_unlink(fs, work.number, work.generation, (const uint8_t *)"victim", 6,
		    victim.number, victim.generation, &unjournaled_time, &result);
		break;
	case OPERATION_ATTRIBUTE:
		update.fields = EXT4_ATTR_CHANGE_TIME | EXT4_ATTR_XATTRS;
		change.value = bytes;
		update.xattrs = &change;
		update.xattr_count = 1;
		error = ext4_set_attributes(fs, big.number, big.generation, &update, &result);
		break;
	default:
		/* A block symlink's target, without NUL bytes. */
		pattern(bytes, block_size, 6);
		for (index = 0; index < block_size / 2U; index++) {
			bytes[index] = (uint8_t)('a' + bytes[index] % 26U);
		}
		error = ext4_symlink(fs, work.number, work.generation, (const uint8_t *)"slink", 5,
		    bytes, block_size / 2U, &update, &unjournaled_time, &result);
		break;
	}
	free(bytes);
	return error;
}

static void
export_cut(struct device *device, const char *exports, const char *source, enum operation operation,
    uint32_t cut)
{
	char prefix[64];
	int length;

	length = snprintf(
	    prefix, sizeof(prefix), "unjournaled-%s-%03u-", operation_names[operation], cut);
	CHECK(length > 0 && (size_t)length < sizeof(prefix));
	storage_export(device, exports, source, prefix);
}

/* The files of keep/, which every repaired image must still hold. */
static void
export_manifest(struct device *device, const char *exports, const char *source)
{
	static const char *const names[] = { "a", "b" };
	struct ext4_sha256 hash;
	struct ext4_inode inode;
	struct ext4_fs *fs;
	uint8_t digest[EXT4_SHA256_DIGEST_SIZE];
	uint8_t *bytes = malloc((KEEP_A_BLOCKS + 1U) * device->block_size);
	const char *name = strrchr(source, '/');
	char path[4096];
	size_t completed;
	unsigned int index;
	unsigned int byte;
	FILE *stream;
	int length;

	CHECK(bytes != NULL);
	name = name == NULL ? source : name + 1;
	length = snprintf(path, sizeof(path), "%s/unjournaled-%s.manifest", exports, name);
	CHECK(length > 0 && (size_t)length < sizeof(path));
	stream = fopen(path, "wx");
	CHECK(stream != NULL);
	EXPECT(ext4_mount(&device->environment, &fs), EXT4_OK);
	for (index = 0; index < sizeof(names) / sizeof(names[0]); index++) {
		inode = find(fs, find(fs, EXT4_ROOT_INODE, "keep").number, names[index]);
		EXPECT(ext4_read(fs, &inode, 0, bytes, (KEEP_A_BLOCKS + 1U) * device->block_size,
			   &completed),
		    EXT4_OK);
		ext4_sha256_init(&hash);
		ext4_sha256_update(&hash, bytes, completed);
		ext4_sha256_final(&hash, digest);
		fprintf(stream, "file keep/%s %zu ", names[index], completed);
		for (byte = 0; byte < sizeof(digest); byte++) {
			fprintf(stream, "%02x", digest[byte]);
		}
		fprintf(stream, "\n");
	}
	fprintf(stream, "symlink keep/fast %s\n", FAST_TARGET);
	ext4_unmount(fs);
	CHECK(fclose(stream) == 0);
	free(bytes);
}

/* A power cut at every write or barrier of each operation. */
static void
power_cuts(struct device *device, const char *exports, const char *source)
{
	struct ext4_recovery_report report;
	struct ext4_write_options unjournaled = { 0, EXT4_WRITE_UNJOURNALED, 0 };
	struct ext4_fs *fs;
	uint8_t *before = malloc(device->size);
	uint32_t events;
	uint32_t cut;
	uint32_t writes;
	uint32_t old = 0;
	uint32_t checked = 0;
	uint32_t torn = 0;
	unsigned int operation;
	const struct ext4_super_disk *super;
	enum ext4_result error;

	CHECK(before != NULL);
	prepare(device);
	memcpy(before, device->stable, device->size);
	if (exports != NULL) {
		export_manifest(device, exports, source);
	}
	for (operation = 0; operation < OPERATION_COUNT; operation++) {
		device_reset(device, before);
		fs = mount_unjournaled(device);
		events = device->events;
		EXPECT(operate(fs, device->block_size, (enum operation)operation), EXT4_OK);
		events = device->events - events;
		ext4_unmount(fs);
		CHECK(events != 0);
		for (cut = 1; cut <= events; cut++) {
			device_reset(device, before);
			fs = mount_unjournaled(device);
			device->stop_at = device->events + cut;
			device->survival = cut % 3U;
			device->partial = cut % 2U != 0;
			error = operate(fs, device->block_size, (enum operation)operation);
			CHECK(error != EXT4_OK && device->off);
			ext4_unmount(fs);
			CHECK(device->live == 0);
			device_reset(device, device->stable);
			error = ext4_mount(&device->environment, &fs);
			if (error == EXT4_OK) {
				/* Nothing that reached the device changed the volume. */
				ext4_unmount(fs);
				CHECK(storage_equal(device, before));
				old++;
				continue;
			}
			/* A torn primary superblock fails its checksum; e2fsck uses a backup. */
			if (error == EXT4_CORRUPT) {
				super = (const struct ext4_super_disk *)(device->cache +
				    EXT4_SUPER_OFFSET);
				CHECK(device->metadata_checksum &&
				    ext4_le32(&super->checksum) !=
					ext4_crc32c(UINT32_MAX, super,
					    offsetof(struct ext4_super_disk, checksum)));
				torn++;
			} else {
				EXPECT(error, EXT4_CHECK_REQUIRED);
			}
			writes = device->writes;
			EXPECT(ext4_recover(&device->environment, &device->writer, &report), error);
			EXPECT(ext4_mount_writable_with_options(
				   &device->environment, &device->writer, NULL, &unjournaled, &fs),
			    error);
			CHECK(device->writes == writes && device->live == 0);
			checked++;
			if (exports != NULL) {
				export_cut(device, exports, source, (enum operation)operation, cut);
			}
		}
	}
	free(before);
	CHECK(checked != 0);
	printf("PASS unjournaled power cuts: %u unchanged volumes, %u needing e2fsck, %u of them "
	       "with a torn superblock\n",
	    old, checked, torn);
}

int
main(int argc, char **argv)
{
	static struct device device;
	const char *exports = NULL;
	const char *journaled = NULL;
	int argument = 1;

	while (argument + 1 < argc && argv[argument][0] == '-') {
		if (strcmp(argv[argument], "--export") == 0) {
			exports = argv[argument + 1];
		} else if (strcmp(argv[argument], "--journaled") == 0) {
			journaled = argv[argument + 1];
		} else {
			CHECK(false);
		}
		argument += 2;
	}
	CHECK(argument < argc);
	for (; argument < argc; argument++) {
		printf("IMAGE %s\n", argv[argument]);
		storage_open(&device, argv[argument]);
		refusals(&device, journaled);
		lifecycle(&device);
		power_cuts(&device, exports, argv[argument]);
		storage_close(&device);
	}
	return 0;
}

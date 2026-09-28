/* SPDX-License-Identifier: BSD-3-Clause */
#include "storage.h"

#include <inttypes.h>

#define GEOMETRY_SECONDS 1700000300
#define GEOMETRY_NAME "geometry-file"

enum operation { ALLOCATE_FILE, WRITE_FILE, REMOVE_FILE };

static const char *const operation_names[] = { "create", "write", "remove" };
static const struct ext4_timestamp geometry_time = { GEOMETRY_SECONDS, 0 };

static struct ext4_inode_update
attributes(void)
{
	struct ext4_inode_update update = { 0 };

	update.fields = EXT4_ATTR_PERMISSIONS | EXT4_ATTR_UID | EXT4_ATTR_GID |
	    EXT4_ATTR_ACCESS_TIME | EXT4_ATTR_MODIFY_TIME | EXT4_ATTR_CHANGE_TIME;
	update.permissions = 0640;
	update.uid = 12345;
	update.gid = 23456;
	update.access_time = update.modify_time = update.change_time = geometry_time;
	return update;
}

static struct ext4_fs *
mount_writer(struct device *device)
{
	struct ext4_fs *fs;

	EXPECT(ext4_mount_writable(&device->environment, &device->writer, &fs), EXT4_OK);
	return fs;
}

static struct ext4_inode
lookup(struct ext4_fs *fs, const char *name)
{
	struct ext4_inode root;
	struct ext4_inode inode;

	EXPECT(ext4_get_inode(fs, EXT4_ROOT_INODE, &root), EXT4_OK);
	EXPECT(ext4_lookup(fs, &root, (const uint8_t *)name, strlen(name), &inode), EXT4_OK);
	return inode;
}

static void
layout_check(struct device *device, struct ext4_fs *fs, const char *expected, bool preserved)
{
	struct ext4_block_range range;
	struct ext4_group group;
	FILE *stream = fopen(expected, "r");
	uint8_t *primary = calloc(device->blocks, 1);
	uint64_t first;
	uint64_t block_bitmap;
	uint64_t inode_bitmap;
	uint64_t inode_table;
	uint64_t offset;
	uint64_t block;
	uint64_t length;
	int64_t super;
	int64_t descriptors;
	int64_t last_descriptor;
	uint32_t index;
	uint32_t seen = 0;

	CHECK(stream != NULL && primary != NULL);
	primary[EXT4_SUPER_OFFSET / device->block_size] = 1;
	for (index = 0; index < fs->info.groups; index++) {
		EXPECT(ext4_group_descriptor_offset(fs, index, &offset), EXT4_OK);
		CHECK(offset / device->block_size < device->blocks);
		primary[offset / device->block_size] = 1;
	}
	while (fscanf(stream,
		   "%" SCNu32 " %" SCNu64 " %" SCNd64 " %" SCNd64 " %" SCNd64 " %" SCNu64
		   " %" SCNu64 " %" SCNu64,
		   &index, &first, &super, &descriptors, &last_descriptor, &block_bitmap,
		   &inode_bitmap, &inode_table) == 8) {
		CHECK(index == seen++);
		EXPECT(ext4_group_get(fs, index, &group), EXT4_OK);
		CHECK(group.block_bitmap == block_bitmap && group.inode_bitmap == inode_bitmap &&
		    group.inode_table == inode_table);
		CHECK(ext4_group_has_super(fs, index) == (super >= 0));
		EXPECT(ext4_group_reserved(fs, index, &range), EXT4_OK);
		length = (super >= 0 ? 1U : 0U) +
		    (descriptors >= 0 ? (uint64_t)(last_descriptor - descriptors + 1) : 0U);
		CHECK(range.first == first && range.length == length);
		CHECK(ext4_system_block(fs, block_bitmap) && ext4_system_block(fs, inode_bitmap));
		for (block = inode_table; block < inode_table + group.table_blocks; block++) {
			CHECK(ext4_system_block(fs, block));
		}
		for (block = first; block < first + length; block++) {
			CHECK(ext4_system_block(fs, block));
			if (preserved && !primary[block]) {
				CHECK(memcmp(device->base + block * device->block_size,
					  device->cache + block * device->block_size,
					  device->block_size) == 0);
			}
		}
	}
	CHECK(feof(stream) && seen == fs->info.groups && fclose(stream) == 0);
	EXPECT(ext4_group_descriptor_offset(fs, fs->info.groups, &offset), EXT4_CORRUPT);
	EXPECT(ext4_group_reserved(fs, fs->info.groups, &range), EXT4_CORRUPT);
	free(primary);
}

static void
prepare(struct device *device, const char *expected)
{
	struct ext4_fs *fs = mount_writer(device);
	struct ext4_inode_update update = attributes();
	struct ext4_inode_update write_update = update;
	struct ext4_inode root;
	struct ext4_inode inode;
	uint8_t *bytes = malloc(device->block_size + 3U);
	uint32_t previous = UINT32_MAX;
	uint32_t group = 0;
	uint32_t index = 0;
	size_t completed;
	char name[32];
	int length;

	CHECK(bytes != NULL);
	write_update.fields = EXT4_ATTR_PERMISSIONS | EXT4_ATTR_MODIFY_TIME | EXT4_ATTR_CHANGE_TIME;
	layout_check(device, fs, expected, false);
	EXPECT(ext4_get_inode(fs, EXT4_ROOT_INODE, &root), EXT4_OK);
	/* Public allocation crosses every descriptor block, initializes lazy groups,
	 * and allocates file data near each group's protected metadata prefix. */
	while (group + 1U < fs->info.groups) {
		length = snprintf(name, sizeof(name), "inode-%05u", index++);
		CHECK(length > 0 && (size_t)length < sizeof(name));
		EXPECT(ext4_create(fs, root.number, root.generation, (const uint8_t *)name,
			   (size_t)length, &update, &geometry_time, &inode),
		    EXT4_OK);
		group = (inode.number - 1U) / fs->inodes_per_group;
		if (group != previous) {
			memset(bytes, (int)(group + 1U), device->block_size + 3U);
			EXPECT(ext4_write(fs, inode.number, inode.generation, 0, bytes,
				   device->block_size + 3U, &write_update, &completed),
			    EXT4_OK);
			CHECK(completed == device->block_size + 3U);
			previous = group;
		}
	}
	EXPECT(ext4_create(fs, root.number, root.generation, (const uint8_t *)GEOMETRY_NAME,
		   sizeof(GEOMETRY_NAME) - 1U, &update, &geometry_time, &inode),
	    EXT4_OK);
	CHECK((inode.number - 1U) / fs->inodes_per_group == fs->info.groups - 1U);
	layout_check(device, fs, expected, true);
	EXPECT(ext4_sync(fs), EXT4_OK);
	ext4_unmount(fs);
	free(bytes);
	printf("PASS descriptor traversal: %u allocated inodes\n", index + 1U);
}

static enum ext4_result
operate(struct ext4_fs *fs, enum operation operation, const struct ext4_inode *root,
    const struct ext4_inode *file)
{
	struct ext4_inode_update update = attributes();
	struct ext4_inode result;
	uint8_t *bytes;
	size_t index;
	size_t completed;
	enum ext4_result error;

	if (operation == ALLOCATE_FILE) {
		return ext4_create(fs, root->number, root->generation,
		    (const uint8_t *)"geometry-extra", 14, &update, &geometry_time, &result);
	}
	if (operation == REMOVE_FILE) {
		return ext4_unlink(fs, root->number, root->generation,
		    (const uint8_t *)GEOMETRY_NAME, sizeof(GEOMETRY_NAME) - 1U, file->number,
		    file->generation, &geometry_time, &result);
	}
	bytes = malloc(fs->info.block_size + 3U);
	CHECK(bytes != NULL);
	update.fields = EXT4_ATTR_PERMISSIONS | EXT4_ATTR_MODIFY_TIME | EXT4_ATTR_CHANGE_TIME;
	for (index = 0; index < fs->info.block_size + 3U; index++) {
		bytes[index] = (uint8_t)(index * 31U + 7U);
	}
	error = ext4_write(fs, file->number, file->generation, fs->info.block_size + 7U, bytes,
	    fs->info.block_size + 3U, &update, &completed);
	CHECK(completed == (error == EXT4_OK ? fs->info.block_size + 3U : 0));
	free(bytes);
	return error;
}

static void
mutation(struct device *device, enum operation operation, bool faults, const char *exports,
    const char *path, const char *expected_layout)
{
	struct ext4_fs *fs;
	struct ext4_inode root;
	struct ext4_inode file;
	uint8_t *original = device->base;
	uint8_t *prepared = malloc(device->size);
	uint8_t *expected = malloc(device->size);
	uint32_t allocations;
	uint32_t reads;
	uint32_t events;
	uint32_t barrier;
	uint32_t phase;
	uint32_t position;
	uint32_t count;
	uint32_t partial;
	uint32_t survival;
	uint32_t cuts = 0;
	uint32_t recovered = 0;
	bool committed;
	char prefix[96];
	enum ext4_result error;

	CHECK(prepared != NULL && expected != NULL);
	memcpy(prepared, device->stable, device->size);
	device->base = prepared;
	device_reset(device, prepared);
	snprintf(prefix, sizeof(prefix), "geometry-%s-before-", operation_names[operation]);
	storage_export(device, exports, path, prefix);
	fs = mount_writer(device);
	EXPECT(ext4_get_inode(fs, EXT4_ROOT_INODE, &root), EXT4_OK);
	file = lookup(fs, GEOMETRY_NAME);
	device->reads = device->allocations = 0;
	EXPECT(operate(fs, operation, &root, &file), EXT4_OK);
	allocations = device->allocations;
	reads = device->reads;
	events = device->events;
	barrier = device->commit_barrier;
	CHECK(barrier != 0 && barrier < events);
	layout_check(device, fs, expected_layout, true);
	EXPECT(ext4_sync(fs), EXT4_OK);
	memcpy(expected, device->stable, device->size);
	snprintf(prefix, sizeof(prefix), "geometry-%s-after-", operation_names[operation]);
	storage_export(device, exports, path, prefix);
	ext4_unmount(fs);
	for (phase = 0; faults && phase < 2; phase++) {
		count = phase == 0 ? allocations : reads;
		for (position = 1; position <= count; position++) {
			device_reset(device, prepared);
			fs = mount_writer(device);
			device->reads = device->allocations = 0;
			device->fail_allocation = phase == 0 ? position : 0;
			device->fail_read = phase == 1 ? position : 0;
			error = operate(fs, operation, &root, &file);
			EXPECT(error, phase == 0 ? EXT4_NO_MEMORY : EXT4_IO);
			committed = device->intent_durable;
			ext4_unmount(fs);
			CHECK(storage_recover(device, committed ? expected : prepared, true));
		}
	}
	for (position = 1; (faults || exports != NULL) && position <= events; position++) {
		if (!faults && position != barrier && position != barrier + 1U) {
			continue;
		}
		for (partial = 0; partial < (faults ? 2U : 1U); partial++) {
			for (survival = 0; survival < (faults ? 3U : 1U); survival++) {
				device_reset(device, prepared);
				fs = mount_writer(device);
				device->stop_at = position;
				device->partial = partial != 0;
				device->survival = survival;
				EXPECT(operate(fs, operation, &root, &file), EXT4_IO);
				CHECK(fs->aborted && device->off);
				committed = device->intent_durable;
				ext4_unmount(fs);
				snprintf(prefix, sizeof(prefix), "geometry-%s-%s-",
				    operation_names[operation],
				    committed ? "pending" : "uncommitted");
				storage_export(device, exports, path, prefix);
				recovered += storage_recover(device, expected, committed) ? 1U : 0U;
				cuts++;
			}
		}
	}
	device_reset(device, expected);
	device->base = original;
	free(expected);
	free(prepared);
	printf("PASS geometry %s: allocations=%u reads=%u cuts=%u recovered=%u "
	       "torn_super_fail_closed=%u\n",
	    operation_names[operation], allocations, reads, cuts, recovered, cuts - recovered);
}

static void
recovery_geometry(struct device *device)
{
	struct ext4_fs *fs;
	struct ext4_transaction *transaction;
	struct ext4_super_disk *super;
	struct ext4_recovery_report report;
	uint32_t barrier;
	uint32_t index;
	uint32_t value;
	bool meta;
	bool sparse;

	fs = mount_writer(device);
	meta = (fs->info.feature_incompat & EXT4_FEATURE_INCOMPAT_META_BG) != 0;
	sparse = (fs->info.feature_compat & EXT4_FEATURE_COMPAT_SPARSE_SUPER2) != 0;
	EXPECT(ext4_transaction_begin(fs->journal, 1, &transaction), EXT4_OK);
	EXPECT(ext4_transaction_super(transaction, &super), EXT4_OK);
	EXPECT(ext4_transaction_commit(transaction), EXT4_OK);
	barrier = device->commit_barrier;
	CHECK(barrier != 0);
	ext4_unmount(fs);
	for (index = 0; index < 4; index++) {
		if ((index == 1 && !meta) || (index >= 2 && !sparse)) {
			continue;
		}
		device_reset(device, device->base);
		fs = mount_writer(device);
		EXPECT(ext4_transaction_begin(fs->journal, 1, &transaction), EXT4_OK);
		EXPECT(ext4_transaction_super(transaction, &super), EXT4_OK);
		if (index == 0) {
			value = ext4_le32(&super->feature_incompat) ^ EXT4_FEATURE_INCOMPAT_META_BG;
			ext4_encode32(&super->feature_incompat, value);
		} else if (index == 1) {
			ext4_encode32(&super->first_meta_group, fs->first_meta_group + 1U);
		} else {
			ext4_encode32(&super->backup_groups[index - 2U], fs->info.groups - 2U);
		}
		/* Leave a checksummed committed record whose geometry
		 * disagrees with the primary owner. Replay must reject before home I/O. */
		device->stop_at = barrier + 1U;
		EXPECT(ext4_transaction_commit(transaction), EXT4_IO);
		CHECK(device->intent_durable);
		ext4_unmount(fs);
		device_reset(device, device->stable);
		EXPECT(
		    ext4_recover(&device->environment, &device->writer, &report), EXT4_UNSUPPORTED);
		CHECK(device->writes == 0 && device->live == 0);
	}
	device_reset(device, device->base);
}

static void
linux_read(struct device *device)
{
	struct ext4_fs *fs;
	struct ext4_inode file;
	struct ext4_inode root;
	struct ext4_group group;
	uint8_t *bytes = malloc(2U * device->block_size + 10U);
	uint8_t *seen;
	uint8_t expected;
	uint32_t index;
	uint32_t owner;
	size_t byte;
	size_t completed;
	char name[32];
	int length;
	enum ext4_result error;

	CHECK(bytes != NULL);
	EXPECT(ext4_mount(&device->environment, &fs), EXT4_OK);
	seen = calloc(fs->info.groups, 1);
	CHECK(seen != NULL);
	file = lookup(fs, GEOMETRY_NAME);
	CHECK(file.size == 2U * device->block_size + 10U);
	EXPECT(ext4_read(fs, &file, 0, bytes, (size_t)file.size, &completed), EXT4_OK);
	CHECK(completed == file.size);
	for (byte = 0; byte < completed; byte++) {
		expected = byte < device->block_size + 7U
		    ? 0
		    : (uint8_t)((byte - device->block_size - 7U) * 31U + 7U);
		if (byte == device->block_size + 7U) {
			expected ^= 0x5aU;
		}
		CHECK(bytes[byte] == expected);
	}
	EXPECT(ext4_get_inode(fs, EXT4_ROOT_INODE, &root), EXT4_OK);
	for (index = 0; index < fs->info.inodes; index++) {
		length = snprintf(name, sizeof(name), "inode-%05u", index);
		CHECK(length > 0 && (size_t)length < sizeof(name));
		error = ext4_lookup(fs, &root, (const uint8_t *)name, (size_t)length, &file);
		if (error == EXT4_NOT_FOUND) {
			break;
		}
		EXPECT(error, EXT4_OK);
		owner = (file.number - 1U) / fs->inodes_per_group;
		CHECK(owner < fs->info.groups);
		if (!seen[owner]) {
			CHECK(file.size == device->block_size + 3U);
			EXPECT(
			    ext4_read(fs, &file, 0, bytes, (size_t)file.size, &completed), EXT4_OK);
			CHECK(completed == file.size);
			for (byte = 0; byte < completed; byte++) {
				CHECK(bytes[byte] == owner + 1U);
			}
		} else {
			CHECK(file.size == 0);
		}
		seen[owner] = 1;
	}
	for (index = 0; index < fs->info.groups; index++) {
		CHECK(seen[index] != 0);
		EXPECT(ext4_group_get(fs, index, &group), EXT4_OK);
	}
	file = lookup(fs, "linux-link");
	CHECK(file.links == 2 && file.uid == 12345 && file.gid == 23456);
	ext4_unmount(fs);
	CHECK(device->writes == 0);
	free(seen);
	free(bytes);
	puts("PASS portable read of Linux geometry data, every seeded group and updated "
	     "descriptors");
}

static void
malformed(struct device *device)
{
	struct ext4_super_disk *super;
	struct ext4_fs *fs;
	uint32_t features;
	uint32_t index;
	uint32_t checksum;

	for (index = 0; index < 4; index++) {
		device_reset(device, device->base);
		super = (struct ext4_super_disk *)(device->cache + EXT4_SUPER_OFFSET);
		if (index == 0) {
			features =
			    ext4_le32(&super->feature_incompat) | EXT4_FEATURE_INCOMPAT_META_BG;
			ext4_encode32(&super->feature_incompat, features);
			ext4_encode32(&super->first_meta_group, UINT32_MAX);
		} else if (index < 3) {
			features =
			    ext4_le32(&super->feature_compat) | EXT4_FEATURE_COMPAT_SPARSE_SUPER2;
			ext4_encode32(&super->feature_compat, features);
			ext4_encode32(&super->backup_groups[index - 1U], UINT32_MAX);
		} else {
			ext4_encode32(&super->feature_compat,
			    ext4_le32(&super->feature_compat) | EXT4_FEATURE_COMPAT_RESIZE_INODE);
			ext4_encode32(&super->feature_incompat,
			    ext4_le32(&super->feature_incompat) | EXT4_FEATURE_INCOMPAT_META_BG);
		}
		if (device->metadata_checksum) {
			checksum = ext4_crc32c(
			    UINT32_MAX, super, offsetof(struct ext4_super_disk, checksum));
			ext4_encode32(&super->checksum, checksum);
		}
		EXPECT(ext4_mount(&device->environment, &fs), EXT4_CORRUPT);
		CHECK(fs == NULL && device->live == 0 && device->writes == 0);
	}
	device_reset(device, device->base);
}

int
main(int argc, char **argv)
{
	struct device device;
	const char *exports = NULL;
	bool faults = false;
	bool read_mode = false;
	int index = 1;
	unsigned int operation;

	if (index < argc && strcmp(argv[index], "--linux-read") == 0) {
		read_mode = true;
		index++;
	}
	if (index < argc && strcmp(argv[index], "--faults") == 0) {
		faults = true;
		index++;
	}
	if (index + 1 < argc && strcmp(argv[index], "--export") == 0) {
		exports = argv[index + 1];
		index += 2;
	}
	CHECK(index + (read_mode ? 1 : 2) == argc && !(faults && exports != NULL));
	CHECK(!read_mode || (!faults && exports == NULL));
	storage_open(&device, argv[index]);
	if (read_mode) {
		linux_read(&device);
		storage_close(&device);
		return 0;
	}
	malformed(&device);
	recovery_geometry(&device);
	prepare(&device, argv[index + 1]);
	for (operation = ALLOCATE_FILE; operation <= REMOVE_FILE; operation++) {
		mutation(&device, (enum operation)operation, faults, exports, argv[index],
		    argv[index + 1]);
	}
	storage_close(&device);
	printf("PASS distributed geometry: %s\n", argv[index]);
	return 0;
}

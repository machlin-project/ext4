/* SPDX-License-Identifier: BSD-3-Clause */
#include "storage.h"

#define XATTR_BINARY_BYTES 600U
#define XATTR_REPLACEMENT_BYTES 700U
#define XATTR_ACL_BYTES 64U
#define XATTR_HIGH_BYTES 13U
#define XATTR_SECURITY_BYTES 19U
#define XATTR_BATCH_KEYS 6U
#define XATTR_CHANGE_SECONDS 1700000070

enum operation {
	CREATE_ATTRIBUTES,
	COPY_SHARED_BLOCK,
	DROP_SHARED_REFERENCE,
	FREE_UNIQUE_BLOCK,
	REPLACE_MAPPED_SYMLINK,
	UPDATE_SECURITY_METADATA,
	REMOVE_OPTIONAL_ATTRIBUTE,
	PRESERVE_SHARED_ATTRIBUTES,
	PRESERVE_EMPTY_ATTRIBUTES,
	OPERATION_COUNT
};

static const char *const operation_names[] = { "create", "cow", "detach", "release", "replace",
	"metadata", "remove-optional", "preserve-shared", "preserve-empty" };
static const char *const operation_paths[] = { "plain", "block", "block", "symlink",
	"mapped-symlink", "many", "block", "block", "plain" };
static const uint8_t high_name[] = "high-\xc3\xa9";

struct operation_data {
	struct ext4_xattr_change changes[XATTR_BATCH_KEYS];
	struct ext4_inode_update update;
	uint8_t bytes[XATTR_REPLACEMENT_BYTES];
	uint8_t acl[XATTR_ACL_BYTES];
};

struct trace {
	uint32_t allocations;
	uint32_t reads;
	uint32_t events;
	uint32_t commit_event;
	bool committed;
};

static void
pattern(uint8_t *bytes, size_t size, bool changed)
{
	size_t index;

	for (index = 0; index < size; index++) {
		bytes[index] = (uint8_t)(index * (changed ? 17U : 29U) + (changed ? 0x51U : 0x83U));
	}
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

static struct ext4_inode_disk *
record(struct device *device, struct ext4_fs *fs, const struct ext4_inode *inode)
{
	uint64_t offset;

	EXPECT(ext4_inode_location(fs, inode->number, &offset), EXT4_OK);
	return (struct ext4_inode_disk *)(device->cache + offset);
}

static uint64_t
attribute_block(struct device *device, struct ext4_fs *fs, const struct ext4_inode *inode)
{
	struct ext4_inode_disk *disk = record(device, fs, inode);

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

static struct ext4_xattr_change
change(enum ext4_xattr_policy policy, uint8_t name_index, const char *name, const void *value,
    size_t size)
{
	struct ext4_xattr_change result;

	memset(&result, 0, sizeof(result));
	result.policy = policy;
	result.name_index = name_index;
	result.name = (const uint8_t *)name;
	result.name_length = strlen(name);
	result.value = value;
	result.value_size = size;
	return result;
}

static struct ext4_inode_update
attributes(const struct ext4_xattr_change *changes, size_t count)
{
	struct ext4_inode_update update;

	memset(&update, 0, sizeof(update));
	update.fields = EXT4_ATTR_XATTRS | EXT4_ATTR_CHANGE_TIME;
	update.change_time.seconds = XATTR_CHANGE_SECONDS;
	update.xattrs = changes;
	update.xattr_count = count;
	return update;
}

static void
value_is(struct ext4_fs *fs, const struct ext4_inode *inode, uint8_t name_index, const char *name,
    const void *expected, size_t size)
{
	uint8_t *bytes = malloc(size + 1);
	size_t returned = SIZE_MAX;

	CHECK(bytes != NULL);
	memset(bytes, 0xa5, size + 1);
	EXPECT(ext4_get_xattr(fs, inode->number, inode->generation, name_index,
		   (const uint8_t *)name, strlen(name), bytes, size, &returned),
	    EXT4_OK);
	CHECK(returned == size && (size == 0 || memcmp(bytes, expected, size) == 0) &&
	    bytes[size] == 0xa5);
	free(bytes);
}

static void
count_is(struct ext4_fs *fs, const struct ext4_inode *inode, size_t count)
{
	size_t returned = SIZE_MAX;

	EXPECT(ext4_list_xattrs(fs, inode->number, inode->generation, NULL, 0, &returned), EXT4_OK);
	CHECK(returned == count);
}

static void
prepare(struct ext4_fs *fs, const struct ext4_inode *inode, enum operation operation,
    struct operation_data *data)
{
	size_t acl_size;

	memset(data, 0, sizeof(*data));
	pattern(data->bytes, sizeof(data->bytes), true);
	data->update = attributes(data->changes, 1);
	data->changes[0] =
	    change(EXT4_XATTR_REPLACE, EXT4_XATTR_USER, "binary", data->bytes, sizeof(data->bytes));
	switch (operation) {
	case CREATE_ATTRIBUTES:
		data->changes[0] = change(EXT4_XATTR_CREATE, EXT4_XATTR_USER, "tiny", "abc", 3);
		data->changes[1] = change(EXT4_XATTR_SET, EXT4_XATTR_USER, "empty", NULL, 0);
		data->changes[2] = change(EXT4_XATTR_CREATE, EXT4_XATTR_USER,
		    (const char *)high_name, data->bytes, XATTR_HIGH_BYTES);
		data->update.xattr_count = 3;
		break;
	case COPY_SHARED_BLOCK:
	case REPLACE_MAPPED_SYMLINK:
		break;
	case DROP_SHARED_REFERENCE:
	case FREE_UNIQUE_BLOCK:
		data->changes[0] = change(EXT4_XATTR_REMOVE, EXT4_XATTR_USER, "binary", NULL, 0);
		break;
	case REMOVE_OPTIONAL_ATTRIBUTE:
		data->changes[0] =
		    change(EXT4_XATTR_REMOVE_IF_PRESENT, EXT4_XATTR_USER, "binary", NULL, 0);
		break;
	case PRESERVE_SHARED_ATTRIBUTES:
	case PRESERVE_EMPTY_ATTRIBUTES:
		data->changes[0] = change(
		    EXT4_XATTR_REMOVE_IF_PRESENT, EXT4_XATTR_SECURITY, "capability", NULL, 0);
		break;
	case UPDATE_SECURITY_METADATA:
		EXPECT(ext4_get_xattr(fs, inode->number, inode->generation,
			   EXT4_XATTR_POSIX_ACL_ACCESS, NULL, 0, data->acl, sizeof(data->acl),
			   &acl_size),
		    EXT4_OK);
		data->changes[1] = change(EXT4_XATTR_REMOVE, EXT4_XATTR_USER, "empty", NULL, 0);
		data->changes[2] = change(EXT4_XATTR_REPLACE, EXT4_XATTR_USER,
		    (const char *)high_name, data->bytes, XATTR_HIGH_BYTES);
		data->changes[3] = change(EXT4_XATTR_CREATE, EXT4_XATTR_SECURITY, "test",
		    data->bytes, XATTR_SECURITY_BYTES);
		data->changes[4] = change(
		    EXT4_XATTR_REPLACE, EXT4_XATTR_POSIX_ACL_ACCESS, "", data->acl, acl_size);
		data->update.xattr_count = 5;
		data->update.fields |= EXT4_ATTR_UID | EXT4_ATTR_GID | EXT4_ATTR_PERMISSIONS;
		data->update.uid = 70000;
		data->update.gid = 80000;
		data->update.permissions = 0761;
		break;
	case OPERATION_COUNT:
		CHECK(false);
	}
}

static void
verify(struct device *device, struct ext4_fs *fs, const struct ext4_inode *before,
    const struct ext4_inode *after, enum operation operation, const struct operation_data *data,
    uint64_t free_blocks, uint64_t old_block)
{
	struct ext4_inode shared = lookup(fs, "shared");
	uint8_t original[XATTR_BINARY_BYTES];
	uint64_t new_block = attribute_block(device, fs, after);
	uint64_t sectors = fs->info.block_size / EXT4_SECTOR_SIZE;
	size_t returned;

	CHECK(after->number == before->number && after->generation == before->generation &&
	    after->size == before->size && after->links == before->links &&
	    after->fast_symlink == before->fast_symlink && after->flags == before->flags &&
	    after->change_time.seconds == XATTR_CHANGE_SECONDS &&
	    after->change_time.nanoseconds == 0 &&
	    memcmp(&after->access_time, &before->access_time, sizeof(after->access_time)) == 0 &&
	    memcmp(&after->modify_time, &before->modify_time, sizeof(after->modify_time)) == 0 &&
	    memcmp(&after->birth_time, &before->birth_time, sizeof(after->birth_time)) == 0 &&
	    memcmp(after->block_data, before->block_data, sizeof(after->block_data)) == 0);
	if (operation == UPDATE_SECURITY_METADATA) {
		CHECK(after->uid == 70000 && after->gid == 80000 &&
		    (after->mode & EXT4_MODE_PERMISSIONS) == 0761);
	} else {
		CHECK(after->uid == before->uid && after->gid == before->gid &&
		    after->mode == before->mode);
	}
	pattern(original, sizeof(original), false);
	value_is(fs, &shared, EXT4_XATTR_USER, "binary", original, sizeof(original));
	switch (operation) {
	case CREATE_ATTRIBUTES:
		count_is(fs, after, 3);
		value_is(fs, after, EXT4_XATTR_USER, "tiny", "abc", 3);
		value_is(fs, after, EXT4_XATTR_USER, "empty", NULL, 0);
		value_is(fs, after, EXT4_XATTR_USER, (const char *)high_name, data->bytes,
		    XATTR_HIGH_BYTES);
		CHECK((new_block != 0) == (fs->inode_size == EXT4_INODE_BASE_SIZE));
		CHECK(after->blocks_512 == (new_block == 0 ? 0 : sectors) &&
		    fs->info.free_blocks == free_blocks - (new_block != 0));
		break;
	case COPY_SHARED_BLOCK:
		CHECK(new_block != 0 && new_block != old_block &&
		    references(device, old_block) == 1 && references(device, new_block) == 1 &&
		    fs->info.free_blocks == free_blocks - 1);
		CHECK(attribute_block(device, fs, &shared) == old_block);
		value_is(fs, after, EXT4_XATTR_USER, "binary", data->bytes, sizeof(data->bytes));
		CHECK(after->blocks_512 == before->blocks_512);
		break;
	case DROP_SHARED_REFERENCE:
	case FREE_UNIQUE_BLOCK:
	case REMOVE_OPTIONAL_ATTRIBUTE:
		count_is(fs, after, 0);
		returned = SIZE_MAX;
		EXPECT(ext4_get_xattr(fs, after->number, after->generation, EXT4_XATTR_USER,
			   (const uint8_t *)"binary", 6, NULL, 0, &returned),
		    EXT4_NOT_FOUND);
		CHECK(returned == SIZE_MAX && new_block == 0 &&
		    after->blocks_512 == before->blocks_512 - sectors);
		CHECK(fs->info.free_blocks == free_blocks + (operation == FREE_UNIQUE_BLOCK));
		if (operation != FREE_UNIQUE_BLOCK) {
			CHECK(references(device, old_block) == 1);
		}
		break;
	case PRESERVE_SHARED_ATTRIBUTES:
	case PRESERVE_EMPTY_ATTRIBUTES:
		count_is(fs, after, operation == PRESERVE_SHARED_ATTRIBUTES ? 1 : 0);
		CHECK(new_block == old_block && after->blocks_512 == before->blocks_512 &&
		    fs->info.free_blocks == free_blocks);
		if (operation == PRESERVE_SHARED_ATTRIBUTES) {
			CHECK(references(device, old_block) == 2);
			value_is(fs, after, EXT4_XATTR_USER, "binary", original, sizeof(original));
		}
		break;
	case REPLACE_MAPPED_SYMLINK:
		CHECK(new_block == old_block && after->blocks_512 == before->blocks_512 &&
		    fs->info.free_blocks == free_blocks);
		value_is(fs, after, EXT4_XATTR_USER, "binary", data->bytes, sizeof(data->bytes));
		break;
	case UPDATE_SECURITY_METADATA:
		count_is(fs, after, 6);
		value_is(fs, after, EXT4_XATTR_USER, "binary", data->bytes, sizeof(data->bytes));
		value_is(fs, after, EXT4_XATTR_USER, (const char *)high_name, data->bytes,
		    XATTR_HIGH_BYTES);
		value_is(fs, after, EXT4_XATTR_SECURITY, "test", data->bytes, XATTR_SECURITY_BYTES);
		value_is(fs, after, EXT4_XATTR_POSIX_ACL_ACCESS, "", data->acl,
		    data->changes[4].value_size);
		CHECK(new_block == old_block && after->blocks_512 == before->blocks_512 &&
		    fs->info.free_blocks == free_blocks);
		break;
	case OPERATION_COUNT:
		CHECK(false);
	}
}

static enum ext4_result
attempt(struct device *device, enum operation operation, unsigned int fault, uint32_t point,
    unsigned int survival, bool partial, struct trace *trace)
{
	struct ext4_fs *fs = mount_writer(device);
	struct ext4_inode inode = lookup(fs, operation_paths[operation]);
	struct ext4_inode result;
	struct ext4_inode untouched;
	struct operation_data data;
	uint64_t free_blocks = fs->info.free_blocks;
	uint64_t old_block = attribute_block(device, fs, &inode);
	uint32_t allocations;
	uint32_t reads;
	uint32_t events;
	uint32_t live = device->live;
	enum ext4_result error;

	prepare(fs, &inode, operation, &data);
	allocations = device->allocations;
	reads = device->reads;
	events = device->events;
	memset(&untouched, 0xa5, sizeof(untouched));
	result = untouched;
	device->survival = survival;
	device->partial = partial;
	if (fault == 1) {
		device->fail_allocation = allocations + point;
	} else if (fault == 2) {
		device->fail_read = reads + point;
	} else if (fault == 3) {
		device->stop_at = events + point;
	}
	error = ext4_set_attributes(fs, inode.number, inode.generation, &data.update, &result);
	if (error != EXT4_OK) {
		CHECK(memcmp(&result, &untouched, sizeof(result)) == 0 && device->live == live);
		if (device->writes == 0) {
			CHECK(fs->info.free_blocks == free_blocks &&
			    memcmp(device->cache, device->base, device->size) == 0 &&
			    memcmp(device->stable, device->base, device->size) == 0);
		}
	} else {
		error = ext4_sync(fs);
	}
	trace->allocations = device->allocations - allocations;
	trace->reads = device->reads - reads;
	trace->events = device->events - events;
	trace->commit_event = device->commit_barrier - events;
	trace->committed = device->intent_durable;
	if (error == EXT4_OK) {
		verify(device, fs, &inode, &result, operation, &data, free_blocks, old_block);
	} else if (device->writes != 0) {
		CHECK(fs->aborted);
		EXPECT(ext4_get_inode(fs, inode.number, &result), EXT4_RECOVERY_REQUIRED);
		EXPECT(
		    ext4_set_attributes(fs, inode.number, inode.generation, &data.update, &result),
		    EXT4_RECOVERY_REQUIRED);
	}
	ext4_unmount(fs);
	CHECK(device->live == 0);
	return error;
}

static void
faults(struct device *device, enum operation operation, bool smoke, const char *exports,
    const char *path)
{
	struct trace baseline;
	struct trace trace;
	uint8_t *expected = malloc(device->size);
	uint32_t point;
	uint32_t limit;
	uint32_t recovered = 0;
	uint32_t torn = 0;
	unsigned int fault;
	unsigned int survival;
	unsigned int partial;
	char prefix[64];

	CHECK(expected != NULL);
	device_reset(device, device->base);
	EXPECT(attempt(device, operation, 0, 0, 0, false, &baseline), EXT4_OK);
	memcpy(expected, device->stable, device->size);
	CHECK(snprintf(prefix, sizeof(prefix), "xattr-write-%s-", operation_names[operation]) > 0);
	storage_export(device, exports, path, prefix);
	CHECK(storage_recover(device, expected, true) && device->writes == 0);
	if (!smoke) {
		for (fault = 1; fault <= 2; fault++) {
			limit = fault == 1 ? baseline.allocations : baseline.reads;
			for (point = 1; point <= limit; point++) {
				device_reset(device, device->base);
				EXPECT(attempt(device, operation, fault, point, 0, false, &trace),
				    fault == 1 ? EXT4_NO_MEMORY : EXT4_IO);
				CHECK(storage_recover(device, expected, trace.committed));
			}
		}
		for (point = 1; point <= baseline.events; point++) {
			for (survival = 0; survival < 3; survival++) {
				for (partial = 0; partial < 2; partial++) {
					device_reset(device, device->base);
					EXPECT(attempt(device, operation, 3, point, survival,
						   partial != 0, &trace),
					    EXT4_IO);
					CHECK(device->off);
					if (storage_recover(device, expected, trace.committed)) {
						recovered++;
					} else {
						torn++;
					}
				}
			}
		}
		printf("PASS xattr mutation faults operation=%s allocations=%u reads=%u cuts=%u "
		       "recovered=%u torn_super_fail_closed=%u\n",
		    operation_names[operation], baseline.allocations, baseline.reads,
		    baseline.events * 6, recovered, torn);
	}
	if (exports != NULL) {
		CHECK(baseline.commit_event > 1 && baseline.commit_event < baseline.events);
		device_reset(device, device->base);
		EXPECT(attempt(device, operation, 3, baseline.commit_event + 1, 0, false, &trace),
		    EXT4_IO);
		CHECK(trace.committed);
		CHECK(snprintf(prefix, sizeof(prefix), "xattr-pending-%s-",
			  operation_names[operation]) > 0);
		storage_export(device, exports, path, prefix);
		CHECK(storage_recover(device, expected, true));
		device_reset(device, device->base);
		EXPECT(attempt(device, operation, 3, baseline.commit_event - 1, 0, false, &trace),
		    EXT4_IO);
		CHECK(!trace.committed);
		CHECK(snprintf(prefix, sizeof(prefix), "xattr-uncommitted-%s-",
			  operation_names[operation]) > 0);
		storage_export(device, exports, path, prefix);
		CHECK(storage_recover(device, expected, false));
	}
	free(expected);
}

static void
rejected(struct device *device)
{
	struct ext4_fs *fs;
	struct ext4_inode inode;
	struct ext4_inode result;
	struct ext4_inode untouched;
	struct ext4_inode_update update;
	struct ext4_xattr_change changes[2];
	uint8_t nul_name[] = { 'a', 0, 'b' };
	uint32_t live;
	unsigned int index;
	enum ext4_result expected;

	device_reset(device, device->base);
	fs = mount_writer(device);
	inode = lookup(fs, "block");
	live = device->live;
	memset(&untouched, 0xa5, sizeof(untouched));
	for (index = 0; index < 19; index++) {
		changes[0] = change(EXT4_XATTR_CREATE, EXT4_XATTR_USER, "new", "abc", 3);
		changes[1] = change(EXT4_XATTR_CREATE, EXT4_XATTR_USER, "binary", "abc", 3);
		update = attributes(changes, 2);
		expected = EXT4_INVALID_ARGUMENT;
		switch (index) {
		case 0:
			expected = EXT4_EXISTS;
			break;
		case 1:
		case 2:
			changes[1] = change(index == 1 ? EXT4_XATTR_REPLACE : EXT4_XATTR_REMOVE,
			    EXT4_XATTR_USER, "missing", NULL, 0);
			expected = EXT4_NOT_FOUND;
			break;
		case 3:
			changes[1] = changes[0];
			break;
		case 4:
			changes[0].name = NULL;
			break;
		case 5:
			changes[0].name = nul_name;
			changes[0].name_length = sizeof(nul_name);
			break;
		case 6:
			changes[0].name_length = EXT4_NAME_MAX + 1;
			break;
		case 7:
			changes[0].value = NULL;
			break;
		case 8:
			changes[0].name_index = 0;
			break;
		case 9:
			changes[0].policy =
			    (enum ext4_xattr_policy)(EXT4_XATTR_REMOVE_IF_PRESENT + 1);
			break;
		case 10:
			changes[0].policy = EXT4_XATTR_REMOVE;
			break;
		case 11:
			update.fields = EXT4_ATTR_XATTRS;
			break;
		case 12:
			update.xattrs = NULL;
			break;
		case 13:
			update.xattr_count = EXT4_XATTR_MAX_CHANGES + 1;
			expected = EXT4_RANGE;
			break;
		case 14:
			changes[0].value_size = (size_t)fs->info.block_size + 1;
			expected = EXT4_NO_SPACE;
			break;
		case 15:
			changes[0].policy = EXT4_XATTR_REMOVE_IF_PRESENT;
			break;
		case 16:
		case 18:
			if (index == 18) {
				inode = lookup(fs, "plain");
			}
			changes[0] = change(
			    EXT4_XATTR_REMOVE_IF_PRESENT, EXT4_XATTR_USER, "missing", NULL, 0);
			changes[1] = changes[0];
			break;
		case 17:
			changes[0] =
			    change(EXT4_XATTR_REMOVE_IF_PRESENT, EXT4_XATTR_USER, "new", NULL, 0);
			changes[1] = change(EXT4_XATTR_CREATE, EXT4_XATTR_USER, "new", "abc", 3);
			break;
		}
		result = untouched;
		EXPECT(ext4_set_attributes(fs, inode.number, inode.generation, &update, &result),
		    expected);
		CHECK(memcmp(&result, &untouched, sizeof(result)) == 0 && device->live == live &&
		    device->writes == 0 && device->events == 0 && !fs->aborted &&
		    memcmp(device->cache, device->base, device->size) == 0 &&
		    memcmp(device->stable, device->base, device->size) == 0);
	}
	update = attributes(NULL, 0);
	EXPECT(ext4_set_attributes(fs, inode.number, inode.generation ^ 1U, &update, &result),
	    EXT4_STALE);
	CHECK(device->writes == 0);
	ext4_unmount(fs);
	device_reset(device, device->base);
	EXPECT(ext4_mount(&device->environment, &fs), EXT4_OK);
	EXPECT(ext4_set_attributes(fs, inode.number, inode.generation, &update, &result),
	    EXT4_READ_ONLY);
	CHECK(memcmp(&result, &untouched, sizeof(result)) == 0 && device->writes == 0);
	ext4_unmount(fs);
	puts("PASS xattr batch policy and argument rejection: 21 unchanged-image/output cases");
}

static void
storage_transitions(struct device *device, const char *exports, const char *path)
{
	struct ext4_fs *fs;
	struct ext4_inode inode;
	struct ext4_inode result;
	struct ext4_inode shared;
	struct ext4_xattr_change batch[2];
	struct ext4_inode_update update;
	uint8_t *before = malloc(device->block_size);
	uint8_t original[XATTR_BINARY_BYTES];
	uint64_t block;
	uint64_t free_blocks;

	CHECK(before != NULL);
	device_reset(device, device->base);
	fs = mount_writer(device);
	inode = lookup(fs, "block");
	shared = lookup(fs, "shared");
	block = attribute_block(device, fs, &inode);
	free_blocks = fs->info.free_blocks;
	memcpy(before, device->cache + block * device->block_size, device->block_size);
	pattern(original, sizeof(original), false);
	batch[0] =
	    change(EXT4_XATTR_REPLACE, EXT4_XATTR_USER, "binary", original, sizeof(original));
	update = attributes(batch, 1);
	EXPECT(ext4_set_attributes(fs, inode.number, inode.generation, &update, &result), EXT4_OK);
	CHECK(attribute_block(device, fs, &result) == block &&
	    fs->info.free_blocks == free_blocks && result.blocks_512 == inode.blocks_512 &&
	    references(device, block) == 2 &&
	    memcmp(before, device->cache + block * device->block_size, device->block_size) == 0);
	batch[0] = change(EXT4_XATTR_REMOVE_IF_PRESENT, EXT4_XATTR_SECURITY, "capability", NULL, 0);
	EXPECT(ext4_set_attributes(fs, inode.number, inode.generation, &update, &result), EXT4_OK);
	CHECK(attribute_block(device, fs, &result) == block &&
	    fs->info.free_blocks == free_blocks && result.blocks_512 == inode.blocks_512 &&
	    references(device, block) == 2 &&
	    memcmp(before, device->cache + block * device->block_size, device->block_size) == 0);
	if (fs->inode_size != EXT4_INODE_BASE_SIZE) {
		batch[0] = change(EXT4_XATTR_CREATE, EXT4_XATTR_USER, "tiny", "abc", 3);
		EXPECT(ext4_set_attributes(fs, inode.number, inode.generation, &update, &result),
		    EXT4_OK);
		CHECK(attribute_block(device, fs, &result) == block &&
		    references(device, block) == 2 && fs->info.free_blocks == free_blocks &&
		    memcmp(before, device->cache + block * device->block_size,
			device->block_size) == 0);
		batch[0] = change(EXT4_XATTR_REMOVE_IF_PRESENT, EXT4_XATTR_USER, "tiny", NULL, 0);
		EXPECT(ext4_set_attributes(fs, inode.number, inode.generation, &update, &result),
		    EXT4_OK);
		EXPECT(ext4_set_attributes(fs, inode.number, inode.generation, &update, &result),
		    EXT4_OK);
		CHECK(memcmp(before, device->cache + block * device->block_size,
			  device->block_size) == 0);
	}
	batch[0] = change(EXT4_XATTR_REMOVE, EXT4_XATTR_USER, "binary", NULL, 0);
	EXPECT(ext4_set_attributes(fs, inode.number, inode.generation, &update, &result), EXT4_OK);
	CHECK(result.blocks_512 == 0 && fs->info.free_blocks == free_blocks &&
	    references(device, block) == 1);
	value_is(fs, &shared, EXT4_XATTR_USER, "binary", original, sizeof(original));
	EXPECT(
	    ext4_set_attributes(fs, shared.number, shared.generation, &update, &result), EXT4_OK);
	CHECK(result.blocks_512 == 0 && fs->info.free_blocks == free_blocks + 1);
	EXPECT(ext4_sync(fs), EXT4_OK);
	ext4_unmount(fs);
	storage_export(device, exports, path, "xattr-references-");
	free(before);
	puts("PASS xattr references: identical updates, conditional removal, inode-body changes, "
	     "shared detach and "
	     "final block release");
}

#include "xattr_write_edges.h"

int
main(int argc, char **argv)
{
	struct device device;
	const char *exports = NULL;
	bool smoke = false;
	bool edges = false;
	bool full = false;
	enum operation operation;
	int argument = 1;

	CHECK(argc >= 2);
	while (argument < argc && argv[argument][0] == '-') {
		if (strcmp(argv[argument], "--smoke") == 0) {
			smoke = true;
			argument++;
		} else if (strcmp(argv[argument], "--edges") == 0) {
			edges = true;
			argument++;
		} else if (strcmp(argv[argument], "--full") == 0) {
			full = true;
			argument++;
		} else {
			CHECK(strcmp(argv[argument], "--export") == 0 && argument + 1 < argc);
			exports = argv[argument + 1];
			argument += 2;
		}
	}
	CHECK(argument < argc && !(edges && full));
	for (; argument < argc; argument++) {
		storage_open(&device, argv[argument]);
		if (full || edges) {
			if (full) {
				full_space(&device, exports, argv[argument]);
			} else {
				capacity_cases(&device, exports, argv[argument]);
				packing_case(&device, exports, argv[argument]);
				ownership_cases(&device);
			}
			storage_close(&device);
			printf("PASS xattr %s: %s\n", full ? "full space" : "packing and ownership",
			    argv[argument]);
			continue;
		}
		rejected(&device);
		storage_transitions(&device, exports, argv[argument]);
		for (operation = CREATE_ATTRIBUTES; operation < OPERATION_COUNT; operation++) {
			faults(&device, operation, smoke, exports, argv[argument]);
		}
		storage_close(&device);
		printf("PASS xattr transactional mutations: %s; full-fault-sweeps=%s\n",
		    argv[argument], smoke ? "no" : "yes");
	}
	return 0;
}

/* SPDX-License-Identifier: BSD-3-Clause */
#include "storage.h"
#include "directory_index.h"

#include <inttypes.h>

/* These fixtures have at most one internal HTree level and shallow file maps.
 * Leave room for mapping and inode reads, but reject entry-by-entry lookup. */
#define LOOKUP_CALLBACK_LIMIT 32U

struct query {
	struct ext4_inode directory;
	uint32_t number;
	size_t length;
	uint8_t name[EXT4_NAME_MAX + 1];
};

struct lookup_path {
	uint8_t *root;
	uint8_t *node;
	uint8_t *leaf;
	uint8_t *next_leaf;
	uint32_t node_logical;
	uint32_t leaf_logical;
	struct query query;
};

enum lookup_damage {
	LOOKUP_DOT,
	LOOKUP_PARENT,
	LOOKUP_RESERVED,
	LOOKUP_INFO_LENGTH,
	LOOKUP_VERSION,
	LOOKUP_LEVEL,
	LOOKUP_FLAGS,
	LOOKUP_ROOT_COUNT,
	LOOKUP_ROOT_OVERFLOW,
	LOOKUP_ROOT_LIMIT,
	LOOKUP_ROOT_ZERO,
	LOOKUP_ROOT_EOF,
	LOOKUP_ROOT_ALIAS,
	LOOKUP_ROOT_ORDER,
	LOOKUP_ROOT_EQUAL,
	LOOKUP_ROOT_CHECKSUM,
	LOOKUP_TAIL_RESERVED,
	LOOKUP_LEAF_SHORT,
	LOOKUP_LEAF_OVERFLOW,
	LOOKUP_LEAF_EMPTY_NAME,
	LOOKUP_LEAF_NUL,
	LOOKUP_LEAF_SLASH,
	LOOKUP_LEAF_TYPE,
	LOOKUP_LEAF_INODE,
	LOOKUP_LEAF_LATE_RECORD,
	LOOKUP_LEAF_CHECKSUM,
	LOOKUP_LEAF_DOT,
	LOOKUP_LEAF_DUPLICATE,
	LOOKUP_LEAF_RANGE,
	LOOKUP_NODE_INODE,
	LOOKUP_NODE_LENGTH,
	LOOKUP_NODE_COUNT,
	LOOKUP_NODE_SELF,
	LOOKUP_NODE_ROOT,
	LOOKUP_NODE_AS_LEAF,
	LOOKUP_NODE_ALIAS,
	LOOKUP_NODE_RANGE,
	LOOKUP_NODE_CHECKSUM,
	LOOKUP_DAMAGE_COUNT
};

static uint8_t
hex_digit(char value)
{
	if (value >= '0' && value <= '9') {
		return (uint8_t)(value - '0');
	}
	CHECK(value >= 'a' && value <= 'f');
	return (uint8_t)(value - 'a' + 10);
}

static void
reset_calls(struct device *device)
{
	device->reads = 0;
	device->allocations = 0;
	device->fail_read = 0;
	device->fail_allocation = 0;
}

static void
check_lookup(
    struct device *device, struct ext4_fs *fs, const struct query *query, enum ext4_result expected)
{
	struct ext4_inode result;
	struct ext4_inode before;
	uint32_t live = device->live;

	memset(&result, 0xa5, sizeof(result));
	memcpy(&before, &result, sizeof(before));
	EXPECT(ext4_lookup(fs, &query->directory, query->name, query->length, &result), expected);
	if (expected == EXT4_OK) {
		CHECK(result.number == query->number);
	} else {
		CHECK(memcmp(&before, &result, sizeof(before)) == 0);
	}
	CHECK(device->live == live && device->writes == 0 && device->events == 0 && !fs->aborted);
}

static void
lookup_faults(
    struct device *device, struct ext4_fs *fs, const struct query *query, enum ext4_result expected)
{
	uint32_t allocations;
	uint32_t reads;
	uint32_t fault;

	reset_calls(device);
	check_lookup(device, fs, query, expected);
	allocations = device->allocations;
	reads = device->reads;
	CHECK(allocations > 0 && reads > 0);
	for (fault = 1; fault <= allocations; fault++) {
		reset_calls(device);
		device->fail_allocation = fault;
		check_lookup(device, fs, query, EXT4_NO_MEMORY);
	}
	for (fault = 1; fault <= reads; fault++) {
		reset_calls(device);
		device->fail_read = fault;
		check_lookup(device, fs, query, EXT4_IO);
	}
	reset_calls(device);
	printf("PASS lookup faults parent=%" PRIu32 " result=%s allocations=%" PRIu32
	       " reads=%" PRIu32 "\n",
	    query->directory.number, ext4_result_string(expected), allocations, reads);
}

static uint32_t
independent_names(struct device *device, struct ext4_fs *fs, const char *expected)
{
	struct query query = { 0 };
	struct query ordinary = { 0 };
	struct query indexed = { 0 };
	FILE *stream;
	char encoded[EXT4_NAME_MAX * 2U + 1U];
	uint32_t parent;
	uint32_t number;
	uint32_t checked = 0;
	size_t length;
	size_t index;
	int fields;

	_Static_assert(EXT4_NAME_MAX * 2U == 510U, "lookup expectation scan width");
	stream = fopen(expected, "r");
	CHECK(stream != NULL);
	while ((fields = fscanf(stream, "%" SCNu32 " %510s %" SCNu32, &parent, encoded, &number)) !=
	    EOF) {
		CHECK(fields == 3 && parent != 0 && number != 0);
		if (parent != query.directory.number) {
			EXPECT(ext4_get_inode(fs, parent, &query.directory), EXT4_OK);
			CHECK((query.directory.mode & EXT4_MODE_TYPE) == EXT4_MODE_DIRECTORY);
		}
		length = strlen(encoded);
		CHECK(length > 0 && length % 2U == 0 && length <= EXT4_NAME_MAX * 2U);
		query.length = length / 2U;
		query.number = number;
		for (index = 0; index < query.length; index++) {
			query.name[index] = (uint8_t)((hex_digit(encoded[index * 2U]) << 4) |
			    hex_digit(encoded[index * 2U + 1U]));
			CHECK(query.name[index] != 0 && query.name[index] != '/');
		}
		query.name[query.length] = 0;
		reset_calls(device);
		check_lookup(device, fs, &query, EXT4_OK);
		if (query.directory.flags & EXT4_INODE_INDEX) {
			CHECK(device->reads <= LOOKUP_CALLBACK_LIMIT &&
			    device->allocations <= LOOKUP_CALLBACK_LIMIT);
			if (query.name[0] != '.') {
				indexed = query;
			}
		} else if (query.name[0] != '.') {
			ordinary = query;
		}
		checked++;
	}
	CHECK(!ferror(stream) && fclose(stream) == 0 && checked > 0 && ordinary.length > 0 &&
	    indexed.length > 0);
	lookup_faults(device, fs, &ordinary, EXT4_OK);
	lookup_faults(device, fs, &indexed, EXT4_OK);
	memcpy(ordinary.name, "lookup-absent", sizeof("lookup-absent"));
	ordinary.length = sizeof("lookup-absent") - 1U;
	lookup_faults(device, fs, &ordinary, EXT4_NOT_FOUND);
	memcpy(indexed.name, ordinary.name, ordinary.length + 1U);
	indexed.length = ordinary.length;
	reset_calls(device);
	check_lookup(device, fs, &indexed, EXT4_NOT_FOUND);
	CHECK(
	    device->reads <= LOOKUP_CALLBACK_LIMIT && device->allocations <= LOOKUP_CALLBACK_LIMIT);
	lookup_faults(device, fs, &indexed, EXT4_NOT_FOUND);
	indexed.name[0] = '.';
	indexed.length = 1;
	indexed.number = indexed.directory.number;
	lookup_faults(device, fs, &indexed, EXT4_OK);
	indexed.name[1] = '.';
	indexed.length = 2;
	indexed.number = EXT4_ROOT_INODE;
	lookup_faults(device, fs, &indexed, EXT4_OK);
	return checked;
}

static uint8_t *
mapped_block(
    struct device *device, struct ext4_fs *fs, const struct ext4_inode *directory, uint32_t logical)
{
	uint64_t physical;

	EXPECT(ext4_map_block(fs, directory, logical, &physical), EXT4_OK);
	CHECK(physical != 0 && physical < device->blocks);
	return device->cache + physical * device->block_size;
}

static struct lookup_path
first_path(struct device *device, struct ext4_fs *fs)
{
	struct lookup_path path = { 0 };
	struct ext4_inode root_inode;
	struct ext4_dx_root_prefix_disk *root;
	struct ext4_dx_entry_disk *entries;
	struct ext4_dx_count_disk *counts;
	struct ext4_dir_entry entry;
	uint32_t record;

	EXPECT(ext4_get_inode(fs, EXT4_ROOT_INODE, &root_inode), EXT4_OK);
	EXPECT(ext4_lookup(fs, &root_inode, (const uint8_t *)"indexed", sizeof("indexed") - 1U,
		   &path.query.directory),
	    EXT4_OK);
	path.root = mapped_block(device, fs, &path.query.directory, 0);
	root = (struct ext4_dx_root_prefix_disk *)path.root;
	counts = (struct ext4_dx_count_disk *)(path.root + sizeof(*root));
	entries = (struct ext4_dx_entry_disk *)counts;
	CHECK(ext4_le16(&counts->count) >= 2);
	if (root->indirect_levels != 0) {
		CHECK(root->indirect_levels == 1);
		path.node_logical = ext4_le32(&entries[0].block);
		path.node = mapped_block(device, fs, &path.query.directory, path.node_logical);
		counts =
		    (struct ext4_dx_count_disk *)(path.node + sizeof(struct ext4_dir_header_disk));
		entries = (struct ext4_dx_entry_disk *)counts;
		CHECK(ext4_le16(&counts->count) >= 2);
	}
	path.leaf_logical = ext4_le32(&entries[0].block);
	path.leaf = mapped_block(device, fs, &path.query.directory, path.leaf_logical);
	path.next_leaf =
	    mapped_block(device, fs, &path.query.directory, ext4_le32(&entries[1].block));
	EXPECT(ext4_directory_entry_decode(fs, path.leaf, 0, &entry, &record), EXT4_OK);
	CHECK(entry.inode != 0 && entry.name_length > 2 && record < fs->info.block_size);
	path.query.number = entry.inode;
	path.query.length = entry.name_length;
	memcpy(path.query.name, entry.name, entry.name_length + 1U);
	check_lookup(device, fs, &path.query, EXT4_OK);
	return path;
}

static void
leaf_checksum(struct ext4_fs *fs, const struct ext4_inode *inode, uint8_t *buffer)
{
	struct ext4_dir_tail_disk *tail;

	if (fs->metadata_checksum) {
		tail = (struct ext4_dir_tail_disk *)(buffer + fs->info.block_size - sizeof(*tail));
		ext4_encode32(&tail->checksum,
		    ext4_crc32c(
			ext4_inode_seed(fs, inode), buffer, fs->info.block_size - sizeof(*tail)));
	}
}

static void
restore_block(struct device *device, uint8_t *buffer)
{
	if (buffer != NULL) {
		memcpy(buffer, device->base + (buffer - device->cache), device->block_size);
	}
}

static void
header_preserves_input(
    struct ext4_fs *fs, const struct ext4_inode *inode, uint32_t logical, uint8_t *buffer)
{
	struct ext4_index_metadata result;
	struct ext4_index_metadata before;
	uint8_t *copy = malloc(fs->info.block_size);
	enum ext4_result error;

	CHECK(copy != NULL);
	memcpy(copy, buffer, fs->info.block_size);
	memset(&result, 0xa5, sizeof(result));
	memcpy(&before, &result, sizeof(before));
	error = ext4_index_decode(fs, inode, logical, buffer, &result);
	CHECK(memcmp(copy, buffer, fs->info.block_size) == 0);
	if (error != EXT4_OK) {
		CHECK(memcmp(&before, &result, sizeof(before)) == 0);
	}
	free(copy);
}

static void
lookup_damage(struct device *device, struct ext4_fs *fs)
{
	struct lookup_path path = first_path(device, fs);
	struct ext4_dx_root_prefix_disk *root = (struct ext4_dx_root_prefix_disk *)path.root;
	struct ext4_dx_entry_disk *root_entries =
	    (struct ext4_dx_entry_disk *)(path.root + sizeof(*root));
	struct ext4_dx_entry_disk *entries;
	struct ext4_dx_count_disk *counts;
	struct ext4_dir_header_disk *header;
	struct ext4_dir_header_disk *second;
	struct ext4_dx_tail_disk *tail;
	uint8_t *buffer;
	uint32_t base;
	uint32_t logical;
	uint32_t record;
	uint32_t cases = 0;
	uint32_t skips = 0;
	bool checksum;
	bool leaf;
	enum ext4_result expected;
	enum lookup_damage damage;

	for (damage = 0; damage < LOOKUP_DAMAGE_COUNT; damage++) {
		if (damage >= LOOKUP_NODE_INODE && path.node == NULL) {
			skips++;
			continue;
		}
		if (!fs->metadata_checksum &&
		    (damage == LOOKUP_ROOT_CHECKSUM || damage == LOOKUP_LEAF_CHECKSUM ||
			damage == LOOKUP_NODE_CHECKSUM || damage == LOOKUP_TAIL_RESERVED)) {
			skips++;
			continue;
		}
		buffer = damage >= LOOKUP_NODE_INODE ? path.node : path.root;
		logical = damage >= LOOKUP_NODE_INODE ? path.node_logical : 0;
		base = logical == 0 ? sizeof(*root) : sizeof(*header);
		counts = (struct ext4_dx_count_disk *)(buffer + base);
		entries = (struct ext4_dx_entry_disk *)counts;
		leaf = damage >= LOOKUP_LEAF_SHORT && damage <= LOOKUP_LEAF_RANGE;
		header = (struct ext4_dir_header_disk *)(leaf ? path.leaf : buffer);
		record = ext4_directory_record_length(fs, (struct ext4_dir_header_disk *)path.leaf);
		second = (struct ext4_dir_header_disk *)(path.leaf + record);
		checksum = true;
		expected = EXT4_CORRUPT;
		switch (damage) {
		case LOOKUP_DOT:
			ext4_encode32(&root->dot.inode, 0);
			break;
		case LOOKUP_PARENT:
			ext4_encode32(&root->dotdot.inode, 0);
			break;
		case LOOKUP_RESERVED:
			ext4_encode32(&root->reserved, 1);
			break;
		case LOOKUP_INFO_LENGTH:
			root->info_length++;
			break;
		case LOOKUP_VERSION:
			root->hash_version = EXT4_HASH_TEA_UNSIGNED + 1U;
			expected = EXT4_UNSUPPORTED;
			break;
		case LOOKUP_LEVEL:
			root->indirect_levels = EXT4_DX_MAX_INDIRECT_LEVELS + 1U;
			expected = EXT4_UNSUPPORTED;
			break;
		case LOOKUP_FLAGS:
			root->flags = 1;
			expected = EXT4_UNSUPPORTED;
			break;
		case LOOKUP_ROOT_COUNT:
		case LOOKUP_NODE_COUNT:
			ext4_encode16(&counts->count, 0);
			break;
		case LOOKUP_ROOT_OVERFLOW:
			ext4_encode16(&counts->count, ext4_le16(&counts->limit) + 1U);
			checksum = false;
			break;
		case LOOKUP_ROOT_LIMIT:
			ext4_encode16(&counts->limit, ext4_le16(&counts->limit) - 1U);
			break;
		case LOOKUP_ROOT_ZERO:
		case LOOKUP_NODE_ROOT:
			ext4_encode32(&entries[0].block, 0);
			break;
		case LOOKUP_ROOT_EOF:
			ext4_encode32(&entries[0].block,
			    (uint32_t)(path.query.directory.size / fs->info.block_size));
			break;
		case LOOKUP_ROOT_ALIAS:
		case LOOKUP_NODE_ALIAS:
			entries[1].block = entries[0].block;
			break;
		case LOOKUP_ROOT_ORDER:
			ext4_encode32(&entries[ext4_le16(&counts->count) - 1U].hash, EXT4_HASH_EOF);
			break;
		case LOOKUP_ROOT_EQUAL:
			ext4_encode32(&entries[1].hash, 0);
			break;
		case LOOKUP_ROOT_CHECKSUM:
			root->dot_name[sizeof(root->dot_name) - 1U] ^= 1U;
			checksum = false;
			break;
		case LOOKUP_TAIL_RESERVED:
			tail = (struct ext4_dx_tail_disk *)(buffer + base +
			    ext4_le16(&counts->limit) * sizeof(*counts));
			ext4_encode32(&tail->reserved, 1);
			expected = EXT4_OK;
			break;
		case LOOKUP_LEAF_SHORT:
			ext4_encode16(
			    &header->record_length, sizeof(*header) - EXT4_DIRECTORY_ALIGNMENT);
			break;
		case LOOKUP_LEAF_OVERFLOW:
			ext4_encode16(&header->record_length, UINT16_MAX);
			break;
		case LOOKUP_LEAF_EMPTY_NAME:
			header->name_length = 0;
			break;
		case LOOKUP_LEAF_NUL:
			path.leaf[sizeof(*header)] = 0;
			break;
		case LOOKUP_LEAF_SLASH:
			path.leaf[sizeof(*header)] = '/';
			break;
		case LOOKUP_LEAF_TYPE:
			header->type = EXT4_FT_SYMLINK + 1U;
			break;
		case LOOKUP_LEAF_INODE:
			ext4_encode32(&header->inode, fs->info.inodes + 1U);
			break;
		case LOOKUP_LEAF_LATE_RECORD:
			ext4_encode16(&second->record_length, 0);
			break;
		case LOOKUP_LEAF_CHECKSUM:
			path.leaf[sizeof(*header)] ^= 1U;
			checksum = false;
			break;
		case LOOKUP_LEAF_DOT:
			header->name_length = 1;
			path.leaf[sizeof(*header)] = '.';
			break;
		case LOOKUP_LEAF_DUPLICATE:
			CHECK(ext4_directory_record_length(fs, second) >=
			    sizeof(*second) + path.query.length);
			second->inode = header->inode;
			second->name_length = header->name_length;
			memcpy((uint8_t *)second + sizeof(*second), path.query.name,
			    path.query.length);
			break;
		case LOOKUP_LEAF_RANGE:
			memcpy(path.leaf, path.next_leaf, fs->info.block_size);
			break;
		case LOOKUP_NODE_INODE:
			ext4_encode32(&header->inode, path.query.directory.number);
			break;
		case LOOKUP_NODE_LENGTH:
			ext4_encode16(&header->record_length, sizeof(*header));
			break;
		case LOOKUP_NODE_SELF:
			ext4_encode32(&entries[0].block, path.node_logical);
			break;
		case LOOKUP_NODE_AS_LEAF:
			entries[0].block = root_entries[1].block;
			break;
		case LOOKUP_NODE_RANGE:
			entries[1].hash = root_entries[1].hash;
			break;
		case LOOKUP_NODE_CHECKSUM:
			tail = (struct ext4_dx_tail_disk *)(buffer + base +
			    ext4_le16(&counts->limit) * sizeof(*counts));
			ext4_encode32(&tail->checksum, ext4_le32(&tail->checksum) ^ 1U);
			checksum = false;
			break;
		case LOOKUP_DAMAGE_COUNT:
			CHECK(false);
		}
		if (checksum) {
			if (leaf) {
				leaf_checksum(fs, &path.query.directory, path.leaf);
			} else {
				ext4_index_checksum_set(fs, &path.query.directory, logical, buffer);
			}
		}
		printf("CHECK lookup damage=%u\n", damage);
		if (!leaf) {
			header_preserves_input(fs, &path.query.directory, logical, buffer);
		}
		check_lookup(device, fs, &path.query, expected);
		restore_block(device, path.root);
		restore_block(device, path.node);
		restore_block(device, path.leaf);
		cases++;
	}
	check_lookup(device, fs, &path.query, EXT4_OK);
	printf("PASS lookup structure cases=%" PRIu32 " skips=%" PRIu32
	       " (absent index level or checksum)\n",
	    cases, skips);
}

static void
hash_signedness(struct device *device)
{
	struct ext4_fs *fs;
	struct lookup_path path;
	struct ext4_dx_root_prefix_disk *root;
	struct ext4_super_disk *super;
	uint32_t flags;
	unsigned int mode;
	unsigned int cases = 0;

	for (mode = 0; mode < 3; mode++) {
		device_reset(device, device->base);
		EXPECT(ext4_mount(&device->environment, &fs), EXT4_OK);
		path = first_path(device, fs);
		super = (struct ext4_super_disk *)(device->cache + EXT4_SUPER_OFFSET);
		flags = ext4_le32(&super->flags);
		if (mode == 2 && !(flags & EXT4_UNSIGNED_DIRECTORY_HASH)) {
			ext4_unmount(fs);
			continue;
		}
		if (mode == 2) {
			root = (struct ext4_dx_root_prefix_disk *)path.root;
			CHECK(root->hash_version <= EXT4_HASH_TEA);
			root->hash_version += EXT4_HASH_LEGACY_UNSIGNED;
			ext4_index_checksum_set(fs, &path.query.directory, 0, path.root);
		}
		flags &= ~(EXT4_SIGNED_DIRECTORY_HASH | EXT4_UNSIGNED_DIRECTORY_HASH);
		if (mode == 1) {
			flags |= EXT4_SIGNED_DIRECTORY_HASH | EXT4_UNSIGNED_DIRECTORY_HASH;
		}
		ext4_encode32(&super->flags, flags);
		if (fs->metadata_checksum) {
			ext4_encode32(&super->checksum,
			    ext4_crc32c(
				UINT32_MAX, super, offsetof(struct ext4_super_disk, checksum)));
		}
		ext4_unmount(fs);
		EXPECT(ext4_mount(&device->environment, &fs), EXT4_OK);
		lookup_faults(device, fs, &path.query, mode == 1 ? EXT4_CORRUPT : EXT4_OK);
		if (mode != 1) {
			memcpy(path.query.name, "lookup-absent", sizeof("lookup-absent"));
			path.query.length = sizeof("lookup-absent") - 1U;
			reset_calls(device);
			check_lookup(device, fs, &path.query, EXT4_NOT_FOUND);
			if (mode == 2) {
				CHECK(device->reads <= LOOKUP_CALLBACK_LIMIT &&
				    device->allocations <= LOOKUP_CALLBACK_LIMIT);
			}
		}
		ext4_unmount(fs);
		cases++;
	}
	device_reset(device, device->base);
	printf(
	    "PASS lookup hash signedness cases=%u explicit_unsigned_skips=%u\n", cases, 3U - cases);
}

static void
lookup_arguments(struct device *device, struct ext4_fs *fs)
{
	struct ext4_inode directory;
	struct ext4_inode result;
	struct ext4_inode before;
	const uint8_t name[] = "name";
	const uint8_t zero[] = { 'a', 0 };
	uint32_t live = device->live;

	EXPECT(ext4_get_inode(fs, EXT4_ROOT_INODE, &directory), EXT4_OK);
	memset(&result, 0xa5, sizeof(result));
	memcpy(&before, &result, sizeof(before));
	reset_calls(device);
	EXPECT(
	    ext4_lookup(NULL, &directory, name, sizeof(name) - 1U, &result), EXT4_INVALID_ARGUMENT);
	EXPECT(ext4_lookup(fs, NULL, name, sizeof(name) - 1U, &result), EXT4_INVALID_ARGUMENT);
	EXPECT(ext4_lookup(fs, &directory, NULL, 1, &result), EXT4_INVALID_ARGUMENT);
	EXPECT(ext4_lookup(fs, &directory, name, 0, &result), EXT4_INVALID_ARGUMENT);
	EXPECT(ext4_lookup(fs, &directory, name, sizeof(name) - 1U, NULL), EXT4_INVALID_ARGUMENT);
	EXPECT(ext4_lookup(fs, &directory, name, EXT4_NAME_MAX + 1U, &result), EXT4_NAME_TOO_LONG);
	EXPECT(ext4_lookup(fs, &directory, zero, sizeof(zero), &result), EXT4_INVALID_ARGUMENT);
	EXPECT(
	    ext4_lookup(fs, &directory, (const uint8_t *)"a/b", 3, &result), EXT4_INVALID_ARGUMENT);
	directory.size++;
	EXPECT(ext4_lookup(fs, &directory, name, sizeof(name) - 1U, &result), EXT4_CORRUPT);
	directory.size--;
	directory.mode = EXT4_MODE_REGULAR;
	EXPECT(ext4_lookup(fs, &directory, name, sizeof(name) - 1U, &result), EXT4_NOT_DIRECTORY);
	fs->aborted = true;
	EXPECT(
	    ext4_lookup(fs, &directory, name, sizeof(name) - 1U, &result), EXT4_RECOVERY_REQUIRED);
	fs->aborted = false;
	CHECK(memcmp(&before, &result, sizeof(before)) == 0 && device->allocations == 0 &&
	    device->reads == 0 && device->live == live && device->writes == 0 &&
	    device->events == 0);
	puts("PASS lookup arguments preserve output without I/O");
}

#include "directory_iteration.h"

static void
lookup_image(const char *image, const char *expected)
{
	struct device device;
	struct ext4_fs *fs;
	uint32_t names;
	unsigned int writable;

	storage_open(&device, image);
	for (writable = 0; writable < 2; writable++) {
		device_reset(&device, device.base);
		EXPECT(writable ? ext4_mount_writable(&device.environment, &device.writer, &fs)
				: ext4_mount(&device.environment, &fs),
		    EXT4_OK);
		names = independent_names(&device, fs, expected);
		independent_iteration(&device, fs, expected);
		lookup_arguments(&device, fs);
		lookup_damage(&device, fs);
		ext4_unmount(fs);
		CHECK(device.live == 0 && device.writes == 0 && device.events == 0 &&
		    memcmp(device.base, device.cache, device.size) == 0 &&
		    memcmp(device.base, device.stable, device.size) == 0);
		printf("PASS lookup independent names=%" PRIu32 " writable_owner=%u %s\n", names,
		    writable, image);
	}
	hash_signedness(&device);
	iteration_mutation(&device);
	storage_close(&device);
}

int
main(int argc, char **argv)
{
	if (argc != 3) {
		fprintf(stderr, "usage: %s IMAGE EXPECTED_LOOKUPS\n", argv[0]);
		return 2;
	}
	lookup_image(argv[1], argv[2]);
	return 0;
}

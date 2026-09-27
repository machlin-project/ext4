/* SPDX-License-Identifier: BSD-3-Clause */
#include "storage.h"

#define XATTR_TEST_CASES 128U
#define XATTR_TEST_PATH_BYTES 128U
#define XATTR_TEST_PAYLOAD_BYTES 128U
#define XATTR_TEST_BINARY_BYTES 600U
#define XATTR_TEST_SHARED_VALUE_BYTES 32U
#define XATTR_TEST_UNKNOWN_NAMESPACE 200U

struct xattr_case {
	char path[XATTR_TEST_PATH_BYTES];
	int name_index;
	uint8_t name[EXT4_NAME_MAX];
	size_t name_length;
	uint8_t *value;
	size_t value_size;
};

static void
sentinel(const void *buffer, size_t size)
{
	const uint8_t *bytes = buffer;
	size_t index;

	for (index = 0; index < size; index++) {
		CHECK(bytes[index] == 0xa5);
	}
}

static size_t
load_cases(const char *path, struct xattr_case *cases)
{
	FILE *input = fopen(path, "r");
	FILE *payload;
	const char *slash = strrchr(path, '/');
	char hex[EXT4_NAME_MAX * 2 + 1];
	char name[XATTR_TEST_PAYLOAD_BYTES];
	char full[4096];
	size_t count = 0;
	size_t index;
	unsigned int byte;
	long size;
	int fields;
	int length;

	CHECK(input != NULL);
	for (;;) {
		CHECK(count < XATTR_TEST_CASES);
		fields = fscanf(input, "%127s %d %510s %127s", cases[count].path,
		    &cases[count].name_index, hex, name);
		if (fields == EOF) {
			break;
		}
		CHECK(fields == 4);
		cases[count].name_length = strcmp(hex, "-") == 0 ? 0 : strlen(hex) / 2;
		CHECK(strcmp(hex, "-") == 0 || strlen(hex) % 2 == 0);
		for (index = 0; index < cases[count].name_length; index++) {
			CHECK(sscanf(hex + index * 2, "%2x", &byte) == 1);
			cases[count].name[index] = (uint8_t)byte;
		}
		if (cases[count].name_index >= 0) {
			CHECK(cases[count].name_index <= UINT8_MAX);
			length = snprintf(full, sizeof(full), "%.*s%s",
			    slash == NULL ? 0 : (int)(slash - path + 1), path, name);
			CHECK(length > 0 && (size_t)length < sizeof(full));
			payload = fopen(full, "rb");
			CHECK(payload != NULL && fseek(payload, 0, SEEK_END) == 0);
			size = ftell(payload);
			CHECK(size >= 0 && size <= EXT4_MAX_BLOCK_SIZE &&
			    fseek(payload, 0, SEEK_SET) == 0);
			cases[count].value_size = (size_t)size;
			cases[count].value = malloc((size_t)size + 1);
			CHECK(cases[count].value != NULL);
			CHECK(fread(cases[count].value, 1, (size_t)size, payload) == (size_t)size);
			CHECK(fclose(payload) == 0);
		}
		count++;
	}
	CHECK(fclose(input) == 0 && count != 0);
	return count;
}

static struct ext4_inode
lookup(struct ext4_fs *fs, const char *name)
{
	struct ext4_inode parent;
	struct ext4_inode result;
	const char *component;
	const char *end;
	size_t length;

	EXPECT(ext4_get_inode(fs, EXT4_ROOT_INODE, &parent), EXT4_OK);
	CHECK(name[0] == '/');
	component = name + 1;
	while (*component != 0) {
		end = strchr(component, '/');
		length = end == NULL ? strlen(component) : (size_t)(end - component);
		CHECK(length != 0);
		EXPECT(
		    ext4_lookup(fs, &parent, (const uint8_t *)component, length, &result), EXT4_OK);
		parent = result;
		component += length + (end != NULL);
	}
	return parent;
}

static void
functional(struct device *device, struct ext4_fs *fs, const struct xattr_case *cases, size_t count)
{
	struct ext4_xattr_key *keys = malloc(XATTR_TEST_CASES * sizeof(*keys));
	struct ext4_inode inode;
	uint8_t *value;
	size_t index;
	size_t other;
	size_t returned;
	size_t expected;
	size_t entry;
	size_t found;

	CHECK(keys != NULL);
	for (index = 0; index < count; index++) {
		inode = lookup(fs, cases[index].path);
		if (cases[index].name_index >= 0) {
			returned = SIZE_MAX;
			EXPECT(ext4_get_xattr(fs, inode.number, inode.generation,
				   (uint8_t)cases[index].name_index, cases[index].name,
				   cases[index].name_length, NULL, 0, &returned),
			    EXT4_OK);
			CHECK(returned == cases[index].value_size);
			value = malloc(returned + 16);
			CHECK(value != NULL);
			memset(value, 0xa5, returned + 16);
			EXPECT(ext4_get_xattr(fs, inode.number, inode.generation,
				   (uint8_t)cases[index].name_index, cases[index].name,
				   cases[index].name_length, value, returned, &returned),
			    EXT4_OK);
			CHECK(returned == cases[index].value_size &&
			    memcmp(value, cases[index].value, returned) == 0);
			sentinel(value + returned, 16);
			memset(value, 0xa5, returned + 16);
			if (cases[index].value_size != 0) {
				returned = SIZE_MAX;
				EXPECT(ext4_get_xattr(fs, inode.number, inode.generation,
					   (uint8_t)cases[index].name_index, cases[index].name,
					   cases[index].name_length, value,
					   cases[index].value_size - 1, &returned),
				    EXT4_RANGE);
				CHECK(returned == SIZE_MAX);
				sentinel(value, cases[index].value_size + 16);
			}
			free(value);
		}
		for (other = 0; other < index && strcmp(cases[other].path, cases[index].path) != 0;
		    other++) {
			/* Only the first occurrence enumerates this inode. */
		}
		if (other != index) {
			continue;
		}
		expected = 0;
		for (other = 0; other < count; other++) {
			if (strcmp(cases[other].path, cases[index].path) == 0 &&
			    cases[other].name_index >= 0) {
				expected++;
			}
		}
		returned = SIZE_MAX;
		EXPECT(ext4_list_xattrs(fs, inode.number, inode.generation, NULL, 0, &returned),
		    EXT4_OK);
		CHECK(returned == expected);
		memset(keys, 0xa5, XATTR_TEST_CASES * sizeof(*keys));
		if (expected != 0) {
			returned = SIZE_MAX;
			EXPECT(ext4_list_xattrs(fs, inode.number, inode.generation, keys,
				   expected - 1, &returned),
			    EXT4_RANGE);
			CHECK(returned == SIZE_MAX);
			sentinel(keys, XATTR_TEST_CASES * sizeof(*keys));
		}
		EXPECT(ext4_list_xattrs(
			   fs, inode.number, inode.generation, keys, XATTR_TEST_CASES, &returned),
		    EXT4_OK);
		CHECK(returned == expected);
		sentinel(keys + returned, (XATTR_TEST_CASES - returned) * sizeof(*keys));
		for (entry = 0; entry < returned; entry++) {
			found = 0;
			for (other = 0; other < count; other++) {
				if (strcmp(cases[other].path, cases[index].path) == 0 &&
				    cases[other].name_index == keys[entry].name_index &&
				    cases[other].name_length == keys[entry].name_length &&
				    memcmp(cases[other].name, keys[entry].name,
					keys[entry].name_length) == 0) {
					CHECK(cases[other].value_size == keys[entry].value_size);
					found++;
				}
			}
			CHECK(found == 1);
			if (entry != 0) {
				CHECK(keys[entry - 1].name_index < keys[entry].name_index ||
				    (keys[entry - 1].name_index == keys[entry].name_index &&
					(keys[entry - 1].name_length < keys[entry].name_length ||
					    (keys[entry - 1].name_length ==
						    keys[entry].name_length &&
						memcmp(keys[entry - 1].name, keys[entry].name,
						    keys[entry].name_length) < 0))));
			}
		}
		returned = SIZE_MAX;
		EXPECT(ext4_get_xattr(fs, inode.number, inode.generation, EXT4_XATTR_USER,
			   (const uint8_t *)"missing", 7, NULL, 0, &returned),
		    EXT4_NOT_FOUND);
		CHECK(returned == SIZE_MAX);
		EXPECT(
		    ext4_list_xattrs(fs, inode.number, inode.generation ^ 1U, NULL, 0, &returned),
		    EXT4_STALE);
		CHECK(returned == SIZE_MAX);
	}
	CHECK(device->writes == 0 && device->events == 0);
	free(keys);
}

static enum ext4_result
inspect(struct ext4_fs *fs, const struct ext4_inode *inode, bool list, void *buffer, size_t *size)
{

	return list
	    ? ext4_list_xattrs(fs, inode->number, inode->generation, buffer, XATTR_TEST_CASES, size)
	    : ext4_get_xattr(fs, inode->number, inode->generation, EXT4_XATTR_USER,
		  (const uint8_t *)"binary", 6, buffer, XATTR_TEST_BINARY_BYTES, size);
}

static void
faults(struct device *device, struct ext4_fs *fs)
{
	struct ext4_inode inode = lookup(fs, "/many");
	size_t bytes = XATTR_TEST_CASES * sizeof(struct ext4_xattr_key);
	uint8_t *output = malloc(bytes);
	size_t size;
	uint32_t allocations;
	uint32_t reads;
	uint32_t index;
	uint32_t live = device->live;
	unsigned int list;

	CHECK(output != NULL);
	for (list = 0; list < 2; list++) {
		device->allocations = device->reads = 0;
		EXPECT(inspect(fs, &inode, list != 0, output, &size), EXT4_OK);
		allocations = device->allocations;
		reads = device->reads;
		CHECK(allocations != 0 && reads != 0 && device->live == live);
		for (index = 1; index <= allocations; index++) {
			device->allocations = 0;
			device->fail_allocation = index;
			size = SIZE_MAX;
			memset(output, 0xa5, bytes);
			EXPECT(inspect(fs, &inode, list != 0, output, &size), EXT4_NO_MEMORY);
			CHECK(size == SIZE_MAX && device->live == live && !fs->aborted);
			sentinel(output, bytes);
		}
		device->fail_allocation = 0;
		for (index = 1; index <= reads; index++) {
			device->reads = 0;
			device->fail_read = index;
			size = SIZE_MAX;
			memset(output, 0xa5, bytes);
			EXPECT(inspect(fs, &inode, list != 0, output, &size), EXT4_IO);
			CHECK(size == SIZE_MAX && device->live == live && !fs->aborted);
			sentinel(output, bytes);
		}
		device->fail_read = 0;
		EXPECT(inspect(fs, &inode, list != 0, output, &size), EXT4_OK);
		printf("PASS xattr %s faults: allocations=%u reads=%u; outputs and lifetime "
		       "preserved\n",
		    list ? "list" : "get", allocations, reads);
	}
	fs->aborted = true;
	EXPECT(inspect(fs, &inode, false, output, &size), EXT4_RECOVERY_REQUIRED);
	EXPECT(inspect(fs, &inode, true, output, &size), EXT4_RECOVERY_REQUIRED);
	fs->aborted = false;
	CHECK(device->writes == 0 && device->events == 0);
	free(output);
}

static void
block_checksum(struct ext4_fs *fs, uint64_t block, struct ext4_xattr_header_disk *header)
{
	struct ext4_block_number_disk address;
	uint32_t checksum;

	if (!fs->metadata_checksum) {
		return;
	}
	ext4_encode32(&address.low, (uint32_t)block);
	ext4_encode32(&address.high, (uint32_t)(block >> 32));
	ext4_encode32(&header->checksum, 0);
	checksum = ext4_crc32c(fs->checksum_seed, &address, sizeof(address));
	checksum = ext4_crc32c(checksum, header, fs->info.block_size);
	ext4_encode32(&header->checksum, checksum);
}

enum xattr_damage {
	XATTR_BAD_MAGIC,
	XATTR_ZERO_REFERENCES,
	XATTR_EXCESS_REFERENCES,
	XATTR_MULTIPLE_BLOCKS,
	XATTR_RESERVED_WORD,
	XATTR_ENTRY_HASH,
	XATTR_BLOCK_HASH,
	XATTR_VALUE_INODE,
	XATTR_VALUE_OVERLAPS_TABLE,
	XATTR_VALUE_UNALIGNED,
	XATTR_VALUE_END,
	XATTR_VALUE_SIZE,
	XATTR_NAME_NUL,
	XATTR_DUPLICATE_KEY,
	XATTR_BLOCK_RANGE,
	XATTR_BLOCK_HIGH_RANGE,
	XATTR_INODE_BLOCK_COUNT,
	XATTR_BAD_CHECKSUM,
	XATTR_DAMAGE_COUNT
};

static void
corruption(struct device *device, struct ext4_fs *fs)
{
	struct ext4_inode inode = lookup(fs, "/block");
	struct ext4_inode_disk *disk;
	struct ext4_xattr_header_disk *header;
	struct ext4_xattr_entry_disk *entry;
	uint8_t *output = malloc(XATTR_TEST_CASES * sizeof(struct ext4_xattr_key));
	uint64_t offset;
	uint64_t block;
	size_t length;
	size_t size;
	uint32_t live = device->live;
	unsigned int passed = 0;
	unsigned int skipped = 0;
	unsigned int list;
	enum xattr_damage damage;

	CHECK(output != NULL);
	EXPECT(ext4_inode_location(fs, inode.number, &offset), EXT4_OK);
	for (damage = XATTR_BAD_MAGIC; damage < XATTR_DAMAGE_COUNT; damage++) {
		if (damage == XATTR_BAD_CHECKSUM && !fs->metadata_checksum) {
			skipped++;
			continue;
		}
		memcpy(device->cache, device->base, device->size);
		disk = (struct ext4_inode_disk *)(device->cache + offset);
		block = ext4_le32(&disk->xattr_block_lo) |
		    ((uint64_t)ext4_le16(&disk->xattr_block_hi) << 32);
		CHECK(block != 0 && block < fs->info.blocks);
		header =
		    (struct ext4_xattr_header_disk *)(device->cache + block * fs->info.block_size);
		entry = (struct ext4_xattr_entry_disk *)(header + 1);
		length = (sizeof(*entry) + entry->name_length + EXT4_XATTR_ALIGNMENT - 1) &
		    ~(size_t)(EXT4_XATTR_ALIGNMENT - 1);
		ext4_encode32(&header->hash, 0);
		if (damage != XATTR_BLOCK_HASH) {
			ext4_encode32(&entry->hash, 0);
		}
		switch (damage) {
		case XATTR_BAD_MAGIC:
			ext4_encode32(&header->magic, 0);
			break;
		case XATTR_ZERO_REFERENCES:
			ext4_encode32(&header->references, 0);
			break;
		case XATTR_EXCESS_REFERENCES:
			ext4_encode32(&header->references, EXT4_XATTR_REFCOUNT_MAX + 1);
			break;
		case XATTR_MULTIPLE_BLOCKS:
			ext4_encode32(&header->blocks, 2);
			break;
		case XATTR_RESERVED_WORD:
			ext4_encode32(&header->reserved[2], 1);
			break;
		case XATTR_ENTRY_HASH:
			ext4_encode32(&entry->hash, 1);
			break;
		case XATTR_BLOCK_HASH:
			ext4_encode32(&header->hash, ext4_le32(&entry->hash) == 1 ? 2 : 1);
			break;
		case XATTR_VALUE_INODE:
			ext4_encode32(&entry->value_inode, inode.number);
			break;
		case XATTR_VALUE_OVERLAPS_TABLE:
			ext4_encode16(&entry->value_offset, sizeof(*header));
			ext4_encode32(&entry->value_size, sizeof(struct ext4_le32));
			break;
		case XATTR_VALUE_UNALIGNED:
			ext4_encode16(&entry->value_offset, ext4_le16(&entry->value_offset) + 1);
			ext4_encode32(&entry->value_size, sizeof(struct ext4_le32));
			break;
		case XATTR_VALUE_END:
			ext4_encode16(&entry->value_offset,
			    (uint16_t)(fs->info.block_size - sizeof(struct ext4_le32)));
			ext4_encode32(&entry->value_size, sizeof(struct ext4_le32) * 2);
			break;
		case XATTR_VALUE_SIZE:
			ext4_encode32(&entry->value_size, UINT32_MAX);
			break;
		case XATTR_NAME_NUL:
			((uint8_t *)(entry + 1))[0] = 0;
			break;
		case XATTR_DUPLICATE_KEY:
			memcpy((uint8_t *)entry + length, entry, length);
			memset((uint8_t *)entry + length * 2, 0, sizeof(struct ext4_le32));
			break;
		case XATTR_BLOCK_RANGE:
			ext4_encode32(&disk->xattr_block_lo, (uint32_t)fs->info.blocks);
			break;
		case XATTR_BLOCK_HIGH_RANGE:
			ext4_encode16(&disk->xattr_block_hi, 1);
			break;
		case XATTR_INODE_BLOCK_COUNT:
			ext4_encode32(&disk->blocks_lo, 0);
			break;
		case XATTR_BAD_CHECKSUM:
		case XATTR_DAMAGE_COUNT:
			break;
		}
		block_checksum(fs, block, header);
		if (damage == XATTR_BAD_CHECKSUM) {
			header->checksum.bytes[0] ^= 1;
		}
		ext4_inode_checksum_set(fs, inode.number, disk);
		for (list = 0; list < 2; list++) {
			size = SIZE_MAX;
			memset(output, 0xa5, XATTR_TEST_CASES * sizeof(struct ext4_xattr_key));
			EXPECT(inspect(fs, &inode, list != 0, output, &size), EXT4_CORRUPT);
			CHECK(size == SIZE_MAX && device->live == live && !fs->aborted);
			sentinel(output, XATTR_TEST_CASES * sizeof(struct ext4_xattr_key));
		}
		passed++;
	}
	memcpy(device->cache, device->base, device->size);
	free(output);
	printf("PASS xattr malformed records: cases=%u get/list; SKIP checksum-absent=%u\n", passed,
	    skipped);
}

#include "xattr_edges.h"

static void
return_linux_attributes(struct device *device, struct ext4_fs *fs, const char *output)
{
	uint8_t value[700];
	uint8_t small[13];
	struct ext4_inode inode;
	struct ext4_inode result;
	struct ext4_xattr_change changes[] = {
		{ EXT4_XATTR_REPLACE, EXT4_XATTR_USER, (const uint8_t *)"binary", 6, value,
		    sizeof(value) },
		{ EXT4_XATTR_REMOVE, EXT4_XATTR_USER, (const uint8_t *)"empty", 5, NULL, 0 },
		{ EXT4_XATTR_CREATE, EXT4_XATTR_USER, (const uint8_t *)"return", 6, small,
		    sizeof(small) },
		{ EXT4_XATTR_REMOVE, EXT4_XATTR_POSIX_ACL_ACCESS, NULL, 0, NULL, 0 },
		{ EXT4_XATTR_REMOVE, EXT4_XATTR_SECURITY, (const uint8_t *)"capability", 10, NULL,
		    0 },
	};
	struct ext4_inode_update update = {
		.fields = EXT4_ATTR_PERMISSIONS | EXT4_ATTR_UID | EXT4_ATTR_GID |
		    EXT4_ATTR_CHANGE_TIME | EXT4_ATTR_XATTRS,
		.uid = 54321,
		.gid = 65432,
		.permissions = 0600,
		.change_time = { .seconds = 1700000090 },
		.xattrs = changes,
		.xattr_count = sizeof(changes) / sizeof(changes[0]),
	};
	FILE *stream;
	size_t index;

	for (index = 0; index < sizeof(value); index++) {
		value[index] = (uint8_t)(index * 31U + 0x49U);
	}
	for (index = 0; index < sizeof(small); index++) {
		small[index] = (uint8_t)(index * 31U + 0x49U);
	}
	inode = lookup(fs, "/linux-xattr-file");
	EXPECT(ext4_set_attributes(fs, inode.number, inode.generation, &update, &result), EXT4_OK);
	CHECK(result.number == inode.number && result.generation == inode.generation &&
	    result.uid == update.uid && result.gid == update.gid &&
	    (result.mode & EXT4_MODE_PERMISSIONS) == update.permissions);
	EXPECT(ext4_sync(fs), EXT4_OK);
	CHECK(memcmp(device->cache, device->stable, device->size) == 0);
	stream = fopen(output, "wbx");
	CHECK(stream != NULL);
	CHECK(fwrite(device->stable, 1, device->size, stream) == device->size);
	CHECK(fclose(stream) == 0);
}

int
main(int argc, char **argv)
{
	struct device device;
	struct ext4_fs *fs;
	struct xattr_case *cases = calloc(XATTR_TEST_CASES, sizeof(*cases));
	struct ext4_inode inode;
	struct ext4_inode unchanged;
	struct ext4_inode result;
	struct ext4_inode_update update = { .fields = EXT4_ATTR_PERMISSIONS | EXT4_ATTR_CHANGE_TIME,
		.permissions = 0600,
		.change_time = { .seconds = 1700000070 } };
	size_t count;
	size_t index;
	const char *exports = NULL;
	const char *roundtrip = NULL;
	const uint8_t *binary = NULL;
	bool verify_only = false;
	int argument = 1;

	CHECK(argc >= 3 && cases != NULL);
	if (strcmp(argv[argument], "--export") == 0) {
		CHECK(argc == 5);
		exports = argv[2];
		argument += 2;
	} else if (strcmp(argv[argument], "--verify") == 0) {
		CHECK(argc == 4);
		verify_only = true;
		argument++;
	} else if (strcmp(argv[argument], "--roundtrip") == 0) {
		CHECK(argc == 5);
		verify_only = true;
		roundtrip = argv[2];
		argument += 2;
	}
	CHECK(argument + 2 == argc);
	count = load_cases(argv[argument + 1], cases);
	if (!verify_only) {
		for (index = 0; index < count; index++) {
			if (strcmp(cases[index].path, "/block") == 0) {
				CHECK(cases[index].value_size == XATTR_TEST_BINARY_BYTES);
				binary = cases[index].value;
			}
		}
		CHECK(binary != NULL);
	}
	storage_open(&device, argv[argument]);
	EXPECT(ext4_mount(&device.environment, &fs), EXT4_OK);
	functional(&device, fs, cases, count);
	if (!verify_only) {
		faults(&device, fs);
		corruption(&device, fs);
		body_corruption(&device, fs);
		positive_edges(&device, fs, binary, exports, argv[argument]);
	}
	ext4_unmount(fs);
	CHECK(device.live == 0 && device.writes == 0 &&
	    memcmp(device.cache, device.base, device.size) == 0);
	EXPECT(ext4_mount_writable(&device.environment, &device.writer, &fs), EXT4_OK);
	functional(&device, fs, cases, count);
	if (!verify_only) {
		faults(&device, fs);
		inode = lookup(fs, "/many");
		memset(&unchanged, 0xa5, sizeof(unchanged));
		result = unchanged;
		EXPECT(ext4_set_attributes(fs, inode.number, inode.generation, &update, &result),
		    EXT4_INVALID_ARGUMENT);
		CHECK(memcmp(&result, &unchanged, sizeof(result)) == 0);
	}
	if (roundtrip != NULL) {
		return_linux_attributes(&device, fs, roundtrip);
	}
	ext4_unmount(fs);
	CHECK(device.live == 0);
	if (roundtrip == NULL) {
		CHECK(device.writes == 0 && device.events == 0 &&
		    memcmp(device.cache, device.base, device.size) == 0 &&
		    memcmp(device.stable, device.base, device.size) == 0);
	}
	storage_close(&device);
	for (index = 0; index < count; index++) {
		free(cases[index].value);
	}
	free(cases);
	printf("PASS xattr %s: %s; exact independent values/list under read-only and writable "
	       "owners\n",
	    roundtrip != NULL ? "Linux return transaction"
		: verify_only ? "value verification"
			      : "reader",
	    argv[argument]);
	return 0;
}

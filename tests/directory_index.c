/* SPDX-License-Identifier: BSD-3-Clause */
#include "storage.h"
#include "directory_index.h"

enum damage {
	DAMAGE_NONE,
	DAMAGE_DOT_INODE,
	DAMAGE_DOT_NAME,
	DAMAGE_DOT_LENGTH,
	DAMAGE_PARENT_INODE,
	DAMAGE_PARENT_NAME,
	DAMAGE_PARENT_LENGTH,
	DAMAGE_RESERVED,
	DAMAGE_INFO_LENGTH,
	DAMAGE_HASH_VERSION,
	DAMAGE_DEPTH,
	DAMAGE_FLAGS,
	DAMAGE_HASH_FLAGS_BOTH,
	DAMAGE_HASH_FLAGS_NEITHER,
	DAMAGE_COUNT_ZERO,
	DAMAGE_COUNT_OVERFLOW,
	DAMAGE_LIMIT,
	DAMAGE_ROOT_CYCLE,
	DAMAGE_BEYOND_EOF,
	DAMAGE_DUPLICATE_CHILD,
	DAMAGE_UNREACHABLE,
	DAMAGE_HASH_ORDER,
	DAMAGE_HASH_DUPLICATE,
	DAMAGE_CHECKSUM,
	DAMAGE_TAIL_RESERVED,
	DAMAGE_NODE_INODE,
	DAMAGE_NODE_NAME,
	DAMAGE_NODE_TYPE,
	DAMAGE_NODE_LENGTH,
	DAMAGE_NODE_COUNT,
	DAMAGE_NODE_CYCLE,
	DAMAGE_NODE_HASH_RANGE,
	DAMAGE_NODE_DUPLICATE_CHILD,
	DAMAGE_COUNT
};

struct observation {
	uint32_t blocks;
	uint32_t leaves;
	uint32_t nodes;
	uint32_t names;
	uint8_t levels;
	uint8_t version;
};

static bool
needs_node(enum damage damage)
{
	return damage >= DAMAGE_NODE_INODE;
}

/* Recompute the wire checksum after structural damage, so those cases exercise
 * topology validation rather than merely failing the checksum gate. */
static void
index_checksum(
    struct ext4_fs *fs, const struct ext4_inode *inode, uint8_t *buffer, uint32_t logical)
{
	uint32_t base = logical == 0 ? sizeof(struct ext4_dx_root_prefix_disk)
				     : sizeof(struct ext4_dir_header_disk);
	struct ext4_dx_count_disk *counts = (struct ext4_dx_count_disk *)(buffer + base);
	struct ext4_dx_tail_disk *tail;
	uint16_t limit = ext4_le16(&counts->limit);
	uint16_t count = ext4_le16(&counts->count);
	uint32_t checksum;

	if (!fs->metadata_checksum || count == 0 || count > limit ||
	    limit > (fs->info.block_size - base - sizeof(*tail)) / sizeof(*counts)) {
		return;
	}
	tail = (struct ext4_dx_tail_disk *)(buffer + base + limit * sizeof(*counts));
	ext4_encode32(&tail->checksum, 0);
	checksum = ext4_crc32c(ext4_inode_seed(fs, inode), buffer, base + count * sizeof(*counts));
	checksum = ext4_crc32c(checksum, tail, sizeof(*tail));
	ext4_encode32(&tail->checksum, checksum);
}

static enum ext4_result
damage_index(struct ext4_allocation *allocation, const struct ext4_inode *inode,
    struct ext4_inode_disk *disk, enum damage damage)
{
	struct ext4_fs *fs = allocation->fs;
	struct ext4_map_run run;
	struct ext4_dx_root_prefix_disk *root;
	struct ext4_dx_count_disk *counts;
	struct ext4_dx_entry_disk *entries;
	struct ext4_dx_tail_disk *tail;
	struct ext4_dir_header_disk *header;
	void *snapshot = NULL;
	uint8_t *buffer;
	uint32_t logical = 0;
	uint32_t base = sizeof(*root);
	uint32_t boundary = 0;
	uint16_t count;
	uint8_t levels;
	uint8_t level;
	enum ext4_result error;

	error = ext4_write_map_lookup(allocation, inode, disk, 0, &run);
	if (error == EXT4_OK) {
		error = ext4_transaction_buffer(allocation->transaction, run.physical, &snapshot);
	}
	if (error != EXT4_OK) {
		return error;
	}
	buffer = snapshot;
	root = (struct ext4_dx_root_prefix_disk *)buffer;
	entries = (struct ext4_dx_entry_disk *)(buffer + base);
	levels = root->indirect_levels;
	if (needs_node(damage)) {
		CHECK(levels > 0 && levels <= EXT4_DX_MAX_INDIRECT_LEVELS);
		for (level = 0; level < levels; level++) {
			boundary = ext4_le32(&entries[1].hash);
			logical = ext4_le32(&entries[0].block);
			error = ext4_write_map_lookup(allocation, inode, disk, logical, &run);
			if (error == EXT4_OK) {
				error = ext4_transaction_buffer(
				    allocation->transaction, run.physical, &snapshot);
			}
			if (error != EXT4_OK) {
				return error;
			}
			buffer = snapshot;
			base = sizeof(*header);
			entries = (struct ext4_dx_entry_disk *)(buffer + base);
		}
	}
	header = (struct ext4_dir_header_disk *)buffer;
	counts = (struct ext4_dx_count_disk *)(buffer + base);
	count = ext4_le16(&counts->count);
	CHECK(count >= 2);
	switch (damage) {
	case DAMAGE_NONE:
		break;
	case DAMAGE_DOT_INODE:
		ext4_encode32(&root->dot.inode, 0);
		break;
	case DAMAGE_DOT_NAME:
		root->dot_name[0] = 'x';
		break;
	case DAMAGE_DOT_LENGTH:
		ext4_encode16(&root->dot.record_length, sizeof(root->dot));
		break;
	case DAMAGE_PARENT_INODE:
		ext4_encode32(&root->dotdot.inode, 0);
		break;
	case DAMAGE_PARENT_NAME:
		root->dotdot_name[1] = 'x';
		break;
	case DAMAGE_PARENT_LENGTH:
		ext4_encode16(&root->dotdot.record_length, sizeof(root->dotdot));
		break;
	case DAMAGE_RESERVED:
		ext4_encode32(&root->reserved, 1);
		break;
	case DAMAGE_INFO_LENGTH:
		root->info_length++;
		break;
	case DAMAGE_HASH_VERSION:
		root->hash_version = UINT8_MAX;
		break;
	case DAMAGE_DEPTH:
		root->indirect_levels = ext4_index_max_levels(fs) + 1;
		break;
	case DAMAGE_FLAGS:
		root->flags = 1;
		break;
	case DAMAGE_HASH_FLAGS_BOTH:
	case DAMAGE_HASH_FLAGS_NEITHER:
		error = ext4_allocation_super(allocation);
		if (error != EXT4_OK) {
			return error;
		}
		ext4_encode32(&allocation->super->flags,
		    damage == DAMAGE_HASH_FLAGS_BOTH
			? EXT4_SIGNED_DIRECTORY_HASH | EXT4_UNSIGNED_DIRECTORY_HASH
			: 0);
		break;
	case DAMAGE_COUNT_ZERO:
	case DAMAGE_NODE_COUNT:
		ext4_encode16(&counts->count, 0);
		break;
	case DAMAGE_COUNT_OVERFLOW:
		ext4_encode16(&counts->count, ext4_le16(&counts->limit) + 1);
		break;
	case DAMAGE_LIMIT:
		ext4_encode16(&counts->limit, ext4_le16(&counts->limit) - 1);
		break;
	case DAMAGE_ROOT_CYCLE:
		ext4_encode32(&entries[0].block, 0);
		break;
	case DAMAGE_BEYOND_EOF:
		ext4_encode32(&entries[0].block, (uint32_t)(inode->size / fs->info.block_size));
		break;
	case DAMAGE_DUPLICATE_CHILD:
	case DAMAGE_NODE_DUPLICATE_CHILD:
		entries[1].block = entries[0].block;
		break;
	case DAMAGE_UNREACHABLE:
		ext4_encode16(&counts->count, count - 1);
		break;
	case DAMAGE_HASH_ORDER:
		ext4_encode32(&entries[count - 1].hash, EXT4_HASH_EOF);
		break;
	case DAMAGE_HASH_DUPLICATE:
		ext4_encode32(&entries[1].hash, 0);
		break;
	case DAMAGE_CHECKSUM:
		CHECK(fs->metadata_checksum);
		root->dot_name[sizeof(root->dot_name) - 1] ^= 1;
		return EXT4_OK;
	case DAMAGE_TAIL_RESERVED:
		tail = (struct ext4_dx_tail_disk *)(buffer + base +
		    ext4_le16(&counts->limit) * sizeof(*counts));
		ext4_encode32(&tail->reserved, 1);
		break;
	case DAMAGE_NODE_INODE:
		ext4_encode32(&header->inode, inode->number);
		break;
	case DAMAGE_NODE_NAME:
		header->name_length = 1;
		break;
	case DAMAGE_NODE_TYPE:
		header->type = EXT4_FT_DIRECTORY;
		break;
	case DAMAGE_NODE_LENGTH:
		ext4_encode16(&header->record_length, sizeof(*header));
		break;
	case DAMAGE_NODE_CYCLE:
		ext4_encode32(&entries[0].block, logical);
		break;
	case DAMAGE_NODE_HASH_RANGE:
		ext4_encode32(&entries[count - 1].hash, boundary + 2);
		break;
	case DAMAGE_COUNT:
		CHECK(false);
	}
	index_checksum(fs, inode, buffer, logical);
	return EXT4_OK;
}

static void
observe(struct ext4_directory_index *index, struct observation *result)
{
	struct ext4_allocation *allocation = index->allocation;
	struct ext4_fs *fs = allocation->fs;
	struct ext4_dir_header_disk *entry;
	struct ext4_name_hash hash;
	uint64_t physical;
	uint32_t logical;
	uint32_t offset;
	uint32_t length;
	uint32_t usable =
	    fs->info.block_size - (fs->metadata_checksum ? sizeof(struct ext4_dir_tail_disk) : 0);

	memset(result, 0, sizeof(*result));
	result->blocks = index->blocks;
	result->levels = index->levels;
	result->version = index->version;
	for (logical = 1; logical < index->blocks; logical++) {
		if (index->ranges[logical].kind == EXT4_INDEX_NODE) {
			result->nodes++;
			continue;
		}
		CHECK(index->ranges[logical].kind == EXT4_INDEX_LEAF);
		result->leaves++;
		EXPECT(ext4_index_read(index, logical, &physical), EXT4_OK);
		EXPECT(ext4_directory_checksum(fs, index->inode, logical, allocation->scratch),
		    EXT4_OK);
		for (offset = 0; offset < usable; offset += length) {
			CHECK(usable - offset >= sizeof(*entry));
			entry = (struct ext4_dir_header_disk *)(allocation->scratch + offset);
			length = ext4_directory_record_length(fs, entry);
			CHECK(length >= sizeof(*entry) && !(length % EXT4_DIRECTORY_ALIGNMENT) &&
			    length <= usable - offset &&
			    entry->name_length <= length - sizeof(*entry));
			if (ext4_le32(&entry->inode) == 0) {
				continue;
			}
			EXPECT(ext4_directory_hash(index->version, index->seed,
				   allocation->scratch + offset + sizeof(*entry),
				   entry->name_length, &hash),
			    EXT4_OK);
			CHECK(ext4_index_contains(&index->ranges[logical], hash.major));
			result->names++;
		}
	}
}

static enum ext4_result
attempt(struct ext4_fs *fs, const struct ext4_inode *parent, enum damage damage,
    struct observation *observation)
{
	struct ext4_transaction *transaction = NULL;
	struct ext4_allocation allocation;
	struct ext4_directory_index index;
	struct ext4_inode inode;
	struct ext4_inode_disk *disk;
	bool initialized = false;
	enum ext4_result error;

	error =
	    ext4_transaction_begin(fs->journal, ext4_journal_credits(fs->journal), &transaction);
	if (error != EXT4_OK) {
		return error;
	}
	error = ext4_edit_inode(fs, transaction, parent->number, parent->generation, &disk, &inode);
	if (error == EXT4_OK) {
		error = ext4_allocation_init(&allocation, fs, transaction, &inode);
		initialized = error == EXT4_OK;
	}
	if (error == EXT4_OK && damage != DAMAGE_NONE) {
		error = damage_index(&allocation, &inode, disk, damage);
	}
	if (error == EXT4_OK) {
		error = ext4_index_open(&allocation, &inode, disk, &index, true);
		if (error == EXT4_OK) {
			if (observation != NULL) {
				observe(&index, observation);
			}
			ext4_index_close(&index);
		}
	}
	if (initialized) {
		ext4_allocation_destroy(&allocation);
	}
	ext4_transaction_cancel(transaction);
	return error;
}

static void
range_boundaries(void)
{
	struct ext4_index_range range = { .lower = 100, .upper = 200 };

	CHECK(!ext4_index_contains(&range, 98) && ext4_index_contains(&range, 100));
	CHECK(ext4_index_contains(&range, 198) && !ext4_index_contains(&range, 200));
	range.upper++;
	CHECK(ext4_index_contains(&range, 200) && !ext4_index_contains(&range, 202));
	range.lower = 201;
	CHECK(ext4_index_contains(&range, 200) && !ext4_index_contains(&range, 198));
	range.upper = EXT4_DX_HASH_END;
	CHECK(ext4_index_contains(&range, EXT4_HASH_EOF - 2));
}

static void
check_image(struct device *device, bool scan_only)
{
	struct ext4_fs *fs;
	struct ext4_inode root;
	struct ext4_inode parent;
	struct ext4_inode peer;
	struct observation observed;
	struct observation other;
	uint32_t live;
	uint32_t allocations;
	uint32_t reads;
	uint32_t index;
	uint32_t malformed = 0;
	uint32_t reserved = 0;
	uint32_t skipped = 0;
	enum damage damage;
	enum ext4_result expected;

	EXPECT(ext4_mount_writable(&device->environment, &device->writer, &fs), EXT4_OK);
	EXPECT(ext4_get_inode(fs, EXT4_ROOT_INODE, &root), EXT4_OK);
	EXPECT(ext4_lookup(fs, &root, (const uint8_t *)"indexed", 7, &parent), EXT4_OK);
	live = device->live;
	EXPECT(attempt(fs, &parent, DAMAGE_NONE, &observed), EXT4_OK);
	CHECK(observed.leaves > 1 && observed.blocks == 1 + observed.leaves + observed.nodes);
	if (scan_only) {
		ext4_unmount(fs);
		CHECK(device->live == 0 && device->writes == 0 &&
		    memcmp(device->base, device->cache, device->size) == 0);
		printf("PASS complete graph blocks=%u leaves=%u nodes=%u levels=%u names=%u\n",
		    observed.blocks, observed.leaves, observed.nodes, observed.levels,
		    observed.names);
		return;
	}
	EXPECT(ext4_lookup(fs, &root, (const uint8_t *)"peer", 4, &peer), EXT4_OK);
	EXPECT(attempt(fs, &peer, DAMAGE_NONE, &other), EXT4_OK);
	if (!(fs->info.feature_incompat & EXT4_FEATURE_INCOMPAT_LARGEDIR)) {
		CHECK(memcmp(&observed, &other, sizeof(observed)) == 0);
	} else {
		CHECK(other.leaves > 1 && other.blocks == 1 + other.leaves + other.nodes &&
		    other.version == observed.version);
	}
	for (damage = DAMAGE_DOT_INODE; damage < DAMAGE_COUNT; damage++) {
		if ((needs_node(damage) && observed.levels == 0) ||
		    ((damage == DAMAGE_CHECKSUM || damage == DAMAGE_TAIL_RESERVED) &&
			!fs->metadata_checksum)) {
			skipped++;
			continue;
		}
		expected = damage == DAMAGE_HASH_VERSION || damage == DAMAGE_DEPTH ||
			damage == DAMAGE_FLAGS || damage == DAMAGE_HASH_FLAGS_NEITHER
		    ? EXT4_UNSUPPORTED
		    : EXT4_CORRUPT;
		if (damage == DAMAGE_DEPTH && fs->metadata_checksum &&
		    ext4_index_max_levels(fs) == EXT4_DX_MAX_INDIRECT_LEVELS) {
			expected = EXT4_CORRUPT;
		}
		/* The dx tail's reserved word is unused, checksum-covered payload;
		 * its format does not require zero. */
		if (damage == DAMAGE_TAIL_RESERVED) {
			expected = EXT4_OK;
			reserved++;
		}
		printf("damage=%u\n", (unsigned int)damage);
		EXPECT(attempt(fs, &parent, damage, NULL), expected);
		CHECK(device->writes == 0 && device->live == live);
		malformed += expected != EXT4_OK;
	}
	device->reads = device->allocations = 0;
	EXPECT(attempt(fs, &parent, DAMAGE_NONE, NULL), EXT4_OK);
	allocations = device->allocations;
	reads = device->reads;
	for (index = 1; index <= allocations; index++) {
		device->allocations = 0;
		device->fail_allocation = index;
		EXPECT(attempt(fs, &parent, DAMAGE_NONE, NULL), EXT4_NO_MEMORY);
		CHECK(device->writes == 0 && device->live == live);
	}
	device->fail_allocation = 0;
	for (index = 1; index <= reads; index++) {
		device->reads = 0;
		device->fail_read = index;
		EXPECT(attempt(fs, &parent, DAMAGE_NONE, NULL), EXT4_IO);
		CHECK(device->writes == 0 && device->live == live);
	}
	device->fail_read = 0;
	EXPECT(attempt(fs, &parent, DAMAGE_NONE, NULL), EXT4_OK);
	ext4_unmount(fs);
	CHECK(device->live == 0 && device->writes == 0 &&
	    memcmp(device->base, device->cache, device->size) == 0);
	CHECK(storage_recover(device, device->base, true) && device->writes == 0);
	printf("PASS graph blocks=%u leaves=%u nodes=%u levels=%u version=%u names=%u "
	       "malformed=%u reserved=%u skipped=%u allocations=%u reads=%u\n",
	    observed.blocks, observed.leaves, observed.nodes, observed.levels, observed.version,
	    observed.names, malformed, reserved, skipped, allocations, reads);
}

int
main(int argc, char **argv)
{
	struct device device;
	const char *exports = NULL;
	bool scan_only = false;
	int argument = 1;

	range_boundaries();
	while (argument < argc && argv[argument][0] == '-') {
		if (strcmp(argv[argument], "--export") == 0 && argument + 1 < argc) {
			exports = argv[++argument];
		} else if (strcmp(argv[argument], "--scan-only") == 0) {
			scan_only = true;
		} else {
			CHECK(false);
		}
		argument++;
	}
	CHECK(argument < argc);
	for (; argument < argc; argument++) {
		printf("IMAGE %s\n", argv[argument]);
		storage_open(&device, argv[argument]);
		check_image(&device, scan_only);
		storage_export(&device, exports, argv[argument], "index-validated-");
		storage_close(&device);
	}
	return 0;
}

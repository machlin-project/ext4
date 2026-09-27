/* SPDX-License-Identifier: BSD-3-Clause */
#include <inttypes.h>

#define EDGE_SHORT_LENGTH 96U
#define EDGE_SHORT_COUNT 9U
#define EDGE_SEARCH_LIMIT 1000000U

struct edge_view {
	struct ext4_transaction *transaction;
	struct ext4_allocation allocation;
	struct ext4_inode inode;
	struct ext4_inode_disk *disk;
	struct ext4_directory_index tree;
};

struct edge_collision {
	uint8_t names[2][EXT4_NAME_MAX + 1];
	uint32_t major;
	uint32_t seed[4];
	uint8_t version;
	struct ext4_index_range range;
};

static void
edge_open(struct ext4_fs *fs, const struct ext4_inode *parent, struct edge_view *view)
{
	EXPECT(ext4_transaction_begin(
		   fs->journal, ext4_journal_credits(fs->journal), &view->transaction),
	    EXT4_OK);
	EXPECT(ext4_edit_inode(fs, view->transaction, parent->number, parent->generation,
		   &view->disk, &view->inode),
	    EXT4_OK);
	EXPECT(
	    ext4_allocation_init(&view->allocation, fs, view->transaction, &view->inode), EXT4_OK);
	EXPECT(ext4_index_open(&view->allocation, &view->inode, view->disk, &view->tree), EXT4_OK);
}

static void
edge_close(struct edge_view *view)
{
	ext4_index_close(&view->tree);
	ext4_allocation_destroy(&view->allocation);
	ext4_transaction_cancel(view->transaction);
}

static uint8_t
edge_hex(char byte)
{
	if (byte >= '0' && byte <= '9') {
		return (uint8_t)(byte - '0');
	}
	CHECK(byte >= 'a' && byte <= 'f');
	return (uint8_t)(byte - 'a' + 10);
}

static struct edge_collision
edge_vectors(struct ext4_fs *fs, const struct ext4_inode *parent, const char *path)
{
	struct edge_view view;
	struct edge_collision collision;
	struct ext4_name_hash hash;
	FILE *stream;
	char encoded[2][EXT4_NAME_MAX * 2U + 1U];
	uint32_t major;
	uint32_t logical;
	uint32_t eligible = 0;
	size_t name;
	size_t index;
	unsigned int version;
	unsigned int matches = 0;
	int fields;

	_Static_assert(EXT4_NAME_MAX * 2U == 510U, "collision vector scan width");
	memset(&collision, 0, sizeof(collision));
	edge_open(fs, parent, &view);
	collision.version = view.tree.version;
	memcpy(collision.seed, view.tree.seed, sizeof(collision.seed));
	stream = fopen(path, "r");
	CHECK(stream != NULL);
	while ((fields = fscanf(stream, "%u %" SCNx32 " %510s %510s", &version, &major, encoded[0],
		    encoded[1])) != EOF) {
		CHECK(fields == 4 && version <= EXT4_HASH_TEA_UNSIGNED);
		if (version != collision.version) {
			continue;
		}
		matches++;
		collision.major = major;
		for (name = 0; name < 2; name++) {
			CHECK(strlen(encoded[name]) == EXT4_NAME_MAX * 2U);
			for (index = 0; index < EXT4_NAME_MAX; index++) {
				collision.names[name][index] =
				    (uint8_t)((edge_hex(encoded[name][index * 2]) << 4) |
					edge_hex(encoded[name][index * 2 + 1]));
				CHECK(collision.names[name][index] != 0 &&
				    collision.names[name][index] != '/');
			}
			EXPECT(ext4_directory_hash(collision.version, collision.seed,
				   collision.names[name], EXT4_NAME_MAX, &hash),
			    EXT4_OK);
			CHECK(hash.major == major);
		}
	}
	CHECK(!ferror(stream) && fclose(stream) == 0 && matches == 1);
	CHECK(memcmp(collision.names[0], collision.names[1], EXT4_NAME_MAX) != 0);
	for (logical = 0; logical < view.tree.blocks; logical++) {
		if (view.tree.ranges[logical].kind == EXT4_INDEX_LEAF &&
		    ext4_index_contains(&view.tree.ranges[logical], collision.major)) {
			eligible++;
			collision.range = view.tree.ranges[logical];
		}
	}
	CHECK(eligible == 1 && collision.major > collision.range.lower &&
	    collision.major + UINT64_C(2) < collision.range.upper);
	edge_close(&view);
	return collision;
}

static void
edge_name(uint8_t *name, uint32_t ordinal, size_t length)
{
	int prefix;

	prefix = snprintf((char *)name, length + 1, "edge-%08x-", ordinal);
	CHECK(prefix > 0 && (size_t)prefix < length);
	memset(name + prefix, 'g', length - (size_t)prefix);
	name[length] = 0;
}

static struct ext4_directory_slot
edge_slot(struct ext4_fs *fs, const struct ext4_inode *parent, const uint8_t *name, size_t length,
    enum ext4_directory_action action)
{
	struct edge_view view;
	struct ext4_directory_slot slot;

	edge_open(fs, parent, &view);
	EXPECT(ext4_directory_scan(
		   &view.allocation, &view.inode, view.disk, name, length, action, 0, &slot),
	    EXT4_OK);
	edge_close(&view);
	return slot;
}

static void
edge_link(struct ext4_fs *fs, const struct ext4_inode *parent, const struct ext4_inode *target,
    const uint8_t *name, size_t length)
{
	struct ext4_inode result;

	EXPECT(ext4_link(fs, parent->number, parent->generation, name, length, target->number,
		   target->generation, &mutation_time, &result),
	    EXT4_OK);
	CHECK(result.number == target->number && result.generation == target->generation);
}

static void
edge_unlink(struct ext4_fs *fs, const struct ext4_inode *parent, const struct ext4_inode *target,
    const uint8_t *name, size_t length)
{
	struct ext4_inode result;

	EXPECT(ext4_unlink(fs, parent->number, parent->generation, name, length, target->number,
		   target->generation, &mutation_time, &result),
	    EXT4_OK);
}

static void
edge_finish(struct device *device, struct ext4_fs *fs)
{
	uint8_t *clean = malloc(device->size);

	CHECK(clean != NULL);
	EXPECT(ext4_sync(fs), EXT4_OK);
	memcpy(clean, device->stable, device->size);
	ext4_unmount(fs);
	CHECK(storage_recover(device, clean, true) && device->writes == 0);
	free(clean);
}

struct edge_leaf_reference {
	uint32_t hash;
	uint32_t logical;
};

static bool
edge_leaf_empty(struct edge_view *view, uint32_t logical)
{
	struct ext4_dir_entry entry;
	uint64_t physical;
	uint32_t offset = 0;
	uint32_t length;

	EXPECT(ext4_index_read(&view->tree, logical, &physical), EXT4_OK);
	while (offset < view->allocation.fs->info.block_size) {
		EXPECT(ext4_directory_entry_decode(
			   view->allocation.fs, view->allocation.scratch, offset, &entry, &length),
		    EXT4_OK);
		if (entry.inode != 0) {
			return false;
		}
		offset += length;
	}
	return true;
}

/* Put the verified collision separator in the root as well as the leaf index.
 * Repartition existing leaves; if a third node is needed, reuse an empty leaf.
 * Every mapped block stays reachable and allocation/inode accounting is unchanged. */
static void
edge_collision_boundary(struct ext4_fs *fs, const struct ext4_inode *parent,
    const struct edge_collision *collision, uint32_t first, uint32_t second)
{
	struct edge_view view;
	struct edge_leaf_reference *leaves;
	struct ext4_dx_entry_disk *entries;
	struct ext4_dx_count_disk *counts;
	struct ext4_dir_header_disk *header;
	struct ext4_map_run run;
	uint32_t nodes[3] = { 0 };
	uint32_t starts[4] = { 0 };
	uint32_t count = 0;
	uint32_t split = 0;
	uint32_t node_count = 2;
	uint32_t position;
	uint32_t group;
	uint32_t spare;
	uint32_t capacity;
	uint64_t physical;
	uint16_t entry;
	uint16_t entries_count;
	uint16_t limit;
	uint8_t *buffer;
	void *snapshot;

	edge_open(fs, parent, &view);
	if (view.tree.levels == 0) {
		edge_close(&view);
		return;
	}
	EXPECT(ext4_index_read(&view.tree, 0, &physical), EXT4_OK);
	counts = (struct ext4_dx_count_disk *)(view.allocation.scratch +
	    sizeof(struct ext4_dx_root_prefix_disk));
	entries = (struct ext4_dx_entry_disk *)counts;
	CHECK(view.tree.levels == 1 && ext4_le16(&counts->count) == 2);
	limit = ext4_le16(&counts->limit);
	nodes[0] = ext4_le32(&entries[0].block);
	nodes[1] = ext4_le32(&entries[1].block);
	capacity = (fs->info.block_size - sizeof(*header) -
		       (fs->metadata_checksum ? sizeof(struct ext4_dx_tail_disk) : 0U)) /
	    sizeof(*counts);
	leaves = calloc(view.tree.blocks, sizeof(*leaves));
	CHECK(leaves != NULL);
	for (group = 0; group < 2; group++) {
		EXPECT(ext4_index_read(&view.tree, nodes[group], &physical), EXT4_OK);
		counts = (struct ext4_dx_count_disk *)(view.allocation.scratch + sizeof(*header));
		entries = (struct ext4_dx_entry_disk *)counts;
		entries_count = ext4_le16(&counts->count);
		for (entry = 0; entry < entries_count; entry++) {
			CHECK(count < view.tree.blocks);
			leaves[count].logical = ext4_le32(&entries[entry].block);
			leaves[count].hash = view.tree.ranges[leaves[count].logical].lower;
			if (leaves[count].logical == second) {
				split = count;
			}
			count++;
		}
	}
	CHECK(split > 0 && split < count && leaves[split - 1U].logical == first &&
	    leaves[split].hash == (collision->major | 1U));
	if (split > capacity || count - split > capacity) {
		for (spare = 0; spare < count; spare++) {
			if (leaves[spare].logical != first && leaves[spare].logical != second &&
			    edge_leaf_empty(&view, leaves[spare].logical)) {
				break;
			}
		}
		CHECK(spare < count);
		nodes[2] = leaves[spare].logical;
		memmove(
		    leaves + spare, leaves + spare + 1U, (count - spare - 1U) * sizeof(*leaves));
		count--;
		if (spare < split) {
			split--;
		}
		node_count = 3;
	}
	leaves[0].hash = 0;
	if (node_count == 2) {
		starts[1] = split;
	} else if (split > capacity) {
		starts[1] = capacity;
		starts[2] = split;
	} else if (count - split > capacity) {
		starts[1] = split;
		starts[2] = split + capacity;
	} else if (split > 1U) {
		starts[1] = split - 1U;
		starts[2] = split;
	} else {
		starts[1] = split;
		starts[2] = split + 1U;
	}
	starts[node_count] = count;
	for (group = 0; group < node_count; group++) {
		entries_count = (uint16_t)(starts[group + 1U] - starts[group]);
		CHECK(entries_count > 0 && entries_count <= capacity);
		EXPECT(ext4_write_map_lookup(
			   &view.allocation, &view.inode, view.disk, nodes[group], &run),
		    EXT4_OK);
		EXPECT(ext4_transaction_buffer(view.transaction, run.physical, &snapshot), EXT4_OK);
		buffer = snapshot;
		memset(buffer, 0, fs->info.block_size);
		header = (struct ext4_dir_header_disk *)buffer;
		ext4_encode16(&header->record_length, (uint16_t)fs->info.block_size);
		counts = (struct ext4_dx_count_disk *)(buffer + sizeof(*header));
		entries = (struct ext4_dx_entry_disk *)counts;
		ext4_encode16(&counts->limit, (uint16_t)capacity);
		ext4_encode16(&counts->count, entries_count);
		for (entry = 0; entry < entries_count; entry++) {
			position = starts[group] + entry;
			if (entry != 0) {
				ext4_encode32(&entries[entry].hash, leaves[position].hash);
			}
			ext4_encode32(&entries[entry].block, leaves[position].logical);
		}
		ext4_index_checksum_set(fs, parent, nodes[group], buffer);
	}
	EXPECT(ext4_write_map_lookup(&view.allocation, &view.inode, view.disk, 0, &run), EXT4_OK);
	EXPECT(ext4_transaction_buffer(view.transaction, run.physical, &snapshot), EXT4_OK);
	buffer = snapshot;
	counts = (struct ext4_dx_count_disk *)(buffer + sizeof(struct ext4_dx_root_prefix_disk));
	memset(counts, 0, fs->info.block_size - sizeof(struct ext4_dx_root_prefix_disk));
	entries = (struct ext4_dx_entry_disk *)counts;
	ext4_encode16(&counts->limit, limit);
	ext4_encode16(&counts->count, (uint16_t)node_count);
	for (group = 0; group < node_count; group++) {
		if (group != 0) {
			ext4_encode32(&entries[group].hash, leaves[starts[group]].hash);
		}
		ext4_encode32(&entries[group].block, nodes[group]);
	}
	ext4_index_checksum_set(fs, parent, 0, buffer);
	ext4_index_close(&view.tree);
	ext4_allocation_destroy(&view.allocation);
	EXPECT(ext4_transaction_commit(view.transaction), EXT4_OK);
	free(leaves);
	edge_open(fs, parent, &view);
	CHECK(view.tree.ranges[first].parent != view.tree.ranges[second].parent &&
	    view.tree.ranges[first].upper == (collision->major | 1U) &&
	    view.tree.ranges[second].lower == (collision->major | 1U));
	edge_close(&view);
	printf("PASS collision crosses internal nodes=%" PRIu32 " leaves=%" PRIu32 ",%" PRIu32 "\n",
	    node_count, first, second);
}

static void
edge_lookup_faults(struct device *device, struct ext4_fs *fs, const struct ext4_inode *parent,
    const uint8_t *name, uint32_t number)
{
	struct ext4_inode result;
	struct ext4_inode before;
	uint32_t allocations = device->allocations;
	uint32_t reads = device->reads;
	uint32_t writes = device->writes;
	uint32_t events = device->events;
	uint32_t live = device->live;
	uint32_t fault;
	enum ext4_result expected;

	expected = number == 0 ? EXT4_NOT_FOUND : EXT4_OK;
	EXPECT(ext4_lookup(fs, parent, name, EXT4_NAME_MAX, &result), expected);
	if (number != 0) {
		CHECK(result.number == number);
	}
	allocations = device->allocations - allocations;
	reads = device->reads - reads;
	for (fault = 1; fault <= allocations + reads; fault++) {
		memset(&result, 0xa5, sizeof(result));
		memcpy(&before, &result, sizeof(before));
		if (fault <= allocations) {
			device->fail_allocation = device->allocations + fault;
			expected = EXT4_NO_MEMORY;
		} else {
			device->fail_read = device->reads + fault - allocations;
			expected = EXT4_IO;
		}
		EXPECT(ext4_lookup(fs, parent, name, EXT4_NAME_MAX, &result), expected);
		CHECK(memcmp(&before, &result, sizeof(before)) == 0 && device->live == live &&
		    device->writes == writes && device->events == events && !fs->aborted);
		device->fail_read = 0;
		device->fail_allocation = 0;
	}
	printf("PASS collision lookup faults target=%" PRIu32 " allocations=%" PRIu32
	       " reads=%" PRIu32 "\n",
	    number, allocations, reads);
}

static void
collision_chain(struct device *device, const char *path, const char *vectors, const char *exports)
{
	struct ext4_fs *fs;
	struct ext4_inode root;
	struct ext4_inode parent;
	struct ext4_inode hello;
	struct ext4_inode other;
	struct ext4_inode result;
	struct ext4_inode_update update = attributes();
	struct edge_collision collision;
	struct edge_view view;
	struct ext4_name_hash hash;
	struct ext4_directory_slot first;
	struct ext4_directory_slot second;
	uint8_t guard[2][EXT4_NAME_MAX + 1];
	uint8_t candidate[EXT4_NAME_MAX + 1];
	uint32_t ordinal;
	uint32_t writes;
	bool found[2] = { false, false };
	size_t side;

	device_reset(device, device->base);
	EXPECT(ext4_mount_writable(&device->environment, &device->writer, &fs), EXT4_OK);
	EXPECT(ext4_get_inode(fs, EXT4_ROOT_INODE, &root), EXT4_OK);
	parent = lookup(fs, &root, "indexed");
	hello = lookup(fs, &root, "hello.txt");
	collision = edge_vectors(fs, &parent, vectors);
	for (ordinal = 0; ordinal < EDGE_SEARCH_LIMIT && !(found[0] && found[1]); ordinal++) {
		edge_name(candidate, ordinal, EXT4_NAME_MAX);
		EXPECT(ext4_directory_hash(
			   collision.version, collision.seed, candidate, EXT4_NAME_MAX, &hash),
		    EXT4_OK);
		if (hash.major == collision.major ||
		    !ext4_index_contains(&collision.range, hash.major)) {
			continue;
		}
		side = hash.major > collision.major;
		if (!found[side]) {
			memcpy(guard[side], candidate, sizeof(candidate));
			found[side] = true;
		}
	}
	CHECK(found[0] && found[1]);
	empty_directory(fs, &parent);
	edge_link(fs, &parent, &hello, guard[0], EXT4_NAME_MAX);
	edge_link(fs, &parent, &hello, collision.names[0], EXT4_NAME_MAX);
	edge_link(fs, &parent, &hello, guard[1], EXT4_NAME_MAX);
	first = edge_slot(fs, &parent, collision.names[1], EXT4_NAME_MAX, EXT4_DIRECTORY_INSERT);
	CHECK(first.repack);
	EXPECT(ext4_create(fs, parent.number, parent.generation, collision.names[1], EXT4_NAME_MAX,
		   &update, &mutation_time, &other),
	    EXT4_OK);
	EXPECT(ext4_get_inode(fs, parent.number, &parent), EXT4_OK);
	first = edge_slot(fs, &parent, collision.names[0], EXT4_NAME_MAX, EXT4_DIRECTORY_FIND);
	second = edge_slot(fs, &parent, collision.names[1], EXT4_NAME_MAX, EXT4_DIRECTORY_FIND);
	CHECK(first.logical != second.logical && first.number == hello.number &&
	    second.number == other.number && hello.number != other.number);
	edge_open(fs, &parent, &view);
	CHECK(view.tree.ranges[first.logical].upper == (collision.major | 1U) &&
	    view.tree.ranges[second.logical].lower == (collision.major | 1U));
	edge_close(&view);
	edge_collision_boundary(fs, &parent, &collision, first.logical, second.logical);
	edge_lookup_faults(device, fs, &parent, collision.names[0], hello.number);
	edge_lookup_faults(device, fs, &parent, collision.names[1], other.number);
	writes = device->writes;
	for (side = 0; side < 2; side++) {
		EXPECT(ext4_link(fs, parent.number, parent.generation, collision.names[side],
			   EXT4_NAME_MAX, hello.number, hello.generation, &mutation_time, &result),
		    EXT4_EXISTS);
	}
	CHECK(device->writes == writes);
	EXPECT(ext4_sync(fs), EXT4_OK);
	storage_export(device, exports, path, "indexed-collided-");
	move(fs, &parent, collision.names[0], &parent, (const uint8_t *)"collision-moved", 0);
	EXPECT(
	    ext4_lookup(fs, &parent, collision.names[0], EXT4_NAME_MAX, &result), EXT4_NOT_FOUND);
	result = lookup(fs, &parent, (const char *)collision.names[1]);
	CHECK(result.number == other.number && result.generation == other.generation);
	move(fs, &parent, (const uint8_t *)"collision-moved", &parent, collision.names[0], 0);
	edge_unlink(fs, &parent, &hello, collision.names[0], EXT4_NAME_MAX);
	edge_unlink(fs, &parent, &hello, guard[0], EXT4_NAME_MAX);
	result = lookup(fs, &parent, (const char *)collision.names[1]);
	CHECK(result.number == other.number && result.generation == other.generation);
	edge_lookup_faults(device, fs, &parent, collision.names[0], 0);
	edge_lookup_faults(device, fs, &parent, collision.names[1], other.number);
	EXPECT(ext4_sync(fs), EXT4_OK);
	storage_export(device, exports, path, "indexed-collision-retained-");
	edge_unlink(fs, &parent, &other, collision.names[1], EXT4_NAME_MAX);
	EXPECT(
	    ext4_lookup(fs, &parent, collision.names[1], EXT4_NAME_MAX, &result), EXT4_NOT_FOUND);
	edge_finish(device, fs);
	printf("PASS collision continuation version=%u major=%08" PRIx32 " leaves=%u,%u\n",
	    collision.version, collision.major, first.logical, second.logical);
}

static void
fragmented_leaf(struct device *device, const char *path, const char *vectors, const char *exports)
{
	struct ext4_fs *fs;
	struct ext4_inode root;
	struct ext4_inode parent;
	struct ext4_inode hello;
	struct ext4_inode before;
	struct ext4_inode result;
	struct ext4_directory_slot slot;
	struct edge_collision collision;
	struct ext4_name_hash hash;
	struct ext4_dir_entry entry;
	uint8_t names[EDGE_SHORT_COUNT][EDGE_SHORT_LENGTH + 1];
	uint8_t candidate[EDGE_SHORT_LENGTH + 1];
	uint64_t free_blocks;
	uint64_t cookie = 0;
	uint32_t ordinal;
	uint32_t count = 0;
	uint32_t index;
	enum ext4_result error;

	device_reset(device, device->base);
	EXPECT(ext4_mount_writable(&device->environment, &device->writer, &fs), EXT4_OK);
	EXPECT(ext4_get_inode(fs, EXT4_ROOT_INODE, &root), EXT4_OK);
	parent = lookup(fs, &root, "indexed");
	hello = lookup(fs, &root, "hello.txt");
	collision = edge_vectors(fs, &parent, vectors);
	for (ordinal = 0; ordinal < EDGE_SEARCH_LIMIT && count < EDGE_SHORT_COUNT; ordinal++) {
		edge_name(candidate, ordinal, EDGE_SHORT_LENGTH);
		EXPECT(ext4_directory_hash(
			   collision.version, collision.seed, candidate, EDGE_SHORT_LENGTH, &hash),
		    EXT4_OK);
		if (ext4_index_contains(&collision.range, hash.major)) {
			memcpy(names[count++], candidate, sizeof(candidate));
		}
	}
	CHECK(count == EDGE_SHORT_COUNT);
	empty_directory(fs, &parent);
	for (index = 0; index < EDGE_SHORT_COUNT; index++) {
		edge_link(fs, &parent, &hello, names[index], EDGE_SHORT_LENGTH);
	}
	for (index = 1; index < EDGE_SHORT_COUNT; index += 2) {
		edge_unlink(fs, &parent, &hello, names[index], EDGE_SHORT_LENGTH);
	}
	slot = edge_slot(fs, &parent, collision.names[0], EXT4_NAME_MAX, EXT4_DIRECTORY_INSERT);
	CHECK(slot.repack);
	EXPECT(ext4_get_inode(fs, parent.number, &before), EXT4_OK);
	free_blocks = fs->info.free_blocks;
	edge_link(fs, &parent, &hello, collision.names[0], EXT4_NAME_MAX);
	EXPECT(ext4_get_inode(fs, parent.number, &parent), EXT4_OK);
	CHECK(parent.size == before.size && parent.blocks_512 == before.blocks_512 &&
	    fs->info.free_blocks == free_blocks);
	for (index = 0; index < EDGE_SHORT_COUNT; index++) {
		error = ext4_lookup(fs, &parent, names[index], EDGE_SHORT_LENGTH, &result);
		EXPECT(error, index & 1U ? EXT4_NOT_FOUND : EXT4_OK);
		if (!(index & 1U)) {
			CHECK(
			    result.number == hello.number && result.generation == hello.generation);
		}
	}
	result = lookup(fs, &parent, (const char *)collision.names[0]);
	CHECK(result.number == hello.number);
	count = 0;
	while ((error = ext4_next_dir(fs, &parent, &cookie, &entry)) == EXT4_OK) {
		count++;
	}
	EXPECT(error, EXT4_NOT_FOUND);
	CHECK(count == 2U + (EDGE_SHORT_COUNT + 1U) / 2U + 1U);
	EXPECT(ext4_sync(fs), EXT4_OK);
	storage_export(device, exports, path, "indexed-compacted-");
	edge_finish(device, fs);
	printf("PASS fragmented leaf compaction version=%u logical=%u retained=%u no_growth\n",
	    collision.version, slot.logical, count - 2U);
}

static void
indexed_edges(struct device *device, const char *path, const char *vectors, const char *exports)
{
	CHECK(device->block_size == EXT4_MIN_BLOCK_SIZE);
	collision_chain(device, path, vectors, exports);
	fragmented_leaf(device, path, vectors, exports);
}

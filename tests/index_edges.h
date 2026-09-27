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

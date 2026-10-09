/* SPDX-License-Identifier: BSD-3-Clause */
#include "storage.h"
#include "directory_index.h"
#include "directory_write.h"

#define ADDED_SMALL 700U
#define ADDED_LARGE 160U
#define ADDED_DEEP 32U
/* Links after a remount whose reads show one classification per mount. */
#define PROBED_LINKS 48U
#define PROBED_NODES 1024U
/* A split reads each node of its path when scanning, repacking and enrolling it. */
#define PROBED_PATH_READS 3U
#define TEST_NAME_PREFIX 11U
#define TEST_HIGH_BYTE_POSITION 16U
#define INDEX_SMALL_JOURNAL_CREDITS 4U

static const struct ext4_timestamp mutation_time = { .seconds = 1700000050 };

static struct ext4_inode_update
attributes(void)
{
	struct ext4_inode_update result;

	memset(&result, 0, sizeof(result));
	result.fields = EXT4_ATTR_PERMISSIONS | EXT4_ATTR_UID | EXT4_ATTR_GID |
	    EXT4_ATTR_ACCESS_TIME | EXT4_ATTR_MODIFY_TIME | EXT4_ATTR_CHANGE_TIME;
	result.permissions = 0750;
	result.uid = 70000;
	result.gid = 80000;
	result.access_time = result.modify_time = result.change_time = mutation_time;
	return result;
}

static void
filename(uint8_t name[EXT4_NAME_MAX + 1], unsigned int index)
{
	CHECK(snprintf((char *)name, EXT4_NAME_MAX + 1, "new-%06u-", index) == TEST_NAME_PREFIX);
	memset(name + TEST_NAME_PREFIX, 'n', EXT4_NAME_MAX - TEST_NAME_PREFIX);
	name[TEST_HIGH_BYTE_POSITION] = (uint8_t)(0x80U + index % 128U);
	name[EXT4_NAME_MAX] = 0;
}

static struct ext4_rename_entry
rename_entry(const struct ext4_inode *parent, const uint8_t *name, const struct ext4_inode *inode)
{
	struct ext4_rename_entry entry;

	memset(&entry, 0, sizeof(entry));
	entry.directory = parent->number;
	entry.directory_generation = parent->generation;
	entry.name = name;
	entry.name_length = strlen((const char *)name);
	if (inode != NULL) {
		entry.inode = inode->number;
		entry.generation = inode->generation;
	}
	return entry;
}

static struct ext4_inode
lookup(struct ext4_fs *fs, const struct ext4_inode *parent, const char *name)
{
	struct ext4_inode inode;

	EXPECT(ext4_lookup(fs, parent, (const uint8_t *)name, strlen(name), &inode), EXT4_OK);
	return inode;
}

static void
move(struct ext4_fs *fs, const struct ext4_inode *from, const uint8_t *source,
    const struct ext4_inode *to, const uint8_t *destination, uint32_t flags)
{
	struct ext4_inode object = lookup(fs, from, (const char *)source);
	struct ext4_inode target;
	struct ext4_inode result;
	struct ext4_rename_entry old = rename_entry(from, source, &object);
	struct ext4_rename_entry new;

	if (flags & EXT4_RENAME_EXCHANGE) {
		target = lookup(fs, to, (const char *)destination);
	}
	new = rename_entry(to, destination, flags & EXT4_RENAME_EXCHANGE ? &target : NULL);
	EXPECT(ext4_rename(fs, &old, &new, flags, &mutation_time, &result), EXT4_OK);
	CHECK(result.number == object.number && result.generation == object.generation);
	result = lookup(fs, to, (const char *)destination);
	CHECK(result.number == object.number);
	if ((object.mode & EXT4_MODE_TYPE) == EXT4_MODE_DIRECTORY) {
		result = lookup(fs, &object, "..");
		CHECK(result.number == to->number);
	}
}

static void
tree_shape(struct ext4_fs *fs, struct ext4_inode *inode, uint32_t *nodes, uint8_t *levels)
{
	struct ext4_transaction *transaction;
	struct ext4_allocation allocation;
	struct ext4_directory_index tree;
	struct ext4_inode_disk *disk;
	uint32_t logical;

	EXPECT(ext4_transaction_begin(fs->journal, ext4_journal_credits(fs->journal), &transaction),
	    EXT4_OK);
	EXPECT(ext4_edit_inode(fs, transaction, inode->number, inode->generation, &disk, inode),
	    EXT4_OK);
	EXPECT(ext4_allocation_init(&allocation, fs, transaction, inode), EXT4_OK);
	EXPECT(ext4_index_open(&allocation, inode, disk, &tree, true), EXT4_OK);
	*nodes = 0;
	*levels = tree.levels;
	for (logical = 0; logical < tree.blocks; logical++) {
		*nodes += tree.ranges[logical].kind == EXT4_INDEX_NODE;
	}
	ext4_index_close(&tree);
	ext4_allocation_destroy(&allocation);
	ext4_transaction_cancel(transaction);
}

static void
verify_names(struct ext4_fs *fs, const struct ext4_inode *parent, uint32_t added, uint32_t target)
{
	struct ext4_dir_entry entry;
	uint8_t name[EXT4_NAME_MAX + 1];
	bool *seen = calloc(added, sizeof(*seen));
	uint64_t cookie = 0;
	uint32_t count = 0;
	unsigned int index;
	enum ext4_result error;

	CHECK(seen != NULL);
	while ((error = ext4_next_dir(fs, parent, &cookie, &entry)) == EXT4_OK) {
		if (entry.name_length < TEST_NAME_PREFIX || memcmp(entry.name, "new-", 4) != 0) {
			continue;
		}
		CHECK(sscanf((const char *)entry.name, "new-%u-", &index) == 1);
		CHECK(index < added && !seen[index] && entry.inode == target);
		filename(name, index);
		CHECK(entry.name_length == EXT4_NAME_MAX &&
		    memcmp(name, entry.name, EXT4_NAME_MAX) == 0);
		seen[index] = true;
		count++;
	}
	EXPECT(error, EXT4_NOT_FOUND);
	CHECK(count == added);
	free(seen);
}

static void
empty_directory(struct ext4_fs *fs, const struct ext4_inode *parent)
{
	struct ext4_dir_entry entry;
	struct ext4_inode inode;
	struct ext4_inode result;
	uint64_t cookie;
	enum ext4_result error;

	for (;;) {
		cookie = 0;
		while ((error = ext4_next_dir(fs, parent, &cookie, &entry)) == EXT4_OK) {
			if (strcmp((const char *)entry.name, ".") != 0 &&
			    strcmp((const char *)entry.name, "..") != 0) {
				break;
			}
		}
		if (error == EXT4_NOT_FOUND) {
			break;
		}
		EXPECT(error, EXT4_OK);
		EXPECT(ext4_get_inode(fs, entry.inode, &inode), EXT4_OK);
		if ((inode.mode & EXT4_MODE_TYPE) == EXT4_MODE_DIRECTORY) {
			EXPECT(ext4_rmdir(fs, parent->number, parent->generation, entry.name,
				   entry.name_length, inode.number, inode.generation,
				   &mutation_time, &result),
			    EXT4_OK);
		} else {
			EXPECT(ext4_unlink(fs, parent->number, parent->generation, entry.name,
				   entry.name_length, inode.number, inode.generation,
				   &mutation_time, &result),
			    EXT4_OK);
		}
	}
}

#include "index_edges.h"

/* Device reads of the index nodes, other than the root, that a classification found. */
static struct {
	uint64_t offsets[PROBED_NODES];
	uint32_t count;
	uint32_t reads;
} node_reads;

static enum ext4_result
node_counting_read(void *context, uint64_t offset, void *buffer, size_t length)
{
	uint32_t index;

	for (index = 0; index < node_reads.count; index++) {
		if (node_reads.offsets[index] >= offset &&
		    node_reads.offsets[index] - offset < length) {
			node_reads.reads++;
		}
	}
	return device_read(context, offset, buffer, length);
}

static void
index_node_offsets(struct device *device, struct ext4_fs *fs, struct ext4_inode *inode)
{
	struct ext4_transaction *transaction;
	struct ext4_allocation allocation;
	struct ext4_directory_index tree;
	struct ext4_inode_disk *disk;
	uint64_t physical;
	uint32_t logical;

	EXPECT(ext4_transaction_begin(fs->journal, ext4_journal_credits(fs->journal), &transaction),
	    EXT4_OK);
	EXPECT(ext4_edit_inode(fs, transaction, inode->number, inode->generation, &disk, inode),
	    EXT4_OK);
	EXPECT(ext4_allocation_init(&allocation, fs, transaction, inode), EXT4_OK);
	EXPECT(ext4_index_open(&allocation, inode, disk, &tree, true), EXT4_OK);
	node_reads.count = 0;
	for (logical = 0; logical < tree.blocks; logical++) {
		if (tree.ranges[logical].kind == EXT4_INDEX_NODE) {
			CHECK(node_reads.count < PROBED_NODES);
			EXPECT(ext4_map_block(fs, inode, logical, &physical), EXT4_OK);
			node_reads.offsets[node_reads.count++] = physical * device->block_size;
		}
	}
	ext4_index_close(&tree);
	ext4_allocation_destroy(&allocation);
	ext4_transaction_cancel(transaction);
}

/* A mount classifies an index at its first change and remembers it: that change
 * reads every index node before probing its path, while later ones, including
 * splits, read only the nodes of one probed path. */
static void
probed_links(struct ext4_fs *fs, const struct ext4_inode *parent, const struct ext4_inode *hello,
    uint32_t first_name, uint8_t levels)
{
	struct ext4_inode result;
	uint8_t name[EXT4_NAME_MAX + 1];
	uint64_t size = parent->size;
	uint32_t index;
	uint32_t first = 0;
	uint32_t most = 0;

	fs->environment.read = node_counting_read;
	for (index = 0; index < PROBED_LINKS; index++) {
		filename(name, first_name + index);
		node_reads.reads = 0;
		EXPECT(ext4_link(fs, parent->number, parent->generation, name, EXT4_NAME_MAX,
			   hello->number, hello->generation, &mutation_time, &result),
		    EXT4_OK);
		if (index == 0) {
			first = node_reads.reads;
		} else if (node_reads.reads > most) {
			most = node_reads.reads;
		}
	}
	fs->environment.read = device_read;
	EXPECT(ext4_get_inode(fs, parent->number, &result), EXT4_OK);
	/* Trees with index nodes split leaves during these links. */
	CHECK((levels == 0 || result.size > size) && first >= node_reads.count + levels &&
	    most <= PROBED_PATH_READS * levels);
	printf("PASS indexed changes read nodes=%u first=%u later<=%u after one classification\n",
	    node_reads.count, first, most);
}

/* An index node damaged before the mount and off a change's probed path still fails
 * that change, the index's first in the mount, which classifies every block. */
static struct ext4_fs *
off_path_damage(struct device *device, struct ext4_fs *fs, struct ext4_inode *parent,
    const struct ext4_inode *hello, uint32_t ordinal)
{
	struct ext4_transaction *transaction;
	struct ext4_allocation allocation;
	struct ext4_directory_index tree;
	struct ext4_inode_disk *disk;
	struct ext4_inode result;
	struct ext4_name_hash hash;
	uint8_t name[EXT4_NAME_MAX + 1];
	uint64_t physical = 0;
	uint64_t offset;
	uint32_t logical;
	uint32_t writes;

	filename(name, ordinal);
	EXPECT(ext4_transaction_begin(fs->journal, ext4_journal_credits(fs->journal), &transaction),
	    EXT4_OK);
	EXPECT(ext4_edit_inode(fs, transaction, parent->number, parent->generation, &disk, parent),
	    EXT4_OK);
	EXPECT(ext4_allocation_init(&allocation, fs, transaction, parent), EXT4_OK);
	EXPECT(ext4_index_open(&allocation, parent, disk, &tree, true), EXT4_OK);
	EXPECT(ext4_directory_hash(tree.version, tree.seed, name, EXT4_NAME_MAX, &hash), EXT4_OK);
	for (logical = 0; logical < tree.blocks && physical == 0; logical++) {
		if (tree.ranges[logical].kind == EXT4_INDEX_NODE &&
		    !ext4_index_contains(&tree.ranges[logical], hash.major)) {
			EXPECT(ext4_map_block(fs, parent, logical, &physical), EXT4_OK);
		}
	}
	ext4_index_close(&tree);
	ext4_allocation_destroy(&allocation);
	ext4_transaction_cancel(transaction);
	CHECK(physical != 0);
	EXPECT(ext4_sync(fs), EXT4_OK);
	ext4_unmount(fs);
	/* The node's limit no longer matches its block size. */
	offset = physical * device->block_size + sizeof(struct ext4_dir_header_disk) +
	    offsetof(struct ext4_dx_count_disk, limit);
	device->cache[offset] ^= 1;
	EXPECT(ext4_mount_writable(&device->environment, &device->writer, &fs), EXT4_OK);
	writes = device->writes;
	EXPECT(ext4_link(fs, parent->number, parent->generation, name, EXT4_NAME_MAX, hello->number,
		   hello->generation, &mutation_time, &result),
	    EXT4_CORRUPT);
	CHECK(device->writes == writes && !fs->aborted);
	ext4_unmount(fs);
	device->cache[offset] ^= 1;
	EXPECT(ext4_mount_writable(&device->environment, &device->writer, &fs), EXT4_OK);
	EXPECT(ext4_get_inode(fs, parent->number, parent), EXT4_OK);
	puts("PASS off-path index damage fails the first change in a mount without writes");
	return fs;
}

static void
functional(struct device *device, const char *exports, const char *path)
{
	struct ext4_fs *fs;
	struct ext4_inode root;
	struct ext4_inode parent;
	struct ext4_inode peer;
	struct ext4_inode hello;
	struct ext4_inode container;
	struct ext4_inode result;
	struct ext4_inode_hold *hold;
	struct ext4_inode_update update = attributes();
	uint8_t name[EXT4_NAME_MAX + 1];
	uint8_t target[EXT4_INODE_BLOCK_BYTES];
	uint8_t *clean = malloc(device->size);
	uint32_t added = device->block_size == EXT4_MIN_BLOCK_SIZE ? ADDED_SMALL : ADDED_LARGE;
	uint32_t nodes;
	uint32_t index;
	uint64_t before;
	uint8_t levels;
	uint32_t writes;

	CHECK(clean != NULL);
	EXPECT(ext4_mount_writable(&device->environment, &device->writer, &fs), EXT4_OK);
	if (fs->info.feature_incompat & EXT4_FEATURE_INCOMPAT_LARGEDIR) {
		added = ADDED_DEEP;
	}
	EXPECT(ext4_get_inode(fs, EXT4_ROOT_INODE, &root), EXT4_OK);
	parent = lookup(fs, &root, "indexed");
	peer = lookup(fs, &root, "peer");
	hello = lookup(fs, &root, "hello.txt");
	before = parent.size;
	for (index = 0; index < added; index++) {
		filename(name, index);
		EXPECT(ext4_link(fs, parent.number, parent.generation, name, EXT4_NAME_MAX,
			   hello.number, hello.generation, &mutation_time, &result),
		    EXT4_OK);
	}
	tree_shape(fs, &parent, &nodes, &levels);
	CHECK(parent.size > before && (parent.flags & EXT4_INODE_INDEX));
	if (device->block_size == EXT4_MIN_BLOCK_SIZE) {
		CHECK(levels == ext4_index_max_levels(fs) && nodes > 2);
	}
	verify_names(fs, &parent, added, hello.number);
	index_node_offsets(device, fs, &parent);
	CHECK(node_reads.count == nodes);
	EXPECT(ext4_sync(fs), EXT4_OK);
	ext4_unmount(fs);
	EXPECT(ext4_mount_writable(&device->environment, &device->writer, &fs), EXT4_OK);
	EXPECT(ext4_get_inode(fs, parent.number, &parent), EXT4_OK);
	probed_links(fs, &parent, &hello, added, levels);
	added += PROBED_LINKS;
	if (nodes > 1) {
		fs = off_path_damage(device, fs, &parent, &hello, added);
	}
	EXPECT(ext4_get_inode(fs, parent.number, &parent), EXT4_OK);
	EXPECT(ext4_get_inode(fs, hello.number, &hello), EXT4_OK);
	verify_names(fs, &parent, added, hello.number);
	writes = device->writes;
	filename(name, 0);
	EXPECT(ext4_link(fs, parent.number, parent.generation, name, EXT4_NAME_MAX, hello.number,
		   hello.generation, &mutation_time, &result),
	    EXT4_EXISTS);
	CHECK(device->writes == writes);
	move(fs, &parent, name, &peer, (const uint8_t *)"moved", EXT4_RENAME_NOREPLACE);
	move(fs, &peer, (const uint8_t *)"moved", &parent, name, 0);
	EXPECT(ext4_create(fs, parent.number, parent.generation, (const uint8_t *)"created", 7,
		   &update, &mutation_time, &result),
	    EXT4_OK);
	EXPECT(ext4_create(fs, peer.number, peer.generation, (const uint8_t *)"peer-file", 9,
		   &update, &mutation_time, &result),
	    EXT4_OK);
	move(fs, &parent, (const uint8_t *)"created", &peer, (const uint8_t *)"peer-file",
	    EXT4_RENAME_EXCHANGE);
	EXPECT(ext4_mkdir(fs, parent.number, parent.generation, (const uint8_t *)"nested", 6,
		   &update, &mutation_time, &result),
	    EXT4_OK);
	memset(target, 'a', sizeof(target));
	EXPECT(ext4_symlink(fs, parent.number, parent.generation, (const uint8_t *)"short-link", 10,
		   (const uint8_t *)"../hello.txt", 12, &update, &mutation_time, &result),
	    EXT4_OK);
	EXPECT(ext4_symlink(fs, parent.number, parent.generation, (const uint8_t *)"long-link", 9,
		   target, sizeof(target), &update, &mutation_time, &result),
	    EXT4_OK);
	EXPECT(ext4_mkdir(fs, root.number, root.generation, (const uint8_t *)"container", 9,
		   &update, &mutation_time, &container),
	    EXT4_OK);
	move(fs, &root, (const uint8_t *)"indexed", &container, (const uint8_t *)"moved", 0);
	move(fs, &container, (const uint8_t *)"moved", &root, (const uint8_t *)"indexed", 0);
	move(fs, &root, (const uint8_t *)"indexed", &root, (const uint8_t *)"peer",
	    EXT4_RENAME_EXCHANGE);
	move(fs, &root, (const uint8_t *)"indexed", &root, (const uint8_t *)"peer",
	    EXT4_RENAME_EXCHANGE);
	EXPECT(ext4_get_inode(fs, peer.number, &peer), EXT4_OK);
	empty_directory(fs, &peer);
	EXPECT(ext4_hold_inode(fs, peer.number, peer.generation, &hold), EXT4_OK);
	EXPECT(ext4_rmdir(fs, root.number, root.generation, (const uint8_t *)"peer", 4, peer.number,
		   peer.generation, &mutation_time, &result),
	    EXT4_OK);
	EXPECT(ext4_refresh_inode(hold, &result), EXT4_OK);
	CHECK(result.links == 0 && result.size == 0);
	EXPECT(ext4_release_inode(hold), EXT4_OK);
	EXPECT(ext4_get_inode(fs, parent.number, &parent), EXT4_OK);
	verify_names(fs, &parent, added, hello.number);
	EXPECT(ext4_sync(fs), EXT4_OK);
	memcpy(clean, device->stable, device->size);
	ext4_unmount(fs);
	CHECK(storage_recover(device, clean, true) && device->writes == 0);
	storage_export(device, exports, path, "indexed-written-");
	free(clean);
	printf(
	    "PASS indexed namespace added=%u nodes=%u levels=%u size_before=%llu size_after=%llu\n",
	    added, nodes, levels, (unsigned long long)before, (unsigned long long)parent.size);
}

enum split_kind { SPLIT_LEAF, GROW_ROOT, SPLIT_NODE, SPLIT_KIND_COUNT };

static const char *const split_names[SPLIT_KIND_COUNT] = { "leaf", "root", "node" };

struct shape {
	uint64_t size;
	uint16_t nodes;
	uint8_t levels;
};

struct trace {
	uint32_t allocations;
	uint32_t reads;
	uint32_t events;
	uint32_t commit_event;
	bool committed;
};

static struct shape
shape(struct ext4_fs *fs, struct ext4_inode *parent)
{
	struct {
		struct ext4_dx_root_prefix_disk root;
		struct ext4_dx_count_disk counts;
	} prefix;
	struct shape result;
	size_t completed;

	EXPECT(ext4_get_inode(fs, parent->number, parent), EXT4_OK);
	EXPECT(ext4_read(fs, parent, 0, &prefix, sizeof(prefix), &completed), EXT4_OK);
	CHECK(completed == sizeof(prefix) &&
	    prefix.root.indirect_levels <= EXT4_DX_MAX_INDIRECT_LEVELS);
	result.size = parent->size;
	result.levels = prefix.root.indirect_levels;
	result.nodes = result.levels == 0 ? 0 : ext4_le16(&prefix.counts.count);
	return result;
}

static struct ext4_fs *
mount_index(struct device *device, struct ext4_inode *parent, struct ext4_inode *hello)
{
	struct ext4_fs *fs;
	struct ext4_inode root;

	EXPECT(ext4_mount_writable(&device->environment, &device->writer, &fs), EXT4_OK);
	EXPECT(ext4_get_inode(fs, EXT4_ROOT_INODE, &root), EXT4_OK);
	*parent = lookup(fs, &root, "indexed");
	*hello = lookup(fs, &root, "hello.txt");
	return fs;
}

static void
find_splits(struct device *device, uint32_t points[SPLIT_KIND_COUNT])
{
	struct ext4_fs *fs;
	struct ext4_inode parent;
	struct ext4_inode hello;
	struct ext4_inode result;
	struct shape previous;
	struct shape current;
	uint8_t name[EXT4_NAME_MAX + 1];
	uint32_t limit = device->block_size == EXT4_MIN_BLOCK_SIZE ? ADDED_SMALL : ADDED_LARGE;
	uint32_t index;
	unsigned int kind;
	bool deep;

	device_reset(device, device->base);
	fs = mount_index(device, &parent, &hello);
	previous = shape(fs, &parent);
	deep = previous.levels != 0;
	for (kind = 0; kind < SPLIT_KIND_COUNT; kind++) {
		points[kind] = UINT32_MAX;
	}
	for (index = 0; index < limit; index++) {
		filename(name, index);
		EXPECT(ext4_link(fs, parent.number, parent.generation, name, EXT4_NAME_MAX,
			   hello.number, hello.generation, &mutation_time, &result),
		    EXT4_OK);
		current = shape(fs, &parent);
		if (current.size > previous.size) {
			kind = current.levels > previous.levels ? GROW_ROOT
			    : current.nodes > previous.nodes	? SPLIT_NODE
								: SPLIT_LEAF;
			if (points[kind] == UINT32_MAX) {
				points[kind] = index;
			}
		}
		previous = current;
		if (points[SPLIT_LEAF] != UINT32_MAX &&
		    (device->block_size != EXT4_MIN_BLOCK_SIZE ||
			(points[SPLIT_NODE] != UINT32_MAX &&
			    (deep || points[GROW_ROOT] != UINT32_MAX)))) {
			break;
		}
	}
	CHECK(points[SPLIT_LEAF] != UINT32_MAX);
	if (device->block_size == EXT4_MIN_BLOCK_SIZE) {
		CHECK(
		    points[SPLIT_NODE] != UINT32_MAX && (deep || points[GROW_ROOT] != UINT32_MAX));
	}
	ext4_unmount(fs);
	CHECK(device->live == 0);
}

static void
prepare_split(struct device *device, uint32_t point)
{
	struct ext4_fs *fs;
	struct ext4_inode parent;
	struct ext4_inode hello;
	struct ext4_inode result;
	uint8_t name[EXT4_NAME_MAX + 1];
	uint32_t index;

	device_reset(device, device->base);
	fs = mount_index(device, &parent, &hello);
	for (index = 0; index < point; index++) {
		filename(name, index);
		EXPECT(ext4_link(fs, parent.number, parent.generation, name, EXT4_NAME_MAX,
			   hello.number, hello.generation, &mutation_time, &result),
		    EXT4_OK);
	}
	EXPECT(ext4_sync(fs), EXT4_OK);
	ext4_unmount(fs);
	memcpy(device->base, device->stable, device->size);
}

static enum ext4_result
split_attempt(struct device *device, uint32_t ordinal, unsigned int fault, uint32_t point,
    unsigned int survival, bool partial, struct trace *trace)
{
	struct ext4_fs *fs;
	struct ext4_inode parent;
	struct ext4_inode hello;
	struct ext4_inode result;
	struct ext4_inode untouched;
	uint8_t name[EXT4_NAME_MAX + 1];
	uint32_t allocations;
	uint32_t reads;
	uint32_t events;
	enum ext4_result error;

	fs = mount_index(device, &parent, &hello);
	filename(name, ordinal);
	allocations = device->allocations;
	reads = device->reads;
	events = device->events;
	memset(&result, 0xa5, sizeof(result));
	untouched = result;
	device->survival = survival;
	device->partial = partial;
	if (fault == 1) {
		device->fail_allocation = allocations + point;
	} else if (fault == 2) {
		device->fail_read = reads + point;
	} else if (fault == 3) {
		device->stop_at = events + point;
	}
	error = ext4_link(fs, parent.number, parent.generation, name, EXT4_NAME_MAX, hello.number,
	    hello.generation, &mutation_time, &result);
	if (error == EXT4_OK) {
		CHECK(result.number == hello.number && result.generation == hello.generation &&
		    result.links == hello.links + 1);
		error = ext4_sync(fs);
	} else {
		CHECK(memcmp(&result, &untouched, sizeof(result)) == 0);
	}
	trace->allocations = device->allocations - allocations;
	trace->reads = device->reads - reads;
	trace->events = device->events - events;
	trace->commit_event = device->commit_barrier - events;
	trace->committed = device->intent_durable;
	if (error != EXT4_OK) {
		if (device->writes != 0) {
			CHECK(fs->aborted);
			EXPECT(ext4_get_inode(fs, parent.number, &result), EXT4_RECOVERY_REQUIRED);
		} else {
			CHECK(memcmp(device->cache, device->base, device->size) == 0);
			CHECK(!fs->aborted || (error == EXT4_IO && fs->journal->aborted));
		}
	}
	ext4_unmount(fs);
	CHECK(device->live == 0);
	return error;
}

static void
split_capacity_guards(struct device *device, uint32_t ordinal)
{
	struct ext4_fs *fs;
	struct ext4_inode parent;
	struct ext4_inode hello;
	struct ext4_inode journal;
	struct ext4_inode result;
	struct ext4_inode untouched;
	struct ext4_jbd_super *journal_super;
	struct ext4_super_disk *super;
	uint8_t name[EXT4_NAME_MAX + 1];
	uint8_t *before = malloc(device->size);
	uint64_t physical;
	uint64_t free_blocks;
	uint32_t free_inodes;
	unsigned int kind;

	CHECK(before != NULL);
	filename(name, ordinal);
	for (kind = 0; kind < 2; kind++) {
		device_reset(device, device->base);
		if (kind == 0) {
			EXPECT(ext4_mount(&device->environment, &fs), EXT4_OK);
			EXPECT(ext4_get_inode(fs, fs->journal_inode, &journal), EXT4_OK);
			EXPECT(ext4_map_block(fs, &journal, 0, &physical), EXT4_OK);
			ext4_unmount(fs);
			journal_super = (struct ext4_jbd_super *)(device->cache +
			    physical * device->block_size);
			/* Preserve valid journal geometry but bound the snapshot capacity
			 * below that needed for a leaf/index/allocation transition. */
			ext4_encode_be32(&journal_super->max_length,
			    ext4_be32(&journal_super->first) + 2 * INDEX_SMALL_JOURNAL_CREDITS + 2);
			if (ext4_be32(&journal_super->feature_incompat) &
			    (EXT4_JBD_CSUM_V2 | EXT4_JBD_CSUM_V3)) {
				ext4_encode_be32(&journal_super->checksum, 0);
				ext4_encode_be32(&journal_super->checksum,
				    ext4_crc32c(UINT32_MAX, journal_super, sizeof(*journal_super)));
			}
		}
		fs = mount_index(device, &parent, &hello);
		free_blocks = fs->info.free_blocks;
		free_inodes = fs->info.free_inodes;
		if (kind == 0) {
			CHECK(ext4_journal_credits(fs->journal) == INDEX_SMALL_JOURNAL_CREDITS);
		} else {
			super = (struct ext4_super_disk *)(device->cache + EXT4_SUPER_OFFSET);
			ext4_encode32(&super->reserved_blocks_lo, (uint32_t)free_blocks);
			ext4_encode32(&super->reserved_blocks_hi, (uint32_t)(free_blocks >> 32));
			if (device->metadata_checksum) {
				ext4_encode32(&super->checksum,
				    ext4_crc32c(UINT32_MAX, super,
					offsetof(struct ext4_super_disk, checksum)));
			}
		}
		memcpy(before, device->cache, device->size);
		memset(&result, 0xa5, sizeof(result));
		untouched = result;
		EXPECT(ext4_link(fs, parent.number, parent.generation, name, EXT4_NAME_MAX,
			   hello.number, hello.generation, &mutation_time, &result),
		    kind == 0 ? EXT4_RANGE : EXT4_NO_SPACE);
		CHECK(memcmp(&result, &untouched, sizeof(result)) == 0 && !fs->aborted &&
		    device->writes == 0 && memcmp(before, device->cache, device->size) == 0 &&
		    fs->info.free_blocks == free_blocks && fs->info.free_inodes == free_inodes);
		EXPECT(ext4_lookup(fs, &parent, name, EXT4_NAME_MAX, &result), EXT4_NOT_FOUND);
		ext4_unmount(fs);
		CHECK(device->live == 0);
	}
	device_reset(device, device->base);
	free(before);
	puts("PASS index split credit and reserved-space exhaustion without writes or leaks");
}

static void
root_capacity(struct device *device, const char *path, const char *exports)
{
	struct ext4_fs *fs;
	struct ext4_inode parent;
	struct ext4_inode hello;
	struct ext4_inode result;
	struct ext4_inode untouched;
	struct ext4_inode before;
	struct edge_view view;
	struct ext4_directory_slot slot;
	struct ext4_name_hash hash;
	uint8_t name[EXT4_NAME_MAX + 1];
	const uint8_t short_name[] = "capacity-reuse";
	uint64_t free_blocks;
	uint32_t free_inodes;
	uint32_t ordinal;
	uint32_t logical;
	uint32_t leaf = UINT32_MAX;
	uint32_t root_limit;
	uint32_t node_limit;
	uint32_t tail;

	device_reset(device, device->base);
	CHECK(device->block_size == EXT4_MIN_BLOCK_SIZE);
	fs = mount_index(device, &parent, &hello);
	edge_open(fs, &parent, &view);
	tail = fs->metadata_checksum ? sizeof(struct ext4_dx_tail_disk) : 0;
	root_limit = (device->block_size - sizeof(struct ext4_dx_root_prefix_disk) - tail) /
	    sizeof(struct ext4_dx_entry_disk);
	node_limit = (device->block_size - sizeof(struct ext4_dir_header_disk) - tail) /
	    sizeof(struct ext4_dx_entry_disk);
	CHECK(view.tree.levels == ext4_index_max_levels(fs) &&
	    view.tree.ranges[0].count == root_limit);
	for (ordinal = 0; ordinal < ADDED_SMALL && leaf == UINT32_MAX; ordinal++) {
		filename(name, ordinal);
		EXPECT(ext4_directory_hash(
			   view.tree.version, view.tree.seed, name, EXT4_NAME_MAX, &hash),
		    EXT4_OK);
		for (logical = 1; logical < view.tree.blocks; logical++) {
			if (view.tree.ranges[logical].kind == EXT4_INDEX_LEAF &&
			    ext4_index_contains(&view.tree.ranges[logical], hash.major) &&
			    view.tree.ranges[view.tree.ranges[logical].parent].count ==
				node_limit &&
			    (view.tree.levels == EXT4_DX_LEGACY_INDIRECT_LEVELS ||
				view.tree
					.ranges[view.tree.ranges[view.tree.ranges[logical].parent]
						.parent]
					.count == node_limit)) {
				leaf = logical;
				break;
			}
		}
	}
	CHECK(leaf != UINT32_MAX);
	edge_close(&view);
	slot = edge_slot(fs, &parent, name, EXT4_NAME_MAX, EXT4_DIRECTORY_INSERT);
	CHECK(slot.repack && slot.logical == leaf);
	free_blocks = fs->info.free_blocks;
	free_inodes = fs->info.free_inodes;
	before = parent;
	memset(&result, 0xa5, sizeof(result));
	untouched = result;
	EXPECT(ext4_link(fs, parent.number, parent.generation, name, EXT4_NAME_MAX, hello.number,
		   hello.generation, &mutation_time, &result),
	    EXT4_UNSUPPORTED);
	CHECK(memcmp(&result, &untouched, sizeof(result)) == 0 && !fs->aborted &&
	    device->writes == 0 && memcmp(device->base, device->cache, device->size) == 0 &&
	    fs->info.free_blocks == free_blocks && fs->info.free_inodes == free_inodes);
	edge_link(fs, &parent, &hello, short_name, sizeof(short_name) - 1);
	EXPECT(ext4_get_inode(fs, parent.number, &parent), EXT4_OK);
	CHECK(parent.size == before.size && parent.blocks_512 == before.blocks_512 &&
	    fs->info.free_blocks == free_blocks && fs->info.free_inodes == free_inodes);
	result = lookup(fs, &parent, (const char *)short_name);
	CHECK(result.number == hello.number && result.generation == hello.generation);
	EXPECT(ext4_lookup(fs, &parent, name, EXT4_NAME_MAX, &result), EXT4_NOT_FOUND);
	EXPECT(ext4_sync(fs), EXT4_OK);
	storage_export(device, exports, path, "indexed-capacity-reused-");
	edge_finish(device, fs);
	printf("PASS full index root count=%u rejects height growth without writes and reuses "
	       "existing leaf space\n",
	    root_limit);
}

static struct {
	uint64_t offset;
	uint32_t hit;
	uint32_t at;
	uint32_t seed;
	uint32_t logical;
	unsigned int kind;
} changing_read;

static enum ext4_result
changing_device_read(void *context, uint64_t offset, void *buffer, size_t length)
{
	struct device *device = context;
	struct ext4_dir_header_disk *header = buffer;
	struct ext4_dir_tail_disk *tail;
	struct ext4_dx_entry_disk *entries;
	struct ext4_dx_count_disk *counts;
	struct ext4_dx_tail_disk *index_tail;
	uint32_t base;
	uint32_t checksum;
	enum ext4_result error;

	error = device_read(context, offset, buffer, length);
	if (error != EXT4_OK || offset != changing_read.offset || length != device->block_size ||
	    ++changing_read.hit != changing_read.at) {
		return error;
	}
	if (changing_read.kind == 0) {
		ext4_encode16(&header->record_length, 0);
		if (device->metadata_checksum) {
			tail = (struct ext4_dir_tail_disk *)((uint8_t *)buffer + length -
			    sizeof(*tail));
			ext4_encode32(&tail->checksum,
			    ext4_crc32c(changing_read.seed, buffer, length - sizeof(*tail)));
		}
	} else {
		base = changing_read.logical == 0 ? sizeof(struct ext4_dx_root_prefix_disk)
						  : sizeof(*header);
		entries = (struct ext4_dx_entry_disk *)((uint8_t *)buffer + base);
		counts = (struct ext4_dx_count_disk *)entries;
		if (changing_read.kind == 1) {
			ext4_encode16(&counts->count, UINT16_MAX);
		} else {
			CHECK(ext4_le16(&counts->count) >= 2);
			entries[1].block = entries[0].block;
			if (device->metadata_checksum) {
				index_tail = (struct ext4_dx_tail_disk *)((uint8_t *)buffer + base +
				    ext4_le16(&counts->limit) * sizeof(*entries));
				ext4_encode32(&index_tail->checksum, 0);
				checksum = ext4_crc32c(changing_read.seed, buffer,
				    base + ext4_le16(&counts->count) * sizeof(*entries));
				checksum = ext4_crc32c(checksum, index_tail, sizeof(*index_tail));
				ext4_encode32(&index_tail->checksum, checksum);
			}
		}
	}
	return EXT4_OK;
}

static void
changing_media_guards(struct device *device, uint32_t ordinal, bool conversion)
{
	struct ext4_fs *fs;
	struct ext4_inode parent;
	struct ext4_inode hello;
	struct ext4_inode result;
	struct ext4_inode untouched;
	struct ext4_inode_disk *disk;
	struct ext4_transaction *transaction;
	struct ext4_allocation allocation;
	struct ext4_directory_index tree;
	struct ext4_directory_slot slot;
	struct ext4_directory_request request = { 0 };
	uint8_t name[EXT4_NAME_MAX + 1];
	uint64_t physical;
	unsigned int kind;

	filename(name, ordinal);
	for (kind = 0; kind < (conversion ? 1U : 3U); kind++) {
		device_reset(device, device->base);
		fs = mount_index(device, &parent, &hello);
		EXPECT(ext4_transaction_begin(
			   fs->journal, ext4_journal_credits(fs->journal), &transaction),
		    EXT4_OK);
		EXPECT(ext4_edit_inode(
			   fs, transaction, parent.number, parent.generation, &disk, &parent),
		    EXT4_OK);
		EXPECT(ext4_allocation_init(&allocation, fs, transaction, &parent), EXT4_OK);
		EXPECT(ext4_directory_request_open(fs, &parent, name, EXT4_NAME_MAX,
			   EXT4_NAME_REQUIRE_KEY, NULL, &request), EXT4_OK);
		EXPECT(ext4_directory_scan(&allocation, &parent, disk, &request,
			   EXT4_DIRECTORY_INSERT, 0, &slot),
		    EXT4_OK);
		ext4_directory_request_close(fs, &request);
		CHECK(conversion ? slot.physical == 0 : slot.repack);
		if (!conversion) {
			EXPECT(ext4_index_open(&allocation, &parent, disk, &tree, true), EXT4_OK);
		}
		memset(&changing_read, 0, sizeof(changing_read));
		changing_read.kind = kind;
		changing_read.seed = ext4_inode_seed(fs, &parent);
		changing_read.logical = conversion ? 0
		    : kind == 0			   ? slot.logical
						   : tree.ranges[slot.logical].parent;
		if (conversion) {
			EXPECT(ext4_map_block(fs, &parent, 0, &physical), EXT4_OK);
		} else {
			EXPECT(ext4_index_read(&tree, changing_read.logical, &physical), EXT4_OK);
		}
		changing_read.offset = physical * device->block_size;
		/* Leaf scan then repack; index graph and root directory scan, graph
		 * reopening for split, then enrollment of the edited node snapshot. */
		changing_read.at = kind == 0 ? 2 : changing_read.logical == 0 ? 4 : 3;
		if (!conversion) {
			ext4_index_close(&tree);
		}
		ext4_allocation_destroy(&allocation);
		ext4_transaction_cancel(transaction);
		fs->environment.read = changing_device_read;
		memset(&result, 0xa5, sizeof(result));
		untouched = result;
		EXPECT(ext4_link(fs, parent.number, parent.generation, name, EXT4_NAME_MAX,
			   hello.number, hello.generation, &mutation_time, &result),
		    EXT4_CORRUPT);
		CHECK(changing_read.hit >= changing_read.at && !fs->aborted &&
		    device->writes == 0 && memcmp(device->base, device->cache, device->size) == 0 &&
		    memcmp(&result, &untouched, sizeof(result)) == 0);
		fs->environment.read = device_read;
		ext4_unmount(fs);
		CHECK(device->live == 0);
	}
	puts("PASS changing-media leaf lengths, index counts and child aliases reject without "
	     "writes");
}

static uint32_t
prepare_index_creation(struct device *device)
{
	struct ext4_fs *fs;
	struct ext4_transaction *transaction;
	struct ext4_allocation allocation;
	struct ext4_inode_disk *disk;
	struct ext4_inode root;
	struct ext4_inode parent;
	struct ext4_inode peer;
	struct ext4_inode hello;
	struct ext4_inode result;
	struct ext4_inode_update update = attributes();
	struct ext4_directory_slot slot;
	struct ext4_directory_request request = { 0 };
	uint8_t name[EXT4_NAME_MAX + 1];
	uint32_t ordinal;

	device_reset(device, device->base);
	EXPECT(ext4_mount_writable(&device->environment, &device->writer, &fs), EXT4_OK);
	EXPECT(ext4_get_inode(fs, EXT4_ROOT_INODE, &root), EXT4_OK);
	hello = lookup(fs, &root, "hello.txt");
	EXPECT(ext4_mkdir(fs, root.number, root.generation, (const uint8_t *)"indexed", 7, &update,
		   &mutation_time, &parent),
	    EXT4_OK);
	EXPECT(ext4_mkdir(fs, root.number, root.generation, (const uint8_t *)"peer", 4, &update,
		   &mutation_time, &peer),
	    EXT4_OK);
	EXPECT(ext4_mkdir(fs, parent.number, parent.generation, (const uint8_t *)"child", 5,
		   &update, &mutation_time, &result),
	    EXT4_OK);
	EXPECT(ext4_mkdir(fs, peer.number, peer.generation, (const uint8_t *)"child", 5, &update,
		   &mutation_time, &result),
	    EXT4_OK);
	for (ordinal = 0; ordinal < device->block_size; ordinal++) {
		EXPECT(ext4_get_inode(fs, parent.number, &parent), EXT4_OK);
		CHECK(parent.size == device->block_size && !(parent.flags & EXT4_INODE_INDEX));
		filename(name, ordinal);
		EXPECT(ext4_transaction_begin(
			   fs->journal, ext4_journal_credits(fs->journal), &transaction),
		    EXT4_OK);
		EXPECT(ext4_edit_inode(
			   fs, transaction, parent.number, parent.generation, &disk, &parent),
		    EXT4_OK);
		EXPECT(ext4_allocation_init(&allocation, fs, transaction, &parent), EXT4_OK);
		EXPECT(ext4_directory_request_open(fs, &parent, name, EXT4_NAME_MAX,
			   EXT4_NAME_REQUIRE_KEY, NULL, &request), EXT4_OK);
		EXPECT(ext4_directory_scan(&allocation, &parent, disk, &request,
			   EXT4_DIRECTORY_INSERT, 0, &slot),
		    EXT4_OK);
		ext4_directory_request_close(fs, &request);
		ext4_allocation_destroy(&allocation);
		ext4_transaction_cancel(transaction);
		if (slot.physical == 0) {
			break;
		}
		EXPECT(ext4_link(fs, parent.number, parent.generation, name, EXT4_NAME_MAX,
			   hello.number, hello.generation, &mutation_time, &result),
		    EXT4_OK);
	}
	CHECK(ordinal < device->block_size);
	EXPECT(ext4_sync(fs), EXT4_OK);
	ext4_unmount(fs);
	CHECK(device->live == 0);
	memcpy(device->base, device->stable, device->size);
	return ordinal;
}

static void
index_creation_shape(struct device *device, uint32_t added)
{
	struct ext4_fs *fs;
	struct ext4_inode parent;
	struct ext4_inode hello;
	struct ext4_inode result;
	uint32_t nodes;
	uint8_t levels;
	uint8_t name[EXT4_NAME_MAX + 1];
	uint32_t ordinal;

	fs = mount_index(device, &parent, &hello);
	tree_shape(fs, &parent, &nodes, &levels);
	CHECK((parent.flags & EXT4_INODE_INDEX) && levels == 0 && nodes == 0);
	CHECK(parent.size == 2U * device->block_size || parent.size == 3U * device->block_size);
	verify_names(fs, &parent, added, hello.number);
	for (ordinal = 0; ordinal < added; ordinal++) {
		filename(name, ordinal);
		EXPECT(ext4_lookup(fs, &parent, name, EXT4_NAME_MAX, &result), EXT4_OK);
		CHECK(result.number == hello.number);
	}
	result = lookup(fs, &parent, "child");
	CHECK((result.mode & EXT4_MODE_TYPE) == EXT4_MODE_DIRECTORY);
	result = lookup(fs, &parent, "..");
	CHECK(result.number == EXT4_ROOT_INODE);
	ext4_unmount(fs);
	CHECK(device->live == 0);
	printf("PASS automatic directory index: blocks=%llu names=%u\n",
	    (unsigned long long)(parent.size / device->block_size), added);
}

static uint32_t
large_split_point(struct device *device, const char *kind, uint32_t *blocks, uint32_t *nodes)
{
	struct ext4_fs *fs;
	struct ext4_inode parent;
	struct ext4_inode hello;
	struct edge_view view;
	struct ext4_directory_slot slot;
	struct ext4_name_hash hash;
	struct ext4_index_range *range;
	uint8_t name[EXT4_NAME_MAX + 1];
	uint32_t tail;
	uint32_t root_limit;
	uint32_t node_limit;
	uint32_t logical;
	uint32_t ordinal;
	uint32_t selected = UINT32_MAX;
	bool grow = strcmp(kind, "grow") == 0;

	device_reset(device, device->base);
	fs = mount_index(device, &parent, &hello);
	CHECK(fs->info.feature_incompat & EXT4_FEATURE_INCOMPAT_LARGEDIR);
	edge_open(fs, &parent, &view);
	tail = fs->metadata_checksum ? sizeof(struct ext4_dx_tail_disk) : 0;
	root_limit = (device->block_size - sizeof(struct ext4_dx_root_prefix_disk) - tail) /
	    sizeof(struct ext4_dx_entry_disk);
	node_limit = (device->block_size - sizeof(struct ext4_dir_header_disk) - tail) /
	    sizeof(struct ext4_dx_entry_disk);
	CHECK(view.tree.levels ==
	    (grow ? EXT4_DX_LEGACY_INDIRECT_LEVELS : EXT4_DX_MAX_INDIRECT_LEVELS));
	CHECK(grow ? view.tree.ranges[0].count == root_limit
		   : view.tree.ranges[0].count < root_limit);
	*blocks = view.tree.blocks;
	*nodes = 0;
	for (logical = 1; logical < view.tree.blocks; logical++) {
		*nodes += view.tree.ranges[logical].kind == EXT4_INDEX_NODE;
	}
	for (ordinal = 0; ordinal < EDGE_SEARCH_LIMIT && selected == UINT32_MAX; ordinal++) {
		filename(name, ordinal);
		EXPECT(ext4_directory_hash(
			   view.tree.version, view.tree.seed, name, EXT4_NAME_MAX, &hash),
		    EXT4_OK);
		for (logical = 1; logical < view.tree.blocks; logical++) {
			range = &view.tree.ranges[logical];
			if (range->kind == EXT4_INDEX_LEAF &&
			    ext4_index_contains(range, hash.major) &&
			    view.tree.ranges[range->parent].count == node_limit &&
			    (grow ||
				view.tree.ranges[view.tree.ranges[range->parent].parent].count ==
				    node_limit)) {
				selected = ordinal;
				break;
			}
		}
	}
	CHECK(selected != UINT32_MAX);
	edge_close(&view);
	slot = edge_slot(fs, &parent, name, EXT4_NAME_MAX, EXT4_DIRECTORY_INSERT);
	CHECK(slot.repack);
	ext4_unmount(fs);
	CHECK(device->live == 0 && device->writes == 0);
	return selected;
}

static void
large_split_shape(struct device *device, uint32_t before_blocks, uint32_t before_nodes)
{
	struct ext4_fs *fs;
	struct ext4_inode parent;
	struct ext4_inode hello;
	uint32_t nodes;
	uint8_t levels;

	fs = mount_index(device, &parent, &hello);
	tree_shape(fs, &parent, &nodes, &levels);
	CHECK(levels == EXT4_DX_MAX_INDIRECT_LEVELS && nodes == before_nodes + 2 &&
	    parent.size == (uint64_t)(before_blocks + 3) * device->block_size);
	ext4_unmount(fs);
	CHECK(device->live == 0);
	printf("PASS deep index transition blocks=%u->%u nodes=%u->%u levels=%u\n", before_blocks,
	    before_blocks + 3, before_nodes, nodes, levels);
}

static void
split_faults(struct device *device, const char *path, const char *exports, bool smoke,
    const char *large_kind, bool conversion)
{
	struct trace baseline;
	struct trace trace;
	uint32_t points[SPLIT_KIND_COUNT];
	uint8_t *original = malloc(device->size);
	uint8_t *expected = malloc(device->size);
	uint32_t point;
	uint32_t limit;
	uint32_t recovered;
	uint32_t torn;
	uint32_t before_blocks = 0;
	uint32_t before_nodes = 0;
	unsigned int kind;
	unsigned int fault;
	unsigned int survival;
	unsigned int partial;
	const char *kind_name;
	char prefix[64];

	CHECK(original != NULL && expected != NULL);
	memcpy(original, device->base, device->size);
	if (conversion) {
		points[SPLIT_LEAF] = prepare_index_creation(device);
		points[GROW_ROOT] = points[SPLIT_NODE] = UINT32_MAX;
	} else if (large_kind == NULL) {
		find_splits(device, points);
	} else {
		for (kind = 0; kind < SPLIT_KIND_COUNT; kind++) {
			points[kind] = UINT32_MAX;
		}
		kind = strcmp(large_kind, "grow") == 0 ? GROW_ROOT : SPLIT_NODE;
		points[kind] = large_split_point(device, large_kind, &before_blocks, &before_nodes);
	}
	for (kind = 0; kind < SPLIT_KIND_COUNT; kind++) {
		if (points[kind] == UINT32_MAX) {
			if (conversion) {
				continue;
			}
			printf("SKIP split=%s: this fixture exercises that transition in another "
			       "profile\n",
			    split_names[kind]);
			continue;
		}
		kind_name = conversion ? "create" : split_names[kind];
		if (!conversion) {
			memcpy(device->base, original, device->size);
		}
		if (large_kind == NULL && !conversion) {
			prepare_split(device, points[kind]);
		}
		split_capacity_guards(device, points[kind]);
		changing_media_guards(device, points[kind], conversion);
		device_reset(device, device->base);
		CHECK(snprintf(prefix, sizeof(prefix), "index-before-%s-", kind_name) > 0);
		storage_export(device, exports, path, prefix);
		device_reset(device, device->base);
		EXPECT(split_attempt(device, points[kind], 0, 0, 0, false, &baseline), EXT4_OK);
		if (large_kind != NULL) {
			large_split_shape(device, before_blocks, before_nodes);
		}
		if (conversion) {
			index_creation_shape(device, points[kind] + 1);
		}
		memcpy(expected, device->stable, device->size);
		CHECK(snprintf(prefix, sizeof(prefix), "index-atomic-%s-", kind_name) > 0);
		storage_export(device, exports, path, prefix);
		CHECK(storage_recover(device, expected, true) && device->writes == 0);
		recovered = torn = 0;
		if (!smoke) {
			for (fault = 1; fault <= 2; fault++) {
				limit = fault == 1 ? baseline.allocations : baseline.reads;
				for (point = 1; point <= limit; point++) {
					device_reset(device, device->base);
					EXPECT(split_attempt(device, points[kind], fault, point, 0,
						   false, &trace),
					    fault == 1 ? EXT4_NO_MEMORY : EXT4_IO);
					CHECK(storage_recover(device, expected, trace.committed));
				}
			}
			for (point = 1; point <= baseline.events; point++) {
				for (survival = 0; survival < 3; survival++) {
					for (partial = 0; partial < 2; partial++) {
						device_reset(device, device->base);
						EXPECT(split_attempt(device, points[kind], 3, point,
							   survival, partial != 0, &trace),
						    EXT4_IO);
						CHECK(device->off);
						if (storage_recover(
							device, expected, trace.committed)) {
							recovered++;
						} else {
							torn++;
						}
					}
				}
			}
			printf(
			    "PASS split faults kind=%s ordinal=%u allocations=%u reads=%u cuts=%u "
			    "recovered=%u torn_super_fail_closed=%u\n",
			    kind_name, points[kind], baseline.allocations, baseline.reads,
			    baseline.events * 6, recovered, torn);
		}
		if (exports != NULL) {
			CHECK(baseline.commit_event > 1 && baseline.commit_event < baseline.events);
			device_reset(device, device->base);
			EXPECT(split_attempt(device, points[kind], 3, baseline.commit_event + 1, 0,
				   false, &trace),
			    EXT4_IO);
			CHECK(trace.committed);
			CHECK(snprintf(prefix, sizeof(prefix), "index-pending-%s-", kind_name) > 0);
			storage_export(device, exports, path, prefix);
			CHECK(storage_recover(device, expected, true));
			device_reset(device, device->base);
			EXPECT(split_attempt(device, points[kind], 3, baseline.commit_event - 1, 0,
				   false, &trace),
			    EXT4_IO);
			CHECK(!trace.committed);
			CHECK(snprintf(prefix, sizeof(prefix), "index-uncommitted-%s-", kind_name) >
			    0);
			storage_export(device, exports, path, prefix);
			CHECK(storage_recover(device, expected, false));
		}
	}
	memcpy(device->base, original, device->size);
	free(expected);
	free(original);
}

#include "directory_links.h"

static void
index_creation_hashes(struct device *device)
{
	struct ext4_super_disk *super;
	struct trace trace;
	uint8_t *original = malloc(device->size);
	uint8_t version;
	uint32_t flags;
	uint32_t ordinal;
	uint32_t preserved;
	unsigned int cases = 0;

	CHECK(original != NULL);
	memcpy(original, device->base, device->size);
	for (version = EXT4_HASH_LEGACY; version <= EXT4_HASH_TEA_UNSIGNED; version++) {
		for (flags = 0; flags <= EXT4_UNSIGNED_DIRECTORY_HASH; flags++) {
			memcpy(device->base, original, device->size);
			super = (struct ext4_super_disk *)(device->base + EXT4_SUPER_OFFSET);
			preserved = ext4_le32(&super->flags) &
			    ~(EXT4_SIGNED_DIRECTORY_HASH | EXT4_UNSIGNED_DIRECTORY_HASH);
			ext4_encode32(&super->flags, preserved | flags);
			super->default_hash_version = version;
			if (device->metadata_checksum) {
				ext4_encode32(&super->checksum,
				    ext4_crc32c(UINT32_MAX, super,
					offsetof(struct ext4_super_disk, checksum)));
			}
			ordinal = prepare_index_creation(device);
			device_reset(device, device->base);
			EXPECT(split_attempt(device, ordinal, 0, 0, 0, false, &trace), EXT4_OK);
			index_creation_shape(device, ordinal + 1);
			cases++;
		}
	}
	memcpy(device->base, original, device->size);
	free(original);
	printf("PASS index creation hash formats=%u, including explicit unsigned hashes without "
	       "legacy flags\n",
	    cases);
}

int
main(int argc, char **argv)
{
	struct device device;
	const char *exports = NULL;
	const char *vectors = NULL;
	const char *large_kind = NULL;
	bool faults = false;
	bool smoke = false;
	bool capacity = false;
	bool conversion = false;
	bool links = false;
	bool real_links = false;
	bool creation_hashes = false;
	int argument = 1;

	while (argument < argc && argv[argument][0] == '-') {
		if (strcmp(argv[argument], "--creation-hashes") == 0) {
			creation_hashes = true;
		} else if (strcmp(argv[argument], "--directory-links") == 0) {
			links = true;
		} else if (strcmp(argv[argument], "--directory-links-image") == 0) {
			links = real_links = true;
		} else if (strcmp(argv[argument], "--create-index") == 0) {
			conversion = faults = true;
		} else if (strcmp(argv[argument], "--capacity") == 0) {
			capacity = true;
		} else if (strcmp(argv[argument], "--large-split") == 0 && argument + 1 < argc) {
			large_kind = argv[++argument];
			CHECK(
			    strcmp(large_kind, "grow") == 0 || strcmp(large_kind, "cascade") == 0);
		} else if (strcmp(argv[argument], "--edges") == 0 && argument + 1 < argc) {
			vectors = argv[++argument];
		} else if (strcmp(argv[argument], "--faults") == 0) {
			faults = true;
		} else if (strcmp(argv[argument], "--fault-smoke") == 0) {
			faults = smoke = true;
		} else if (strcmp(argv[argument], "--export") == 0 && argument + 1 < argc) {
			exports = argv[++argument];
		} else {
			CHECK(false);
		}
		argument++;
	}
	CHECK(argument < argc);
	CHECK(vectors == NULL || !faults);
	CHECK(!capacity || (vectors == NULL && !faults));
	CHECK(large_kind == NULL || faults);
	CHECK(!conversion || (large_kind == NULL && !capacity && vectors == NULL));
	CHECK(!links || (!faults && !capacity && vectors == NULL && large_kind == NULL));
	CHECK(!creation_hashes || (!links && !faults && !capacity && vectors == NULL));
	for (; argument < argc; argument++) {
		printf("IMAGE %s\n", argv[argument]);
		storage_open(&device, argv[argument]);
		if (creation_hashes) {
			index_creation_hashes(&device);
		} else if (links) {
			directory_links(&device, argv[argument], exports, real_links);
		} else if (capacity) {
			root_capacity(&device, argv[argument], exports);
		} else if (vectors != NULL) {
			indexed_edges(&device, argv[argument], vectors, exports);
		} else if (faults) {
			split_faults(
			    &device, argv[argument], exports, smoke, large_kind, conversion);
		} else {
			functional(&device, exports, argv[argument]);
		}
		storage_close(&device);
	}
	return 0;
}

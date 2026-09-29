/* SPDX-License-Identifier: BSD-3-Clause */
#include "storage.h"

#include <inttypes.h>

/* Long deterministic sequences of mixed public operations. A model retains the
 * expected namespace, file bytes, links, permissions and attributes. Selected
 * atomic operations also run from a clean snapshot with an injected power cut;
 * recovery must reproduce the exact old or new image outside the journal. */

#define SUSTAINED_SECONDS 1700002000
#define SUSTAINED_UID 71000U
#define SUSTAINED_GID 81000U
#define OBJECT_LIMIT 1024U
#define ENTRY_LIMIT 2048U
#define DEFAULT_OBJECTS 160U
#define DEFAULT_ENTRIES 320U
#define DEFAULT_DIRECTORIES 40U
#define HOLD_LIMIT 4U
#define BALLAST_LIMIT 4U
#define BALLAST_BYTES (24U * 1024U * 1024U)
#define BALLAST_CHUNK (256U * 1024U)
#define INDEX_FLAG 0x00001000U
#define FILE_LIMIT (128U * 1024U)
#define WRITE_LIMIT (12U * 1024U)
#define PARTIAL_LIMIT (80U * 1024U)
#define INLINE_KEY "data"
#define XATTR_KEYS 4U
#define XATTR_VALUE_LIMIT 80U
#define VERIFY_INTERVAL 50U
#define REMOUNT_INTERVAL 200U
#define CRASH_INTERVAL 6U
#define LONG_NAME_PERCENT 20U
#define EXISTING_NAME_PERCENT 8U
#define ROOT_NAME "sustained"
#define MANIFEST_NAME "manifest.txt"
#define NO_INDEX UINT32_MAX

/* Ballast files are regular files whose bytes derive from their seed. They fill
 * the volume without keeping megabytes of expected data in the model. */
enum kind {
	KIND_FREE,
	KIND_FILE,
	KIND_DIRECTORY,
	KIND_SYMLINK,
	KIND_FIFO,
	KIND_DEVICE,
	KIND_BALLAST
};

#define NONDIRECTORY_KINDS                                                                         \
	((1U << KIND_FILE) | (1U << KIND_SYMLINK) | (1U << KIND_FIFO) | (1U << KIND_DEVICE) |      \
	    (1U << KIND_BALLAST))

enum operation {
	OP_CREATE,
	OP_MKDIR,
	OP_SYMLINK,
	OP_MKNOD,
	OP_WRITE,
	OP_WRITE_PARTIAL,
	OP_TRUNCATE,
	OP_TRUNCATE_ATOMIC,
	OP_FALLOCATE,
	OP_LINK,
	OP_UNLINK,
	OP_RMDIR,
	OP_RENAME,
	OP_XATTR,
	OP_CHMOD,
	OP_HOLD,
	OP_RELEASE,
	OP_BALLAST,
	OP_COUNT
};

static const char *const operation_names[OP_COUNT] = { "create", "mkdir", "symlink", "mknod",
	"write", "write-partial", "truncate", "truncate-atomic", "fallocate", "link", "unlink",
	"rmdir", "rename", "xattr", "chmod", "hold", "release", "ballast" };

/* Relative frequency of each operation in the random sequence. */
static const uint32_t operation_weights[OP_COUNT] = { 12, 5, 3, 2, 16, 4, 5, 3, 4, 5, 9, 3, 10, 5,
	3, 2, 2, 3 };

static const uint8_t name_alphabet[] =
    "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789._-";

struct object {
	enum kind kind;
	uint32_t number;
	uint32_t generation;
	uint32_t names;
	uint32_t children;
	uint32_t subdirectories;
	uint64_t size;
	uint64_t seed;
	uint8_t *data;
	uint16_t permissions;
	bool xattr_present[XATTR_KEYS];
	uint8_t xattr_length[XATTR_KEYS];
	uint8_t xattr_value[XATTR_KEYS][XATTR_VALUE_LIMIT];
	struct ext4_inode_hold *hold;
};

struct entry {
	bool used;
	uint32_t parent;
	uint32_t object;
	size_t name_length;
	uint8_t name[EXT4_NAME_MAX];
};

/* Every random choice is made while planning. Execution is deterministic, so
 * the same plan can run once to produce the new image and again with a crash. */
struct plan {
	enum operation operation;
	uint32_t object;
	uint32_t directory;
	uint32_t entry;
	uint32_t target_entry;
	uint32_t flags;
	uint32_t key;
	uint64_t offset;
	uint64_t length;
	uint64_t seed;
	uint16_t permissions;
	bool remove;
	bool atomic;
	size_t name_length;
	uint8_t name[EXT4_NAME_MAX];
};

struct state {
	struct device device;
	struct ext4_fs *fs;
	struct object objects[OBJECT_LIMIT];
	struct entry entries[ENTRY_LIMIT];
	uint8_t *pre;
	uint8_t *post;
	uint8_t *buffer;
	uint8_t *ballast;
	uint64_t random;
	uint32_t root;
	uint32_t object_limit;
	uint32_t entry_limit;
	uint32_t directory_limit;
	uint32_t holds;
	uint32_t clock;
	uint32_t block_size;
	/* Deferred commit capacity; zero commits every operation durably. */
	uint32_t commit_blocks;
	uint32_t commits;
	bool extents;
	bool crashing;
	uint32_t performed[OP_COUNT];
	uint32_t rejected[OP_COUNT];
	uint32_t no_space;
	uint32_t crashes;
	uint32_t crash_committed;
	uint32_t torn_superblocks;
	uint32_t verifications;
	uint32_t remounts;
};

struct listing {
	struct state *state;
	uint32_t directory;
	uint32_t count;
};

static uint64_t
next_random(struct state *state)
{
	uint64_t value;

	state->random += UINT64_C(0x9e3779b97f4a7c15);
	value = state->random;
	value = (value ^ (value >> 30)) * UINT64_C(0xbf58476d1ce4e5b9);
	value = (value ^ (value >> 27)) * UINT64_C(0x94d049bb133111eb);
	return value ^ (value >> 31);
}

static uint32_t
pick(struct state *state, uint32_t bound)
{
	CHECK(bound != 0);
	return (uint32_t)(next_random(state) % bound);
}

static uint8_t
pattern_byte(uint64_t seed, uint64_t index)
{
	uint64_t value = seed + index * UINT64_C(0x9e3779b97f4a7c15);

	value ^= value >> 29;
	value *= UINT64_C(0xbf58476d1ce4e5b9);
	return (uint8_t)(value >> 56);
}

static struct ext4_timestamp
now(struct state *state)
{
	struct ext4_timestamp time = { SUSTAINED_SECONDS, 0 };

	time.seconds += state->clock++;
	return time;
}

static struct ext4_inode_update
creation(struct state *state, uint16_t permissions)
{
	struct ext4_inode_update update = { 0 };

	update.fields = EXT4_ATTR_PERMISSIONS | EXT4_ATTR_UID | EXT4_ATTR_GID |
	    EXT4_ATTR_ACCESS_TIME | EXT4_ATTR_MODIFY_TIME | EXT4_ATTR_CHANGE_TIME |
	    EXT4_ATTR_XATTRS;
	update.permissions = permissions;
	update.uid = SUSTAINED_UID;
	update.gid = SUSTAINED_GID;
	update.access_time = now(state);
	update.modify_time = update.access_time;
	update.change_time = update.access_time;
	return update;
}

static struct ext4_inode_update
modification(struct state *state, const struct object *object)
{
	struct ext4_inode_update update = { 0 };

	update.fields = EXT4_ATTR_PERMISSIONS | EXT4_ATTR_MODIFY_TIME | EXT4_ATTR_CHANGE_TIME |
	    EXT4_ATTR_XATTRS;
	update.permissions = object->permissions;
	update.modify_time = now(state);
	update.change_time = update.modify_time;
	return update;
}

static uint32_t
find_entry(const struct state *state, uint32_t parent, const uint8_t *name, size_t length)
{
	uint32_t index;

	for (index = 0; index < ENTRY_LIMIT; index++) {
		if (state->entries[index].used && state->entries[index].parent == parent &&
		    state->entries[index].name_length == length &&
		    memcmp(state->entries[index].name, name, length) == 0) {
			return index;
		}
	}
	return NO_INDEX;
}

static uint32_t
entry_of(const struct state *state, uint32_t object)
{
	uint32_t index;

	for (index = 0; index < ENTRY_LIMIT; index++) {
		if (state->entries[index].used && state->entries[index].object == object) {
			return index;
		}
	}
	return NO_INDEX;
}

static uint32_t
free_object(const struct state *state)
{
	uint32_t index;

	for (index = 0; index < state->object_limit; index++) {
		if (state->objects[index].kind == KIND_FREE) {
			return index;
		}
	}
	return NO_INDEX;
}

static uint32_t
free_entry(const struct state *state)
{
	uint32_t index;

	for (index = 0; index < state->entry_limit; index++) {
		if (!state->entries[index].used) {
			return index;
		}
	}
	return NO_INDEX;
}

/* Visit candidates from a random start so every eligible index can be chosen. */
static uint32_t
random_object(struct state *state, uint32_t kinds, bool linked)
{
	uint32_t start = pick(state, OBJECT_LIMIT);
	uint32_t step;
	uint32_t index;
	const struct object *object;

	for (step = 0; step < OBJECT_LIMIT; step++) {
		index = (start + step) % OBJECT_LIMIT;
		object = &state->objects[index];
		if (object->kind != KIND_FREE && (kinds & (1U << object->kind)) &&
		    (!linked || object->names != 0)) {
			return index;
		}
	}
	return NO_INDEX;
}

static uint32_t
random_entry(struct state *state, uint32_t kinds)
{
	uint32_t start = pick(state, ENTRY_LIMIT);
	uint32_t step;
	uint32_t index;

	for (step = 0; step < ENTRY_LIMIT; step++) {
		index = (start + step) % ENTRY_LIMIT;
		if (state->entries[index].used &&
		    (kinds & (1U << state->objects[state->entries[index].object].kind))) {
			return index;
		}
	}
	return NO_INDEX;
}

static bool
is_ancestor(const struct state *state, uint32_t ancestor, uint32_t object)
{
	uint32_t entry;

	while (object != state->root) {
		if (object == ancestor) {
			return true;
		}
		entry = entry_of(state, object);
		CHECK(entry != NO_INDEX);
		object = state->entries[entry].parent;
	}
	return ancestor == state->root;
}

static void
random_name(struct state *state, struct plan *plan)
{
	size_t index;

	do {
		plan->name_length = pick(state, 100) < LONG_NAME_PERCENT
		    ? 25U + pick(state, EXT4_NAME_MAX - 24U)
		    : 1U + pick(state, 24);
		for (index = 0; index < plan->name_length; index++) {
			plan->name[index] = name_alphabet[pick(state, sizeof(name_alphabet) - 1U)];
		}
	} while ((plan->name_length == 1 && plan->name[0] == '.') ||
	    (plan->name_length == 2 && plan->name[0] == '.' && plan->name[1] == '.'));
}

/* Usually choose an absent name; occasionally reuse one to test rejection. */
static void
choose_name(struct state *state, struct plan *plan)
{
	uint32_t existing;

	if (pick(state, 100) < EXISTING_NAME_PERCENT) {
		existing = random_entry(state, UINT32_MAX);
		if (existing != NO_INDEX && state->entries[existing].parent == plan->directory) {
			plan->name_length = state->entries[existing].name_length;
			memcpy(plan->name, state->entries[existing].name, plan->name_length);
			return;
		}
	}
	do {
		random_name(state, plan);
	} while (find_entry(state, plan->directory, plan->name, plan->name_length) != NO_INDEX);
}

static uint32_t
add_entry(struct state *state, uint32_t parent, uint32_t object, const uint8_t *name, size_t length)
{
	uint32_t index = free_entry(state);
	struct entry *entry;

	CHECK(index != NO_INDEX);
	entry = &state->entries[index];
	entry->used = true;
	entry->parent = parent;
	entry->object = object;
	entry->name_length = length;
	memcpy(entry->name, name, length);
	state->objects[parent].children++;
	if (state->objects[object].kind == KIND_DIRECTORY) {
		state->objects[parent].subdirectories++;
	}
	state->objects[object].names++;
	return index;
}

static void
release_object(struct state *state, uint32_t index)
{
	struct object *object = &state->objects[index];

	free(object->data);
	memset(object, 0, sizeof(*object));
}

/* Remove one name. The object disappears with its last name unless held. */
static void
remove_entry(struct state *state, uint32_t index)
{
	struct entry *entry = &state->entries[index];
	struct object *object = &state->objects[entry->object];

	CHECK(entry->used && object->names != 0);
	state->objects[entry->parent].children--;
	if (object->kind == KIND_DIRECTORY) {
		state->objects[entry->parent].subdirectories--;
	}
	object->names--;
	entry->used = false;
	if (object->names == 0 && object->hold == NULL) {
		release_object(state, entry->object);
	}
}

static uint32_t
new_object(struct state *state, enum kind kind, const struct ext4_inode *inode)
{
	uint32_t index = free_object(state);
	struct object *object;

	CHECK(index != NO_INDEX);
	object = &state->objects[index];
	memset(object, 0, sizeof(*object));
	object->kind = kind;
	object->number = inode->number;
	object->generation = inode->generation;
	object->permissions = inode->mode & 07777U;
	if (kind == KIND_FILE || kind == KIND_SYMLINK) {
		object->data = calloc(1, FILE_LIMIT);
		CHECK(object->data != NULL);
	}
	return index;
}

static void
fill_pattern(uint8_t *buffer, uint64_t seed, uint64_t length)
{
	uint64_t index;

	for (index = 0; index < length; index++) {
		buffer[index] = pattern_byte(seed, index);
	}
}

static void
fill_target(uint8_t *buffer, uint64_t seed, uint64_t length)
{
	uint64_t index;

	for (index = 0; index < length; index++) {
		buffer[index] = (uint8_t)(1U + pattern_byte(seed, index) % UINT8_MAX);
	}
}

static uint32_t
directory_count(const struct state *state)
{
	uint32_t count = 0;
	uint32_t index;

	for (index = 0; index < OBJECT_LIMIT; index++) {
		count += state->objects[index].kind == KIND_DIRECTORY;
	}
	return count;
}

static bool
plan_operation(struct state *state, struct plan *plan)
{
	uint32_t total = 0;
	uint32_t value;
	uint32_t index;
	struct object *object;
	struct entry *entry;

	for (index = 0; index < OP_COUNT; index++) {
		total += operation_weights[index];
	}
	value = pick(state, total);
	for (index = 0; value >= operation_weights[index]; index++) {
		value -= operation_weights[index];
	}
	memset(plan, 0, sizeof(*plan));
	plan->operation = (enum operation)index;
	plan->seed = next_random(state);
	plan->object = plan->directory = plan->entry = plan->target_entry = NO_INDEX;
	switch (plan->operation) {
	case OP_CREATE:
	case OP_MKDIR:
	case OP_SYMLINK:
	case OP_MKNOD:
		if (free_object(state) == NO_INDEX || free_entry(state) == NO_INDEX ||
		    (plan->operation == OP_MKDIR &&
			directory_count(state) >= state->directory_limit)) {
			return false;
		}
		plan->directory = random_object(state, 1U << KIND_DIRECTORY, false);
		plan->permissions = (uint16_t)(plan->operation == OP_MKDIR ? 0750U : 0640U);
		plan->length =
		    plan->operation == OP_SYMLINK ? 1U + pick(state, state->block_size - 1U) : 0;
		plan->flags = pick(state, 2);
		choose_name(state, plan);
		return true;
	case OP_WRITE:
	case OP_WRITE_PARTIAL:
		plan->object = random_object(state, 1U << KIND_FILE, false);
		if (plan->object == NO_INDEX) {
			return false;
		}
		object = &state->objects[plan->object];
		plan->length =
		    1U + pick(state, plan->operation == OP_WRITE ? WRITE_LIMIT : PARTIAL_LIMIT);
		value = (uint32_t)(object->size + state->block_size * 2U);
		if (value > FILE_LIMIT - plan->length) {
			value = (uint32_t)(FILE_LIMIT - plan->length);
		}
		plan->offset = pick(state, value + 1U);
		return true;
	case OP_TRUNCATE:
	case OP_TRUNCATE_ATOMIC:
		plan->object = random_object(state, 1U << KIND_FILE, false);
		if (plan->object == NO_INDEX) {
			return false;
		}
		object = &state->objects[plan->object];
		value = pick(state, 10);
		plan->offset = value < 2 ? 0
		    : value < 6		 ? pick(state, (uint32_t)object->size + 1U)
					 : pick(state, FILE_LIMIT + 1U);
		return true;
	case OP_FALLOCATE:
		plan->object = random_object(state, 1U << KIND_FILE, false);
		if (plan->object == NO_INDEX || !state->extents) {
			return false;
		}
		plan->length = 1U + pick(state, PARTIAL_LIMIT);
		plan->offset = pick(state, (uint32_t)(FILE_LIMIT - plan->length) + 1U);
		value = pick(state, 3);
		plan->flags = value == 0 ? 0
		    : value == 1	 ? EXT4_FALLOC_KEEP_SIZE
					 : EXT4_FALLOC_KEEP_SIZE | EXT4_FALLOC_PUNCH_HOLE;
		return true;
	case OP_LINK:
		plan->object = random_object(state, NONDIRECTORY_KINDS, true);
		if (plan->object == NO_INDEX || free_entry(state) == NO_INDEX) {
			return false;
		}
		plan->directory = random_object(state, 1U << KIND_DIRECTORY, false);
		choose_name(state, plan);
		return true;
	case OP_UNLINK:
		plan->entry = random_entry(state, NONDIRECTORY_KINDS);
		return plan->entry != NO_INDEX;
	case OP_RMDIR:
		plan->entry = random_entry(state, 1U << KIND_DIRECTORY);
		if (plan->entry == NO_INDEX) {
			return false;
		}
		/* Mostly target empty directories; a populated one must reject. */
		if (state->objects[state->entries[plan->entry].object].children != 0 &&
		    pick(state, 4) != 0) {
			return false;
		}
		return true;
	case OP_RENAME:
		plan->entry = random_entry(state, UINT32_MAX);
		if (plan->entry == NO_INDEX) {
			return false;
		}
		plan->directory = random_object(state, 1U << KIND_DIRECTORY, false);
		value = pick(state, 20);
		plan->flags = value < 14 ? 0
		    : value < 17	 ? EXT4_RENAME_NOREPLACE
					 : EXT4_RENAME_EXCHANGE;
		if (pick(state, 2) == 0) {
			plan->target_entry = random_entry(state, UINT32_MAX);
			if (plan->target_entry != NO_INDEX &&
			    state->entries[plan->target_entry].parent != plan->directory) {
				plan->target_entry = NO_INDEX;
			}
		}
		if (plan->target_entry == NO_INDEX) {
			if (free_entry(state) == NO_INDEX) {
				return false;
			}
			do {
				random_name(state, plan);
			} while (find_entry(state, plan->directory, plan->name,
				     plan->name_length) != NO_INDEX);
		} else {
			entry = &state->entries[plan->target_entry];
			plan->name_length = entry->name_length;
			memcpy(plan->name, entry->name, entry->name_length);
		}
		return true;
	case OP_XATTR:
		plan->object =
		    random_object(state, (1U << KIND_FILE) | (1U << KIND_DIRECTORY), true);
		if (plan->object == NO_INDEX || plan->object == state->root) {
			return false;
		}
		plan->key = pick(state, XATTR_KEYS);
		plan->remove = pick(state, 3) == 0;
		plan->length = 1U + pick(state, XATTR_VALUE_LIMIT);
		return true;
	case OP_CHMOD:
		plan->object = random_object(state, UINT32_MAX, true);
		if (plan->object == NO_INDEX || plan->object == state->root) {
			return false;
		}
		plan->permissions = (uint16_t)(0400U | pick(state, 0400));
		return true;
	case OP_HOLD:
		if (state->holds == HOLD_LIMIT) {
			return false;
		}
		plan->object = random_object(state, 1U << KIND_FILE, true);
		return plan->object != NO_INDEX && state->objects[plan->object].hold == NULL;
	case OP_BALLAST:
		if (free_object(state) == NO_INDEX || free_entry(state) == NO_INDEX) {
			return false;
		}
		for (index = 0, value = 0; index < OBJECT_LIMIT; index++) {
			value += state->objects[index].kind == KIND_BALLAST;
		}
		if (value >= BALLAST_LIMIT) {
			return false;
		}
		plan->directory = state->root;
		plan->permissions = 0600;
		plan->length = 1U + pick(state, BALLAST_BYTES);
		do {
			random_name(state, plan);
		} while (
		    find_entry(state, plan->directory, plan->name, plan->name_length) != NO_INDEX);
		return true;
	case OP_RELEASE:
		for (index = 0; index < OBJECT_LIMIT; index++) {
			if (state->objects[index].hold != NULL) {
				plan->object = index;
				if (pick(state, 2) == 0) {
					break;
				}
			}
		}
		return plan->object != NO_INDEX;
	case OP_COUNT:
		break;
	}
	return false;
}

static bool
crash_eligible(const struct state *state, const struct plan *plan)
{
	return state->holds == 0 && plan->operation != OP_WRITE_PARTIAL &&
	    plan->operation != OP_FALLOCATE && plan->operation != OP_HOLD &&
	    plan->operation != OP_RELEASE && plan->operation != OP_BALLAST;
}

static enum ext4_result
execute_create(struct state *state, const struct plan *plan, bool apply)
{
	struct object *directory = &state->objects[plan->directory];
	struct ext4_inode_update update = creation(state, plan->permissions);
	struct ext4_timestamp time = now(state);
	struct ext4_special_file special = { EXT4_FT_FIFO, 0, 0 };
	struct ext4_inode inode;
	uint8_t target[EXT4_MAX_BLOCK_SIZE];
	uint32_t object;
	enum kind kind = KIND_FILE;
	enum ext4_result error;
	bool exists = find_entry(state, plan->directory, plan->name, plan->name_length) != NO_INDEX;

	switch (plan->operation) {
	case OP_MKDIR:
		kind = KIND_DIRECTORY;
		error = ext4_mkdir(state->fs, directory->number, directory->generation, plan->name,
		    plan->name_length, &update, &time, &inode);
		break;
	case OP_SYMLINK:
		kind = KIND_SYMLINK;
		fill_target(target, plan->seed, plan->length);
		error = ext4_symlink(state->fs, directory->number, directory->generation,
		    plan->name, plan->name_length, target, plan->length, &update, &time, &inode);
		break;
	case OP_MKNOD:
		if (plan->flags != 0) {
			kind = KIND_DEVICE;
			special.type = EXT4_FT_CHARACTER;
			special.device_major = (uint32_t)(plan->seed % EXT4_DEVICE_MAJOR_MAX);
			special.device_minor = (uint32_t)(plan->seed >> 32) % EXT4_DEVICE_MINOR_MAX;
		} else {
			kind = KIND_FIFO;
		}
		error = ext4_mknod(state->fs, directory->number, directory->generation, plan->name,
		    plan->name_length, &special, &update, &time, &inode);
		break;
	default:
		error = ext4_create(state->fs, directory->number, directory->generation, plan->name,
		    plan->name_length, &update, &time, &inode);
		break;
	}
	if (state->crashing) {
		return error;
	}
	if (exists) {
		EXPECT(error, EXT4_EXISTS);
		return error;
	}
	if (error == EXT4_NO_SPACE) {
		return error;
	}
	EXPECT(error, EXT4_OK);
	if (apply) {
		object = new_object(state, kind, &inode);
		if (kind == KIND_SYMLINK) {
			memcpy(state->objects[object].data, target, plan->length);
			state->objects[object].size = plan->length;
		}
		if (kind == KIND_DEVICE) {
			CHECK(inode.device_major == special.device_major &&
			    inode.device_minor == special.device_minor);
		}
		add_entry(state, plan->directory, object, plan->name, plan->name_length);
	}
	return error;
}

static enum ext4_result
execute_write(struct state *state, const struct plan *plan, bool apply)
{
	struct object *object = &state->objects[plan->object];
	struct ext4_inode_update update = modification(state, object);
	size_t completed = 0;
	enum ext4_result error;

	fill_pattern(state->buffer, plan->seed, plan->length);
	if (plan->operation == OP_WRITE) {
		error = ext4_write(state->fs, object->number, object->generation, plan->offset,
		    state->buffer, plan->length, &update, &completed);
		if (state->crashing) {
			return error;
		}
		CHECK(error == EXT4_OK ? completed == plan->length : completed == 0);
	} else {
		error = ext4_write_partial(state->fs, object->number, object->generation,
		    plan->offset, state->buffer, plan->length, &update, &completed);
		CHECK(completed <= plan->length && (error != EXT4_OK || completed == plan->length));
	}
	/* Capacity limits reject without changing visible state. */
	if (error != EXT4_OK && error != EXT4_NO_SPACE && error != EXT4_UNSUPPORTED) {
		EXPECT(error, EXT4_OK);
	}
	if (apply && completed != 0) {
		memcpy(object->data + plan->offset, state->buffer, completed);
		if (plan->offset + completed > object->size) {
			object->size = plan->offset + completed;
		}
	}
	return error;
}

static enum ext4_result
execute_truncate(struct state *state, const struct plan *plan, bool apply)
{
	struct object *object = &state->objects[plan->object];
	struct ext4_inode_update update = modification(state, object);
	struct ext4_inode inode;
	enum ext4_result error;

	if (plan->operation == OP_TRUNCATE_ATOMIC) {
		error = ext4_truncate_atomic(
		    state->fs, object->number, object->generation, plan->offset, &update, &inode);
	} else {
		error = ext4_truncate(
		    state->fs, object->number, object->generation, plan->offset, &update, &inode);
	}
	if (state->crashing || error == EXT4_NO_SPACE || error == EXT4_UNSUPPORTED) {
		return error;
	}
	EXPECT(error, EXT4_OK);
	CHECK(inode.size == plan->offset);
	if (apply) {
		if (plan->offset < object->size) {
			memset(object->data + plan->offset, 0, object->size - plan->offset);
		}
		object->size = plan->offset;
	}
	return error;
}

static enum ext4_result
execute_fallocate(struct state *state, const struct plan *plan, bool apply)
{
	struct object *object = &state->objects[plan->object];
	struct ext4_inode_update update = modification(state, object);
	uint64_t completed = 0;
	uint64_t end;
	enum ext4_result error;

	error = ext4_fallocate(state->fs, object->number, object->generation, plan->offset,
	    plan->length, plan->flags, &update, &completed);
	CHECK(completed <= plan->length && (error != EXT4_OK || completed == plan->length));
	if (error != EXT4_OK && error != EXT4_NO_SPACE && error != EXT4_UNSUPPORTED) {
		EXPECT(error, EXT4_OK);
	}
	if (apply && completed != 0) {
		end = plan->offset + completed;
		if (plan->flags & EXT4_FALLOC_PUNCH_HOLE) {
			if (end > object->size) {
				end = object->size;
			}
			if (plan->offset < end) {
				memset(object->data + plan->offset, 0, end - plan->offset);
			}
		} else if (!(plan->flags & EXT4_FALLOC_KEEP_SIZE) && end > object->size) {
			object->size = end;
		}
	}
	return error;
}

static enum ext4_result
execute_link(struct state *state, const struct plan *plan, bool apply)
{
	struct object *directory = &state->objects[plan->directory];
	struct object *object = &state->objects[plan->object];
	struct ext4_timestamp time = now(state);
	struct ext4_inode inode;
	enum ext4_result error;
	bool exists = find_entry(state, plan->directory, plan->name, plan->name_length) != NO_INDEX;

	error = ext4_link(state->fs, directory->number, directory->generation, plan->name,
	    plan->name_length, object->number, object->generation, &time, &inode);
	if (state->crashing) {
		return error;
	}
	if (exists) {
		EXPECT(error, EXT4_EXISTS);
		return error;
	}
	if (error == EXT4_NO_SPACE) {
		return error;
	}
	EXPECT(error, EXT4_OK);
	CHECK(inode.number == object->number && inode.links == object->names + 1U);
	if (apply) {
		add_entry(state, plan->directory, plan->object, plan->name, plan->name_length);
	}
	return error;
}

static enum ext4_result
execute_remove(struct state *state, const struct plan *plan, bool apply)
{
	struct entry *entry = &state->entries[plan->entry];
	struct object *directory = &state->objects[entry->parent];
	struct object *object = &state->objects[entry->object];
	struct ext4_timestamp time = now(state);
	struct ext4_inode inode;
	enum ext4_result error;

	if (plan->operation == OP_RMDIR) {
		error = ext4_rmdir(state->fs, directory->number, directory->generation, entry->name,
		    entry->name_length, object->number, object->generation, &time, &inode);
		if (!state->crashing && object->children != 0) {
			EXPECT(error, EXT4_NOT_EMPTY);
			return error;
		}
	} else {
		error =
		    ext4_unlink(state->fs, directory->number, directory->generation, entry->name,
			entry->name_length, object->number, object->generation, &time, &inode);
	}
	if (state->crashing) {
		return error;
	}
	EXPECT(error, EXT4_OK);
	CHECK(inode.number == object->number);
	if (apply) {
		remove_entry(state, plan->entry);
	}
	return error;
}

/* The model decides whether a rename is valid before calling the core. */
static bool
rename_valid(const struct state *state, const struct plan *plan, uint32_t *replaced, bool *noop)
{
	const struct entry *source = &state->entries[plan->entry];
	const struct object *moving = &state->objects[source->object];
	const struct entry *target = NULL;
	const struct object *other = NULL;
	uint32_t existing = find_entry(state, plan->directory, plan->name, plan->name_length);

	*replaced = existing;
	*noop = false;
	if (existing != NO_INDEX) {
		target = &state->entries[existing];
		other = &state->objects[target->object];
	}
	if (target != NULL && target->object == source->object) {
		*noop = plan->flags != EXT4_RENAME_NOREPLACE;
		return *noop;
	}
	if (plan->flags == EXT4_RENAME_EXCHANGE) {
		if (target == NULL) {
			return false;
		}
		if (moving->kind == KIND_DIRECTORY &&
		    is_ancestor(state, source->object, target->parent)) {
			return false;
		}
		return other->kind != KIND_DIRECTORY ||
		    !is_ancestor(state, target->object, source->parent);
	}
	if (moving->kind == KIND_DIRECTORY && is_ancestor(state, source->object, plan->directory)) {
		return false;
	}
	if (target == NULL) {
		return true;
	}
	if (plan->flags == EXT4_RENAME_NOREPLACE) {
		return false;
	}
	if ((moving->kind == KIND_DIRECTORY) != (other->kind == KIND_DIRECTORY)) {
		return false;
	}
	return other->kind != KIND_DIRECTORY || other->children == 0;
}

static void
move_entry(struct state *state, uint32_t index, uint32_t parent, const uint8_t *name, size_t length)
{
	struct entry *entry = &state->entries[index];
	bool directory = state->objects[entry->object].kind == KIND_DIRECTORY;

	state->objects[entry->parent].children--;
	state->objects[parent].children++;
	if (directory) {
		state->objects[entry->parent].subdirectories--;
		state->objects[parent].subdirectories++;
	}
	entry->parent = parent;
	entry->name_length = length;
	memcpy(entry->name, name, length);
}

static enum ext4_result
execute_rename(struct state *state, const struct plan *plan, bool apply)
{
	struct entry *source_entry = &state->entries[plan->entry];
	struct object *source_parent = &state->objects[source_entry->parent];
	struct object *moving = &state->objects[source_entry->object];
	struct object *directory = &state->objects[plan->directory];
	struct ext4_rename_entry source = { source_parent->number, source_parent->generation,
		source_entry->name, source_entry->name_length, moving->number, moving->generation };
	struct ext4_rename_entry destination = { directory->number, directory->generation,
		plan->name, plan->name_length, 0, 0 };
	struct ext4_timestamp time = now(state);
	struct ext4_inode inode;
	struct entry saved;
	uint32_t replaced;
	bool noop;
	bool valid = rename_valid(state, plan, &replaced, &noop);
	enum ext4_result error;

	if (replaced != NO_INDEX) {
		destination.inode = state->objects[state->entries[replaced].object].number;
		destination.generation = state->objects[state->entries[replaced].object].generation;
	}
	error = ext4_rename(state->fs, &source, &destination, plan->flags, &time, &inode);
	if (state->crashing) {
		return error;
	}
	if (!valid) {
		CHECK(error != EXT4_OK && error != EXT4_IO && !state->fs->aborted);
		return error;
	}
	if (error == EXT4_NO_SPACE) {
		return error;
	}
	EXPECT(error, EXT4_OK);
	if (!apply || noop) {
		return error;
	}
	if (plan->flags == EXT4_RENAME_EXCHANGE) {
		saved = state->entries[replaced];
		move_entry(state, replaced, source_entry->parent, source_entry->name,
		    source_entry->name_length);
		move_entry(state, plan->entry, saved.parent, saved.name, saved.name_length);
		return error;
	}
	if (replaced != NO_INDEX) {
		remove_entry(state, replaced);
	}
	move_entry(state, plan->entry, plan->directory, plan->name, plan->name_length);
	return error;
}

static enum ext4_result
execute_attributes(struct state *state, const struct plan *plan, bool apply)
{
	struct object *object = &state->objects[plan->object];
	struct ext4_inode_update update = { 0 };
	struct ext4_xattr_change change = { 0 };
	struct ext4_inode inode;
	uint8_t name[2] = { 'k', 0 };
	uint8_t value[XATTR_VALUE_LIMIT];
	enum ext4_result error;

	update.fields = EXT4_ATTR_CHANGE_TIME | EXT4_ATTR_XATTRS;
	update.change_time = now(state);
	if (plan->operation == OP_CHMOD) {
		update.fields |= EXT4_ATTR_PERMISSIONS;
		update.permissions = plan->permissions;
	} else {
		name[1] = (uint8_t)('0' + plan->key);
		fill_pattern(value, plan->seed, plan->length);
		change.policy = plan->remove ? EXT4_XATTR_REMOVE : EXT4_XATTR_SET;
		change.name_index = EXT4_XATTR_USER;
		change.name = name;
		change.name_length = sizeof(name);
		change.value = plan->remove ? NULL : value;
		change.value_size = plan->remove ? 0 : plan->length;
		update.xattrs = &change;
		update.xattr_count = 1;
	}
	error = ext4_set_attributes(state->fs, object->number, object->generation, &update, &inode);
	if (state->crashing) {
		return error;
	}
	if (plan->operation == OP_XATTR && plan->remove && !object->xattr_present[plan->key]) {
		CHECK(error != EXT4_OK && error != EXT4_IO && !state->fs->aborted);
		return error;
	}
	if (error == EXT4_NO_SPACE) {
		return error;
	}
	EXPECT(error, EXT4_OK);
	if (apply) {
		if (plan->operation == OP_CHMOD) {
			object->permissions = plan->permissions;
		} else {
			object->xattr_present[plan->key] = !plan->remove;
			object->xattr_length[plan->key] = plan->remove ? 0 : (uint8_t)plan->length;
			memcpy(
			    object->xattr_value[plan->key], value, plan->remove ? 0 : plan->length);
		}
	}
	return error;
}

static enum ext4_result
execute_hold(struct state *state, const struct plan *plan)
{
	struct object *object = &state->objects[plan->object];
	enum ext4_result error;

	if (plan->operation == OP_HOLD) {
		EXPECT(
		    ext4_hold_inode(state->fs, object->number, object->generation, &object->hold),
		    EXT4_OK);
		state->holds++;
		return EXT4_OK;
	}
	error = ext4_release_inode(object->hold);
	EXPECT(error, EXT4_OK);
	object->hold = NULL;
	state->holds--;
	if (object->names == 0) {
		release_object(state, plan->object);
	}
	return error;
}

/* Create a ballast file and grow it with seeded bytes until the request or the
 * volume's space ends. A short durable prefix remains a valid model state. */
static enum ext4_result
execute_ballast(struct state *state, const struct plan *plan)
{
	struct object *directory = &state->objects[plan->directory];
	struct ext4_inode_update update = creation(state, plan->permissions);
	struct ext4_timestamp time = now(state);
	struct ext4_inode inode;
	struct object *object;
	uint64_t offset = 0;
	uint64_t length;
	size_t completed = 0;
	uint32_t index;
	enum ext4_result error;

	error = ext4_create(state->fs, directory->number, directory->generation, plan->name,
	    plan->name_length, &update, &time, &inode);
	if (error == EXT4_NO_SPACE) {
		return error;
	}
	EXPECT(error, EXT4_OK);
	index = new_object(state, KIND_BALLAST, &inode);
	add_entry(state, plan->directory, index, plan->name, plan->name_length);
	object = &state->objects[index];
	object->seed = plan->seed;
	while (offset < plan->length) {
		length = plan->length - offset;
		if (length > BALLAST_CHUNK) {
			length = BALLAST_CHUNK;
		}
		update = modification(state, object);
		error = ext4_write_partial(state->fs, object->number, object->generation, offset,
		    state->ballast + offset, length, &update, &completed);
		offset += completed;
		object->size = offset;
		if (error == EXT4_NO_SPACE || error == EXT4_UNSUPPORTED) {
			state->no_space++;
			return EXT4_OK;
		}
		EXPECT(error, EXT4_OK);
	}
	return error;
}

static enum ext4_result
execute(struct state *state, const struct plan *plan, bool apply)
{
	switch (plan->operation) {
	case OP_CREATE:
	case OP_MKDIR:
	case OP_SYMLINK:
	case OP_MKNOD:
		return execute_create(state, plan, apply);
	case OP_WRITE:
	case OP_WRITE_PARTIAL:
		return execute_write(state, plan, apply);
	case OP_TRUNCATE:
	case OP_TRUNCATE_ATOMIC:
		return execute_truncate(state, plan, apply);
	case OP_FALLOCATE:
		return execute_fallocate(state, plan, apply);
	case OP_LINK:
		return execute_link(state, plan, apply);
	case OP_UNLINK:
	case OP_RMDIR:
		return execute_remove(state, plan, apply);
	case OP_RENAME:
		return execute_rename(state, plan, apply);
	case OP_XATTR:
	case OP_CHMOD:
		return execute_attributes(state, plan, apply);
	case OP_HOLD:
	case OP_RELEASE:
		return execute_hold(state, plan);
	case OP_BALLAST:
		return execute_ballast(state, plan);
	case OP_COUNT:
		break;
	}
	CHECK(false);
	return EXT4_INVALID_ARGUMENT;
}

static uint16_t
expected_type(enum kind kind)
{
	switch (kind) {
	case KIND_FILE:
	case KIND_BALLAST:
		return EXT4_MODE_REGULAR;
	case KIND_DIRECTORY:
		return EXT4_MODE_DIRECTORY;
	case KIND_SYMLINK:
		return EXT4_MODE_SYMLINK;
	case KIND_FIFO:
		return EXT4_MODE_FIFO;
	case KIND_DEVICE:
		return EXT4_MODE_CHARACTER;
	case KIND_FREE:
		break;
	}
	CHECK(false);
	return 0;
}

static void
verify_content(struct state *state, const struct object *object, const struct ext4_inode *inode,
    bool attributes)
{
	struct ext4_xattr_key keys[XATTR_KEYS + 2U];
	uint8_t name[2] = { 'k', 0 };
	uint8_t value[XATTR_VALUE_LIMIT];
	size_t completed;
	size_t count;
	size_t size;
	size_t index;
	uint32_t key;
	uint32_t present = 0;
	uint32_t listed = 0;

	CHECK(inode->number == object->number && inode->generation == object->generation);
	CHECK((inode->mode & EXT4_MODE_TYPE) == expected_type(object->kind));
	CHECK((inode->mode & 07777U) == object->permissions);
	if (object->kind == KIND_FILE || object->kind == KIND_SYMLINK ||
	    object->kind == KIND_BALLAST) {
		CHECK(inode->size == object->size);
	}
	if (object->kind == KIND_BALLAST && object->size != 0) {
		EXPECT(ext4_read(state->fs, inode, 0, state->ballast + BALLAST_BYTES, object->size,
			   &completed),
		    EXT4_OK);
		CHECK(completed == object->size &&
		    memcmp(state->ballast + BALLAST_BYTES, state->ballast, object->size) == 0);
	}
	if (object->kind == KIND_FILE) {
		if (object->size != 0) {
			EXPECT(
			    ext4_read(state->fs, inode, 0, state->buffer, object->size, &completed),
			    EXT4_OK);
			CHECK(completed == object->size &&
			    memcmp(state->buffer, object->data, object->size) == 0);
		}
	} else if (object->kind == KIND_SYMLINK) {
		if (inode->fast_symlink) {
			CHECK(memcmp(inode->block_data, object->data, object->size) == 0);
		} else {
			EXPECT(
			    ext4_read(state->fs, inode, 0, state->buffer, object->size, &completed),
			    EXT4_OK);
			CHECK(completed == object->size &&
			    memcmp(state->buffer, object->data, object->size) == 0);
		}
	}
	if (object->kind == KIND_DIRECTORY) {
		CHECK(inode->links == 2U + object->subdirectories);
	} else {
		CHECK(inode->links == object->names);
	}
	for (key = 0; attributes && key < XATTR_KEYS; key++) {
		name[1] = (uint8_t)('0' + key);
		if (!object->xattr_present[key]) {
			EXPECT(ext4_get_xattr(state->fs, object->number, object->generation,
				   EXT4_XATTR_USER, name, sizeof(name), NULL, 0, &size),
			    EXT4_NOT_FOUND);
			continue;
		}
		present++;
		EXPECT(ext4_get_xattr(state->fs, object->number, object->generation,
			   EXT4_XATTR_USER, name, sizeof(name), value, sizeof(value), &size),
		    EXT4_OK);
		CHECK(size == object->xattr_length[key] &&
		    memcmp(value, object->xattr_value[key], size) == 0);
	}
	if (attributes) {
		EXPECT(ext4_list_xattrs(state->fs, object->number, object->generation, keys,
			   XATTR_KEYS + 2U, &count),
		    EXT4_OK);
		/* The raw list also reports an inline-data inode's internal key. */
		for (index = 0; index < count; index++) {
			if (keys[index].name_index == EXT4_XATTR_USER) {
				listed++;
				continue;
			}
			CHECK((inode->flags & EXT4_INODE_INLINE_DATA) &&
			    keys[index].name_index == EXT4_XATTR_SYSTEM &&
			    keys[index].name_length == sizeof(INLINE_KEY) - 1U &&
			    memcmp(keys[index].name, INLINE_KEY, sizeof(INLINE_KEY) - 1U) == 0);
		}
		CHECK(listed == present);
	}
}

static enum ext4_dir_action
visit_entry(void *context, const struct ext4_dir_entry *entry, uint64_t next_cookie)
{
	struct listing *listing = context;
	struct state *state = listing->state;
	uint32_t index;

	(void)next_cookie;
	if ((entry->name_length == 1 && entry->name[0] == '.') ||
	    (entry->name_length == 2 && entry->name[0] == '.' && entry->name[1] == '.')) {
		return EXT4_DIR_ACCEPT;
	}
	index = find_entry(state, listing->directory, entry->name, entry->name_length);
	CHECK(index != NO_INDEX &&
	    state->objects[state->entries[index].object].number == entry->inode);
	listing->count++;
	return EXT4_DIR_ACCEPT;
}

static void
verify(struct state *state)
{
	struct ext4_inode inode;
	struct ext4_inode parent;
	struct listing listing;
	struct entry *entry;
	struct object *object;
	uint64_t cookie;
	uint32_t index;

	for (index = 0; index < OBJECT_LIMIT; index++) {
		object = &state->objects[index];
		if (object->kind != KIND_DIRECTORY) {
			continue;
		}
		EXPECT(ext4_get_inode(state->fs, object->number, &inode), EXT4_OK);
		verify_content(state, object, &inode, true);
		listing.state = state;
		listing.directory = index;
		listing.count = 0;
		cookie = 0;
		EXPECT(ext4_iterate_dir(state->fs, &inode, &cookie, visit_entry, &listing),
		    EXT4_NOT_FOUND);
		CHECK(listing.count == object->children);
	}
	for (index = 0; index < ENTRY_LIMIT; index++) {
		entry = &state->entries[index];
		if (!entry->used) {
			continue;
		}
		EXPECT(ext4_get_inode(state->fs, state->objects[entry->parent].number, &parent),
		    EXT4_OK);
		EXPECT(ext4_lookup(state->fs, &parent, entry->name, entry->name_length, &inode),
		    EXT4_OK);
		verify_content(state, &state->objects[entry->object], &inode, true);
	}
	for (index = 0; index < OBJECT_LIMIT; index++) {
		object = &state->objects[index];
		if (object->hold != NULL) {
			EXPECT(ext4_refresh_inode(object->hold, &inode), EXT4_OK);
			verify_content(state, object, &inode, object->names != 0);
		}
	}
	state->verifications++;
}

static void
mount_writer(struct state *state)
{
	struct ext4_write_options options = { state->commit_blocks };

	EXPECT(ext4_mount_writable_with_options(
		   &state->device.environment, &state->device.writer, NULL, &options, &state->fs),
	    EXT4_OK);
}

/* Under deferred commit, a measured operation ends with an explicit commit, so its
 * power cuts land in the commit that makes it durable. */
static enum ext4_result
execute_durable(struct state *state, const struct plan *plan, bool apply)
{
	enum ext4_result error = execute(state, plan, apply);
	enum ext4_result commit;

	if (state->commit_blocks == 0) {
		return error;
	}
	commit = ext4_commit(state->fs);
	state->commits++;
	return commit != EXT4_OK ? commit : error;
}

static void
release_holds(struct state *state)
{
	struct plan plan;
	uint32_t index;

	memset(&plan, 0, sizeof(plan));
	plan.operation = OP_RELEASE;
	for (index = 0; index < OBJECT_LIMIT; index++) {
		if (state->objects[index].hold != NULL) {
			plan.object = index;
			execute_hold(state, &plan);
		}
	}
}

/* Clean shutdown, a read-only verification mount, then a new writable owner. */
static void
remount(struct state *state)
{
	release_holds(state);
	EXPECT(ext4_sync(state->fs), EXT4_OK);
	ext4_unmount(state->fs);
	CHECK(state->device.live == 0);
	EXPECT(ext4_mount(&state->device.environment, &state->fs), EXT4_OK);
	verify(state);
	ext4_unmount(state->fs);
	mount_writer(state);
	state->remounts++;
}

static void
clean_snapshot(struct state *state, uint8_t *snapshot)
{
	struct device *device = &state->device;

	EXPECT(ext4_sync(state->fs), EXT4_OK);
	ext4_unmount(state->fs);
	CHECK(device->live == 0 && !device->off);
	memcpy(snapshot, device->cache, device->size);
}

/* Measure the plan from a clean snapshot without changing the model, repeat it
 * from the same bytes with a power cut at a random write or barrier, then apply
 * it to the model and require the same new image. */
static enum ext4_result
execute_with_crash(struct state *state, const struct plan *plan)
{
	struct device *device = &state->device;
	struct ext4_recovery_report report;
	struct ext4_super_disk *super;
	uint32_t clock;
	uint32_t events;
	bool committed;
	enum ext4_result expected;
	enum ext4_result error;

	clean_snapshot(state, state->pre);
	clock = state->clock;
	device_reset(device, state->pre);
	mount_writer(state);
	events = device->events;
	expected = execute_durable(state, plan, false);
	events = device->events - events;
	clean_snapshot(state, state->post);
	if (events != 0) {
		device_reset(device, state->pre);
		mount_writer(state);
		state->clock = clock;
		device->stop_at = device->events + 1U + pick(state, events);
		device->survival = pick(state, 3);
		device->partial = pick(state, 2) != 0;
		state->crashing = true;
		error = execute_durable(state, plan, false);
		state->crashing = false;
		CHECK(device->off && error != EXT4_OK && error != expected);
		ext4_unmount(state->fs);
		committed = device->intent_durable;
		state->crashes++;
		device_reset(device, device->stable);
		error = ext4_recover(&device->environment, &device->writer, &report);
		if (error == EXT4_CORRUPT) {
			super = (struct ext4_super_disk *)(device->cache + EXT4_SUPER_OFFSET);
			CHECK(device->metadata_checksum && device->writes == 0 &&
			    ext4_le32(&super->checksum) !=
				ext4_crc32c(
				    UINT32_MAX, super, offsetof(struct ext4_super_disk, checksum)));
			state->torn_superblocks++;
		} else {
			EXPECT(error, EXT4_OK);
			CHECK(device->live == 0 &&
			    memcmp(device->cache, device->stable, device->size) == 0);
			CHECK(storage_equal(device, state->post) ||
			    (!committed && storage_equal(device, state->pre)));
			state->crash_committed += storage_equal(device, state->post);
		}
	}
	device_reset(device, state->pre);
	mount_writer(state);
	state->clock = clock;
	error = execute(state, plan, true);
	CHECK(error == expected);
	clean_snapshot(state, state->pre);
	CHECK(memcmp(state->pre, state->post, device->size) == 0);
	device_reset(device, state->post);
	mount_writer(state);
	return expected;
}

static void
setup_root(struct state *state)
{
	struct ext4_inode root;
	struct ext4_inode inode;
	struct ext4_inode_update update = creation(state, 0755);
	struct ext4_timestamp time = now(state);
	struct object *object;

	memset(state->objects, 0, sizeof(state->objects));
	memset(state->entries, 0, sizeof(state->entries));
	EXPECT(ext4_get_inode(state->fs, EXT4_ROOT_INODE, &root), EXT4_OK);
	EXPECT(ext4_mkdir(state->fs, EXT4_ROOT_INODE, root.generation, (const uint8_t *)ROOT_NAME,
		   strlen(ROOT_NAME), &update, &time, &inode),
	    EXT4_OK);
	state->root = new_object(state, KIND_DIRECTORY, &inode);
	object = &state->objects[state->root];
	object->names = 1;
}

static void
write_manifest(struct state *state, const char *directory)
{
	char path[4096];
	char data_path[4096];
	FILE *stream;
	FILE *data;
	struct entry *entry;
	struct object *object;
	uint32_t chain[OBJECT_LIMIT];
	uint32_t depth;
	uint32_t index;
	uint32_t key;
	uint32_t parent;
	size_t offset;
	int length;

	length = snprintf(path, sizeof(path), "%s/%s", directory, MANIFEST_NAME);
	CHECK(length > 0 && (size_t)length < sizeof(path));
	stream = fopen(path, "wx");
	CHECK(stream != NULL);
	fprintf(stream, "root %s\n", ROOT_NAME);
	for (index = 0; index < ENTRY_LIMIT; index++) {
		entry = &state->entries[index];
		if (!entry->used) {
			continue;
		}
		object = &state->objects[entry->object];
		depth = 0;
		chain[depth++] = index;
		for (parent = entry->parent; parent != state->root;) {
			chain[depth] = entry_of(state, parent);
			CHECK(chain[depth] != NO_INDEX);
			parent = state->entries[chain[depth]].parent;
			depth++;
		}
		fprintf(stream, "%s %u %u %o ",
		    object->kind == KIND_FILE || object->kind == KIND_BALLAST ? "file"
			: object->kind == KIND_DIRECTORY		      ? "directory"
			: object->kind == KIND_SYMLINK			      ? "symlink"
			: object->kind == KIND_FIFO			      ? "fifo"
									      : "device",
		    object->number,
		    object->kind == KIND_DIRECTORY ? 2U + object->subdirectories : object->names,
		    object->permissions);
		while (depth > 0) {
			depth--;
			entry = &state->entries[chain[depth]];
			fprintf(
			    stream, "/%.*s", (int)entry->name_length, (const char *)entry->name);
		}
		fprintf(stream, "\n");
		if (object->kind == KIND_FILE || object->kind == KIND_SYMLINK ||
		    object->kind == KIND_BALLAST) {
			length = snprintf(data_path, sizeof(data_path), "%s/object-%u.data",
			    directory, object->number);
			CHECK(length > 0 && (size_t)length < sizeof(data_path));
			data = fopen(data_path, "wb");
			CHECK(data != NULL);
			offset = (size_t)object->size;
			CHECK(fwrite(object->kind == KIND_BALLAST ? state->ballast : object->data,
				  1, offset, data) == offset);
			CHECK(fclose(data) == 0);
		}
		for (key = 0; key < XATTR_KEYS; key++) {
			if (object->xattr_present[key]) {
				fprintf(stream, "xattr %u user.k%u ", object->number, key);
				for (offset = 0; offset < object->xattr_length[key]; offset++) {
					fprintf(stream, "%02x", object->xattr_value[key][offset]);
				}
				fprintf(stream, "\n");
			}
		}
	}
	CHECK(fclose(stream) == 0);
}

/* Describe the final shape so a run can be judged against its purpose. */
static void
report_shape(struct state *state)
{
	struct ext4_inode inode;
	struct ext4_info info;
	uint64_t bytes = 0;
	uint32_t entries = 0;
	uint32_t widest = 0;
	uint32_t indexed = 0;
	uint32_t index;
	struct object *object;

	for (index = 0; index < ENTRY_LIMIT; index++) {
		entries += state->entries[index].used;
	}
	for (index = 0; index < OBJECT_LIMIT; index++) {
		object = &state->objects[index];
		if (object->kind == KIND_DIRECTORY) {
			EXPECT(ext4_get_inode(state->fs, object->number, &inode), EXT4_OK);
			indexed += (inode.flags & INDEX_FLAG) != 0;
			if (object->children > widest) {
				widest = object->children;
			}
		} else if (object->kind == KIND_FILE || object->kind == KIND_BALLAST) {
			bytes += object->size;
		}
	}
	ext4_get_info(state->fs, &info);
	printf("SHAPE entries=%u widest=%u indexed=%u file_bytes=%" PRIu64 " free_blocks=%" PRIu64
	       " blocks=%" PRIu64 " no_space=%u\n",
	    entries, widest, indexed, bytes, info.free_blocks, info.blocks, state->no_space);
}

static uint32_t
parse_number(const char *text)
{
	unsigned long value;
	char *end;

	value = strtoul(text, &end, 0);
	CHECK(*text != 0 && *end == 0 && value != 0 && value <= UINT32_MAX);
	return (uint32_t)value;
}

int
main(int argc, char **argv)
{
	static struct state state;
	struct plan plan;
	struct ext4_info info;
	const char *export_directory = NULL;
	uint64_t seed;
	uint32_t operations;
	uint32_t performed = 0;
	uint32_t attempts = 0;
	uint32_t index;
	int argument;
	char *end;
	enum ext4_result error;

	if (argc < 4) {
		fprintf(stderr,
		    "usage: %s IMAGE SEED OPERATIONS [--objects N] [--entries N] "
		    "[--directories N] [--commit-blocks N] [--export DIRECTORY]\n",
		    argv[0]);
		return 2;
	}
	seed = strtoull(argv[2], &end, 0);
	CHECK(*argv[2] != 0 && *end == 0);
	operations = parse_number(argv[3]);
	state.object_limit = DEFAULT_OBJECTS;
	state.entry_limit = DEFAULT_ENTRIES;
	state.directory_limit = DEFAULT_DIRECTORIES;
	for (argument = 4; argument < argc; argument += 2) {
		CHECK(argument + 1 < argc);
		if (strcmp(argv[argument], "--objects") == 0) {
			state.object_limit = parse_number(argv[argument + 1]);
		} else if (strcmp(argv[argument], "--entries") == 0) {
			state.entry_limit = parse_number(argv[argument + 1]);
		} else if (strcmp(argv[argument], "--directories") == 0) {
			state.directory_limit = parse_number(argv[argument + 1]);
		} else if (strcmp(argv[argument], "--commit-blocks") == 0) {
			state.commit_blocks = parse_number(argv[argument + 1]);
		} else if (strcmp(argv[argument], "--export") == 0) {
			export_directory = argv[argument + 1];
		} else {
			CHECK(false);
		}
	}
	CHECK(state.object_limit <= OBJECT_LIMIT && state.entry_limit <= ENTRY_LIMIT);
	storage_open(&state.device, argv[1]);
	state.random = seed;
	state.pre = malloc(state.device.size);
	state.post = malloc(state.device.size);
	state.buffer = malloc(FILE_LIMIT);
	state.ballast = malloc(2U * BALLAST_BYTES);
	CHECK(state.pre != NULL && state.post != NULL && state.buffer != NULL &&
	    state.ballast != NULL);
	fill_pattern(state.ballast, seed, BALLAST_BYTES);
	mount_writer(&state);
	ext4_get_info(state.fs, &info);
	state.block_size = info.block_size;
	state.extents = (info.feature_incompat & EXT4_FEATURE_INCOMPAT_EXTENTS) != 0;
	setup_root(&state);
	while (performed < operations) {
		CHECK(++attempts < operations * 64U);
		if (!plan_operation(&state, &plan)) {
			continue;
		}
		if (crash_eligible(&state, &plan) && pick(&state, CRASH_INTERVAL) == 0) {
			error = execute_with_crash(&state, &plan);
		} else {
			error = execute(&state, &plan, true);
		}
		CHECK(!state.fs->aborted && state.device.live != 0);
		if (error == EXT4_OK) {
			state.performed[plan.operation]++;
		} else {
			state.rejected[plan.operation]++;
			state.no_space += error == EXT4_NO_SPACE;
		}
		performed++;
		if (performed % VERIFY_INTERVAL == 0) {
			verify(&state);
		}
		if (performed % REMOUNT_INTERVAL == 0) {
			remount(&state);
		}
	}
	remount(&state);
	verify(&state);
	release_holds(&state);
	report_shape(&state);
	EXPECT(ext4_sync(state.fs), EXT4_OK);
	ext4_unmount(state.fs);
	CHECK(state.device.live == 0);
	if (export_directory != NULL) {
		memcpy(state.device.stable, state.device.cache, state.device.size);
		storage_export(&state.device, export_directory, argv[1], "sustained-");
		write_manifest(&state, export_directory);
	}
	for (index = 0; index < OBJECT_LIMIT; index++) {
		free(state.objects[index].data);
	}
	printf("PASS %u operations seed=%" PRIu64 " objects=%u entries=%u directories=%u "
	       "verifications=%u remounts=%u crashes=%u committed=%u torn=%u commit_blocks=%u "
	       "commits=%u\n",
	    operations, seed, state.object_limit, state.entry_limit, state.directory_limit,
	    state.verifications, state.remounts, state.crashes, state.crash_committed,
	    state.torn_superblocks, state.commit_blocks, state.commits);
	for (index = 0; index < OP_COUNT; index++) {
		printf("%s performed=%u rejected=%u\n", operation_names[index],
		    state.performed[index], state.rejected[index]);
	}
	free(state.ballast);
	free(state.buffer);
	free(state.post);
	free(state.pre);
	storage_close(&state.device);
	return 0;
}

/* SPDX-License-Identifier: BSD-3-Clause */
#include "quota.h"
#include "allocate.h"

#define EXT4_QUOTA_CHANGES_INITIAL 32U
#define EXT4_QUOTA_LOW_BYTE 0xffU

enum ext4_quota_type { EXT4_QUOTA_USER, EXT4_QUOTA_GROUP, EXT4_QUOTA_PROJECT };

static const uint32_t ext4_quota_magics[EXT4_QUOTA_TYPES] = { EXT4_QUOTA_USER_MAGIC,
	EXT4_QUOTA_GROUP_MAGIC, EXT4_QUOTA_PROJECT_MAGIC };

/* What one inode record charges: its IDs, bytes and inode count. */
struct ext4_quota_usage {
	uint32_t ids[EXT4_QUOTA_TYPES];
	uint64_t space;
	uint64_t inodes;
	bool charged;
};

struct ext4_quota_change {
	uint32_t type;
	uint32_t id;
	int64_t space;
	int64_t inodes;
};

struct ext4_quota_state {
	struct ext4_fs *fs;
	struct ext4_transaction *transaction;
	uint8_t *previous;
	uint8_t *scratch;
	uint8_t *quota_block;
	struct ext4_quota_change *changes;
	size_t change_count;
	size_t change_capacity;
	/* Seconds from the adapter's clock while its policy enforces limits. */
	int64_t now;
	bool enforcing;
};

/* One quota file while its usage changes inside a commit. The record is the
 * transaction's current copy; growth enrolls it and switches to that buffer. */
struct ext4_quota_file {
	struct ext4_quota_state *state;
	struct ext4_allocation allocation;
	struct ext4_inode inode;
	struct ext4_inode_disk *disk;
	uint8_t *record;
	uint32_t type;
	/* Grace periods from the quota file's information block, when enforced. */
	int64_t block_grace;
	int64_t inode_grace;
	bool enforced;
	bool allocation_ready;
	bool grown;
};

static enum ext4_result ext4_quota_read(struct ext4_quota_file *file, uint32_t quota_block);

static uint64_t
ext4_quota_le64(const struct ext4_le32 value[2])
{
	return (uint64_t)ext4_le32(&value[0]) | (uint64_t)ext4_le32(&value[1]) << 32;
}

static void
ext4_quota_encode64(struct ext4_le32 value[2], uint64_t number)
{
	ext4_encode32(&value[0], (uint32_t)number);
	ext4_encode32(&value[1], (uint32_t)(number >> 32));
}

static bool
ext4_quota_zero(const void *buffer, size_t length)
{
	const uint8_t *bytes = buffer;
	size_t index;

	for (index = 0; index < length; index++) {
		if (bytes[index] != 0) {
			return false;
		}
	}
	return true;
}

bool
ext4_quota_system_inode(const struct ext4_fs *fs, uint32_t number)
{
	uint32_t type;

	for (type = 0; type < EXT4_QUOTA_TYPES; type++) {
		if (fs->quota_inodes[type] != 0 && fs->quota_inodes[type] == number) {
			return true;
		}
	}
	return false;
}

enum ext4_result
ext4_quota_super_validate(struct ext4_fs *fs, const struct ext4_super_disk *super)
{
	uint32_t numbers[EXT4_QUOTA_TYPES];
	uint32_t type;

	ext4_zero(fs->quota_inodes, sizeof(fs->quota_inodes));
	if ((fs->info.feature_ro_compat & EXT4_FEATURE_RO_PROJECT) &&
	    fs->inode_size <= EXT4_INODE_BASE_SIZE) {
		/* Linux refuses project IDs without room for i_projid. */
		return EXT4_UNSUPPORTED;
	}
	if (!(fs->info.feature_ro_compat & EXT4_FEATURE_RO_QUOTA)) {
		return EXT4_OK;
	}
	numbers[EXT4_QUOTA_USER] = ext4_le32(&super->user_quota_inode);
	numbers[EXT4_QUOTA_GROUP] = ext4_le32(&super->group_quota_inode);
	numbers[EXT4_QUOTA_PROJECT] = ext4_le32(&super->project_quota_inode);
	if ((numbers[EXT4_QUOTA_USER] != 0 && numbers[EXT4_QUOTA_USER] != EXT4_USER_QUOTA_INODE) ||
	    (numbers[EXT4_QUOTA_GROUP] != 0 &&
		numbers[EXT4_QUOTA_GROUP] != EXT4_GROUP_QUOTA_INODE)) {
		return EXT4_CORRUPT;
	}
	if (numbers[EXT4_QUOTA_PROJECT] != 0 &&
	    (!(fs->info.feature_ro_compat & EXT4_FEATURE_RO_PROJECT) ||
		numbers[EXT4_QUOTA_PROJECT] < fs->first_inode ||
		numbers[EXT4_QUOTA_PROJECT] > fs->info.inodes ||
		numbers[EXT4_QUOTA_PROJECT] == fs->journal_inode)) {
		return EXT4_CORRUPT;
	}
	for (type = 0; type < EXT4_QUOTA_TYPES; type++) {
		fs->quota_inodes[type] = numbers[type];
	}
	return EXT4_OK;
}

static void
ext4_quota_runs_sift(struct ext4_inode_table_run *runs, size_t root, size_t count)
{
	struct ext4_inode_table_run saved;
	size_t child;

	while (root * 2U + 1U < count) {
		child = root * 2U + 1U;
		if (child + 1U < count && runs[child].block < runs[child + 1U].block) {
			child++;
		}
		if (runs[root].block >= runs[child].block) {
			return;
		}
		saved = runs[root];
		runs[root] = runs[child];
		runs[child] = saved;
		root = child;
	}
}

static void
ext4_quota_runs_sort(struct ext4_inode_table_run *runs, size_t count)
{
	struct ext4_inode_table_run saved;
	size_t index;

	for (index = count / 2U; index > 0; index--) {
		ext4_quota_runs_sift(runs, index - 1U, count);
	}
	for (index = count; index > 1; index--) {
		saved = runs[0];
		runs[0] = runs[index - 1U];
		runs[index - 1U] = saved;
		ext4_quota_runs_sift(runs, 0, index - 1U);
	}
}

/* Record where each group's inode table lives so commit can recognize changed
 * inode records among its snapshots. FLEX_BG tables usually merge into a few runs. */
static enum ext4_result
ext4_quota_runs_build(struct ext4_fs *fs)
{
	struct ext4_group group;
	struct ext4_inode_table_run *run;
	uint32_t index;
	enum ext4_result error;

	fs->inode_table_runs = fs->environment.allocate(
	    fs->environment.context, (size_t)fs->info.groups * sizeof(*fs->inode_table_runs));
	if (fs->inode_table_runs == NULL) {
		return EXT4_NO_MEMORY;
	}
	fs->inode_table_run_count = 0;
	for (index = 0; index < fs->info.groups; index++) {
		error = ext4_group_get(fs, index, &group);
		if (error != EXT4_OK) {
			return error;
		}
		if (index == 0) {
			fs->inode_table_blocks = group.table_blocks;
		}
		if (group.table_blocks != fs->inode_table_blocks || group.table_blocks == 0) {
			return EXT4_CORRUPT;
		}
		run = fs->inode_table_run_count == 0
		    ? NULL
		    : &fs->inode_table_runs[fs->inode_table_run_count - 1U];
		if (run != NULL && run->first_group + run->groups == index &&
		    run->block + (uint64_t)run->groups * fs->inode_table_blocks ==
			group.inode_table) {
			run->groups++;
			continue;
		}
		run = &fs->inode_table_runs[fs->inode_table_run_count++];
		run->block = group.inode_table;
		run->first_group = index;
		run->groups = 1;
	}
	ext4_quota_runs_sort(fs->inode_table_runs, fs->inode_table_run_count);
	return EXT4_OK;
}

static enum ext4_result
ext4_quota_file_validate(struct ext4_fs *fs, uint32_t type)
{
	struct ext4_quota_header_disk *header;
	struct ext4_inode inode;
	uint8_t *buffer = NULL;
	uint64_t physical;
	uint32_t blocks;
	enum ext4_result error;

	error = ext4_get_inode(fs, fs->quota_inodes[type], &inode);
	if (error != EXT4_OK) {
		return error == EXT4_NOT_FOUND ? EXT4_CORRUPT : error;
	}
	if ((inode.mode & EXT4_MODE_TYPE) != EXT4_MODE_REGULAR ||
	    inode.size < (uint64_t)(EXT4_QUOTA_TREE_ROOT + 1U) * EXT4_QUOTA_BLOCK_SIZE ||
	    inode.size % EXT4_QUOTA_BLOCK_SIZE != 0 ||
	    inode.size > (uint64_t)UINT32_MAX * EXT4_QUOTA_BLOCK_SIZE) {
		return EXT4_CORRUPT;
	}
	error = ext4_map_block(fs, &inode, 0, &physical);
	if (error != EXT4_OK) {
		return error;
	}
	if (physical == 0) {
		return EXT4_CORRUPT;
	}
	buffer = fs->environment.allocate(fs->environment.context, fs->info.block_size);
	if (buffer == NULL) {
		return EXT4_NO_MEMORY;
	}
	error = ext4_block_read(fs, physical, buffer);
	header = (struct ext4_quota_header_disk *)buffer;
	blocks = ext4_le32(&header->blocks);
	if (error == EXT4_OK &&
	    (ext4_le32(&header->magic) != ext4_quota_magics[type] ||
		ext4_le32(&header->version) != EXT4_QUOTA_VERSION)) {
		error = EXT4_UNSUPPORTED;
	}
	if (error == EXT4_OK &&
	    (blocks <= EXT4_QUOTA_TREE_ROOT ||
		(uint64_t)blocks * EXT4_QUOTA_BLOCK_SIZE > inode.size ||
		ext4_le32(&header->free_block) >= blocks ||
		ext4_le32(&header->free_entry) >= blocks)) {
		error = EXT4_CORRUPT;
	}
	fs->environment.release(fs->environment.context, buffer, fs->info.block_size);
	return error;
}

enum ext4_result
ext4_quota_open(struct ext4_fs *fs)
{
	uint32_t type;
	enum ext4_result error;

	if (fs->quota_active || !(fs->info.feature_ro_compat & EXT4_FEATURE_RO_QUOTA)) {
		return EXT4_OK;
	}
	for (type = 0; type < EXT4_QUOTA_TYPES; type++) {
		if (fs->quota_inodes[type] == 0) {
			continue;
		}
		error = ext4_quota_file_validate(fs, type);
		if (error != EXT4_OK) {
			return error;
		}
	}
	error = ext4_quota_runs_build(fs);
	if (error != EXT4_OK) {
		ext4_quota_close(fs);
		return error;
	}
	fs->quota_active = true;
	return EXT4_OK;
}

void
ext4_quota_close(struct ext4_fs *fs)
{
	if (fs->inode_table_runs != NULL) {
		fs->environment.release(fs->environment.context, fs->inode_table_runs,
		    (size_t)fs->info.groups * sizeof(*fs->inode_table_runs));
		fs->inode_table_runs = NULL;
	}
	fs->inode_table_run_count = 0;
	fs->quota_active = false;
}

/* Read a block as the transaction currently sees it, or as the device still
 * holds it for the previous state. The result lives in scratch. */
static enum ext4_result
ext4_quota_view(struct ext4_quota_state *state, uint64_t block, bool current)
{
	return current ? ext4_transaction_read(state->transaction, block, state->scratch)
		       : ext4_block_read(state->fs, block, state->scratch);
}

static enum ext4_result
ext4_quota_inode_allocated(
    struct ext4_quota_state *state, uint32_t number, bool current, bool *allocated)
{
	struct ext4_fs *fs = state->fs;
	struct ext4_group group;
	uint64_t offset;
	uint32_t index = (number - 1U) / fs->inodes_per_group;
	uint32_t bit = (number - 1U) % fs->inodes_per_group;
	enum ext4_result error;

	*allocated = false;
	error = ext4_group_descriptor_offset(fs, index, &offset);
	if (error == EXT4_OK) {
		error = ext4_quota_view(state, offset / fs->info.block_size, current);
	}
	if (error == EXT4_OK) {
		error = ext4_group_decode(fs, index,
		    (struct ext4_group_disk *)(state->scratch + offset % fs->info.block_size),
		    &group);
	}
	if (error != EXT4_OK || (group.flags & EXT4_GROUP_INODE_UNINIT)) {
		return error;
	}
	error = ext4_quota_view(state, group.inode_bitmap, current);
	if (error == EXT4_OK) {
		*allocated =
		    (state->scratch[bit / EXT4_BITS_PER_BYTE] >> (bit % EXT4_BITS_PER_BYTE)) & 1U;
	}
	return error;
}

/* Count attribute entries that reference value inodes; each charges one inode. */
static enum ext4_result
ext4_quota_value_references(const uint8_t *first, const uint8_t *end, uint64_t *count)
{
	const struct ext4_xattr_entry_disk *entry;
	const uint8_t *cursor = first;
	size_t size;

	while ((size_t)(end - cursor) >= sizeof(struct ext4_le32) &&
	    !ext4_quota_zero(cursor, sizeof(struct ext4_le32))) {
		if ((size_t)(end - cursor) < sizeof(*entry)) {
			return EXT4_CORRUPT;
		}
		entry = (const struct ext4_xattr_entry_disk *)cursor;
		size = (sizeof(*entry) + entry->name_length + EXT4_XATTR_ALIGNMENT - 1U) &
		    ~(size_t)(EXT4_XATTR_ALIGNMENT - 1U);
		if (size > (size_t)(end - cursor)) {
			return EXT4_CORRUPT;
		}
		if (ext4_le32(&entry->value_inode) != 0) {
			(*count)++;
		}
		cursor += size;
	}
	return EXT4_OK;
}

static enum ext4_result
ext4_quota_value_inodes(struct ext4_quota_state *state, const struct ext4_inode_disk *disk,
    bool current, uint64_t *count)
{
	struct ext4_fs *fs = state->fs;
	const uint8_t *record = (const uint8_t *)disk;
	uint64_t block = ext4_le32(&disk->xattr_block_lo);
	size_t start;
	enum ext4_result error;

	*count = 0;
	if (!(fs->info.feature_incompat & EXT4_FEATURE_INCOMPAT_EA_INODE)) {
		return EXT4_OK;
	}
	if (fs->inode_size > EXT4_INODE_BASE_SIZE) {
		start = EXT4_INODE_BASE_SIZE + ext4_le16(&disk->extra_size);
		if (start + sizeof(struct ext4_le32) <= fs->inode_size &&
		    ext4_le32((const struct ext4_le32 *)(record + start)) == EXT4_XATTR_MAGIC) {
			error =
			    ext4_quota_value_references(record + start + sizeof(struct ext4_le32),
				record + fs->inode_size, count);
			if (error != EXT4_OK) {
				return error;
			}
		}
	}
	if (fs->info.feature_incompat & EXT4_FEATURE_INCOMPAT_64BIT) {
		block |= (uint64_t)ext4_le16(&disk->xattr_block_hi) << 32;
	}
	if (block == 0) {
		return EXT4_OK;
	}
	if (block >= fs->info.blocks) {
		return EXT4_CORRUPT;
	}
	error = ext4_quota_view(state, block, current);
	if (error != EXT4_OK) {
		return error;
	}
	if (ext4_le32((const struct ext4_le32 *)state->scratch) != EXT4_XATTR_MAGIC) {
		return EXT4_CORRUPT;
	}
	return ext4_quota_value_references(state->scratch + sizeof(struct ext4_xattr_header_disk),
	    state->scratch + fs->info.block_size, count);
}

/* Linux and e2fsck charge every in-use inode except reserved and system inodes
 * and private attribute-value inodes. An unlinked inode stays charged until its
 * final deletion releases its allocation-bitmap bit. A reallocated slot's former
 * bytes may be stale table contents, so verify also consults the bitmap. */
static enum ext4_result
ext4_quota_usage(struct ext4_quota_state *state, uint32_t number,
    const struct ext4_inode_disk *disk, bool current, bool verify, struct ext4_quota_usage *usage)
{
	struct ext4_fs *fs = state->fs;
	uint64_t blocks;
	uint64_t references;
	uint32_t flags = ext4_le32(&disk->flags);
	uint16_t extra_size = 0;
	bool allocated = true;
	enum ext4_result error;

	ext4_zero(usage, sizeof(*usage));
	if ((number != EXT4_ROOT_INODE && number < fs->first_inode) ||
	    ext4_quota_system_inode(fs, number) || number == fs->orphan_file_inode ||
	    number == fs->journal_inode || ext4_le16(&disk->mode) == 0 ||
	    (flags & EXT4_INODE_EA_INODE)) {
		return EXT4_OK;
	}
	if (verify || ext4_le16(&disk->links) == 0) {
		error = ext4_quota_inode_allocated(state, number, current, &allocated);
		if (error != EXT4_OK || !allocated) {
			return error;
		}
	}
	if (fs->inode_size > EXT4_INODE_BASE_SIZE) {
		extra_size = ext4_le16(&disk->extra_size);
	}
	usage->ids[EXT4_QUOTA_USER] =
	    (uint32_t)ext4_le16(&disk->uid_lo) | (uint32_t)ext4_le16(&disk->uid_hi) << 16;
	usage->ids[EXT4_QUOTA_GROUP] =
	    (uint32_t)ext4_le16(&disk->gid_lo) | (uint32_t)ext4_le16(&disk->gid_hi) << 16;
	if (EXT4_INODE_HAS_FIELD(extra_size, project_id) &&
	    extra_size <= fs->inode_size - EXT4_INODE_BASE_SIZE) {
		usage->ids[EXT4_QUOTA_PROJECT] = ext4_le32(&disk->project_id);
	}
	blocks = ext4_le32(&disk->blocks_lo);
	if (fs->info.feature_ro_compat & EXT4_FEATURE_RO_HUGE_FILE) {
		blocks |= (uint64_t)ext4_le16(&disk->blocks_hi) << 32;
	}
	/* Attribute-value charges are already part of the owner's i_blocks. */
	usage->space = (fs->info.feature_ro_compat & EXT4_FEATURE_RO_HUGE_FILE) &&
		(flags & EXT4_INODE_HUGE_FILE)
	    ? blocks * fs->info.block_size
	    : blocks * EXT4_SECTOR_SIZE;
	error = ext4_quota_value_inodes(state, disk, current, &references);
	if (error != EXT4_OK) {
		return error;
	}
	usage->inodes = references + 1U;
	usage->charged = true;
	return EXT4_OK;
}

static enum ext4_result
ext4_quota_change_add(
    struct ext4_quota_state *state, uint32_t type, uint32_t id, int64_t space, int64_t inodes)
{
	struct ext4_fs *fs = state->fs;
	struct ext4_quota_change *changes;
	size_t capacity;
	size_t index;

	for (index = 0; index < state->change_count; index++) {
		if (state->changes[index].type == type && state->changes[index].id == id) {
			state->changes[index].space += space;
			state->changes[index].inodes += inodes;
			return EXT4_OK;
		}
	}
	if (state->change_count == state->change_capacity) {
		capacity = state->change_capacity == 0 ? EXT4_QUOTA_CHANGES_INITIAL
						       : state->change_capacity * 2U;
		if (capacity > SIZE_MAX / sizeof(*changes)) {
			return EXT4_NO_MEMORY;
		}
		changes =
		    fs->environment.allocate(fs->environment.context, capacity * sizeof(*changes));
		if (changes == NULL) {
			return EXT4_NO_MEMORY;
		}
		if (state->changes != NULL) {
			ext4_copy(changes, state->changes, state->change_count * sizeof(*changes));
			fs->environment.release(fs->environment.context, state->changes,
			    state->change_capacity * sizeof(*changes));
		}
		state->changes = changes;
		state->change_capacity = capacity;
	}
	state->changes[state->change_count].type = type;
	state->changes[state->change_count].id = id;
	state->changes[state->change_count].space = space;
	state->changes[state->change_count].inodes = inodes;
	state->change_count++;
	return EXT4_OK;
}

static enum ext4_result
ext4_quota_charge(struct ext4_quota_state *state, const struct ext4_quota_usage *usage, int sign)
{
	uint32_t type;
	enum ext4_result error;

	if (!usage->charged) {
		return EXT4_OK;
	}
	if (usage->space > INT64_MAX || usage->inodes > INT64_MAX) {
		return EXT4_RANGE;
	}
	for (type = 0; type < EXT4_QUOTA_TYPES; type++) {
		if (state->fs->quota_inodes[type] == 0) {
			continue;
		}
		error = ext4_quota_change_add(state, type, usage->ids[type],
		    sign * (int64_t)usage->space, sign * (int64_t)usage->inodes);
		if (error != EXT4_OK) {
			return error;
		}
	}
	return EXT4_OK;
}

static const struct ext4_inode_table_run *
ext4_quota_run(const struct ext4_fs *fs, uint64_t block)
{
	size_t low = 0;
	size_t high = fs->inode_table_run_count;
	size_t middle;
	const struct ext4_inode_table_run *run;

	while (low < high) {
		middle = low + (high - low) / 2U;
		run = &fs->inode_table_runs[middle];
		if (block < run->block) {
			high = middle;
		} else if (block - run->block >= (uint64_t)run->groups * fs->inode_table_blocks) {
			low = middle + 1U;
		} else {
			return run;
		}
	}
	return NULL;
}

/* Compare one changed inode-table block with its committed contents. */
static enum ext4_result
ext4_quota_collect_block(struct ext4_quota_state *state, uint64_t block, const uint8_t *current)
{
	struct ext4_fs *fs = state->fs;
	const struct ext4_inode_table_run *run = ext4_quota_run(fs, block);
	const struct ext4_inode_disk *previous;
	const struct ext4_inode_disk *next;
	struct ext4_quota_usage before;
	struct ext4_quota_usage after;
	uint64_t position;
	uint32_t per_block = fs->info.block_size / fs->inode_size;
	uint32_t group;
	uint32_t number;
	uint32_t slot;
	enum ext4_result error;

	if (run == NULL) {
		return EXT4_OK;
	}
	position = block - run->block;
	group = run->first_group + (uint32_t)(position / fs->inode_table_blocks);
	number = group * fs->inodes_per_group +
	    (uint32_t)(position % fs->inode_table_blocks) * per_block + 1U;
	error = ext4_block_read(fs, block, state->previous);
	if (error != EXT4_OK) {
		return error;
	}
	for (slot = 0; slot < per_block; slot++, number++) {
		if (number > fs->info.inodes || number > (group + 1U) * fs->inodes_per_group) {
			break;
		}
		if (ext4_equal(current + (size_t)slot * fs->inode_size,
			state->previous + (size_t)slot * fs->inode_size, fs->inode_size)) {
			continue;
		}
		previous = (const struct ext4_inode_disk *)(state->previous +
		    (size_t)slot * fs->inode_size);
		next = (const struct ext4_inode_disk *)(current + (size_t)slot * fs->inode_size);
		error = ext4_quota_usage(state, number, previous, false,
		    ext4_le32(&previous->generation) != ext4_le32(&next->generation), &before);
		if (error == EXT4_OK) {
			error = ext4_quota_usage(state, number, next, true, false, &after);
		}
		if (error == EXT4_OK) {
			error = ext4_quota_charge(state, &before, -1);
		}
		if (error == EXT4_OK) {
			error = ext4_quota_charge(state, &after, 1);
		}
		if (error != EXT4_OK) {
			return error;
		}
	}
	return EXT4_OK;
}

static enum ext4_result
ext4_quota_file_open(struct ext4_quota_state *state, uint32_t type, struct ext4_quota_file *file)
{
	struct ext4_fs *fs = state->fs;
	const struct ext4_quota_header_disk *header;
	uint64_t offset;
	uint32_t number = fs->quota_inodes[type];
	enum ext4_result error;

	ext4_zero(file, sizeof(*file));
	file->state = state;
	file->type = type;
	file->record = fs->environment.allocate(fs->environment.context, fs->inode_size);
	if (file->record == NULL) {
		return EXT4_NO_MEMORY;
	}
	error = ext4_inode_location(fs, number, &offset);
	if (error == EXT4_OK) {
		error = ext4_quota_view(state, offset / fs->info.block_size, true);
	}
	if (error != EXT4_OK) {
		return error;
	}
	ext4_copy(file->record, state->scratch + offset % fs->info.block_size, fs->inode_size);
	error = ext4_inode_decode(fs, number, file->record, &file->inode);
	if (error != EXT4_OK) {
		return error;
	}
	if ((file->inode.mode & EXT4_MODE_TYPE) != EXT4_MODE_REGULAR ||
	    file->inode.size % EXT4_QUOTA_BLOCK_SIZE != 0) {
		return EXT4_CORRUPT;
	}
	error = ext4_allocation_init(&file->allocation, fs, state->transaction, &file->inode);
	file->allocation_ready = true;
	if (error == EXT4_OK) {
		error = ext4_allocation_super(&file->allocation);
	}
	/* Like Linux quota writes, metadata growth may use the reserved pool. */
	file->allocation.reserved_blocks = 0;
	file->enforced = state->enforcing && (fs->quota_policy.types & (1U << type));
	if (error == EXT4_OK && file->enforced) {
		error = ext4_quota_read(file, 0);
	}
	if (error == EXT4_OK && file->enforced) {
		header = (const struct ext4_quota_header_disk *)state->quota_block;
		file->block_grace = ext4_le32(&header->block_grace);
		file->inode_grace = ext4_le32(&header->inode_grace);
	}
	return error;
}

static enum ext4_result
ext4_quota_file_close(struct ext4_quota_file *file, enum ext4_result error)
{
	struct ext4_fs *fs = file->state->fs;

	if (error == EXT4_OK && file->grown) {
		error = ext4_inode_account(
		    &file->allocation, &file->inode, file->disk, file->inode.size);
		if (error == EXT4_OK) {
			ext4_inode_checksum_set(fs, fs->quota_inodes[file->type], file->disk);
		}
	}
	if (file->allocation_ready) {
		ext4_allocation_destroy(&file->allocation);
	}
	/* After growth the record is the transaction's buffer. */
	if (file->record != NULL && file->disk == NULL) {
		fs->environment.release(fs->environment.context, file->record, fs->inode_size);
	}
	return error;
}

static enum ext4_result
ext4_quota_locate(
    struct ext4_quota_file *file, uint32_t quota_block, uint64_t *physical, uint32_t *offset)
{
	struct ext4_fs *fs = file->state->fs;
	struct ext4_map_run run;
	uint64_t byte = (uint64_t)quota_block << EXT4_QUOTA_BLOCK_BITS;
	enum ext4_result error;

	if (byte + EXT4_QUOTA_BLOCK_SIZE > file->inode.size) {
		return EXT4_CORRUPT;
	}
	error = ext4_write_map_lookup(&file->allocation, &file->inode,
	    (const struct ext4_inode_disk *)file->record, (uint32_t)(byte / fs->info.block_size),
	    &run);
	if (error != EXT4_OK) {
		return error;
	}
	if (run.physical == 0 || run.unwritten) {
		return EXT4_CORRUPT;
	}
	*physical = run.physical;
	*offset = (uint32_t)(byte % fs->info.block_size);
	return EXT4_OK;
}

static enum ext4_result
ext4_quota_read(struct ext4_quota_file *file, uint32_t quota_block)
{
	struct ext4_quota_state *state = file->state;
	uint64_t physical;
	uint32_t offset;
	enum ext4_result error;

	error = ext4_quota_locate(file, quota_block, &physical, &offset);
	if (error == EXT4_OK) {
		error = ext4_quota_view(state, physical, true);
	}
	if (error == EXT4_OK) {
		ext4_copy(state->quota_block, state->scratch + offset, EXT4_QUOTA_BLOCK_SIZE);
	}
	return error;
}

static enum ext4_result
ext4_quota_edit(struct ext4_quota_file *file, uint32_t quota_block, uint8_t **result)
{
	void *buffer = NULL;
	uint64_t physical;
	uint32_t offset;
	enum ext4_result error;

	error = ext4_quota_locate(file, quota_block, &physical, &offset);
	if (error == EXT4_OK) {
		error = ext4_transaction_buffer(file->state->transaction, physical, &buffer);
	}
	if (error == EXT4_OK) {
		*result = (uint8_t *)buffer + offset;
	}
	return error;
}

/* Extend the quota file by one 1 KiB block, allocating a zeroed filesystem block
 * when the new block starts one. Linux sizes the file by the bytes written. */
static enum ext4_result
ext4_quota_extend(struct ext4_quota_file *file, uint32_t quota_block)
{
	struct ext4_fs *fs = file->state->fs;
	struct ext4_map_run run;
	void *buffer = NULL;
	uint64_t byte = (uint64_t)quota_block << EXT4_QUOTA_BLOCK_BITS;
	uint64_t physical;
	uint64_t offset;
	uint32_t logical = (uint32_t)(byte / fs->info.block_size);
	bool zero;
	enum ext4_result error;

	if (byte != file->inode.size || byte > (uint64_t)UINT32_MAX * fs->info.block_size) {
		return EXT4_CORRUPT;
	}
	if (file->disk == NULL) {
		error = ext4_inode_location(fs, fs->quota_inodes[file->type], &offset);
		if (error == EXT4_OK) {
			error = ext4_transaction_buffer(
			    file->state->transaction, offset / fs->info.block_size, &buffer);
		}
		if (error != EXT4_OK) {
			return error;
		}
		file->disk =
		    (struct ext4_inode_disk *)((uint8_t *)buffer + offset % fs->info.block_size);
		fs->environment.release(fs->environment.context, file->record, fs->inode_size);
		file->record = (uint8_t *)file->disk;
		file->grown = true;
	}
	error = ext4_write_map_lookup(&file->allocation, &file->inode, file->disk, logical, &run);
	if (error != EXT4_OK) {
		return error;
	}
	if (run.physical == 0) {
		error = ext4_write_map_allocate(
		    &file->allocation, &file->inode, file->disk, logical, &physical, &zero);
		if (error == EXT4_OK) {
			error =
			    ext4_transaction_buffer(file->state->transaction, physical, &buffer);
		}
		if (error != EXT4_OK) {
			return error;
		}
		/* Later lookups read the extended map from the enrolled record. */
		ext4_zero(buffer, fs->info.block_size);
	} else if (run.unwritten) {
		return EXT4_CORRUPT;
	}
	file->inode.size = byte + EXT4_QUOTA_BLOCK_SIZE;
	return EXT4_OK;
}

static enum ext4_result
ext4_quota_free_block(
    struct ext4_quota_file *file, struct ext4_quota_header_disk *header, uint32_t *result)
{
	struct ext4_quota_leaf_disk *leaf;
	uint8_t *buffer = NULL;
	uint32_t block = ext4_le32(&header->free_block);
	uint32_t blocks = ext4_le32(&header->blocks);
	enum ext4_result error;

	if (block != 0) {
		if (block <= EXT4_QUOTA_TREE_ROOT || block >= blocks) {
			return EXT4_CORRUPT;
		}
		error = ext4_quota_edit(file, block, &buffer);
		if (error != EXT4_OK) {
			return error;
		}
		leaf = (struct ext4_quota_leaf_disk *)buffer;
		ext4_copy(&header->free_block, &leaf->next_free, sizeof(header->free_block));
	} else {
		if (blocks == UINT32_MAX) {
			return EXT4_RANGE;
		}
		block = blocks;
		error = ext4_quota_extend(file, block);
		if (error == EXT4_OK) {
			error = ext4_quota_edit(file, block, &buffer);
		}
		if (error != EXT4_OK) {
			return error;
		}
		ext4_encode32(&header->blocks, blocks + 1U);
	}
	ext4_zero(buffer, EXT4_QUOTA_BLOCK_SIZE);
	*result = block;
	return EXT4_OK;
}

static enum ext4_result
ext4_quota_unlink_free_entry(struct ext4_quota_file *file, struct ext4_quota_header_disk *header,
    struct ext4_quota_leaf_disk *leaf)
{
	struct ext4_quota_leaf_disk *other;
	uint8_t *buffer = NULL;
	uint32_t next = ext4_le32(&leaf->next_free);
	uint32_t previous = ext4_le32(&leaf->previous_free);
	enum ext4_result error;

	if (next != 0) {
		error = ext4_quota_edit(file, next, &buffer);
		if (error != EXT4_OK) {
			return error;
		}
		other = (struct ext4_quota_leaf_disk *)buffer;
		ext4_copy(
		    &other->previous_free, &leaf->previous_free, sizeof(other->previous_free));
	}
	if (previous != 0) {
		error = ext4_quota_edit(file, previous, &buffer);
		if (error != EXT4_OK) {
			return error;
		}
		other = (struct ext4_quota_leaf_disk *)buffer;
		ext4_copy(&other->next_free, &leaf->next_free, sizeof(other->next_free));
	} else {
		ext4_encode32(&header->free_entry, next);
	}
	ext4_encode32(&leaf->next_free, 0);
	ext4_encode32(&leaf->previous_free, 0);
	return EXT4_OK;
}

/* Take a slot from the first leaf with free entries, as the Linux quota tree does. */
static enum ext4_result
ext4_quota_free_entry(struct ext4_quota_file *file, struct ext4_quota_header_disk *header,
    uint32_t *block, uint32_t *slot)
{
	struct ext4_quota_leaf_disk *leaf;
	const uint8_t *entry;
	uint8_t *buffer = NULL;
	uint32_t blocks = ext4_le32(&header->blocks);
	uint16_t entries;
	enum ext4_result error;

	*block = ext4_le32(&header->free_entry);
	if (*block != 0) {
		if (*block <= EXT4_QUOTA_TREE_ROOT || *block >= blocks) {
			return EXT4_CORRUPT;
		}
		error = ext4_quota_edit(file, *block, &buffer);
	} else {
		error = ext4_quota_free_block(file, header, block);
		if (error == EXT4_OK) {
			error = ext4_quota_edit(file, *block, &buffer);
		}
		if (error == EXT4_OK) {
			ext4_encode32(&header->free_entry, *block);
		}
	}
	if (error != EXT4_OK) {
		return error;
	}
	leaf = (struct ext4_quota_leaf_disk *)buffer;
	entries = ext4_le16(&leaf->entries);
	if (entries >= EXT4_QUOTA_LEAF_ENTRIES) {
		return EXT4_CORRUPT;
	}
	if (entries + 1U >= EXT4_QUOTA_LEAF_ENTRIES) {
		error = ext4_quota_unlink_free_entry(file, header, leaf);
		if (error != EXT4_OK) {
			return error;
		}
	}
	ext4_encode16(&leaf->entries, (uint16_t)(entries + 1U));
	for (*slot = 0; *slot < EXT4_QUOTA_LEAF_ENTRIES; (*slot)++) {
		entry =
		    buffer + sizeof(*leaf) + (size_t)*slot * sizeof(struct ext4_quota_entry_disk);
		if (ext4_quota_zero(entry, sizeof(struct ext4_quota_entry_disk))) {
			return EXT4_OK;
		}
	}
	return EXT4_CORRUPT;
}

static uint32_t
ext4_quota_index(uint32_t id, uint32_t depth)
{
	return (id >> ((EXT4_QUOTA_TREE_DEPTH - depth - 1U) * EXT4_QUOTA_ID_BITS_PER_LEVEL)) &
	    EXT4_QUOTA_LOW_BYTE;
}

static bool
ext4_quota_entry_matches(const struct ext4_quota_entry_disk *entry, uint32_t id)
{
	return !ext4_quota_zero(entry, sizeof(*entry)) && ext4_le32(&entry->id) == id;
}

/* Find or insert the entry for id. Tree blocks are read without enrolling them;
 * only blocks that change join the transaction. */
static enum ext4_result
ext4_quota_entry(struct ext4_quota_file *file, uint32_t id, bool insert, uint8_t **result)
{
	struct ext4_quota_header_disk *header = NULL;
	const struct ext4_le32 *references;
	uint8_t *buffer = NULL;
	uint32_t path[EXT4_QUOTA_TREE_DEPTH + 1U];
	uint32_t blocks;
	uint32_t depth;
	uint32_t leaf;
	uint32_t slot;
	uint32_t child;
	enum ext4_result error;

	*result = NULL;
	error = ext4_quota_read(file, 0);
	if (error != EXT4_OK) {
		return error;
	}
	blocks = ext4_le32(&((struct ext4_quota_header_disk *)file->state->quota_block)->blocks);
	path[0] = EXT4_QUOTA_TREE_ROOT;
	for (depth = 0; depth < EXT4_QUOTA_TREE_DEPTH; depth++) {
		error = ext4_quota_read(file, path[depth]);
		if (error != EXT4_OK) {
			return error;
		}
		references = (const struct ext4_le32 *)file->state->quota_block;
		path[depth + 1U] = ext4_le32(&references[ext4_quota_index(id, depth)]);
		if (path[depth + 1U] == 0) {
			break;
		}
		if (path[depth + 1U] <= EXT4_QUOTA_TREE_ROOT || path[depth + 1U] >= blocks) {
			return EXT4_CORRUPT;
		}
	}
	if (depth == EXT4_QUOTA_TREE_DEPTH) {
		error = ext4_quota_read(file, path[depth]);
		if (error != EXT4_OK) {
			return error;
		}
		for (slot = 0; slot < EXT4_QUOTA_LEAF_ENTRIES; slot++) {
			if (ext4_quota_entry_matches(
				(const struct ext4_quota_entry_disk *)(file->state->quota_block +
				    sizeof(struct ext4_quota_leaf_disk) +
				    (size_t)slot * sizeof(struct ext4_quota_entry_disk)),
				id)) {
				error = ext4_quota_edit(file, path[depth], &buffer);
				if (error == EXT4_OK) {
					*result = buffer + sizeof(struct ext4_quota_leaf_disk) +
					    (size_t)slot * sizeof(struct ext4_quota_entry_disk);
				}
				return error;
			}
		}
		/* The tree names a leaf for this ID that does not contain it. */
		return EXT4_CORRUPT;
	}
	if (!insert) {
		return EXT4_OK;
	}
	error = ext4_quota_edit(file, 0, &buffer);
	if (error != EXT4_OK) {
		return error;
	}
	header = (struct ext4_quota_header_disk *)buffer;
	/* Create missing index blocks below the deepest existing reference. */
	for (; depth < EXT4_QUOTA_TREE_DEPTH - 1U; depth++) {
		error = ext4_quota_free_block(file, header, &child);
		if (error == EXT4_OK) {
			error = ext4_quota_edit(file, path[depth], &buffer);
		}
		if (error != EXT4_OK) {
			return error;
		}
		ext4_encode32(&((struct ext4_le32 *)buffer)[ext4_quota_index(id, depth)], child);
		path[depth + 1U] = child;
	}
	error = ext4_quota_free_entry(file, header, &leaf, &slot);
	if (error == EXT4_OK) {
		error = ext4_quota_edit(file, path[depth], &buffer);
	}
	if (error != EXT4_OK) {
		return error;
	}
	ext4_encode32(&((struct ext4_le32 *)buffer)[ext4_quota_index(id, depth)], leaf);
	error = ext4_quota_edit(file, leaf, &buffer);
	if (error == EXT4_OK) {
		*result = buffer + sizeof(struct ext4_quota_leaf_disk) +
		    (size_t)slot * sizeof(struct ext4_quota_entry_disk);
		ext4_encode32(&((struct ext4_quota_entry_disk *)*result)->id, id);
	}
	return error;
}

static uint64_t
ext4_quota_add(uint64_t value, int64_t change)
{
	uint64_t magnitude;

	if (change >= 0) {
		return (uint64_t)change > UINT64_MAX - value ? UINT64_MAX
							     : value + (uint64_t)change;
	}
	magnitude = (uint64_t)(-(change + 1)) + 1U;
	/* Linux clamps usage at zero instead of wrapping. */
	return magnitude > value ? 0 : value - magnitude;
}

static uint64_t
ext4_quota_limit_bytes(const struct ext4_le32 limit[2])
{
	uint64_t blocks = ext4_quota_le64(limit);

	return blocks > (UINT64_MAX >> EXT4_QUOTA_LIMIT_SHIFT) ? UINT64_MAX
							       : blocks << EXT4_QUOTA_LIMIT_SHIFT;
}

/* Check one enforced increase against a hard and a soft limit, starting the soft
 * limit's grace period as Linux's dquot does. */
static enum ext4_result
ext4_quota_limit(int64_t now, uint64_t used, uint64_t hard, uint64_t soft, int64_t grace,
    struct ext4_le32 time[2])
{
	uint64_t expiry = ext4_quota_le64(time);

	if (hard != 0 && used > hard) {
		return EXT4_QUOTA_EXCEEDED;
	}
	if (soft != 0 && used > soft) {
		if (expiry != 0 && expiry <= (uint64_t)INT64_MAX && now >= (int64_t)expiry) {
			return EXT4_QUOTA_EXCEEDED;
		}
		if (expiry == 0) {
			ext4_quota_encode64(time, (uint64_t)(now + grace));
		}
	}
	return EXT4_OK;
}

static enum ext4_result
ext4_quota_entry_update(const struct ext4_quota_file *file, struct ext4_quota_entry_disk *entry,
    int64_t space, int64_t inodes)
{
	struct ext4_quota_entry_disk marker;
	uint64_t used_space;
	uint64_t used_inodes;
	enum ext4_result error = EXT4_OK;

	ext4_zero(&marker, sizeof(marker));
	ext4_quota_encode64(marker.inode_time, EXT4_QUOTA_EMPTY_MARKER);
	if (ext4_equal(entry, &marker, sizeof(marker))) {
		ext4_quota_encode64(entry->inode_time, 0);
	}
	used_space = ext4_quota_add(ext4_quota_le64(entry->space), space);
	used_inodes = ext4_quota_add(ext4_quota_le64(entry->inodes), inodes);
	if (file->enforced && space > 0) {
		error = ext4_quota_limit(file->state->now, used_space,
		    ext4_quota_limit_bytes(entry->space_hard),
		    ext4_quota_limit_bytes(entry->space_soft), file->block_grace,
		    entry->space_time);
	}
	if (error == EXT4_OK && file->enforced && inodes > 0) {
		error = ext4_quota_limit(file->state->now, used_inodes,
		    ext4_quota_le64(entry->inode_hard), ext4_quota_le64(entry->inode_soft),
		    file->inode_grace, entry->inode_time);
	}
	if (error != EXT4_OK) {
		return error;
	}
	/* Usage back within a soft limit ends its grace period. */
	if (space < 0 && used_space <= ext4_quota_limit_bytes(entry->space_soft)) {
		ext4_quota_encode64(entry->space_time, 0);
	}
	if (inodes < 0 && used_inodes <= ext4_quota_le64(entry->inode_soft)) {
		ext4_quota_encode64(entry->inode_time, 0);
	}
	ext4_quota_encode64(entry->space, used_space);
	ext4_quota_encode64(entry->inodes, used_inodes);
	if (ext4_quota_zero(entry, sizeof(*entry))) {
		ext4_quota_encode64(entry->inode_time, EXT4_QUOTA_EMPTY_MARKER);
	}
	return EXT4_OK;
}

static enum ext4_result
ext4_quota_apply(struct ext4_quota_state *state)
{
	struct ext4_quota_file file;
	const struct ext4_quota_change *change;
	uint8_t *entry;
	uint32_t type;
	size_t index;
	bool opened;
	enum ext4_result error = EXT4_OK;

	for (type = 0; type < EXT4_QUOTA_TYPES && error == EXT4_OK; type++) {
		opened = false;
		for (index = 0; index < state->change_count && error == EXT4_OK; index++) {
			change = &state->changes[index];
			if (change->type != type || (change->space == 0 && change->inodes == 0)) {
				continue;
			}
			if (!opened) {
				opened = true;
				error = ext4_quota_file_open(state, type, &file);
				if (error != EXT4_OK) {
					break;
				}
			}
			/* A missing entry needs one only for a positive charge. */
			error = ext4_quota_entry(
			    &file, change->id, change->space > 0 || change->inodes > 0, &entry);
			if (error == EXT4_OK && entry != NULL) {
				error = ext4_quota_entry_update(&file,
				    (struct ext4_quota_entry_disk *)entry, change->space,
				    change->inodes);
			}
		}
		if (opened) {
			error = ext4_quota_file_close(&file, error);
		}
	}
	return error;
}

enum ext4_result
ext4_quota_commit(struct ext4_transaction *transaction)
{
	struct ext4_quota_state state;
	struct ext4_fs *fs = ext4_transaction_fs(transaction);
	const void *buffer;
	uint64_t block;
	uint32_t count = ext4_transaction_count(transaction);
	uint32_t index;
	enum ext4_result error = EXT4_OK;

	if (!fs->quota_active) {
		return EXT4_OK;
	}
	ext4_zero(&state, sizeof(state));
	state.fs = fs;
	state.transaction = transaction;
	state.enforcing = fs->quota_policy.types != 0 && !fs->quota_exempt;
	if (state.enforcing) {
		state.now = fs->quota_policy.now(fs->quota_policy.context);
	}
	state.previous = fs->environment.allocate(fs->environment.context, fs->info.block_size);
	state.scratch = fs->environment.allocate(fs->environment.context, fs->info.block_size);
	state.quota_block =
	    fs->environment.allocate(fs->environment.context, EXT4_QUOTA_BLOCK_SIZE);
	if (state.previous == NULL || state.scratch == NULL || state.quota_block == NULL) {
		error = EXT4_NO_MEMORY;
	}
	for (index = 0; index < count && error == EXT4_OK; index++) {
		ext4_transaction_entry(transaction, index, &block, &buffer);
		error = ext4_quota_collect_block(&state, block, buffer);
	}
	if (error == EXT4_OK) {
		error = ext4_quota_apply(&state);
	}
	if (state.changes != NULL) {
		fs->environment.release(fs->environment.context, state.changes,
		    state.change_capacity * sizeof(*state.changes));
	}
	if (state.quota_block != NULL) {
		fs->environment.release(
		    fs->environment.context, state.quota_block, EXT4_QUOTA_BLOCK_SIZE);
	}
	if (state.scratch != NULL) {
		fs->environment.release(
		    fs->environment.context, state.scratch, fs->info.block_size);
	}
	if (state.previous != NULL) {
		fs->environment.release(
		    fs->environment.context, state.previous, fs->info.block_size);
	}
	return error;
}

enum ext4_result
ext4_quota_policy_set(struct ext4_fs *fs, const struct ext4_quota_policy *policy)
{
	uint32_t type;

	if (fs == NULL) {
		return EXT4_INVALID_ARGUMENT;
	}
	if (policy == NULL || policy->types == 0) {
		ext4_zero(&fs->quota_policy, sizeof(fs->quota_policy));
		return EXT4_OK;
	}
	if ((policy->types &
		~(uint32_t)(EXT4_QUOTA_ENFORCE_USER | EXT4_QUOTA_ENFORCE_GROUP |
		    EXT4_QUOTA_ENFORCE_PROJECT)) ||
	    policy->now == NULL) {
		return EXT4_INVALID_ARGUMENT;
	}
	for (type = 0; type < EXT4_QUOTA_TYPES; type++) {
		if ((policy->types & (1U << type)) &&
		    (!fs->quota_active || fs->quota_inodes[type] == 0)) {
			return EXT4_UNSUPPORTED;
		}
	}
	fs->quota_policy = *policy;
	return EXT4_OK;
}

void
ext4_quota_exempt(struct ext4_fs *fs, bool exempt)
{
	if (fs != NULL) {
		fs->quota_exempt = exempt;
	}
}

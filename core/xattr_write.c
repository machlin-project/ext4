/* SPDX-License-Identifier: BSD-3-Clause */
#include "allocate.h"
#include "xattr.h"
#include "inline.h"

#define EXT4_XATTR_BLOCK_HASH_SHIFT 16U
#define EXT4_XATTR_HASH_BITS 32U

struct ext4_xattr_mutation {
	struct ext4_xattr_record record;
	enum ext4_xattr_policy policy;
};

struct ext4_xattr_edit {
	struct ext4_xattr_snapshot snapshot;
	struct ext4_xattr_mutation *changes;
	struct ext4_xattr_record *records;
	uint8_t *entries;
	size_t changes_count;
	size_t entries_size;
	size_t capacity;
	size_t count;
};

static size_t
ext4_xattr_round(size_t size)
{
	return (size + EXT4_XATTR_ALIGNMENT - 1) & ~(size_t)(EXT4_XATTR_ALIGNMENT - 1);
}

static size_t
ext4_xattr_record_size(const struct ext4_xattr_record *record)
{
	return ext4_xattr_round(sizeof(*record->entry) + record->entry->name_length) +
	    (record->inode_storage ? 0 : ext4_xattr_round(ext4_le32(&record->entry->value_size)));
}

enum ext4_result
ext4_xattr_changes_validate(
    struct ext4_fs *fs, const struct ext4_xattr_change *changes, size_t count)
{
	const struct ext4_xattr_change *change;
	size_t index;
	size_t byte;

	if (count > EXT4_XATTR_MAX_CHANGES) {
		return EXT4_RANGE;
	}
	if (count != 0 && changes == NULL) {
		return EXT4_INVALID_ARGUMENT;
	}
	for (index = 0; index < count; index++) {
		change = &changes[index];
		if (change->policy < EXT4_XATTR_SET || change->policy > EXT4_XATTR_REMOVE ||
		    change->name_index == 0 || change->name_length > EXT4_NAME_MAX ||
		    (change->name_length != 0 && change->name == NULL) ||
		    (change->value_size != 0 && change->value == NULL) ||
		    (change->policy == EXT4_XATTR_REMOVE &&
			(change->value != NULL || change->value_size != 0))) {
			return EXT4_INVALID_ARGUMENT;
		}
		for (byte = 0; byte < change->name_length; byte++) {
			if (change->name[byte] == 0) {
				return EXT4_INVALID_ARGUMENT;
			}
		}
		if (ext4_inline_key(change->name_index, change->name, change->name_length)) {
			return EXT4_INVALID_ARGUMENT;
		}
		if ((fs->info.feature_incompat & EXT4_FEATURE_INCOMPAT_EA_INODE) &&
		    change->value_size > EXT4_XATTR_VALUE_MAX) {
			return EXT4_RANGE;
		}
		if (!(fs->info.feature_incompat & EXT4_FEATURE_INCOMPAT_EA_INODE) &&
		    change->value_size > fs->info.block_size) {
			return EXT4_NO_SPACE;
		}
	}
	return EXT4_OK;
}

static void
ext4_xattr_mutation_sift(struct ext4_xattr_mutation *changes, size_t root, size_t count)
{
	struct ext4_xattr_mutation saved = changes[root];
	size_t child;

	while (root < count / 2) {
		child = root * 2 + 1;
		if (child + 1 < count &&
		    ext4_xattr_compare(
			changes[child].record.entry, changes[child + 1].record.entry) < 0) {
			child++;
		}
		if (ext4_xattr_compare(saved.record.entry, changes[child].record.entry) >= 0) {
			break;
		}
		changes[root] = changes[child];
		root = child;
	}
	changes[root] = saved;
}

static enum ext4_result
ext4_xattr_merge(struct ext4_xattr_edit *edit, const struct ext4_xattr_change *changes)
{
	struct ext4_fs *fs = edit->snapshot.fs;
	struct ext4_xattr_entry_disk *entry;
	struct ext4_xattr_mutation saved;
	struct ext4_xattr_mutation *change;
	size_t offset = 0;
	size_t index;
	size_t old = 0;
	size_t next = 0;
	int order;

	edit->capacity = edit->snapshot.count + edit->changes_count;
	for (index = 0; index < edit->changes_count; index++) {
		edit->entries_size += ext4_xattr_round(sizeof(*entry) + changes[index].name_length);
	}
	edit->records = fs->environment.allocate(
	    fs->environment.context, edit->capacity * sizeof(*edit->records));
	edit->changes = fs->environment.allocate(
	    fs->environment.context, edit->changes_count * sizeof(*edit->changes));
	edit->entries = fs->environment.allocate(fs->environment.context, edit->entries_size);
	if (edit->records == NULL || edit->changes == NULL || edit->entries == NULL) {
		return EXT4_NO_MEMORY;
	}
	ext4_zero(edit->entries, edit->entries_size);
	for (index = 0; index < edit->changes_count; index++) {
		entry = (struct ext4_xattr_entry_disk *)(edit->entries + offset);
		entry->name_index = changes[index].name_index;
		entry->name_length = (uint8_t)changes[index].name_length;
		ext4_copy(entry + 1, changes[index].name, entry->name_length);
		ext4_encode32(&entry->value_size, (uint32_t)changes[index].value_size);
		edit->changes[index].record.entry = entry;
		edit->changes[index].record.value = changes[index].value;
		edit->changes[index].record.external = false;
		edit->changes[index].record.value_inode = 0;
		edit->changes[index].record.value_hash = 0;
		edit->changes[index].record.inode_storage = false;
		edit->changes[index].record.new_inode = false;
		edit->changes[index].policy = changes[index].policy;
		offset += ext4_xattr_round(sizeof(*entry) + entry->name_length);
	}
	for (index = edit->changes_count / 2; index != 0; index--) {
		ext4_xattr_mutation_sift(edit->changes, index - 1, edit->changes_count);
	}
	for (index = edit->changes_count; index > 1; index--) {
		saved = edit->changes[0];
		edit->changes[0] = edit->changes[index - 1];
		edit->changes[index - 1] = saved;
		ext4_xattr_mutation_sift(edit->changes, 0, index - 1);
	}
	for (index = 1; index < edit->changes_count; index++) {
		if (ext4_xattr_compare(edit->changes[index - 1].record.entry,
			edit->changes[index].record.entry) == 0) {
			return EXT4_INVALID_ARGUMENT;
		}
	}
	while (old < edit->snapshot.count || next < edit->changes_count) {
		order = old == edit->snapshot.count ? 1
		    : next == edit->changes_count
		    ? -1
		    : ext4_xattr_compare(
			  edit->snapshot.records[old].entry, edit->changes[next].record.entry);
		if (order < 0) {
			edit->records[edit->count++] = edit->snapshot.records[old++];
			continue;
		}
		change = &edit->changes[next++];
		if ((order == 0 && change->policy == EXT4_XATTR_CREATE) ||
		    (order != 0 &&
			(change->policy == EXT4_XATTR_REPLACE ||
			    change->policy == EXT4_XATTR_REMOVE))) {
			return order == 0 ? EXT4_EXISTS : EXT4_NOT_FOUND;
		}
		if (order == 0) {
			change->record.external = edit->snapshot.records[old++].external;
		}
		if (change->policy != EXT4_XATTR_REMOVE) {
			edit->records[edit->count++] = change->record;
		}
	}
	return EXT4_OK;
}

static bool
ext4_xattr_record_equal(const struct ext4_xattr_record *left, const struct ext4_xattr_record *right)
{
	if (ext4_xattr_compare(left->entry, right->entry) != 0 ||
	    ext4_le32(&left->entry->value_size) != ext4_le32(&right->entry->value_size)) {
		return false;
	}
	if (left->inode_storage || right->inode_storage) {
		return left->inode_storage && right->inode_storage && left->value_inode != 0 &&
		    left->value_inode == right->value_inode;
	}
	return ext4_equal(left->value, right->value, ext4_le32(&left->entry->value_size));
}

static bool
ext4_xattr_unchanged(struct ext4_xattr_edit *edit, bool external_only)
{
	size_t old = 0;
	size_t next = 0;

	for (;;) {
		while (external_only && old < edit->snapshot.count &&
		    !edit->snapshot.records[old].external) {
			old++;
		}
		while (external_only && next < edit->count && !edit->records[next].external) {
			next++;
		}
		if (old == edit->snapshot.count || next == edit->count) {
			return old == edit->snapshot.count && next == edit->count;
		}
		if (!ext4_xattr_record_equal(
			&edit->snapshot.records[old++], &edit->records[next++])) {
			return false;
		}
	}
}

static enum ext4_result
ext4_xattr_place(struct ext4_xattr_edit *edit, size_t body_capacity)
{
	struct ext4_fs *fs = edit->snapshot.fs;
	size_t external_capacity =
	    fs->info.block_size - sizeof(struct ext4_xattr_header_disk) - sizeof(struct ext4_le32);
	size_t body_used = 0;
	size_t total = 0;
	size_t cost;
	size_t index;
	size_t units;
	size_t pinned = edit->count;
	size_t chosen;
	size_t sum;
	uint32_t *previous;

	for (index = 0; index < edit->count; index++) {
		if (ext4_inline_key(edit->records[index].entry->name_index,
			(const uint8_t *)(edit->records[index].entry + 1),
			edit->records[index].entry->name_length)) {
			pinned = index;
			cost = ext4_xattr_record_size(&edit->records[index]);
			if (edit->records[index].inode_storage || cost > body_capacity) {
				return EXT4_NO_SPACE;
			}
			edit->records[index].external = false;
			body_capacity -= cost;
			break;
		}
	}
	units = body_capacity / EXT4_XATTR_ALIGNMENT;
	for (index = 0; index < edit->count; index++) {
		if (index == pinned) {
			continue;
		}
		cost = ext4_xattr_record_size(&edit->records[index]);
		total += cost;
		if (cost > body_capacity) {
			edit->records[index].external = true;
		}
		if (!edit->records[index].external) {
			body_used += cost;
		}
	}
	if (total > body_capacity + external_capacity) {
		return EXT4_NO_SPACE;
	}
	/* Keep existing storage assignments whenever they fit. In particular, an
	 * inode-body edit must not copy an unchanged shared external block. */
	if (body_used <= body_capacity && total - body_used <= external_capacity) {
		return EXT4_OK;
	}
	/* Costs are four-byte units. A bounded subset sum finds a fitting packing
	 * when a greedy choice would falsely report ENOSPC. Each reachable sum is
	 * assigned once; descending iteration prevents using a record twice. */
	previous =
	    fs->environment.allocate(fs->environment.context, (units + 1) * sizeof(*previous));
	if (previous == NULL) {
		return EXT4_NO_MEMORY;
	}
	ext4_zero(previous, (units + 1) * sizeof(*previous));
	previous[0] = UINT32_MAX;
	for (index = 0; index < edit->count; index++) {
		if (index == pinned) {
			continue;
		}
		cost = ext4_xattr_record_size(&edit->records[index]) / EXT4_XATTR_ALIGNMENT;
		for (sum = units; sum >= cost; sum--) {
			if (previous[sum] == 0 && previous[sum - cost] != 0) {
				previous[sum] = (uint32_t)index + 1;
			}
		}
	}
	for (chosen = units; previous[chosen] == 0; chosen--) {
		/* The empty subset is always reachable. */
	}
	if (total - chosen * EXT4_XATTR_ALIGNMENT > external_capacity) {
		fs->environment.release(
		    fs->environment.context, previous, (units + 1) * sizeof(*previous));
		return EXT4_NO_SPACE;
	}
	for (index = 0; index < edit->count; index++) {
		edit->records[index].external = index != pinned;
	}
	while (chosen != 0) {
		index = previous[chosen] - 1;
		edit->records[index].external = false;
		chosen -= ext4_xattr_record_size(&edit->records[index]) / EXT4_XATTR_ALIGNMENT;
	}
	fs->environment.release(fs->environment.context, previous, (units + 1) * sizeof(*previous));
	return EXT4_OK;
}

static enum ext4_result
ext4_xattr_place_values(struct ext4_xattr_edit *edit, size_t body_capacity)
{
	size_t index;
	size_t selected;
	uint32_t largest;
	uint32_t size;
	enum ext4_result error;

	for (;;) {
		error = ext4_xattr_place(edit, body_capacity);
		if (error != EXT4_NO_SPACE ||
		    !(edit->snapshot.fs->info.feature_incompat & EXT4_FEATURE_INCOMPAT_EA_INODE)) {
			return error;
		}
		largest = 0;
		selected = edit->count;
		for (index = 0; index < edit->count; index++) {
			size = ext4_le32(&edit->records[index].entry->value_size);
			if (!edit->records[index].inode_storage && size > largest &&
			    !ext4_inline_key(edit->records[index].entry->name_index,
				(const uint8_t *)(edit->records[index].entry + 1),
				edit->records[index].entry->name_length)) {
				largest = size;
				selected = index;
			}
		}
		if (selected == edit->count) {
			return EXT4_NO_SPACE;
		}
		edit->records[selected].inode_storage = true;
		edit->records[selected].external = false;
	}
}

static void
ext4_xattr_pack(
    struct ext4_xattr_edit *edit, uint8_t *buffer, size_t size, size_t first, bool external)
{
	struct ext4_xattr_entry_disk *entry;
	struct ext4_xattr_record *record;
	struct ext4_xattr_header_disk *header;
	size_t next = first;
	size_t value_end = size;
	size_t value_size;
	size_t index;
	uint32_t hash = 0;
	uint32_t entry_hash;
	bool hashable = true;
	bool present = false;

	for (index = 0; index < edit->count; index++) {
		record = &edit->records[index];
		if (record->external != external) {
			continue;
		}
		present = true;
		entry = (struct ext4_xattr_entry_disk *)(buffer + next);
		value_size = ext4_le32(&record->entry->value_size);
		ext4_copy(entry, record->entry, sizeof(*entry) + record->entry->name_length);
		if (record->inode_storage) {
			ext4_encode16(&entry->value_offset, 0);
			ext4_encode32(&entry->value_inode, record->value_inode);
			entry_hash = ext4_xattr_inode_entry_hash(entry, record->value_hash, false);
		} else {
			value_end -= ext4_xattr_round(value_size);
			ext4_encode16(&entry->value_offset,
			    value_size == 0 ? 0 : (uint16_t)(value_end - (external ? 0 : first)));
			ext4_encode32(&entry->value_inode, 0);
			ext4_copy(buffer + value_end, record->value, value_size);
			entry_hash =
			    external ? ext4_xattr_hash(entry, buffer + value_end, false) : 0;
		}
		ext4_encode32(&entry->hash, entry_hash);
		hashable = hashable && entry_hash != 0;
		hash = ((hash << EXT4_XATTR_BLOCK_HASH_SHIFT) |
			   (hash >> (EXT4_XATTR_HASH_BITS - EXT4_XATTR_BLOCK_HASH_SHIFT))) ^
		    entry_hash;
		next += ext4_xattr_round(sizeof(*entry) + entry->name_length);
	}
	if (external) {
		header = (struct ext4_xattr_header_disk *)buffer;
		ext4_encode32(&header->magic, EXT4_XATTR_MAGIC);
		ext4_encode32(&header->references, 1);
		ext4_encode32(&header->blocks, 1);
		ext4_encode32(&header->hash, hashable ? hash : 0);
	} else if (present) {
		ext4_encode32((struct ext4_le32 *)(buffer + first - sizeof(struct ext4_le32)),
		    EXT4_XATTR_MAGIC);
	}
}

static bool
ext4_xattr_external_changed(struct ext4_xattr_edit *edit)
{
	size_t index;
	bool external = false;

	for (index = 0; index < edit->count; index++) {
		external = external || edit->records[index].external;
	}
	return (!external && edit->snapshot.external_block != 0) ||
	    !ext4_xattr_unchanged(edit, true);
}

static enum ext4_result
ext4_xattr_reference_edits(struct ext4_allocation *allocation, struct ext4_xattr_edit *edit)
{
	const struct ext4_xattr_record *old_record;
	const struct ext4_xattr_record *new_record;
	const struct ext4_xattr_header_disk *header;
	uint64_t old_blocks = ext4_xattr_value_blocks(&edit->snapshot);
	uint64_t new_blocks = 0;
	size_t old;
	size_t next;
	size_t index;
	int order;
	unsigned int pass;
	bool old_owned;
	bool new_owned;
	bool changed = ext4_xattr_external_changed(edit);
	bool shared = false;
	enum ext4_result error;

	if (edit->snapshot.external_block != 0) {
		header = (const struct ext4_xattr_header_disk *)edit->snapshot.block;
		shared = ext4_le32(&header->references) > 1;
	}
	/* A shared block owns one value reference, irrespective of its own refcount.
	 * Copying it creates references; detaching one owner preserves the old ones.
	 * Increment all surviving values before dropping any old references. */
	for (pass = 0; pass < 2; pass++) {
		old = 0;
		next = 0;
		while (old < edit->snapshot.count || next < edit->count) {
			order = old == edit->snapshot.count ? 1
			    : next == edit->count
			    ? -1
			    : ext4_xattr_compare(
				  edit->snapshot.records[old].entry, edit->records[next].entry);
			old_record = order <= 0 ? &edit->snapshot.records[old++] : NULL;
			new_record = order >= 0 ? &edit->records[next++] : NULL;
			old_owned = old_record != NULL && old_record->inode_storage &&
			    (!old_record->external || (changed && !shared));
			new_owned = new_record != NULL && new_record->inode_storage &&
			    (!new_record->external || changed);
			if (old_owned && new_owned &&
			    old_record->value_inode == new_record->value_inode) {
				continue;
			}
			if (pass == 0 && new_owned && !new_record->new_inode) {
				error = ext4_xattr_inode_adjust(allocation, new_record, 1);
			} else if (pass == 1 && old_owned) {
				error = ext4_xattr_inode_adjust(allocation, old_record, -1);
			} else {
				continue;
			}
			if (error != EXT4_OK) {
				return error;
			}
		}
	}
	for (index = 0; index < edit->count; index++) {
		if (edit->records[index].inode_storage) {
			new_blocks +=
			    ((uint64_t)ext4_le32(&edit->records[index].entry->value_size) +
				allocation->fs->info.block_size - 1U) /
			    allocation->fs->info.block_size;
		}
	}
	if (new_blocks > old_blocks) {
		allocation->attribute_blocks_added += new_blocks - old_blocks;
	} else {
		allocation->attribute_blocks_removed += old_blocks - new_blocks;
	}
	return EXT4_OK;
}

static enum ext4_result
ext4_xattr_external_edit(
    struct ext4_allocation *allocation, struct ext4_xattr_edit *edit, struct ext4_inode_disk *disk)
{
	struct ext4_fs *fs = allocation->fs;
	struct ext4_xattr_header_disk *header;
	void *buffer;
	uint64_t block = edit->snapshot.external_block;
	uint32_t references = 0;
	size_t index;
	bool external = false;
	enum ext4_result error;

	for (index = 0; index < edit->count; index++) {
		external = external || edit->records[index].external;
	}
	if ((external || block == 0) && ext4_xattr_unchanged(edit, true)) {
		return EXT4_OK;
	}
	if (block != 0) {
		header = (struct ext4_xattr_header_disk *)edit->snapshot.block;
		references = ext4_le32(&header->references);
		if (references > 1) {
			error = ext4_transaction_buffer(allocation->transaction, block, &buffer);
			if (error != EXT4_OK) {
				return error;
			}
			if (!ext4_equal(buffer, edit->snapshot.block, fs->info.block_size)) {
				return EXT4_CORRUPT;
			}
			header = buffer;
			ext4_encode32(&header->references, references - 1);
			ext4_xattr_checksum_set(fs, block, header);
			allocation->detached_shared_blocks++;
			block = 0;
		} else if (!external) {
			error = ext4_free_blocks(allocation, block, 1);
			if (error != EXT4_OK) {
				return error;
			}
			block = 0;
		}
	}
	if (external) {
		if (block == 0) {
			error = ext4_allocate_block(allocation, &block);
			if (error != EXT4_OK) {
				return error;
			}
		}
		error = ext4_transaction_buffer(allocation->transaction, block, &buffer);
		if (error != EXT4_OK) {
			return error;
		}
		if (block == edit->snapshot.external_block &&
		    !ext4_equal(buffer, edit->snapshot.block, fs->info.block_size)) {
			return EXT4_CORRUPT;
		}
		ext4_zero(buffer, fs->info.block_size);
		ext4_xattr_pack(
		    edit, buffer, fs->info.block_size, sizeof(struct ext4_xattr_header_disk), true);
		ext4_xattr_checksum_set(fs, block, buffer);
	}
	ext4_encode32(&disk->xattr_block_lo, (uint32_t)block);
	ext4_encode16(&disk->xattr_block_hi, (uint16_t)(block >> 32));
	return EXT4_OK;
}

enum ext4_result
ext4_xattr_apply(struct ext4_allocation *allocation, struct ext4_inode *inode,
    struct ext4_inode_disk *disk, const struct ext4_xattr_change *changes, size_t count)
{
	struct ext4_fs *fs = allocation->fs;
	struct ext4_xattr_edit edit;
	size_t body = fs->inode_size;
	size_t body_capacity = 0;
	size_t extra;
	size_t index;
	bool inline_full = false;
	enum ext4_result error;

	ext4_zero(&edit, sizeof(edit));
	edit.changes_count = count;
	error = ext4_xattr_open_inode(fs, inode, disk, &edit.snapshot);
	if (error != EXT4_OK) {
		goto out;
	}
	if (count == 0) {
		goto out;
	}
	error = ext4_xattr_merge(&edit, changes);
	if (error != EXT4_OK || ext4_xattr_unchanged(&edit, false)) {
		goto out;
	}
	if (fs->inode_size > EXT4_INODE_BASE_SIZE) {
		extra = ext4_le16(&disk->extra_size);
		if (extra != 0 &&
		    EXT4_INODE_BASE_SIZE + extra + 2 * sizeof(struct ext4_le32) <= fs->inode_size) {
			body = EXT4_INODE_BASE_SIZE + extra + sizeof(struct ext4_le32);
			body_capacity = fs->inode_size - body - sizeof(struct ext4_le32);
		}
	}
	error = ext4_xattr_place_values(&edit, body_capacity);
	inline_full = error == EXT4_NO_SPACE && (inode->flags & EXT4_INODE_INLINE_DATA);
	if (error != EXT4_OK) {
		goto out;
	}
	if (edit.count != 0 && !(fs->info.feature_compat & EXT4_FEATURE_COMPAT_EXT_ATTR)) {
		error = ext4_allocation_super(allocation);
		if (error != EXT4_OK) {
			goto out;
		}
		ext4_encode32(&allocation->super->feature_compat,
		    ext4_le32(&allocation->super->feature_compat) | EXT4_FEATURE_COMPAT_EXT_ATTR);
	}
	for (index = 0; index < edit.count; index++) {
		if (inode->fast_symlink && inode->size != 0 && edit.records[index].inode_storage) {
			/* Value charges make i_blocks look like a mapped symlink to Linux.
			 * Do not publish inode-body target bytes as physical block pointers. */
			error = EXT4_UNSUPPORTED;
			goto out;
		}
		if (edit.records[index].inode_storage && edit.records[index].value_inode == 0) {
			error = ext4_xattr_inode_create(allocation, inode, &edit.records[index]);
			if (error != EXT4_OK) {
				goto out;
			}
		}
	}
	error = ext4_xattr_reference_edits(allocation, &edit);
	if (error == EXT4_OK) {
		error = ext4_xattr_external_edit(allocation, &edit, disk);
	}
	if (error == EXT4_OK && body < fs->inode_size) {
		ext4_zero((uint8_t *)disk + body - sizeof(struct ext4_le32),
		    fs->inode_size - body + sizeof(struct ext4_le32));
		ext4_xattr_pack(&edit, (uint8_t *)disk, fs->inode_size, body, false);
	}
out:
	if (edit.entries != NULL) {
		fs->environment.release(fs->environment.context, edit.entries, edit.entries_size);
	}
	if (edit.changes != NULL) {
		fs->environment.release(
		    fs->environment.context, edit.changes, count * sizeof(*edit.changes));
	}
	if (edit.records != NULL) {
		fs->environment.release(
		    fs->environment.context, edit.records, edit.capacity * sizeof(*edit.records));
	}
	ext4_xattr_close(&edit.snapshot);
	if (inline_full) {
		error = ext4_inline_expand(allocation, inode, disk);
		if (error == EXT4_OK) {
			error = ext4_xattr_apply(allocation, inode, disk, changes, count);
		}
	}
	return error;
}

enum ext4_result
ext4_xattr_drop(struct ext4_allocation *allocation, struct ext4_inode *inode,
    struct ext4_inode_disk *disk, bool *done)
{
	struct ext4_xattr_edit edit;
	struct ext4_xattr_change change;
	const struct ext4_xattr_record *record;
	const struct ext4_xattr_header_disk *header;
	size_t index;
	bool shared = false;
	enum ext4_result error;

	ext4_zero(&edit, sizeof(edit));
	error = ext4_xattr_open_inode(allocation->fs, inode, disk, &edit.snapshot);
	if (error != EXT4_OK) {
		goto out;
	}
	if (edit.snapshot.external_block != 0) {
		header = (const struct ext4_xattr_header_disk *)edit.snapshot.block;
		shared = ext4_le32(&header->references) > 1;
	}
	/* Reclaim one private value per transaction. Shared blocks can be detached
	 * whole without allocating a copy or changing any value reference. */
	for (index = 0; index < edit.snapshot.count; index++) {
		record = &edit.snapshot.records[index];
		if (!record->inode_storage || (record->external && shared)) {
			continue;
		}
		ext4_zero(&change, sizeof(change));
		change.policy = EXT4_XATTR_REMOVE;
		change.name_index = record->entry->name_index;
		change.name = (const uint8_t *)(record->entry + 1);
		change.name_length = record->entry->name_length;
		error = ext4_xattr_apply(allocation, inode, disk, &change, 1);
		*done = false;
		goto out;
	}
	error = ext4_xattr_reference_edits(allocation, &edit);
	if (error == EXT4_OK) {
		error = ext4_xattr_external_edit(allocation, &edit, disk);
	}
out:
	ext4_xattr_close(&edit.snapshot);
	return error;
}

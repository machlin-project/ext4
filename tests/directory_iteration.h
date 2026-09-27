/* SPDX-License-Identifier: BSD-3-Clause */
#ifndef MACHLIN_EXT4_TEST_DIRECTORY_ITERATION_H
#define MACHLIN_EXT4_TEST_DIRECTORY_ITERATION_H

struct iteration_entry {
	uint32_t parent;
	uint32_t number;
	size_t length;
	uint8_t name[EXT4_NAME_MAX + 1U];
	uint64_t cookie;
	size_t position;
	bool seen;
};

struct iteration_context {
	struct iteration_entry *entries;
	size_t count;
	size_t accepted;
	size_t batch;
	size_t limit;
	bool remember;
};

struct iteration_observer {
	struct ext4_fs *fs;
	enum ext4_dir_action action;
	const uint8_t *name;
	size_t length;
	size_t calls;
	size_t matches;
	uint32_t number;
	bool query_inode;
};

static enum ext4_dir_action
iteration_observe(void *opaque, const struct ext4_dir_entry *entry, uint64_t next_cookie)
{
	struct iteration_observer *observer = opaque;
	struct ext4_inode inode;

	CHECK(next_cookie != 0);
	observer->calls++;
	if (observer->query_inode) {
		EXPECT(ext4_get_inode(observer->fs, entry->inode, &inode), EXT4_OK);
		CHECK(inode.number == entry->inode);
	}
	if (observer->name != NULL && entry->name_length == observer->length &&
	    memcmp(entry->name, observer->name, observer->length) == 0) {
		CHECK(observer->number == entry->inode);
		observer->matches++;
	}
	return observer->action;
}

static void
iteration_guards(struct device *device, struct ext4_fs *fs)
{
	struct iteration_observer observer = { .fs = fs, .action = EXT4_DIR_ACCEPT };
	struct ext4_inode root;
	struct ext4_inode changed;
	struct lookup_path path;
	struct ext4_dir_header_disk *second;
	struct ext4_dir_entry output;
	struct ext4_dir_entry before;
	uint64_t cookie = 0;
	uint64_t start;
	uint32_t record;
	uint32_t live = device->live;

	EXPECT(ext4_get_inode(fs, EXT4_ROOT_INODE, &root), EXT4_OK);
	reset_calls(device);
	EXPECT(ext4_iterate_dir(NULL, &root, &cookie, iteration_observe, &observer),
	    EXT4_INVALID_ARGUMENT);
	EXPECT(ext4_iterate_dir(fs, NULL, &cookie, iteration_observe, &observer),
	    EXT4_INVALID_ARGUMENT);
	EXPECT(
	    ext4_iterate_dir(fs, &root, NULL, iteration_observe, &observer), EXT4_INVALID_ARGUMENT);
	EXPECT(ext4_iterate_dir(fs, &root, &cookie, NULL, &observer), EXT4_INVALID_ARGUMENT);
	changed = root;
	changed.mode = EXT4_MODE_REGULAR;
	EXPECT(ext4_iterate_dir(fs, &changed, &cookie, iteration_observe, &observer),
	    EXT4_NOT_DIRECTORY);
	changed = root;
	changed.size++;
	EXPECT(ext4_iterate_dir(fs, &changed, &cookie, iteration_observe, &observer), EXT4_CORRUPT);
	changed.size = ((uint64_t)UINT32_MAX + 2U) * fs->info.block_size;
	EXPECT(ext4_iterate_dir(fs, &changed, &cookie, iteration_observe, &observer), EXT4_RANGE);
	changed.size = 0;
	EXPECT(
	    ext4_iterate_dir(fs, &changed, &cookie, iteration_observe, &observer), EXT4_NOT_FOUND);
	fs->aborted = true;
	EXPECT(ext4_iterate_dir(fs, &root, &cookie, iteration_observe, &observer),
	    EXT4_RECOVERY_REQUIRED);
	fs->aborted = false;
	CHECK(cookie == 0 && observer.calls == 0 && device->reads == 0 && device->allocations == 0);
	cookie = root.size + 1U;
	EXPECT(ext4_iterate_dir(fs, &root, &cookie, iteration_observe, &observer), EXT4_CORRUPT);
	CHECK(cookie == root.size + 1U && observer.calls == 0);
	cookie = 1;
	EXPECT(ext4_iterate_dir(fs, &root, &cookie, iteration_observe, &observer), EXT4_CORRUPT);
	CHECK(cookie == 1 && observer.calls == 0 && device->live == live);
	cookie = 0;
	observer.action = (enum ext4_dir_action)(EXT4_DIR_STOP + 1);
	EXPECT(ext4_iterate_dir(fs, &root, &cookie, iteration_observe, &observer),
	    EXT4_INVALID_ARGUMENT);
	CHECK(cookie == 0 && observer.calls == 1 && device->live == live);
	observer.action = EXT4_DIR_ACCEPT_STOP;
	observer.query_inode = true;
	EXPECT(ext4_iterate_dir(fs, &root, &cookie, iteration_observe, &observer), EXT4_OK);
	CHECK(cookie != 0 && observer.calls == 2 && device->live == live);

	/* Damage a later record but repair its checksum. Neither streaming nor the
	 * single-entry wrapper may publish the earlier valid record from that block. */
	path = first_path(device, fs);
	record = ext4_directory_record_length(fs, (struct ext4_dir_header_disk *)path.leaf);
	second = (struct ext4_dir_header_disk *)(path.leaf + record);
	ext4_encode16(&second->record_length, sizeof(*second) - EXT4_DIRECTORY_ALIGNMENT);
	leaf_checksum(fs, &path.query.directory, path.leaf);
	start = (uint64_t)path.leaf_logical * fs->info.block_size;
	cookie = start;
	observer.calls = 0;
	EXPECT(ext4_iterate_dir(fs, &path.query.directory, &cookie, iteration_observe, &observer),
	    EXT4_CORRUPT);
	CHECK(cookie == start && observer.calls == 0 && device->live == live);
	memset(&output, 0xa5, sizeof(output));
	memcpy(&before, &output, sizeof(before));
	EXPECT(ext4_next_dir(fs, &path.query.directory, &cookie, &output), EXT4_CORRUPT);
	CHECK(cookie == start && memcmp(&before, &output, sizeof(before)) == 0);
	restore_block(device, path.leaf);
	EXPECT(ext4_iterate_dir(fs, &path.query.directory, &cookie, iteration_observe, &observer),
	    EXT4_OK);
	CHECK(cookie > start && observer.calls == 1 && device->live == live);
	puts("PASS directory visitor arguments, late corruption and nested inode reads");
}

static void
iteration_mutation(struct device *device)
{
	const uint8_t name[] = { 'i', 't', 'e', 'r', '-', 0xff };
	struct ext4_inode_update attributes = { .fields = EXT4_ATTR_PERMISSIONS | EXT4_ATTR_UID |
		    EXT4_ATTR_GID | EXT4_ATTR_ACCESS_TIME | EXT4_ATTR_MODIFY_TIME |
		    EXT4_ATTR_CHANGE_TIME | EXT4_ATTR_XATTRS,
		.permissions = 0644,
		.access_time = { .seconds = 1700000000 },
		.modify_time = { .seconds = 1700000000 },
		.change_time = { .seconds = 1700000000 } };
	struct ext4_fs *fs;
	struct ext4_inode root;
	struct ext4_inode created;
	struct ext4_inode removed;
	struct iteration_observer observer = {
		.action = EXT4_DIR_ACCEPT, .name = name, .length = sizeof(name), .query_inode = true
	};
	uint64_t cookie;
	size_t initial;
	unsigned int step;

	device_reset(device, device->base);
	EXPECT(ext4_mount_writable(&device->environment, &device->writer, &fs), EXT4_OK);
	observer.fs = fs;
	EXPECT(ext4_get_inode(fs, EXT4_ROOT_INODE, &root), EXT4_OK);
	cookie = 0;
	EXPECT(ext4_iterate_dir(fs, &root, &cookie, iteration_observe, &observer), EXT4_NOT_FOUND);
	CHECK(observer.matches == 0);
	initial = observer.calls;
	EXPECT(ext4_create(fs, root.number, root.generation, name, sizeof(name), &attributes,
		   &attributes.change_time, &created),
	    EXT4_OK);
	observer.number = created.number;
	for (step = 0; step < 2; step++) {
		EXPECT(ext4_get_inode(fs, EXT4_ROOT_INODE, &root), EXT4_OK);
		observer.calls = 0;
		observer.matches = 0;
		cookie = 0;
		EXPECT(ext4_iterate_dir(fs, &root, &cookie, iteration_observe, &observer),
		    EXT4_NOT_FOUND);
		CHECK(observer.calls == initial + (step == 0) && observer.matches == (step == 0));
		if (step == 0) {
			EXPECT(ext4_unlink(fs, root.number, root.generation, name, sizeof(name),
				   created.number, created.generation, &attributes.change_time,
				   &removed),
			    EXT4_OK);
		}
	}
	EXPECT(ext4_sync(fs), EXT4_OK);
	ext4_unmount(fs);
	CHECK(device->live == 0);
	puts("PASS directory iteration refreshes after create/unlink with opaque byte names");
}

static int
iteration_compare(const void *left, const void *right)
{
	const struct iteration_entry *a = left;
	const struct iteration_entry *b = right;
	size_t length = a->length < b->length ? a->length : b->length;
	int order;

	if (a->parent != b->parent) {
		return a->parent < b->parent ? -1 : 1;
	}
	order = memcmp(a->name, b->name, length);
	return order != 0 ? order : (a->length > b->length) - (a->length < b->length);
}

static enum ext4_dir_action
iteration_visit(void *opaque, const struct ext4_dir_entry *entry, uint64_t cookie)
{
	struct iteration_context *context = opaque;
	struct iteration_entry key = { 0 };
	struct iteration_entry *expected;

	key.parent = context->entries[0].parent;
	key.length = entry->name_length;
	memcpy(key.name, entry->name, key.length);
	expected = bsearch(&key, context->entries, context->count, sizeof(key), iteration_compare);
	CHECK(expected != NULL && expected->number == entry->inode && !expected->seen);
	if (context->limit == 0) {
		return EXT4_DIR_STOP;
	}
	if (context->remember) {
		expected->cookie = cookie;
		expected->position = context->accepted;
	} else {
		CHECK(expected->cookie == cookie && expected->position == context->accepted);
	}
	expected->seen = true;
	context->accepted++;
	return ++context->batch == context->limit ? EXT4_DIR_ACCEPT_STOP : EXT4_DIR_ACCEPT;
}

static void
iteration_reset(struct iteration_context *context)
{
	size_t index;

	for (index = 0; index < context->count; index++) {
		context->entries[index].seen = false;
	}
	context->accepted = 0;
	context->batch = 0;
}

static void
iteration_faults(struct device *device, struct ext4_fs *fs, const struct ext4_inode *directory,
    struct iteration_context *context, uint32_t allocations, uint32_t reads)
{
	uint64_t cookie;
	uint32_t live = device->live;
	uint32_t count;
	uint32_t fault;
	uint32_t checked = 0;
	unsigned int kind;
	bool exhaustive = directory->size / fs->info.block_size <= 64U;

	context->limit = SIZE_MAX;
	for (kind = 0; kind < 2; kind++) {
		count = kind == 0 ? allocations : reads;
		for (fault = 1; fault <= count; fault++) {
			/* Large-image coverage samples boundary and middle failures. Small
			 * directories sweep every allocation and read callback. */
			if (!exhaustive && fault != 1 && fault != (count + 1U) / 2U &&
			    fault != count) {
				continue;
			}
			iteration_reset(context);
			reset_calls(device);
			cookie = 0;
			if (kind == 0) {
				device->fail_allocation = fault;
			} else {
				device->fail_read = fault;
			}
			EXPECT(ext4_iterate_dir(fs, directory, &cookie, iteration_visit, context),
			    kind == 0 ? EXT4_NO_MEMORY : EXT4_IO);
			CHECK(device->live == live && !fs->aborted && cookie <= directory->size);
			reset_calls(device);
			EXPECT(ext4_iterate_dir(fs, directory, &cookie, iteration_visit, context),
			    EXT4_NOT_FOUND);
			CHECK(context->accepted == context->count && device->live == live);
			checked++;
		}
	}
	printf("PASS directory iteration faults parent=%" PRIu32 " cases=%" PRIu32
	       " exhaustive=%u\n",
	    directory->number, checked, exhaustive);
}

static void
iteration_directory(
    struct device *device, struct ext4_fs *fs, struct iteration_entry *entries, size_t count)
{
	struct iteration_context context = { entries, count, 0, 0, SIZE_MAX, true };
	struct ext4_inode directory;
	uint64_t cookie = 0;
	uint64_t saved;
	uint32_t reads;
	uint32_t allocations;
	uint32_t live = device->live;
	size_t index;
	enum ext4_result error;

	EXPECT(ext4_get_inode(fs, entries[0].parent, &directory), EXT4_OK);
	reset_calls(device);
	EXPECT(
	    ext4_iterate_dir(fs, &directory, &cookie, iteration_visit, &context), EXT4_NOT_FOUND);
	CHECK(context.accepted == count && cookie == directory.size && device->live == live);
	reads = device->reads;
	allocations = device->allocations;
	CHECK(reads <= directory.size / fs->info.block_size * (EXT4_EXTENT_MAX_DEPTH + 1U));
	CHECK(allocations <= directory.size / fs->info.block_size + 1U);
	printf("PASS directory iteration parent=%" PRIu32 " entries=%zu reads=%" PRIu32
	       " allocations=%" PRIu32 "\n",
	    directory.number, count, reads, allocations);
	context.remember = false;
	iteration_reset(&context);
	context.limit = 0;
	cookie = 0;
	EXPECT(ext4_iterate_dir(fs, &directory, &cookie, iteration_visit, &context), EXT4_OK);
	CHECK(cookie == 0 && context.accepted == 0 && device->live == live);
	context.limit = 31;
	do {
		context.batch = 0;
		saved = cookie;
		error = ext4_iterate_dir(fs, &directory, &cookie, iteration_visit, &context);
		CHECK(error != EXT4_OK || cookie > saved);
	} while (error == EXT4_OK);
	CHECK(error == EXT4_NOT_FOUND && context.accepted == count && cookie == directory.size);
	/* Resume selected saved cookies independently of their previous call. */
	for (index = 0; index < count; index += count / 5U + 1U) {
		iteration_reset(&context);
		context.limit = SIZE_MAX;
		context.accepted = entries[index].position + 1U;
		cookie = entries[index].cookie;
		EXPECT(ext4_iterate_dir(fs, &directory, &cookie, iteration_visit, &context),
		    EXT4_NOT_FOUND);
		CHECK(context.accepted == count && cookie == directory.size);
	}
	iteration_faults(device, fs, &directory, &context, allocations, reads);
	reset_calls(device);
	cookie = directory.size;
	EXPECT(
	    ext4_iterate_dir(fs, &directory, &cookie, iteration_visit, &context), EXT4_NOT_FOUND);
	CHECK(device->reads == 0 && device->allocations == 0 && device->live == live);
}

static void
independent_iteration(struct device *device, struct ext4_fs *fs, const char *path)
{
	struct iteration_entry *entries = NULL;
	struct iteration_entry *entry;
	FILE *stream;
	char encoded[EXT4_NAME_MAX * 2U + 1U];
	uint32_t parent;
	uint32_t number;
	size_t count = 0;
	size_t capacity = 0;
	size_t first;
	size_t end;
	size_t index;
	size_t length;
	int fields;

	_Static_assert(EXT4_NAME_MAX * 2U == 510U, "directory expectation scan width");
	stream = fopen(path, "r");
	CHECK(stream != NULL);
	while ((fields = fscanf(stream, "%" SCNu32 " %510s %" SCNu32, &parent, encoded, &number)) !=
	    EOF) {
		CHECK(fields == 3 && parent != 0 && number != 0);
		if (count == capacity) {
			capacity = capacity == 0 ? 512 : capacity * 2U;
			CHECK(capacity > count && capacity <= SIZE_MAX / sizeof(*entries));
			entries = realloc(entries, capacity * sizeof(*entries));
			CHECK(entries != NULL);
		}
		entry = &entries[count++];
		memset(entry, 0, sizeof(*entry));
		entry->parent = parent;
		entry->number = number;
		length = strlen(encoded);
		CHECK(length != 0 && length % 2U == 0 && length <= EXT4_NAME_MAX * 2U);
		entry->length = length / 2U;
		for (index = 0; index < entry->length; index++) {
			entry->name[index] = (uint8_t)((hex_digit(encoded[index * 2U]) << 4) |
			    hex_digit(encoded[index * 2U + 1U]));
		}
	}
	CHECK(fclose(stream) == 0 && count != 0);
	qsort(entries, count, sizeof(*entries), iteration_compare);
	for (first = 0; first < count; first = end) {
		for (end = first + 1U; end < count && entries[end].parent == entries[first].parent;
		    end++) {
			CHECK(iteration_compare(&entries[end - 1U], &entries[end]) < 0);
		}
		iteration_directory(device, fs, entries + first, end - first);
	}
	CHECK(device->writes == 0 && device->events == 0 && !fs->aborted);
	iteration_guards(device, fs);
	free(entries);
}

#endif

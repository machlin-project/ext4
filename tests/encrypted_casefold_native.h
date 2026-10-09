/* SPDX-License-Identifier: BSD-3-Clause */
#ifndef MACHLIN_EXT4_ENCRYPTED_CASEFOLD_NATIVE_H
#define MACHLIN_EXT4_ENCRYPTED_CASEFOLD_NATIVE_H

/* Fixed linear Linux-import model. Expected bytes come from this model, never
 * filesystem readback. Name records are separate from inode identities. */
#define NATIVE_PARENTS 4U
#define NATIVE_FILES 10U
#define NATIVE_NAMES 12U
#define NATIVE_BYTES 113U
#define NATIVE_CONTEXT 40U

struct native_file {
	struct ext4_inode inode;
	uint8_t context[NATIVE_CONTEXT];
	uint8_t bytes[NATIVE_BYTES];
	size_t size;
};

struct native_name {
	const char *name;
	const char *alias;
	unsigned int parent;
	unsigned int file;
	bool present;
};

struct native_model {
	struct ext4_inode parents[NATIVE_PARENTS];
	uint8_t contexts[NATIVE_PARENTS][NATIVE_CONTEXT];
	struct native_file files[NATIVE_FILES];
	struct native_name names[NATIVE_NAMES];
};

static const char *const native_paths[NATIVE_PARENTS] = {
	"probe-0-0", "probe-0-1", "probe-1-0", "probe-1-1"
};

static void
native_context(struct ext4_fs *fs, const struct ext4_inode *inode, uint8_t *context)
{
	size_t size = 0;

	EXPECT(ext4_get_xattr(fs, inode->number, inode->generation, 9,
		   (const uint8_t *)"c", 1, context, NATIVE_CONTEXT, &size), EXT4_OK);
	CHECK(size == NATIVE_CONTEXT);
}

static struct ext4_inode
native_parent(struct ext4_fs *fs, struct native_model *model, unsigned int index)
{
	struct ext4_inode inode;
	uint8_t context[NATIVE_CONTEXT];

	CHECK(index < NATIVE_PARENTS);
	inode = find(fs, EXT4_ROOT_INODE, native_paths[index]);
	CHECK(inode.number == model->parents[index].number &&
	    inode.generation == model->parents[index].generation);
	CHECK(inode.mode == (EXT4_MODE_DIRECTORY | 0700U) && inode.links == 2 &&
	    inode.size == fs->info.block_size);
	CHECK((inode.flags & (EXT4_INODE_ENCRYPT | EXT4_INODE_CASEFOLD |
	    EXT4_INODE_INLINE_DATA | EXT4_INODE_INDEX)) ==
	    (EXT4_INODE_ENCRYPT | EXT4_INODE_CASEFOLD));
	native_context(fs, &inode, context);
	CHECK(memcmp(context, model->contexts[index], sizeof(context)) == 0);
	return inode;
}

static void
native_verify(struct ext4_fs *fs, struct native_model *model)
{
	struct listing listing;
	struct ext4_inode parent;
	struct ext4_inode inode;
	struct ext4_inode alias;
	uint8_t context[NATIVE_CONTEXT];
	uint8_t bytes[NATIVE_BYTES];
	uint64_t cookie;
	size_t completed;
	unsigned int directory;
	unsigned int index;
	unsigned int other;
	unsigned int count;
	unsigned int links;

	for (directory = 0; directory < NATIVE_PARENTS; directory++) {
		parent = native_parent(fs, model, directory);
		memset(&listing, 0, sizeof(listing));
		cookie = 0;
		EXPECT(ext4_iterate_dir(fs, &parent, &cookie, collect, &listing), EXT4_NOT_FOUND);
		count = 2;
		CHECK(listed(&listing, ".") && listed(&listing, ".."));
		for (index = 0; index < listing.count; index++) {
			if (strcmp(listing.names[index], ".") == 0) {
				CHECK(listing.numbers[index] == parent.number);
			} else if (strcmp(listing.names[index], "..") == 0) {
				CHECK(listing.numbers[index] == EXT4_ROOT_INODE);
			}
		}
		for (index = 0; index < NATIVE_NAMES; index++) {
			struct native_name *name = &model->names[index];
			struct native_file *file = &model->files[name->file];

			if (!name->present || name->parent != directory) {
				continue;
			}
			count++;
			CHECK(listed(&listing, name->name));
			inode = find(fs, parent.number, name->name);
			alias = find(fs, parent.number, name->alias);
			CHECK(inode.number == file->inode.number && inode.generation == file->inode.generation &&
			    alias.number == inode.number && alias.generation == inode.generation);
			CHECK(inode.mode == file->inode.mode && inode.size == file->size &&
			    (inode.flags & EXT4_INODE_ENCRYPT));
			links = 0;
			for (other = 0; other < NATIVE_NAMES; other++) {
				links += model->names[other].present && model->names[other].file == name->file;
			}
			CHECK(inode.links == links);
			native_context(fs, &inode, context);
			CHECK(memcmp(context, file->context, sizeof(context)) == 0);
			EXPECT(ext4_read(fs, &inode, 0, bytes, sizeof(bytes), &completed), EXT4_OK);
			CHECK(completed == file->size && memcmp(bytes, file->bytes, completed) == 0);
		}
		CHECK(listing.count == count);
	}
}

static void
native_create(struct ext4_fs *fs, struct native_model *model, unsigned int slot,
    unsigned int parent, unsigned int file_index, const char *name, const char *alias, size_t size)
{
	struct ext4_inode directory = native_parent(fs, model, parent);
	struct ext4_inode_update update = creation();
	struct native_file *file = &model->files[file_index];
	size_t index;
	size_t completed;

	CHECK(slot < NATIVE_NAMES && file_index < NATIVE_FILES && size <= NATIVE_BYTES);
	EXPECT(ext4_create(fs, directory.number, directory.generation, (const uint8_t *)name,
		   strlen(name), &update, &encrypt_time, &file->inode), EXT4_OK);
	file->size = size;
	for (index = 0; index < size; index++) {
		file->bytes[index] = (uint8_t)(file_index * 37U + index * 13U);
	}
	if (size != 0) {
		update = data_update(&file->inode);
		EXPECT(ext4_write_request(fs, file->inode.number, file->inode.generation, 0,
			   file->bytes, size, &update, &completed), EXT4_OK);
		CHECK(completed == size);
	}
	native_context(fs, &file->inode, file->context);
	CHECK(file->inode.mode == (EXT4_MODE_REGULAR | PERMISSIONS));
	CHECK(memcmp(file->context, model->contexts[parent], 24) == 0);
	for (index = 0; index < NATIVE_PARENTS; index++) {
		CHECK(memcmp(file->context + 24, model->contexts[index] + 24, 16) != 0);
	}
	for (index = 0; index < NATIVE_FILES; index++) {
		if (index != file_index && model->files[index].inode.number != 0) {
			CHECK(memcmp(file->context + 24, model->files[index].context + 24, 16) != 0);
		}
	}
	model->names[slot] = (struct native_name){ name, alias, parent, file_index, true };
}

static void
native_hex(FILE *stream, const void *bytes, size_t size)
{
	const uint8_t *data = bytes;
	size_t index;

	for (index = 0; index < size; index++) {
		CHECK(fprintf(stream, "%02x", data[index]) == 2);
	}
}

static void
native_export(struct device *device, struct native_model *model, const char *exports,
    const char *source)
{
	char path[1024];
	FILE *stream;
	unsigned int index;
	int length;

	storage_export(device, exports, source, "combined-");
	length = snprintf(path, sizeof(path), "%s/combined.manifest", exports);
	CHECK(length > 0 && (size_t)length < sizeof(path));
	stream = fopen(path, "wx");
	CHECK(stream != NULL);
	CHECK(fprintf(stream, "combined-linear-v1\n") > 0);
	for (index = 0; index < NATIVE_PARENTS; index++) {
		CHECK(fprintf(stream, "parent %u %s %u %u ", index, native_paths[index],
		    model->parents[index].number, model->parents[index].generation) > 0);
		native_hex(stream, model->contexts[index], NATIVE_CONTEXT);
		CHECK(fputc('\n', stream) != EOF);
	}
	for (index = 0; index < 8U; index++) {
		struct native_file *file = &model->files[index];

		CHECK(fprintf(stream, "file %u %u %u %u %zu ", index, file->inode.number,
		    file->inode.generation, file->inode.mode, file->size) > 0);
		native_hex(stream, file->context, NATIVE_CONTEXT);
		CHECK(fputc(' ', stream) != EOF);
		native_hex(stream, file->bytes, file->size);
		CHECK(fputc('\n', stream) != EOF);
	}
	for (index = 0; index < NATIVE_NAMES; index++) {
		struct native_name *name = &model->names[index];

		if (!name->present) {
			continue;
		}
		CHECK(fprintf(stream, "name %u %u ", name->parent, name->file) > 0);
		native_hex(stream, name->name, strlen(name->name));
		CHECK(fputc(' ', stream) != EOF);
		native_hex(stream, name->alias, strlen(name->alias));
		CHECK(fputc('\n', stream) != EOF);
	}
	CHECK(fclose(stream) == 0);
}

static void
native_keyless(struct device *device, struct ext4_fs *fs, struct native_model *model,
    const char *exports, bool remove)
{
	struct listing listing;
	struct ext4_inode parent;
	struct ext4_inode inode;
	struct ext4_inode result;
	struct ext4_inode sentinel;
	struct ext4_rename_entry from;
	struct ext4_rename_entry to;
	struct ext4_inode_update update = creation();
	uint8_t *before = malloc(device->size);
	uint8_t byte;
	size_t completed;
	unsigned int directory;
	unsigned int index;
	unsigned int expected;
	unsigned int names;
	unsigned int match;
	unsigned int slot;
	bool seen[NATIVE_NAMES];
	uint32_t events;
	char path[1024];
	FILE *stream = NULL;
	int length;

	CHECK(before != NULL);
	if (!remove) {
		length = snprintf(path, sizeof(path), "%s/combined.nokey", exports);
		CHECK(length > 0 && (size_t)length < sizeof(path));
		stream = fopen(path, "wx");
		CHECK(stream != NULL);
	}
	for (directory = 0; directory < NATIVE_PARENTS; directory++) {
		parent = native_parent(fs, model, directory);
		memset(seen, 0, sizeof(seen));
		names = nokey_listing(fs, &parent, &listing);
		CHECK(listing.count == names + 2U && listed(&listing, ".") && listed(&listing, ".."));
		expected = 0;
		for (index = 0; index < NATIVE_NAMES; index++) {
			if (model->names[index].present && model->names[index].parent == directory) {
				expected++;
				EXPECT(ext4_lookup(fs, &parent, (const uint8_t *)model->names[index].name,
					   strlen(model->names[index].name), &result), EXT4_NOT_FOUND);
				EXPECT(ext4_lookup(fs, &parent, (const uint8_t *)model->names[index].alias,
					   strlen(model->names[index].alias), &result), EXT4_NOT_FOUND);
			}
		}
		CHECK(names == expected);
		memcpy(before, device->cache, device->size);
		events = device->events;
		if (remove) {
			EXPECT(ext4_create(fs, parent.number, parent.generation,
				   (const uint8_t *)"Blocked", 7, &update, &encrypt_time, &result),
			    EXT4_ENCRYPTED);
			CHECK(events == device->events && memcmp(before, device->cache, device->size) == 0);
		}
		for (index = 0; index < listing.count; index++) {
			if (strcmp(listing.names[index], ".") == 0) {
				CHECK(listing.numbers[index] == parent.number);
				continue;
			}
			if (strcmp(listing.names[index], "..") == 0) {
				CHECK(listing.numbers[index] == EXT4_ROOT_INODE);
				continue;
			}
			match = NATIVE_NAMES;
			for (slot = 0; slot < NATIVE_NAMES; slot++) {
				if (model->names[slot].present && model->names[slot].parent == directory &&
				    model->files[model->names[slot].file].inode.number == listing.numbers[index]) {
					CHECK(match == NATIVE_NAMES && !seen[slot]);
					match = slot;
				}
			}
			CHECK(match != NATIVE_NAMES);
			seen[match] = true;
			EXPECT(ext4_get_inode(fs, listing.numbers[index], &inode), EXT4_OK);
			if (inode.size != 0) {
				memcpy(before, device->cache, device->size);
				events = device->events;
				byte = 0x5a;
				completed = SIZE_MAX;
				EXPECT(ext4_read(fs, &inode, 0, &byte, 1, &completed), EXT4_ENCRYPTED);
				CHECK(completed == 0 && byte == 0x5a);
				CHECK(events == device->events && memcmp(before, device->cache, device->size) == 0);
			}
			if (remove) {
				memcpy(before, device->cache, device->size);
				events = device->events;
				memset(&sentinel, 0x5a, sizeof(sentinel));
				memcpy(&result, &sentinel, sizeof(result));
				from = entry_of(&parent, listing.names[index], &inode);
				to = entry_of(&parent, "Blocked", NULL);
				EXPECT(ext4_rename(fs, &from, &to, 0, &encrypt_time, &result), EXT4_ENCRYPTED);
				CHECK(memcmp(&result, &sentinel, sizeof(result)) == 0);
				EXPECT(ext4_link(fs, parent.number, parent.generation, (const uint8_t *)"Blocked", 7,
					   inode.number, inode.generation, &encrypt_time, &result), EXT4_ENCRYPTED);
				CHECK(memcmp(&result, &sentinel, sizeof(result)) == 0);
				CHECK(events == device->events && memcmp(before, device->cache, device->size) == 0);
			}
			if (remove && directory % 2U == 0 &&
			    inode.number == model->files[8U + directory / 2U].inode.number) {
				EXPECT(ext4_unlink(fs, parent.number, parent.generation,
					   (const uint8_t *)listing.names[index], strlen(listing.names[index]),
					   inode.number, inode.generation, &encrypt_time, &result), EXT4_OK);
				model->names[8U + directory / 2U].present = false;
			} else if (stream != NULL) {
				CHECK(fprintf(stream, "%u %u %s\n", directory, inode.number,
				    listing.names[index]) > 0);
			}
		}
	}
	if (stream != NULL) {
		CHECK(fclose(stream) == 0);
	}
	free(before);
}

static void
native_roundtrip(struct device *device, const char *exports, const char *source)
{
	static struct native_model model;
	struct keyring keyring;
	struct ext4_crypto_environment crypto;
	struct ext4_encryption_policy policy;
	struct ext4_encryption_policy expected;
	struct ext4_inode_update update = creation();
	struct ext4_inode left;
	struct ext4_inode right;
	struct ext4_inode result;
	struct ext4_rename_entry from;
	struct ext4_rename_entry to;
	struct ext4_fs *fs;
	uint8_t *before = malloc(device->size);
	unsigned int index;
	unsigned int pair;
	unsigned int a;
	unsigned int b;
	uint32_t events;

	CHECK(before != NULL && exports != NULL);
	memset(&model, 0, sizeof(model));
	keyring_init(&keyring, PROBE_KEY_OFFSET);
	crypto = keyring_environment(&keyring);
	EXPECT(ext4_mount_writable(&device->environment, &device->writer, &fs), EXT4_OK);
	EXPECT(ext4_set_crypto(fs, &crypto), EXT4_OK);
	for (index = 0; index < NATIVE_PARENTS; index++) {
		model.parents[index] = find(fs, EXT4_ROOT_INODE, native_paths[index]);
		native_context(fs, &model.parents[index], model.contexts[index]);
		memset(&expected, 0, sizeof(expected));
		expected.version = FSCRYPT_V2;
		expected.contents_mode = EXT4_FSCRYPT_MODE_AES_256_XTS;
		expected.filenames_mode = EXT4_FSCRYPT_MODE_AES_256_CTS;
		expected.flags = index < 2U ? 0 : FSCRYPT_PAD_32;
		memcpy(expected.identifier, keyring.identifier, sizeof(keyring.identifier));
		EXPECT(ext4_get_encryption_policy(fs, &model.parents[index], &policy), EXT4_OK);
		CHECK(memcmp(&policy, &expected, sizeof(policy)) == 0);
		model.files[index].inode = find(fs, model.parents[index].number, "Stra\303\237e");
		CHECK(model.files[index].inode.mode == (EXT4_MODE_REGULAR | 0600U));
		native_context(fs, &model.files[index].inode, model.files[index].context);
		CHECK(memcmp(model.files[index].context, model.contexts[index], 24) == 0);
		model.files[index].bytes[0] = 'K';
		model.files[index].size = 1;
		model.names[index] = (struct native_name){ "Stra\303\237e", "STRASSE", index, index, true };
	}
	native_verify(fs, &model);
	for (pair = 0; pair < 2U; pair++) {
		a = pair * 2U;
		b = a + 1U;
		CHECK(memcmp(model.contexts[a] + 24, model.contexts[b] + 24, 16) != 0);
		native_create(fs, &model, 4U + a, a, 4U + a, "\303\211", "e\314\201", 97);
		native_create(fs, &model, 4U + b, b, 4U + b, "Beta", "bETA", 113);
		for (index = a; index <= b; index++) {
			left = native_parent(fs, &model, index);
			memcpy(before, device->cache, device->size);
			events = device->events;
			EXPECT(ext4_create(fs, left.number, left.generation,
				   (const uint8_t *)model.names[4U + index].alias,
				   strlen(model.names[4U + index].alias), &update, &encrypt_time, &result),
			    EXT4_EXISTS);
			CHECK(events == device->events && memcmp(before, device->cache, device->size) == 0);
		}
		left = native_parent(fs, &model, a);
		right = native_parent(fs, &model, b);
		from = entry_of(&left, "e\314\201", &model.files[4U + a].inode);
		to = entry_of(&right, "bETA", &model.files[4U + b].inode);
		EXPECT(ext4_rename(fs, &from, &to, EXT4_RENAME_EXCHANGE, &encrypt_time, &result), EXT4_OK);
		model.names[4U + a].file = 4U + b;
		model.names[4U + b].file = 4U + a;
		native_verify(fs, &model);
		from = entry_of(&left, "STRASSE", &model.files[a].inode);
		to = entry_of(&right, "Moved", NULL);
		EXPECT(ext4_rename(fs, &from, &to, EXT4_RENAME_NOREPLACE, &encrypt_time, &result), EXT4_OK);
		model.names[a] = (struct native_name){ "Moved", "MOVED", b, a, true };
		EXPECT(ext4_link(fs, left.number, left.generation, (const uint8_t *)"Native-Link", 11,
			   model.files[a].inode.number, model.files[a].inode.generation, &encrypt_time, &result),
		    EXT4_OK);
		model.names[10U + pair] = (struct native_name){ "Native-Link", "native-link", a, a, true };
		native_create(fs, &model, 8U + pair, a, 8U + pair, "Remove", "REMOVE", 0);
		native_verify(fs, &model);
	}
	left = native_parent(fs, &model, 0);
	right = native_parent(fs, &model, 2);
	from = entry_of(&left, "native-link", &model.files[0].inode);
	to = entry_of(&right, "Cross-Policy", NULL);
	memcpy(before, device->cache, device->size);
	events = device->events;
	EXPECT(ext4_rename(fs, &from, &to, 0, &encrypt_time, &result), EXT4_CROSS_POLICY);
	EXPECT(ext4_link(fs, right.number, right.generation, (const uint8_t *)"Cross-Policy", 12,
		   model.files[0].inode.number, model.files[0].inode.generation, &encrypt_time, &result),
	    EXT4_CROSS_POLICY);
	CHECK(events == device->events && memcmp(before, device->cache, device->size) == 0);
	EXPECT(ext4_set_crypto(fs, NULL), EXT4_OK);
	CHECK(keyring.handles == 0);
	native_keyless(device, fs, &model, exports, true);
	CHECK(!model.names[8].present && !model.names[9].present);
	EXPECT(ext4_set_crypto(fs, &crypto), EXT4_OK);
	native_verify(fs, &model);
	EXPECT(ext4_sync(fs), EXT4_OK);
	ext4_unmount(fs);
	CHECK(device->live == 0 && keyring.handles == 0);
	CHECK(memcmp(device->cache, device->stable, device->size) == 0);
	memcpy(before, device->stable, device->size);
	events = device->events;
	EXPECT(ext4_mount(&device->environment, &fs), EXT4_OK);
	native_keyless(device, fs, &model, exports, false);
	EXPECT(ext4_set_crypto(fs, &crypto), EXT4_OK);
	native_verify(fs, &model);
	ext4_unmount(fs);
	CHECK(device->live == 0 && keyring.handles == 0);
	CHECK(events == device->events && memcmp(before, device->cache, device->size) == 0);
	native_export(device, &model, exports, source);
	free(before);
	puts("PASS native combined import/mutation/export: 4 linear parents, 10 names, 8 files");
}

#endif

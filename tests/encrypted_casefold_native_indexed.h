/* SPDX-License-Identifier: BSD-3-Clause */
#ifndef MACHLIN_EXT4_ENCRYPTED_CASEFOLD_NATIVE_INDEXED_H
#define MACHLIN_EXT4_ENCRYPTED_CASEFOLD_NATIVE_INDEXED_H

#include "fscrypt.h"
#include "sha.h"

/* Linux authors every initial inode. The fixed operation formula is independently
 * repeated by the raw and Linux oracles, rather than exported as an expectation. */
#define INDEXED_LIMIT 512U
#define INDEXED_DIRECTORIES 9U

enum indexed_endpoint {
	INDEXED_OLD,
	INDEXED_FULL,
	INDEXED_COLLISION,
	INDEXED_RENAME
};

struct indexed_listing {
	char names[INDEXED_LIMIT][EXT4_NAME_MAX + 1U];
	uint32_t numbers[INDEXED_LIMIT];
	bool seen[INDEXED_LIMIT];
	unsigned int count;
};

struct indexed_model {
	struct ext4_inode directories[INDEXED_DIRECTORIES];
	uint8_t contexts[INDEXED_DIRECTORIES][NATIVE_CONTEXT];
	struct ext4_inode files[3][5];
	uint8_t file_contexts[3][5][NATIVE_CONTEXT];
	unsigned int collision[4];
	unsigned int baseline;
	unsigned int total;
	bool small;
};

static const char *const indexed_ids[INDEXED_DIRECTORIES] = {
	"main0", "peer0", "child0", "grand0", "main1", "peer1", "child1", "grand1", "collision"
};

static void
indexed_name(char *name, const char *prefix, unsigned int index)
{
	static const char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ";
	int length = snprintf(name, EXT4_NAME_MAX + 1U, "%s-%06u-", prefix, index);
	unsigned int offset;

	CHECK(index < 262144U && length > 0 && length < (int)EXT4_NAME_MAX);
	for (offset = (unsigned int)length; offset < EXT4_NAME_MAX; offset++) {
		name[offset] = alphabet[(offset - (unsigned int)length) % 26U];
	}
	name[EXT4_NAME_MAX] = 0;
}

static void
indexed_alias(char *alias, const char *name)
{
	size_t index;

	for (index = 0; name[index] != 0; index++) {
		CHECK(index < EXT4_NAME_MAX);
		alias[index] = name[index] >= 'A' && name[index] <= 'Z' ?
		    (char)(name[index] + ('a' - 'A')) : name[index];
	}
	alias[index] = 0;
}

static enum ext4_dir_action
indexed_collect(void *context, const struct ext4_dir_entry *entry, uint64_t cookie)
{
	struct indexed_listing *listing = context;
	unsigned int index;

	(void)cookie;
	CHECK(listing->count < INDEXED_LIMIT && entry->name_length <= EXT4_NAME_MAX);
	CHECK(entry->name[entry->name_length] == 0);
	for (index = 0; index < listing->count; index++) {
		CHECK(strcmp(listing->names[index], (const char *)entry->name) != 0);
	}
	memcpy(listing->names[listing->count], entry->name, (size_t)entry->name_length + 1U);
	listing->numbers[listing->count++] = entry->inode;
	return EXT4_DIR_ACCEPT;
}

static void
indexed_expect(struct ext4_fs *fs, const struct ext4_inode *parent,
    struct indexed_listing *listing, const char *name, uint32_t number, bool alias)
{
	struct ext4_inode inode;
	char folded[EXT4_NAME_MAX + 1U];
	unsigned int index;
	unsigned int found = 0;

	for (index = 0; index < listing->count; index++) {
		if (strcmp(listing->names[index], name) == 0) {
			CHECK(!listing->seen[index] && listing->numbers[index] == number);
			listing->seen[index] = true;
			found++;
		}
	}
	CHECK(found == 1);
	if (alias) {
		indexed_alias(folded, name);
		EXPECT(ext4_lookup(fs, parent, (const uint8_t *)folded, strlen(folded), &inode), EXT4_OK);
		CHECK(inode.number == number);
	}
}

static void
indexed_directory(struct ext4_fs *fs, struct indexed_model *model, unsigned int index,
    struct ext4_inode *inode)
{
	uint8_t context[NATIVE_CONTEXT];

	EXPECT(ext4_get_inode(fs, model->directories[index].number, inode), EXT4_OK);
	CHECK(inode->generation == model->directories[index].generation);
	CHECK(inode->mode == (EXT4_MODE_DIRECTORY | 0700U));
	CHECK((inode->flags & (EXT4_INODE_ENCRYPT | EXT4_INODE_CASEFOLD |
	    EXT4_INODE_INLINE_DATA)) == (EXT4_INODE_ENCRYPT | EXT4_INODE_CASEFOLD));
	CHECK(inode->size != 0 && inode->size <= 1024U * 1024U);
	native_context(fs, inode, context);
	CHECK(memcmp(context, model->contexts[index], sizeof(context)) == 0);
}

static void
indexed_verify(struct ext4_fs *fs, struct indexed_model *model, enum indexed_endpoint endpoint)
{
	struct indexed_listing *listing = calloc(1, sizeof(*listing));
	struct ext4_inode inode;
	uint8_t bytes[161];
	uint8_t context[NATIVE_CONTEXT];
	char name[EXT4_NAME_MAX + 1U];
	uint64_t cookie;
	size_t completed;
	unsigned int directory;
	unsigned int parent;
	unsigned int kind;
	unsigned int index;
	unsigned int slot;
	bool final = endpoint == INDEXED_FULL;
	bool renamed = final || endpoint == INDEXED_RENAME;
	bool collision = final || endpoint == INDEXED_COLLISION;
	unsigned int count = final ? model->total : model->baseline;
	unsigned int links;
	uint32_t up;

	CHECK(listing != NULL);
	for (directory = 0; directory < (model->small ? 9U : 8U); directory++) {
		indexed_directory(fs, model, directory, &inode);
		memset(listing, 0, sizeof(*listing));
		cookie = 0;
		EXPECT(ext4_iterate_dir(fs, &inode, &cookie, indexed_collect, listing), EXT4_NOT_FOUND);
		parent = directory / 4U;
		kind = directory % 4U;
		up = EXT4_ROOT_INODE;
		if (directory < 8U && kind == 2U) {
			up = model->directories[parent * 4U + (final ? 1U : 0U)].number;
		} else if (directory < 8U && kind == 3U) {
			up = model->directories[parent * 4U + 2U].number;
		}
		indexed_expect(fs, &inode, listing, ".", inode.number, false);
		indexed_expect(fs, &inode, listing, "..", up, false);
		if (directory == 8U) {
			CHECK(!collision || (inode.flags & EXT4_INODE_INDEX));
			for (index = 0; index < 4U; index++) {
				if (index == 2U && !collision) {
					continue;
				}
				indexed_name(name, "Collision", model->collision[index]);
				indexed_expect(fs, &inode, listing, name,
				    model->files[2][index < 2U ? 0U : 1U].number, true);
			}
			CHECK(inode.links == 2);
		} else if (kind == 0) {
			CHECK(inode.flags & EXT4_INODE_INDEX);
			for (index = 0; index < count; index++) {
				if ((final && index == 0U) ||
				    (index == 1U && (final || (renamed && parent == 0U)))) {
					continue;
				}
				indexed_name(name, "Bulk", index);
				slot = final && index == 2U ? 4U : index % 4U;
				indexed_expect(fs, &inode, listing, name, model->files[parent][slot].number, true);
			}
			if (!final) {
				indexed_expect(fs, &inode, listing, "Child",
				    model->directories[directory + 2U].number, true);
			}
			CHECK(inode.links == (final ? 2U : 3U));
		} else if (kind == 1U) {
			indexed_expect(fs, &inode, listing, "Exchange",
			    model->files[parent][final ? 2U : 4U].number, true);
			if (final || (renamed && parent == 0U)) {
				indexed_expect(fs, &inode, listing, "Moved", model->files[parent][1].number, true);
			}
			if (final) {
				indexed_expect(fs, &inode, listing, "Linked", model->files[parent][3].number, true);
				indexed_expect(fs, &inode, listing, "Moved-Child",
				    model->directories[directory + 1U].number, true);
			}
			CHECK(inode.links == (final ? 3U : 2U));
		} else if (kind == 2U) {
			indexed_expect(fs, &inode, listing, "Grandchild",
			    model->directories[directory + 1U].number, true);
			CHECK(inode.links == 3U);
		} else {
			CHECK(inode.links == 2U);
		}
		for (index = 0; index < listing->count; index++) {
			CHECK(listing->seen[index]);
		}
	}
	for (parent = 0; parent < (model->small ? 3U : 2U); parent++) {
		for (slot = 0; slot < (parent == 2U ? 2U : 5U); slot++) {
			EXPECT(ext4_get_inode(fs, model->files[parent][slot].number, &inode), EXT4_OK);
			CHECK(inode.generation == model->files[parent][slot].generation);
			CHECK(inode.mode == (EXT4_MODE_REGULAR | 0600U) && inode.size == 97U + slot * 16U);
			native_context(fs, &inode, context);
			CHECK(memcmp(context, model->file_contexts[parent][slot], sizeof(context)) == 0);
			links = parent == 2U ? (slot == 0U || collision ? 2U : 1U) :
			    (slot == 4U ? 1U : count / 4U);
			if (parent < 2U && final && slot == 0U) {
				links--;
			} else if (parent < 2U && final && slot == 3U) {
				links++;
			}
			CHECK(inode.links == links);
			completed = 0;
			EXPECT(ext4_read(fs, &inode, 0, bytes, (size_t)inode.size, &completed), EXT4_OK);
			CHECK(completed == inode.size);
			for (index = 0; index < completed; index++) {
				CHECK(bytes[index] == (uint8_t)(parent * 53U + slot * 29U + index * 13U));
			}
		}
	}
	free(listing);
}

static void
indexed_nokey_name(struct ext4_fs *fs, const struct ext4_inode *directory,
    const char *name, char *encoded)
{
	struct ext4_fscrypt_key key;
	struct ext4_name_hash hash = { 0 };
	uint8_t padded[EXT4_NAME_MAX];
	uint8_t cipher[EXT4_NAME_MAX];
	char folded[EXT4_NAME_MAX + 1U];
	size_t length = 0;

	indexed_alias(folded, name);
	EXPECT(ext4_fscrypt_key(fs, directory, &key), EXT4_OK);
	EXPECT(ext4_fscrypt_name_encrypt(fs, &key, (const uint8_t *)name, strlen(name),
	    EXT4_NAME_MAX, padded, cipher, &length), EXT4_OK);
	EXPECT(ext4_fscrypt_name_hash(fs, &key, (const uint8_t *)folded, strlen(folded), &hash), EXT4_OK);
	length = ext4_fscrypt_nokey_encode(cipher, length, hash.major, hash.minor, (uint8_t *)encoded);
	CHECK(length <= EXT4_NAME_MAX);
	encoded[length] = 0;
}

static void
indexed_plan(struct indexed_model *model, const char *path)
{
	FILE *stream;
	char magic[64];
	char context[82];
	char line[128];
	char *cursor;
	char *end;
	unsigned long value;
	unsigned int index;
	unsigned int byte;

	CHECK(path != NULL);
	stream = fopen(path, "rb");
	CHECK(stream != NULL && fgets(magic, sizeof(magic), stream) != NULL);
	CHECK(strcmp(magic, "combined-indexed-collision-v1\n") == 0);
	CHECK(fgets(context, sizeof(context), stream) != NULL && strlen(context) == 81U);
	CHECK(context[80] == '\n');
	for (index = 0; index < NATIVE_CONTEXT; index++) {
		byte = model->contexts[8][index];
		CHECK(context[index * 2U] == "0123456789abcdef"[byte >> 4]);
		CHECK(context[index * 2U + 1U] == "0123456789abcdef"[byte & 15U]);
	}
	CHECK(fgets(line, sizeof(line), stream) != NULL && fgetc(stream) == EOF);
	CHECK(!ferror(stream) && fclose(stream) == 0);
	cursor = line;
	for (index = 0; index < 4U; index++) {
		CHECK(*cursor >= '0' && *cursor <= '9');
		CHECK(cursor[0] != '0' || cursor[1] == (index == 3U ? '\n' : ' '));
		value = strtoul(cursor, &end, 10);
		CHECK(value < 262144U && *end == (index == 3U ? '\n' : ' '));
		model->collision[index] = (unsigned int)value;
		cursor = end + 1;
	}
	CHECK(*cursor == 0);
	for (index = 0; index < 4U; index++) {
		for (byte = 0; byte < index; byte++) {
			CHECK(model->collision[index] != model->collision[byte]);
		}
	}
}

static void
indexed_capture(struct ext4_fs *fs, struct indexed_model *model, const char *plan)
{
	struct ext4_encryption_policy policy;
	char name[EXT4_NAME_MAX + 1U];
	unsigned int parent;
	unsigned int slot;
	unsigned int index;
	int length;

	for (parent = 0; parent < 2U; parent++) {
		length = snprintf(name, sizeof(name), "indexed-%u", parent);
		CHECK(length > 0 && (size_t)length < sizeof(name));
		model->directories[parent * 4U] = find(fs, EXT4_ROOT_INODE, name);
		length = snprintf(name, sizeof(name), "indexed-peer-%u", parent);
		CHECK(length > 0 && (size_t)length < sizeof(name));
		model->directories[parent * 4U + 1U] = find(fs, EXT4_ROOT_INODE, name);
		model->directories[parent * 4U + 2U] = find(fs, model->directories[parent * 4U].number, "Child");
		model->directories[parent * 4U + 3U] = find(fs, model->directories[parent * 4U + 2U].number, "Grandchild");
		for (slot = 0; slot < 5U; slot++) {
			indexed_name(name, "Bulk", slot);
			model->files[parent][slot] = slot == 4U ?
			    find(fs, model->directories[parent * 4U + 1U].number, "Exchange") :
			    find(fs, model->directories[parent * 4U].number, name);
			native_context(fs, &model->files[parent][slot], model->file_contexts[parent][slot]);
		}
	}
	if (model->small) {
		model->directories[8] = find(fs, EXT4_ROOT_INODE, "indexed-collision");
	}
	for (index = 0; index < (model->small ? 9U : 8U); index++) {
		native_context(fs, &model->directories[index], model->contexts[index]);
		EXPECT(ext4_get_encryption_policy(fs, &model->directories[index], &policy), EXT4_OK);
		CHECK(policy.version == FSCRYPT_V2 && policy.contents_mode == EXT4_FSCRYPT_MODE_AES_256_XTS &&
		    policy.filenames_mode == EXT4_FSCRYPT_MODE_AES_256_CTS);
		CHECK(policy.flags == (index >= 4U && index < 8U ? FSCRYPT_PAD_32 : 0));
	}
	if (model->small) {
		indexed_plan(model, plan);
		for (slot = 0; slot < 2U; slot++) {
			indexed_name(name, "Collision", model->collision[slot == 0U ? 0U : 3U]);
			model->files[2][slot] = find(fs, model->directories[8].number, name);
			native_context(fs, &model->files[2][slot], model->file_contexts[2][slot]);
		}
		CHECK(model->files[2][0].number != model->files[2][1].number);
	}
}

static void
indexed_mutate(struct ext4_fs *fs, struct indexed_model *model,
    struct keyring *keyring, const struct ext4_crypto_environment *crypto)
{
	struct ext4_inode main;
	struct ext4_inode peer;
	struct ext4_inode result;
	struct ext4_rename_entry from;
	struct ext4_rename_entry to;
	char name[EXT4_NAME_MAX + 1U];
	char encoded[EXT4_NAME_MAX + 1U];
	unsigned int parent;
	unsigned int index;
	unsigned int slot;

	for (parent = 0; parent < 2U; parent++) {
		indexed_directory(fs, model, parent * 4U, &main);
		indexed_directory(fs, model, parent * 4U + 1U, &peer);
		for (index = model->baseline; index < model->total; index++) {
			indexed_name(name, "Bulk", index);
			slot = index % 4U;
			EXPECT(ext4_link(fs, main.number, main.generation, (const uint8_t *)name, strlen(name),
			    model->files[parent][slot].number, model->files[parent][slot].generation,
			    &encrypt_time, &result), EXT4_OK);
		}
		indexed_name(name, "Bulk", 1);
		from = entry_of(&main, name, &model->files[parent][1]);
		to = entry_of(&peer, "Moved", NULL);
		EXPECT(ext4_rename(fs, &from, &to, EXT4_RENAME_NOREPLACE, &encrypt_time, &result), EXT4_OK);
		indexed_name(name, "Bulk", 2);
		from = entry_of(&main, name, &model->files[parent][2]);
		to = entry_of(&peer, "Exchange", &model->files[parent][4]);
		EXPECT(ext4_rename(fs, &from, &to, EXT4_RENAME_EXCHANGE, &encrypt_time, &result), EXT4_OK);
		EXPECT(ext4_link(fs, peer.number, peer.generation, (const uint8_t *)"Linked", 6,
		    model->files[parent][3].number, model->files[parent][3].generation,
		    &encrypt_time, &result), EXT4_OK);
		from = entry_of(&main, "Child", &model->directories[parent * 4U + 2U]);
		to = entry_of(&peer, "Moved-Child", NULL);
		EXPECT(ext4_rename(fs, &from, &to, EXT4_RENAME_NOREPLACE, &encrypt_time, &result), EXT4_OK);
		indexed_directory(fs, model, parent * 4U, &main);
		indexed_name(name, "Bulk", 0);
		indexed_nokey_name(fs, &main, name, encoded);
		EXPECT(ext4_set_crypto(fs, NULL), EXT4_OK);
		CHECK(keyring->handles == 0);
		EXPECT(ext4_lookup(fs, &main, (const uint8_t *)encoded, strlen(encoded), &result), EXT4_OK);
		CHECK(result.number == model->files[parent][0].number);
		EXPECT(ext4_unlink(fs, main.number, main.generation, (const uint8_t *)encoded,
		    strlen(encoded), result.number, result.generation, &encrypt_time, &result), EXT4_OK);
		EXPECT(ext4_set_crypto(fs, crypto), EXT4_OK);
	}
	if (model->small) {
		indexed_directory(fs, model, 8, &main);
		indexed_name(name, "Collision", model->collision[2]);
		EXPECT(ext4_link(fs, main.number, main.generation, (const uint8_t *)name, strlen(name),
		    model->files[2][1].number, model->files[2][1].generation, &encrypt_time, &result), EXT4_OK);
	}
}

static void
indexed_keyless_export(struct ext4_fs *fs, struct indexed_model *model, const char *exports)
{
	struct indexed_listing *listing = calloc(1, sizeof(*listing));
	struct ext4_inode inode;
	struct ext4_inode result;
	char path[1024];
	uint8_t byte;
	uint64_t cookie;
	size_t completed;
	FILE *stream;
	unsigned int directory;
	unsigned int index;
	int length = snprintf(path, sizeof(path), "%s/indexed.nokey", exports);

	CHECK(listing != NULL && length > 0 && (size_t)length < sizeof(path));
	stream = fopen(path, "wx");
	CHECK(stream != NULL);
	for (directory = 0; directory < (model->small ? 9U : 8U); directory++) {
		indexed_directory(fs, model, directory, &inode);
		memset(listing, 0, sizeof(*listing));
		cookie = 0;
		EXPECT(ext4_iterate_dir(fs, &inode, &cookie, indexed_collect, listing), EXT4_NOT_FOUND);
		for (index = 0; index < listing->count; index++) {
			if (strcmp(listing->names[index], ".") == 0 || strcmp(listing->names[index], "..") == 0) {
				continue;
			}
			EXPECT(ext4_lookup(fs, &inode, (const uint8_t *)listing->names[index],
			    strlen(listing->names[index]), &result), EXT4_OK);
			CHECK(result.number == listing->numbers[index]);
			if ((result.mode & EXT4_MODE_TYPE) == EXT4_MODE_REGULAR) {
				byte = 0xa5;
				completed = 77;
				EXPECT(ext4_read(fs, &result, 0, &byte, 1, &completed), EXT4_ENCRYPTED);
				CHECK(completed == 0 && byte == 0xa5);
			}
			CHECK(fprintf(stream, "%s %u %s\n", indexed_ids[directory], listing->numbers[index],
			    listing->names[index]) > 0);
		}
	}
	CHECK(fclose(stream) == 0);
	free(listing);
}

static void
indexed_roundtrip(struct device *device, const char *exports, const char *source, const char *plan)
{
	struct indexed_model *model = calloc(1, sizeof(*model));
	struct keyring keyring;
	struct ext4_crypto_environment crypto;
	struct ext4_fs *fs;
	uint8_t *before = malloc(device->size);
	uint32_t events;

	CHECK(model != NULL && before != NULL && exports != NULL);
	CHECK(device->size <= 32U * 1024U * 1024U &&
	    (device->block_size == 1024U || device->block_size == 4096U));
	model->small = device->block_size == 1024U;
	model->baseline = model->small ? 384U : 64U;
	model->total = model->baseline + (model->small ? 64U : 32U);
	keyring_init(&keyring, PROBE_KEY_OFFSET);
	crypto = keyring_environment(&keyring);
	EXPECT(ext4_mount_writable(&device->environment, &device->writer, &fs), EXT4_OK);
	EXPECT(ext4_set_crypto(fs, &crypto), EXT4_OK);
	indexed_capture(fs, model, plan);
	indexed_verify(fs, model, INDEXED_OLD);
	indexed_mutate(fs, model, &keyring, &crypto);
	indexed_verify(fs, model, INDEXED_FULL);
	EXPECT(ext4_sync(fs), EXT4_OK);
	ext4_unmount(fs);
	CHECK(device->live == 0 && keyring.handles == 0);
	CHECK(memcmp(device->cache, device->stable, device->size) == 0);
	memcpy(before, device->stable, device->size);
	events = device->events;
	EXPECT(ext4_mount(&device->environment, &fs), EXT4_OK);
	indexed_keyless_export(fs, model, exports);
	EXPECT(ext4_set_crypto(fs, &crypto), EXT4_OK);
	indexed_verify(fs, model, INDEXED_FULL);
	ext4_unmount(fs);
	CHECK(device->live == 0 && keyring.handles == 0 && device->events == events);
	CHECK(memcmp(before, device->cache, device->size) == 0);
	storage_export(device, exports, source, "indexed-");
	free(before);
	free(model);
	puts("PASS native indexed import/mutation/export: bounded bulk, long no-key names and directory moves");
}

/* Each process emits one bounded cut. The wrapper checks it independently before
 * retaining a representative or removing the already-verified transient image. */
#define INDEXED_CUT_LIMIT 64U

struct indexed_trace {
	struct device *device;
	char kinds[INDEXED_CUT_LIMIT];
	unsigned int count;
	bool active;
};

static void
indexed_trace_event(struct indexed_trace *trace, char kind)
{
	if (trace->active) {
		CHECK(trace->count < INDEXED_CUT_LIMIT);
		trace->kinds[trace->count++] = kind;
	}
}

static enum ext4_result
indexed_trace_write(void *context, uint64_t offset, const void *bytes, size_t size)
{
	struct indexed_trace *trace = context;
	enum ext4_result error;

	indexed_trace_event(trace, 'w');
	error = device_write(trace->device, offset, bytes, size);
	return error;
}

static enum ext4_result
indexed_trace_flush(void *context)
{
	struct indexed_trace *trace = context;

	indexed_trace_event(trace, 'f');
	return device_flush(trace->device);
}

static enum ext4_result
indexed_target(struct ext4_fs *fs, struct indexed_model *model, bool collision)
{
	struct ext4_inode main;
	struct ext4_inode peer;
	struct ext4_inode result;
	struct ext4_rename_entry from;
	struct ext4_rename_entry to;
	char name[EXT4_NAME_MAX + 1U];

	indexed_directory(fs, model, collision ? 8U : 0U, &main);
	if (collision) {
		indexed_name(name, "Collision", model->collision[2]);
		return ext4_link(fs, main.number, main.generation, (const uint8_t *)name, strlen(name),
		    model->files[2][1].number, model->files[2][1].generation, &encrypt_time, &result);
	}
	indexed_directory(fs, model, 1U, &peer);
	indexed_name(name, "Bulk", 1);
	from = entry_of(&main, name, &model->files[0][1]);
	to = entry_of(&peer, "Moved", NULL);
	return ext4_rename(fs, &from, &to, EXT4_RENAME_NOREPLACE, &encrypt_time, &result);
}

static enum indexed_endpoint
indexed_classify(struct ext4_fs *fs, struct indexed_model *model, bool collision)
{
	struct ext4_inode directory;
	struct ext4_inode inode;
	char name[EXT4_NAME_MAX + 1U];
	enum ext4_result error;

	indexed_directory(fs, model, collision ? 8U : 1U, &directory);
	if (collision) {
		indexed_name(name, "Collision", model->collision[2]);
	} else {
		memcpy(name, "Moved", 6);
	}
	error = ext4_lookup(fs, &directory, (const uint8_t *)name, strlen(name), &inode);
	CHECK(error == EXT4_OK || error == EXT4_NOT_FOUND);
	if (error == EXT4_NOT_FOUND) {
		return INDEXED_OLD;
	}
	CHECK(inode.number == model->files[collision ? 2U : 0U][1].number);
	return collision ? INDEXED_COLLISION : INDEXED_RENAME;
}

static void
indexed_media_hash(struct device *device, uint8_t *digest)
{
	struct ext4_sha256 hash;

	ext4_sha256_init(&hash);
	ext4_sha256_update(&hash, device->stable, device->size);
	ext4_sha256_final(&hash, digest);
}

static void
indexed_cut_report(const char *exports, const struct indexed_trace *trace,
    const char *operation, unsigned int cut, const uint8_t *before, const uint8_t *after)
{
	char path[1024];
	FILE *stream;
	unsigned int index;
	int length = snprintf(path, sizeof(path), "%s/indexed-crash.json", exports);

	CHECK(length > 0 && (size_t)length < sizeof(path));
	stream = fopen(path, "wx");
	CHECK(stream != NULL);
	CHECK(fprintf(stream, "{\"schema\":\"indexed-whole-write-cut-v1\",\"operation\":\"%s\","
	    "\"cut\":%u,\"events\":%u,\"survival\":%u,\"partial\":false,"
	    "\"recovery\":\"%s\",\"event_types\":[", operation, cut, trace->count,
	    cut % 2U, cut == 0 ? "reference" : "success") > 0);
	for (index = 0; index < trace->count; index++) {
		CHECK(fprintf(stream, "%s\"%s\"", index == 0 ? "" : ",",
		    trace->kinds[index] == 'w' ? "write" : "flush") > 0);
	}
	CHECK(fprintf(stream, "],\"pre_recovery_sha256\":\"") > 0);
	native_hex(stream, before, EXT4_SHA256_DIGEST_SIZE);
	CHECK(fprintf(stream, "\",\"export_sha256\":\"") > 0);
	native_hex(stream, after, EXT4_SHA256_DIGEST_SIZE);
	CHECK(fprintf(stream, "\"}\n") > 0 && fclose(stream) == 0);
}

static void
indexed_crash(struct device *device, const char *exports, const char *source,
    const char *plan, const char *operation, unsigned int cut)
{
	struct indexed_model *model = calloc(1, sizeof(*model));
	struct indexed_trace trace = { .device = device };
	struct ext4_write_environment writer = device->writer;
	struct ext4_recovery_report recovery;
	struct keyring keyring;
	struct ext4_crypto_environment crypto;
	struct ext4_fs *fs;
	uint8_t before[EXT4_SHA256_DIGEST_SIZE];
	uint8_t after[EXT4_SHA256_DIGEST_SIZE];
	uint8_t *stable = malloc(device->size);
	uint32_t events;
	bool collision = strcmp(operation, "collision") == 0;
	enum indexed_endpoint endpoint;
	enum ext4_result error;

	CHECK(model != NULL && stable != NULL && exports != NULL && cut <= INDEXED_CUT_LIMIT);
	CHECK(collision || strcmp(operation, "rename") == 0);
	CHECK(device->size <= 32U * 1024U * 1024U &&
	    (device->block_size == 1024U || device->block_size == 4096U));
	model->small = device->block_size == 1024U;
	CHECK(!collision || model->small);
	model->baseline = model->small ? 384U : 64U;
	model->total = model->baseline + (model->small ? 64U : 32U);
	keyring_init(&keyring, PROBE_KEY_OFFSET);
	crypto = keyring_environment(&keyring);
	device->writer.context = &trace;
	device->writer.write = indexed_trace_write;
	device->writer.flush = indexed_trace_flush;
	EXPECT(ext4_mount_writable(&device->environment, &device->writer, &fs), EXT4_OK);
	EXPECT(ext4_set_crypto(fs, &crypto), EXT4_OK);
	indexed_capture(fs, model, plan);
	if (cut != 0) {
		device->stop_at = device->events + cut;
		device->survival = cut % 2U;
		device->partial = false;
	}
	trace.active = true;
	error = indexed_target(fs, model, collision);
	trace.active = false;
	CHECK(trace.count != 0);
	if (cut == 0) {
		EXPECT(error, EXT4_OK);
		CHECK(!device->off);
	} else {
		EXPECT(error, EXT4_IO);
		CHECK(device->off && trace.count == cut);
	}
	indexed_media_hash(device, before);
	if (cut != 0) {
		ext4_unmount(fs);
		CHECK(device->live == 0 && keyring.handles == 0);
		device->writer = writer;
		device_reset(device, device->stable);
		/* This bounded whole-write model requires recovery. A refusal is a
		 * failed test, not an accepted torn-sector or reordered-write outcome. */
		EXPECT(ext4_recover(&device->environment, &device->writer, &recovery), EXT4_OK);
		EXPECT(ext4_mount_writable(&device->environment, &device->writer, &fs), EXT4_OK);
		EXPECT(ext4_set_crypto(fs, &crypto), EXT4_OK);
	}
	endpoint = indexed_classify(fs, model, collision);
	if (cut == 0) {
		CHECK(endpoint == (collision ? INDEXED_COLLISION : INDEXED_RENAME));
	}
	indexed_verify(fs, model, endpoint);
	EXPECT(ext4_sync(fs), EXT4_OK);
	ext4_unmount(fs);
	device->writer = writer;
	CHECK(device->live == 0 && keyring.handles == 0);
	CHECK(memcmp(device->cache, device->stable, device->size) == 0);
	memcpy(stable, device->stable, device->size);
	events = device->events;
	EXPECT(ext4_mount(&device->environment, &fs), EXT4_OK);
	indexed_keyless_export(fs, model, exports);
	EXPECT(ext4_set_crypto(fs, &crypto), EXT4_OK);
	indexed_verify(fs, model, endpoint);
	ext4_unmount(fs);
	CHECK(device->live == 0 && keyring.handles == 0 && device->events == events);
	CHECK(memcmp(stable, device->cache, device->size) == 0);
	indexed_media_hash(device, after);
	storage_export(device, exports, source, "indexed-");
	indexed_cut_report(exports, &trace, operation, cut, before, after);
	free(stable);
	free(model);
	printf("PASS native indexed %s whole-write cut %u: %u target events\n",
	    operation, cut, trace.count);
}

#endif

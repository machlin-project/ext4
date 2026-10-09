/* SPDX-License-Identifier: BSD-3-Clause */
#include "directory_index.h"
#include "directory_write.h"
#include "fscrypt.h"
#include "keyring.h"
#include "xattr.h"

#include <stdio.h>

#define CHECK(value) do { if (!(value)) { \
	fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #value); exit(1); } } while (0)
#define EXPECT(value, wanted) do { enum ext4_result actual = (value); \
	if (actual != (wanted)) { fprintf(stderr, "%s:%d: %s: %s, expected %s\n", \
	__FILE__, __LINE__, #value, ext4_result_string(actual), ext4_result_string(wanted)); \
	exit(1); } } while (0)
#define MODEL_BLOCKS 64U
#define MODEL_DIRECTORY 20U
#define MODEL_LEAF 22U
#define MODEL_INODE_SIZE 256U
#define FIELD_COUNT 15U
#define FOLD_LIMIT 4096U

enum field { NAME_LENGTH, QUERY_LENGTH, CIPHER_LENGTH, HASH_LENGTH, QUERY_HASH_LENGTH,
	STORED_MAJOR, STORED_MINOR, QUERY_MAJOR, QUERY_MINOR, PREPARED_FILTER,
	RELAXED_MATCH, STRICT_MATCH, NAME_VALID, QUERY_VALID, PADDING };

/* Directory/inode wire model around independently authored EVP filename fixtures.
 * This is not a mounted Linux-authored filesystem or a native acceptance test. */
struct fixture {
	uint32_t fields[FIELD_COUNT];
	uint8_t *name;
	uint8_t *query;
	uint8_t *cipher;
	uint8_t *hash;
	uint8_t *query_hash;
	uint8_t *nokey;
	size_t nokey_length;
	uint8_t context[40];
	uint8_t keys[48];
};

struct model {
	struct keyring keyring;
	struct ext4_fs fs;
	struct ext4_inode directory;
	const struct fixture *fixture;
	uint8_t *device;
	size_t size;
	uint32_t live;
	uint32_t allocations;
	uint32_t reads;
	uint32_t writes;
	uint32_t flushes;
	uint32_t fail_allocation;
	uint32_t fail_read;
	uint32_t cipher_calls;
	uint32_t hash_calls;
	uint32_t derives;
	uint64_t forced_hash;
	bool force_hash;
	bool hash_original;
	bool allow_encrypt;
	bool missing_key;
	bool fail_hash;
	bool fail_cipher;
	bool fail_hash_key;
};

static uint8_t *
load(const char *directory, const char *name, size_t limit, size_t *length)
{
	char path[512];
	FILE *file;
	uint8_t *bytes;
	int written;
	long size;

	written = snprintf(path, sizeof(path), "%s/%s", directory, name);
	CHECK(written >= 0 && (size_t)written < sizeof(path));
	file = fopen(path, "rb");
	CHECK(file != NULL && fseek(file, 0, SEEK_END) == 0);
	size = ftell(file);
	CHECK(size >= 0 && (size_t)size <= limit && fseek(file, 0, SEEK_SET) == 0);
	bytes = malloc((size_t)size + 1U);
	CHECK(bytes != NULL && fread(bytes, 1, (size_t)size, file) == (size_t)size);
	CHECK(fclose(file) == 0);
	bytes[size] = 0;
	*length = (size_t)size;
	return bytes;
}

static uint8_t *
part(const char *directory, const char *stem, const char *suffix, size_t expected)
{
	char name[128];
	uint8_t *bytes;
	size_t size;
	int written = snprintf(name, sizeof(name), "%s.%s", stem, suffix);

	CHECK(written >= 0 && (size_t)written < sizeof(name));
	bytes = load(directory, name, expected, &size);
	CHECK(size == expected);
	return bytes;
}

static void *
model_allocate(void *context, size_t size)
{
	struct model *model = context;
	void *buffer;

	if (++model->allocations == model->fail_allocation) {
		return NULL;
	}
	buffer = malloc(size);

	CHECK(buffer != NULL);
	model->live++;
	return buffer;
}

static void
model_release(void *context, void *buffer, size_t size)
{
	struct model *model = context;

	(void)size;
	CHECK(buffer != NULL && model->live != 0);
	model->live--;
	free(buffer);
}

static enum ext4_result
model_read(void *context, uint64_t offset, void *buffer, size_t length)
{
	struct model *model = context;

	CHECK(offset <= model->size && length <= model->size - offset);
	if (++model->reads == model->fail_read) {
		return EXT4_IO;
	}
	memcpy(buffer, model->device + offset, length);
	return EXT4_OK;
}

static enum ext4_result
model_find(void *context, uint8_t version, const uint8_t *identifier, size_t length, void **key)
{
	struct model *model = context;

	return model->missing_key ? EXT4_NOT_FOUND :
	    keyring_find(&model->keyring, version, identifier, length, key);
}

static enum ext4_result
model_derive(void *context, void *master, uint8_t version, const uint8_t *info,
    size_t info_length, size_t key_length, void **result)
{
	struct model *model = context;
	enum ext4_result error;

	CHECK(version == 2 && info_length == 25 && memcmp(info, "fscrypt", 8) == 0);
	CHECK(memcmp(info + 9, model->fixture->context + 24, 16) == 0);
	CHECK((info[8] == 2 && key_length == 32) || (info[8] == 5 && key_length == 16));
	model->derives++;
	if (info[8] == 5 && model->fail_hash_key) {
		return EXT4_IO;
	}
	error = keyring_derive(context, master, version, info, info_length, key_length, result);
	if (error == EXT4_OK) {
		CHECK(memcmp(((struct key *)*result)->bytes,
			model->fixture->keys + (info[8] == 5 ? 32 : 0), key_length) == 0);
	}
	return error;
}

static enum ext4_result
model_cipher(void *context, void *key, uint8_t mode, bool encrypt, const uint8_t *iv,
    const void *input, void *output, size_t length)
{
	struct model *model = context;

	model->cipher_calls++;
	CHECK((!encrypt || model->allow_encrypt) && mode == EXT4_FSCRYPT_MODE_AES_256_CTS);
	return model->fail_cipher ? EXT4_IO :
	    keyring_cipher(context, key, mode, encrypt, iv, input, output, length);
}

static enum ext4_result
model_hash(void *context, void *key, const uint8_t *name, size_t length, uint64_t *hash)
{
	struct model *model = context;
	uint32_t wanted = model->fixture->fields[model->hash_original ? HASH_LENGTH : QUERY_HASH_LENGTH];
	const uint8_t *bytes = model->hash_original ? model->fixture->hash : model->fixture->query_hash;

	model->hash_calls++;
	CHECK(length == wanted);
	CHECK(memcmp(name, bytes, length) == 0);
	if (model->force_hash && !model->fail_hash) {
		*hash = model->forced_hash;
		return EXT4_OK;
	}
	return model->fail_hash ? EXT4_IO : keyring_siphash(context, key, name, length, hash);
}

static struct ext4_crypto_environment
model_crypto(struct model *model)
{
	struct ext4_crypto_environment crypto = keyring_environment(&model->keyring);

	crypto.context = model;
	crypto.find_key = model_find;
	crypto.derive_key = model_derive;
	crypto.cipher = model_cipher;
	crypto.siphash = model_hash;
	return crypto;
}

static void
record(uint8_t *bytes, uint32_t number, uint32_t length, const uint8_t *name, uint8_t names)
{
	struct ext4_dir_header_disk *header = (void *)bytes;

	ext4_encode32(&header->inode, number);
	ext4_encode16(&header->record_length, length == 65536U ? UINT16_MAX : (uint16_t)length);
	header->name_length = names;
	header->type = number == EXT4_ROOT_INODE ? EXT4_FT_DIRECTORY : EXT4_FT_REGULAR;
	memcpy(bytes + sizeof(*header), name, names);
}

static void
seal_directory(struct model *model, uint32_t logical, uint8_t *bytes)
{
	struct ext4_fs *fs = &model->fs;
	struct ext4_dir_tail_disk *tail;
	struct ext4_dx_count_disk *count;
	struct ext4_dx_tail_disk *index_tail;
	uint32_t checksum;
	size_t base = sizeof(struct ext4_dx_root_prefix_disk);

	if (!fs->metadata_checksum) {
		return;
	}
	if ((model->directory.flags & EXT4_INODE_INDEX) && logical == 0) {
		count = (void *)(bytes + base);
		index_tail = (void *)(bytes + base + ext4_le16(&count->limit) * sizeof(*count));
		memset(index_tail, 0, sizeof(*index_tail));
		checksum = ext4_crc32c(ext4_inode_seed(fs, &model->directory), bytes,
		    base + ext4_le16(&count->count) * sizeof(*count));
		checksum = ext4_crc32c(checksum, index_tail, sizeof(*index_tail));
		ext4_encode32(&index_tail->checksum, checksum);
	} else {
		tail = (void *)(bytes + fs->info.block_size - sizeof(*tail));
		memset(tail, 0, sizeof(*tail));
		ext4_encode16(&tail->record_length, sizeof(*tail));
		tail->type = EXT4_DIRECTORY_TAIL_TYPE;
		checksum = ext4_crc32c(ext4_inode_seed(fs, &model->directory), bytes,
		    fs->info.block_size - sizeof(*tail));
		ext4_encode32(&tail->checksum, checksum);
	}
}

static void
model_init(struct model *model, const struct fixture *fixture, uint32_t block_size,
    bool indexed, bool checksum, bool strict)
{
	struct ext4_fs *fs = &model->fs;
	struct ext4_crypto_environment crypto;
	struct ext4_group_disk *group;
	struct ext4_inode_disk *disk;
	struct ext4_xattr_entry_disk *attribute;
	struct ext4_dx_root_prefix_disk *root;
	struct ext4_dx_count_disk *count;
	struct ext4_dx_entry_disk *index;
	struct ext4_dir_hash_disk *hash;
	uint8_t *bytes;
	uint8_t *leaf;
	uint32_t number;
	uint32_t offset = indexed ? 0U : 24U;
	uint32_t limit = block_size - (checksum ? sizeof(struct ext4_dir_tail_disk) : 0U);
	uint32_t hash_offset = 8U + ((fixture->fields[CIPHER_LENGTH] + 3U) & ~3U);

	model->fixture = fixture;
	model->size = (size_t)block_size * MODEL_BLOCKS;
	model->device = calloc(1, model->size);
	CHECK(model->device != NULL);
	keyring_init(&model->keyring, 3);
	crypto = model_crypto(model);
	fs->environment = (struct ext4_environment){ model, model->size, model_read,
		model_allocate, model_release };
	EXPECT(ext4_set_crypto(fs, &crypto), EXT4_OK);
	fs->info.block_size = block_size;
	fs->info.blocks = MODEL_BLOCKS;
	fs->info.inodes = 32;
	fs->info.groups = 1;
	fs->info.feature_compat = EXT4_FEATURE_COMPAT_EXT_ATTR | EXT4_FEATURE_COMPAT_DIR_INDEX;
	fs->info.feature_incompat = EXT4_FEATURE_INCOMPAT_ENCRYPT | EXT4_FEATURE_INCOMPAT_CASEFOLD |
	    EXT4_FEATURE_INCOMPAT_FILETYPE;
	fs->info.feature_ro_compat = checksum ? EXT4_FEATURE_RO_METADATA_CSUM : 0;
	fs->metadata_checksum = checksum;
	fs->checksum_seed = 0x12345678U;
	fs->inode_size = MODEL_INODE_SIZE;
	fs->descriptor_size = EXT4_GROUP_BASE_SIZE;
	fs->inodes_per_group = 32;
	fs->blocks_per_group = MODEL_BLOCKS;
	fs->clusters_per_group = MODEL_BLOCKS;
	fs->cluster_blocks = 1;
	fs->casefold_strict = strict;
	group = (void *)(model->device + (EXT4_SUPER_OFFSET / block_size + 1U) * block_size);
	ext4_encode32(&group->block_bitmap_lo, 3);
	ext4_encode32(&group->inode_bitmap_lo, 4);
	ext4_encode32(&group->inode_table_lo, 5);
	memset(model->device + 4U * block_size, 0xff, 4);
	if (checksum) {
		ext4_encode16(&group->inode_bitmap_checksum_lo,
		    (uint16_t)ext4_crc32c(fs->checksum_seed, model->device + 4U * block_size, 4));
		ext4_group_checksum_set(fs, 0, group);
	}
	for (number = 2; number <= 3; number++) {
		bytes = model->device + 5U * block_size + (number - 1U) * MODEL_INODE_SIZE;
		disk = (void *)bytes;
		ext4_encode16(&disk->mode, number == 2 ? EXT4_MODE_DIRECTORY | 0755 : EXT4_MODE_REGULAR | 0644);
		ext4_encode16(&disk->links, 1);
		ext4_encode32(&disk->generation, 1);
		ext4_encode32(&disk->flags, EXT4_INODE_ENCRYPT | (number == 2 ? EXT4_INODE_CASEFOLD |
		    (indexed ? EXT4_INODE_INDEX : 0U) : 0U));
		if (number == 2) {
			ext4_encode32(&disk->size_lo, (indexed ? 2U : 1U) * block_size);
			ext4_encode32(&disk->blocks_lo, (indexed ? 2U : 1U) * block_size / 512U);
			ext4_encode32((struct ext4_le32 *)disk->block_data, MODEL_DIRECTORY);
			if (indexed) {
				ext4_encode32((struct ext4_le32 *)(disk->block_data + 4), MODEL_LEAF);
			}
		}
		ext4_encode16(&disk->extra_size, 32);
		ext4_encode32((struct ext4_le32 *)(bytes + 160), EXT4_XATTR_MAGIC);
		attribute = (void *)(bytes + 164);
		attribute->name_index = EXT4_XATTR_INDEX_ENCRYPTION;
		attribute->name_length = 1;
		bytes[180] = 'c';
		ext4_encode16(&attribute->value_offset, 52);
		ext4_encode32(&attribute->value_size, 40);
		memcpy(bytes + 216, fixture->context, 40);
		ext4_inode_checksum_set(fs, number, disk);
	}
	EXPECT(ext4_get_inode(fs, 2, &model->directory), EXT4_OK);
	bytes = model->device + MODEL_DIRECTORY * block_size;
	record(bytes, 2, 12, (const uint8_t *)".", 1);
	record(bytes + 12, 2, indexed ? block_size - 12U : 12U, (const uint8_t *)"..", 2);
	if (indexed) {
		root = (void *)bytes;
		root->hash_version = EXT4_HASH_SIPHASH;
		root->info_length = 8;
		count = (void *)(bytes + sizeof(*root));
		ext4_encode16(&count->limit,
		    (uint16_t)((block_size - sizeof(*root) - (checksum ? 8U : 0U)) / 8U));
		ext4_encode16(&count->count, 1);
		index = (void *)count;
		ext4_encode32(&index->block, 1);
	}
	leaf = model->device + (indexed ? MODEL_LEAF : MODEL_DIRECTORY) * block_size;
	record(leaf + offset, 3, limit - offset, fixture->cipher, (uint8_t)fixture->fields[CIPHER_LENGTH]);
	hash = (void *)(leaf + offset + hash_offset);
	ext4_encode32(&hash->major, fixture->fields[STORED_MAJOR]);
	ext4_encode32(&hash->minor, fixture->fields[STORED_MINOR]);
	seal_directory(model, 0, bytes);
	if (indexed) {
		seal_directory(model, 1, leaf);
	}
}

static void
lookup_is(struct model *model, const uint8_t *name, size_t length, enum ext4_result expected)
{
	struct ext4_inode result;
	struct ext4_inode before;

	memset(&result, 0xa5, sizeof(result));
	memcpy(&before, &result, sizeof(before));
	EXPECT(ext4_lookup(&model->fs, &model->directory, name, length, &result), expected);
	if (expected == EXT4_OK) {
		CHECK(result.number == 3);
	} else {
		CHECK(memcmp(&result, &before, sizeof(result)) == 0);
	}
	CHECK(model->live == 0);
}

static void
lookup_faults(struct model *model)
{
	struct ext4_crypto_environment crypto = model_crypto(model);
	const struct fixture *fixture = model->fixture;
	uint32_t allocations;
	uint32_t reads;
	uint32_t failure;

	EXPECT(ext4_set_crypto(&model->fs, &crypto), EXT4_OK);
	model->allocations = model->reads = 0;
	lookup_is(model, fixture->query, fixture->fields[QUERY_LENGTH], EXT4_OK);
	allocations = model->allocations;
	reads = model->reads;
	CHECK(allocations != 0 && reads != 0 && allocations < 100U && reads < 100U);
	for (failure = 1; failure <= allocations; failure++) {
		EXPECT(ext4_set_crypto(&model->fs, &crypto), EXT4_OK);
		model->allocations = model->reads = 0;
		model->fail_allocation = failure;
		lookup_is(model, fixture->query, fixture->fields[QUERY_LENGTH], EXT4_NO_MEMORY);
		model->fail_allocation = 0;
	}
	for (failure = 1; failure <= reads; failure++) {
		EXPECT(ext4_set_crypto(&model->fs, &crypto), EXT4_OK);
		model->allocations = model->reads = 0;
		model->fail_read = failure;
		lookup_is(model, fixture->query, fixture->fields[QUERY_LENGTH], EXT4_IO);
		model->fail_read = 0;
	}
	EXPECT(ext4_set_crypto(&model->fs, &crypto), EXT4_OK);
	CHECK(model->keyring.handles == 0 && model->live == 0);
}

static void
hash_boundaries(struct model *model)
{
	static const uint64_t values[] = { UINT64_C(0x0000000100000001),
		UINT64_C(0xfffffffe12345678), UINT64_C(0xffffffff87654321) };
	static const uint32_t majors[] = { 0, 0xfffffffcU, 0xfffffffcU };
	struct ext4_fscrypt_key key;
	struct ext4_name_hash hash;
	struct ext4_name_hash before;
	size_t index;

	EXPECT(ext4_fscrypt_key(&model->fs, &model->directory, &key), EXT4_OK);
	model->force_hash = true;
	for (index = 0; index < sizeof(values) / sizeof(values[0]); index++) {
		model->forced_hash = values[index];
		EXPECT(ext4_fscrypt_name_hash(&model->fs, &key, model->fixture->query_hash,
			   model->fixture->fields[QUERY_HASH_LENGTH], &hash), EXT4_OK);
		CHECK(hash.major == majors[index] && hash.minor == (uint32_t)values[index]);
	}
	memset(&hash, 0xa5, sizeof(hash));
	before = hash;
	model->fail_hash = true;
	EXPECT(ext4_fscrypt_name_hash(&model->fs, &key, model->fixture->query_hash,
		   model->fixture->fields[QUERY_HASH_LENGTH], &hash), EXT4_IO);
	CHECK(memcmp(&hash, &before, sizeof(hash)) == 0);
	model->fail_hash = false;
	model->force_hash = false;
}

static void
index_routes(struct model *model)
{
	struct ext4_fs *fs = &model->fs;
	const struct fixture *fixture = model->fixture;
	struct ext4_crypto_environment crypto = model_crypto(model);
	uint32_t bs = fs->info.block_size;
	uint8_t *root_bytes = model->device + MODEL_DIRECTORY * bs;
	uint8_t *node_bytes = model->device + 21U * bs;
	uint8_t *first = model->device + MODEL_LEAF * bs;
	uint8_t *second = model->device + 23U * bs;
	struct ext4_dx_root_prefix_disk *root = (void *)root_bytes;
	struct ext4_inode_disk *disk = (void *)(model->device + 5U * bs + MODEL_INODE_SIZE);
	struct ext4_dx_count_disk *counts;
	struct ext4_dx_entry_disk *entries;
	struct ext4_dir_header_disk *header = (void *)first;
	struct ext4_index_metadata metadata;
	struct ext4_inode ordinary;
	uint8_t encoded[EXT4_NAME_MAX];
	uint32_t limit = bs - (fs->metadata_checksum ? sizeof(struct ext4_dir_tail_disk) : 0U);
	uint32_t major = fixture->fields[STORED_MAJOR];
	uint32_t first_logical;
	size_t length;
	unsigned int depth;
	unsigned int collision;

	CHECK(major >= 2U);
	memcpy(second, first, bs);
	for (depth = 0; depth < 2; depth++) {
		for (collision = 0; collision < 2; collision++) {
			first_logical = depth + 1U;
			ext4_encode32(&disk->size_lo, (depth + 3U) * bs);
			ext4_encode32(&disk->blocks_lo, (depth + 3U) * bs / 512U);
			ext4_encode32((struct ext4_le32 *)(disk->block_data + 4), depth ? 21U : MODEL_LEAF);
			ext4_encode32((struct ext4_le32 *)(disk->block_data + 8), depth ? MODEL_LEAF : 23U);
			ext4_encode32((struct ext4_le32 *)(disk->block_data + 12), depth ? 23U : 0U);
			ext4_inode_checksum_set(fs, 2, disk);
			EXPECT(ext4_get_inode(fs, 2, &model->directory), EXT4_OK);
			root->indirect_levels = (uint8_t)depth;
			counts = (void *)(root_bytes + sizeof(*root));
			ext4_encode16(&counts->count, depth ? 1 : 2);
			entries = (void *)counts;
			ext4_encode32(&entries[0].block, 1);
			if (depth) {
				memset(node_bytes, 0, bs);
				record(node_bytes, 0, bs, (const uint8_t *)"", 0);
				((struct ext4_dir_header_disk *)node_bytes)->type = 0;
				counts = (void *)(node_bytes + sizeof(struct ext4_dir_header_disk));
				ext4_encode16(&counts->limit, (uint16_t)((bs - 8U -
				    (fs->metadata_checksum ? 8U : 0U)) / 8U));
				ext4_encode16(&counts->count, 2);
				entries = (void *)counts;
				ext4_encode32(&entries[0].block, first_logical);
			}
			ext4_encode32(&entries[1].hash, major | collision);
			ext4_encode32(&entries[1].block, first_logical + 1U);
			if (depth) {
				ext4_index_checksum_set(fs, &model->directory, 1, node_bytes);
			}
			seal_directory(model, 0, root_bytes);
			memset(first, 0, bs);
			record(first, 0, limit, (const uint8_t *)"x", 1);
			seal_directory(model, first_logical, first);
			lookup_is(model, fixture->query, fixture->fields[QUERY_LENGTH], EXT4_OK);
			/* An even boundary probes the second leaf directly. An odd one
			 * must validate the first leaf before following the continuation. */
			ext4_encode16(&header->record_length, 4);
			seal_directory(model, first_logical, first);
			lookup_is(model, fixture->query, fixture->fields[QUERY_LENGTH],
			    collision ? EXT4_CORRUPT : EXT4_OK);
			ext4_encode16(&header->record_length, limit == 65536U ? UINT16_MAX : (uint16_t)limit);
			seal_directory(model, first_logical, first);
			EXPECT(ext4_set_crypto(fs, NULL), EXT4_OK);
			lookup_is(model, fixture->nokey, fixture->nokey_length, EXT4_OK);
			length = ext4_fscrypt_nokey_encode(fixture->cipher, fixture->fields[CIPHER_LENGTH],
			    major - 2U, fixture->fields[STORED_MINOR], encoded);
			lookup_is(model, encoded, length, EXT4_NOT_FOUND);
			EXPECT(ext4_set_crypto(fs, &crypto), EXT4_OK);
		}
	}
	ordinary = model->directory;
	ordinary.flags &= ~(uint32_t)EXT4_INODE_CASEFOLD;
	EXPECT(ext4_index_decode(fs, &ordinary, 0, root_bytes, &metadata), EXT4_CORRUPT);
	root->hash_version = EXT4_HASH_SIPHASH + 1U;
	seal_directory(model, 0, root_bytes);
	lookup_is(model, fixture->query, fixture->fields[QUERY_LENGTH], EXT4_UNSUPPORTED);
	root->hash_version = EXT4_HASH_SIPHASH;
	seal_directory(model, 0, root_bytes);
}

static void
listing_is(struct model *model, bool nokey)
{
	struct ext4_dir_entry entry;
	uint64_t cookie = 0;
	unsigned int count = 0;
	enum ext4_result error;

	while ((error = ext4_next_dir(&model->fs, &model->directory, &cookie, &entry)) == EXT4_OK) {
		if (count < 2) {
			CHECK(entry.inode == 2 && entry.name_length == count + 1U);
			CHECK(entry.name[0] == '.' && (count == 0 || entry.name[1] == '.'));
		} else {
			CHECK(count == 2 && entry.inode == 3);
			CHECK(entry.name_length == (nokey ? model->fixture->nokey_length :
				model->fixture->fields[NAME_LENGTH]));
			CHECK(memcmp(entry.name, nokey ? model->fixture->nokey : model->fixture->name,
				entry.name_length) == 0);
		}
		count++;
	}
	EXPECT(error, EXT4_NOT_FOUND);
	CHECK(count == 3 && cookie == model->directory.size && model->live == 0);
}

static void
decode_bounds(struct model *model, bool indexed)
{
	uint8_t *bytes = model->device + (indexed ? MODEL_LEAF : MODEL_DIRECTORY) *
	    model->fs.info.block_size;
	uint32_t offset = indexed ? 0U : 24U;
	struct ext4_dir_header_disk *header = (void *)(bytes + offset);
	struct ext4_dir_header_disk saved;
	struct ext4_dir_entry entry;
	uint32_t length = 0;
	uint16_t short_length = (uint16_t)(8U + ((header->name_length + 3U) & ~3U));

	memcpy(&saved, header, sizeof(saved));
	ext4_encode16(&header->record_length, short_length);
	EXPECT(ext4_directory_entry_decode(&model->fs, bytes, offset, model->directory.flags,
		   &entry, &length, NULL), EXT4_CORRUPT);
	ext4_encode32(&header->inode, 0);
	EXPECT(ext4_directory_entry_decode(&model->fs, bytes, offset, model->directory.flags,
		   &entry, &length, NULL), EXT4_CORRUPT);
	header->name_length = 0;
	ext4_encode16(&header->record_length, 16);
	EXPECT(ext4_directory_entry_decode(&model->fs, bytes, offset, model->directory.flags,
		   &entry, &length, NULL), EXT4_CORRUPT);
	ext4_encode16(&header->record_length, 20);
	EXPECT(ext4_directory_entry_decode(&model->fs, bytes, offset, model->directory.flags,
		   &entry, &length, NULL), EXT4_OK);
	CHECK(entry.inode == 0 && length == 20);
	memcpy(header, &saved, sizeof(saved));
}

static void
prepared_requests(struct model *model)
{
	const struct fixture *fixture = model->fixture;
	struct ext4_fs *fs = &model->fs;
	struct ext4_crypto_environment crypto = model_crypto(model);
	struct ext4_directory_request request = { 0 };
	struct ext4_name_hash stored = { fixture->fields[STORED_MAJOR], fixture->fields[STORED_MINOR] };
	struct ext4_name_hash hash;
	uint8_t cipher[EXT4_NAME_MAX];
	uint32_t derives;
	bool match;

	EXPECT(ext4_set_crypto(fs, &crypto), EXT4_OK);
	model->hash_original = true;
	model->allow_encrypt = true;
	EXPECT(ext4_directory_request_open(fs, &model->directory, fixture->name,
		   fixture->fields[NAME_LENGTH], EXT4_NAME_REQUIRE_KEY, cipher, &request), EXT4_OK);
	model->allow_encrypt = false;
	CHECK(request.identity == EXT4_NAME_KEYED && request.hash_ready);
	CHECK(request.compare.name == fixture->name && request.compare.length == fixture->fields[NAME_LENGTH]);
	CHECK(request.disk_name == cipher && request.disk_length == fixture->fields[CIPHER_LENGTH]);
	CHECK(memcmp(cipher, fixture->cipher, request.disk_length) == 0);
	CHECK(request.hash.major == stored.major && request.hash.minor == stored.minor);
	EXPECT(ext4_directory_request_match(fs, &request, fixture->cipher,
		   fixture->fields[CIPHER_LENGTH], &stored, &match), EXT4_OK);
	CHECK(match == (!fs->casefold_strict || fixture->fields[NAME_VALID] != 0));
	/* Replacing the provider evicts both handles. The request owns only bytes
	 * and folds, and comparison must acquire a fresh pair rather than use them. */
	EXPECT(ext4_set_crypto(fs, &crypto), EXT4_OK);
	CHECK(model->keyring.handles == 0);
	EXPECT(ext4_directory_request_match(fs, &request, fixture->cipher,
		   fixture->fields[CIPHER_LENGTH], &stored, &match), EXT4_OK);
	CHECK(match == (!fs->casefold_strict || fixture->fields[NAME_VALID] != 0));
	model->fail_cipher = true;
	EXPECT(ext4_directory_request_match(fs, &request, fixture->cipher,
		   fixture->fields[CIPHER_LENGTH], &stored, &match), EXT4_IO);
	CHECK(!match);
	model->fail_cipher = false;
	ext4_directory_request_close(fs, &request);
	CHECK(model->live == 0);
	derives = model->derives;
	EXPECT(ext4_directory_request_open(fs, &model->directory, (const uint8_t *)".",
		   1, EXT4_NAME_REQUIRE_KEY, NULL, &request), EXT4_OK);
	CHECK(request.compare.folds == NULL && request.nokey == NULL && model->derives == derives);
	ext4_directory_request_close(fs, &request);
	EXPECT(ext4_set_crypto(fs, NULL), EXT4_OK);
	EXPECT(ext4_directory_request_open(fs, &model->directory, fixture->nokey,
		   fixture->nokey_length, EXT4_NAME_ALLOW_NOKEY_REMOVAL, cipher, &request), EXT4_OK);
	CHECK(request.identity == EXT4_NAME_NOKEY_QUERY && request.compare.folds == NULL);
	CHECK(ext4_directory_request_probe(&request));
	EXPECT(ext4_directory_request_hash(fs, &request, EXT4_HASH_SIPHASH, NULL, &hash), EXT4_OK);
	CHECK(hash.major == stored.major && hash.minor == stored.minor);
	EXPECT(ext4_directory_request_match(fs, &request, fixture->cipher,
		   fixture->fields[CIPHER_LENGTH], &stored, &match), EXT4_OK);
	CHECK(match && model->keyring.handles == 0);
	memcpy(cipher, fixture->cipher, fixture->fields[CIPHER_LENGTH]);
	request.disk_name = cipher;
	request.disk_length = fixture->fields[CIPHER_LENGTH];
	request.resolved_number = 3;
	request.identity = EXT4_NAME_NOKEY_RESOLVED;
	request.hash = stored;
	request.hash_ready = true;
	EXPECT(ext4_directory_request_match(fs, &request, fixture->cipher,
		   fixture->fields[CIPHER_LENGTH], &stored, &match), EXT4_OK);
	CHECK(match);
	cipher[0] ^= 1U;
	EXPECT(ext4_directory_request_match(fs, &request, fixture->cipher,
		   fixture->fields[CIPHER_LENGTH], &stored, &match), EXT4_OK);
	CHECK(!match);
	ext4_directory_request_close(fs, &request);
	CHECK(model->live == 0 && model->keyring.handles == 0);
	model->hash_original = false;
	EXPECT(ext4_set_crypto(fs, &crypto), EXT4_OK);
}

static void
policy_refusals(struct model *model)
{
	struct ext4_fs *fs = &model->fs;
	uint8_t *bytes = model->device + 5U * fs->info.block_size + MODEL_INODE_SIZE;
	struct ext4_inode_disk *disk = (void *)bytes;
	struct ext4_xattr_entry_disk *attribute = (void *)(bytes + 164);
	struct ext4_fscrypt_context_v1_disk *context = (void *)(bytes + 216);
	struct ext4_dir_entry entry;
	uint64_t cookie = 0;
	uint32_t reads = model->reads;
	uint32_t allocations = model->allocations;

	model->directory.flags |= EXT4_INODE_INLINE_DATA;
	lookup_is(model, model->fixture->query, model->fixture->fields[QUERY_LENGTH],
	    EXT4_UNSUPPORTED);
	EXPECT(ext4_next_dir(fs, &model->directory, &cookie, &entry), EXT4_UNSUPPORTED);
	CHECK(cookie == 0 && model->reads == reads && model->allocations == allocations);
	model->directory.flags &= ~(uint32_t)EXT4_INODE_INLINE_DATA;
	EXPECT(ext4_set_crypto(fs, NULL), EXT4_OK);
	memset(bytes + 216, 0, 40);
	context->version = 1;
	context->contents_mode = EXT4_FSCRYPT_MODE_AES_256_XTS;
	context->filenames_mode = EXT4_FSCRYPT_MODE_AES_256_CTS;
	memcpy(context->descriptor, model->keyring.descriptor, sizeof(context->descriptor));
	memcpy(context->nonce, model->fixture->context + 24, sizeof(context->nonce));
	ext4_encode32(&attribute->value_size, sizeof(*context));
	ext4_inode_checksum_set(fs, 2, disk);
	lookup_is(model, model->fixture->query, model->fixture->fields[QUERY_LENGTH],
	    EXT4_UNSUPPORTED);
	EXPECT(ext4_next_dir(fs, &model->directory, &cookie, &entry), EXT4_UNSUPPORTED);
	CHECK(cookie == 0 && model->live == 0 && model->keyring.handles == 0);
}

static void
check_case(const struct fixture *fixture, uint32_t block_size, bool indexed,
    bool checksum, bool strict)
{
	struct model *model = calloc(1, sizeof(*model));
	struct ext4_crypto_environment crypto;
	struct ext4_dir_hash_disk *hash;
	struct ext4_dx_root_prefix_disk *root;
	struct ext4_inode inode;
	uint8_t encoded[EXT4_NAME_MAX];
	uint8_t *leaf;
	uint32_t offset = indexed ? 0U : 24U;
	uint32_t calls;
	size_t length;
	bool match = fixture->fields[strict ? STRICT_MATCH : RELAXED_MATCH] != 0;

	CHECK(model != NULL);
	model_init(model, fixture, block_size, indexed, checksum, strict);
	decode_bounds(model, indexed);
	lookup_is(model, fixture->query, fixture->fields[QUERY_LENGTH],
	    match ? EXT4_OK : EXT4_NOT_FOUND);
	CHECK(model->hash_calls == 1 && model->derives == 2 && model->keyring.handles == 2);
	listing_is(model, false);
	EXPECT(ext4_lookup(&model->fs, &model->directory, (const uint8_t *)".", 1, &inode), EXT4_OK);
	CHECK(inode.number == 2);
	EXPECT(ext4_lookup(&model->fs, &model->directory, (const uint8_t *)"..", 2, &inode), EXT4_OK);
	CHECK(inode.number == 2);
	leaf = model->device + (indexed ? MODEL_LEAF : MODEL_DIRECTORY) * block_size;
	hash = (void *)(leaf + offset + 8U + ((fixture->fields[CIPHER_LENGTH] + 3U) & ~3U));
	/* Stored minor is a keyed prepared-name filter, not no-key identity. */
	ext4_encode32(&hash->minor, fixture->fields[STORED_MINOR] ^ 1U);
	seal_directory(model, indexed ? 1U : 0U, leaf);
	lookup_is(model, fixture->query, fixture->fields[QUERY_LENGTH],
	    match && !fixture->fields[PREPARED_FILTER] ? EXT4_OK : EXT4_NOT_FOUND);
	ext4_encode32(&hash->minor, fixture->fields[STORED_MINOR]);
	seal_directory(model, indexed ? 1U : 0U, leaf);
	model->fail_hash = true;
	lookup_is(model, fixture->query, fixture->fields[QUERY_LENGTH], EXT4_IO);
	model->fail_hash = false;
	if (match) {
		model->fail_cipher = true;
		lookup_is(model, fixture->query, fixture->fields[QUERY_LENGTH], EXT4_IO);
		model->fail_cipher = false;
	}
	EXPECT(ext4_set_crypto(&model->fs, NULL), EXT4_OK);
	CHECK(model->keyring.handles == 0);
	calls = model->cipher_calls;
	listing_is(model, true);
	lookup_is(model, fixture->nokey, fixture->nokey_length, EXT4_OK);
	length = ext4_fscrypt_nokey_encode(fixture->cipher, fixture->fields[CIPHER_LENGTH],
	    fixture->fields[STORED_MAJOR], fixture->fields[STORED_MINOR] ^ 1U, encoded);
	lookup_is(model, encoded, length, EXT4_OK);
	length = ext4_fscrypt_nokey_encode(fixture->cipher, fixture->fields[CIPHER_LENGTH],
	    fixture->fields[STORED_MAJOR] ^ 2U, fixture->fields[STORED_MINOR], encoded);
	lookup_is(model, encoded, length, EXT4_OK);
	CHECK(model->cipher_calls == calls && model->keyring.handles == 0);
	crypto = model_crypto(model);
	model->missing_key = true;
	crypto.siphash = NULL;
	EXPECT(ext4_set_crypto(&model->fs, &crypto), EXT4_OK);
	listing_is(model, true);
	lookup_is(model, fixture->nokey, fixture->nokey_length, EXT4_OK);
	model->missing_key = false;
	lookup_is(model, fixture->query, fixture->fields[QUERY_LENGTH], EXT4_UNSUPPORTED);
	CHECK(model->keyring.handles == 0);
	crypto = model_crypto(model);
	EXPECT(ext4_set_crypto(&model->fs, &crypto), EXT4_OK);
	model->fail_hash_key = true;
	lookup_is(model, fixture->query, fixture->fields[QUERY_LENGTH], EXT4_IO);
	CHECK(model->keyring.handles == 0 && model->fs.fscrypt_key_count == 0);
	model->fail_hash_key = false;
	if (indexed) {
		root = (void *)(model->device + MODEL_DIRECTORY * block_size);
		root->hash_version = EXT4_HASH_HALF_MD4;
		seal_directory(model, 0, (uint8_t *)root);
		lookup_is(model, fixture->query, fixture->fields[QUERY_LENGTH], EXT4_CORRUPT);
		root->hash_version = EXT4_HASH_SIPHASH;
		seal_directory(model, 0, (uint8_t *)root);
	}
	if (fixture->fields[NAME_LENGTH] == 1) {
		lookup_faults(model);
		hash_boundaries(model);
		if (indexed) {
			index_routes(model);
		}
	}
	prepared_requests(model);
	policy_refusals(model);
	EXPECT(ext4_set_crypto(&model->fs, NULL), EXT4_OK);
	CHECK(model->keyring.handles == 0 && model->live == 0);
	free(model->device);
	free(model);
}

static void
fixture_open(struct fixture *fixture, const char *directory, const char *stem)
{
	char name[128];
	uint8_t *bytes;
	char *cursor;
	char *end;
	size_t length;
	unsigned long value;
	unsigned int index;
	int written;

	written = snprintf(name, sizeof(name), "%s.meta", stem);
	CHECK(written >= 0 && (size_t)written < sizeof(name));
	bytes = load(directory, name, 512, &length);
	cursor = (char *)bytes;
	for (index = 0; index < FIELD_COUNT; index++) {
		value = strtoul(cursor, &end, 10);
		CHECK(end != cursor && value <= UINT32_MAX);
		fixture->fields[index] = (uint32_t)value;
		cursor = end;
	}
	while (*cursor == ' ' || *cursor == '\n' || *cursor == '\r') {
		cursor++;
	}
	CHECK(*cursor == 0);
	free(bytes);
	CHECK(fixture->fields[NAME_LENGTH] > 0 && fixture->fields[NAME_LENGTH] <= EXT4_NAME_MAX);
	CHECK(fixture->fields[QUERY_LENGTH] > 0 && fixture->fields[QUERY_LENGTH] <= EXT4_NAME_MAX);
	CHECK(fixture->fields[CIPHER_LENGTH] >= 16 && fixture->fields[CIPHER_LENGTH] <= EXT4_NAME_MAX);
	CHECK(fixture->fields[HASH_LENGTH] < FOLD_LIMIT && fixture->fields[QUERY_HASH_LENGTH] < FOLD_LIMIT);
	CHECK(fixture->fields[PADDING] == 0);
	fixture->name = part(directory, stem, "name", fixture->fields[NAME_LENGTH]);
	fixture->query = part(directory, stem, "query", fixture->fields[QUERY_LENGTH]);
	fixture->cipher = part(directory, stem, "cipher", fixture->fields[CIPHER_LENGTH]);
	fixture->hash = part(directory, stem, "hashbytes", fixture->fields[HASH_LENGTH]);
	fixture->query_hash = part(directory, stem, "queryhashbytes", fixture->fields[QUERY_HASH_LENGTH]);
	written = snprintf(name, sizeof(name), "%s.nokey", stem);
	CHECK(written >= 0 && (size_t)written < sizeof(name));
	fixture->nokey = load(directory, name, EXT4_NAME_MAX, &fixture->nokey_length);
	bytes = load(directory, "context.bin", sizeof(fixture->context), &length);
	CHECK(length == sizeof(fixture->context));
	memcpy(fixture->context, bytes, length);
	free(bytes);
	bytes = load(directory, "keys.bin", sizeof(fixture->keys), &length);
	CHECK(length == sizeof(fixture->keys));
	memcpy(fixture->keys, bytes, length);
	free(bytes);
}

#include "encrypted_casefold_write.h"

int
main(int argc, char **argv)
{
	static const uint32_t sizes[] = { 1024, 4096, 65536 };
	struct fixture *fixture = calloc(1, sizeof(*fixture));
	size_t size;
	unsigned int indexed;
	unsigned int checksum;
	unsigned int strict;
	uint64_t hash;

	CHECK(argc == 3 && fixture != NULL);
	fixture_open(fixture, argv[1], argv[2]);
	/* Independent provider outputs verify the test adapter itself first. */
	hash = test_siphash24(fixture->keys + 32, fixture->hash, fixture->fields[HASH_LENGTH]);
	CHECK(((uint32_t)(hash >> 32) & ~1U) == fixture->fields[STORED_MAJOR]);
	CHECK((uint32_t)hash == fixture->fields[STORED_MINOR]);
	hash = test_siphash24(fixture->keys + 32, fixture->query_hash,
	    fixture->fields[QUERY_HASH_LENGTH]);
	CHECK(((uint32_t)(hash >> 32) & ~1U) == fixture->fields[QUERY_MAJOR]);
	CHECK((uint32_t)hash == fixture->fields[QUERY_MINOR]);
	for (size = 0; size < sizeof(sizes) / sizeof(sizes[0]); size++) {
		for (indexed = 0; indexed < 2; indexed++) {
			for (checksum = 0; checksum < 2; checksum++) {
				for (strict = 0; strict < 2; strict++) {
					check_case(fixture, sizes[size], indexed != 0,
					    checksum != 0, strict != 0);
					mutation_snapshots(fixture, sizes[size], indexed != 0,
					    checksum != 0, strict != 0);
				}
			}
		}
	}
	free(fixture->name);
	free(fixture->query);
	free(fixture->cipher);
	free(fixture->hash);
	free(fixture->query_hash);
	free(fixture->nokey);
	free(fixture);
	printf("PASS encrypted casefold %s: 24 wire models, keyed and no-key lookup, provider faults\n",
	    argv[2]);
	return 0;
}

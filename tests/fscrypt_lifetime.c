/* SPDX-License-Identifier: BSD-3-Clause */
#include "fscrypt.h"
#include "xattr.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(condition) \
	do { \
		if (!(condition)) { \
			fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition); \
			exit(1); \
		} \
	} while (0)
#define MODEL_KEYS 128U

/* Wire-format ownership model. The identity cipher does not test AES. */
struct model_key {
	bool live;
	uint8_t owner;
};
struct model {
	struct ext4_fs fs;
	struct ext4_inode directory;
	struct model_key keys[MODEL_KEYS];
	uint8_t *device;
	size_t size;
	uint32_t allocated;
	uint32_t live;
	uint32_t key_live;
	uint32_t released;
	uint32_t stale_cipher;
	uint32_t expected_owner;
	uint32_t directory_derives;
	uint32_t entries;
	uint32_t evict_at;
	uint32_t fail_derive;
	bool evicted;
	bool nokey;
};

static void *
model_allocate(void *opaque, size_t size)
{
	struct model *m = opaque;
	void *p = malloc(size);

	CHECK(p != NULL);
	m->live++;
	return p;
}

static void
model_release(void *opaque, void *p, size_t size)
{
	struct model *m = opaque;

	(void)size;
	CHECK(m->live != 0);
	m->live--;
	free(p);
}

static enum ext4_result
model_read(void *opaque, uint64_t offset, void *buffer, size_t size)
{
	struct model *m = opaque;

	CHECK(offset <= m->size && size <= m->size - offset);
	memcpy(buffer, m->device + offset, size);
	return EXT4_OK;
}

static struct model_key *
model_new(struct model *m, uint8_t owner)
{
	struct model_key *key;

	CHECK(m->allocated < MODEL_KEYS);
	key = &m->keys[m->allocated++];
	key->live = true;
	key->owner = owner;
	m->key_live++;
	return key;
}

static enum ext4_result
model_find(void *opaque, uint8_t version, const uint8_t *identifier, size_t size, void **key)
{
	struct model *m = opaque;

	(void)identifier;
	CHECK(version == 2 && size == 16);
	*key = model_new(m, 0);
	return EXT4_OK;
}

static enum ext4_result
model_derive(void *opaque, void *master, uint8_t version, const uint8_t *info,
    size_t info_size, size_t key_size, void **output)
{
	struct model *m = opaque;
	struct model_key *key = master;
	uint8_t owner;

	CHECK(key->live && version == 2 && info_size == 25);
	CHECK(key_size == 32 || key_size == 64);
	owner = info[9];
	if (owner == 2 && ++m->directory_derives == m->fail_derive) {
		return EXT4_IO;
	}
	*output = model_new(m, owner);
	return EXT4_OK;
}

static void
model_key_release(void *opaque, void *handle)
{
	struct model *m = opaque;
	struct model_key *key = handle;

	CHECK(key->live && m->key_live != 0);
	key->live = false;
	key->owner = 0xff;
	m->key_live--;
	m->released++;
}

static enum ext4_result
model_cipher(void *opaque, void *handle, uint8_t mode, bool encrypt,
    const uint8_t *iv, const void *input, void *output, size_t length)
{
	struct model *m = opaque;
	struct model_key *key = handle;

	(void)iv;
	CHECK(!encrypt && (mode == EXT4_FSCRYPT_MODE_AES_256_CTS ||
	    mode == EXT4_FSCRYPT_MODE_AES_256_XTS));
	if (!key->live) {
		m->stale_cipher++;
		return EXT4_IO;
	}
	CHECK(key->owner == m->expected_owner);
	memcpy(output, input, length);
	return EXT4_OK;
}

static enum ext4_result
model_random(void *opaque, void *buffer, size_t length)
{
	(void)opaque;
	memset(buffer, 0x5a, length);
	return EXT4_OK;
}

static void
model_inode(struct model *m, uint32_t number, bool directory)
{
	uint32_t bs = m->fs.info.block_size;
	uint8_t *bytes = m->device + 5U * bs + (number - 1U) * 256U;
	struct ext4_inode_disk *disk = (void *)bytes;
	struct ext4_xattr_entry_disk *entry = (void *)(bytes + 164U);
	struct ext4_fscrypt_context_v2_disk *context = (void *)(bytes + 216U);

	ext4_encode16(&disk->mode, directory ? EXT4_MODE_DIRECTORY | 0755 : EXT4_MODE_REGULAR | 0644);
	ext4_encode16(&disk->links, 1);
	ext4_encode32(&disk->generation, 1);
	ext4_encode32(&disk->flags, EXT4_INODE_ENCRYPT);
	ext4_encode32(&disk->size_lo, bs);
	ext4_encode32(&disk->blocks_lo, bs / 512U);
	ext4_encode32((struct ext4_le32 *)disk->block_data, directory ? 20U : 21U);
	ext4_encode16(&disk->extra_size, 32);
	ext4_encode32((struct ext4_le32 *)(bytes + 160U), EXT4_XATTR_MAGIC);
	entry->name_index = EXT4_XATTR_INDEX_ENCRYPTION;
	entry->name_length = 1;
	bytes[180] = 'c';
	ext4_encode16(&entry->value_offset, 52);
	ext4_encode32(&entry->value_size, sizeof(*context));
	context->version = 2;
	context->contents_mode = EXT4_FSCRYPT_MODE_AES_256_XTS;
	context->filenames_mode = EXT4_FSCRYPT_MODE_AES_256_CTS;
	context->nonce[0] = (uint8_t)number;
}

static void
model_entry(struct model *m, uint32_t offset, uint32_t length, const char *name, uint8_t names)
{
	uint32_t bs = m->fs.info.block_size;
	uint8_t *bytes = m->device + (offset >= bs ? 22U : 20U) * bs + offset % bs;
	struct ext4_dir_header_disk *entry = (void *)bytes;

	ext4_encode32(&entry->inode, 3);
	ext4_encode16(&entry->record_length,
	    length == EXT4_MAX_BLOCK_SIZE ? UINT16_MAX : (uint16_t)length);
	entry->name_length = names;
	entry->type = EXT4_FT_REGULAR;
	memcpy(bytes + sizeof(*entry), name, strlen(name));
}

static void
model_init(struct model *m, uint32_t bs, bool cross_block, bool nokey)
{
	struct ext4_fs *fs = &m->fs;
	struct ext4_crypto_environment crypto = { 0 };
	struct ext4_group_disk *group;
	uint32_t number;

	m->nokey = nokey;
	m->size = (size_t)bs * 64U;
	m->device = calloc(1, m->size);
	CHECK(m->device != NULL);
	fs->environment = (struct ext4_environment){ m, m->size, model_read,
	    model_allocate, model_release };
	crypto.context = m;
	crypto.find_key = model_find;
	crypto.derive_key = model_derive;
	crypto.cipher = model_cipher;
	crypto.release_key = model_key_release;
	crypto.random_bytes = model_random;
	CHECK(ext4_set_crypto(fs, nokey ? NULL : &crypto) == EXT4_OK);
	m->expected_owner = 2;
	fs->info.block_size = bs;
	fs->info.blocks = 64;
	fs->info.inodes = 32;
	fs->info.groups = 1;
	fs->info.feature_compat = EXT4_FEATURE_COMPAT_EXT_ATTR;
	fs->info.feature_incompat = EXT4_FEATURE_INCOMPAT_ENCRYPT | EXT4_FEATURE_INCOMPAT_FILETYPE;
	fs->inode_size = 256;
	fs->descriptor_size = 32;
	fs->inodes_per_group = 32;
	fs->blocks_per_group = 64;
	fs->clusters_per_group = 64;
	fs->cluster_blocks = 1;
	group = (void *)(m->device + (EXT4_SUPER_OFFSET / bs + 1U) * bs);
	ext4_encode32(&group->block_bitmap_lo, 3);
	ext4_encode32(&group->inode_bitmap_lo, 4);
	ext4_encode32(&group->inode_table_lo, 5);
	memset(m->device + 4U * bs, 0xff, 4);
	for (number = 2; number <= 18; number++) {
		model_inode(m, number, number == 2);
	}
	model_entry(m, 0, 12, ".", 1);
	model_entry(m, 12, 12, "..", 2);
	if (cross_block) {
		struct ext4_inode_disk *disk = (void *)(m->device + 5U * bs + 256U);

		ext4_encode32(&disk->size_lo, 2U * bs);
		ext4_encode32(&disk->blocks_lo, 2U * bs / 512U);
		ext4_encode32((struct ext4_le32 *)(disk->block_data + 4U), 22);
		model_entry(m, 24, bs - 24U, "first", 16);
		model_entry(m, bs, bs, "second", 16);
	} else {
		model_entry(m, 24, 24, "first", 16);
		model_entry(m, 48, bs - 48U, "second", 16);
	}
	CHECK(ext4_get_inode(fs, 2, &m->directory) == EXT4_OK);
}

static enum ext4_dir_action
model_visit(void *opaque, const struct ext4_dir_entry *entry, uint64_t next_cookie)
{
	struct model *m = opaque;
	static const char *const names[] = { ".", "..", "first", "second" };
	struct ext4_inode inode;
	struct ext4_fscrypt_nokey decoded;
	uint8_t expected[16] = { 0 };
	uint8_t byte;
	size_t completed;
	uint32_t number;

	(void)next_cookie;
	CHECK(m->entries < 4);
	if (m->nokey && m->entries >= 2U) {
		CHECK(ext4_fscrypt_nokey_decode(entry->name, entry->name_length, &decoded));
		memcpy(expected, names[m->entries], strlen(names[m->entries]));
		CHECK(ext4_fscrypt_nokey_match(&decoded, expected, sizeof(expected)));
	} else {
		CHECK(entry->name_length == strlen(names[m->entries]));
		CHECK(memcmp(entry->name, names[m->entries], entry->name_length) == 0);
	}
	m->entries++;
	if (m->entries == m->evict_at) {
		for (number = 3; number <= 18; number++) {
			CHECK(ext4_get_inode(&m->fs, number, &inode) == EXT4_OK);
			m->expected_owner = number;
			CHECK(ext4_read(&m->fs, &inode, 0, &byte, 1, &completed) == EXT4_OK);
			CHECK(completed == 1);
		}
		m->evicted = true;
		m->expected_owner = 2;
	}
	return EXT4_DIR_ACCEPT;
}

static void
model_run(uint32_t bs, uint32_t evict_at, bool fail, bool cross_block, bool nokey)
{
	struct model *m = calloc(1, sizeof(*m));
	uint64_t cookie = 0;
	enum ext4_result error;

	CHECK(m != NULL);
	model_init(m, bs, cross_block, nokey);
	m->evict_at = evict_at;
	m->fail_derive = fail ? 2U : 0U;
	error = ext4_iterate_dir(&m->fs, &m->directory, &cookie, model_visit, m);
	CHECK(m->stale_cipher == 0);
	CHECK(error == (fail ? EXT4_IO : EXT4_NOT_FOUND));
	CHECK(m->entries == (fail ? (evict_at == 1U ? 2U : 3U) : 4U));
	CHECK(cookie == (fail ? (evict_at == 1U ? 24U : (cross_block ? bs : 48U))
			      : (cross_block ? 2U * bs : bs)));
	CHECK(m->evicted == (evict_at != 0));
	CHECK(m->directory_derives == (nokey ? 0U : (evict_at == 0 ? 1U : 2U)));
	if (fail) {
		m->fail_derive = 0;
		CHECK(ext4_iterate_dir(&m->fs, &m->directory, &cookie, model_visit, m) ==
		    EXT4_NOT_FOUND);
		CHECK(m->entries == 4U && cookie == (cross_block ? 2U * bs : bs));
		CHECK(m->directory_derives == 3U && m->stale_cipher == 0);
	}
	CHECK(ext4_set_crypto(&m->fs, NULL) == EXT4_OK);
	CHECK(m->key_live == 0 && m->released == m->allocated && m->live == 0);
	free(m->device);
	free(m);
}

int
main(void)
{
	static const uint32_t sizes[] = { 1024, 4096, 65536 };
	size_t i;

	for (i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
		model_run(sizes[i], 0, false, false, false);
		model_run(sizes[i], 0, false, true, false);
		model_run(sizes[i], 1, false, false, false);
		model_run(sizes[i], 1, false, true, false);
		model_run(sizes[i], 3, false, false, false);
		model_run(sizes[i], 3, false, true, false);
		model_run(sizes[i], 1, true, false, false);
		model_run(sizes[i], 1, true, true, false);
		model_run(sizes[i], 3, true, false, false);
		model_run(sizes[i], 3, true, true, false);
		model_run(sizes[i], 0, false, false, true);
		model_run(sizes[i], 0, false, true, true);
	}
	puts("fscrypt visitor lifetime: 36 cases passed");
	return 0;
}

/* SPDX-License-Identifier: BSD-3-Clause */
#include "crypto.h"
#include "storage.h"

#include <inttypes.h>

/* fscrypt without keys. The core must deny every operation that needs plaintext
 * names, contents or symlink targets, preserve encrypted objects, and still let
 * the unencrypted part of the volume and encrypted objects' own metadata change.
 * --synthetic builds the tree through the core and then marks it encrypted in
 * memory; otherwise the image comes from the Linux fscrypt probe. --key then also
 * supplies the probe's master key through a test adapter built on the reference
 * cryptography: every encrypted name, file and symlink target must read back as the
 * probe wrote it, and a different key or none must leave them unreadable. --write
 * IMAGE instead encrypts a directory of a base image through the core with that key
 * and writes, reshapes, links and renames objects in it, comparing every step with a
 * model; the export directory then receives the image and a manifest for Linux. A
 * power cut at every write or barrier of an encrypted overwrite and of a truncation
 * must recover the file's old or new contents. */

#define ENCRYPT_SECONDS 1700006000
#define PLAIN_FILE 24U
#define LARGE_FILE_BYTES (3U * 65536U + 777U)
#define SYNTHETIC_FILES 6U
#define PERMISSIONS 0640U
#define CHANGED_PERMISSIONS 0600U

static const struct ext4_timestamp encrypt_time = { ENCRYPT_SECONDS, 0 };

/* The Linux probe's tree: odd files in secret, even ones in secret/inner, every fifth
 * with a long name, a symlink and an empty directory. */
#define PROBE_FILES 24U
#define PROBE_LONG_NAME_BYTES 180U
#define PROBE_LINK_TARGET "inner/file-01-target-x"
#define PROBE_KEY_BYTES 64U
#define PROBE_KEY_MULTIPLIER 7U
#define PROBE_KEY_OFFSET 3U
#define NAME_BYTES 256U
#define FSCRYPT_HKDF_PREFIX "fscrypt"
#define FSCRYPT_HKDF_PREFIX_SIZE 8U
#define FSCRYPT_KEY_IDENTIFIER_CONTEXT 1U
#define FSCRYPT_IDENTIFIER_BYTES 16U
#define FSCRYPT_V2 2U
#define FSCRYPT_V1_KEY_BYTES 16U
#define FSCRYPT_PAD_16 0x02U
#define FSCRYPT_PAD_32 0x03U
#define FSCRYPT_MODE_ADIANTUM 9U
/* Objects the keyed write test creates, and bytes a file of it may hold. */
#define MODEL_OBJECTS 24U
#define MODEL_FILE_BYTES (96U * 1024U)
#define MODEL_DIRECTORIES 3U
#define MODEL_VAULT 0U
#define MODEL_SUB 1U
#define MODEL_PLAIN 2U
#define LONG_TARGET_BYTES 300U
#define PLAINTEXT_PROBE_BYTES 64U

/* A test adapter's keyring: one master key, found by its fscrypt identifier, and a
 * counter that makes every nonce distinct. */
struct keyring {
	uint8_t master[PROBE_KEY_BYTES];
	uint8_t identifier[FSCRYPT_IDENTIFIER_BYTES];
	uint32_t handles;
	uint32_t derivations;
	uint64_t nonces;
};

struct key {
	uint8_t bytes[PROBE_KEY_BYTES];
	size_t size;
};

static void
keyring_init(struct keyring *keyring, uint8_t offset)
{
	uint8_t info[FSCRYPT_HKDF_PREFIX_SIZE + 1U];
	unsigned int index;

	memset(keyring, 0, sizeof(*keyring));
	for (index = 0; index < PROBE_KEY_BYTES; index++) {
		keyring->master[index] = (uint8_t)(index * PROBE_KEY_MULTIPLIER + offset);
	}
	memcpy(info, FSCRYPT_HKDF_PREFIX, FSCRYPT_HKDF_PREFIX_SIZE);
	info[FSCRYPT_HKDF_PREFIX_SIZE] = FSCRYPT_KEY_IDENTIFIER_CONTEXT;
	test_hkdf_sha512(keyring->master, sizeof(keyring->master), info, sizeof(info),
	    keyring->identifier, sizeof(keyring->identifier));
}

static struct key *
key_new(struct keyring *keyring)
{
	struct key *key = calloc(1, sizeof(*key));

	CHECK(key != NULL);
	keyring->handles++;
	return key;
}

static enum ext4_result
find_key(void *context, uint8_t version, const uint8_t *identifier, size_t size, void **master)
{
	struct keyring *keyring = context;
	struct key *key;

	if (version != FSCRYPT_V2 || size != sizeof(keyring->identifier) ||
	    memcmp(identifier, keyring->identifier, size) != 0) {
		return EXT4_NOT_FOUND;
	}
	key = key_new(keyring);
	memcpy(key->bytes, keyring->master, sizeof(keyring->master));
	key->size = sizeof(keyring->master);
	*master = key;
	return EXT4_OK;
}

static enum ext4_result
derive_key(void *context, void *master, uint8_t version, const uint8_t *info, size_t info_size,
    size_t key_size, void **result)
{
	struct keyring *keyring = context;
	struct key *source = master;
	struct key *key;
	struct test_aes aes;
	size_t offset;

	CHECK(key_size <= sizeof(key->bytes) && key_size <= source->size);
	key = key_new(keyring);
	key->size = key_size;
	if (version == FSCRYPT_V2) {
		test_hkdf_sha512(
		    source->bytes, source->size, info, info_size, key->bytes, key_size);
	} else {
		/* Version 1: the master key encrypted with AES-128-ECB under the nonce. */
		CHECK(info_size == FSCRYPT_V1_KEY_BYTES);
		test_aes_init(&aes, info, FSCRYPT_V1_KEY_BYTES);
		for (offset = 0; offset < key_size; offset += TEST_AES_BLOCK) {
			test_aes_encrypt(&aes, source->bytes + offset, key->bytes + offset);
		}
	}
	keyring->derivations++;
	*result = key;
	return EXT4_OK;
}

static enum ext4_result
cipher(void *context, void *handle, uint8_t mode, bool encrypt, const uint8_t *iv,
    const void *input, void *output, size_t length)
{
	struct key *key = handle;

	(void)context;
	CHECK(input != output);
	if (mode == EXT4_FSCRYPT_MODE_AES_256_XTS) {
		test_aes_xts(key->bytes, key->size, iv, encrypt, input, output, length);
	} else if (mode == EXT4_FSCRYPT_MODE_AES_256_CTS) {
		test_aes_cts(key->bytes, key->size, iv, encrypt, input, output, length);
	} else {
		return EXT4_UNSUPPORTED;
	}
	return EXT4_OK;
}

static void
release_key(void *context, void *handle)
{
	struct keyring *keyring = context;

	CHECK(keyring->handles != 0);
	keyring->handles--;
	free(handle);
}

static enum ext4_result
random_bytes(void *context, void *buffer, size_t length)
{
	struct keyring *keyring = context;
	uint8_t counter[sizeof(keyring->nonces)];
	uint8_t digest[EXT4_SHA256_DIGEST_SIZE];
	struct ext4_sha256 hash;
	unsigned int index;

	CHECK(length <= sizeof(digest));
	keyring->nonces++;
	for (index = 0; index < sizeof(counter); index++) {
		counter[index] = (uint8_t)(keyring->nonces >> (8U * index));
	}
	ext4_sha256_init(&hash);
	ext4_sha256_update(&hash, counter, sizeof(counter));
	ext4_sha256_final(&hash, digest);
	memcpy(buffer, digest, length);
	return EXT4_OK;
}

static struct ext4_crypto_environment
keyring_environment(struct keyring *keyring)
{
	struct ext4_crypto_environment crypto = { keyring, NULL, false, find_key, derive_key,
		cipher, release_key, random_bytes };

	return crypto;
}

/* Matches the Linux probe so both fixture kinds share the plain file check. */
static uint8_t
pattern(unsigned int file, size_t index)
{
	return (uint8_t)(file * 131U + index * 17U + (index >> 9));
}

static size_t
file_size(unsigned int file)
{
	return file == 0 ? LARGE_FILE_BYTES : 1U + file * 97U;
}

static struct ext4_inode_update
creation(void)
{
	struct ext4_inode_update update = { 0 };

	update.fields = EXT4_ATTR_PERMISSIONS | EXT4_ATTR_UID | EXT4_ATTR_GID |
	    EXT4_ATTR_ACCESS_TIME | EXT4_ATTR_MODIFY_TIME | EXT4_ATTR_CHANGE_TIME |
	    EXT4_ATTR_XATTRS;
	update.permissions = PERMISSIONS;
	update.access_time = encrypt_time;
	update.modify_time = encrypt_time;
	update.change_time = encrypt_time;
	return update;
}

static struct ext4_inode_update
data_update(const struct ext4_inode *inode)
{
	struct ext4_inode_update update = { 0 };

	update.fields = EXT4_ATTR_PERMISSIONS | EXT4_ATTR_MODIFY_TIME | EXT4_ATTR_CHANGE_TIME |
	    EXT4_ATTR_XATTRS;
	update.permissions = (uint16_t)(inode->mode & 07777U);
	update.modify_time = encrypt_time;
	update.change_time = encrypt_time;
	return update;
}

static struct ext4_inode
find(struct ext4_fs *fs, uint32_t parent, const char *name)
{
	struct ext4_inode directory;
	struct ext4_inode inode;

	EXPECT(ext4_get_inode(fs, parent, &directory), EXT4_OK);
	EXPECT(ext4_lookup(fs, &directory, (const uint8_t *)name, strlen(name), &inode), EXT4_OK);
	return inode;
}

static void
write_pattern(struct ext4_fs *fs, const struct ext4_inode *inode, unsigned int file)
{
	struct ext4_inode_update update = data_update(inode);
	uint8_t *bytes = malloc(file_size(file));
	size_t completed;
	size_t index;

	CHECK(bytes != NULL);
	for (index = 0; index < file_size(file); index++) {
		bytes[index] = pattern(file, index);
	}
	EXPECT(ext4_write_partial(fs, inode->number, inode->generation, 0, bytes, file_size(file),
		   &update, &completed),
	    EXT4_OK);
	free(bytes);
}

static void
verify_pattern(struct ext4_fs *fs, const struct ext4_inode *inode, unsigned int file)
{
	uint8_t *bytes = malloc(file_size(file));
	size_t completed;
	size_t index;

	CHECK(bytes != NULL && inode->size == file_size(file));
	EXPECT(ext4_read(fs, inode, 0, bytes, file_size(file), &completed), EXT4_OK);
	CHECK(completed == file_size(file));
	for (index = 0; index < completed; index++) {
		CHECK(bytes[index] == pattern(file, index));
	}
	free(bytes);
}

static void
mark_encrypted(struct device *device, struct ext4_fs *fs, uint32_t number)
{
	struct ext4_inode_disk *disk;
	uint64_t offset;

	EXPECT(ext4_inode_location(fs, number, &offset), EXT4_OK);
	disk = (struct ext4_inode_disk *)(device->cache + offset);
	ext4_encode32(&disk->flags, ext4_le32(&disk->flags) | EXT4_INODE_ENCRYPT);
	ext4_inode_checksum_set(fs, number, disk);
}

/* Build an unencrypted tree, then mark part of it encrypted as Linux would. */
static void
synthesize(struct device *device)
{
	struct ext4_inode_update update = creation();
	struct ext4_super_disk *super;
	struct ext4_inode secret;
	struct ext4_inode inner;
	struct ext4_inode empty;
	struct ext4_inode plain;
	struct ext4_inode locked;
	struct ext4_inode inode;
	struct ext4_inode files[SYNTHETIC_FILES];
	struct ext4_inode link;
	struct ext4_fs *fs;
	char name[32];
	unsigned int index;

	EXPECT(ext4_mount_writable(&device->environment, &device->writer, &fs), EXT4_OK);
	EXPECT(ext4_mkdir(fs, EXT4_ROOT_INODE, 0, (const uint8_t *)"secret", 6, &update,
		   &encrypt_time, &secret),
	    EXT4_OK);
	EXPECT(ext4_mkdir(fs, secret.number, secret.generation, (const uint8_t *)"inner", 5,
		   &update, &encrypt_time, &inner),
	    EXT4_OK);
	EXPECT(ext4_mkdir(fs, secret.number, secret.generation, (const uint8_t *)"empty", 5,
		   &update, &encrypt_time, &empty),
	    EXT4_OK);
	for (index = 0; index < SYNTHETIC_FILES; index++) {
		snprintf(name, sizeof(name), "file-%02u", index);
		EXPECT(ext4_create(fs, index % 2U ? secret.number : inner.number,
			   index % 2U ? secret.generation : inner.generation, (const uint8_t *)name,
			   strlen(name), &update, &encrypt_time, &files[index]),
		    EXT4_OK);
		write_pattern(fs, &files[index], index);
	}
	EXPECT(ext4_symlink(fs, secret.number, secret.generation, (const uint8_t *)"link", 4,
		   (const uint8_t *)"inner/file-01-target-x", 22, &update, &encrypt_time, &link),
	    EXT4_OK);
	EXPECT(ext4_mkdir(fs, EXT4_ROOT_INODE, 0, (const uint8_t *)"plain", 5, &update,
		   &encrypt_time, &plain),
	    EXT4_OK);
	EXPECT(ext4_create(fs, plain.number, plain.generation, (const uint8_t *)"visible", 7,
		   &update, &encrypt_time, &inode),
	    EXT4_OK);
	write_pattern(fs, &inode, PLAIN_FILE);
	EXPECT(ext4_mkdir(fs, EXT4_ROOT_INODE, 0, (const uint8_t *)"locked-empty", 12, &update,
		   &encrypt_time, &locked),
	    EXT4_OK);
	EXPECT(ext4_sync(fs), EXT4_OK);
	ext4_unmount(fs);
	EXPECT(ext4_mount(&device->environment, &fs), EXT4_OK);
	mark_encrypted(device, fs, secret.number);
	mark_encrypted(device, fs, inner.number);
	mark_encrypted(device, fs, empty.number);
	mark_encrypted(device, fs, link.number);
	mark_encrypted(device, fs, locked.number);
	for (index = 0; index < SYNTHETIC_FILES; index++) {
		mark_encrypted(device, fs, files[index].number);
	}
	super = (struct ext4_super_disk *)(device->cache + EXT4_SUPER_OFFSET);
	ext4_encode32(&super->feature_incompat,
	    ext4_le32(&super->feature_incompat) | EXT4_FEATURE_INCOMPAT_ENCRYPT);
	if (fs->metadata_checksum) {
		ext4_encode32(&super->checksum,
		    ext4_crc32c(UINT32_MAX, super, offsetof(struct ext4_super_disk, checksum)));
	}
	ext4_unmount(fs);
	memcpy(device->base, device->cache, device->size);
	device_reset(device, device->base);
	puts("PASS synthesized an encrypted tree on an ENCRYPT volume");
}

/* Encrypted regular files and symlinks, found without reading encrypted names. */
static void
encrypted_objects(struct ext4_fs *fs, struct ext4_inode *file, struct ext4_inode *link)
{
	struct ext4_inode inode;
	uint32_t number;
	uint32_t files = 0;
	uint32_t links = 0;

	for (number = fs->first_inode; number <= fs->info.inodes; number++) {
		if (ext4_get_inode(fs, number, &inode) != EXT4_OK ||
		    !(inode.flags & EXT4_INODE_ENCRYPT)) {
			continue;
		}
		if ((inode.mode & EXT4_MODE_TYPE) == EXT4_MODE_REGULAR && inode.size != 0) {
			*file = inode;
			files++;
		} else if ((inode.mode & EXT4_MODE_TYPE) == EXT4_MODE_SYMLINK) {
			*link = inode;
			links++;
		}
	}
	CHECK(files != 0 && links != 0);
}

static enum ext4_dir_action
visit(void *context, const struct ext4_dir_entry *entry, uint64_t next_cookie)
{
	(void)context;
	(void)entry;
	(void)next_cookie;
	CHECK(false);
	return EXT4_DIR_STOP;
}

static void
probe_name(char *name, unsigned int file)
{
	int length = file % 5U == 0
	    ? snprintf(name, NAME_BYTES, "long-%02u-%0*u", file, (int)PROBE_LONG_NAME_BYTES, file)
	    : snprintf(name, NAME_BYTES, "file-%02u", file);

	CHECK(length > 0 && length < (int)NAME_BYTES);
}

struct listing {
	char names[PROBE_FILES + 8U][NAME_BYTES];
	uint32_t count;
};

static enum ext4_dir_action
collect(void *context, const struct ext4_dir_entry *entry, uint64_t next_cookie)
{
	struct listing *listing = context;

	(void)next_cookie;
	CHECK(listing->count < sizeof(listing->names) / sizeof(listing->names[0]));
	CHECK(entry->name_length < NAME_BYTES && entry->name[entry->name_length] == 0);
	memcpy(listing->names[listing->count++], entry->name, (size_t)entry->name_length + 1U);
	return EXT4_DIR_ACCEPT;
}

static bool
listed(const struct listing *listing, const char *name)
{
	uint32_t index;

	for (index = 0; index < listing->count; index++) {
		if (strcmp(listing->names[index], name) == 0) {
			return true;
		}
	}
	return false;
}

/* With the probe's key every encrypted name, file and target reads back. */
static void
keyed(struct device *device)
{
	static struct listing listing;
	struct keyring keyring;
	struct keyring other;
	struct ext4_crypto_environment crypto;
	struct ext4_fs *fs;
	struct ext4_inode secret;
	struct ext4_inode inner;
	struct ext4_inode directory;
	struct ext4_inode inode;
	struct ext4_mapping mapping;
	struct ext4_dir_entry entry;
	char name[NAME_BYTES];
	char target[NAME_BYTES];
	uint64_t cookie = 0;
	uint32_t derivations;
	unsigned int file;
	size_t completed;

	keyring_init(&keyring, PROBE_KEY_OFFSET);
	keyring_init(&other, PROBE_KEY_OFFSET + 1U);
	EXPECT(ext4_mount(&device->environment, &fs), EXT4_OK);
	secret = find(fs, EXT4_ROOT_INODE, "secret");
	/* Another master key is not the one the policy names. */
	crypto = keyring_environment(&other);
	EXPECT(ext4_set_crypto(fs, &crypto), EXT4_OK);
	EXPECT(ext4_iterate_dir(fs, &secret, &cookie, collect, &listing), EXT4_ENCRYPTED);
	EXPECT(ext4_lookup(fs, &secret, (const uint8_t *)"inner", 5, &inode), EXT4_ENCRYPTED);
	crypto = keyring_environment(&keyring);
	EXPECT(ext4_set_crypto(fs, &crypto), EXT4_OK);
	EXPECT(ext4_iterate_dir(fs, &secret, &cookie, collect, &listing), EXT4_NOT_FOUND);
	CHECK(listed(&listing, ".") && listed(&listing, "..") && listed(&listing, "inner") &&
	    listed(&listing, "empty") && listed(&listing, "link") &&
	    listing.count == 5U + PROBE_FILES / 2U);
	inner = find(fs, secret.number, "inner");
	listing.count = 0;
	cookie = 0;
	EXPECT(ext4_iterate_dir(fs, &inner, &cookie, collect, &listing), EXT4_NOT_FOUND);
	CHECK(listing.count == 2U + PROBE_FILES / 2U);
	for (file = 0; file < PROBE_FILES; file++) {
		directory = file % 2U ? secret : inner;
		probe_name(name, file);
		CHECK(listed(&listing, name) == !(file % 2U));
		inode = find(fs, directory.number, name);
		CHECK((inode.flags & EXT4_INODE_ENCRYPT) && inode.size == file_size(file));
		verify_pattern(fs, &inode, file);
		EXPECT(ext4_map_read(fs, &inode, 0, 1, &mapping), EXT4_ENCRYPTED);
	}
	inode = find(fs, secret.number, "link");
	EXPECT(ext4_read(fs, &inode, 0, target, sizeof(target), &completed), EXT4_OK);
	CHECK(completed == strlen(PROBE_LINK_TARGET) &&
	    memcmp(target, PROBE_LINK_TARGET, completed) == 0);
	directory = find(fs, secret.number, "empty");
	cookie = 0;
	EXPECT(ext4_next_dir(fs, &directory, &cookie, &entry), EXT4_OK);
	CHECK(strcmp((const char *)entry.name, ".") == 0);
	EXPECT(ext4_next_dir(fs, &directory, &cookie, &entry), EXT4_OK);
	CHECK(strcmp((const char *)entry.name, "..") == 0);
	EXPECT(ext4_next_dir(fs, &directory, &cookie, &entry), EXT4_NOT_FOUND);
	/* A derived key is reused while it stays among the mount's cached keys. */
	inode = find(fs, secret.number, "file-01");
	verify_pattern(fs, &inode, 1);
	derivations = keyring.derivations;
	verify_pattern(fs, &inode, 1);
	CHECK(keyring.derivations == derivations);
	EXPECT(ext4_set_crypto(fs, NULL), EXT4_OK);
	CHECK(keyring.handles == 0);
	EXPECT(ext4_read(fs, &inode, 0, target, 1, &completed), EXT4_ENCRYPTED);
	crypto = keyring_environment(&keyring);
	EXPECT(ext4_set_crypto(fs, &crypto), EXT4_OK);
	verify_pattern(fs, &inode, 1);
	ext4_unmount(fs);
	CHECK(keyring.handles == 0 && other.handles == 0 && device->writes == 0);
	printf("PASS keyed access reads %u encrypted files, their names and the symlink target\n",
	    PROBE_FILES);
}

enum model_kind { MODEL_FILE, MODEL_SYMLINK, MODEL_DIRECTORY, MODEL_FIFO };

struct model_object {
	enum model_kind kind;
	uint32_t directory;
	char name[NAME_BYTES];
	uint8_t *data;
	size_t size;
	bool present;
};

/* What the keyed write test expects each name to hold. */
struct model {
	const char *paths[MODEL_DIRECTORIES];
	struct ext4_inode directories[MODEL_DIRECTORIES];
	struct model_object objects[MODEL_OBJECTS];
	uint32_t count;
};

static struct model_object *
model_add(struct model *model, enum model_kind kind, uint32_t directory, const char *name)
{
	struct model_object *object;

	CHECK(model->count < MODEL_OBJECTS && strlen(name) < NAME_BYTES);
	object = &model->objects[model->count++];
	memset(object, 0, sizeof(*object));
	object->kind = kind;
	object->directory = directory;
	snprintf(object->name, sizeof(object->name), "%s", name);
	object->data = calloc(1, MODEL_FILE_BYTES);
	CHECK(object->data != NULL);
	object->present = true;
	return object;
}

static struct model_object *
model_find(struct model *model, uint32_t directory, const char *name)
{
	uint32_t index;

	for (index = 0; index < model->count; index++) {
		if (model->objects[index].present && model->objects[index].directory == directory &&
		    strcmp(model->objects[index].name, name) == 0) {
			return &model->objects[index];
		}
	}
	CHECK(false);
	return NULL;
}

/* Directories change size and indexing as names arrive; read them fresh. */
static struct ext4_inode
model_directory(struct ext4_fs *fs, struct model *model, uint32_t directory)
{
	EXPECT(ext4_get_inode(
		   fs, model->directories[directory].number, &model->directories[directory]),
	    EXT4_OK);
	return model->directories[directory];
}

static struct ext4_inode
model_inode(struct ext4_fs *fs, struct model *model, const struct model_object *object)
{
	struct ext4_inode directory = model_directory(fs, model, object->directory);
	struct ext4_inode inode;

	EXPECT(ext4_lookup(
		   fs, &directory, (const uint8_t *)object->name, strlen(object->name), &inode),
	    EXT4_OK);
	return inode;
}

/* Model contents do not repeat across files or offsets, unlike pattern(), so a
 * plaintext run found on the device identifies its file: SplitMix64 of the seed and
 * the eight-byte word, one byte at a time. */
static uint8_t
model_byte(unsigned int seed, size_t offset)
{
	uint64_t value = ((uint64_t)seed << 40) ^ (uint64_t)(offset / 8U);

	value += UINT64_C(0x9e3779b97f4a7c15);
	value = (value ^ (value >> 30)) * UINT64_C(0xbf58476d1ce4e5b9);
	value = (value ^ (value >> 27)) * UINT64_C(0x94d049bb133111eb);
	value ^= value >> 31;
	return (uint8_t)(value >> (8U * (offset % 8U)));
}

/* Write bytes of the model's contents through the core and into the model. */
static void
model_write(struct ext4_fs *fs, struct model *model, struct model_object *object, size_t offset,
    size_t length, unsigned int seed)
{
	struct ext4_inode inode = model_inode(fs, model, object);
	struct ext4_inode_update update = data_update(&inode);
	size_t completed;
	size_t done = 0;
	size_t index;

	CHECK(offset + length <= MODEL_FILE_BYTES);
	for (index = 0; index < length; index++) {
		object->data[offset + index] = model_byte(seed, offset + index);
	}
	while (done < length) {
		completed = 0;
		EXPECT(ext4_write_partial(fs, inode.number, inode.generation, offset + done,
			   object->data + offset + done, length - done, &update, &completed),
		    EXT4_OK);
		CHECK(completed != 0);
		done += completed;
	}
	if (offset + length > object->size) {
		object->size = offset + length;
	}
}

static void
model_truncate(struct ext4_fs *fs, struct model *model, struct model_object *object, size_t size)
{
	struct ext4_inode inode = model_inode(fs, model, object);
	struct ext4_inode_update update = data_update(&inode);

	CHECK(size <= MODEL_FILE_BYTES);
	EXPECT(ext4_truncate(fs, inode.number, inode.generation, size, &update, &inode), EXT4_OK);
	if (size > object->size) {
		memset(object->data + object->size, 0, size - object->size);
	} else {
		memset(object->data + size, 0, MODEL_FILE_BYTES - size);
	}
	object->size = size;
}

static void
model_create(struct ext4_fs *fs, struct model *model, enum model_kind kind, uint32_t directory,
    const char *name, const char *target)
{
	struct ext4_inode_update update = creation();
	struct ext4_special_file fifo = { EXT4_FT_FIFO, 0, 0 };
	struct ext4_inode parent = model_directory(fs, model, directory);
	struct ext4_inode inode;
	struct model_object *object = model_add(model, kind, directory, name);

	if (kind == MODEL_FILE) {
		EXPECT(ext4_create(fs, parent.number, parent.generation, (const uint8_t *)name,
			   strlen(name), &update, &encrypt_time, &inode),
		    EXT4_OK);
	} else if (kind == MODEL_DIRECTORY) {
		EXPECT(ext4_mkdir(fs, parent.number, parent.generation, (const uint8_t *)name,
			   strlen(name), &update, &encrypt_time, &inode),
		    EXT4_OK);
	} else if (kind == MODEL_FIFO) {
		EXPECT(ext4_mknod(fs, parent.number, parent.generation, (const uint8_t *)name,
			   strlen(name), &fifo, &update, &encrypt_time, &inode),
		    EXT4_OK);
	} else {
		EXPECT(ext4_symlink(fs, parent.number, parent.generation, (const uint8_t *)name,
			   strlen(name), (const uint8_t *)target, strlen(target), &update,
			   &encrypt_time, &inode),
		    EXT4_OK);
		memcpy(object->data, target, strlen(target));
		object->size = strlen(target);
	}
	/* Objects in an encrypted directory are encrypted, except special files. */
	CHECK(((inode.flags & EXT4_INODE_ENCRYPT) != 0) ==
	    ((parent.flags & EXT4_INODE_ENCRYPT) != 0 && kind != MODEL_FIFO));
	CHECK(!(inode.flags & EXT4_INODE_ENCRYPT) || !(inode.flags & EXT4_INODE_INLINE_DATA));
}

static void
model_list(struct ext4_fs *fs, struct model *model, uint32_t directory)
{
	static struct listing listing;
	struct ext4_inode inode = model_directory(fs, model, directory);
	uint64_t cookie = 0;
	uint32_t expected = 2;
	uint32_t index;

	listing.count = 0;
	EXPECT(ext4_iterate_dir(fs, &inode, &cookie, collect, &listing), EXT4_NOT_FOUND);
	CHECK(listed(&listing, ".") && listed(&listing, ".."));
	for (index = 0; index < model->count; index++) {
		if (model->objects[index].present && model->objects[index].directory == directory) {
			CHECK(listed(&listing, model->objects[index].name));
			expected++;
		}
	}
	/* The vault also names the subdirectory, which the model keeps separately. */
	CHECK(listing.count == expected + (directory == MODEL_VAULT ? 1U : 0U));
}

static void
model_verify(struct ext4_fs *fs, struct model *model)
{
	struct model_object *object;
	struct ext4_inode inode;
	uint8_t *bytes = malloc(MODEL_FILE_BYTES);
	uint32_t index;
	size_t completed;

	CHECK(bytes != NULL);
	for (index = 0; index < MODEL_DIRECTORIES; index++) {
		model_list(fs, model, index);
	}
	for (index = 0; index < model->count; index++) {
		object = &model->objects[index];
		if (!object->present || object->kind == MODEL_DIRECTORY ||
		    object->kind == MODEL_FIFO) {
			continue;
		}
		inode = model_inode(fs, model, object);
		completed = 0;
		EXPECT(ext4_read(fs, &inode, 0, bytes, MODEL_FILE_BYTES, &completed), EXT4_OK);
		CHECK(completed == object->size && memcmp(bytes, object->data, completed) == 0);
		CHECK(object->kind != MODEL_FILE || inode.size == object->size);
	}
	free(bytes);
}

static bool
device_contains(const struct device *device, const uint8_t *needle, size_t length)
{
	size_t offset;

	for (offset = 0; offset + length <= device->size; offset++) {
		if (device->cache[offset] == needle[0] &&
		    memcmp(device->cache + offset, needle, length) == 0) {
			return true;
		}
	}
	return false;
}

static const char *
model_kind_name(enum model_kind kind)
{
	switch (kind) {
	case MODEL_FILE:
		return "file";
	case MODEL_SYMLINK:
		return "symlink";
	case MODEL_FIFO:
		return "fifo";
	default:
		return "directory";
	}
}

static void
model_export(const struct model *model, const char *directory, const char *source)
{
	const struct model_object *object;
	struct ext4_sha256 hash;
	uint8_t digest[EXT4_SHA256_DIGEST_SIZE];
	const char *name = strrchr(source, '/');
	const char *extension;
	char path[4096];
	FILE *manifest;
	uint32_t index;
	unsigned int byte;
	int length;

	name = name == NULL ? source : name + 1;
	extension = strrchr(name, '.');
	length = snprintf(path, sizeof(path), "%s/encrypted-%.*s.manifest", directory,
	    (int)(extension == NULL ? strlen(name) : (size_t)(extension - name)), name);
	CHECK(length > 0 && (size_t)length < sizeof(path));
	manifest = fopen(path, "wx");
	CHECK(manifest != NULL);
	for (index = 0; index < model->count; index++) {
		object = &model->objects[index];
		if (!object->present) {
			continue;
		}
		fprintf(manifest, "%s %s/%s", model_kind_name(object->kind),
		    model->paths[object->directory], object->name);
		if (object->kind == MODEL_FILE) {
			ext4_sha256_init(&hash);
			ext4_sha256_update(&hash, object->data, object->size);
			ext4_sha256_final(&hash, digest);
			fprintf(manifest, " %zu ", object->size);
			for (byte = 0; byte < sizeof(digest); byte++) {
				fprintf(manifest, "%02x", digest[byte]);
			}
		} else if (object->kind == MODEL_SYMLINK) {
			fprintf(manifest, " %.*s", (int)object->size, (const char *)object->data);
		}
		fprintf(manifest, "\n");
	}
	CHECK(fclose(manifest) == 0);
}

/* Set the ENCRYPT feature on a base image, as tune2fs -O encrypt does. */
static void
enable_encryption(struct device *device)
{
	struct ext4_super_disk *super =
	    (struct ext4_super_disk *)(device->base + EXT4_SUPER_OFFSET);

	if (ext4_le32(&super->feature_incompat) & EXT4_FEATURE_INCOMPAT_ENCRYPT) {
		return;
	}
	ext4_encode32(&super->feature_incompat,
	    ext4_le32(&super->feature_incompat) | EXT4_FEATURE_INCOMPAT_ENCRYPT);
	if (device->metadata_checksum) {
		ext4_encode32(&super->checksum,
		    ext4_crc32c(UINT32_MAX, super, offsetof(struct ext4_super_disk, checksum)));
	}
	device_reset(device, device->base);
}

static struct ext4_rename_entry
entry_of(const struct ext4_inode *directory, const char *name, const struct ext4_inode *inode)
{
	struct ext4_rename_entry entry = { directory->number, directory->generation,
		(const uint8_t *)name, strlen(name), inode == NULL ? 0 : inode->number,
		inode == NULL ? 0 : inode->generation };

	return entry;
}

/* Policies: invalid, unsupported, unknown keys and unsuitable directories refuse. */
static void
policies(struct ext4_fs *fs, struct model *model, const struct keyring *keyring,
    const struct keyring *other)
{
	struct ext4_encryption_policy policy;
	struct ext4_encryption_policy read_back;
	struct ext4_inode inode = model->directories[MODEL_VAULT];
	struct ext4_inode result;

	memset(&policy, 0, sizeof(policy));
	policy.version = FSCRYPT_V2;
	policy.contents_mode = EXT4_FSCRYPT_MODE_AES_256_XTS;
	policy.filenames_mode = EXT4_FSCRYPT_MODE_AES_256_CTS;
	policy.flags = FSCRYPT_PAD_32;
	memcpy(policy.identifier, other->identifier, sizeof(other->identifier));
	EXPECT(ext4_set_encryption_policy(fs, inode.number, inode.generation, &policy, &result),
	    EXT4_ENCRYPTED);
	memcpy(policy.identifier, keyring->identifier, sizeof(keyring->identifier));
	policy.version = FSCRYPT_V2 + 1U;
	EXPECT(ext4_set_encryption_policy(fs, inode.number, inode.generation, &policy, &result),
	    EXT4_INVALID_ARGUMENT);
	policy.version = FSCRYPT_V2;
	policy.contents_mode = FSCRYPT_MODE_ADIANTUM;
	EXPECT(ext4_set_encryption_policy(fs, inode.number, inode.generation, &policy, &result),
	    EXT4_UNSUPPORTED);
	policy.contents_mode = EXT4_FSCRYPT_MODE_AES_256_XTS;
	inode = model->directories[MODEL_PLAIN];
	EXPECT(ext4_set_encryption_policy(fs, inode.number, inode.generation, &policy, &result),
	    EXT4_NOT_EMPTY);
	inode = model_inode(fs, model, model_find(model, MODEL_PLAIN, "visible"));
	EXPECT(ext4_set_encryption_policy(fs, inode.number, inode.generation, &policy, &result),
	    EXT4_NOT_DIRECTORY);
	inode = model->directories[MODEL_VAULT];
	EXPECT(ext4_set_encryption_policy(fs, inode.number, inode.generation, &policy, &result),
	    EXT4_OK);
	CHECK(result.flags & EXT4_INODE_ENCRYPT);
	model->directories[MODEL_VAULT] = result;
	EXPECT(ext4_set_encryption_policy(fs, inode.number, inode.generation, &policy, &result),
	    EXT4_OK);
	EXPECT(ext4_get_encryption_policy(fs, &result, &read_back), EXT4_OK);
	CHECK(memcmp(&read_back, &policy, sizeof(policy)) == 0);
	policy.flags = FSCRYPT_PAD_16;
	EXPECT(ext4_set_encryption_policy(fs, inode.number, inode.generation, &policy, &result),
	    EXT4_EXISTS);
	EXPECT(ext4_get_encryption_policy(fs, &model->directories[MODEL_PLAIN], &read_back),
	    EXT4_NOT_FOUND);
}

/* Names move within the policy; other objects cannot enter it. */
static void
moves(struct ext4_fs *fs, struct model *model)
{
	struct ext4_inode *vault = &model->directories[MODEL_VAULT];
	struct ext4_inode *sub = &model->directories[MODEL_SUB];
	struct ext4_inode *plain = &model->directories[MODEL_PLAIN];
	struct ext4_rename_entry from;
	struct ext4_rename_entry to;
	struct ext4_inode inode;
	struct ext4_inode result;
	struct model_object *object;

	inode = model_inode(fs, model, model_find(model, MODEL_VAULT, "tiny"));
	from = entry_of(vault, "tiny", &inode);
	to = entry_of(sub, "tiny-moved", NULL);
	EXPECT(ext4_rename(fs, &from, &to, 0, &encrypt_time, &result), EXT4_OK);
	object = model_find(model, MODEL_VAULT, "tiny");
	object->directory = MODEL_SUB;
	snprintf(object->name, sizeof(object->name), "tiny-moved");
	inode = model_inode(fs, model, model_find(model, MODEL_VAULT, "block"));
	EXPECT(ext4_link(fs, vault->number, vault->generation, (const uint8_t *)"block-link", 10,
		   inode.number, inode.generation, &encrypt_time, &result),
	    EXT4_OK);
	object = model_add(model, MODEL_FILE, MODEL_VAULT, "block-link");
	memcpy(object->data, model_find(model, MODEL_VAULT, "block")->data, MODEL_FILE_BYTES);
	object->size = model_find(model, MODEL_VAULT, "block")->size;
	inode = model_inode(fs, model, model_find(model, MODEL_PLAIN, "visible"));
	from = entry_of(plain, "visible", &inode);
	to = entry_of(vault, "visible", NULL);
	EXPECT(ext4_rename(fs, &from, &to, 0, &encrypt_time, &result), EXT4_CROSS_POLICY);
	EXPECT(ext4_link(fs, vault->number, vault->generation, (const uint8_t *)"visible", 7,
		   inode.number, inode.generation, &encrypt_time, &result),
	    EXT4_CROSS_POLICY);
	/* An encrypted file may move into an unencrypted directory. */
	inode = model_inode(fs, model, model_find(model, MODEL_VAULT, "block-link"));
	from = entry_of(vault, "block-link", &inode);
	to = entry_of(plain, "block-out", NULL);
	EXPECT(ext4_rename(fs, &from, &to, 0, &encrypt_time, &result), EXT4_OK);
	object = model_find(model, MODEL_VAULT, "block-link");
	object->directory = MODEL_PLAIN;
	snprintf(object->name, sizeof(object->name), "block-out");
	inode = model_inode(fs, model, model_find(model, MODEL_VAULT, "fifo"));
	EXPECT(ext4_unlink(fs, vault->number, vault->generation, (const uint8_t *)"fifo", 4,
		   inode.number, inode.generation, &encrypt_time, &result),
	    EXT4_OK);
	model_find(model, MODEL_VAULT, "fifo")->present = false;
	inode = model_inode(fs, model, model_find(model, MODEL_VAULT, "empty"));
	EXPECT(ext4_unlink(fs, vault->number, vault->generation, (const uint8_t *)"empty", 5,
		   inode.number, inode.generation, &encrypt_time, &result),
	    EXT4_OK);
	model_find(model, MODEL_VAULT, "empty")->present = false;
}

/* Reshape encrypted contents: overwrite inside blocks, shrink into a block and grow
 * again, write into preallocation and punch a hole with partial edges. */
static void
reshape(struct ext4_fs *fs, struct model *model, uint32_t block)
{
	struct ext4_inode_update write;
	struct ext4_inode inode;
	struct model_object *object;
	uint64_t completed;

	object = model_find(model, MODEL_VAULT, "partial");
	model_write(fs, model, object, block + 50U, 100, 10);
	model_truncate(fs, model, object, 2U * block + 10U);
	model_truncate(fs, model, object, 2U * block + 500U);
	model_create(fs, model, MODEL_FILE, MODEL_VAULT, "prealloc", NULL);
	object = model_find(model, MODEL_VAULT, "prealloc");
	inode = model_inode(fs, model, object);
	write = data_update(&inode);
	EXPECT(ext4_fallocate(fs, inode.number, inode.generation, 0, 4U * block,
		   EXT4_FALLOC_KEEP_SIZE, &write, &completed),
	    EXT4_OK);
	model_write(fs, model, object, block + 5U, 10, 11);
	object = model_find(model, MODEL_VAULT, "large");
	inode = model_inode(fs, model, object);
	write = data_update(&inode);
	EXPECT(ext4_fallocate(fs, inode.number, inode.generation, block / 2U, 2U * block,
		   EXT4_FALLOC_KEEP_SIZE | EXT4_FALLOC_PUNCH_HOLE, &write, &completed),
	    EXT4_OK);
	memset(object->data + block / 2U, 0, 2U * block);
}

enum cut_operation { CUT_OVERWRITE, CUT_TRUNCATE };

/* Apply one operation to the encrypted file, or its effect to a copy of contents. */
static enum ext4_result
cut_apply(struct ext4_fs *fs, const struct ext4_inode *inode, enum cut_operation operation,
    uint32_t block, uint8_t *contents, size_t *size)
{
	struct ext4_inode_update update;
	struct ext4_inode result;
	size_t index;
	size_t completed;

	if (fs != NULL) {
		update = data_update(inode);
	}
	if (operation == CUT_OVERWRITE) {
		for (index = 0; index < 2U * block; index++) {
			contents[block / 2U + index] = model_byte(12, index);
		}
		return fs == NULL ? EXT4_OK
				  : ext4_write(fs, inode->number, inode->generation, block / 2U,
					contents + block / 2U, 2U * block, &update, &completed);
	}
	memset(contents + block + 10U, 0, *size - (block + 10U));
	*size = block + 10U;
	return fs == NULL
	    ? EXT4_OK
	    : ext4_truncate(fs, inode->number, inode->generation, *size, &update, &result);
}

static void
keyed_power_cuts(struct device *device, enum cut_operation operation)
{
	struct keyring keyring;
	struct ext4_crypto_environment crypto;
	struct ext4_encryption_policy policy;
	struct ext4_recovery_report report;
	struct ext4_inode_update update = creation();
	struct ext4_inode vault;
	struct ext4_inode inode;
	struct ext4_fs *fs;
	uint32_t block = device->block_size;
	uint32_t events;
	uint32_t cut;
	uint32_t original = 0;
	uint32_t changed = 0;
	uint8_t *before = malloc(device->size);
	uint8_t *old_contents = malloc(4U * block);
	uint8_t *new_contents = malloc(4U * block);
	uint8_t *read_back = malloc(4U * block);
	size_t old_size = 3U * block + 77U;
	size_t new_size = old_size;
	size_t index;
	size_t completed;
	enum ext4_result error;

	CHECK(before != NULL && old_contents != NULL && new_contents != NULL && read_back != NULL);
	for (index = 0; index < old_size; index++) {
		old_contents[index] = model_byte(13, index);
	}
	memcpy(new_contents, old_contents, old_size);
	CHECK(cut_apply(NULL, NULL, operation, block, new_contents, &new_size) == EXT4_OK);
	enable_encryption(device);
	device_reset(device, device->base);
	keyring_init(&keyring, PROBE_KEY_OFFSET);
	crypto = keyring_environment(&keyring);
	EXPECT(ext4_mount_writable(&device->environment, &device->writer, &fs), EXT4_OK);
	EXPECT(ext4_set_crypto(fs, &crypto), EXT4_OK);
	EXPECT(ext4_mkdir(fs, EXT4_ROOT_INODE, 0, (const uint8_t *)"cuts", 4, &update,
		   &encrypt_time, &vault),
	    EXT4_OK);
	memset(&policy, 0, sizeof(policy));
	policy.version = FSCRYPT_V2;
	policy.contents_mode = EXT4_FSCRYPT_MODE_AES_256_XTS;
	policy.filenames_mode = EXT4_FSCRYPT_MODE_AES_256_CTS;
	policy.flags = FSCRYPT_PAD_32;
	memcpy(policy.identifier, keyring.identifier, sizeof(keyring.identifier));
	EXPECT(ext4_set_encryption_policy(fs, vault.number, vault.generation, &policy, &vault),
	    EXT4_OK);
	EXPECT(ext4_create(fs, vault.number, vault.generation, (const uint8_t *)"file", 4, &update,
		   &encrypt_time, &inode),
	    EXT4_OK);
	update = data_update(&inode);
	EXPECT(ext4_write(fs, inode.number, inode.generation, 0, old_contents, old_size, &update,
		   &completed),
	    EXT4_OK);
	inode = find(fs, vault.number, "file");
	EXPECT(ext4_sync(fs), EXT4_OK);
	ext4_unmount(fs);
	memcpy(before, device->stable, device->size);
	EXPECT(ext4_mount_writable(&device->environment, &device->writer, &fs), EXT4_OK);
	EXPECT(ext4_set_crypto(fs, &crypto), EXT4_OK);
	events = device->events;
	memcpy(read_back, old_contents, old_size);
	index = old_size;
	EXPECT(cut_apply(fs, &inode, operation, block, read_back, &index), EXT4_OK);
	events = device->events - events;
	ext4_unmount(fs);
	for (cut = 1; cut <= events; cut++) {
		device_reset(device, before);
		EXPECT(ext4_mount_writable(&device->environment, &device->writer, &fs), EXT4_OK);
		EXPECT(ext4_set_crypto(fs, &crypto), EXT4_OK);
		device->stop_at = device->events + cut;
		device->survival = cut % 3U;
		device->partial = cut % 2U != 0;
		memcpy(read_back, old_contents, old_size);
		index = old_size;
		error = cut_apply(fs, &inode, operation, block, read_back, &index);
		CHECK(error != EXT4_OK && device->off);
		ext4_unmount(fs);
		device_reset(device, device->stable);
		error = ext4_recover(&device->environment, &device->writer, &report);
		if (error == EXT4_CORRUPT) {
			CHECK(device->metadata_checksum && device->writes == 0);
			continue;
		}
		EXPECT(error, EXT4_OK);
		EXPECT(ext4_mount(&device->environment, &fs), EXT4_OK);
		EXPECT(ext4_set_crypto(fs, &crypto), EXT4_OK);
		inode = find(fs, vault.number, "file");
		EXPECT(ext4_read(fs, &inode, 0, read_back, 4U * block, &completed), EXT4_OK);
		if (completed == old_size && memcmp(read_back, old_contents, old_size) == 0) {
			original++;
		} else {
			CHECK(completed == new_size &&
			    memcmp(read_back, new_contents, new_size) == 0);
			changed++;
		}
		ext4_unmount(fs);
		CHECK(keyring.handles == 0);
	}
	CHECK(original != 0 && changed != 0);
	free(read_back);
	free(new_contents);
	free(old_contents);
	free(before);
	printf("PASS encrypted %s power cuts: %u events, %u old, %u new\n",
	    operation == CUT_OVERWRITE ? "overwrite" : "truncation", events, original, changed);
}

/* Encrypt a directory through the core and shape objects in it with a model. */
static void
keyed_write(struct device *device, const char *exports, const char *source)
{
	static struct model model;
	struct keyring keyring;
	struct keyring other;
	struct ext4_crypto_environment crypto;
	struct ext4_inode_update update = creation();
	struct ext4_inode inode;
	struct ext4_fs *fs;
	struct model_object *object;
	char name[NAME_BYTES];
	char target[LONG_TARGET_BYTES + 1U];
	uint32_t block = device->block_size;
	unsigned int index;
	size_t completed;

	enable_encryption(device);
	keyring_init(&keyring, PROBE_KEY_OFFSET);
	keyring_init(&other, PROBE_KEY_OFFSET + 1U);
	memset(&model, 0, sizeof(model));
	model.paths[MODEL_VAULT] = "vault";
	model.paths[MODEL_SUB] = "vault/sub";
	model.paths[MODEL_PLAIN] = "plain";
	EXPECT(ext4_mount_writable(&device->environment, &device->writer, &fs), EXT4_OK);
	crypto = keyring_environment(&keyring);
	EXPECT(ext4_set_crypto(fs, &crypto), EXT4_OK);
	EXPECT(ext4_mkdir(fs, EXT4_ROOT_INODE, 0, (const uint8_t *)"vault", 5, &update,
		   &encrypt_time, &model.directories[MODEL_VAULT]),
	    EXT4_OK);
	EXPECT(ext4_mkdir(fs, EXT4_ROOT_INODE, 0, (const uint8_t *)"plain", 5, &update,
		   &encrypt_time, &model.directories[MODEL_PLAIN]),
	    EXT4_OK);
	model_create(fs, &model, MODEL_FILE, MODEL_PLAIN, "visible", NULL);
	model_write(fs, &model, model_find(&model, MODEL_PLAIN, "visible"), 0, 5000, 1);
	policies(fs, &model, &keyring, &other);
	/* Objects of every kind and size under encrypted names. */
	model_create(fs, &model, MODEL_DIRECTORY, MODEL_VAULT, "sub", NULL);
	model.count--;
	free(model.objects[model.count].data);
	model.directories[MODEL_SUB] = find(fs, model.directories[MODEL_VAULT].number, "sub");
	model_create(fs, &model, MODEL_FILE, MODEL_VAULT, "empty", NULL);
	model_create(fs, &model, MODEL_FILE, MODEL_VAULT, "tiny", NULL);
	model_write(fs, &model, model_find(&model, MODEL_VAULT, "tiny"), 0, 1, 2);
	model_create(fs, &model, MODEL_FILE, MODEL_VAULT, "block", NULL);
	model_write(fs, &model, model_find(&model, MODEL_VAULT, "block"), 0, block, 3);
	model_create(fs, &model, MODEL_FILE, MODEL_VAULT, "partial", NULL);
	model_write(fs, &model, model_find(&model, MODEL_VAULT, "partial"), 0, 3U * block + 77U, 4);
	model_create(fs, &model, MODEL_FILE, MODEL_VAULT, "sparse", NULL);
	object = model_find(&model, MODEL_VAULT, "sparse");
	model_write(fs, &model, object, 0, 100, 5);
	model_write(fs, &model, object, 5U * block + 10U, 490, 6);
	model_create(fs, &model, MODEL_FILE, MODEL_VAULT, "large", NULL);
	model_write(fs, &model, model_find(&model, MODEL_VAULT, "large"), 0, 20U * block + 11U, 7);
	for (index = 0; index < PROBE_LONG_NAME_BYTES; index++) {
		name[index] = (char)('a' + index % 26U);
	}
	name[PROBE_LONG_NAME_BYTES] = 0;
	model_create(fs, &model, MODEL_FILE, MODEL_VAULT, name, NULL);
	model_write(fs, &model, model_find(&model, MODEL_VAULT, name), 0, 2U * block, 8);
	model_create(fs, &model, MODEL_FILE, MODEL_SUB, "inner", NULL);
	model_write(fs, &model, model_find(&model, MODEL_SUB, "inner"), 0, block + 1U, 9);
	model_create(fs, &model, MODEL_SYMLINK, MODEL_VAULT, "short-link", "tiny");
	for (index = 0; index < LONG_TARGET_BYTES; index++) {
		target[index] = index % 50U == 49U ? '/' : (char)('A' + index % 26U);
	}
	target[LONG_TARGET_BYTES] = 0;
	model_create(fs, &model, MODEL_SYMLINK, MODEL_VAULT, "long-link", target);
	model_create(fs, &model, MODEL_FIFO, MODEL_VAULT, "fifo", NULL);
	model_verify(fs, &model);
	reshape(fs, &model, block);
	model_verify(fs, &model);
	moves(fs, &model);
	model_verify(fs, &model);
	/* Neither plaintext names nor contents reach the device. */
	EXPECT(ext4_sync(fs), EXT4_OK);
	CHECK(!device_contains(device, (const uint8_t *)name, PROBE_LONG_NAME_BYTES));
	CHECK(!device_contains(device, (const uint8_t *)target, LONG_TARGET_BYTES));
	CHECK(!device_contains(device, model_find(&model, MODEL_VAULT, "large")->data + 3U * block,
	    PLAINTEXT_PROBE_BYTES));
	CHECK(device_contains(
	    device, model_find(&model, MODEL_PLAIN, "visible")->data, PLAINTEXT_PROBE_BYTES));
	/* Without the key, encrypted names and contents are unreadable again. */
	EXPECT(ext4_set_crypto(fs, NULL), EXT4_OK);
	EXPECT(
	    ext4_lookup(fs, &model.directories[MODEL_VAULT], (const uint8_t *)"large", 5, &inode),
	    EXT4_ENCRYPTED);
	inode = find(fs, model.directories[MODEL_PLAIN].number, "block-out");
	EXPECT(ext4_read(fs, &inode, 0, target, 1, &completed), EXT4_ENCRYPTED);
	ext4_unmount(fs);
	CHECK(keyring.handles == 0 && other.handles == 0);
	/* The committed state reads back on a read-only mount. */
	EXPECT(ext4_mount(&device->environment, &fs), EXT4_OK);
	crypto = keyring_environment(&keyring);
	EXPECT(ext4_set_crypto(fs, &crypto), EXT4_OK);
	model.directories[MODEL_VAULT] = find(fs, EXT4_ROOT_INODE, "vault");
	model.directories[MODEL_PLAIN] = find(fs, EXT4_ROOT_INODE, "plain");
	model.directories[MODEL_SUB] = find(fs, model.directories[MODEL_VAULT].number, "sub");
	model_verify(fs, &model);
	ext4_unmount(fs);
	CHECK(keyring.handles == 0 && device->live == 0);
	if (exports != NULL) {
		storage_export(device, exports, source, "encrypted-");
		model_export(&model, exports, source);
	}
	for (index = 0; index < model.count; index++) {
		free(model.objects[index].data);
	}
	printf("PASS keyed writes encrypt %u names, contents and targets in %s\n", model.count,
	    source);
}

static void
read_only(struct device *device)
{
	struct ext4_fs *fs;
	struct ext4_inode secret;
	struct ext4_inode file;
	struct ext4_inode link;
	struct ext4_inode inode;
	struct ext4_mapping mapping;
	struct ext4_dir_entry entry;
	uint8_t byte;
	uint64_t cookie = 0;
	size_t completed;

	EXPECT(ext4_mount(&device->environment, &fs), EXT4_OK);
	CHECK(fs->info.feature_incompat & EXT4_FEATURE_INCOMPAT_ENCRYPT);
	secret = find(fs, EXT4_ROOT_INODE, "secret");
	CHECK(secret.flags & EXT4_INODE_ENCRYPT);
	EXPECT(ext4_iterate_dir(fs, &secret, &cookie, visit, NULL), EXT4_ENCRYPTED);
	EXPECT(ext4_next_dir(fs, &secret, &cookie, &entry), EXT4_ENCRYPTED);
	EXPECT(ext4_lookup(fs, &secret, (const uint8_t *)"inner", 5, &inode), EXT4_ENCRYPTED);
	/* Directory blocks hold ciphertext names but no encrypted bytes. */
	EXPECT(ext4_read(fs, &secret, 0, &byte, 1, &completed), EXT4_OK);
	encrypted_objects(fs, &file, &link);
	EXPECT(ext4_read(fs, &file, 0, &byte, 1, &completed), EXT4_ENCRYPTED);
	CHECK(completed == 0);
	EXPECT(ext4_map_read(fs, &file, 0, 1, &mapping), EXT4_ENCRYPTED);
	EXPECT(ext4_read(fs, &link, 0, &byte, 1, &completed), EXT4_ENCRYPTED);
	inode = find(fs, find(fs, EXT4_ROOT_INODE, "plain").number, "visible");
	verify_pattern(fs, &inode, PLAIN_FILE);
	ext4_unmount(fs);
	CHECK(device->live == 0 && device->writes == 0);
	puts("PASS read-only access denies encrypted names, contents and targets");
}

/* known supplies the source identity when its encrypted name cannot be looked up. */
static enum ext4_result
rename_names(struct ext4_fs *fs, uint32_t from_directory, const char *from_name,
    uint32_t to_directory, const char *to_name, const struct ext4_inode *known)
{
	struct ext4_inode source_parent;
	struct ext4_inode destination_parent;
	struct ext4_inode object;
	struct ext4_inode result;
	struct ext4_rename_entry from;
	struct ext4_rename_entry to;

	EXPECT(ext4_get_inode(fs, from_directory, &source_parent), EXT4_OK);
	EXPECT(ext4_get_inode(fs, to_directory, &destination_parent), EXT4_OK);
	object = known != NULL ? *known : find(fs, from_directory, from_name);
	from = (struct ext4_rename_entry){ source_parent.number, source_parent.generation,
		(const uint8_t *)from_name, strlen(from_name), object.number, object.generation };
	to = (struct ext4_rename_entry){ destination_parent.number, destination_parent.generation,
		(const uint8_t *)to_name, strlen(to_name), 0, 0 };
	return ext4_rename(fs, &from, &to, 0, &encrypt_time, &result);
}

static void
writable(struct device *device, const char *exports, const char *source)
{
	static const uint8_t byte = 'x';
	struct ext4_inode_update update = creation();
	struct ext4_inode_update attributes = { 0 };
	struct ext4_xattr_change context = { EXT4_XATTR_SET, EXT4_XATTR_INDEX_ENCRYPTION,
		(const uint8_t *)"c", 1, "forged", 6 };
	struct ext4_special_file fifo = { EXT4_FT_FIFO, 0, 0 };
	struct ext4_fs *fs;
	struct ext4_inode secret;
	struct ext4_inode plain;
	struct ext4_inode visible;
	struct ext4_inode file;
	struct ext4_inode link;
	struct ext4_inode result;
	uint64_t allocated;
	uint32_t writes;
	size_t completed;

	EXPECT(ext4_mount_writable(&device->environment, &device->writer, &fs), EXT4_OK);
	secret = find(fs, EXT4_ROOT_INODE, "secret");
	plain = find(fs, EXT4_ROOT_INODE, "plain");
	visible = find(fs, plain.number, "visible");
	encrypted_objects(fs, &file, &link);
	writes = device->writes;
	/* Names inside an encrypted directory would have to be encrypted. */
	EXPECT(ext4_create(fs, secret.number, secret.generation, (const uint8_t *)"new", 3, &update,
		   &encrypt_time, &result),
	    EXT4_ENCRYPTED);
	EXPECT(ext4_mkdir(fs, secret.number, secret.generation, (const uint8_t *)"new", 3, &update,
		   &encrypt_time, &result),
	    EXT4_ENCRYPTED);
	EXPECT(ext4_mknod(fs, secret.number, secret.generation, (const uint8_t *)"new", 3, &fifo,
		   &update, &encrypt_time, &result),
	    EXT4_ENCRYPTED);
	EXPECT(ext4_symlink(fs, secret.number, secret.generation, (const uint8_t *)"new", 3,
		   (const uint8_t *)"target", 6, &update, &encrypt_time, &result),
	    EXT4_ENCRYPTED);
	EXPECT(ext4_link(fs, secret.number, secret.generation, (const uint8_t *)"new", 3,
		   visible.number, visible.generation, &encrypt_time, &result),
	    EXT4_ENCRYPTED);
	EXPECT(ext4_unlink(fs, secret.number, secret.generation, (const uint8_t *)"any", 3,
		   file.number, file.generation, &encrypt_time, &result),
	    EXT4_ENCRYPTED);
	EXPECT(ext4_rmdir(fs, secret.number, secret.generation, (const uint8_t *)"any", 3,
		   file.number, file.generation, &encrypt_time, &result),
	    EXT4_ENCRYPTED);
	EXPECT(
	    rename_names(fs, secret.number, "any", EXT4_ROOT_INODE, "out", &file), EXT4_ENCRYPTED);
	EXPECT(
	    rename_names(fs, plain.number, "visible", secret.number, "in", NULL), EXT4_ENCRYPTED);
	/* Plaintext data cannot be written into, or cut out of, ciphertext. */
	update = data_update(&file);
	EXPECT(ext4_write(fs, file.number, file.generation, 0, &byte, 1, &update, &completed),
	    EXT4_ENCRYPTED);
	EXPECT(ext4_write_partial(
		   fs, file.number, file.generation, file.size, &byte, 1, &update, &completed),
	    EXT4_ENCRYPTED);
	EXPECT(
	    ext4_truncate(fs, file.number, file.generation, 1, &update, &result), EXT4_ENCRYPTED);
	EXPECT(ext4_truncate_atomic(
		   fs, file.number, file.generation, file.size + 1U, &update, &result),
	    EXT4_ENCRYPTED);
	EXPECT(ext4_fallocate(fs, file.number, file.generation, 0, 1, EXT4_FALLOC_KEEP_SIZE,
		   &update, &allocated),
	    EXT4_ENCRYPTED);
	attributes.fields = EXT4_ATTR_CHANGE_TIME | EXT4_ATTR_XATTRS;
	attributes.change_time = encrypt_time;
	attributes.xattrs = &context;
	attributes.xattr_count = 1;
	EXPECT(ext4_set_attributes(fs, file.number, file.generation, &attributes, &result),
	    EXT4_PERMISSION_DENIED);
	CHECK(device->writes == writes && !fs->aborted);

	/* Metadata of encrypted objects and the unencrypted namespace still change. */
	attributes.fields = EXT4_ATTR_PERMISSIONS | EXT4_ATTR_CHANGE_TIME | EXT4_ATTR_XATTRS;
	attributes.permissions = CHANGED_PERMISSIONS;
	attributes.xattrs = NULL;
	attributes.xattr_count = 0;
	EXPECT(
	    ext4_set_attributes(fs, file.number, file.generation, &attributes, &result), EXT4_OK);
	CHECK((result.mode & 07777U) == CHANGED_PERMISSIONS);
	EXPECT(ext4_rmdir(fs, EXT4_ROOT_INODE, 0, (const uint8_t *)"secret", 6, secret.number,
		   secret.generation, &encrypt_time, &result),
	    EXT4_NOT_EMPTY);
	result = find(fs, EXT4_ROOT_INODE, "locked-empty");
	EXPECT(ext4_rmdir(fs, EXT4_ROOT_INODE, 0, (const uint8_t *)"locked-empty", 12,
		   result.number, result.generation, &encrypt_time, &result),
	    EXT4_OK);
	EXPECT(rename_names(fs, EXT4_ROOT_INODE, "plain", EXT4_ROOT_INODE, "renamed-plain", NULL),
	    EXT4_OK);
	EXPECT(rename_names(fs, plain.number, "visible", plain.number, "visible-moved", NULL),
	    EXT4_OK);
	/* Moving the encrypted directory object rewrites its dotdot entry. */
	EXPECT(rename_names(fs, EXT4_ROOT_INODE, "secret", plain.number, "secret", NULL), EXT4_OK);
	EXPECT(rename_names(fs, plain.number, "secret", EXT4_ROOT_INODE, "secret", NULL), EXT4_OK);
	EXPECT(ext4_sync(fs), EXT4_OK);
	ext4_unmount(fs);
	CHECK(device->live == 0);

	EXPECT(ext4_mount(&device->environment, &fs), EXT4_OK);
	secret = find(fs, EXT4_ROOT_INODE, "secret");
	CHECK((secret.flags & EXT4_INODE_ENCRYPT) && secret.links >= 2);
	visible = find(fs, find(fs, EXT4_ROOT_INODE, "renamed-plain").number, "visible-moved");
	verify_pattern(fs, &visible, PLAIN_FILE);
	EXPECT(ext4_get_inode(fs, file.number, &result), EXT4_OK);
	CHECK(result.size == file.size && (result.mode & 07777U) == CHANGED_PERMISSIONS);
	ext4_unmount(fs);
	memcpy(device->stable, device->cache, device->size);
	storage_export(device, exports, source, "encrypt-mutated-");
	puts("PASS writable owner preserves ciphertext and changes unencrypted names");
}

int
main(int argc, char **argv)
{
	static struct device device;
	const char *image = NULL;
	const char *exports = NULL;
	bool synthetic = false;
	bool key = false;
	bool write = false;
	int argument;

	for (argument = 1; argument < argc; argument++) {
		if (strcmp(argv[argument], "--synthetic") == 0) {
			synthetic = true;
		} else if (strcmp(argv[argument], "--key") == 0) {
			key = true;
		} else if (strcmp(argv[argument], "--write") == 0) {
			write = true;
		} else if (image == NULL) {
			image = argv[argument];
		} else if (exports == NULL) {
			exports = argv[argument];
		} else {
			image = NULL;
			break;
		}
	}
	if (image == NULL || (key + synthetic + write) > 1) {
		fprintf(stderr,
		    "usage: %s [--synthetic | --key | --write] IMAGE [EXPORT_DIRECTORY]\n",
		    argv[0]);
		return 2;
	}
	storage_open(&device, image);
	if (write) {
		keyed_write(&device, exports, image);
		keyed_power_cuts(&device, CUT_OVERWRITE);
		keyed_power_cuts(&device, CUT_TRUNCATE);
		storage_close(&device);
		return 0;
	}
	if (synthetic) {
		synthesize(&device);
	}
	read_only(&device);
	if (key) {
		keyed(&device);
	}
	writable(&device, exports, image);
	storage_close(&device);
	return 0;
}

/* SPDX-License-Identifier: BSD-3-Clause */
#include "fscrypt.h"
#include "allocate.h"
#include "directory_write.h"
#include "inline.h"
#include "journal.h"
#include "sha.h"
#include "xattr.h"

union ext4_fscrypt_context_disk {
	struct ext4_fscrypt_context_v1_disk v1;
	struct ext4_fscrypt_context_v2_disk v2;
};

/* The core implements fscrypt's formats, key derivation inputs, IVs and padding as
 * Linux defines them; the adapter supplies master keys, derivation and ciphers. */

static const uint8_t ext4_base64url[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

#define EXT4_BASE64_BITS 6U
#define EXT4_BASE64_ALPHABET 64U

/* Public filename bytes, not key material. Decode each alphabet range directly. */
static uint32_t
ext4_base64url_value(uint8_t byte)
{
	if (byte >= 'A' && byte <= 'Z') {
		return byte - 'A';
	}
	if (byte >= 'a' && byte <= 'z') {
		return byte - 'a' + 26U;
	}
	if (byte >= '0' && byte <= '9') {
		return byte - '0' + 52U;
	}
	if (byte == '-') {
		return 62U;
	}
	return byte == '_' ? 63U : EXT4_BASE64_ALPHABET;
}

size_t
ext4_fscrypt_nokey_encode(
    const uint8_t *cipher, size_t length, uint32_t hash, uint32_t minor_hash, uint8_t *name)
{
	struct ext4_sha256 context;
	struct ext4_le32 word;
	uint8_t nokey[EXT4_FSCRYPT_NOKEY_MAX];
	uint32_t accumulator = 0;
	size_t size;
	size_t index;
	size_t written = 0;
	unsigned int bits = 0;

	ext4_encode32(&word, hash);
	ext4_copy(nokey, &word, sizeof(word));
	ext4_encode32(&word, minor_hash);
	ext4_copy(nokey + sizeof(word), &word, sizeof(word));
	if (length <= EXT4_FSCRYPT_NOKEY_BYTES) {
		ext4_copy(nokey + EXT4_FSCRYPT_NOKEY_HASHES, cipher, length);
		size = EXT4_FSCRYPT_NOKEY_HASHES + length;
	} else {
		ext4_copy(nokey + EXT4_FSCRYPT_NOKEY_HASHES, cipher, EXT4_FSCRYPT_NOKEY_BYTES);
		ext4_sha256_init(&context);
		ext4_sha256_update(
		    &context, cipher + EXT4_FSCRYPT_NOKEY_BYTES, length - EXT4_FSCRYPT_NOKEY_BYTES);
		ext4_sha256_final(
		    &context, nokey + EXT4_FSCRYPT_NOKEY_HASHES + EXT4_FSCRYPT_NOKEY_BYTES);
		size = EXT4_FSCRYPT_NOKEY_MAX;
	}
	for (index = 0; index < size; index++) {
		accumulator = (accumulator << 8) | nokey[index];
		bits += 8U;
		while (bits >= EXT4_BASE64_BITS) {
			bits -= EXT4_BASE64_BITS;
			name[written++] =
			    ext4_base64url[(accumulator >> bits) & (EXT4_BASE64_ALPHABET - 1U)];
		}
	}
	if (bits != 0) {
		name[written++] = ext4_base64url[(accumulator << (EXT4_BASE64_BITS - bits)) &
		    (EXT4_BASE64_ALPHABET - 1U)];
	}
	return written;
}

bool
ext4_fscrypt_nokey_decode(const uint8_t *name, size_t length, struct ext4_fscrypt_nokey *nokey)
{
	uint32_t accumulator = 0;
	uint32_t value;
	size_t index;
	unsigned int bits = 0;

	nokey->size = 0;
	if (length > EXT4_FSCRYPT_NOKEY_NAME_MAX) {
		return false;
	}
	for (index = 0; index < length; index++) {
		value = ext4_base64url_value(name[index]);
		if (value == EXT4_BASE64_ALPHABET) {
			return false;
		}
		accumulator = (accumulator << EXT4_BASE64_BITS) | value;
		bits += EXT4_BASE64_BITS;
		if (bits >= 8U) {
			bits -= 8U;
			nokey->bytes[nokey->size++] = (uint8_t)(accumulator >> bits);
		}
	}
	/* Leftover bits must be zero, and the size one Linux can produce. */
	return (accumulator & ((1U << bits) - 1U)) == 0 &&
	    nokey->size >= EXT4_FSCRYPT_NOKEY_HASHES + EXT4_FSCRYPT_NAME_MIN &&
	    (nokey->size <= EXT4_FSCRYPT_NOKEY_HASHES + EXT4_FSCRYPT_NOKEY_BYTES ||
		nokey->size == EXT4_FSCRYPT_NOKEY_MAX);
}

const uint8_t *
ext4_fscrypt_nokey_cipher(const struct ext4_fscrypt_nokey *nokey, size_t *length)
{
	if (nokey->size == EXT4_FSCRYPT_NOKEY_MAX) {
		return NULL;
	}
	*length = nokey->size - EXT4_FSCRYPT_NOKEY_HASHES;
	return nokey->bytes + EXT4_FSCRYPT_NOKEY_HASHES;
}

bool
ext4_fscrypt_nokey_match(
    const struct ext4_fscrypt_nokey *nokey, const uint8_t *cipher, size_t length)
{
	struct ext4_sha256 context;
	uint8_t digest[EXT4_SHA256_DIGEST_SIZE];
	const uint8_t *full;
	size_t full_length;

	full = ext4_fscrypt_nokey_cipher(nokey, &full_length);
	if (full != NULL) {
		return length == full_length && ext4_equal(cipher, full, length);
	}
	if (length <= EXT4_FSCRYPT_NOKEY_BYTES ||
	    !ext4_equal(
		cipher, nokey->bytes + EXT4_FSCRYPT_NOKEY_HASHES, EXT4_FSCRYPT_NOKEY_BYTES)) {
		return false;
	}
	ext4_sha256_init(&context);
	ext4_sha256_update(
	    &context, cipher + EXT4_FSCRYPT_NOKEY_BYTES, length - EXT4_FSCRYPT_NOKEY_BYTES);
	ext4_sha256_final(&context, digest);
	return ext4_equal(digest,
	    nokey->bytes + EXT4_FSCRYPT_NOKEY_HASHES + EXT4_FSCRYPT_NOKEY_BYTES, sizeof(digest));
}

bool
ext4_fscrypt_dot(const uint8_t *name, size_t length)
{
	return (length == 1 && name[0] == '.') || (length == 2 && name[0] == '.' && name[1] == '.');
}

enum ext4_result
ext4_fscrypt_policy(
    struct ext4_fs *fs, const struct ext4_inode *inode, struct ext4_fscrypt_policy *policy)
{
	union ext4_fscrypt_context_disk context;

	size_t size;
	enum ext4_result error;

	ext4_zero(policy, sizeof(*policy));
	error = ext4_get_xattr(fs, inode->number, inode->generation, EXT4_XATTR_INDEX_ENCRYPTION,
	    (const uint8_t *)EXT4_FSCRYPT_CONTEXT_NAME, EXT4_FSCRYPT_CONTEXT_NAME_SIZE, &context,
	    sizeof(context), &size);
	/* An encrypted inode without a context of either size cannot be decrypted. */
	if (error == EXT4_NOT_FOUND || error == EXT4_RANGE) {
		return EXT4_CORRUPT;
	}
	if (error != EXT4_OK) {
		return error;
	}
	if (size == sizeof(context.v1) && context.v1.version == EXT4_FSCRYPT_CONTEXT_V1) {
		policy->version = EXT4_FSCRYPT_CONTEXT_V1;
		policy->contents_mode = context.v1.contents_mode;
		policy->filenames_mode = context.v1.filenames_mode;
		policy->flags = context.v1.flags;
		policy->identifier_size = EXT4_FSCRYPT_DESCRIPTOR_SIZE;
		ext4_copy(policy->identifier, context.v1.descriptor, EXT4_FSCRYPT_DESCRIPTOR_SIZE);
		ext4_copy(policy->nonce, context.v1.nonce, EXT4_FSCRYPT_NONCE_SIZE);
	} else if (size == sizeof(context.v2) && context.v2.version == EXT4_FSCRYPT_CONTEXT_V2) {
		/* A data unit other than the filesystem block is not implemented. */
		if (context.v2.log2_data_unit_size != 0) {
			return EXT4_UNSUPPORTED;
		}
		if (context.v2.reserved[0] != 0 || context.v2.reserved[1] != 0 ||
		    context.v2.reserved[2] != 0) {
			return EXT4_CORRUPT;
		}
		policy->version = EXT4_FSCRYPT_CONTEXT_V2;
		policy->contents_mode = context.v2.contents_mode;
		policy->filenames_mode = context.v2.filenames_mode;
		policy->flags = context.v2.flags;
		policy->identifier_size = EXT4_FSCRYPT_IDENTIFIER_SIZE;
		ext4_copy(policy->identifier, context.v2.identifier, EXT4_FSCRYPT_IDENTIFIER_SIZE);
		ext4_copy(policy->nonce, context.v2.nonce, EXT4_FSCRYPT_NONCE_SIZE);
	} else {
		return EXT4_UNSUPPORTED;
	}
	if (policy->contents_mode != EXT4_FSCRYPT_MODE_AES_256_XTS ||
	    policy->filenames_mode != EXT4_FSCRYPT_MODE_AES_256_CTS ||
	    (policy->flags & ~(uint32_t)EXT4_FSCRYPT_PAD_MASK) != 0) {
		return EXT4_UNSUPPORTED;
	}
	return EXT4_OK;
}

static void
ext4_fscrypt_remember(
    struct ext4_fs *fs, const struct ext4_inode *inode, const struct ext4_fscrypt_key *key)
{
	struct ext4_fscrypt_cached_key *entry = &fs->fscrypt_keys[fs->fscrypt_key_next];

	if (fs->fscrypt_key_count == EXT4_FSCRYPT_KEYS) {
		fs->crypto.release_key(fs->crypto.context, entry->handle);
		if (entry->hash_handle != NULL) {
			fs->crypto.release_key(fs->crypto.context, entry->hash_handle);
		}
	} else {
		fs->fscrypt_key_count++;
	}
	fs->fscrypt_key_next = (fs->fscrypt_key_next + 1U) % EXT4_FSCRYPT_KEYS;
	entry->number = inode->number;
	entry->generation = inode->generation;
	entry->handle = key->handle;
	entry->mode = key->mode;
	entry->flags = key->flags;
	entry->hash_handle = key->hash_handle;
}

static enum ext4_result
ext4_fscrypt_derive_keys(struct ext4_fs *fs, const struct ext4_fscrypt_policy *policy,
    uint32_t type, bool hashed, struct ext4_fscrypt_key *key)
{
	uint8_t info[EXT4_FSCRYPT_HKDF_PREFIX_SIZE + 1U + EXT4_FSCRYPT_NONCE_SIZE];
	void *master = NULL;
	size_t info_size;
	enum ext4_result error;

	ext4_zero(key, sizeof(*key));
	if (fs->crypto.find_key == NULL) {
		return EXT4_ENCRYPTED;
	}
	if (hashed && policy->version != EXT4_FSCRYPT_CONTEXT_V2) {
		return EXT4_UNSUPPORTED;
	}
	if (type != EXT4_MODE_REGULAR && type != EXT4_MODE_DIRECTORY && type != EXT4_MODE_SYMLINK) {
		return EXT4_UNSUPPORTED;
	}
	/* Regular files encrypt contents; directories and symlinks encrypt names. */
	key->mode = type == EXT4_MODE_REGULAR ? policy->contents_mode : policy->filenames_mode;
	key->flags = policy->flags;
	error = fs->crypto.find_key(fs->crypto.context, policy->version, policy->identifier,
	    policy->identifier_size, &master);
	if (error == EXT4_NOT_FOUND) {
		return EXT4_ENCRYPTED;
	}
	if (error != EXT4_OK) {
		return error;
	}
	if (hashed && fs->crypto.siphash == NULL) {
		fs->crypto.release_key(fs->crypto.context, master);
		return EXT4_UNSUPPORTED;
	}
	if (policy->version == EXT4_FSCRYPT_CONTEXT_V2) {
		ext4_copy(info, EXT4_FSCRYPT_HKDF_PREFIX, EXT4_FSCRYPT_HKDF_PREFIX_SIZE);
		info[EXT4_FSCRYPT_HKDF_PREFIX_SIZE] = EXT4_FSCRYPT_HKDF_PER_FILE_KEY;
		ext4_copy(info + EXT4_FSCRYPT_HKDF_PREFIX_SIZE + 1U, policy->nonce,
		    EXT4_FSCRYPT_NONCE_SIZE);
		info_size = sizeof(info);
	} else {
		ext4_copy(info, policy->nonce, EXT4_FSCRYPT_NONCE_SIZE);
		info_size = EXT4_FSCRYPT_NONCE_SIZE;
	}
	error = fs->crypto.derive_key(fs->crypto.context, master, policy->version, info, info_size,
	    key->mode == EXT4_FSCRYPT_MODE_AES_256_XTS ? EXT4_FSCRYPT_XTS_KEY_SIZE
						       : EXT4_FSCRYPT_CTS_KEY_SIZE,
	    &key->handle);
	if (error == EXT4_OK && hashed) {
		info[EXT4_FSCRYPT_HKDF_PREFIX_SIZE] = EXT4_FSCRYPT_HKDF_DIRECTORY_HASH;
		error = fs->crypto.derive_key(fs->crypto.context, master, policy->version,
		    info, info_size, EXT4_FSCRYPT_HASH_KEY_SIZE, &key->hash_handle);
		if (error != EXT4_OK) {
			fs->crypto.release_key(fs->crypto.context, key->handle);
			key->handle = NULL;
			key->hash_handle = NULL;
		}
	}
	fs->crypto.release_key(fs->crypto.context, master);
	return error;
}

enum ext4_result
ext4_fscrypt_derive(struct ext4_fs *fs, const struct ext4_fscrypt_policy *policy, uint32_t type,
    struct ext4_fscrypt_key *key)
{
	return ext4_fscrypt_derive_keys(fs, policy, type, false, key);
}

enum ext4_result
ext4_fscrypt_directory_policy(struct ext4_fs *fs, const struct ext4_inode *directory)
{
	struct ext4_fscrypt_policy policy;
	enum ext4_result error;

	if (!ext4_directory_has_hashes(directory->flags)) {
		return EXT4_OK;
	}
	if (!(fs->info.feature_incompat & EXT4_FEATURE_INCOMPAT_CASEFOLD)) {
		return EXT4_CORRUPT;
	}
	/* Linux encryption uses block-backed directories. Do not send combined
	 * ciphertext through the inline plaintext record parser. */
	if (directory->flags & EXT4_INODE_INLINE_DATA) {
		return EXT4_UNSUPPORTED;
	}
	error = ext4_fscrypt_policy(fs, directory, &policy);
	if (error == EXT4_OK && policy.version != EXT4_FSCRYPT_CONTEXT_V2) {
		error = EXT4_UNSUPPORTED;
	}
	return error;
}

enum ext4_result
ext4_fscrypt_name_hash(struct ext4_fs *fs, const struct ext4_fscrypt_key *key,
    const uint8_t *name, size_t length, struct ext4_name_hash *hash)
{
	struct ext4_name_hash result;
	uint64_t combined = 0;
	enum ext4_result error;

	if (key->hash_handle == NULL || fs->crypto.siphash == NULL) {
		return EXT4_UNSUPPORTED;
	}
	error = fs->crypto.siphash(fs->crypto.context, key->hash_handle, name, length, &combined);
	if (error != EXT4_OK) {
		return error;
	}
	result.major = (uint32_t)(combined >> 32) & ~1U;
	result.minor = (uint32_t)combined;
	if (result.major == EXT4_HASH_EOF) {
		result.major -= 2U;
	}
	*hash = result;
	return EXT4_OK;
}

enum ext4_result
ext4_fscrypt_key(struct ext4_fs *fs, const struct ext4_inode *inode, struct ext4_fscrypt_key *key)
{
	struct ext4_fscrypt_policy policy;
	struct ext4_fscrypt_cached_key *entry;
	uint32_t index;
	enum ext4_result error;

	for (index = 0; index < fs->fscrypt_key_count; index++) {
		entry = &fs->fscrypt_keys[index];
		if (entry->number == inode->number && entry->generation == inode->generation) {
			key->handle = entry->handle;
			key->mode = entry->mode;
			key->flags = entry->flags;
			key->hash_handle = entry->hash_handle;
			return EXT4_OK;
		}
	}
	if (fs->crypto.find_key == NULL) {
		return EXT4_ENCRYPTED;
	}
	error = ext4_fscrypt_policy(fs, inode, &policy);
	if (error == EXT4_OK) {
		error = ext4_fscrypt_derive_keys(fs, &policy, inode->mode & EXT4_MODE_TYPE,
		    (inode->mode & EXT4_MODE_TYPE) == EXT4_MODE_DIRECTORY &&
			ext4_directory_has_hashes(inode->flags), key);
	}
	if (error == EXT4_OK) {
		ext4_fscrypt_remember(fs, inode, key);
	}
	return error;
}

void
ext4_fscrypt_forget(struct ext4_fs *fs)
{
	uint32_t index;

	for (index = 0; index < fs->fscrypt_key_count; index++) {
		fs->crypto.release_key(fs->crypto.context, fs->fscrypt_keys[index].handle);
		if (fs->fscrypt_keys[index].hash_handle != NULL) {
			fs->crypto.release_key(fs->crypto.context, fs->fscrypt_keys[index].hash_handle);
		}
	}
	fs->fscrypt_key_count = 0;
	fs->fscrypt_key_next = 0;
}

enum ext4_result
ext4_fscrypt_block(struct ext4_fs *fs, const struct ext4_fscrypt_key *key, uint64_t logical,
    bool encrypt, const void *input, void *output)
{
	uint8_t iv[EXT4_FSCRYPT_IV_SIZE] = { 0 };

	/* The IV is the data unit's index, little-endian, zero-extended. */
	ext4_encode32((struct ext4_le32 *)iv, (uint32_t)logical);
	ext4_encode32(
	    (struct ext4_le32 *)(iv + sizeof(struct ext4_le32)), (uint32_t)(logical >> 32));
	return fs->crypto.cipher(fs->crypto.context, key->handle, key->mode, encrypt, iv, input,
	    output, fs->info.block_size);
}

enum ext4_result
ext4_fscrypt_name_decrypt(struct ext4_fs *fs, const struct ext4_fscrypt_key *key,
    const uint8_t *cipher, size_t length, size_t maximum, uint8_t *plain, size_t *plain_length)
{
	uint8_t iv[EXT4_FSCRYPT_IV_SIZE] = { 0 };
	enum ext4_result error;

	if (length < EXT4_FSCRYPT_NAME_MIN || length > maximum) {
		return EXT4_CORRUPT;
	}
	error = fs->crypto.cipher(
	    fs->crypto.context, key->handle, key->mode, false, iv, cipher, plain, length);
	if (error != EXT4_OK) {
		return error;
	}
	*plain_length = 0;
	while (*plain_length < length && plain[*plain_length] != 0) {
		(*plain_length)++;
	}
	return *plain_length == 0 ? EXT4_CORRUPT : EXT4_OK;
}

enum ext4_result
ext4_fscrypt_name_encrypt(struct ext4_fs *fs, const struct ext4_fscrypt_key *key,
    const uint8_t *plain, size_t length, size_t maximum, uint8_t *padded, uint8_t *cipher,
    size_t *cipher_length)
{
	uint8_t iv[EXT4_FSCRYPT_IV_SIZE] = { 0 };
	size_t padding = (size_t)EXT4_FSCRYPT_PAD_BASE << (key->flags & EXT4_FSCRYPT_PAD_MASK);
	size_t size = length < EXT4_FSCRYPT_NAME_MIN ? EXT4_FSCRYPT_NAME_MIN : length;

	if (length == 0 || length > maximum) {
		return EXT4_NAME_TOO_LONG;
	}
	size = (size + padding - 1U) / padding * padding;
	if (size > maximum) {
		size = maximum;
	}
	ext4_zero(padded, size);
	ext4_copy(padded, plain, length);
	*cipher_length = size;
	return fs->crypto.cipher(
	    fs->crypto.context, key->handle, key->mode, true, iv, padded, cipher, size);
}

enum ext4_result
ext4_fscrypt_symlink_encrypt(struct ext4_fs *fs, const struct ext4_fscrypt_key *key,
    const uint8_t *target, size_t length, uint8_t *stored, size_t *stored_length)
{
	struct ext4_le16 cipher_length_disk;
	uint8_t *padded;
	size_t maximum = fs->info.block_size - EXT4_FSCRYPT_SYMLINK_HEADER - 1U;
	size_t cipher_length = 0;
	enum ext4_result error;

	padded = fs->environment.allocate(fs->environment.context, fs->info.block_size);
	if (padded == NULL) {
		return EXT4_NO_MEMORY;
	}
	ext4_zero(stored, fs->info.block_size);
	error = ext4_fscrypt_name_encrypt(fs, key, target, length, maximum, padded,
	    stored + EXT4_FSCRYPT_SYMLINK_HEADER, &cipher_length);
	fs->environment.release(fs->environment.context, padded, fs->info.block_size);
	if (error != EXT4_OK) {
		return error;
	}
	/* Linux counts a NUL after the ciphertext in the stored link but not the size. */
	ext4_encode16(&cipher_length_disk, (uint16_t)cipher_length);
	ext4_copy(stored, &cipher_length_disk, sizeof(cipher_length_disk));
	*stored_length = EXT4_FSCRYPT_SYMLINK_HEADER + cipher_length;
	return EXT4_OK;
}

/* Admit a policy's modes and flags as ext4_fscrypt_policy does for contexts. */
static enum ext4_result
ext4_fscrypt_policy_valid(
    uint8_t version, uint8_t contents_mode, uint8_t filenames_mode, uint8_t flags)
{
	if (version != EXT4_FSCRYPT_CONTEXT_V1 && version != EXT4_FSCRYPT_CONTEXT_V2) {
		return EXT4_INVALID_ARGUMENT;
	}
	if (contents_mode != EXT4_FSCRYPT_MODE_AES_256_XTS ||
	    filenames_mode != EXT4_FSCRYPT_MODE_AES_256_CTS ||
	    (flags & ~(uint32_t)EXT4_FSCRYPT_PAD_MASK) != 0) {
		return EXT4_UNSUPPORTED;
	}
	return EXT4_OK;
}

bool
ext4_fscrypt_policy_equal(
    const struct ext4_fscrypt_policy *left, const struct ext4_fscrypt_policy *right)
{
	return left->version == right->version && left->contents_mode == right->contents_mode &&
	    left->filenames_mode == right->filenames_mode && left->flags == right->flags &&
	    left->identifier_size == right->identifier_size &&
	    ext4_equal(left->identifier, right->identifier, left->identifier_size);
}

/* Store a policy with a fresh nonce as the inode's context attribute. */
static enum ext4_result
ext4_fscrypt_store(struct ext4_allocation *allocation, struct ext4_inode *inode,
    struct ext4_inode_disk *disk, struct ext4_fscrypt_policy *policy)
{
	struct ext4_fs *fs = allocation->fs;
	struct ext4_xattr_change change;

	union ext4_fscrypt_context_disk context;

	uint8_t nonce[EXT4_FSCRYPT_NONCE_SIZE];
	enum ext4_result error;

	error = fs->crypto.random_bytes(fs->crypto.context, nonce, sizeof(nonce));
	if (error != EXT4_OK) {
		return error;
	}
	ext4_copy(policy->nonce, nonce, sizeof(nonce));
	ext4_zero(&context, sizeof(context));
	ext4_zero(&change, sizeof(change));
	if (policy->version == EXT4_FSCRYPT_CONTEXT_V1) {
		context.v1.version = EXT4_FSCRYPT_CONTEXT_V1;
		context.v1.contents_mode = policy->contents_mode;
		context.v1.filenames_mode = policy->filenames_mode;
		context.v1.flags = policy->flags;
		ext4_copy(context.v1.descriptor, policy->identifier, EXT4_FSCRYPT_DESCRIPTOR_SIZE);
		ext4_copy(context.v1.nonce, nonce, sizeof(nonce));
		change.value_size = sizeof(context.v1);
	} else {
		context.v2.version = EXT4_FSCRYPT_CONTEXT_V2;
		context.v2.contents_mode = policy->contents_mode;
		context.v2.filenames_mode = policy->filenames_mode;
		context.v2.flags = policy->flags;
		ext4_copy(context.v2.identifier, policy->identifier, EXT4_FSCRYPT_IDENTIFIER_SIZE);
		ext4_copy(context.v2.nonce, nonce, sizeof(nonce));
		change.value_size = sizeof(context.v2);
	}
	change.policy = EXT4_XATTR_CREATE;
	change.name_index = EXT4_XATTR_INDEX_ENCRYPTION;
	change.name = (const uint8_t *)EXT4_FSCRYPT_CONTEXT_NAME;
	change.name_length = EXT4_FSCRYPT_CONTEXT_NAME_SIZE;
	change.value = &context;
	error = ext4_xattr_apply(allocation, inode, disk, &change, 1);
	if (error == EXT4_OK) {
		inode->flags |= EXT4_INODE_ENCRYPT;
		ext4_encode32(&disk->flags, inode->flags);
	}
	return error;
}

enum ext4_result
ext4_fscrypt_inherit(struct ext4_allocation *allocation, const struct ext4_inode *parent,
    struct ext4_inode *child, struct ext4_inode_disk *disk, struct ext4_fscrypt_policy *policy)
{
	uint32_t type = child->mode & EXT4_MODE_TYPE;
	enum ext4_result error;

	/* Like Linux, special files keep unencrypted inodes under encrypted names. */
	if (!(parent->flags & EXT4_INODE_ENCRYPT) ||
	    (type != EXT4_MODE_REGULAR && type != EXT4_MODE_DIRECTORY &&
		type != EXT4_MODE_SYMLINK)) {
		return EXT4_OK;
	}
	error = ext4_fscrypt_policy(allocation->fs, parent, policy);
	if (error == EXT4_OK) {
		error = ext4_fscrypt_store(allocation, child, disk, policy);
	}
	return error;
}

enum ext4_result
ext4_fscrypt_permitted(
    struct ext4_fs *fs, const struct ext4_inode *directory, const struct ext4_inode *inode)
{
	struct ext4_fscrypt_policy expected;
	struct ext4_fscrypt_policy actual;
	uint32_t type = inode->mode & EXT4_MODE_TYPE;
	enum ext4_result error;

	if (!(directory->flags & EXT4_INODE_ENCRYPT) ||
	    (type != EXT4_MODE_REGULAR && type != EXT4_MODE_DIRECTORY &&
		type != EXT4_MODE_SYMLINK)) {
		return EXT4_OK;
	}
	if (!(inode->flags & EXT4_INODE_ENCRYPT)) {
		return EXT4_CROSS_POLICY;
	}
	error = ext4_fscrypt_policy(fs, directory, &expected);
	if (error == EXT4_OK) {
		error = ext4_fscrypt_policy(fs, inode, &actual);
	}
	if (error == EXT4_OK && !ext4_fscrypt_policy_equal(&expected, &actual)) {
		error = EXT4_CROSS_POLICY;
	}
	return error;
}

enum ext4_result
ext4_get_encryption_policy(
    struct ext4_fs *fs, const struct ext4_inode *inode, struct ext4_encryption_policy *result)
{
	struct ext4_fscrypt_policy policy;
	enum ext4_result error;

	if (fs == NULL || inode == NULL || result == NULL) {
		return EXT4_INVALID_ARGUMENT;
	}
	if (fs->aborted) {
		return EXT4_RECOVERY_REQUIRED;
	}
	if (!(inode->flags & EXT4_INODE_ENCRYPT)) {
		return EXT4_NOT_FOUND;
	}
	error = ext4_fscrypt_policy(fs, inode, &policy);
	if (error != EXT4_OK) {
		return error;
	}
	ext4_zero(result, sizeof(*result));
	result->version = policy.version;
	result->contents_mode = policy.contents_mode;
	result->filenames_mode = policy.filenames_mode;
	result->flags = policy.flags;
	ext4_copy(result->identifier, policy.identifier, policy.identifier_size);
	return EXT4_OK;
}

enum ext4_result
ext4_set_encryption_policy(struct ext4_fs *fs, uint32_t number, uint32_t generation,
    const struct ext4_encryption_policy *requested, struct ext4_inode *result)
{
	struct ext4_transaction *transaction;
	struct ext4_allocation allocation;
	struct ext4_inode_disk *disk;
	struct ext4_inode inode;
	struct ext4_fscrypt_policy policy;
	struct ext4_fscrypt_policy existing;
	void *master = NULL;
	bool ready = false;
	enum ext4_result error;

	if (fs == NULL || requested == NULL || result == NULL) {
		return EXT4_INVALID_ARGUMENT;
	}
	if (fs->aborted) {
		return EXT4_RECOVERY_REQUIRED;
	}
	if (fs->journal == NULL) {
		return EXT4_READ_ONLY;
	}
	error = ext4_fscrypt_policy_valid(requested->version, requested->contents_mode,
	    requested->filenames_mode, requested->flags);
	if (error != EXT4_OK) {
		return error;
	}
	if (!(fs->info.feature_incompat & EXT4_FEATURE_INCOMPAT_ENCRYPT)) {
		return EXT4_UNSUPPORTED;
	}
	ext4_zero(&policy, sizeof(policy));
	policy.version = requested->version;
	policy.contents_mode = requested->contents_mode;
	policy.filenames_mode = requested->filenames_mode;
	policy.flags = requested->flags;
	policy.identifier_size = requested->version == EXT4_FSCRYPT_CONTEXT_V1
	    ? EXT4_FSCRYPT_DESCRIPTOR_SIZE
	    : EXT4_FSCRYPT_IDENTIFIER_SIZE;
	ext4_copy(policy.identifier, requested->identifier, policy.identifier_size);
	/* The adapter must hold the master key, as Linux requires it for new policies. */
	if (fs->crypto.find_key == NULL) {
		return EXT4_ENCRYPTED;
	}
	error = fs->crypto.find_key(
	    fs->crypto.context, policy.version, policy.identifier, policy.identifier_size, &master);
	if (error == EXT4_NOT_FOUND) {
		return EXT4_ENCRYPTED;
	}
	if (error != EXT4_OK) {
		return error;
	}
	fs->crypto.release_key(fs->crypto.context, master);
	error =
	    ext4_transaction_begin(fs->journal, ext4_journal_credits(fs->journal), &transaction);
	if (error != EXT4_OK) {
		return error;
	}
	error = ext4_edit_inode(fs, transaction, number, generation, &disk, &inode);
	if (error == EXT4_OK && (inode.mode & EXT4_MODE_TYPE) != EXT4_MODE_DIRECTORY) {
		error = EXT4_NOT_DIRECTORY;
	}
	if (error == EXT4_OK && (inode.flags & EXT4_INODE_ENCRYPT)) {
		error = ext4_fscrypt_policy(fs, &inode, &existing);
		if (error == EXT4_OK && ext4_directory_has_hashes(inode.flags) &&
		    existing.version != EXT4_FSCRYPT_CONTEXT_V2) {
			error = EXT4_UNSUPPORTED;
		}
		if (error == EXT4_OK) {
			error =
			    ext4_fscrypt_policy_equal(&existing, &policy) ? EXT4_OK : EXT4_EXISTS;
		}
		ext4_transaction_cancel(transaction);
		if (error == EXT4_OK) {
			*result = inode;
		}
		return error;
	}
	if (error == EXT4_OK && (inode.flags & EXT4_INODE_IMMUTABLE)) {
		error = EXT4_PERMISSION_DENIED;
	}
	if (error == EXT4_OK && (inode.flags & EXT4_INODE_CASEFOLD) &&
	    policy.version != EXT4_FSCRYPT_CONTEXT_V2) {
		error = EXT4_UNSUPPORTED;
	}
	if (error == EXT4_OK) {
		error = ext4_allocation_init(&allocation, fs, transaction, &inode);
		ready = error == EXT4_OK;
	}
	if (error == EXT4_OK) {
		error = ext4_directory_change_format(&allocation, &inode, disk,
		    inode.flags | EXT4_INODE_ENCRYPT);
	}
	if (error == EXT4_OK) {
		error = ext4_fscrypt_store(&allocation, &inode, disk, &policy);
	}
	if (error == EXT4_OK) {
		error = ext4_inode_account(&allocation, &inode, disk, inode.size);
	}
	if (error == EXT4_OK) {
		ext4_inode_checksum_set(fs, number, disk);
		error = ext4_inode_decode_live(fs, number, disk, &inode);
	}
	if (ready) {
		ext4_allocation_destroy(&allocation);
	}
	if (error != EXT4_OK) {
		ext4_transaction_cancel(transaction);
		return error;
	}
	error = ext4_transaction_commit(transaction);
	if (error != EXT4_OK) {
		if (!ext4_commit_rejected(fs->journal, error)) {
			fs->aborted = true;
		}
		return error;
	}
	ext4_fscrypt_forget(fs);
	*result = inode;
	return EXT4_OK;
}

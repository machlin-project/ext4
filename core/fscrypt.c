/* SPDX-License-Identifier: BSD-3-Clause */
#include "fscrypt.h"

/* The core implements fscrypt's formats, key derivation inputs, IVs and padding as
 * Linux defines them; the adapter supplies master keys, derivation and ciphers. */

bool
ext4_fscrypt_dot(const uint8_t *name, size_t length)
{
	return (length == 1 && name[0] == '.') || (length == 2 && name[0] == '.' && name[1] == '.');
}

enum ext4_result
ext4_fscrypt_policy(
    struct ext4_fs *fs, const struct ext4_inode *inode, struct ext4_fscrypt_policy *policy)
{
	union {
		struct ext4_fscrypt_context_v1_disk v1;
		struct ext4_fscrypt_context_v2_disk v2;
	} context;

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
	} else {
		fs->fscrypt_key_count++;
	}
	fs->fscrypt_key_next = (fs->fscrypt_key_next + 1U) % EXT4_FSCRYPT_KEYS;
	entry->number = inode->number;
	entry->generation = inode->generation;
	entry->handle = key->handle;
	entry->mode = key->mode;
	entry->flags = key->flags;
}

enum ext4_result
ext4_fscrypt_key(struct ext4_fs *fs, const struct ext4_inode *inode, struct ext4_fscrypt_key *key)
{
	struct ext4_fscrypt_policy policy;
	struct ext4_fscrypt_cached_key *entry;
	uint8_t info[EXT4_FSCRYPT_HKDF_PREFIX_SIZE + 1U + EXT4_FSCRYPT_NONCE_SIZE];
	void *master = NULL;
	size_t info_size;
	uint32_t index;
	uint32_t type = inode->mode & EXT4_MODE_TYPE;
	enum ext4_result error;

	for (index = 0; index < fs->fscrypt_key_count; index++) {
		entry = &fs->fscrypt_keys[index];
		if (entry->number == inode->number && entry->generation == inode->generation) {
			key->handle = entry->handle;
			key->mode = entry->mode;
			key->flags = entry->flags;
			return EXT4_OK;
		}
	}
	if (fs->crypto.find_key == NULL) {
		return EXT4_ENCRYPTED;
	}
	if (type != EXT4_MODE_REGULAR && type != EXT4_MODE_DIRECTORY && type != EXT4_MODE_SYMLINK) {
		return EXT4_UNSUPPORTED;
	}
	error = ext4_fscrypt_policy(fs, inode, &policy);
	if (error != EXT4_OK) {
		return error;
	}
	/* Regular files encrypt contents; directories and symlinks encrypt names. */
	key->mode = type == EXT4_MODE_REGULAR ? policy.contents_mode : policy.filenames_mode;
	key->flags = policy.flags;
	error = fs->crypto.find_key(
	    fs->crypto.context, policy.version, policy.identifier, policy.identifier_size, &master);
	if (error == EXT4_NOT_FOUND) {
		return EXT4_ENCRYPTED;
	}
	if (error != EXT4_OK) {
		return error;
	}
	if (policy.version == EXT4_FSCRYPT_CONTEXT_V2) {
		ext4_copy(info, EXT4_FSCRYPT_HKDF_PREFIX, EXT4_FSCRYPT_HKDF_PREFIX_SIZE);
		info[EXT4_FSCRYPT_HKDF_PREFIX_SIZE] = EXT4_FSCRYPT_HKDF_PER_FILE_KEY;
		ext4_copy(info + EXT4_FSCRYPT_HKDF_PREFIX_SIZE + 1U, policy.nonce,
		    EXT4_FSCRYPT_NONCE_SIZE);
		info_size = sizeof(info);
	} else {
		ext4_copy(info, policy.nonce, EXT4_FSCRYPT_NONCE_SIZE);
		info_size = EXT4_FSCRYPT_NONCE_SIZE;
	}
	error = fs->crypto.derive_key(fs->crypto.context, master, policy.version, info, info_size,
	    key->mode == EXT4_FSCRYPT_MODE_AES_256_XTS ? EXT4_FSCRYPT_XTS_KEY_SIZE
						       : EXT4_FSCRYPT_CTS_KEY_SIZE,
	    &key->handle);
	fs->crypto.release_key(fs->crypto.context, master);
	if (error != EXT4_OK) {
		return error;
	}
	ext4_fscrypt_remember(fs, inode, key);
	return EXT4_OK;
}

void
ext4_fscrypt_forget(struct ext4_fs *fs)
{
	uint32_t index;

	for (index = 0; index < fs->fscrypt_key_count; index++) {
		fs->crypto.release_key(fs->crypto.context, fs->fscrypt_keys[index].handle);
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
    const uint8_t *cipher, size_t length, uint8_t *plain, size_t *plain_length)
{
	uint8_t iv[EXT4_FSCRYPT_IV_SIZE] = { 0 };
	enum ext4_result error;

	if (length < EXT4_FSCRYPT_NAME_MIN || length > EXT4_NAME_MAX) {
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
    const uint8_t *plain, size_t length, uint8_t *cipher, size_t *cipher_length)
{
	uint8_t padded[EXT4_NAME_MAX];
	uint8_t iv[EXT4_FSCRYPT_IV_SIZE] = { 0 };
	size_t padding = (size_t)EXT4_FSCRYPT_PAD_BASE << (key->flags & EXT4_FSCRYPT_PAD_MASK);
	size_t size = length < EXT4_FSCRYPT_NAME_MIN ? EXT4_FSCRYPT_NAME_MIN : length;

	if (length == 0 || length > EXT4_NAME_MAX) {
		return EXT4_NAME_TOO_LONG;
	}
	size = (size + padding - 1U) / padding * padding;
	if (size > EXT4_NAME_MAX) {
		size = EXT4_NAME_MAX;
	}
	ext4_zero(padded, sizeof(padded));
	ext4_copy(padded, plain, length);
	*cipher_length = size;
	return fs->crypto.cipher(
	    fs->crypto.context, key->handle, key->mode, true, iv, padded, cipher, size);
}

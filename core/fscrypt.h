/* SPDX-License-Identifier: BSD-3-Clause */
#ifndef MACHLIN_EXT4_FSCRYPT_H
#define MACHLIN_EXT4_FSCRYPT_H

#include "internal.h"

/* fscrypt as Linux stores it on ext4: an encryption context in attribute index 9,
 * name "c", holds the policy and a per-inode nonce. Version 2 contexts name the master
 * key by a 16-byte identifier and derive per-file keys with HKDF-SHA512; version 1
 * contexts name it by an 8-byte descriptor and derive with AES-128-ECB. */
#define EXT4_FSCRYPT_CONTEXT_NAME "c"
#define EXT4_FSCRYPT_CONTEXT_NAME_SIZE 1U
#define EXT4_FSCRYPT_CONTEXT_V1 1U
#define EXT4_FSCRYPT_CONTEXT_V2 2U
#define EXT4_FSCRYPT_DESCRIPTOR_SIZE 8U
#define EXT4_FSCRYPT_IDENTIFIER_SIZE 16U
#define EXT4_FSCRYPT_NONCE_SIZE 16U
#define EXT4_FSCRYPT_IV_SIZE 16U
/* Policy flags: name padding of 4 << (flags & PAD_MASK) bytes. Other flags select
 * keys and IVs by inode number or share one key per mode; they are unsupported. */
#define EXT4_FSCRYPT_PAD_MASK 0x03U
#define EXT4_FSCRYPT_PAD_BASE 4U
#define EXT4_FSCRYPT_XTS_KEY_SIZE 64U
#define EXT4_FSCRYPT_CTS_KEY_SIZE 32U
/* Names shorter than one cipher block are padded to it. */
#define EXT4_FSCRYPT_NAME_MIN 16U
/* HKDF info: "fscrypt" with its NUL, a context byte, then context-specific data. */
#define EXT4_FSCRYPT_HKDF_PREFIX "fscrypt"
#define EXT4_FSCRYPT_HKDF_PREFIX_SIZE 8U
#define EXT4_FSCRYPT_HKDF_PER_FILE_KEY 2U
/* An encrypted symlink stores its ciphertext's length before it. */
#define EXT4_FSCRYPT_SYMLINK_HEADER 2U

struct ext4_fscrypt_context_v1_disk {
	uint8_t version;
	uint8_t contents_mode;
	uint8_t filenames_mode;
	uint8_t flags;
	uint8_t descriptor[EXT4_FSCRYPT_DESCRIPTOR_SIZE];
	uint8_t nonce[EXT4_FSCRYPT_NONCE_SIZE];
};

struct ext4_fscrypt_context_v2_disk {
	uint8_t version;
	uint8_t contents_mode;
	uint8_t filenames_mode;
	uint8_t flags;
	uint8_t log2_data_unit_size;
	uint8_t reserved[3];
	uint8_t identifier[EXT4_FSCRYPT_IDENTIFIER_SIZE];
	uint8_t nonce[EXT4_FSCRYPT_NONCE_SIZE];
};

_Static_assert(sizeof(struct ext4_fscrypt_context_v1_disk) == 28, "fscrypt v1 context size");
_Static_assert(sizeof(struct ext4_fscrypt_context_v2_disk) == 40, "fscrypt v2 context size");

struct ext4_fscrypt_policy {
	uint8_t version;
	uint8_t contents_mode;
	uint8_t filenames_mode;
	uint8_t flags;
	uint8_t identifier[EXT4_FSCRYPT_IDENTIFIER_SIZE];
	uint8_t identifier_size;
	uint8_t nonce[EXT4_FSCRYPT_NONCE_SIZE];
};

/* An inode's derived key and what it encrypts: contents for regular files, names and
 * targets for directories and symlinks. */
struct ext4_fscrypt_key {
	void *handle;
	uint8_t mode;
	uint8_t flags;
};

/* Decode and admit an encrypted inode's policy; UNSUPPORTED for modes and flags the
 * core does not implement. */
enum ext4_result ext4_fscrypt_policy(
    struct ext4_fs *fs, const struct ext4_inode *inode, struct ext4_fscrypt_policy *policy);
/* The inode's key from the mount's cache or the adapter; ENCRYPTED without the
 * adapter's master key. The mount owns the handle. */
enum ext4_result ext4_fscrypt_key(
    struct ext4_fs *fs, const struct ext4_inode *inode, struct ext4_fscrypt_key *key);
/* Release every cached key. */
void ext4_fscrypt_forget(struct ext4_fs *fs);
/* Decrypt or encrypt one filesystem block of contents at a logical block. */
enum ext4_result ext4_fscrypt_block(struct ext4_fs *fs, const struct ext4_fscrypt_key *key,
    uint64_t logical, bool encrypt, const void *input, void *output);
/* Decrypt a stored name into plaintext without its padding; plain holds 255 bytes. */
enum ext4_result ext4_fscrypt_name_decrypt(struct ext4_fs *fs, const struct ext4_fscrypt_key *key,
    const uint8_t *cipher, size_t length, uint8_t *plain, size_t *plain_length);
/* Pad and encrypt a name as it is stored; cipher holds 255 bytes. */
enum ext4_result ext4_fscrypt_name_encrypt(struct ext4_fs *fs, const struct ext4_fscrypt_key *key,
    const uint8_t *plain, size_t length, uint8_t *cipher, size_t *cipher_length);
/* Whether a directory entry name is the unencrypted "." or "..". */
bool ext4_fscrypt_dot(const uint8_t *name, size_t length);

#endif

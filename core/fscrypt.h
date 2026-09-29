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
/* Linux's no-key names encode, with base64url and no padding, the name's two
 * little-endian 32-bit dirhash words, up to 149 bytes of ciphertext and, for longer
 * ciphertext, the SHA-256 of the rest: at most 189 bytes, or 252 characters. */
#define EXT4_FSCRYPT_NOKEY_HASHES 8U
#define EXT4_FSCRYPT_NOKEY_BYTES 149U
#define EXT4_FSCRYPT_NOKEY_MAX (EXT4_FSCRYPT_NOKEY_HASHES + EXT4_FSCRYPT_NOKEY_BYTES + 32U)
#define EXT4_FSCRYPT_NOKEY_NAME_MAX 252U

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

/* A decoded no-key name: the full ciphertext, or its prefix and the rest's SHA-256. */
struct ext4_fscrypt_nokey {
	uint8_t bytes[EXT4_FSCRYPT_NOKEY_MAX];
	size_t size;
};

struct ext4_allocation;

/* Decode and admit an encrypted inode's policy; UNSUPPORTED for modes and flags the
 * core does not implement. */
enum ext4_result ext4_fscrypt_policy(
    struct ext4_fs *fs, const struct ext4_inode *inode, struct ext4_fscrypt_policy *policy);
/* Derive the key of an inode of type under policy, which includes its nonce;
 * ENCRYPTED without the adapter's master key. The caller releases the handle. */
enum ext4_result ext4_fscrypt_derive(struct ext4_fs *fs, const struct ext4_fscrypt_policy *policy,
    uint32_t type, struct ext4_fscrypt_key *key);
/* A committed inode's key from the mount's cache or the adapter; ENCRYPTED without the
 * adapter's master key. The mount owns the handle. */
enum ext4_result ext4_fscrypt_key(
    struct ext4_fs *fs, const struct ext4_inode *inode, struct ext4_fscrypt_key *key);
/* Release every cached key. */
void ext4_fscrypt_forget(struct ext4_fs *fs);
/* Decrypt or encrypt one filesystem block of contents at a logical block. */
enum ext4_result ext4_fscrypt_block(struct ext4_fs *fs, const struct ext4_fscrypt_key *key,
    uint64_t logical, bool encrypt, const void *input, void *output);
/* Decrypt a stored name or target of at most maximum bytes into plaintext without
 * its padding; plain holds length bytes. */
enum ext4_result ext4_fscrypt_name_decrypt(struct ext4_fs *fs, const struct ext4_fscrypt_key *key,
    const uint8_t *cipher, size_t length, size_t maximum, uint8_t *plain, size_t *plain_length);
/* Pad a name or target to the policy's padding, at least 16 and at most maximum bytes,
 * and encrypt it as it is stored; padded and cipher hold maximum bytes. */
enum ext4_result ext4_fscrypt_name_encrypt(struct ext4_fs *fs, const struct ext4_fscrypt_key *key,
    const uint8_t *plain, size_t length, size_t maximum, uint8_t *padded, uint8_t *cipher,
    size_t *cipher_length);
/* The stored form of an encrypted symlink target in a block-sized buffer: the
 * ciphertext's little-endian 16-bit length, the ciphertext and a NUL. stored_length is
 * the symlink's size, which excludes the NUL. */
enum ext4_result ext4_fscrypt_symlink_encrypt(struct ext4_fs *fs,
    const struct ext4_fscrypt_key *key, const uint8_t *target, size_t length, uint8_t *stored,
    size_t *stored_length);
/* Encode stored ciphertext as Linux's no-key name into name, which holds 252 bytes. */
size_t ext4_fscrypt_nokey_encode(
    const uint8_t *cipher, size_t length, uint32_t hash, uint32_t minor_hash, uint8_t *name);
/* Decode a no-key name; false for names that no stored name could have produced. */
bool ext4_fscrypt_nokey_decode(
    const uint8_t *name, size_t length, struct ext4_fscrypt_nokey *nokey);
/* Whether stored ciphertext is the one a decoded no-key name identifies. */
bool ext4_fscrypt_nokey_match(
    const struct ext4_fscrypt_nokey *nokey, const uint8_t *cipher, size_t length);
/* The full ciphertext a no-key name carries, or NULL when it holds only a prefix. */
const uint8_t *ext4_fscrypt_nokey_cipher(const struct ext4_fscrypt_nokey *nokey, size_t *length);
/* Scan a directory for the entry a no-key name identifies, reporting its stored
 * ciphertext, of at most 255 bytes, and inode; NOT_FOUND without one. */
enum ext4_result ext4_directory_nokey_find(struct ext4_fs *fs, const struct ext4_inode *directory,
    const struct ext4_fscrypt_nokey *nokey, uint8_t *cipher, size_t *cipher_length,
    uint32_t *number);
/* Whether a directory entry name is the unencrypted "." or "..". */
bool ext4_fscrypt_dot(const uint8_t *name, size_t length);
bool ext4_fscrypt_policy_equal(
    const struct ext4_fscrypt_policy *left, const struct ext4_fscrypt_policy *right);
/* Give a new regular file, directory or symlink in an encrypted directory the
 * directory's policy with a nonce of its own, reported in policy. Special files stay
 * unencrypted. */
enum ext4_result ext4_fscrypt_inherit(struct ext4_allocation *allocation,
    const struct ext4_inode *parent, struct ext4_inode *child, struct ext4_inode_disk *disk,
    struct ext4_fscrypt_policy *policy);
/* Whether an inode may have a name in a directory: an encrypted directory holds only
 * special files and objects of its own policy; otherwise CROSS_POLICY. */
enum ext4_result ext4_fscrypt_permitted(
    struct ext4_fs *fs, const struct ext4_inode *directory, const struct ext4_inode *inode);

#endif

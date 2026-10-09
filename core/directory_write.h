/* SPDX-License-Identifier: BSD-3-Clause */
#ifndef MACHLIN_EXT4_DIRECTORY_WRITE_H
#define MACHLIN_EXT4_DIRECTORY_WRITE_H

#include "allocate.h"
#include "unicode.h"

struct ext4_fscrypt_nokey;

enum ext4_directory_identity {
	EXT4_NAME_PLAIN,
	EXT4_NAME_KEYED,
	EXT4_NAME_NOKEY_QUERY,
	EXT4_NAME_NOKEY_RESOLVED,
	EXT4_NAME_REPLAY
};

enum ext4_directory_prepare {
	EXT4_NAME_REQUIRE_KEY,
	EXT4_NAME_ALLOW_NOKEY_REMOVAL,
	EXT4_NAME_LOGGED
};

/* Operation-owned comparison and disk identities. cipher is caller-owned storage
 * of NAME_MAX bytes; compare.folds and nokey are released by request_close.
 * No borrowed crypto handle survives preparation or a candidate comparison. */
struct ext4_directory_request {
	const struct ext4_inode *directory;
	struct ext4_directory_name compare;
	const uint8_t *disk_name;
	size_t disk_length;
	uint8_t *cipher;
	struct ext4_fscrypt_nokey *nokey;
	struct ext4_name_hash hash;
	uint32_t resolved_number;
	enum ext4_directory_identity identity;
	bool hash_ready;
	bool hash_filter;
};

enum ext4_result ext4_directory_request_open(struct ext4_fs *fs,
    const struct ext4_inode *directory, const uint8_t *name, size_t length,
    enum ext4_directory_prepare prepare, uint8_t *cipher,
    struct ext4_directory_request *request);
void ext4_directory_request_close(struct ext4_fs *fs, struct ext4_directory_request *request);
bool ext4_directory_request_probe(const struct ext4_directory_request *request);
enum ext4_result ext4_directory_request_hash(struct ext4_fs *fs,
    struct ext4_directory_request *request, uint8_t version, const uint32_t seed[4],
    struct ext4_name_hash *hash);
enum ext4_result ext4_directory_request_match(struct ext4_fs *fs,
    struct ext4_directory_request *request, const uint8_t *name, size_t length,
    const struct ext4_name_hash *stored, bool *match);

struct ext4_directory_slot {
	struct ext4_inode_disk *inline_disk;
	uint32_t inline_tail;
	uint32_t inline_hash;
	uint64_t physical;
	uint32_t logical;
	uint32_t offset;
	uint32_t used;
	uint32_t length;
	uint32_t previous;
	uint32_t number;
	bool repack;
	enum ext4_file_type type;
};

enum ext4_directory_action { EXT4_DIRECTORY_INSERT, EXT4_DIRECTORY_FIND, EXT4_DIRECTORY_EMPTY };

/* INSERT returning EXT4_EXISTS reports the matching inode in slot->number.
 * Linear directories and EMPTY validate every record. Indexed FIND and INSERT
 * validate the complete map and index graph, then every record in the root and
 * in each leaf whose hash interval contains the name. */
enum ext4_result ext4_directory_scan(struct ext4_allocation *allocation, struct ext4_inode *parent,
    struct ext4_inode_disk *disk, struct ext4_directory_request *request,
    enum ext4_directory_action action, uint32_t expected_parent, struct ext4_directory_slot *slot);
enum ext4_result ext4_directory_insert(struct ext4_allocation *allocation,
    const struct ext4_inode *parent, struct ext4_inode_disk *disk, struct ext4_directory_slot *slot,
    uint32_t number, enum ext4_file_type type, struct ext4_directory_request *request);
/* Validate an empty old layout and stage its format transition. May expand inline
 * storage, updating inode/disk allocation flags; caller merges only its flag delta. */
enum ext4_result ext4_directory_change_format(struct ext4_allocation *allocation,
    struct ext4_inode *inode, struct ext4_inode_disk *disk, uint32_t desired_flags);
enum ext4_result ext4_directory_initialize(struct ext4_allocation *allocation,
    struct ext4_inode *inode, struct ext4_inode_disk *disk, uint32_t parent);
enum ext4_result ext4_directory_remove(struct ext4_allocation *allocation,
    const struct ext4_inode *parent, const struct ext4_directory_slot *slot);
enum ext4_result ext4_directory_replace(struct ext4_allocation *allocation,
    const struct ext4_inode *parent, const struct ext4_directory_slot *slot, uint32_t number,
    enum ext4_file_type type);

#endif

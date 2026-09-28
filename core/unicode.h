/* SPDX-License-Identifier: BSD-3-Clause */
#ifndef MACHLIN_EXT4_UNICODE_H
#define MACHLIN_EXT4_UNICODE_H

#include "internal.h"

/* ext4's utf8-12.1 encoding (s_encoding 1). Casefolding applies full case
 * folding, canonical decomposition and canonical ordering and removes default
 * ignorable code points; code points without Unicode 12.1 data map to themselves.
 * Malformed UTF-8 or a surrogate makes a name opaque: it is then hashed and
 * compared as raw bytes, and strict mode rejects it. */
#define EXT4_UNICODE_EXPANSION 4U
#define EXT4_CASEFOLD_POINTS (EXT4_NAME_MAX * EXT4_UNICODE_EXPANSION)
#define EXT4_UTF8_MAX_BYTES 4U
#define EXT4_CASEFOLD_BYTES (EXT4_CASEFOLD_POINTS * EXT4_UTF8_MAX_BYTES)

struct ext4_unicode_range {
	uint32_t first;
	uint32_t last;
};

struct ext4_unicode_class {
	uint32_t first;
	uint32_t last;
	uint8_t value;
};

struct ext4_unicode_mapping {
	uint32_t code;
	uint16_t offset;
	uint8_t length;
};

/* Working storage for one folded name; callers allocate it from the environment. */
struct ext4_casefold {
	uint32_t points[EXT4_CASEFOLD_POINTS];
	uint8_t classes[EXT4_CASEFOLD_POINTS];
	uint8_t bytes[EXT4_CASEFOLD_BYTES];
	size_t length;
};

/* A name as one directory compares and hashes it. In a casefolded directory a
 * well-formed name uses its folded form and opaque names hash their raw bytes. As
 * in Linux, a relaxed encoding matches an opaque name only by its exact bytes and
 * a strict encoding never matches one. folds holds the key's fold and scratch
 * space for entries; it is NULL for other directories. */
struct ext4_directory_name {
	const uint8_t *name;
	size_t length;
	struct ext4_casefold *folds;
	bool folded;
	bool strict;
};

bool ext4_directory_casefolded(const struct ext4_fs *fs, const struct ext4_inode *directory);
enum ext4_result ext4_directory_name_open(struct ext4_fs *fs, const struct ext4_inode *directory,
    const uint8_t *name, size_t length, struct ext4_directory_name *key);
void ext4_directory_name_close(struct ext4_fs *fs, struct ext4_directory_name *key);
/* Hash name, which may be the key's own name or a stored entry. */
enum ext4_result ext4_directory_name_hash(struct ext4_directory_name *key, uint8_t version,
    const uint32_t seed[4], const uint8_t *name, size_t length, struct ext4_name_hash *hash);
bool ext4_directory_name_match(
    struct ext4_directory_name *key, const uint8_t *entry, size_t length);

/* OK stores the folded UTF-8 name. INVALID_ARGUMENT reports an opaque name. */
enum ext4_result ext4_casefold_name(struct ext4_casefold *fold, const uint8_t *name, size_t length);
/* Strict-mode names must be well-formed UTF-8 without surrogates. */
bool ext4_utf8_name_valid(const uint8_t *name, size_t length);

#endif

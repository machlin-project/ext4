/* SPDX-License-Identifier: BSD-3-Clause */
#include "unicode.h"
#include "unicode_data.h"

#define EXT4_HANGUL_FIRST 0xac00U
#define EXT4_HANGUL_LAST 0xd7a3U
#define EXT4_HANGUL_LEADING 0x1100U
#define EXT4_HANGUL_VOWEL 0x1161U
#define EXT4_HANGUL_TRAILING 0x11a7U
#define EXT4_HANGUL_VOWELS 21U
#define EXT4_HANGUL_TRAILS 28U
#define EXT4_UNICODE_MAX 0x10ffffU
#define EXT4_SURROGATE_FIRST 0xd800U
#define EXT4_SURROGATE_LAST 0xdfffU
/* A removed ignorable code point still has class 0 and ends a reordering run. */
#define EXT4_UNICODE_BARRIER UINT32_MAX

/* Each input code point uses at least one byte and yields at most one mapping. */
_Static_assert(EXT4_UNICODE_MAPPING_MAX <= EXT4_UNICODE_EXPANSION, "casefold expansion bound");
_Static_assert(EXT4_CASEFOLD_BYTES <= EXT4_DIRECTORY_HASH_MAX, "casefolded hash input bound");

/* Decode one UTF-8 scalar value, rejecting overlong forms and surrogates. */
static size_t
ext4_utf8_decode(const uint8_t *bytes, size_t length, uint32_t *code)
{
	static const uint32_t minimum[5] = { 0, 0, 0x80U, 0x800U, 0x10000U };
	uint32_t value;
	size_t count;
	size_t index;

	if (length == 0) {
		return 0;
	}
	if (bytes[0] < 0x80U) {
		*code = bytes[0];
		return 1;
	}
	count = (bytes[0] & 0xe0U) == 0xc0U ? 2U
	    : (bytes[0] & 0xf0U) == 0xe0U   ? 3U
	    : (bytes[0] & 0xf8U) == 0xf0U   ? 4U
					    : 0;
	if (count == 0 || count > length) {
		return 0;
	}
	value = bytes[0] & (0x7fU >> count);
	for (index = 1; index < count; index++) {
		if ((bytes[index] & 0xc0U) != 0x80U) {
			return 0;
		}
		value = value << 6 | (bytes[index] & 0x3fU);
	}
	if (value < minimum[count] || value > EXT4_UNICODE_MAX ||
	    (value >= EXT4_SURROGATE_FIRST && value <= EXT4_SURROGATE_LAST)) {
		return 0;
	}
	*code = value;
	return count;
}

static size_t
ext4_utf8_encode(uint32_t code, uint8_t *output)
{
	if (code < 0x80U) {
		output[0] = (uint8_t)code;
		return 1;
	}
	if (code < 0x800U) {
		output[0] = (uint8_t)(0xc0U | code >> 6);
		output[1] = (uint8_t)(0x80U | (code & 0x3fU));
		return 2;
	}
	if (code < 0x10000U) {
		output[0] = (uint8_t)(0xe0U | code >> 12);
		output[1] = (uint8_t)(0x80U | (code >> 6 & 0x3fU));
		output[2] = (uint8_t)(0x80U | (code & 0x3fU));
		return 3;
	}
	output[0] = (uint8_t)(0xf0U | code >> 18);
	output[1] = (uint8_t)(0x80U | (code >> 12 & 0x3fU));
	output[2] = (uint8_t)(0x80U | (code >> 6 & 0x3fU));
	output[3] = (uint8_t)(0x80U | (code & 0x3fU));
	return 4;
}

static bool
ext4_unicode_ignorable_code(uint32_t code)
{
	size_t low = 0;
	size_t high = EXT4_UNICODE_IGNORABLE_COUNT;
	size_t middle;

	while (low < high) {
		middle = low + (high - low) / 2U;
		if (ext4_unicode_ignorable[middle].last < code) {
			low = middle + 1U;
		} else if (ext4_unicode_ignorable[middle].first > code) {
			high = middle;
		} else {
			return true;
		}
	}
	return false;
}

static uint8_t
ext4_unicode_class_of(uint32_t code)
{
	size_t low = 0;
	size_t high = EXT4_UNICODE_CLASS_COUNT;
	size_t middle;

	while (low < high) {
		middle = low + (high - low) / 2U;
		if (ext4_unicode_classes[middle].last < code) {
			low = middle + 1U;
		} else if (ext4_unicode_classes[middle].first > code) {
			high = middle;
		} else {
			return ext4_unicode_classes[middle].value;
		}
	}
	return 0;
}

static const struct ext4_unicode_mapping *
ext4_unicode_mapping_of(uint32_t code)
{
	size_t low = 0;
	size_t high = EXT4_UNICODE_MAPPING_COUNT;
	size_t middle;

	while (low < high) {
		middle = low + (high - low) / 2U;
		if (ext4_unicode_mappings[middle].code < code) {
			low = middle + 1U;
		} else if (ext4_unicode_mappings[middle].code > code) {
			high = middle;
		} else {
			return &ext4_unicode_mappings[middle];
		}
	}
	return NULL;
}

static void
ext4_casefold_append(struct ext4_casefold *fold, size_t *count, uint32_t code)
{
	fold->points[*count] = code;
	fold->classes[*count] = ext4_unicode_class_of(code);
	(*count)++;
}

enum ext4_result
ext4_casefold_name(struct ext4_casefold *fold, const uint8_t *name, size_t length)
{
	const struct ext4_unicode_mapping *mapping;
	uint32_t code;
	uint32_t saved;
	uint32_t index;
	uint8_t saved_class;
	size_t offset = 0;
	size_t count = 0;
	size_t used;
	size_t position;
	size_t run;

	if (length > EXT4_NAME_MAX) {
		return EXT4_INVALID_ARGUMENT;
	}
	while (offset < length) {
		used = ext4_utf8_decode(name + offset, length - offset, &code);
		if (used == 0) {
			return EXT4_INVALID_ARGUMENT;
		}
		offset += used;
		if (ext4_unicode_ignorable_code(code)) {
			fold->points[count] = EXT4_UNICODE_BARRIER;
			fold->classes[count++] = 0;
			continue;
		}
		if (code >= EXT4_HANGUL_FIRST && code <= EXT4_HANGUL_LAST) {
			index = code - EXT4_HANGUL_FIRST;
			ext4_casefold_append(fold, &count,
			    EXT4_HANGUL_LEADING +
				index / (EXT4_HANGUL_VOWELS * EXT4_HANGUL_TRAILS));
			ext4_casefold_append(fold, &count,
			    EXT4_HANGUL_VOWEL +
				index % (EXT4_HANGUL_VOWELS * EXT4_HANGUL_TRAILS) /
				    EXT4_HANGUL_TRAILS);
			if (index % EXT4_HANGUL_TRAILS != 0) {
				ext4_casefold_append(fold, &count,
				    EXT4_HANGUL_TRAILING + index % EXT4_HANGUL_TRAILS);
			}
			continue;
		}
		mapping = ext4_unicode_mapping_of(code);
		if (mapping == NULL) {
			ext4_casefold_append(fold, &count, code);
			continue;
		}
		for (index = 0; index < mapping->length; index++) {
			ext4_casefold_append(
			    fold, &count, ext4_unicode_pool[mapping->offset + index]);
		}
	}
	/* Canonical ordering: a stable insertion sort of each run of non-starters. */
	for (position = 1; position < count; position++) {
		saved = fold->points[position];
		saved_class = fold->classes[position];
		if (saved_class == 0) {
			continue;
		}
		for (run = position; run > 0 && fold->classes[run - 1U] > saved_class; run--) {
			fold->points[run] = fold->points[run - 1U];
			fold->classes[run] = fold->classes[run - 1U];
		}
		fold->points[run] = saved;
		fold->classes[run] = saved_class;
	}
	fold->length = 0;
	for (position = 0; position < count; position++) {
		if (fold->points[position] != EXT4_UNICODE_BARRIER) {
			fold->length +=
			    ext4_utf8_encode(fold->points[position], fold->bytes + fold->length);
		}
	}
	return EXT4_OK;
}

bool
ext4_utf8_name_valid(const uint8_t *name, size_t length)
{
	uint32_t code;
	size_t offset = 0;
	size_t used;

	while (offset < length) {
		used = ext4_utf8_decode(name + offset, length - offset, &code);
		if (used == 0) {
			return false;
		}
		offset += used;
	}
	return true;
}

bool
ext4_directory_casefolded(const struct ext4_fs *fs, const struct ext4_inode *directory)
{
	return (fs->info.feature_incompat & EXT4_FEATURE_INCOMPAT_CASEFOLD) &&
	    (directory->flags & EXT4_INODE_CASEFOLD);
}

enum ext4_result
ext4_directory_name_open(struct ext4_fs *fs, const struct ext4_inode *directory,
    const uint8_t *name, size_t length, struct ext4_directory_name *key)
{
	ext4_zero(key, sizeof(*key));
	key->name = name;
	key->length = length;
	if (!ext4_directory_casefolded(fs, directory)) {
		return EXT4_OK;
	}
	key->folds = fs->environment.allocate(fs->environment.context, 2U * sizeof(*key->folds));
	if (key->folds == NULL) {
		return EXT4_NO_MEMORY;
	}
	key->folded = name != NULL && ext4_casefold_name(&key->folds[0], name, length) == EXT4_OK;
	key->strict = fs->casefold_strict;
	return EXT4_OK;
}

void
ext4_directory_name_close(struct ext4_fs *fs, struct ext4_directory_name *key)
{
	if (key->folds != NULL) {
		fs->environment.release(
		    fs->environment.context, key->folds, 2U * sizeof(*key->folds));
		key->folds = NULL;
	}
}

enum ext4_result
ext4_directory_name_hash(struct ext4_directory_name *key, uint8_t version, const uint32_t seed[4],
    const uint8_t *name, size_t length, struct ext4_name_hash *hash)
{
	if (key->folds != NULL && ext4_casefold_name(&key->folds[1], name, length) == EXT4_OK) {
		return ext4_directory_hash(
		    version, seed, key->folds[1].bytes, key->folds[1].length, hash);
	}
	return ext4_directory_hash(version, seed, name, length, hash);
}

bool
ext4_directory_name_match(struct ext4_directory_name *key, const uint8_t *entry, size_t length)
{
	bool exact = length == key->length && ext4_equal(entry, key->name, length);

	if (key->folds == NULL) {
		return exact;
	}
	if (!key->folded) {
		return exact && !key->strict;
	}
	return exact ||
	    (ext4_casefold_name(&key->folds[1], entry, length) == EXT4_OK &&
		key->folds[1].length == key->folds[0].length &&
		ext4_equal(key->folds[1].bytes, key->folds[0].bytes, key->folds[0].length));
}

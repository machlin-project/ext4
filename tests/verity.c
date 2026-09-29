/* SPDX-License-Identifier: BSD-3-Clause */
#define _POSIX_C_SOURCE 200809L
#include "journal.h"
#include "image.h"
#include "sha.h"
#include "verity.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define CHECK(expression)                                                                          \
	do {                                                                                       \
		if (!(expression)) {                                                               \
			fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #expression);           \
			exit(1);                                                                   \
		}                                                                                  \
	} while (0)
#define EXPECT(expression, expected)                                                               \
	do {                                                                                       \
		enum ext4_result actual = (expression);                                            \
		if (actual != (expected)) {                                                        \
			fprintf(stderr, "%s:%d: %s: %s, expected %s\n", __FILE__, __LINE__,        \
			    #expression, ext4_result_string(actual),                               \
			    ext4_result_string(expected));                                         \
			exit(1);                                                                   \
		}                                                                                  \
	} while (0)

/* Verified reads of independently authored fs-verity files. The manifest names
 * each file, its size, the SHA-256 of its contents and, for damaged files, the
 * Merkle data block that must fail or "all" when the descriptor is invalid. */

#define MANIFEST_LINE 512U
#define CHUNK_BYTES 6000U
#define NAME_BYTES 64U
#define ENTRY_LIMIT 16U
#define COPY_BYTES (1024U * 1024U)
#define PROTECTED_PERMISSIONS 0400U
#define VERITY_SECONDS 1700005000
#define ALIAS_NAME "verity-alias"
#define RENAMED_NAME "verity-renamed"
#define NOTE_NAME "note"
#define NOTE_VALUE "metadata remains mutable"

struct entry {
	char kind[16];
	char name[NAME_BYTES];
	uint64_t size;
	uint8_t sha256[EXT4_SHA256_DIGEST_SIZE];
	char sha256_text[2U * EXT4_SHA256_DIGEST_SIZE + 1U];
	char digest[2U * EXT4_SHA512_DIGEST_SIZE + 1U];
	char failure[16];
};

static void
hex_decode(const char *text, uint8_t *output, size_t length)
{
	unsigned int value;
	size_t index;

	CHECK(strlen(text) == length * 2U);
	for (index = 0; index < length; index++) {
		CHECK(sscanf(text + index * 2U, "%2x", &value) == 1);
		output[index] = (uint8_t)value;
	}
}

static void
known_answers(void)
{
	static const char *const sha256_abc =
	    "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad";
	static const char *const sha512_abc =
	    "ddaf35a193617abacc417349ae20413112e6fa4e89a97ea20a9eeee64b55d39a"
	    "2192992a274fc1a836ba3c23a3feebbd454d4423643ce80e2a9ac94fa54ca49f";
	static const char *const sha256_long =
	    "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1";
	static const char *const sha512_long =
	    "8e959b75dae313da8cf4f72814fc143f8f7779c6eb9f7fa17299aeadb6889018"
	    "501d289e4900f7e4331b99dec4b5433ac7d329eeb6dd26545e96e55b874be909";
	static const char *const long256 =
	    "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
	static const char *const long512 =
	    "abcdefghbcdefghicdefghijdefghijkefghijklfghijklmghijklmnhijklmno"
	    "ijklmnopjklmnopqklmnopqrlmnopqrsmnopqrstnopqrstu";
	struct ext4_sha256 sha256;
	struct ext4_sha512 sha512;
	uint8_t digest[EXT4_SHA512_DIGEST_SIZE];
	uint8_t expected[EXT4_SHA512_DIGEST_SIZE];
	uint8_t split[EXT4_SHA512_DIGEST_SIZE];
	size_t cut;

	ext4_sha256_init(&sha256);
	ext4_sha256_update(&sha256, "abc", 3);
	ext4_sha256_final(&sha256, digest);
	hex_decode(sha256_abc, expected, EXT4_SHA256_DIGEST_SIZE);
	CHECK(memcmp(digest, expected, EXT4_SHA256_DIGEST_SIZE) == 0);
	ext4_sha512_init(&sha512);
	ext4_sha512_update(&sha512, "abc", 3);
	ext4_sha512_final(&sha512, digest);
	hex_decode(sha512_abc, expected, EXT4_SHA512_DIGEST_SIZE);
	CHECK(memcmp(digest, expected, EXT4_SHA512_DIGEST_SIZE) == 0);
	/* Every split of the two-block messages yields the same digest. */
	for (cut = 0; cut <= strlen(long256); cut++) {
		ext4_sha256_init(&sha256);
		ext4_sha256_update(&sha256, long256, cut);
		ext4_sha256_update(&sha256, long256 + cut, strlen(long256) - cut);
		ext4_sha256_final(&sha256, split);
		hex_decode(sha256_long, expected, EXT4_SHA256_DIGEST_SIZE);
		CHECK(memcmp(split, expected, EXT4_SHA256_DIGEST_SIZE) == 0);
	}
	for (cut = 0; cut <= strlen(long512); cut++) {
		ext4_sha512_init(&sha512);
		ext4_sha512_update(&sha512, long512, cut);
		ext4_sha512_update(&sha512, long512 + cut, strlen(long512) - cut);
		ext4_sha512_final(&sha512, split);
		hex_decode(sha512_long, expected, EXT4_SHA512_DIGEST_SIZE);
		CHECK(memcmp(split, expected, EXT4_SHA512_DIGEST_SIZE) == 0);
	}
	puts("PASS SHA-256 and SHA-512 known answers and split updates");
}

static struct ext4_inode
find(struct ext4_fs *fs, const char *name)
{
	struct ext4_inode root;
	struct ext4_inode inode;

	EXPECT(ext4_get_inode(fs, EXT4_ROOT_INODE, &root), EXT4_OK);
	EXPECT(ext4_lookup(fs, &root, (const uint8_t *)name, strlen(name), &inode), EXT4_OK);
	return inode;
}

static enum ext4_result
read_all(struct ext4_fs *fs, const struct ext4_inode *inode, uint8_t *buffer, size_t *total)
{
	size_t completed;
	size_t length;
	enum ext4_result error;

	*total = 0;
	while (*total < inode->size) {
		length = inode->size - *total < CHUNK_BYTES ? (size_t)(inode->size - *total)
							    : CHUNK_BYTES;
		error = ext4_read(fs, inode, *total, buffer + *total, length, &completed);
		*total += completed;
		if (error != EXT4_OK) {
			return error;
		}
		CHECK(completed == length);
	}
	return EXT4_OK;
}

/* One request crosses Merkle and extent boundaries without recreating the read
 * context. Failed operations may publish only a byte-exact verified prefix. */
static void
verify_whole(struct ext4_posix_image *image, struct ext4_fs *fs, const struct ext4_inode *inode,
    const uint8_t *expected)
{
	uint8_t *buffer = malloc(inode->size + 1U);
	uint64_t allocations = image->allocation_calls;
	uint64_t reads = image->read_calls;
	uint64_t fault;
	size_t completed;
	enum ext4_result error;

	CHECK(buffer != NULL);
	EXPECT(ext4_read(fs, inode, 0, buffer, inode->size, &completed), EXT4_OK);
	CHECK(completed == inode->size && memcmp(buffer, expected, completed) == 0);
	allocations = image->allocation_calls - allocations;
	reads = image->read_calls - reads;
	for (fault = 1; fault <= allocations + (reads != 0 ? 3U : 0U); fault++) {
		memset(buffer, 0xcc, inode->size + 1U);
		if (fault <= allocations) {
			image->fail_allocation_at = image->allocation_calls + fault;
			error = EXT4_NO_MEMORY;
		} else {
			/* First, middle and last device read of the successful request. */
			image->fail_read_at =
			    image->read_calls + 1U + (reads - 1U) * (fault - allocations - 1U) / 2U;
			error = EXT4_IO;
		}
		EXPECT(ext4_read(fs, inode, 0, buffer, inode->size, &completed), error);
		CHECK(completed < inode->size && image->live_allocations == 1U);
		CHECK(memcmp(buffer, expected, completed) == 0 && buffer[completed] == 0xcc);
		image->fail_allocation_at = 0;
		image->fail_read_at = 0;
	}
	free(buffer);
}

static void
verify_good(struct ext4_posix_image *image, struct ext4_fs *fs, const struct entry *entry)
{
	struct ext4_inode inode = find(fs, entry->name);
	struct ext4_mapping mapping;
	struct ext4_sha256 sha256;
	uint8_t digest[EXT4_SHA256_DIGEST_SIZE];
	uint8_t *buffer;
	uint8_t *piece;
	uint64_t allocations;
	uint64_t reads;
	uint64_t fault;
	size_t total;
	size_t completed;
	uint64_t offset;
	enum ext4_result expected;

	CHECK(inode.size == entry->size && (inode.flags & EXT4_INODE_VERITY));
	buffer = malloc(inode.size + 1U);
	piece = malloc(CHUNK_BYTES);
	CHECK(buffer != NULL && piece != NULL);
	allocations = image->allocation_calls;
	reads = image->read_calls;
	EXPECT(read_all(fs, &inode, buffer, &total), EXT4_OK);
	allocations = image->allocation_calls - allocations;
	reads = image->read_calls - reads;
	ext4_sha256_init(&sha256);
	ext4_sha256_update(&sha256, buffer, total);
	ext4_sha256_final(&sha256, digest);
	CHECK(total == entry->size && memcmp(digest, entry->sha256, sizeof(digest)) == 0);
	verify_whole(image, fs, &inode, buffer);
	for (offset = 0; offset < inode.size; offset += inode.size / 7U + 1U) {
		EXPECT(ext4_read(fs, &inode, offset, piece, CHUNK_BYTES, &completed), EXT4_OK);
		CHECK(completed ==
			(inode.size - offset < CHUNK_BYTES ? inode.size - offset : CHUNK_BYTES) &&
		    memcmp(piece, buffer + offset, completed) == 0);
	}
	if (inode.size != 0) {
		EXPECT(ext4_map_read(fs, &inode, 0, 1, &mapping), EXT4_UNSUPPORTED);
	}
	/* Allocation and read failures return errors without leaks or partial lies. */
	for (fault = 1; fault <= allocations + reads; fault++) {
		if (fault <= allocations) {
			image->fail_allocation_at = image->allocation_calls + fault;
			expected = EXT4_NO_MEMORY;
		} else {
			image->fail_read_at = image->read_calls + fault - allocations;
			expected = EXT4_IO;
		}
		EXPECT(read_all(fs, &inode, buffer, &total), expected);
		CHECK(total < inode.size && image->live_allocations == 1U);
		image->fail_allocation_at = 0;
		image->fail_read_at = 0;
	}
	free(piece);
	free(buffer);
	printf("PASS verified %s: %" PRIu64 " bytes, %" PRIu64 " allocation and %" PRIu64
	       " read faults\n",
	    entry->name, entry->size, allocations, reads);
}

static void
verify_damaged(struct ext4_fs *fs, const struct entry *entry, uint32_t block_size)
{
	struct ext4_inode inode = find(fs, entry->name);
	uint8_t *buffer = malloc(inode.size);
	uint64_t block;
	size_t total;
	size_t completed;

	CHECK(buffer != NULL);
	if (strcmp(entry->failure, "all") == 0) {
		CHECK(ext4_read(fs, &inode, 0, buffer, 1, &completed) != EXT4_OK && completed == 0);
		CHECK(ext4_read(fs, &inode, inode.size - 1U, buffer, 1, &completed) != EXT4_OK);
	} else {
		block = strtoull(entry->failure, NULL, 10);
		EXPECT(
		    ext4_read(fs, &inode, block * block_size, buffer, 1, &completed), EXT4_CORRUPT);
		CHECK(completed == 0);
		/* A sequential read keeps the verified prefix before the damaged block. */
		EXPECT(read_all(fs, &inode, buffer, &total), EXT4_CORRUPT);
		CHECK(total == block * block_size);
		memset(buffer, 0xcc, inode.size);
		EXPECT(ext4_read(fs, &inode, 0, buffer, inode.size, &completed), EXT4_CORRUPT);
		CHECK(completed == block * block_size && buffer[completed] == 0xcc);
		/* The last block belongs to another verified level-zero hash block. */
		EXPECT(ext4_read(fs, &inode, inode.size - 1U, buffer, 1, &completed), EXT4_OK);
		CHECK(completed == 1);
	}
	free(buffer);
	printf("PASS rejected damaged %s at %s\n", entry->name, entry->failure);
}

static void
copy_image(const char *source, const char *destination)
{
	FILE *input = fopen(source, "rb");
	FILE *output = fopen(destination, "wb");
	uint8_t *buffer = malloc(COPY_BYTES);
	size_t count;

	CHECK(input != NULL && output != NULL && buffer != NULL);
	while ((count = fread(buffer, 1, COPY_BYTES, input)) != 0) {
		CHECK(fwrite(buffer, 1, count, output) == count);
	}
	CHECK(ferror(input) == 0 && fclose(input) == 0 && fclose(output) == 0);
	free(buffer);
}

static struct ext4_inode_update
data_update(const struct ext4_inode *inode)
{
	struct ext4_inode_update update = { 0 };

	update.fields = EXT4_ATTR_PERMISSIONS | EXT4_ATTR_MODIFY_TIME | EXT4_ATTR_CHANGE_TIME |
	    EXT4_ATTR_XATTRS;
	update.permissions = (uint16_t)(inode->mode & 07777U);
	update.modify_time.seconds = VERITY_SECONDS;
	update.change_time.seconds = VERITY_SECONDS;
	return update;
}

/* A writable owner may change names and attributes but never verity data or
 * the Merkle metadata beyond EOF; final deletion releases that metadata too. */
static void
writable(const char *source, const struct entry *entries, uint32_t count, uint32_t block_size,
    const char *exports)
{
	static const uint8_t byte = 'x';
	const struct entry *large = NULL;
	const struct entry *small = NULL;
	struct ext4_posix_image image;
	struct ext4_fs *fs;
	struct ext4_inode root;
	struct ext4_inode target;
	struct ext4_inode other;
	struct ext4_inode result;
	struct ext4_inode_update update;
	struct ext4_xattr_change note = { EXT4_XATTR_SET, EXT4_XATTR_USER,
		(const uint8_t *)NOTE_NAME, sizeof(NOTE_NAME) - 1U, NOTE_VALUE,
		sizeof(NOTE_VALUE) - 1U };
	struct ext4_rename_entry from;
	struct ext4_rename_entry to;
	struct ext4_timestamp time = { VERITY_SECONDS, 0 };
	struct ext4_sha256 sha256;
	struct ext4_info before;
	struct ext4_info after;
	uint8_t digest[EXT4_SHA256_DIGEST_SIZE];
	uint8_t value[sizeof(NOTE_VALUE)];
	uint8_t *buffer;
	uint64_t writes;
	uint64_t allocated;
	char path[4096];
	FILE *manifest;
	size_t completed;
	size_t total;
	uint32_t index;
	int length;
	int fd;

	for (index = 0; index < count; index++) {
		if (strcmp(entries[index].kind, "good") != 0 || entries[index].size == 0) {
			continue;
		}
		if (large == NULL || entries[index].size > large->size) {
			large = &entries[index];
		}
	}
	for (index = 0; index < count; index++) {
		if (strcmp(entries[index].kind, "good") == 0 && entries[index].size != 0 &&
		    &entries[index] != large &&
		    (small == NULL || entries[index].size < small->size)) {
			small = &entries[index];
		}
	}
	CHECK(large != NULL && small != NULL);
	if (exports != NULL) {
		length = snprintf(path, sizeof(path), "%s/verity-mutated.img", exports);
		CHECK(length > 0 && (size_t)length < sizeof(path));
	} else {
		length = snprintf(path, sizeof(path), "%s/ext4-verity-XXXXXX",
		    getenv("TMPDIR") != NULL ? getenv("TMPDIR") : "/tmp");
		CHECK(length > 0 && (size_t)length < sizeof(path));
		fd = mkstemp(path);
		CHECK(fd >= 0 && close(fd) == 0);
	}
	copy_image(source, path);
	EXPECT(ext4_posix_open_writable(&image, path), EXT4_OK);
	EXPECT(ext4_mount_writable(&image.environment, &image.writer, &fs), EXT4_OK);
	EXPECT(ext4_get_inode(fs, EXT4_ROOT_INODE, &root), EXT4_OK);
	target = find(fs, large->name);
	other = find(fs, small->name);
	update = data_update(&target);
	writes = image.write_calls;
	EXPECT(ext4_write(fs, target.number, target.generation, 0, &byte, 1, &update, &completed),
	    EXT4_PERMISSION_DENIED);
	EXPECT(ext4_write_partial(fs, target.number, target.generation, target.size, &byte, 1,
		   &update, &completed),
	    EXT4_PERMISSION_DENIED);
	EXPECT(ext4_truncate(fs, target.number, target.generation, 0, &update, &result),
	    EXT4_PERMISSION_DENIED);
	EXPECT(ext4_truncate(fs, target.number, target.generation, target.size + block_size,
		   &update, &result),
	    EXT4_PERMISSION_DENIED);
	EXPECT(ext4_truncate_atomic(
		   fs, target.number, target.generation, target.size / 2U, &update, &result),
	    EXT4_PERMISSION_DENIED);
	EXPECT(ext4_fallocate(fs, target.number, target.generation, 0, block_size,
		   EXT4_FALLOC_KEEP_SIZE, &update, &allocated),
	    EXT4_PERMISSION_DENIED);
	EXPECT(ext4_fallocate(fs, target.number, target.generation, 0, block_size,
		   EXT4_FALLOC_KEEP_SIZE | EXT4_FALLOC_PUNCH_HOLE, &update, &allocated),
	    EXT4_PERMISSION_DENIED);
	CHECK(image.write_calls == writes);

	update.fields = EXT4_ATTR_PERMISSIONS | EXT4_ATTR_CHANGE_TIME | EXT4_ATTR_XATTRS;
	update.permissions = PROTECTED_PERMISSIONS;
	update.xattrs = &note;
	update.xattr_count = 1;
	EXPECT(
	    ext4_set_attributes(fs, target.number, target.generation, &update, &result), EXT4_OK);
	EXPECT(ext4_link(fs, root.number, root.generation, (const uint8_t *)ALIAS_NAME,
		   strlen(ALIAS_NAME), target.number, target.generation, &time, &result),
	    EXT4_OK);
	from = (struct ext4_rename_entry){ root.number, root.generation,
		(const uint8_t *)ALIAS_NAME, strlen(ALIAS_NAME), target.number, target.generation };
	to = (struct ext4_rename_entry){ root.number, root.generation,
		(const uint8_t *)RENAMED_NAME, strlen(RENAMED_NAME), 0, 0 };
	EXPECT(ext4_rename(fs, &from, &to, 0, &time, &result), EXT4_OK);
	EXPECT(ext4_unlink(fs, root.number, root.generation, (const uint8_t *)large->name,
		   strlen(large->name), target.number, target.generation, &time, &result),
	    EXT4_OK);
	ext4_get_info(fs, &before);
	EXPECT(ext4_unlink(fs, root.number, root.generation, (const uint8_t *)small->name,
		   strlen(small->name), other.number, other.generation, &time, &result),
	    EXT4_OK);
	ext4_get_info(fs, &after);
	/* Deletion releases the data, tree, descriptor and size blocks. */
	CHECK(other.blocks_512 != 0 &&
	    after.free_blocks - before.free_blocks == other.blocks_512 * 512U / block_size);
	EXPECT(ext4_sync(fs), EXT4_OK);
	ext4_unmount(fs);
	CHECK(image.live_allocations == 0);
	ext4_posix_close(&image);

	EXPECT(ext4_posix_open(&image, path), EXT4_OK);
	EXPECT(ext4_mount(&image.environment, &fs), EXT4_OK);
	result = find(fs, RENAMED_NAME);
	CHECK(result.number == target.number && (result.flags & EXT4_INODE_VERITY) &&
	    result.links == 1 && (result.mode & 07777U) == PROTECTED_PERMISSIONS);
	buffer = malloc(result.size + 1U);
	CHECK(buffer != NULL);
	EXPECT(read_all(fs, &result, buffer, &total), EXT4_OK);
	ext4_sha256_init(&sha256);
	ext4_sha256_update(&sha256, buffer, total);
	ext4_sha256_final(&sha256, digest);
	CHECK(memcmp(digest, large->sha256, sizeof(digest)) == 0);
	free(buffer);
	EXPECT(ext4_get_xattr(fs, result.number, result.generation, EXT4_XATTR_USER,
		   (const uint8_t *)NOTE_NAME, sizeof(NOTE_NAME) - 1U, value, sizeof(value),
		   &completed),
	    EXT4_OK);
	CHECK(completed == sizeof(NOTE_VALUE) - 1U && memcmp(value, NOTE_VALUE, completed) == 0);
	EXPECT(ext4_lookup(fs, &root, (const uint8_t *)small->name, strlen(small->name), &other),
	    EXT4_NOT_FOUND);
	ext4_unmount(fs);
	CHECK(image.live_allocations == 0);
	ext4_posix_close(&image);
	if (exports == NULL) {
		CHECK(unlink(path) == 0);
	} else {
		length = snprintf(path, sizeof(path), "%s/verity-mutated.manifest", exports);
		CHECK(length > 0 && (size_t)length < sizeof(path));
		manifest = fopen(path, "wx");
		CHECK(manifest != NULL);
		fprintf(manifest, "algorithm 0 block %u\n", block_size);
		for (index = 0; index < count; index++) {
			if (&entries[index] == large || &entries[index] == small) {
				continue;
			}
			fprintf(manifest, "%s %s %" PRIu64 " %s %s %s\n", entries[index].kind,
			    entries[index].name, entries[index].size, entries[index].sha256_text,
			    entries[index].digest, entries[index].failure);
		}
		fprintf(manifest, "good %s %" PRIu64 " %s %s -\n", RENAMED_NAME, large->size,
		    large->sha256_text, large->digest);
		CHECK(fclose(manifest) == 0);
	}
	puts("PASS writable owner protects verity data and changes names and attributes");
}

int
main(int argc, char **argv)
{
	static struct entry entries[ENTRY_LIMIT];
	struct ext4_posix_image image;
	struct entry *entry;
	struct ext4_fs *fs;
	char line[MANIFEST_LINE];
	FILE *manifest;
	unsigned int algorithm;
	unsigned int block_size;
	uint32_t count = 0;
	uint32_t good = 0;
	uint32_t damaged = 0;
	uint32_t index;

	if (argc != 3 && argc != 4) {
		fprintf(stderr, "usage: %s VERITY_IMAGE MANIFEST [EXPORT_DIRECTORY]\n", argv[0]);
		return 2;
	}
	known_answers();
	manifest = fopen(argv[2], "r");
	CHECK(manifest != NULL && fgets(line, sizeof(line), manifest) != NULL);
	CHECK(sscanf(line, "algorithm %u block %u", &algorithm, &block_size) == 2);
	while (fgets(line, sizeof(line), manifest) != NULL) {
		CHECK(count < ENTRY_LIMIT);
		entry = &entries[count++];
		CHECK(
		    sscanf(line, "%15s %63s %" SCNu64 " %64s %128s %15s", entry->kind, entry->name,
			&entry->size, entry->sha256_text, entry->digest, entry->failure) == 6);
		hex_decode(entry->sha256_text, entry->sha256, sizeof(entry->sha256));
	}
	CHECK(fclose(manifest) == 0);
	EXPECT(ext4_posix_open(&image, argv[1]), EXT4_OK);
	EXPECT(ext4_mount(&image.environment, &fs), EXT4_OK);
	CHECK(fs->info.feature_ro_compat & EXT4_FEATURE_RO_VERITY);
	for (index = 0; index < count; index++) {
		if (strcmp(entries[index].kind, "good") == 0) {
			verify_good(&image, fs, &entries[index]);
			good++;
		} else {
			verify_damaged(fs, &entries[index], block_size);
			damaged++;
		}
	}
	ext4_unmount(fs);
	CHECK(image.live_allocations == 0 && image.write_calls == 0);
	ext4_posix_close(&image);
	writable(argv[1], entries, count, block_size, argc == 4 ? argv[3] : NULL);
	printf("PASS %u verified and %u damaged fs-verity files, algorithm %u\n", good, damaged,
	    algorithm);
	return 0;
}

/* SPDX-License-Identifier: BSD-3-Clause */
/* Independent fixture serialization using e2fsprogs disk types and checksums. */
#include "config.h"
#include <stddef.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <ext2fs/ext2fs.h>
#include <ext2fs/ext2_ext_attr.h>
#include <ext2fs/kernel-jbd.h>
#include <ext2fs/fast_commit.h>

#define FIXTURE_SEQUENCE 7U
#define CREATED_FILES 12U
#define LARGE_PREFIX_FILES_1K 256U
#define LARGE_PREFIX_FILES_4K 1024U
#define HUGE_PREFIX_FILES 1024U
#define LONG_NAME_BYTES 230U
#define DEVICE_LEGACY_MASK 0xffU
#define DEVICE_MAJOR_SHIFT 8U
#define DEVICE_MINOR_HIGH_SHIFT 12U
#define ATTRIBUTE_BODY_VALUES 3U
#define ATTRIBUTE_EXTERNAL_VALUES 5U

static const struct {
	const char *name;
	__u16 mode;
	unsigned int type;
	__u32 major;
	__u32 minor;
} special_nodes[] = {
	{ "character-legacy", LINUX_S_IFCHR, EXT2_FT_CHRDEV, 255, 255 },
	{ "character-wide", LINUX_S_IFCHR, EXT2_FT_CHRDEV, 256, 256 },
	{ "character-zero", LINUX_S_IFCHR, EXT2_FT_CHRDEV, 0, 0 },
	{ "block-wide", LINUX_S_IFBLK, EXT2_FT_BLKDEV, 4095, 1048575 },
	{ "fifo", LINUX_S_IFIFO, EXT2_FT_FIFO, 0, 0 },
	{ "socket", LINUX_S_IFSOCK, EXT2_FT_SOCK, 0, 0 },
};

static const char *link_names[] = { "link-short", "link-59", "link-60" };

enum fixture_damage {
	DAMAGE_NONE,
	DAMAGE_LINK_SIZE,
	DAMAGE_LINK_NUL,
	DAMAGE_LINK_TERMINATOR,
	DAMAGE_SPECIAL_SIZE,
	DAMAGE_MISSING_LINK_RANGE,
	DAMAGE_INDIRECT_UNWRITTEN,
	DAMAGE_INDIRECT_LOGICAL_LIMIT,
	DAMAGE_NAME_OWNER,
	DAMAGE_SYSTEM_RANGE,
	DAMAGE_FOREIGN_RANGE,
	DAMAGE_CASEFOLD_TRANSITION
};

struct fixture {
	ext2_filsys pending;
	ext2_filsys expected;
	unsigned char *stream;
	unsigned char *block;
	size_t capacity;
	size_t position;
	__u32 checksum;
	__u32 first;
	__u32 blocks;
	unsigned int commits;
	bool large_prefix;
	bool huge_prefix;
	enum fixture_damage damage;
};

static void
require(int condition, const char *operation)
{
	if (!condition) {
		fprintf(stderr, "%s\n", operation);
		exit(1);
	}
}

static void
check(errcode_t error, const char *operation)
{
	if (error != 0) {
		fprintf(stderr, "%s: %ld\n", operation, (long)error);
		exit(1);
	}
}

static void
created_name(char *name, unsigned int index)
{
	int prefix = snprintf(name, LONG_NAME_BYTES + 1U, "entry-%02u-", index);

	require(prefix > 0 && (unsigned int)prefix < LONG_NAME_BYTES, "form fixture name");
	memset(name + prefix, 'a' + (int)(index % 26U), LONG_NAME_BYTES - (size_t)prefix);
	name[LONG_NAME_BYTES] = 0;
}

static blk64_t
mapped_block(ext2_filsys fs, ext2_ino_t inode, __u32 logical, int *flags)
{
	blk64_t physical;

	check(
	    ext2fs_bmap2(fs, inode, NULL, NULL, 0, logical, flags, &physical), "map fixture block");
	return physical;
}

static void
journal_write(struct fixture *fixture, __u32 logical, const void *bytes)
{
	blk64_t physical;
	int flags;

	physical = mapped_block(
	    fixture->pending, fixture->pending->super->s_journal_inum, logical, &flags);
	require(physical != 0 && flags == 0, "require allocated journal block");
	check(io_channel_write_blk64(fixture->pending->io, physical, 1, bytes),
	    "write journal block");
}

static void
pad(struct fixture *fixture)
{
	struct ext4_fc_tl header;
	unsigned char *bytes = fixture->stream + fixture->position;
	size_t remaining =
	    fixture->pending->blocksize - fixture->position % fixture->pending->blocksize;

	require(remaining >= sizeof(header) && remaining <= fixture->capacity - fixture->position,
	    "bound padding record");
	header.fc_tag = ext2fs_cpu_to_le16(EXT4_FC_TAG_PAD);
	header.fc_len = ext2fs_cpu_to_le16(remaining - sizeof(header));
	memcpy(bytes, &header, sizeof(header));
	fixture->checksum = ext2fs_crc32c_le(fixture->checksum, bytes, remaining);
	fixture->position += remaining;
}

static void
record(struct fixture *fixture, __u16 tag, const void *bytes, size_t length)
{
	struct ext4_fc_tl header;
	unsigned char *destination;
	size_t total = sizeof(header) + length;
	size_t remaining =
	    fixture->pending->blocksize - fixture->position % fixture->pending->blocksize;

	/* Leave room for the next TLV header, including a block-filling PAD. */
	require(total + sizeof(header) <= fixture->pending->blocksize, "bound fixture record");
	if (total + sizeof(header) > remaining) {
		pad(fixture);
	}
	require(total <= fixture->capacity - fixture->position, "bound fast-commit area");
	destination = fixture->stream + fixture->position;
	header.fc_tag = ext2fs_cpu_to_le16(tag);
	header.fc_len = ext2fs_cpu_to_le16(length);
	memcpy(destination, &header, sizeof(header));
	memcpy(destination + sizeof(header), bytes, length);
	fixture->checksum = ext2fs_crc32c_le(fixture->checksum, destination, total);
	fixture->position += total;
}

static void
commit(struct fixture *fixture)
{
	struct ext4_fc_tl header;
	struct ext4_fc_tail tail = { 0 };
	unsigned char *bytes;
	size_t remaining =
	    fixture->pending->blocksize - fixture->position % fixture->pending->blocksize;

	if (remaining < sizeof(header) + sizeof(tail)) {
		pad(fixture);
		remaining = fixture->pending->blocksize;
	}
	require(remaining <= fixture->capacity - fixture->position, "bound commit record");
	bytes = fixture->stream + fixture->position;
	header.fc_tag = ext2fs_cpu_to_le16(EXT4_FC_TAG_TAIL);
	header.fc_len = ext2fs_cpu_to_le16(remaining - sizeof(header));
	tail.fc_tid = ext2fs_cpu_to_le32(FIXTURE_SEQUENCE);
	memcpy(bytes, &header, sizeof(header));
	memcpy(bytes + sizeof(header), &tail, sizeof(tail));
	fixture->checksum = ext2fs_crc32c_le(
	    fixture->checksum, bytes, sizeof(header) + offsetof(struct ext4_fc_tail, fc_crc));
	tail.fc_crc = ext2fs_cpu_to_le32(fixture->checksum);
	memcpy(bytes + sizeof(header), &tail, sizeof(tail));
	fixture->position += remaining;
	fixture->checksum = 0;
	fixture->commits++;
}

static ext2_ino_t
lookup(struct fixture *fixture, ext2_ino_t parent, const char *name)
{
	ext2_ino_t inode;

	check(ext2fs_lookup(fixture->expected, parent, name, (int)strlen(name), NULL, &inode),
	    "lookup expected inode");
	return inode;
}

static void
inode_record(struct fixture *fixture, ext2_ino_t number, unsigned int links)
{
	struct ext4_fc_inode *value;
	struct ext2_inode_large *inode;
	size_t length = sizeof(*value) + EXT2_INODE_SIZE(fixture->expected->super);

	value = calloc(1, length);
	require(value != NULL, "allocate inode record");
	value->fc_ino = ext2fs_cpu_to_le32(number);
	inode = (struct ext2_inode_large *)(value + 1);
	check(ext2fs_read_inode_full(fixture->expected, number, (struct ext2_inode *)inode,
		  EXT2_INODE_SIZE(fixture->expected->super)),
	    "read expected inode");
	if (links != 0) {
		inode->i_links_count = links;
	}
	if ((fixture->damage == DAMAGE_LINK_SIZE || fixture->damage == DAMAGE_LINK_NUL ||
		fixture->damage == DAMAGE_LINK_TERMINATOR) &&
	    number == lookup(fixture, EXT2_ROOT_INO, "link-short")) {
		if (fixture->damage == DAMAGE_LINK_SIZE) {
			inode->i_size = sizeof(inode->i_block);
		} else if (fixture->damage == DAMAGE_LINK_NUL) {
			((unsigned char *)inode->i_block)[0] = 0;
		} else if (fixture->damage == DAMAGE_LINK_TERMINATOR) {
			((unsigned char *)inode->i_block)[inode->i_size] = 'X';
		}
	}
	if (fixture->damage == DAMAGE_SPECIAL_SIZE &&
	    number == lookup(fixture, EXT2_ROOT_INO, "character-wide")) {
		inode->i_size = 1;
	}
	if (fixture->damage == DAMAGE_CASEFOLD_TRANSITION && number == EXT2_ROOT_INO) {
		struct ext2_inode original;

		check(ext2fs_read_inode(fixture->pending, number, &original),
		    "read unchanged-generation directory");
		require(original.i_generation == inode->i_generation,
		    "require unchanged-generation flag transition");
		inode->i_flags |= EXT4_ENCRYPT_FL | EXT4_CASEFOLD_FL;
	}
	check(ext2fs_inode_csum_set(fixture->expected, number, inode), "checksum logged inode");
#ifdef WORDS_BIGENDIAN
	ext2fs_swap_inode_full(
	    fixture->expected, inode, inode, 1, EXT2_INODE_SIZE(fixture->expected->super));
#endif
	record(fixture, EXT4_FC_TAG_INODE, value, length);
	free(value);
}

static void
name_record(
    struct fixture *fixture, __u16 type, ext2_ino_t parent, ext2_ino_t inode, const char *name)
{
	struct ext4_fc_dentry_info *value;
	size_t length = strlen(name);

	require(length > 0 && length <= EXT2_NAME_LEN, "bound directory entry name");
	value = calloc(1, sizeof(*value) + length);
	require(value != NULL, "allocate name record");
	value->fc_parent_ino = ext2fs_cpu_to_le32(parent);
	value->fc_ino = ext2fs_cpu_to_le32(inode);
	memcpy(value + 1, name, length);
	record(fixture, type, value, sizeof(*value) + length);
	free(value);
}

static void
data_records(struct fixture *fixture, ext2_ino_t number)
{
	struct ext2_inode inode;
	struct ext4_fc_add_range range;
	struct ext3_extent extent;
	blk64_t physical;
	__u32 logical;
	__u32 blocks;
	int flags;

	check(ext2fs_read_inode(fixture->expected, number, &inode), "read expected file size");
	if (fixture->damage == DAMAGE_MISSING_LINK_RANGE &&
	    number == lookup(fixture, EXT2_ROOT_INO, "link-60")) {
		return;
	}
	if (ext2fs_is_fast_symlink(&inode)) {
		return;
	}
	require(inode.i_size_high == 0, "bound fixture file size");
	blocks = (inode.i_size + fixture->expected->blocksize - 1U) / fixture->expected->blocksize;
	for (logical = 0; logical < blocks; logical++) {
		physical = mapped_block(fixture->expected, number, logical, &flags);
		if (physical == 0) {
			continue;
		}
		check(io_channel_read_blk64(fixture->expected->io, physical, 1, fixture->block),
		    "read expected file data");
		check(io_channel_write_blk64(fixture->pending->io, physical, 1, fixture->block),
		    "persist ordered file data");
		memset(&extent, 0, sizeof(extent));
		extent.ee_block = ext2fs_cpu_to_le32(logical);
		extent.ee_len =
		    ext2fs_cpu_to_le16(1U + ((flags & BMAP_RET_UNINIT) ? EXT_INIT_MAX_LEN : 0));
		extent.ee_start_hi = ext2fs_cpu_to_le16(physical >> 32);
		extent.ee_start = ext2fs_cpu_to_le32(physical);
		if (number == lookup(fixture, EXT2_ROOT_INO, "renamed") && logical == 0) {
			/* A logged range may not claim system metadata or a block owned
			 * by an inode the log never touches. */
			if (fixture->damage == DAMAGE_SYSTEM_RANGE) {
				physical = ext2fs_inode_table_loc(fixture->expected, 0);
			} else if (fixture->damage == DAMAGE_FOREIGN_RANGE) {
				physical = mapped_block(fixture->expected,
				    lookup(fixture, EXT2_ROOT_INO, "lost+found"), 0, &flags);
			}
			extent.ee_start_hi = ext2fs_cpu_to_le16(physical >> 32);
			extent.ee_start = ext2fs_cpu_to_le32(physical);
			if (fixture->damage == DAMAGE_INDIRECT_UNWRITTEN) {
				extent.ee_len = ext2fs_cpu_to_le16(EXT_INIT_MAX_LEN + 1U);
			} else if (fixture->damage == DAMAGE_INDIRECT_LOGICAL_LIMIT) {
				__u32 per_block = fixture->expected->blocksize / sizeof(__u32);
				__u32 limit = EXT2_NDIR_BLOCKS +
				    per_block * (1U + per_block * (1U + per_block));

				extent.ee_block = ext2fs_cpu_to_le32(limit);
			}
		}
		range.fc_ino = ext2fs_cpu_to_le32(number);
		memcpy(range.fc_ex, &extent, sizeof(extent));
		record(fixture, EXT4_FC_TAG_ADD_RANGE, &range, sizeof(range));
	}
}

static ext2_ino_t
pending_lookup(struct fixture *fixture, const char *name)
{
	ext2_ino_t number;

	check(
	    ext2fs_lookup(fixture->pending, EXT2_ROOT_INO, name, (int)strlen(name), NULL, &number),
	    "lookup checkpointed orphan inode");
	return number;
}

static void
overlapping_ranges(struct fixture *fixture, ext2_ino_t number)
{
	struct ext4_fc_add_range range;
	struct ext3_extent extent = { 0 };
	blk64_t first;
	blk64_t second;
	int flags;

	first = mapped_block(fixture->expected, number, 0, &flags);
	require(first != 0 && flags == 0, "require initialized repeated range");
	second = mapped_block(fixture->expected, number, 1, &flags);
	require(second == first + 1U && flags == 0, "require contiguous repeated range");
	extent.ee_len = ext2fs_cpu_to_le16(2);
	extent.ee_start = ext2fs_cpu_to_le32(first);
	extent.ee_start_hi = ext2fs_cpu_to_le16(first >> 32);
	range.fc_ino = ext2fs_cpu_to_le32(number);
	memcpy(range.fc_ex, &extent, sizeof(extent));
	record(fixture, EXT4_FC_TAG_ADD_RANGE, &range, sizeof(range));
	/* Repeated single-block records are contained by this two-block record.
	 * Their ranges must protect one union, without rejecting legitimate reuse. */
	data_records(fixture, number);
}

static void
pending_orphan(struct fixture *fixture, const char *name, int unlinked, __u32 size)
{
	struct ext2_inode_large inode;
	ext2_ino_t number = pending_lookup(fixture, name);

	check(ext2fs_read_inode_full(
		  fixture->pending, number, (struct ext2_inode *)&inode, sizeof(inode)),
	    "read checkpointed orphan record");
	if (unlinked) {
		check(ext2fs_unlink(fixture->pending, EXT2_ROOT_INO, name, number, 0),
		    "remove orphan name without releasing allocation");
		inode.i_links_count = 0;
		inode.i_dtime = 0;
	}
	if (size != 0) {
		inode.i_size = size;
	}
	check(ext2fs_write_inode_full(
		  fixture->pending, number, (struct ext2_inode *)&inode, sizeof(inode)),
	    "persist checkpointed orphan record");
}

static void
prepare_orphans(struct fixture *fixture)
{
	ext2_filsys fs = fixture->pending;
	ext2_ino_t numbers[5];
	ext2_ino_t orphan_file = fs->super->s_orphan_file_inum;
	ext2_ino_t legacy;
	struct ext2_inode inode;
	__le32 *entries = (__le32 *)fixture->block;
	blk64_t physical;
	unsigned int count = 4;
	unsigned int index;
	int flags;

	if (!ext2fs_has_feature_orphan_file(fs->super) && !ext2fs_has_feature_ea_inode(fs->super) &&
	    ext2fs_has_feature_extents(fs->super)) {
		return;
	}
	numbers[0] = pending_lookup(fixture, "victim");
	numbers[1] = pending_lookup(fixture, "final-delete");
	numbers[2] = pending_lookup(fixture, "orphan-held");
	numbers[3] = pending_lookup(fixture, "orphan-truncate");
	if (ext2fs_has_feature_ea_inode(fs->super)) {
		numbers[count++] = pending_lookup(fixture, "victim-shared");
		require(numbers[count - 1] == lookup(fixture, EXT2_ROOT_INO, "reused-shared"),
		    "require second attribute owner's inode-number reuse");
	}
	legacy = pending_lookup(fixture, "legacy");
	require(numbers[0] == lookup(fixture, EXT2_ROOT_INO, "reused"),
	    "require actual inode-number reuse in fixture");
	pending_orphan(fixture, "orphan-held", 1, 0);
	pending_orphan(fixture, "legacy", 1, 0);
	pending_orphan(fixture, "orphan-truncate", 0, fs->blocksize + 13U);
	if (!ext2fs_has_feature_orphan_file(fs->super)) {
		for (index = 0; index < count; index++) {
			check(ext2fs_read_inode(fs, numbers[index], &inode),
			    "read legacy orphan member");
			inode.i_dtime = index + 1U < count ? numbers[index + 1U] : 0;
			check(ext2fs_write_inode(fs, numbers[index], &inode),
			    "link independent legacy orphan member");
		}
		check(ext2fs_read_inode(fs, legacy, &inode), "read legacy orphan head");
		inode.i_dtime = numbers[0];
		check(
		    ext2fs_write_inode(fs, legacy, &inode), "link independent legacy orphan head");
		fs->super->s_last_orphan = legacy;
		ext2fs_mark_super_dirty(fs);
		return;
	}
	physical = mapped_block(fs, orphan_file, 0, &flags);
	require(physical != 0 && flags == 0, "require mapped orphan slots");
	check(io_channel_read_blk64(fs->io, physical, 1, entries), "read initial orphan slots");
	for (index = 0; index < count; index++) {
		require(entries[index] == 0, "require empty initial orphan slot");
		entries[index] = ext2fs_cpu_to_le32(numbers[index]);
	}
	check(ext2fs_orphan_file_block_csum_set(fs, orphan_file, physical, (char *)entries),
	    "checksum independent orphan slots");
	check(io_channel_write_blk64(fs->io, physical, 1, entries), "persist pending orphan slots");
	fs->super->s_last_orphan = legacy;
	ext2fs_set_feature_orphan_present(fs->super);
	ext2fs_mark_super_dirty(fs);
}

static void
orphan_records(struct fixture *fixture)
{
	ext2_ino_t victim;
	ext2_ino_t final;

	if (!ext2fs_has_feature_orphan_file(fixture->pending->super) &&
	    !ext2fs_has_feature_ea_inode(fixture->pending->super) &&
	    ext2fs_has_feature_extents(fixture->pending->super)) {
		return;
	}
	victim = pending_lookup(fixture, "victim");
	final = pending_lookup(fixture, "final-delete");
	name_record(fixture, EXT4_FC_TAG_UNLINK, EXT2_ROOT_INO, victim, "victim");
	inode_record(fixture, victim, 0);
	data_records(fixture, victim);
	name_record(fixture, EXT4_FC_TAG_CREAT, EXT2_ROOT_INO, victim, "reused");
	if (ext2fs_has_feature_ea_inode(fixture->pending->super)) {
		victim = pending_lookup(fixture, "victim-shared");
		name_record(fixture, EXT4_FC_TAG_UNLINK, EXT2_ROOT_INO, victim, "victim-shared");
		inode_record(fixture, victim, 0);
		data_records(fixture, victim);
		name_record(fixture, EXT4_FC_TAG_CREAT, EXT2_ROOT_INO, victim, "reused-shared");
	}
	name_record(fixture, EXT4_FC_TAG_UNLINK, EXT2_ROOT_INO, final, "final-delete");
}

static void
detach_shared_xattrs(const char *path, const char *owner)
{
	ext2_filsys fs;
	ext2_ino_t number;

	check(ext2fs_open(path, EXT2_FLAG_RW | EXT2_FLAG_64BITS, 0, 0, unix_io_manager, &fs),
	    "open independent expected attribute owner");
	check(ext2fs_read_bitmaps(fs), "read independent attribute allocation bitmaps");
	check(ext2fs_namei(fs, EXT2_ROOT_INO, EXT2_ROOT_INO, owner, &number),
	    "lookup expected old generation");
	/* Use the library's whole-block detach operation. Removing each key from
	 * a shared block with debugfs incorrectly frees its surviving value inodes. */
	check(ext2fs_free_ext_attr(fs, number, NULL), "detach expected shared attribute block");
	check(ext2fs_close(fs), "close expected attribute detachment");
}

static void
share_xattrs(const char *path, const char *target_path)
{
	ext2_filsys fs;
	struct ext2_inode_large *owner;
	struct ext2_inode_large *survivor;
	struct ext2_inode value;
	struct ext2_ext_attr_header *header;
	struct ext2_ext_attr_entry *entry;
	struct ext2_ext_attr_entry *first;
	ext2_ino_t source;
	ext2_ino_t target;
	blk64_t block;
	unsigned char *bytes;
	unsigned int inode_size;
	unsigned int body;
	unsigned int count;
	unsigned int charge;
	unsigned int value_size;
	__u64 references;

	check(ext2fs_open(path, EXT2_FLAG_RW | EXT2_FLAG_64BITS, 0, 0, unix_io_manager, &fs),
	    "open attribute-sharing fixture");
	check(ext2fs_namei(fs, EXT2_ROOT_INO, EXT2_ROOT_INO, "/victim", &source),
	    "lookup old attribute owner");
	check(ext2fs_namei(fs, EXT2_ROOT_INO, EXT2_ROOT_INO, target_path, &target),
	    "lookup surviving attribute owner");
	inode_size = EXT2_INODE_SIZE(fs->super);
	owner = calloc(1, inode_size);
	survivor = calloc(1, inode_size);
	bytes = malloc(fs->blocksize);
	require(owner != NULL && survivor != NULL && bytes != NULL,
	    "allocate independent sharing buffers");
	check(ext2fs_read_inode_full(fs, source, (struct ext2_inode *)owner, inode_size),
	    "read old attribute owner");
	check(ext2fs_read_inode_full(fs, target, (struct ext2_inode *)survivor, inode_size),
	    "read surviving attribute owner");
	body = EXT2_GOOD_OLD_INODE_SIZE + owner->i_extra_isize;
	require(owner->i_extra_isize == survivor->i_extra_isize &&
		body + sizeof(__le32) + sizeof(*entry) < inode_size,
	    "require matching extended inode bodies");
	first = (struct ext2_ext_attr_entry *)((unsigned char *)owner + body + sizeof(__le32));
	entry = first;
	count = 0;
	while ((unsigned char *)entry + sizeof(*entry) <= (unsigned char *)owner + inode_size &&
	    !EXT2_EXT_IS_LAST_ENTRY(entry)) {
		require(entry->e_value_inum != 0 && entry->e_value_offs == 0,
		    "require body entries backed by value inodes");
		count++;
		entry = EXT2_EXT_ATTR_NEXT(entry);
	}
	require(count == ATTRIBUTE_BODY_VALUES, "require two private and one shared body value");
	require(first->e_name_len == strlen("value0") &&
		memcmp(EXT2_EXT_ATTR_NAME(first), "value0", first->e_name_len) == 0,
	    "require maximum-size value as first body entry");
	check(ext2fs_read_inode(fs, ext2fs_le32_to_cpu(first->e_value_inum), &value),
	    "read shared value inode");
	references = ext2fs_get_ea_inode_ref(&value);
	require((value.i_flags & EXT4_EA_INODE_FL) && references > 0 && references < 3,
	    "require one or two value owners before adding another");
	ext2fs_set_ea_inode_ref(&value, references + 1);
	check(ext2fs_write_inode(fs, ext2fs_le32_to_cpu(first->e_value_inum), &value),
	    "persist shared value reference");
	memset((unsigned char *)survivor + body, 0, inode_size - body);
	memcpy((unsigned char *)survivor + body, (unsigned char *)owner + body,
	    sizeof(__le32) + EXT2_EXT_ATTR_LEN(first->e_name_len));
	value_size = ext2fs_le32_to_cpu(first->e_value_size);
	charge = (value_size + fs->blocksize - 1U) / fs->blocksize;
	block = ext2fs_file_acl_block(fs, (struct ext2_inode *)owner);
	require(block != 0 && ext2fs_file_acl_block(fs, (struct ext2_inode *)survivor) == 0,
	    "require one existing attribute block and a fresh survivor");
	check(ext2fs_read_ext_attr3(fs, block, bytes, source), "read independent attribute block");
	header = (struct ext2_ext_attr_header *)bytes;
	require(header->h_refcount > 0 && header->h_refcount < 3,
	    "require one or two block owners before adding another");
	entry = (struct ext2_ext_attr_entry *)(header + 1);
	count = 0;
	while ((unsigned char *)entry + sizeof(*entry) <= bytes + fs->blocksize &&
	    !EXT2_EXT_IS_LAST_ENTRY(entry)) {
		require(entry->e_value_inum != 0 && entry->e_value_offs == 0,
		    "require external entries backed by value inodes");
		charge += (entry->e_value_size + fs->blocksize - 1U) / fs->blocksize;
		count++;
		entry = EXT2_EXT_ATTR_NEXT(entry);
	}
	require(count == ATTRIBUTE_EXTERNAL_VALUES, "require five shared external values");
	header->h_refcount++;
	check(ext2fs_write_ext_attr3(fs, block, bytes, source), "persist shared attribute block");
	ext2fs_file_acl_block_set(fs, (struct ext2_inode *)survivor, block);
	check(ext2fs_iblk_add_blocks(fs, (struct ext2_inode *)survivor, charge + 1U),
	    "charge shared logical values and attribute block");
	check(ext2fs_write_inode_full(fs, target, (struct ext2_inode *)survivor, inode_size),
	    "persist surviving attribute owner");
	check(ext2fs_close(fs), "close independent sharing fixture");
	free(bytes);
	free(survivor);
	free(owner);
	puts("shared-body=1 private-body=2 shared-external=5");
}

static void
create_specials(const char *path)
{
	ext2_filsys fs;
	struct ext2_inode inode;
	ext2_ino_t number;
	unsigned int index;
	__u32 major;
	__u32 minor;

	check(ext2fs_open(path, EXT2_FLAG_RW | EXT2_FLAG_64BITS, 0, 0, unix_io_manager, &fs),
	    "open expected filesystem for special nodes");
	check(ext2fs_read_bitmaps(fs), "read independent allocator bitmaps");
	for (index = 0; index < sizeof(special_nodes) / sizeof(special_nodes[0]); index++) {
		memset(&inode, 0, sizeof(inode));
		inode.i_mode = special_nodes[index].mode | 0640;
		inode.i_links_count = 1;
		major = special_nodes[index].major;
		minor = special_nodes[index].minor;
		if (major <= DEVICE_LEGACY_MASK && minor <= DEVICE_LEGACY_MASK) {
			inode.i_block[0] = (major << DEVICE_MAJOR_SHIFT) | minor;
		} else {
			inode.i_block[1] = (major << DEVICE_MAJOR_SHIFT) |
			    (minor & DEVICE_LEGACY_MASK) |
			    ((minor & ~DEVICE_LEGACY_MASK) << DEVICE_MINOR_HIGH_SHIFT);
		}
		check(ext2fs_new_inode(fs, EXT2_ROOT_INO, inode.i_mode, NULL, &number),
		    "allocate independent special inode");
		check(
		    ext2fs_write_new_inode(fs, number, &inode), "write independent special inode");
		ext2fs_inode_alloc_stats2(fs, number, 1, 0);
		check(ext2fs_link(fs, EXT2_ROOT_INO, special_nodes[index].name, number,
			  special_nodes[index].type),
		    "link independent special inode");
	}
	check(ext2fs_close(fs), "close independent special-node filesystem");
}

static void
prepare_indirect(const char *path)
{
	ext2_filsys fs;
	ext2_file_t file;
	ext2_ino_t number;
	struct ext2_inode inode;
	blk64_t logical = EXT2_NDIR_BLOCKS;
	blk64_t span = 1;
	blk64_t per_block;
	unsigned int written;
	unsigned int depth;
	unsigned char marker = 'T';

	check(ext2fs_open(path, EXT2_FLAG_RW | EXT2_FLAG_64BITS, 0, 0, unix_io_manager, &fs),
	    "open independent indirect filesystem");
	check(ext2fs_read_bitmaps(fs), "read indirect fixture bitmaps");
	require(!ext2fs_has_feature_extents(fs->super), "require indirect fixture format");
	check(ext2fs_namei(fs, EXT2_ROOT_INO, EXT2_ROOT_INO, "/victim", &number),
	    "find old indirect generation");
	check(ext2fs_file_open(fs, number, EXT2_FILE_WRITE, &file), "open old indirect file");
	per_block = fs->blocksize / sizeof(__u32);
	for (depth = EXT2_IND_BLOCK; depth <= EXT2_TIND_BLOCK; depth++) {
		check(ext2fs_file_llseek(file, logical * fs->blocksize, EXT2_SEEK_SET, NULL),
		    "seek indirect boundary");
		check(ext2fs_file_write(file, &marker, sizeof(marker), &written),
		    "allocate old indirect branch");
		require(written == sizeof(marker), "require complete indirect marker");
		span *= per_block;
		logical += span;
	}
	check(ext2fs_file_close(file), "close old indirect file");
	check(ext2fs_read_inode(fs, number, &inode), "read old indirect roots");
	for (depth = EXT2_IND_BLOCK; depth <= EXT2_TIND_BLOCK; depth++) {
		require(inode.i_block[depth] != 0, "require all three indirect levels");
	}
	check(ext2fs_close(fs), "close independent indirect filesystem");
	printf("old-generation-indirect-depth=3\n");
}

static void
special_records(struct fixture *fixture)
{
	ext2_ino_t number;
	unsigned int index;

	for (index = 0; index < sizeof(link_names) / sizeof(link_names[0]); index++) {
		number = lookup(fixture, EXT2_ROOT_INO, link_names[index]);
		inode_record(fixture, number, 0);
		data_records(fixture, number);
		name_record(fixture, EXT4_FC_TAG_CREAT, EXT2_ROOT_INO, number, link_names[index]);
	}
	for (index = 0; index < sizeof(special_nodes) / sizeof(special_nodes[0]); index++) {
		number = lookup(fixture, EXT2_ROOT_INO, special_nodes[index].name);
		inode_record(fixture, number, 0);
		name_record(
		    fixture, EXT4_FC_TAG_CREAT, EXT2_ROOT_INO, number, special_nodes[index].name);
	}
}

int
main(int argc, char **argv)
{
	struct fixture fixture = { 0 };
	struct ext4_fc_head head = { 0 };
	struct ext4_fc_del_range removed;
	struct ext2_inode source;
	journal_superblock_t *journal;
	ext2_ino_t hello;
	ext2_ino_t directory;
	ext2_ino_t child;
	blk64_t physical;
	__u32 fast_blocks;
	__u32 block;
	char name[LONG_NAME_BYTES + 1U];
	unsigned int index;
	unsigned int created_files;
	int flags;

	if (argc == 3 && strcmp(argv[1], "--create-specials") == 0) {
		create_specials(argv[2]);
		return 0;
	}
	if (argc == 3 && strcmp(argv[1], "--prepare-indirect") == 0) {
		prepare_indirect(argv[2]);
		return 0;
	}
	if (argc == 4 && strcmp(argv[1], "--share-xattrs") == 0) {
		share_xattrs(argv[2], argv[3]);
		return 0;
	}
	if (argc == 4 && strcmp(argv[1], "--detach-shared-xattrs") == 0) {
		detach_shared_xattrs(argv[2], argv[3]);
		return 0;
	}
	if (argc != 3 && argc != 4) {
		fprintf(stderr,
		    "usage: fast-commit-fixture PENDING_COPY EXPECTED_IMAGE "
		    "[--specials|--large-prefix|--huge-prefix]\n");
		return 2;
	}
	fixture.huge_prefix = argc == 4 && strcmp(argv[3], "--huge-prefix") == 0;
	fixture.large_prefix = argc == 4 &&
	    (strcmp(argv[3], "--large-prefix") == 0 ||
		strcmp(argv[3], "--large-prefix-conflict") == 0 || fixture.huge_prefix);
	if (fixture.large_prefix && strcmp(argv[3], "--large-prefix-conflict") == 0) {
		fixture.damage = DAMAGE_NAME_OWNER;
	}
	if (argc == 4 && strcmp(argv[3], "--specials") != 0 && !fixture.large_prefix) {
		if (strcmp(argv[3], "--bad-link-size") == 0) {
			fixture.damage = DAMAGE_LINK_SIZE;
		} else if (strcmp(argv[3], "--bad-link-nul") == 0) {
			fixture.damage = DAMAGE_LINK_NUL;
		} else if (strcmp(argv[3], "--bad-link-terminator") == 0) {
			fixture.damage = DAMAGE_LINK_TERMINATOR;
		} else if (strcmp(argv[3], "--bad-special-size") == 0) {
			fixture.damage = DAMAGE_SPECIAL_SIZE;
		} else if (strcmp(argv[3], "--missing-link-range") == 0) {
			fixture.damage = DAMAGE_MISSING_LINK_RANGE;
		} else if (strcmp(argv[3], "--indirect-unwritten") == 0) {
			fixture.damage = DAMAGE_INDIRECT_UNWRITTEN;
		} else if (strcmp(argv[3], "--system-range") == 0) {
			fixture.damage = DAMAGE_SYSTEM_RANGE;
		} else if (strcmp(argv[3], "--foreign-range") == 0) {
			fixture.damage = DAMAGE_FOREIGN_RANGE;
		} else if (strcmp(argv[3], "--indirect-logical-limit") == 0) {
			fixture.damage = DAMAGE_INDIRECT_LOGICAL_LIMIT;
		} else if (strcmp(argv[3], "--casefold-transition") == 0) {
			fixture.damage = DAMAGE_CASEFOLD_TRANSITION;
		} else {
			require(0, "unknown fixture damage mode");
		}
	}
	check(ext2fs_open(argv[1], EXT2_FLAG_RW | EXT2_FLAG_64BITS, 0, 0, unix_io_manager,
		  &fixture.pending),
	    "open pending filesystem");
	check(ext2fs_open(argv[2], EXT2_FLAG_64BITS, 0, 0, unix_io_manager, &fixture.expected),
	    "open independently authored expected filesystem");
	require(fixture.pending->blocksize == fixture.expected->blocksize,
	    "require matching fixture geometry");
	if (fixture.damage == DAMAGE_CASEFOLD_TRANSITION) {
		require(ext2fs_has_feature_casefold(fixture.pending->super),
		    "require independently authored casefold encoding");
		ext2fs_set_feature_encrypt(fixture.pending->super);
		ext2fs_mark_super_dirty(fixture.pending);
	}
	fixture.block = calloc(1, fixture.pending->blocksize);
	require(fixture.block != NULL, "allocate fixture block");
	physical = mapped_block(fixture.pending, fixture.pending->super->s_journal_inum, 0, &flags);
	check(io_channel_read_blk64(fixture.pending->io, physical, 1, fixture.block),
	    "read journal superblock");
	journal = (journal_superblock_t *)fixture.block;
	require(ext2fs_be32_to_cpu(journal->s_header.h_magic) == JBD2_MAGIC_NUMBER,
	    "require journal superblock");
	fixture.blocks = ext2fs_be32_to_cpu(journal->s_maxlen);
	fast_blocks = ext2fs_be32_to_cpu(journal->s_num_fc_blks);
	require(fast_blocks > 1 && fast_blocks < fixture.blocks, "require fast-commit area");
	fixture.first = fixture.blocks - fast_blocks + 1U;
	fixture.capacity = (size_t)(fast_blocks - 1U) * fixture.pending->blocksize;
	fixture.stream = calloc(1, fixture.capacity);
	require(fixture.stream != NULL, "allocate fast-commit stream");
	journal->s_start = journal->s_first;
	journal->s_sequence = ext2fs_cpu_to_be32(FIXTURE_SEQUENCE);
	journal->s_feature_compat = 0;
	journal->s_feature_incompat =
	    ext2fs_cpu_to_be32(JBD2_FEATURE_INCOMPAT_REVOKE | JBD2_FEATURE_INCOMPAT_64BIT |
		JBD2_FEATURE_INCOMPAT_CSUM_V3 | JBD2_FEATURE_INCOMPAT_FAST_COMMIT);
	journal->s_checksum_type = JBD2_CRC32C_CHKSUM;
	journal->s_checksum = 0;
	journal->s_checksum =
	    ext2fs_cpu_to_be32(ext2fs_crc32c_le(~0U, fixture.block, sizeof(*journal)));
	journal_write(&fixture, 0, fixture.block);
	block = ext2fs_be32_to_cpu(journal->s_first);
	memset(fixture.block, 0, fixture.pending->blocksize);
	journal_write(&fixture, block, fixture.block);
	prepare_orphans(&fixture);

	head.fc_tid = ext2fs_cpu_to_le32(FIXTURE_SEQUENCE);
	record(&fixture, EXT4_FC_TAG_HEAD, &head, sizeof(head));
	if (fixture.damage == DAMAGE_CASEFOLD_TRANSITION) {
		/* A checksummed negative transition, not a valid encrypted context. */
		inode_record(&fixture, EXT2_ROOT_INO, 0);
		commit(&fixture);
		goto write_stream;
	}

	hello = lookup(&fixture, EXT2_ROOT_INO, "renamed");
	check(ext2fs_read_inode(fixture.pending, hello, &source), "read source range length");
	require(source.i_size_high == 0, "bound source range length");
	/* Like Linux, each commit logs a changed inode's ranges before its record. */
	removed.fc_ino = ext2fs_cpu_to_le32(hello);
	removed.fc_lblk = 0;
	removed.fc_len = ext2fs_cpu_to_le32(
	    (source.i_size + fixture.pending->blocksize - 1U) / fixture.pending->blocksize);
	record(&fixture, EXT4_FC_TAG_DEL_RANGE, &removed, sizeof(removed));
	data_records(&fixture, hello);
	inode_record(&fixture, hello, 1);
	commit(&fixture);

	directory = lookup(&fixture, EXT2_ROOT_INO, "new-dir");
	inode_record(&fixture, directory, 0);
	name_record(&fixture, EXT4_FC_TAG_CREAT, EXT2_ROOT_INO, directory, "new-dir");
	created_files = fixture.huge_prefix ? HUGE_PREFIX_FILES
	    : fixture.large_prefix
	    ? (fixture.pending->blocksize == 1024 ? LARGE_PREFIX_FILES_1K : LARGE_PREFIX_FILES_4K)
	    : CREATED_FILES;
	for (index = 0; index < created_files; index++) {
		created_name(name, index);
		child = lookup(&fixture, directory, name);
		inode_record(&fixture, child, 0);
		data_records(&fixture, child);
		name_record(&fixture, EXT4_FC_TAG_CREAT, directory, child, name);
	}
	/* Linux then logs each changed regular file's ranges and final record after
	 * the names; its replay derives block counts from these final records. It
	 * never logs a directory or reserved inode outside a creation. The prefix
	 * profiles fill their areas and log every file again in the last commit. */
	for (index = 0; !fixture.large_prefix && index < created_files; index++) {
		created_name(name, index);
		child = lookup(&fixture, directory, name);
		data_records(&fixture, child);
		inode_record(&fixture, child, 0);
	}
	commit(&fixture);

	name_record(&fixture, EXT4_FC_TAG_LINK, EXT2_ROOT_INO, hello, "alias");
	name_record(&fixture, EXT4_FC_TAG_LINK, EXT2_ROOT_INO, hello, "renamed");
	name_record(&fixture, EXT4_FC_TAG_UNLINK, EXT2_ROOT_INO, hello, "hello.txt");
	inode_record(&fixture, hello, 0);
	orphan_records(&fixture);
	if (fixture.large_prefix) {
		for (index = created_files; index > 0; index--) {
			created_name(name, index - 1U);
			child = lookup(&fixture, directory, name);
			inode_record(&fixture, child, 0);
			overlapping_ranges(&fixture, child);
			if (index == created_files || index == created_files / 2U || index == 1) {
				/* Repeated creation/link records must preserve the existing
				 * identity and its link count after an interrupted replay. */
				name_record(&fixture, EXT4_FC_TAG_CREAT, directory, child, name);
				name_record(&fixture, EXT4_FC_TAG_LINK, directory, child, name);
			}
		}
		if (fixture.damage == DAMAGE_NAME_OWNER) {
			name_record(&fixture, EXT4_FC_TAG_LINK, directory, hello, name);
		}
	} else if (argc == 4 && fixture.damage != DAMAGE_CASEFOLD_TRANSITION) {
		special_records(&fixture);
	}
	commit(&fixture);
write_stream:
	for (block = fixture.first; block < fixture.blocks; block++) {
		journal_write(&fixture, block,
		    fixture.stream + (size_t)(block - fixture.first) * fixture.pending->blocksize);
	}
	fixture.pending->super->s_feature_incompat |= EXT3_FEATURE_INCOMPAT_RECOVER;
	ext2fs_mark_super_dirty(fixture.pending);
	printf("sequence=%u commits=%u fast_blocks=%zu\n", FIXTURE_SEQUENCE, fixture.commits,
	    fixture.position / fixture.pending->blocksize);
	free(fixture.stream);
	free(fixture.block);
	check(ext2fs_close(fixture.expected), "close expected filesystem");
	check(ext2fs_close(fixture.pending), "close pending filesystem");
	return 0;
}

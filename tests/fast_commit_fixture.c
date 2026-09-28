/* SPDX-License-Identifier: BSD-3-Clause */
/* Independent fixture serialization using e2fsprogs disk types and checksums. */
#include "config.h"
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <ext2fs/ext2fs.h>
#include <ext2fs/kernel-jbd.h>
#include <ext2fs/fast_commit.h>

#define FIXTURE_SEQUENCE 7U
#define SOURCE_BLOCKS 5U
#define CREATED_FILES 12U
#define LONG_NAME_BYTES 230U
#define DEVICE_LEGACY_MASK 0xffU
#define DEVICE_MAJOR_SHIFT 8U
#define DEVICE_MINOR_HIGH_SHIFT 12U

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
	DAMAGE_MISSING_LINK_RANGE
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
	if (fixture->damage != DAMAGE_NONE &&
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
	ext2_ino_t numbers[4];
	ext2_ino_t orphan_file = fs->super->s_orphan_file_inum;
	ext2_ino_t legacy;
	__le32 *entries = (__le32 *)fixture->block;
	blk64_t physical;
	unsigned int index;
	int flags;

	if (!ext2fs_has_feature_orphan_file(fs->super)) {
		return;
	}
	numbers[0] = pending_lookup(fixture, "victim");
	numbers[1] = pending_lookup(fixture, "final-delete");
	numbers[2] = pending_lookup(fixture, "orphan-held");
	numbers[3] = pending_lookup(fixture, "orphan-truncate");
	legacy = pending_lookup(fixture, "legacy");
	require(numbers[0] == lookup(fixture, EXT2_ROOT_INO, "reused"),
	    "require actual inode-number reuse in fixture");
	pending_orphan(fixture, "orphan-held", 1, 0);
	pending_orphan(fixture, "legacy", 1, 0);
	pending_orphan(fixture, "orphan-truncate", 0, fs->blocksize + 13U);
	physical = mapped_block(fs, orphan_file, 0, &flags);
	require(physical != 0 && flags == 0, "require mapped orphan slots");
	check(io_channel_read_blk64(fs->io, physical, 1, entries), "read initial orphan slots");
	for (index = 0; index < sizeof(numbers) / sizeof(numbers[0]); index++) {
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

	if (!ext2fs_has_feature_orphan_file(fixture->pending->super)) {
		return;
	}
	victim = pending_lookup(fixture, "victim");
	final = pending_lookup(fixture, "final-delete");
	name_record(fixture, EXT4_FC_TAG_UNLINK, EXT2_ROOT_INO, victim, "victim");
	inode_record(fixture, victim, 0);
	data_records(fixture, victim);
	name_record(fixture, EXT4_FC_TAG_CREAT, EXT2_ROOT_INO, victim, "reused");
	name_record(fixture, EXT4_FC_TAG_UNLINK, EXT2_ROOT_INO, final, "final-delete");
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
	journal_superblock_t *journal;
	ext2_ino_t hello;
	ext2_ino_t directory;
	ext2_ino_t child;
	blk64_t physical;
	__u32 fast_blocks;
	__u32 block;
	char name[LONG_NAME_BYTES + 1U];
	unsigned int index;
	int prefix;
	int flags;

	if (argc == 3 && strcmp(argv[1], "--create-specials") == 0) {
		create_specials(argv[2]);
		return 0;
	}
	if (argc != 3 && argc != 4) {
		fprintf(stderr,
		    "usage: fast-commit-fixture PENDING_COPY EXPECTED_IMAGE [--specials]\n");
		return 2;
	}
	if (argc == 4 && strcmp(argv[3], "--specials") != 0) {
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
	hello = lookup(&fixture, EXT2_ROOT_INO, "renamed");
	inode_record(&fixture, hello, 1);
	removed.fc_ino = ext2fs_cpu_to_le32(hello);
	removed.fc_lblk = 0;
	removed.fc_len = ext2fs_cpu_to_le32(SOURCE_BLOCKS);
	record(&fixture, EXT4_FC_TAG_DEL_RANGE, &removed, sizeof(removed));
	data_records(&fixture, hello);
	commit(&fixture);

	directory = lookup(&fixture, EXT2_ROOT_INO, "new-dir");
	inode_record(&fixture, directory, 0);
	name_record(&fixture, EXT4_FC_TAG_CREAT, EXT2_ROOT_INO, directory, "new-dir");
	for (index = 0; index < CREATED_FILES; index++) {
		prefix = snprintf(name, sizeof(name), "entry-%02u-", index);
		require(prefix > 0 && (unsigned int)prefix < LONG_NAME_BYTES, "form fixture name");
		memset(name + prefix, 'a' + (int)index, LONG_NAME_BYTES - (size_t)prefix);
		name[LONG_NAME_BYTES] = 0;
		child = lookup(&fixture, directory, name);
		inode_record(&fixture, child, 0);
		data_records(&fixture, child);
		name_record(&fixture, EXT4_FC_TAG_CREAT, directory, child, name);
	}
	commit(&fixture);

	name_record(&fixture, EXT4_FC_TAG_LINK, EXT2_ROOT_INO, hello, "alias");
	name_record(&fixture, EXT4_FC_TAG_LINK, EXT2_ROOT_INO, hello, "renamed");
	name_record(&fixture, EXT4_FC_TAG_UNLINK, EXT2_ROOT_INO, hello, "hello.txt");
	inode_record(&fixture, hello, 0);
	orphan_records(&fixture);
	if (argc == 4) {
		special_records(&fixture);
	}
	commit(&fixture);
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

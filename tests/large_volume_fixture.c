/* SPDX-License-Identifier: BSD-3-Clause */
/* Independent fixture author; linked only with e2fsprogs. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <ext2fs/ext2fs.h>

static blk64_t first_high_block;

static errcode_t
allocate_high(ext2_filsys fs, blk64_t goal, blk64_t *result)
{
	if (goal < first_high_block || goal >= ext2fs_blocks_count(fs->super)) {
		goal = first_high_block;
	}
	return ext2fs_new_block2(fs, goal, fs->block_map, result);
}

static void
check(errcode_t error, const char *operation)
{
	if (error != 0) {
		fprintf(stderr, "%s: %ld\n", operation, (long)error);
		exit(1);
	}
}

int
main(int argc, char **argv)
{
	ext2_filsys fs;
	ext2_file_t file;
	struct ext2_inode inode = { 0 };
	struct ext2_xattr_handle *xattrs;
	ext2_ino_t hint;
	ext2_ino_t directory;
	ext2_ino_t seed;
	uint64_t offset;
	uint64_t cluster_bytes;
	uint8_t bytes[32];
	uint8_t value[300];
	unsigned int index;
	unsigned int written;
	unsigned int span;

	if (argc != 2) {
		fprintf(stderr, "usage: large-volume-fixture IMAGE\n");
		return 2;
	}
	check(ext2fs_open(argv[1], EXT2_FLAG_RW | EXT2_FLAG_64BITS, 0, 0, unix_io_manager, &fs),
	    "open large-volume fixture");
	check(ext2fs_read_bitmaps(fs), "read large-volume bitmaps");
	first_high_block = ext2fs_group_first_block2(fs, fs->group_desc_count - 1U);
	ext2fs_set_alloc_block_callback(fs, allocate_high, NULL);
	hint = (fs->group_desc_count - 1U) * fs->super->s_inodes_per_group + 1U;
	check(ext2fs_new_inode(fs, hint, LINUX_S_IFDIR | 0755, NULL, &directory),
	    "allocate last-group directory inode");
	check(ext2fs_mkdir(fs, EXT2_ROOT_INO, directory, "upper"), "create last-group directory");
	check(ext2fs_new_inode(fs, directory, LINUX_S_IFREG | 0640, NULL, &seed),
	    "allocate last-group file inode");
	inode.i_mode = LINUX_S_IFREG | 0640;
	inode.i_links_count = 1;
	inode.i_flags = EXT4_EXTENTS_FL;
	inode.i_generation = 123;
	check(ext2fs_write_new_inode(fs, seed, &inode), "initialize seed inode");
	ext2fs_inode_alloc_stats2(fs, seed, 1, 0);
	check(ext2fs_link(fs, directory, "seed", seed, EXT2_FT_REG_FILE), "link seed inode");
	check(ext2fs_file_open(fs, seed, EXT2_FILE_WRITE, &file), "open independent seed");
	cluster_bytes = (uint64_t)fs->blocksize * EXT2FS_CLUSTER_RATIO(fs);
	for (span = 0; span < 5; span++) {
		offset = 2U * span * cluster_bytes + 7U;
		memset(bytes, 'A' + (int)span, sizeof(bytes));
		check(
		    ext2fs_file_llseek(file, offset, EXT2_SEEK_SET, NULL), "seek independent seed");
		check(ext2fs_file_write(file, bytes, sizeof(bytes), &written),
		    "write independent seed");
		if (written != sizeof(bytes)) {
			return 1;
		}
	}
	check(ext2fs_file_close(file), "close independent seed");
	for (index = 0; index < sizeof(value); index++) {
		value[index] = (uint8_t)(0x90U + index % 23U);
	}
	check(ext2fs_xattrs_open(fs, seed, &xattrs), "open independent attributes");
	check(ext2fs_xattrs_read(xattrs), "read independent attributes");
	check(
	    ext2fs_xattr_set(xattrs, "user.large", value, sizeof(value)), "set external attribute");
	check(ext2fs_xattrs_write(xattrs), "write external attribute");
	check(ext2fs_xattrs_close(&xattrs), "close independent attributes");
	printf("directory=%u seed=%u first_block=%llu block_size=%u cluster_blocks=%u\n", directory,
	    seed, (unsigned long long)ext2fs_group_first_block2(fs, fs->group_desc_count - 1U),
	    fs->blocksize, EXT2FS_CLUSTER_RATIO(fs));
	check(ext2fs_close(fs), "close large-volume fixture");
	return 0;
}

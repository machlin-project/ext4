/* SPDX-License-Identifier: BSD-3-Clause */
/* Independent fixture association, using only e2fsprogs's public disk types. */
#include "config.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <ext2fs/ext2fs.h>
#include <ext2fs/kernel-jbd.h>

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
	ext2_filsys device;
	journal_superblock_t *journal;
	void *buffer;
	unsigned int format;
	unsigned int super_block;
	__u32 features = JBD2_FEATURE_INCOMPAT_REVOKE;

	if (argc != 4) {
		fprintf(stderr, "usage: external-journal-fixture FILESYSTEM JOURNAL FORMAT\n");
		return 2;
	}
	format = (unsigned int)strtoul(argv[3], NULL, 10);
	if (format > 3) {
		return 2;
	}
	check(ext2fs_open(argv[1], EXT2_FLAG_RW | EXT2_FLAG_64BITS, 0, 0, unix_io_manager, &fs),
	    "open filesystem");
	check(ext2fs_open(argv[2], EXT2_FLAG_RW | EXT2_FLAG_64BITS | EXT2_FLAG_JOURNAL_DEV_OK, 0, 0,
		  unix_io_manager, &device),
	    "open external journal");
	if (fs->blocksize != device->blocksize || ext2fs_has_feature_journal(fs->super) ||
	    !ext2fs_has_feature_journal_dev(device->super)) {
		return 1;
	}
	super_block = (unsigned int)ext2fs_journal_sb_start(device->blocksize);
	buffer = calloc(1, device->blocksize);
	if (buffer == NULL) {
		return 1;
	}
	check(io_channel_read_blk64(device->io, super_block, 1, buffer), "read journal superblock");
	journal = buffer;
	if (ext2fs_be32_to_cpu(journal->s_header.h_magic) != JBD2_MAGIC_NUMBER ||
	    ext2fs_be32_to_cpu(journal->s_nr_users) != 0) {
		return 1;
	}
	journal->s_nr_users = ext2fs_cpu_to_be32(1);
	memcpy(journal->s_users, fs->super->s_uuid, sizeof(fs->super->s_uuid));
	if (format == 1) {
		journal->s_feature_compat = ext2fs_cpu_to_be32(JBD2_FEATURE_COMPAT_CHECKSUM);
	}
	if (format >= 2) {
		features |= JBD2_FEATURE_INCOMPAT_64BIT;
		features |=
		    format == 2 ? JBD2_FEATURE_INCOMPAT_CSUM_V2 : JBD2_FEATURE_INCOMPAT_CSUM_V3;
		journal->s_checksum_type = JBD2_CRC32C_CHKSUM;
	}
	journal->s_feature_incompat = ext2fs_cpu_to_be32(features);
	if (format >= 2) {
		journal->s_checksum = 0;
		journal->s_checksum =
		    ext2fs_cpu_to_be32(ext2fs_crc32c_le(~0U, buffer, sizeof(*journal)));
	}
	check(io_channel_write_blk64(device->io, super_block, 1, buffer), "associate journal user");
	fs->super->s_journal_inum = 0;
	/* Regular-image fixtures have no host dev_t; explicit UUID lookup owns them. */
	fs->super->s_journal_dev = 0;
	memcpy(fs->super->s_journal_uuid, device->super->s_uuid, sizeof(fs->super->s_journal_uuid));
	ext2fs_set_feature_journal(fs->super);
	ext2fs_mark_super_dirty(fs);
	free(buffer);
	check(ext2fs_close(device), "close journal device");
	check(ext2fs_close(fs), "close filesystem");
	return 0;
}

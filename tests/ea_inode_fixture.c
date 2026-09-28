/* SPDX-License-Identifier: BSD-3-Clause */
/* Fixture-only e2fsprogs client. No filesystem implementation links this tool. */
#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <ext2fs/ext2fs.h>

#define ATTRIBUTE_VALUE_MAX 65536L

int
main(int argc, char **argv)
{
	ext2_filsys fs = NULL;
	struct ext2_xattr_handle *attributes = NULL;
	struct ext2_inode inode;
	ext2_ino_t number;
	errcode_t error;
	unsigned int flags = XATTR_HANDLE_FLAG_RAW;
	FILE *input;
	void *value;
	long size;

	if (argc != 5) {
		fprintf(stderr, "usage: ea-inode-fixture IMAGE PATH KEY VALUE_FILE\n");
		return 2;
	}
	input = fopen(argv[4], "rb");
	if (input == NULL || fseek(input, 0, SEEK_END) != 0) {
		return 1;
	}
	size = ftell(input);
	if (size < 0 || size > ATTRIBUTE_VALUE_MAX || fseek(input, 0, SEEK_SET) != 0) {
		fclose(input);
		return 1;
	}
	value = malloc(size == 0 ? 1U : (size_t)size);
	if (value == NULL || fread(value, 1, (size_t)size, input) != (size_t)size) {
		fclose(input);
		free(value);
		return 1;
	}
	fclose(input);
	error = ext2fs_open(argv[1], EXT2_FLAG_RW | EXT2_FLAG_64BITS, 0, 0, unix_io_manager, &fs);
	if (error == 0) {
		error = ext2fs_read_bitmaps(fs);
	}
	if (error == 0) {
		error = ext2fs_namei(fs, EXT2_ROOT_INO, EXT2_ROOT_INO, argv[2], &number);
	}
	if (error == 0) {
		error = ext2fs_xattrs_open(fs, number, &attributes);
	}
	if (error == 0) {
		error = ext2fs_xattrs_flags(attributes, &flags, NULL);
	}
	if (error == 0) {
		error = ext2fs_xattrs_read(attributes);
	}
	if (error == 0) {
		error = ext2fs_xattr_set(attributes, argv[3], value, (size_t)size);
	}
	/* This fixture creates distinct keys once. The library stores EA inodes
	 * but does not charge their logical value blocks to the owning inode. */
	if (error == 0 && (size_t)size > fs->blocksize) {
		error = ext2fs_read_inode(fs, number, &inode);
		if (error == 0) {
			error = ext2fs_iblk_add_blocks(
			    fs, &inode, ((size_t)size + fs->blocksize - 1U) / fs->blocksize);
		}
		if (error == 0) {
			error = ext2fs_write_inode(fs, number, &inode);
		}
	}
	if (attributes != NULL) {
		ext2fs_xattrs_close(&attributes);
	}
	if (fs != NULL) {
		if (error == 0) {
			error = ext2fs_close(fs);
		} else {
			ext2fs_free(fs);
		}
	}
	free(value);
	if (error != 0) {
		fprintf(stderr, "e2fsprogs attribute creation failed: %ld\n", (long)error);
		return 1;
	}
	return 0;
}

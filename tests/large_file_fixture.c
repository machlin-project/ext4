/* SPDX-License-Identifier: BSD-3-Clause */
/* Fixture-only e2fsprogs client. No filesystem implementation links this tool. */
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <ext2fs/ext2fs.h>

static uint64_t
number(const char *text)
{
	char *end;
	unsigned long long value;

	errno = 0;
	value = strtoull(text, &end, 10);
	if (errno != 0 || *text == '\0' || *text == '-' || *end != '\0') {
		fprintf(stderr, "Invalid unsigned offset: %s\n", text);
		exit(2);
	}
	return value;
}

static unsigned int
hex_digit(char value)
{
	if (value >= '0' && value <= '9') {
		return (unsigned int)(value - '0');
	}
	if (value >= 'a' && value <= 'f') {
		return (unsigned int)(value - 'a' + 10);
	}

	fprintf(stderr, "Invalid hexadecimal data\n");
	exit(2);
}

int
main(int argc, char **argv)
{
	ext2_filsys fs = NULL;
	ext2_file_t file = NULL;
	ext2_ino_t inode;
	errcode_t error;
	uint64_t size;
	uint64_t offset;
	uint8_t bytes[64];
	size_t length;
	size_t index;
	unsigned int written;
	int argument;

	if (argc < 6 || (argc - 4) % 2 != 0) {
		fprintf(stderr, "usage: large-file-fixture IMAGE PATH SIZE OFFSET HEX_BYTES ...\n");
		return 2;
	}
	size = number(argv[3]);
	error = ext2fs_open(argv[1], EXT2_FLAG_RW | EXT2_FLAG_64BITS, 0, 0, unix_io_manager, &fs);
	if (error == 0) {
		error = ext2fs_read_bitmaps(fs);
	}
	if (error == 0) {
		error = ext2fs_namei(fs, EXT2_ROOT_INO, EXT2_ROOT_INO, argv[2], &inode);
	}
	if (error == 0) {
		error = ext2fs_file_open(fs, inode, EXT2_FILE_WRITE, &file);
	}
	for (argument = 4; error == 0 && argument < argc; argument += 2) {
		offset = number(argv[argument]);
		length = strlen(argv[argument + 1]);
		if (length == 0 || length % 2 != 0 || length / 2 > sizeof(bytes) || offset > size ||
		    length / 2 > size - offset) {
			error = EINVAL;
			break;
		}
		length /= 2;
		for (index = 0; index < length; index++) {
			bytes[index] = (uint8_t)(hex_digit(argv[argument + 1][2 * index]) * 16U +
			    hex_digit(argv[argument + 1][2 * index + 1]));
		}
		error = ext2fs_file_llseek(file, offset, EXT2_SEEK_SET, NULL);
		if (error == 0) {
			error = ext2fs_file_write(file, bytes, (unsigned int)length, &written);
			if (error == 0 && written != length) {
				error = EIO;
			}
		}
	}
	if (error == 0) {
		error = ext2fs_file_set_size2(file, (ext2_off64_t)size);
	}
	if (file != NULL) {
		errcode_t closed;

		closed = ext2fs_file_close(file);
		if (error == 0) {
			error = closed;
		}
	}
	if (fs != NULL) {
		if (error == 0) {
			error = ext2fs_close(fs);
		} else {
			ext2fs_free(fs);
		}
	}
	if (error != 0) {
		fprintf(stderr, "e2fsprogs sparse file creation failed: %ld\n", (long)error);
		return 1;
	}
	return 0;
}

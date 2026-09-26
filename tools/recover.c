/* SPDX-License-Identifier: BSD-3-Clause */
#include "image.h"

#include <stdio.h>
#include <string.h>

int
main(int argc, char **argv)
{
	struct ext4_posix_image image;
	struct ext4_recovery_report report;
	enum ext4_result error;

	if (argc != 3 || strcmp(argv[1], "--write") != 0) {
		fprintf(stderr, "usage: %s --write OFFLINE_IMAGE\n", argv[0]);
		return 2;
	}
	error = ext4_posix_open_writable(&image, argv[2]);
	if (error != EXT4_OK) {
		fprintf(stderr, "open: %s\n", ext4_result_string(error));
		return 1;
	}
	error = ext4_recover(&image.environment, &image.writer, &report);
	printf("recovery: %s; transactions=%u replayed=%u revoked=%u discarded_tail=%s\n",
	    ext4_result_string(error), report.transactions, report.replayed_blocks,
	    report.revoked_blocks, report.discarded_tail ? "yes" : "no");
	if (image.live_allocations != 0) {
		fprintf(stderr, "recovery leaked allocations\n");
		error = EXT4_CORRUPT;
	}
	ext4_posix_close(&image);
	return error == EXT4_OK ? 0 : 1;
}

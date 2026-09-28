/* SPDX-License-Identifier: BSD-3-Clause */
#include "image.h"

#include <stdio.h>
#include <string.h>

int
main(int argc, char **argv)
{
	struct ext4_posix_image image;
	struct ext4_posix_image journal_image;
	struct ext4_journal_environment journal;
	const struct ext4_journal_environment *external = NULL;
	struct ext4_recovery_report report;
	enum ext4_result error;

	if ((argc != 3 && argc != 5) || strcmp(argv[1], "--write") != 0 ||
	    (argc == 5 && strcmp(argv[3], "--journal") != 0)) {
		fprintf(stderr, "usage: %s --write OFFLINE_IMAGE [--journal OFFLINE_JOURNAL]\n",
		    argv[0]);
		return 2;
	}
	error = ext4_posix_open_writable(&image, argv[2]);
	if (error != EXT4_OK) {
		fprintf(stderr, "open: %s\n", ext4_result_string(error));
		return 1;
	}
	if (argc == 5) {
		error = ext4_posix_open_writable(&journal_image, argv[4]);
		if (error != EXT4_OK) {
			fprintf(stderr, "open journal: %s\n", ext4_result_string(error));
			ext4_posix_close(&image);
			return 1;
		}
		journal.context = journal_image.environment.context;
		journal.size_bytes = journal_image.environment.size_bytes;
		journal.read = journal_image.environment.read;
		journal.write = journal_image.writer.write;
		journal.flush = journal_image.writer.flush;
		external = &journal;
	}
	error = ext4_recover_with_journal(&image.environment, &image.writer, external, &report);
	printf("recovery: %s; transactions=%u replayed=%u revoked=%u discarded_tail=%s orphans=%u "
	       "orphan_transactions=%u orphan_file_transfers=%u accounting_updated=%s "
	       "fast_commits=%u\n",
	    ext4_result_string(error), report.transactions, report.replayed_blocks,
	    report.revoked_blocks, report.discarded_tail ? "yes" : "no", report.cleaned_orphans,
	    report.orphan_transactions, report.orphan_file_transfers,
	    report.accounting_updated ? "yes" : "no", report.fast_commits);
	if (image.live_allocations != 0) {
		fprintf(stderr, "recovery leaked allocations\n");
		error = EXT4_CORRUPT;
	}
	if (external != NULL) {
		ext4_posix_close(&journal_image);
	}
	ext4_posix_close(&image);
	return error == EXT4_OK ? 0 : 1;
}

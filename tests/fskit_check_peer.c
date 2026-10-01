/* SPDX-License-Identifier: BSD-3-Clause */
#include "../adapters/fskit/Ext4CheckProtocol.h"
#include <string.h>
#include <unistd.h>

/* Deliberately dishonest child: a success exit must not override a rejected
 * resource request. Each mode exercises a different parent admission check. */
int
main(int argc, char **argv)
{
	struct ext4_check_message message = { 0 };
	int64_t deadline = ext4_check_deadline(5);

	if (argc != 2) {
		return 1;
	}
	message.magic = ext4_check_encode32(EXT4_CHECK_MAGIC);
	message.version = ext4_check_encode32(EXT4_CHECK_VERSION);
	message.operation = ext4_check_encode32(EXT4_CHECK_OPEN);
	message.flags = ext4_check_encode32(EXT4_CHECK_WRITABLE);
	if (strcmp(argv[1], "preen") == 0) {
		message.magic = ext4_check_encode32(EXT4_CHECK_MAGIC ^ 1U);
	}
	if (ext4_check_send(STDIN_FILENO, &message, sizeof(message), deadline) != 0) {
		return 0;
	}
	if (ext4_check_receive(STDIN_FILENO, &message, sizeof(message), deadline) != 0 ||
	    strcmp(argv[1], "verify") == 0 || strcmp(argv[1], "preen") == 0) {
		return 0;
	}
	message.operation = ext4_check_encode32(EXT4_CHECK_WRITE);
	message.offset = ext4_check_encode64(0);
	message.length = ext4_check_encode32(EXT4_CHECK_MAX_TRANSFER + 1U);
	message.flags = ext4_check_encode32(0);
	(void)ext4_check_send(STDIN_FILENO, &message, sizeof(message), deadline);
	(void)ext4_check_receive(STDIN_FILENO, &message, sizeof(message), deadline);
	return 0;
}

/* SPDX-License-Identifier: BSD-3-Clause */
#include "../../adapters/fskit/Ext4CheckProtocol.h"
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define CHECK_OPERATIONAL_ERROR 8

int ext4_e2fsck_main(int argc, char **argv);
int ext4_maintenance_io_error(void);

int
main(int argc, char **argv)
{
	int socketType;
	int result;
	socklen_t size = sizeof(socketType);
	char *arguments[] = { "Machlin ext4 check", "-f", NULL, "-E", "nodiscard,readahead_kb=0",
		EXT4_CHECK_RESOURCE_NAME, NULL };

	if (argc != 2 || getsockopt(STDIN_FILENO, SOL_SOCKET, SO_TYPE, &socketType, &size) != 0 ||
	    socketType != SOCK_STREAM) {
		fputs("The checker requires an inherited resource connection and one mode.\n",
		    stderr);
		return CHECK_OPERATIONAL_ERROR;
	}
	if (strcmp(argv[1], "verify") == 0) {
		arguments[2] = "-n";
	} else if (strcmp(argv[1], "repair") == 0) {
		arguments[2] = "-y";
	} else if (strcmp(argv[1], "preen") == 0) {
		arguments[2] = "-p";
	} else {
		fputs("Unknown check mode.\n", stderr);
		return CHECK_OPERATIONAL_ERROR;
	}
	result = ext4_e2fsck_main((int)(sizeof(arguments) / sizeof(arguments[0])) - 1, arguments);
	/* A read, write, barrier or transport failure cannot be accepted as a full
	 * check even if the external checker subsequently returns a success bit. */
	return ext4_maintenance_io_error() != 0 ? result | CHECK_OPERATIONAL_ERROR : result;
}

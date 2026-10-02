/* SPDX-License-Identifier: BSD-3-Clause */
#include "../../adapters/fskit/Ext4CheckProtocol.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
#include <support/profile.h>
#include <uuid/uuid.h>

#define FORMAT_OPERATIONAL_ERROR 8
#define FORMAT_VOLUME_NAME_BYTES 16

int ext4_mke2fs_main(int argc, char **argv);
int ext4_maintenance_io_error(void);
extern const char *mke2fs_default_profile;

/* Use the pinned tool's embedded profile without reading host/user settings. */
long
ext4_format_profile_init(const char *const *files, profile_t *profile)
{
	const char *defaults[] = { "<default>", NULL };
	long error;

	(void)files;
	error = profile_init(defaults, profile);
	if (error == 0) {
		error = profile_set_default(*profile, mke2fs_default_profile);
	}
	return error;
}

long
ext4_format_physical_sector_size(const char *name, int *size)
{
	if (strcmp(name, EXT4_CHECK_RESOURCE_NAME) != 0 || size == NULL) {
		return EINVAL;
	}
	/* The wire contract exposes logical sectors only. Zero is mke2fs's
	 * documented-in-source value for unavailable physical geometry. */
	*size = 0;
	return 0;
}

/* Native device/mount discovery cannot inspect an inherited FSKit capability.
 * Its parent has already acquired the resource and excluded mounted volumes and
 * retained items. Only mke2fs's two pathname admission calls use these shims; all
 * media reads, writes and barriers still go through the resource I/O manager. */
int
ext4_format_resource_plausible(const char *name, int flags, int *isDevice)
{
	(void)flags;
	if (strcmp(name, EXT4_CHECK_RESOURCE_NAME) != 0) {
		return 0;
	}
	if (isDevice != NULL) {
		*isDevice = 1;
	}
	return 1;
}

void
ext4_format_resource_check_mount(const char *name, int force, const char *type)
{
	(void)force;
	(void)type;
	if (strcmp(name, EXT4_CHECK_RESOURCE_NAME) != 0) {
		fputs("The formatter accepts only its inherited resource.\n", stderr);
		exit(FORMAT_OPERATIONAL_ERROR);
	}
}

int
ext4_format_main(int argc, char **argv)
{
	int socketType;
	int result;
	socklen_t size = sizeof(socketType);
	uuid_t uuid;

	if (argc != 4 || getsockopt(STDIN_FILENO, SOL_SOCKET, SO_TYPE, &socketType, &size) != 0 ||
	    socketType != SOCK_STREAM) {
		fputs("The formatter requires an inherited resource, block size, label and UUID.\n",
		    stderr);
		return FORMAT_OPERATIONAL_ERROR;
	}
	if ((strcmp(argv[1], "1024") != 0 && strcmp(argv[1], "2048") != 0 &&
		strcmp(argv[1], "4096") != 0) ||
	    strlen(argv[2]) > FORMAT_VOLUME_NAME_BYTES || strlen(argv[3]) != 36 ||
	    uuid_parse(argv[3], uuid) != 0) {
		fputs("Invalid ext4 format parameters.\n", stderr);
		return FORMAT_OPERATIONAL_ERROR;
	}
	{
		char *arguments[] = { "Machlin ext4 format", "-t", "ext4", "-b", argv[1], "-I",
			"256", "-m", "0", "-O",
			"none,has_journal,ext_attr,resize_inode,dir_index,filetype,extent,64bit,"
			"flex_bg,sparse_super,large_file,huge_file,dir_nlink,extra_isize,metadata_"
			"csum",
			"-E", "nodiscard,lazy_itable_init=0,lazy_journal_init=0,root_owner=0:0",
			"-L", argv[2], "-U", argv[3], "-F", EXT4_CHECK_RESOURCE_NAME, NULL };

		result = ext4_mke2fs_main(
		    (int)(sizeof(arguments) / sizeof(arguments[0])) - 1, arguments);
	}
	return ext4_maintenance_io_error() != 0 ? result | FORMAT_OPERATIONAL_ERROR : result;
}

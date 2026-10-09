/* SPDX-License-Identifier: BSD-3-Clause */
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/fs.h>
#include <linux/fscrypt.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>

/* A capability probe, not a guest init program or a core interoperability test.
 * The caller supplies an already mounted, disposable filesystem. This binary
 * never mounts devices, loads modules, changes namespaces or reboots a machine. */
static void
require(int condition, const char *operation)
{
	if (!condition) {
		fprintf(stderr, "FAIL %s: %s\n", operation, strerror(errno));
		exit(EXIT_FAILURE);
	}
}

static void
add_key(int root, uint8_t identifier[FSCRYPT_KEY_IDENTIFIER_SIZE])
{
	struct fscrypt_add_key_arg *argument;
	size_t size = sizeof(*argument) + 64U;
	unsigned int index;

	argument = calloc(1, size);
	require(argument != NULL, "allocate key argument");
	argument->key_spec.type = FSCRYPT_KEY_SPEC_TYPE_IDENTIFIER;
	argument->raw_size = 64;
	for (index = 0; index < 64U; index++) {
		argument->raw[index] = (uint8_t)(index * 7U + 3U);
	}
	require(ioctl(root, FS_IOC_ADD_ENCRYPTION_KEY, argument) == 0, "add v2 test key");
	memcpy(identifier, argument->key_spec.u.identifier, FSCRYPT_KEY_IDENTIFIER_SIZE);
	memset(argument, 0, size);
	free(argument);
}

static void
casefold(int directory)
{
	int flags = 0;

	require(ioctl(directory, FS_IOC_GETFLAGS, &flags) == 0, "read directory flags");
	flags |= FS_CASEFOLD_FL;
	require(ioctl(directory, FS_IOC_SETFLAGS, &flags) == 0, "enable casefold");
}

static void
check_directory(int directory, const struct fscrypt_policy_v2 *policy, int keyed)
{
	struct fscrypt_get_policy_ex_arg argument;
	struct stat original;
	struct stat alias;
	struct dirent *entry;
	DIR *stream;
	unsigned int names = 0;
	int flags = 0;
	int file;
	char byte = 0;

	memset(&argument, 0, sizeof(argument));
	argument.policy_size = sizeof(argument.policy);
	require(ioctl(directory, FS_IOC_GET_ENCRYPTION_POLICY_EX, &argument) == 0,
	    "read v2 policy");
	require(argument.policy_size == sizeof(*policy) &&
		memcmp(&argument.policy.v2, policy, sizeof(*policy)) == 0, "exact v2 policy");
	require(ioctl(directory, FS_IOC_GETFLAGS, &flags) == 0 &&
		(flags & (FS_CASEFOLD_FL | FS_ENCRYPT_FL)) == (FS_CASEFOLD_FL | FS_ENCRYPT_FL),
	    "combined inode flags");
	if (keyed) {
		require(fstatat(directory, "Stra\303\237e", &original, AT_SYMLINK_NOFOLLOW) == 0 &&
			fstatat(directory, "STRASSE", &alias, AT_SYMLINK_NOFOLLOW) == 0 &&
			original.st_ino == alias.st_ino && original.st_dev == alias.st_dev,
		    "Unicode alias identity");
		file = openat(directory, "strasse", O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
		require(file >= 0 && read(file, &byte, 1) == 1 && byte == 'K', "keyed content");
		require(close(file) == 0, "close keyed file");
	} else {
		errno = 0;
		require(fstatat(directory, "STRASSE", &alias, AT_SYMLINK_NOFOLLOW) == -1 &&
			errno == ENOENT, "plaintext unavailable without key");
	}
	stream = fdopendir(dup(directory));
	require(stream != NULL, "open listing");
	errno = 0;
	while ((entry = readdir(stream)) != NULL) {
		if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
			continue;
		}
		names++;
		require(fstatat(directory, entry->d_name, &alias, AT_SYMLINK_NOFOLLOW) == 0 &&
			S_ISREG(alias.st_mode), "listed identity lookup");
		if (keyed) {
			require(strcmp(entry->d_name, "Stra\303\237e") == 0, "stored spelling");
		} else {
			errno = 0;
			file = openat(directory, entry->d_name, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
			require(file == -1 && errno == ENOKEY, "keyless content refusal");
		}
		errno = 0;
	}
	require(errno == 0 && names == 1, "exact directory listing");
	require(closedir(stream) == 0, "close listing");
}

int
main(int argc, char **argv)
{
	struct fscrypt_policy_v2 policy;
	uint8_t identifier[FSCRYPT_KEY_IDENTIFIER_SIZE];
	unsigned int padding;
	unsigned int order;
	char name[32];
	int root;
	int directory;
	int file;
	int length;
	int create;
	int keyed;

	if (argc != 3 || (strcmp(argv[1], "create") != 0 &&
		strcmp(argv[1], "keyed") != 0 && strcmp(argv[1], "nokey") != 0)) {
		fprintf(stderr, "usage: %s create|keyed|nokey MOUNTED-DIRECTORY\n", argv[0]);
		return EXIT_FAILURE;
	}
	create = strcmp(argv[1], "create") == 0;
	keyed = strcmp(argv[1], "nokey") != 0;
	root = open(argv[2], O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
	require(root >= 0, "open disposable filesystem");
	memset(identifier, 0, sizeof(identifier));
	if (keyed) {
		add_key(root, identifier);
	}
	for (padding = 0; padding < 2; padding++) {
		for (order = 0; order < 2; order++) {
			length = snprintf(name, sizeof(name), "probe-%u-%u", padding, order);
			require(length > 0 && (size_t)length < sizeof(name), "form probe name");
			if (create) {
				require(mkdirat(root, name, 0700) == 0, "create probe directory");
			}
			directory = openat(root, name, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
			require(directory >= 0, "open probe directory");
			memset(&policy, 0, sizeof(policy));
			policy.version = FSCRYPT_POLICY_V2;
			policy.contents_encryption_mode = FSCRYPT_MODE_AES_256_XTS;
			policy.filenames_encryption_mode = FSCRYPT_MODE_AES_256_CTS;
			policy.flags = padding == 0 ? FSCRYPT_POLICY_FLAGS_PAD_4 : FSCRYPT_POLICY_FLAGS_PAD_32;
			memcpy(policy.master_key_identifier, identifier, sizeof(identifier));
			if (create) {
				if (order == 0) {
					casefold(directory);
				}
				require(ioctl(directory, FS_IOC_SET_ENCRYPTION_POLICY, &policy) == 0,
				    "set v2 policy");
				if (order != 0) {
					casefold(directory);
				}
				file = openat(directory, "Stra\303\237e",
				    O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
				require(file >= 0 && write(file, "K", 1) == 1 && fsync(file) == 0,
				    "create encrypted file");
				require(close(file) == 0, "close created file");
				errno = 0;
				file = openat(directory, "STRASSE",
				    O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
				require(file == -1 && errno == EEXIST, "folded duplicate refusal");
				require(fsync(directory) == 0, "sync probe directory");
			}
			if (!keyed) {
				/* Identifier is public context, read it without installing a key. */
				struct fscrypt_get_policy_ex_arg actual;

				memset(&actual, 0, sizeof(actual));
				actual.policy_size = sizeof(actual.policy);
				require(ioctl(directory, FS_IOC_GET_ENCRYPTION_POLICY_EX, &actual) == 0,
				    "read keyless identifier");
				memcpy(policy.master_key_identifier, actual.policy.v2.master_key_identifier,
				    sizeof(identifier));
			}
			check_directory(directory, &policy, keyed);
			require(close(directory) == 0, "close probe directory");
		}
	}
	if (create) {
		require(fsync(root) == 0, "sync filesystem root");
	}
	require(close(root) == 0, "close filesystem root");
	printf("PASS native encrypted-casefold capability %s: 4 policies\n", argv[1]);
	return EXIT_SUCCESS;
}

/* SPDX-License-Identifier: BSD-3-Clause */
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/reboot.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/utsname.h>
#include <unistd.h>

/* Guest probe for fscrypt interoperability. Phase 0 creates an encrypted
 * directory tree and plain files; phase 1 adds the same key again after the core
 * changed only unencrypted names and requires every encrypted byte to survive.
 * Phase 2 adds the key to a tree the core encrypted and requires every object the
 * manifest lists: files by size and SHA-256, symlinks by target, and the types of
 * directories and FIFOs. Before adding the key it reports the no-key names and
 * symlink targets of the directories /nokey names, for the host to compare with the
 * core's. */

#define PHASE_CREATE 0U
#define PHASE_VERIFY 1U
#define PHASE_CORE 2U
#define MANIFEST_LINE 1024U
#define SHA256_BYTES 32U
#define SHA256_BLOCK 64U
#define KEY_BYTES 64U
#define KEY_IDENTIFIER_BYTES 16U
#define FSCRYPT_POLICY_V2 2U
#define FSCRYPT_MODE_AES_256_XTS 1U
#define FSCRYPT_MODE_AES_256_CTS 4U
#define FSCRYPT_POLICY_FLAGS_PAD_32 0x03U
#define FSCRYPT_KEY_SPEC_TYPE_IDENTIFIER 2U
#define FSCRYPT_IOCTL_TYPE 'f'
#define FSCRYPT_SET_POLICY_NUMBER 19
#define FSCRYPT_ADD_KEY_NUMBER 23
#define ENCRYPTED_FILES 24U
#define LARGE_FILE_BYTES (3U * 65536U + 777U)
#define LONG_NAME_BYTES 180U
/* Linux's no-key names are at most 252 base64url characters. */
#define NOKEY_NAME_MAX 252U

/* Linux UAPI <linux/fscrypt.h> layouts; the musl sysroot has no kernel headers.
 * The ioctl numbers encode the v1 policy and the key argument without raw bytes. */
struct fscrypt_policy_v1 {
	uint8_t version;
	uint8_t contents_encryption_mode;
	uint8_t filenames_encryption_mode;
	uint8_t flags;
	uint8_t master_key_descriptor[8];
};

struct fscrypt_policy_v2 {
	uint8_t version;
	uint8_t contents_encryption_mode;
	uint8_t filenames_encryption_mode;
	uint8_t flags;
	uint8_t reserved[4];
	uint8_t master_key_identifier[KEY_IDENTIFIER_BYTES];
};

struct fscrypt_key_specifier {
	uint32_t type;
	uint32_t reserved;

	union {
		uint8_t reserved_space[32];
		uint8_t identifier[KEY_IDENTIFIER_BYTES];
	} u;
};

struct fscrypt_add_key_header {
	struct fscrypt_key_specifier key_spec;
	uint32_t raw_size;
	uint32_t key_id;
	uint32_t reserved[8];
};

struct fscrypt_add_key_argument {
	struct fscrypt_add_key_header header;
	uint8_t raw[KEY_BYTES];
};

#define FS_IOC_SET_ENCRYPTION_POLICY                                                               \
	_IOR(FSCRYPT_IOCTL_TYPE, FSCRYPT_SET_POLICY_NUMBER, struct fscrypt_policy_v1)
#define FS_IOC_ADD_ENCRYPTION_KEY                                                                  \
	_IOWR(FSCRYPT_IOCTL_TYPE, FSCRYPT_ADD_KEY_NUMBER, struct fscrypt_add_key_header)

static void
power_off(int passed)
{
	printf("LINUX_ENCRYPT_RESULT=%s\n", passed ? "PASS" : "FAIL");
	fflush(stdout);
	fflush(stderr);
	reboot(RB_POWER_OFF);
	for (;;) {
		pause();
	}
}

static void
require(int condition, const char *operation)
{
	if (!condition) {
		fprintf(stderr, "%s: %s\n", operation, strerror(errno));
		power_off(0);
	}
}

static uint8_t
pattern(unsigned int file, size_t index)
{
	return (uint8_t)(file * 131U + index * 17U + (index >> 9));
}

static size_t
file_size(unsigned int file)
{
	return file == 0 ? LARGE_FILE_BYTES : 1U + file * 97U;
}

static void
file_name(char *path, size_t size, const char *directory, unsigned int file)
{
	int length;

	length = file % 5U == 0
	    ? snprintf(path, size, "%s/long-%02u-%0*u", directory, file, (int)LONG_NAME_BYTES, file)
	    : snprintf(path, size, "%s/file-%02u", directory, file);
	require(length > 0 && (size_t)length < size, "form encrypted file name");
}

static void
add_key(int directory, uint8_t *identifier)
{
	struct fscrypt_add_key_argument argument;
	unsigned int index;

	memset(&argument, 0, sizeof(argument));
	argument.header.key_spec.type = FSCRYPT_KEY_SPEC_TYPE_IDENTIFIER;
	argument.header.raw_size = KEY_BYTES;
	for (index = 0; index < KEY_BYTES; index++) {
		argument.raw[index] = (uint8_t)(index * 7U + 3U);
	}
	require(
	    ioctl(directory, FS_IOC_ADD_ENCRYPTION_KEY, &argument) == 0, "add fscrypt master key");
	memcpy(identifier, argument.header.key_spec.u.identifier, KEY_IDENTIFIER_BYTES);
}

/* FIPS 180-4 SHA-256; the static guest has no library for it. */
struct sha256 {
	uint32_t state[8];
	uint8_t block[SHA256_BLOCK];
	uint64_t length;
	size_t used;
};

static const uint32_t sha256_rounds[64] = { 0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5,
	0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be,
	0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786,
	0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152,
	0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
	0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e,
	0x92722c85, 0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624,
	0xf40e3585, 0x106aa070, 0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3,
	0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
	0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2 };

static uint32_t
rotate(uint32_t value, unsigned int count)
{
	return (value >> count) | (value << (32U - count));
}

static void
sha256_block(struct sha256 *context)
{
	uint32_t words[64];
	uint32_t state[8];
	uint32_t first;
	uint32_t second;
	unsigned int index;

	for (index = 0; index < 16U; index++) {
		words[index] = (uint32_t)context->block[4U * index] << 24 |
		    (uint32_t)context->block[4U * index + 1U] << 16 |
		    (uint32_t)context->block[4U * index + 2U] << 8 |
		    context->block[4U * index + 3U];
	}
	for (index = 16; index < 64U; index++) {
		words[index] = words[index - 16U] +
		    (rotate(words[index - 15U], 7) ^ rotate(words[index - 15U], 18) ^
			(words[index - 15U] >> 3)) +
		    words[index - 7U] +
		    (rotate(words[index - 2U], 17) ^ rotate(words[index - 2U], 19) ^
			(words[index - 2U] >> 10));
	}
	memcpy(state, context->state, sizeof(state));
	for (index = 0; index < 64U; index++) {
		first = state[7] +
		    (rotate(state[4], 6) ^ rotate(state[4], 11) ^ rotate(state[4], 25)) +
		    ((state[4] & state[5]) ^ (~state[4] & state[6])) + sha256_rounds[index] +
		    words[index];
		second = (rotate(state[0], 2) ^ rotate(state[0], 13) ^ rotate(state[0], 22)) +
		    ((state[0] & state[1]) ^ (state[0] & state[2]) ^ (state[1] & state[2]));
		memmove(state + 1, state, 7U * sizeof(state[0]));
		state[4] += first;
		state[0] = first + second;
	}
	for (index = 0; index < 8U; index++) {
		context->state[index] += state[index];
	}
}

static void
sha256_init(struct sha256 *context)
{
	static const uint32_t initial[8] = { 0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
		0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19 };

	memset(context, 0, sizeof(*context));
	memcpy(context->state, initial, sizeof(initial));
}

static void
sha256_update(struct sha256 *context, const uint8_t *bytes, size_t length)
{
	size_t index;

	for (index = 0; index < length; index++) {
		context->block[context->used++] = bytes[index];
		if (context->used == SHA256_BLOCK) {
			sha256_block(context);
			context->used = 0;
		}
	}
	context->length += length;
}

static void
sha256_final(struct sha256 *context, char *text)
{
	uint64_t bits = context->length * 8U;
	uint8_t padding = 0x80;
	uint8_t zero = 0;
	uint8_t length[8];
	unsigned int index;

	sha256_update(context, &padding, 1);
	while (context->used != SHA256_BLOCK - sizeof(length)) {
		sha256_update(context, &zero, 1);
	}
	for (index = 0; index < sizeof(length); index++) {
		length[index] = (uint8_t)(bits >> (56U - 8U * index));
	}
	sha256_update(context, length, sizeof(length));
	for (index = 0; index < 8U; index++) {
		snprintf(text + 8U * index, 9, "%08x", context->state[index]);
	}
}

/* Check the core-encrypted tree the manifest describes. */
static void
verify_core(void)
{
	struct sha256 context;
	struct stat metadata;
	static uint8_t bytes[65536];
	char line[MANIFEST_LINE];
	char kind[16];
	char path[MANIFEST_LINE];
	char expected[MANIFEST_LINE];
	char digest[2U * SHA256_BYTES + 1U];
	char target[MANIFEST_LINE];
	unsigned long long size;
	unsigned int checked = 0;
	ssize_t count;
	FILE *manifest = fopen("/manifest", "r");
	int fd;

	require(manifest != NULL, "open core manifest");
	while (fgets(line, sizeof(line), manifest) != NULL) {
		require(sscanf(line, "%15s %1023s", kind, expected) == 2, "parse manifest entry");
		snprintf(path, sizeof(path), "/mnt/%s", expected);
		require(lstat(path, &metadata) == 0, path);
		if (strcmp(kind, "file") == 0) {
			require(sscanf(line, "%*s %*s %llu %64s", &size, expected) == 2 &&
				S_ISREG(metadata.st_mode) &&
				(unsigned long long)metadata.st_size == size,
			    "core file size");
			fd = open(path, O_RDONLY | O_CLOEXEC);
			require(fd >= 0, "open core file");
			sha256_init(&context);
			while ((count = read(fd, bytes, sizeof(bytes))) > 0) {
				sha256_update(&context, bytes, (size_t)count);
			}
			require(count == 0 && close(fd) == 0, "read core file");
			sha256_final(&context, digest);
			require(strcmp(digest, expected) == 0, "compare core file contents");
		} else if (strcmp(kind, "symlink") == 0) {
			require(sscanf(line, "%*s %*s %1023s", expected) == 1 &&
				S_ISLNK(metadata.st_mode),
			    "core symlink");
			count = readlink(path, target, sizeof(target) - 1U);
			require(count > 0, "read core symlink");
			target[count] = 0;
			require(strcmp(target, expected) == 0, "compare core symlink target");
		} else if (strcmp(kind, "fifo") == 0) {
			require(S_ISFIFO(metadata.st_mode), "core fifo");
		} else {
			require(strcmp(kind, "directory") == 0 && S_ISDIR(metadata.st_mode),
			    "core directory");
		}
		printf("LINUX_ENCRYPT_CORE_OBJECT=%s %s\n", kind, path);
		checked++;
	}
	require(fclose(manifest) == 0 && checked != 0, "complete core manifest");
	printf("LINUX_ENCRYPT_CORE_OBJECTS=%u\n", checked);
}

/* Without the key, list each directory /nokey names and read its symlinks' targets;
 * every listed name must also look up. */
static void
report_nokey(void)
{
	struct dirent *entry;
	struct stat metadata;
	char line[MANIFEST_LINE];
	char directory[MANIFEST_LINE];
	char path[2U * MANIFEST_LINE];
	char target[NOKEY_NAME_MAX + 1U];
	unsigned int names = 0;
	ssize_t count;
	FILE *manifest = fopen("/nokey", "r");
	DIR *stream;

	require(manifest != NULL, "open no-key manifest");
	while (fgets(line, sizeof(line), manifest) != NULL) {
		if (sscanf(line, "directory %1023s", directory) != 1) {
			continue;
		}
		snprintf(path, sizeof(path), "/mnt/%s", directory);
		stream = opendir(path);
		require(stream != NULL, path);
		errno = 0;
		while ((entry = readdir(stream)) != NULL) {
			if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
				continue;
			}
			printf("LINUX_ENCRYPT_NOKEY=name %s %s\n", directory, entry->d_name);
			snprintf(path, sizeof(path), "/mnt/%s/%s", directory, entry->d_name);
			require(lstat(path, &metadata) == 0, "look up no-key name");
			if (S_ISLNK(metadata.st_mode)) {
				count = readlink(path, target, sizeof(target) - 1U);
				require(count > 0, "read no-key symlink target");
				target[count] = 0;
				printf("LINUX_ENCRYPT_NOKEY=link %s/%s %s\n", directory,
				    entry->d_name, target);
			}
			names++;
			errno = 0;
		}
		require(errno == 0 && closedir(stream) == 0, "list no-key directory");
	}
	require(fclose(manifest) == 0 && names != 0, "complete no-key listing");
	printf("LINUX_ENCRYPT_NOKEY_NAMES=%u\n", names);
}

static void
write_file(const char *path, unsigned int file)
{
	uint8_t bytes[4096];
	size_t size = file_size(file);
	size_t done;
	size_t chunk;
	size_t index;
	int fd = open(path, O_CREAT | O_EXCL | O_WRONLY | O_CLOEXEC, 0640);

	require(fd >= 0, path);
	for (done = 0; done < size; done += chunk) {
		chunk = size - done < sizeof(bytes) ? size - done : sizeof(bytes);
		for (index = 0; index < chunk; index++) {
			bytes[index] = pattern(file, done + index);
		}
		require(write(fd, bytes, chunk) == (ssize_t)chunk, "write encrypted file");
	}
	require(fsync(fd) == 0 && close(fd) == 0, "close encrypted file");
}

static void
verify_file(const char *path, unsigned int file)
{
	uint8_t bytes[4096];
	size_t size = file_size(file);
	size_t done;
	size_t index;
	ssize_t count;
	int fd = open(path, O_RDONLY | O_CLOEXEC);

	require(fd >= 0, path);
	for (done = 0; done < size; done += (size_t)count) {
		count = read(fd, bytes, sizeof(bytes));
		require(count > 0, "read encrypted file");
		for (index = 0; index < (size_t)count; index++) {
			require(
			    bytes[index] == pattern(file, done + index), "verify encrypted byte");
		}
	}
	require(read(fd, bytes, 1) == 0 && close(fd) == 0, "verify encrypted EOF");
}

int
main(void)
{
	const char *modules[] = { "virtio_blk", "crc32c_generic", "crc16", "mbcache", "jbd2",
		"ext4" };
	struct fscrypt_policy_v2 policy;
	struct utsname identity;
	char path[512];
	char target[128];
	unsigned int phase = PHASE_VERIFY + 1U;
	unsigned int index;
	FILE *config;
	int directory;
	int fd;
	int result;

	setvbuf(stdout, NULL, _IOLBF, 0);
	require(uname(&identity) == 0, "uname");
	printf("LINUX_ENCRYPT_KERNEL=%s %s %s\n", identity.sysname, identity.release,
	    identity.machine);
	require(mount("devtmpfs", "/dev", "devtmpfs", 0, NULL) == 0, "mount devtmpfs");
	for (index = 0; index < sizeof(modules) / sizeof(modules[0]); index++) {
		snprintf(path, sizeof(path), "/modules/%s.ko", modules[index]);
		fd = open(path, O_RDONLY | O_CLOEXEC);
		require(fd >= 0, "open matching module");
		result = (int)syscall(SYS_finit_module, fd, "", 0);
		require(result == 0 || errno == EEXIST, path);
		require(close(fd) == 0, "close module");
	}
	config = fopen("/phase", "r");
	require(config != NULL && fscanf(config, "%u", &phase) == 1 && fclose(config) == 0 &&
		phase <= PHASE_CORE,
	    "read probe phase");
	require(mount("/dev/vda", "/mnt", "ext4", MS_NOATIME, "data=ordered") == 0,
	    "mount encryption filesystem");
	if (phase == PHASE_CORE) {
		if (access("/nokey", F_OK) == 0) {
			report_nokey();
		}
		directory = open("/mnt", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
		require(directory >= 0, "open filesystem root");
		add_key(directory, policy.master_key_identifier);
		require(close(directory) == 0, "close filesystem root");
		verify_core();
		puts("LINUX_ENCRYPT_CORE_VERIFIED");
	} else if (phase == PHASE_CREATE) {
		require(mkdir("/mnt/secret", 0750) == 0, "create encrypted root");
		directory = open("/mnt/secret", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
		require(directory >= 0, "open encrypted root");
		memset(&policy, 0, sizeof(policy));
		policy.version = FSCRYPT_POLICY_V2;
		policy.contents_encryption_mode = FSCRYPT_MODE_AES_256_XTS;
		policy.filenames_encryption_mode = FSCRYPT_MODE_AES_256_CTS;
		policy.flags = FSCRYPT_POLICY_FLAGS_PAD_32;
		add_key(directory, policy.master_key_identifier);
		require(ioctl(directory, FS_IOC_SET_ENCRYPTION_POLICY, &policy) == 0,
		    "set encryption policy");
		require(close(directory) == 0, "close encrypted root");
		require(mkdir("/mnt/secret/inner", 0750) == 0, "create encrypted subdirectory");
		require(mkdir("/mnt/secret/empty", 0750) == 0, "create empty encrypted directory");
		for (index = 0; index < ENCRYPTED_FILES; index++) {
			file_name(path, sizeof(path),
			    index % 2U ? "/mnt/secret" : "/mnt/secret/inner", index);
			write_file(path, index);
		}
		snprintf(target, sizeof(target), "inner/file-01-target-%s", "x");
		require(symlink(target, "/mnt/secret/link") == 0, "create encrypted symlink");
		require(mkdir("/mnt/plain", 0755) == 0, "create plain directory");
		write_file("/mnt/plain/visible", ENCRYPTED_FILES);
		/* An empty encrypted directory at the top level can be removed without a key. */
		require(mkdir("/mnt/locked-empty", 0750) == 0, "create empty encrypted root");
		directory = open("/mnt/locked-empty", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
		require(directory >= 0 &&
			ioctl(directory, FS_IOC_SET_ENCRYPTION_POLICY, &policy) == 0 &&
			close(directory) == 0,
		    "encrypt empty root");
		puts("LINUX_ENCRYPT_CREATED");
	} else {
		directory = open("/mnt/secret", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
		require(directory >= 0, "open encrypted root");
		add_key(directory, policy.master_key_identifier);
		require(close(directory) == 0, "close encrypted root");
		for (index = 0; index < ENCRYPTED_FILES; index++) {
			file_name(path, sizeof(path),
			    index % 2U ? "/mnt/secret" : "/mnt/secret/inner", index);
			verify_file(path, index);
		}
		result = (int)readlink("/mnt/secret/link", target, sizeof(target) - 1U);
		require(result > 0, "read encrypted symlink");
		target[result] = 0;
		require(strcmp(target, "inner/file-01-target-x") == 0, "verify encrypted symlink");
		require(access("/mnt/locked-empty", F_OK) < 0 && errno == ENOENT,
		    "verify removed empty encrypted root");
		verify_file("/mnt/renamed-plain/visible-moved", ENCRYPTED_FILES);
		puts("LINUX_ENCRYPT_VERIFIED");
	}
	require(umount("/mnt") == 0, "cleanly unmount encryption filesystem");
	power_off(1);
	return 0;
}

/* SPDX-License-Identifier: BSD-3-Clause */
#include <dirent.h>
#include <sys/statvfs.h>

#define TEST_SYMLINK_INLINE_CAPACITY 60U

struct namespace_entry {
	char name[EXT4_NAME_MAX + 1];
	unsigned long long inode;
	bool seen;
};

static void
namespace_inode_checks(void)
{
	struct stat actual;
	FILE *input;
	char path[512];
	char mounted[sizeof(path) + 4];
	unsigned long long inode;
	unsigned long long links;
	unsigned long long size;
	unsigned long long blocks;
	unsigned int mode;
	unsigned int uid;
	unsigned int gid;
	long long atime;
	long long mtime;
	long long ctime;
	long access_nsec;
	long modify_nsec;
	long change_nsec;
	int fields;

	input = fopen("/namespace-inodes", "r");
	require(input != NULL, "open independent namespace inode expectations");
	for (;;) {
		fields =
		    fscanf(input, "%511s %llu %o %u %u %llu %llu %llu %lld %ld %lld %ld %lld %ld",
			path, &inode, &mode, &uid, &gid, &links, &size, &blocks, &atime,
			&access_nsec, &mtime, &modify_nsec, &ctime, &change_nsec);
		if (fields == EOF) {
			break;
		}
		require(fields == 14, "parse namespace inode expectation");
		snprintf(mounted, sizeof(mounted), "/mnt%s", path);
		require(lstat(mounted, &actual) == 0, mounted);
		if (actual.st_ino != inode || actual.st_mode != mode || actual.st_uid != uid ||
		    actual.st_gid != gid || actual.st_nlink != links ||
		    actual.st_size != (off_t)size || actual.st_blocks != (blkcnt_t)blocks ||
		    actual.st_atim.tv_sec != atime || actual.st_atim.tv_nsec != access_nsec ||
		    actual.st_mtim.tv_sec != mtime || actual.st_mtim.tv_nsec != modify_nsec ||
		    actual.st_ctim.tv_sec != ctime || actual.st_ctim.tv_nsec != change_nsec) {
			fprintf(stderr,
			    "Namespace metadata mismatch: %s inode=%llu mode=%o "
			    "uid=%u gid=%u links=%llu size=%lld blocks=%lld "
			    "atime=%lld.%09ld mtime=%lld.%09ld ctime=%lld.%09ld\n",
			    mounted, (unsigned long long)actual.st_ino, actual.st_mode,
			    actual.st_uid, actual.st_gid, (unsigned long long)actual.st_nlink,
			    (long long)actual.st_size, (long long)actual.st_blocks,
			    (long long)actual.st_atim.tv_sec, actual.st_atim.tv_nsec,
			    (long long)actual.st_mtim.tv_sec, actual.st_mtim.tv_nsec,
			    (long long)actual.st_ctim.tv_sec, actual.st_ctim.tv_nsec);
			power_off(0);
		}
	}
	require(!ferror(input) && fclose(input) == 0, "finish namespace inode expectations");
}

static void
namespace_directory_checks(void)
{
	struct namespace_entry *expected;
	struct dirent *entry;
	struct stat actual;
	DIR *directory;
	FILE *input;
	char path[512];
	char mounted[sizeof(path) + 4];
	unsigned int count;
	unsigned int index;
	unsigned int observed;
	int fields;

	input = fopen("/namespace-directories", "r");
	require(input != NULL, "open independent directory expectations");
	for (;;) {
		fields = fscanf(input, "%511s %u", path, &count);
		if (fields == EOF) {
			break;
		}
		require(fields == 2 && count >= 2 && count <= 4096, "parse directory expectations");
		expected = calloc(count, sizeof(*expected));
		require(expected != NULL, "allocate directory expectation");
		for (index = 0; index < count; index++) {
			require(fscanf(input, "%255s %llu", expected[index].name,
				    &expected[index].inode) == 2,
			    "parse expected directory entry");
		}
		snprintf(mounted, sizeof(mounted), "/mnt%s", path);
		directory = opendir(mounted);
		require(directory != NULL, mounted);
		observed = 0;
		for (;;) {
			errno = 0;
			entry = readdir(directory);
			if (entry == NULL) {
				require(errno == 0, "enumerate directory");
				break;
			}
			for (index = 0; index < count; index++) {
				if (strcmp(entry->d_name, expected[index].name) == 0) {
					break;
				}
			}
			require(index != count && !expected[index].seen &&
				entry->d_ino == expected[index].inode,
			    "compare unique directory entry identity");
			/* Lookup of the mount root's parent crosses into the initramfs.
			 * Its on-disk dotdot is still checked by enumeration above. */
			if (strcmp(path, "/") != 0 || strcmp(entry->d_name, "..") != 0) {
				require(fstatat(dirfd(directory), entry->d_name, &actual,
					    AT_SYMLINK_NOFOLLOW) == 0 &&
					actual.st_ino == entry->d_ino,
				    "verify lookup agrees with enumeration");
			}
			expected[index].seen = true;
			observed++;
		}
		require(observed == count, "verify complete directory enumeration");
		require(closedir(directory) == 0, "close directory");
		free(expected);
	}
	require(!ferror(input) && fclose(input) == 0, "finish directory expectations");
}

static void
namespace_data_checks(void)
{
	FILE *input;
	uint8_t expected[1024];
	uint8_t actual[sizeof(expected)];
	char path[512];
	char mounted[sizeof(path) + 4];
	char oracle[64];
	unsigned int index = 0;
	ssize_t length;
	int fd;
	int reference;

	input = fopen("/namespace-files", "r");
	require(input != NULL, "open independent file expectations");
	while (fscanf(input, "%511s", path) == 1) {
		snprintf(mounted, sizeof(mounted), "/mnt%s", path);
		snprintf(oracle, sizeof(oracle), "/expected/%u", index++);
		fd = open(mounted, O_RDONLY | O_CLOEXEC);
		reference = open(oracle, O_RDONLY | O_CLOEXEC);
		require(fd >= 0 && reference >= 0, "open namespace file and independent oracle");
		for (;;) {
			length = read(reference, expected, sizeof(expected));
			require(length >= 0, "read independent file contents");
			require(read(fd, actual, length > 0 ? (size_t)length : 1) == length &&
				memcmp(actual, expected, (size_t)length) == 0,
			    "compare complete namespace file contents including holes");
			if (length == 0) {
				break;
			}
		}
		require(close(fd) == 0 && close(reference) == 0, "close namespace comparisons");
	}
	require(feof(input) && !ferror(input) && fclose(input) == 0, "finish file expectations");
}

static void
namespace_symlink_checks(uint32_t block_size)
{
	struct stat inode;
	FILE *input;
	uint8_t *expected;
	uint8_t *actual;
	char path[512];
	char mounted[sizeof(path) + 4];
	char oracle[64];
	size_t capacities[] = { 1, TEST_SYMLINK_INLINE_CAPACITY - 1, TEST_SYMLINK_INLINE_CAPACITY,
		1024, 0 };
	size_t length;
	size_t completed;
	size_t index;
	unsigned int count = 0;
	int fields;
	int fd;

	input = fopen("/namespace-links", "r");
	require(input != NULL, "open independent symlink expectations");
	expected = malloc(block_size + 1);
	actual = malloc(block_size + 1);
	require(expected != NULL && actual != NULL, "allocate bounded readlink buffers");
	capacities[4] = block_size;
	for (;;) {
		fields = fscanf(input, "%511s %63s", path, oracle);
		if (fields == EOF) {
			break;
		}
		require(fields == 2, "parse independent symlink expectation");
		snprintf(mounted, sizeof(mounted), "/mnt%s", path);
		require(lstat(mounted, &inode) == 0 && S_ISLNK(inode.st_mode) &&
			inode.st_size > 0 && inode.st_size < block_size,
		    "verify symlink type and bounded length");
		length = (size_t)inode.st_size;
		fd = open(oracle, O_RDONLY | O_CLOEXEC);
		require(fd >= 0 && read(fd, expected, block_size + 1) == (ssize_t)length &&
			close(fd) == 0,
		    "read independent opaque symlink target");
		for (index = 0; index < sizeof(capacities) / sizeof(capacities[0]); index++) {
			completed = length < capacities[index] ? length : capacities[index];
			memset(actual, 0xa5, block_size + 1);
			require(readlink(mounted, (char *)actual, capacities[index]) ==
				    (ssize_t)completed &&
				memcmp(actual, expected, completed) == 0 &&
				actual[completed] == 0xa5,
			    "compare exact and truncated readlink without a returned NUL");
		}
		count++;
	}
	require(!ferror(input) && fclose(input) == 0, "finish symlink expectations");
	free(actual);
	free(expected);
	printf("LINUX_EXT4_NAMESPACE_SYMLINKS count=%u\n", count);
}

static void
namespace_exhausted_name(char *path, size_t capacity, unsigned int index)
{
	int length;
	size_t prefix = sizeof("/mnt/") - 1;

	length = snprintf(path, capacity, "/mnt/node-%08u", index);
	require(length > 0 && (size_t)length < prefix + EXT4_NAME_MAX &&
		capacity > prefix + EXT4_NAME_MAX,
	    "format exhausted inode name");
	memset(path + length, 'n', prefix + EXT4_NAME_MAX - (size_t)length);
	path[prefix + EXT4_NAME_MAX] = 0;
}

static void
check_namespace(uint32_t block_size)
{
	struct stat released;
	struct stat created;
	struct statvfs counts;
	FILE *input;
	char path[EXT4_NAME_MAX + sizeof("/mnt/")];
	char link_target[64];
	uint8_t payload[73];
	unsigned int exhaust;
	unsigned int basic;
	unsigned int index;
	int fd;
	int directory;
	int root;

	input = fopen("/namespace-options", "r");
	require(input != NULL, "open namespace options");
	require(fscanf(input, "%u %u", &exhaust, &basic) == 2, "read namespace options");
	require(fclose(input) == 0, "close namespace options");
	namespace_inode_checks();
	namespace_directory_checks();
	namespace_data_checks();
	namespace_symlink_checks(block_size);
	if (basic) {
		require(readlink("/mnt/hello-link", link_target, sizeof(link_target)) == 9 &&
			memcmp(link_target, "hello.txt", 9) == 0,
		    "verify original symlink target");
		require(readlink("/mnt/symlink-alias", link_target, sizeof(link_target)) == 9 &&
			memcmp(link_target, "hello.txt", 9) == 0,
		    "verify hardlink to symlink");
	}
	puts("LINUX_EXT4_NAMESPACE_PASS");
	if (exhaust) {
		require(statvfs("/mnt", &counts) == 0 && counts.f_ffree == 0,
		    "verify Linux sees inode exhaustion");
		errno = 0;
		fd = open("/mnt/linux-file", O_CREAT | O_EXCL | O_RDWR | O_CLOEXEC, TEST_MODE);
		require(fd == -1 && errno == ENOSPC, "verify Linux create inode exhaustion");
		errno = 0;
		require(mkdir("/mnt/linux-dir", 0750) == -1 && errno == ENOSPC,
		    "verify Linux mkdir inode exhaustion");
		namespace_exhausted_name(path, sizeof(path), 1);
		require(stat(path, &released) == 0 && unlink(path) == 0,
		    "release one core-allocated inode in Linux");
	}
	fd = open("/mnt/linux-file", O_CREAT | O_EXCL | O_RDWR | O_CLOEXEC, TEST_MODE);
	require(
	    fd >= 0 && fstat(fd, &created) == 0, "Linux create after portable namespace changes");
	if (exhaust) {
		require(created.st_ino == released.st_ino && statvfs("/mnt", &counts) == 0 &&
			counts.f_ffree == 0,
		    "verify Linux reused the sole free inode");
		for (index = 2; index <= 3; index++) {
			namespace_exhausted_name(path, sizeof(path), index);
			require(unlink(path) == 0,
			    "release namespace capacity for Linux directory and symlink");
		}
		puts("LINUX_EXT4_NAMESPACE_REUSE_PASS");
	}
	require(mkdir("/mnt/linux-dir", 0750) == 0, "Linux mkdir after portable allocation");
	require(link("/mnt/linux-file", "/mnt/linux-link") == 0, "Linux cross-directory hardlink");
	require(rename("/mnt/linux-file", "/mnt/linux-dir/renamed") == 0, "Linux rename new inode");
	require(symlink("linux-dir/renamed", "/mnt/linux-symlink") == 0, "Linux create symlink");
	require(fchown(fd, TEST_UID, TEST_GID) == 0 && fchmod(fd, TEST_MODE) == 0,
	    "Linux set created file owners and permissions");
	for (index = 0; index < sizeof(payload); index++) {
		payload[index] = (uint8_t)(index * 13 + TEST_LINUX_BYTE);
	}
	require(pwrite(fd, payload, sizeof(payload), (off_t)block_size + 3) == sizeof(payload),
	    "Linux sparse write to created file");
	require(fsync(fd) == 0, "Linux fsync namespace data and metadata");
	directory = open("/mnt/linux-dir", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	root = open("/mnt", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	require(directory >= 0 && root >= 0 && fsync(directory) == 0 && fsync(root) == 0,
	    "Linux fsync both namespace parents");
	puts("LINUX_EXT4_COMMITTED_RECOVERY_PENDING");
	power_off(1);
}

/* SPDX-License-Identifier: BSD-3-Clause */
#define _DARWIN_C_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <libproc.h>
#include <limits.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <unistd.h>

#define PAGE_BYTES 4096U
#define FILE_BYTES (2U * PAGE_BYTES)
#define ORIGINAL_BYTE 0x35
#define UNCERTAIN_BYTE 0xa7
#define RECOVERED_BYTE 0x59
#define BARRIER_EXECUTABLE                                                                         \
	"/Applications/Machlin ext4.app/Contents/Helpers/Ext4DeviceSetup.app/Contents/Library/"    \
	"LaunchServices/Ext4DeviceBarrier"

#define CHECK(condition)                                                                           \
	do {                                                                                       \
		if (!(condition)) {                                                                \
			fprintf(stderr, "%s:%d: %s (errno=%d: %s)\n", __FILE__, __LINE__,          \
			    #condition, errno, strerror(errno));                                   \
			exit(EXIT_FAILURE);                                                        \
		}                                                                                  \
	} while (0)

static int
io_failure(int result, int error)
{
	return result == -1 &&
	    (error == EIO || error == ENXIO || error == ENODEV || error == ETIMEDOUT);
}

static void
signal_barrier(const char *number, const char *name)
{
	struct proc_bsdinfo information;
	char path[PROC_PIDPATHINFO_MAXSIZE];
	char *end;
	long parsed;
	pid_t pid;
	int operation;

	CHECK(geteuid() == 0);
	errno = 0;
	parsed = strtol(number, &end, 10);
	CHECK(errno == 0 && end != number && *end == '\0' && parsed > 1 && parsed <= INT_MAX);
	pid = (pid_t)parsed;
	operation = strcmp(name, "STOP") == 0 ? SIGSTOP
	    : strcmp(name, "CONT") == 0	      ? SIGCONT
	    : strcmp(name, "KILL") == 0	      ? SIGKILL
					      : 0;
	CHECK(operation != 0 && pid != getpid());
	CHECK(proc_pidinfo(pid, PROC_PIDTBSDINFO, 0, &information, sizeof(information)) ==
	    sizeof(information));
	CHECK(information.pbi_uid == 0 && information.pbi_pid == (uint32_t)pid);
	/* launchd can use a relative argv[0]. Inspect the kernel's executable path,
	 * not ps comm/args, and never print any process environment or credentials. */
	CHECK(proc_pidpath(pid, path, sizeof(path)) > 0);
	CHECK(strcmp(path, BARRIER_EXECUTABLE) == 0);
	CHECK(kill(pid, operation) == 0);
	printf("{\"pid\":%d,\"signal\":\"%s\",\"executable\":\"%s\"}\n", pid, name, path);
}

static void
receive(const char *expected)
{
	char command[32];

	CHECK(fgets(command, sizeof(command), stdin) != NULL);
	CHECK(strcmp(command, expected) == 0);
}

static int
exercise_failure(int root)
{
	uint8_t data[FILE_BYTES];
	int fd;
	ssize_t written;
	int write_error;
	int synced;
	int sync_error;
	int retry;
	int retry_error;
	int resized;
	int resize_error;
	int closed;
	int close_error;
	int passed;

	fd = openat(root, "barrier-failure", O_CREAT | O_EXCL | O_RDWR | O_NOFOLLOW, 0600);
	CHECK(fd >= 0);
	memset(data, ORIGINAL_BYTE, sizeof(data));
	CHECK(pwrite(fd, data, sizeof(data), 0) == sizeof(data));
	CHECK(fsync(fd) == 0 && fsync(root) == 0);
	/* The controller injects the service fault only after a durable baseline
	 * and while this descriptor remains open. No root file operations run here. */
	printf("{\"phase\":\"ready\",\"pid\":%d}\n", getpid());
	CHECK(fflush(stdout) == 0);
	receive("fault\n");
	memset(data, UNCERTAIN_BYTE, PAGE_BYTES);
	errno = 0;
	written = pwrite(fd, data, PAGE_BYTES, 0);
	write_error = errno;
	errno = 0;
	synced = fsync(fd);
	sync_error = errno;
	printf("{\"phase\":\"fault\",\"written\":%lld,\"write_errno\":%d,"
	       "\"fsync\":%d,\"fsync_errno\":%d}\n",
	    (long long)written, write_error, synced, sync_error);
	CHECK(fflush(stdout) == 0);
	receive("restored\n");
	/* Restoring a service cannot make an uncertain transaction safe. The same
	 * mounted owner must retain its error, including for a new mutation. */
	errno = 0;
	retry = fsync(fd);
	retry_error = errno;
	errno = 0;
	resized = ftruncate(fd, FILE_BYTES + PAGE_BYTES);
	resize_error = errno;
	errno = 0;
	closed = close(fd);
	close_error = errno;
	passed =
	    ((written >= 0 && written <= PAGE_BYTES) || io_failure((int)written, write_error)) &&
	    io_failure(synced, sync_error) && io_failure(retry, retry_error) &&
	    io_failure(resized, resize_error);
	printf("{\"phase\":\"finished\",\"fsync\":%d,\"fsync_errno\":%d,"
	       "\"truncate\":%d,\"truncate_errno\":%d,\"close\":%d,\"close_errno\":%d,"
	       "\"passed\":%s}\n",
	    retry, retry_error, resized, resize_error, closed, close_error,
	    passed ? "true" : "false");
	CHECK(fflush(stdout) == 0);
	return passed;
}

static void
verify_recovery(int root, int writable)
{
	uint8_t data[FILE_BYTES];
	struct stat status;
	size_t index;
	int fd;
	int proof;

	fd = openat(root, "barrier-failure", O_RDONLY | O_NOFOLLOW);
	CHECK(fd >= 0 && fstat(fd, &status) == 0 && status.st_size == FILE_BYTES);
	CHECK(pread(fd, data, sizeof(data), 0) == sizeof(data));
	for (index = 0; index < sizeof(data); index++) {
		/* An unacknowledged overwrite can survive or disappear. It cannot
		 * corrupt the untouched, previously synced half of the file. */
		CHECK(data[index] == ORIGINAL_BYTE ||
		    (index < PAGE_BYTES && data[index] == UNCERTAIN_BYTE));
	}
	CHECK(close(fd) == 0);
	if (writable) {
		proof =
		    openat(root, "barrier-recovered", O_CREAT | O_EXCL | O_RDWR | O_NOFOLLOW, 0600);
		CHECK(proof >= 0);
		memset(data, RECOVERED_BYTE, sizeof(data));
		CHECK(pwrite(proof, data, sizeof(data), 0) == sizeof(data));
		CHECK(fsync(proof) == 0 && close(proof) == 0 && fsync(root) == 0);
	} else {
		proof = openat(root, "barrier-recovered", O_RDONLY | O_NOFOLLOW);
		CHECK(proof >= 0 && fstat(proof, &status) == 0 && status.st_size == FILE_BYTES);
		CHECK(pread(proof, data, sizeof(data), 0) == sizeof(data));
		for (index = 0; index < sizeof(data); index++) {
			CHECK(data[index] == RECOVERED_BYTE);
		}
		CHECK(close(proof) == 0);
	}
	puts("{\"phase\":\"verified\",\"passed\":true}");
}

int
main(int argc, char **argv)
{
	struct statfs filesystem;
	int root;
	int writable;
	int passed = 1;
	int closed;
	int close_error;

	if (argc == 4 && strcmp(argv[1], "--barrier-signal") == 0) {
		signal_barrier(argv[2], argv[3]);
		return EXIT_SUCCESS;
	}
	CHECK(argc == 3 &&
	    (strcmp(argv[2], "fault") == 0 || strcmp(argv[2], "recover") == 0 ||
		strcmp(argv[2], "verify") == 0));
	CHECK(geteuid() != 0);
	root = open(argv[1], O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
	CHECK(root >= 0 && fstatfs(root, &filesystem) == 0);
	CHECK(strcmp(filesystem.f_fstypename, "machlinext4") == 0);
	CHECK((filesystem.f_flags & MNT_IGNORE_OWNERSHIP) == 0);
	writable = strcmp(argv[2], "verify") != 0;
	CHECK(((filesystem.f_flags & MNT_RDONLY) == 0) == writable);
	if (strcmp(argv[2], "fault") == 0) {
		passed = exercise_failure(root);
	} else {
		verify_recovery(root, writable);
	}
	errno = 0;
	closed = close(root);
	close_error = errno;
	CHECK(closed == 0 || (strcmp(argv[2], "fault") == 0 && io_failure(closed, close_error)));
	return passed ? EXIT_SUCCESS : EXIT_FAILURE;
}

/* SPDX-License-Identifier: BSD-3-Clause */
#define _GNU_SOURCE
#include <ext4/ext4.h>

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/reboot.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/utsname.h>
#include <time.h>
#include <unistd.h>

/* One writer at a time, on identical disposable ext4 volumes. Both acknowledge
 * writes before durability and commit every SYNC_BYTES, including the final batch.
 * Timings include that durability work. Native adapters need separate acceptance. */
#define FILE_BYTES (16U * 1024U * 1024U)
#define SAMPLE_BYTES (256U * 1024U * 1024U)
#define SYNC_BYTES (1024U * 1024U)
#define SEQUENTIAL_BYTES (64U * 1024U)
#define RANDOM_BYTES 4096U
#define SAMPLES 7U
#define PERMUTATION_MULTIPLIER 4051U
#define PERMUTATION_ADDEND 17U

struct device {
	int fd;
	uint64_t reads;
	uint64_t read_bytes;
	uint64_t writes;
	uint64_t write_bytes;
	uint64_t flushes;
	uint64_t allocations;
	uint64_t live_bytes;
	uint64_t peak_bytes;
};

struct writer {
	struct device device;
	struct ext4_fs *fs;
	struct ext4_inode inode;
	struct ext4_inode_hold *hold;
	int fd;
	uint8_t *data;
	uint8_t *read_buffer;
};

struct device_io {
	unsigned long long reads;
	unsigned long long read_sectors;
	unsigned long long writes;
	unsigned long long write_sectors;
	unsigned long long flushes;
};

static _Noreturn void
finish(bool passed)
{
	printf("LINUX_WRITE_RESULT=%s\n", passed ? "PASS" : "FAIL");
	fflush(stdout);
	fflush(stderr);
	reboot(RB_POWER_OFF);
	for (;;) {
		pause();
	}
}

static void
require(bool condition, const char *operation)
{
	if (!condition) {
		fprintf(stderr, "%s: %s\n", operation, strerror(errno));
		finish(false);
	}
}

static uint64_t
now(clockid_t clock)
{
	struct timespec time;

	require(clock_gettime(clock, &time) == 0, "read clock");
	return (uint64_t)time.tv_sec * UINT64_C(1000000000) + (uint64_t)time.tv_nsec;
}

static void
expect(enum ext4_result error, const char *operation)
{
	if (error != EXT4_OK) {
		fprintf(stderr, "%s: %s\n", operation, ext4_result_string(error));
		finish(false);
	}
}

static struct device_io
device_io(bool core)
{
	struct device_io result = { 0 };
	FILE *file = fopen("/proc/diskstats", "r");
	char line[512];
	char name[32];
	bool found = false;

	require(file != NULL, "open disk accounting");
	while (fgets(line, sizeof(line), file) != NULL) {
		if (sscanf(line,
			"%*u %*u %31s %llu %*u %llu %*u %llu %*u %llu "
			"%*u %*u %*u %*u %*u %*u %*u %*u %llu",
			name, &result.reads, &result.read_sectors, &result.writes,
			&result.write_sectors, &result.flushes) == 6 &&
		    strcmp(name, core ? "vdb" : "vda") == 0) {
			found = true;
			break;
		}
	}
	require(fclose(file) == 0 && found, "read device accounting");
	return result;
}

static bool
transfer(int fd, uint64_t offset, void *buffer, size_t length, bool write)
{
	uint8_t *bytes = buffer;
	size_t completed = 0;
	ssize_t count;

	while (completed < length) {
		count = write
		    ? pwrite(fd, bytes + completed, length - completed, (off_t)(offset + completed))
		    : pread(fd, bytes + completed, length - completed, (off_t)(offset + completed));
		if (count < 0 && errno == EINTR) {
			continue;
		}
		if (count <= 0) {
			return false;
		}
		completed += (size_t)count;
	}
	return true;
}

static enum ext4_result
device_read(void *context, uint64_t offset, void *buffer, size_t length)
{
	struct device *device = context;

	device->reads++;
	device->read_bytes += length;
	return transfer(device->fd, offset, buffer, length, false) ? EXT4_OK : EXT4_IO;
}

static enum ext4_result
device_write(void *context, uint64_t offset, const void *buffer, size_t length)
{
	struct device *device = context;

	device->writes++;
	device->write_bytes += length;
	return transfer(device->fd, offset, (void *)buffer, length, true) ? EXT4_OK : EXT4_IO;
}

static enum ext4_result
device_flush(void *context)
{
	struct device *device = context;

	device->flushes++;
	return fsync(device->fd) == 0 ? EXT4_OK : EXT4_IO;
}

static void *
allocate(void *context, size_t size)
{
	struct device *device = context;
	void *memory = malloc(size);

	device->allocations++;
	if (memory != NULL) {
		device->live_bytes += size;
		if (device->live_bytes > device->peak_bytes) {
			device->peak_bytes = device->live_bytes;
		}
	}
	return memory;
}

static void
release(void *context, void *memory, size_t size)
{
	struct device *device = context;

	require(memory != NULL && device->live_bytes >= size, "allocation ownership");
	device->live_bytes -= size;
	free(memory);
}

static void
read_file(struct writer *writer, bool core, bool verify)
{
	uint64_t offset;
	size_t completed;

	if (core) {
		expect(ext4_get_inode(writer->fs, writer->inode.number, &writer->inode),
		    "refresh core snapshot");
	}
	for (offset = 0; offset < FILE_BYTES; offset += SEQUENTIAL_BYTES) {
		if (core) {
			expect(ext4_read(writer->fs, &writer->inode, offset, writer->read_buffer,
				   SEQUENTIAL_BYTES, &completed),
			    "read core file");
			require(completed == SEQUENTIAL_BYTES, "core read length");
		} else {
			require(transfer(writer->fd, offset, writer->read_buffer, SEQUENTIAL_BYTES,
				    false),
			    "read Linux file");
		}
		if (verify) {
			require(memcmp(writer->read_buffer, writer->data + offset,
				    SEQUENTIAL_BYTES) == 0,
			    "independent expected file bytes");
		}
	}
}

static void
durable(struct writer *writer, bool core)
{
	if (core) {
		expect(ext4_commit(writer->fs), "durable core commit");
	} else {
		require(fsync(writer->fd) == 0, "durable Linux fsync");
	}
}

static void
sample(struct writer *writer, bool core, bool random, unsigned int iteration)
{
	struct ext4_inode_update update = { 0 };
	struct timespec timestamp;
	struct device before;
	struct device_io io_before;
	struct device_io io_after;
	size_t request = random ? RANDOM_BYTES : SEQUENTIAL_BYTES;
	uint64_t blocks = FILE_BYTES / request;
	uint64_t offset;
	uint64_t start;
	uint64_t cpu;
	uint64_t elapsed;
	uint64_t index;
	size_t completed;

	/* Warm each contender's own file/device path. Nothing is written or made
	 * durable by this warmup; all sample writes and the final fsync are timed. */
	read_file(writer, core, false);
	update.fields = EXT4_ATTR_PERMISSIONS | EXT4_ATTR_MODIFY_TIME | EXT4_ATTR_CHANGE_TIME |
	    EXT4_ATTR_XATTRS;
	update.permissions = writer->inode.mode & ~(uint16_t)EXT4_MODE_TYPE;
	writer->device.peak_bytes = writer->device.live_bytes;
	before = writer->device;
	io_before = device_io(core);
	cpu = now(CLOCK_PROCESS_CPUTIME_ID);
	start = now(CLOCK_MONOTONIC_RAW);
	for (index = 0; index < SAMPLE_BYTES / request; index++) {
		offset = (random ? (index * PERMUTATION_MULTIPLIER + PERMUTATION_ADDEND) % blocks
				 : index % blocks) *
		    request;
		/* Both contenders pay for the caller's timestamp acquisition. */
		require(clock_gettime(CLOCK_REALTIME, &timestamp) == 0, "write timestamp");
		update.modify_time =
		    (struct ext4_timestamp){ timestamp.tv_sec, (uint32_t)timestamp.tv_nsec };
		update.change_time = update.modify_time;
		if (core) {
			expect(
			    ext4_write(writer->fs, writer->inode.number, writer->inode.generation,
				offset, writer->data + offset, request, &update, &completed),
			    "core overwrite");
			require(completed == request, "core write length");
		} else {
			require(transfer(writer->fd, offset, writer->data + offset, request, true),
			    "Linux overwrite");
		}
		if ((index + 1U) % (SYNC_BYTES / request) == 0) {
			durable(writer, core);
		}
	}
	elapsed = now(CLOCK_MONOTONIC_RAW) - start;
	cpu = now(CLOCK_PROCESS_CPUTIME_ID) - cpu;
	io_after = device_io(core);
	printf("WRITE_SAMPLE {\"writer\":\"%s\",\"access\":\"%s\",\"sample\":%u,"
	       "\"request_bytes\":%zu,\"bytes\":%u,\"sync_bytes\":%u,\"elapsed_ns\":%" PRIu64
	       ",\"cpu_ns\":%" PRIu64 ",\"read_callbacks\":%" PRIu64 ",\"read_bytes\":%" PRIu64
	       ",\"write_callbacks\":%" PRIu64 ",\"write_bytes\":%" PRIu64 ",\"flushes\":%" PRIu64
	       ",\"core_allocations\":%" PRIu64 ",\"core_peak_live_bytes\":%" PRIu64
	       ",\"device_reads\":%llu,\"device_read_sectors\":%llu,\"device_writes\":%llu,"
	       "\"device_write_sectors\":%llu,\"device_flushes\":%llu}\n",
	    core ? "core" : "linux", random ? "random" : "sequential", iteration, request,
	    SAMPLE_BYTES, SYNC_BYTES, elapsed, cpu, writer->device.reads - before.reads,
	    writer->device.read_bytes - before.read_bytes, writer->device.writes - before.writes,
	    writer->device.write_bytes - before.write_bytes,
	    writer->device.flushes - before.flushes,
	    writer->device.allocations - before.allocations, core ? writer->device.peak_bytes : 0,
	    io_after.reads - io_before.reads, io_after.read_sectors - io_before.read_sectors,
	    io_after.writes - io_before.writes, io_after.write_sectors - io_before.write_sectors,
	    io_after.flushes - io_before.flushes);
	read_file(writer, core, true);
}

int
main(void)
{
	static const char *modules[] = { "virtio_blk", "crc32c_generic", "crc16", "mbcache", "jbd2",
		"ext4" };
	struct writer writer = { 0 };
	struct ext4_environment environment;
	struct ext4_write_environment writes;
	struct ext4_write_options options = {
		.commit_blocks = 256, .checkpoint_blocks = 512, .flags = EXT4_WRITE_ORDERED_DATA
	};
	struct ext4_inode root;
	struct utsname identity;
	struct stat status;
	cpu_set_t cpus;
	char path[128];
	uint64_t size = 0;
	size_t byte;
	unsigned int index;
	unsigned int iteration;
	unsigned int contender;
	unsigned int random;
	int fd;
	int result;

	if (getpid() != 1) {
		fprintf(stderr, "Run only as PID 1 in the dedicated disposable Linux guest\n");
		return 2;
	}
	setvbuf(stdout, NULL, _IOLBF, 0);
	require(uname(&identity) == 0, "uname");
	printf(
	    "LINUX_WRITE_KERNEL=%s %s %s\n", identity.sysname, identity.release, identity.machine);
	puts("WRITE_CONTRACT=preallocated warm overwrites; ordered data; "
	     "durable fsync/commit each 1 MiB included in timing; "
	     "same guest and initial images; library versus VFS; no native-adapter claim");
	puts("CORE_WRITE_INODE=ext4_hold_inode for the Linux file descriptor lifetime; "
	     "current inode contents and mutation policy checked on every write");
	CPU_ZERO(&cpus);
	CPU_SET(0, &cpus);
	require(sched_setaffinity(0, sizeof(cpus), &cpus) == 0, "pin benchmark CPU");
	require(mount("devtmpfs", "/dev", "devtmpfs", 0, NULL) == 0 &&
		mount("proc", "/proc", "proc", 0, NULL) == 0,
	    "mount guest interfaces");
	for (index = 0; index < sizeof(modules) / sizeof(modules[0]); index++) {
		snprintf(path, sizeof(path), "/modules/%s.ko", modules[index]);
		fd = open(path, O_RDONLY | O_CLOEXEC);
		require(fd >= 0, "open module");
		result = (int)syscall(SYS_finit_module, fd, "", 0);
		require(result == 0 || errno == EEXIST, path);
		require(close(fd) == 0, "close module");
	}
	require(mount("/dev/vda", "/mnt", "ext4", MS_NOATIME, "data=ordered") == 0,
	    "mount Linux writer");
	writer.fd = open("/mnt/contiguous.bin", O_RDWR | O_CLOEXEC);
	require(writer.fd >= 0 && fstat(writer.fd, &status) == 0 && status.st_size == FILE_BYTES,
	    "open preallocated Linux file");
	writer.device.fd = open("/dev/vdb", O_RDWR | O_CLOEXEC);
	require(writer.device.fd >= 0 && ioctl(writer.device.fd, BLKGETSIZE64, &size) == 0,
	    "open exclusive core device");
	environment =
	    (struct ext4_environment){ &writer.device, size, device_read, allocate, release };
	writes =
	    (struct ext4_write_environment){ &writer.device, device_write, device_flush, NULL };
	expect(ext4_mount_writable_with_options(&environment, &writes, NULL, &options, &writer.fs),
	    "mount core writer");
	expect(ext4_get_inode(writer.fs, EXT4_ROOT_INODE, &root), "read core root");
	expect(ext4_lookup(writer.fs, &root, (const uint8_t *)"contiguous.bin", 14, &writer.inode),
	    "open core file");
	require(writer.inode.size == FILE_BYTES, "preallocated core file size");
	expect(
	    ext4_hold_inode(writer.fs, writer.inode.number, writer.inode.generation, &writer.hold),
	    "hold core file");
	require(posix_memalign((void **)&writer.data, RANDOM_BYTES, FILE_BYTES) == 0 &&
		posix_memalign((void **)&writer.read_buffer, RANDOM_BYTES, SEQUENTIAL_BYTES) == 0,
	    "allocate aligned caller buffers");
	for (random = 0; random < 2U; random++) {
		for (iteration = 0; iteration < SAMPLES; iteration++) {
			for (byte = 0; byte < FILE_BYTES; byte++) {
				writer.data[byte] = (uint8_t)(byte * 131U +
				    (byte / RANDOM_BYTES) * 17U + (byte / SEQUENTIAL_BYTES) * 29U +
				    iteration + random * SAMPLES);
			}
			for (contender = 0; contender < 2U; contender++) {
				sample(&writer, (iteration + contender) % 2U != 0, random != 0,
				    iteration);
			}
		}
	}
	expect(ext4_sync(writer.fs), "finish core checkpoint");
	expect(ext4_release_inode(writer.hold), "release core file");
	ext4_unmount(writer.fs);
	require(writer.device.live_bytes == 0 && close(writer.device.fd) == 0 &&
		close(writer.fd) == 0 && umount("/mnt") == 0,
	    "clean writers teardown");
	/* Linux independently reads the completed core volume after its owner releases it. */
	require(mount("/dev/vdb", "/mnt", "ext4", MS_RDONLY | MS_NOATIME, "noload") == 0,
	    "Linux mount core output");
	writer.fd = open("/mnt/contiguous.bin", O_RDONLY | O_CLOEXEC);
	require(writer.fd >= 0, "open core output through Linux");
	read_file(&writer, false, true);
	require(close(writer.fd) == 0 && umount("/mnt") == 0, "close independent validation");
	free(writer.read_buffer);
	free(writer.data);
	finish(true);
}

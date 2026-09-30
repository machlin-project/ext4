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
#include <sys/auxv.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/mount.h>
#include <sys/reboot.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/utsname.h>
#include <time.h>
#include <unistd.h>

/* Both readers use the same immutable ext4 volume and buffered Linux device I/O.
 * This compares a library call with Linux VFS pread, not a mounted native adapter.
 * Cold means guest page caches are dropped, not host/storage caches. */
#define FILE_BYTES (16U * 1024U * 1024U)
#define PAGE_BYTES 4096U
#define SEQUENTIAL_BYTES (1024U * 1024U)
#define SAMPLES 7U
#define WARM_SEQUENTIAL_PASSES 512U
#define COLD_SEQUENTIAL_PASSES 128U
#define WARM_RANDOM_PASSES 64U
#define COLD_RANDOM_PASSES 4U
#define PERMUTATION_MULTIPLIER 4051U
#define PERMUTATION_ADDEND 17U
#define LINUX_SECTOR_BYTES 512U

enum reader_kind {
	READER_LINUX,
	READER_CORE,
	READER_RAW,
	READER_CORE_DEMAND,
	READER_LINUX_MAPPED,
	READER_CORE_MAPPED
};

struct read_source {
	int fd;
	size_t length;
	const uint8_t *mapping;
};

struct device_io {
	unsigned long long reads;
	unsigned long long sectors;
};

struct device {
	struct read_source source;
	uint64_t reads;
	uint64_t bytes;
	uint64_t allocations;
	uint64_t live;
};

struct reader {
	struct device device;
	struct ext4_fs *fs;
	struct ext4_inode inode;
	struct ext4_inode_hold *hold;
	uint64_t raw_offset;
	struct read_source source;
	bool sparse;
	uint8_t *buffer;
};

static _Noreturn void
finish(bool passed)
{
	printf("LINUX_READ_RESULT=%s\n", passed ? "PASS" : "FAIL");
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
	struct timespec value;

	require(clock_gettime(clock, &value) == 0, "read clock");
	return (uint64_t)value.tv_sec * UINT64_C(1000000000) + (uint64_t)value.tv_nsec;
}

static bool
read_exact(int fd, uint64_t offset, void *buffer, size_t size)
{
	uint8_t *bytes = buffer;
	size_t done = 0;
	ssize_t count;

	while (done < size) {
		count = pread(fd, bytes + done, size - done, (off_t)(offset + done));
		if (count < 0 && errno == EINTR) {
			continue;
		}
		if (count <= 0) {
			return false;
		}
		done += (size_t)count;
	}
	return true;
}

static enum ext4_result
device_read(void *context, uint64_t offset, void *buffer, size_t size)
{
	struct device *device = context;

	device->reads++;
	device->bytes += size;
	if (device->source.mapping != NULL) {
		require(offset <= device->source.length && size <= device->source.length - offset,
		    "mapped device bounds");
		memcpy(buffer, device->source.mapping + offset, size);
		return EXT4_OK;
	}
	return read_exact(device->source.fd, offset, buffer, size) ? EXT4_OK : EXT4_IO;
}

static bool
core_reader(enum reader_kind kind)
{
	return kind == READER_CORE || kind == READER_CORE_DEMAND || kind == READER_CORE_MAPPED;
}

static struct read_source *
mapped_source(struct reader *reader, enum reader_kind kind)
{
	if (kind == READER_CORE_MAPPED) {
		return &reader->device.source;
	}
	return kind == READER_LINUX_MAPPED ? &reader->source : NULL;
}

static void
source_map(struct read_source *source, bool random)
{
	void *mapping;

	if (source == NULL) {
		return;
	}
	require(source->mapping == NULL, "mapping ownership");
	mapping = mmap(NULL, source->length, PROT_READ, MAP_SHARED, source->fd, 0);
	require(mapping != MAP_FAILED, "map diagnostic source");
	source->mapping = mapping;
	require(madvise(mapping, source->length, random ? MADV_RANDOM : MADV_SEQUENTIAL) == 0,
	    "mapped access advice");
}

static void
source_unmap(struct read_source *source)
{
	if (source != NULL && source->mapping != NULL) {
		require(munmap((void *)source->mapping, source->length) == 0, "unmap source");
		source->mapping = NULL;
	}
}

static void *
allocate(void *context, size_t size)
{
	struct device *device = context;
	void *memory = malloc(size);

	device->allocations++;
	if (memory != NULL) {
		device->live++;
	}
	return memory;
}

static void
release(void *context, void *memory, size_t size)
{
	struct device *device = context;

	(void)size;
	device->live--;
	free(memory);
}

static uint8_t
expected_byte(uint64_t offset, bool sparse)
{
	if (sparse && (offset / PAGE_BYTES) % 2U != 0) {
		return 0;
	}
	return (uint8_t)(offset * 131U + (offset / PAGE_BYTES) * 17U +
	    (offset / SEQUENTIAL_BYTES) * 29U + 7U);
}

static void
read_file(struct reader *reader, enum reader_kind kind, uint64_t offset, size_t size)
{
	size_t completed;

	if (core_reader(kind)) {
		require(ext4_read_held(reader->hold, offset, reader->buffer, size, &completed) ==
			    EXT4_OK &&
			completed == size,
		    "core read");
	} else if (kind == READER_RAW) {
		/* Diagnostic lower bound: known contiguous mapping, no filesystem work.
		 * This is never counted as a core result. */
		require(device_read(&reader->device, reader->raw_offset + offset, reader->buffer,
			    size) == EXT4_OK,
		    "raw backend read");
	} else if (kind == READER_LINUX_MAPPED) {
		require(offset <= reader->source.length && size <= reader->source.length - offset,
		    "mapped Linux file bounds");
		memcpy(reader->buffer, reader->source.mapping + offset, size);
	} else {
		require(read_exact(reader->source.fd, offset, reader->buffer, size), "Linux pread");
	}
}

static void
verify_file(struct reader *reader, enum reader_kind kind)
{
	uint64_t offset;
	size_t index;

	for (offset = 0; offset < FILE_BYTES; offset += SEQUENTIAL_BYTES) {
		read_file(reader, kind, offset, SEQUENTIAL_BYTES);
		for (index = 0; index < SEQUENTIAL_BYTES; index++) {
			require(
			    reader->buffer[index] == expected_byte(offset + index, reader->sparse),
			    "independent file contents");
		}
	}
}

static void
drop_caches(void)
{
	int fd = open("/proc/sys/vm/drop_caches", O_WRONLY | O_CLOEXEC);

	/* The probe is PID 1 in a disposable guest and has no writable filesystem. */
	require(fd >= 0 && write(fd, "3\n", 2) == 2 && close(fd) == 0, "drop guest caches");
}

static uint64_t
request_offset(uint64_t index, size_t request, bool random)
{
	uint64_t blocks = FILE_BYTES / request;

	return (random ? (index * PERMUTATION_MULTIPLIER + PERMUTATION_ADDEND) % blocks
		       : index % blocks) *
	    request;
}

static struct device_io
device_io(void)
{
	struct device_io result = { 0 };
	FILE *file = fopen("/proc/diskstats", "r");
	char line[512];
	char name[32];
	bool found = false;

	require(file != NULL, "open disk accounting");
	while (fgets(line, sizeof(line), file) != NULL) {
		/* Linux diskstats: identity, completed/merged reads, sectors read. */
		if (sscanf(line, "%*u %*u %31s %llu %*u %llu", name, &result.reads,
			&result.sectors) == 3 &&
		    strcmp(name, "vda") == 0) {
			found = true;
			break;
		}
	}
	require(fclose(file) == 0 && found, "read device accounting");
	return result;
}

static void
sample(struct reader *reader, const char *name, enum reader_kind kind, bool cold, bool random,
    unsigned int sample_number)
{
	static const char *reader_names[] = { "linux", "core", "raw", "core-demand", "linux-mapped",
		"core-mapped" };
	struct read_source *mapped = mapped_source(reader, kind);
	struct device_io before;
	struct device_io after;
	size_t request = random ? PAGE_BYTES : SEQUENTIAL_BYTES;
	uint64_t count = FILE_BYTES / request;
	uint64_t expected = 0;
	uint64_t witness = 0;
	uint64_t offset = 0;
	uint64_t start;
	uint64_t cpu_start;
	uint64_t elapsed = 0;
	uint64_t cpu = 0;
	uint64_t live;
	uint64_t index;
	size_t byte;
	unsigned int pass;
	unsigned int passes = random ? (cold ? COLD_RANDOM_PASSES : WARM_RANDOM_PASSES)
				     : (cold ? COLD_SEQUENTIAL_PASSES : WARM_SEQUENTIAL_PASSES);

	/* The same full-file warmup, validation and advice precede each contender.
	 * Cold passes then evict guest pages outside the timed region. */
	require(posix_fadvise(reader->source.fd, 0, 0,
		    random ? POSIX_FADV_RANDOM : POSIX_FADV_SEQUENTIAL) == 0 &&
		posix_fadvise(reader->device.source.fd, 0, 0,
		    random || kind == READER_CORE_DEMAND ? POSIX_FADV_RANDOM
							 : POSIX_FADV_SEQUENTIAL) == 0,
	    "matching access advice");
	source_map(mapped, random);
	verify_file(reader, kind);
	live = reader->device.live;
	for (index = 0; index < count; index++) {
		offset = request_offset(index, request, random);
		expected += (uint64_t)expected_byte(offset, reader->sparse) +
		    ((uint64_t)expected_byte(offset + request - 1U, reader->sparse) << 8);
	}
	expected *= passes;
	reader->device.reads = 0;
	reader->device.bytes = 0;
	reader->device.allocations = 0;
	before = device_io();
	for (pass = 0; pass < passes; pass++) {
		if (cold) {
			/* The core's retained metadata must not survive the cold reset.
			 * Lazy snapshot/leaf reconstruction is charged inside the read loop. */
			if (core_reader(kind)) {
				ext4_drop_read_cache(reader->hold);
			}
			/* Mapped pages must lose every PTE reference before cache eviction.
			 * Recreating the mapping and all page faults belong to the timed work. */
			source_unmap(mapped);
			drop_caches();
		}
		cpu_start = now(CLOCK_PROCESS_CPUTIME_ID);
		start = now(CLOCK_MONOTONIC_RAW);
		if (cold) {
			source_map(mapped, random);
		}
		for (index = 0; index < count; index++) {
			offset = request_offset(index, request, random);
			read_file(reader, kind, offset, request);
			witness += (uint64_t)reader->buffer[0] +
			    ((uint64_t)reader->buffer[request - 1U] << 8);
		}
		elapsed += now(CLOCK_MONOTONIC_RAW) - start;
		cpu += now(CLOCK_PROCESS_CPUTIME_ID) - cpu_start;
	}
	after = device_io();
	require(witness == expected && reader->device.live == live, "batch output and ownership");
	for (byte = 0; byte < request; byte++) {
		require(reader->buffer[byte] == expected_byte(offset + byte, reader->sparse),
		    "last read contents");
	}
	source_unmap(mapped);
	printf("READ_SAMPLE {\"file\":\"%s\",\"reader\":\"%s\",\"cache\":\"%s\","
	       "\"access\":\"%s\",\"sample\":%u,\"request_bytes\":%zu,\"bytes\":%" PRIu64
	       ",\"elapsed_ns\":%" PRIu64 ",\"cpu_ns\":%" PRIu64 ",\"core_read_callbacks\":%" PRIu64
	       ",\"core_read_bytes\":%" PRIu64 ",\"core_allocations\":%" PRIu64
	       ",\"device_reads\":%llu,\"device_read_bytes\":%llu}\n",
	    name, reader_names[kind], cold ? "guest-cold" : "warm",
	    random ? "random" : "sequential", sample_number, request, (uint64_t)passes * FILE_BYTES,
	    elapsed, cpu, reader->device.reads, reader->device.bytes, reader->device.allocations,
	    after.reads - before.reads, (after.sectors - before.sectors) * LINUX_SECTOR_BYTES);
}

int
main(void)
{
	static const char *modules[] = { "virtio_blk", "crc32c_generic", "crc16", "mbcache", "jbd2",
		"ext4" };
	static const char *names[] = { "contiguous.bin", "sparse.bin" };

	static const enum reader_kind readers[] = { READER_LINUX, READER_CORE,
#if defined(EXT4_READ_BACKEND_DIAGNOSTICS)
		READER_CORE_DEMAND, READER_LINUX_MAPPED, READER_CORE_MAPPED,
#endif
		READER_RAW };
	struct reader reader = { 0 };
	struct ext4_environment environment = { 0 };
	struct ext4_inode root;
	struct ext4_mapping mapping;
	struct utsname identity;
	struct stat status;
	cpu_set_t cpus;
	char path[128];
	uint64_t device_bytes = 0;
	unsigned int index;
	unsigned int profile;
	unsigned int iteration;
	unsigned int contender;
	unsigned int contenders;
	unsigned int cold;
	unsigned int random;
	int fd;
	int result;

	if (getpid() != 1) {
		fprintf(stderr, "Run only as PID 1 in the dedicated disposable Linux guest\n");
		return 2;
	}
	setvbuf(stdout, NULL, _IOLBF, 0);
	require(uname(&identity) == 0, "uname");
	printf("LINUX_READ_KERNEL=%s %s %s HWCAP=%lx\n", identity.sysname, identity.release,
	    identity.machine, getauxval(AT_HWCAP));
	puts("CORE_READ_API=ext4_read_held metadata_cache=bounded file_data_cache=none "
	     "cold_metadata=discarded_per_pass");
#if defined(EXT4_READ_BACKEND_DIAGNOSTICS)
	puts("READ_BACKENDS=core-demand disables raw-device speculation; "
	     "mapped readers are diagnostics, reset mappings before cold eviction, "
	     "include remap/fault/copy costs, and require a fault-free immutable resource");
#endif
	CPU_ZERO(&cpus);
	CPU_SET(0, &cpus);
	require(sched_setaffinity(0, sizeof(cpus), &cpus) == 0, "pin benchmark CPU");
	require(mount("devtmpfs", "/dev", "devtmpfs", 0, NULL) == 0 &&
		mount("proc", "/proc", "proc", 0, NULL) == 0,
	    "mount guest interfaces");
	for (index = 0; index < sizeof(modules) / sizeof(modules[0]); index++) {
		snprintf(path, sizeof(path), "/modules/%s.ko", modules[index]);
		fd = open(path, O_RDONLY | O_CLOEXEC);
		require(fd >= 0, "open matching module");
		result = (int)syscall(SYS_finit_module, fd, "", 0);
		require(result == 0 || errno == EEXIST, path);
		require(close(fd) == 0, "close module");
	}
	require(mount("/dev/vda", "/mnt", "ext4", MS_RDONLY | MS_NOATIME, "noload") == 0,
	    "mount immutable benchmark image");
	reader.device.source.fd = open("/dev/vda", O_RDONLY | O_CLOEXEC);
	require(reader.device.source.fd >= 0 &&
		ioctl(reader.device.source.fd, BLKGETSIZE64, &device_bytes) == 0 &&
		device_bytes <= SIZE_MAX,
	    "open raw benchmark device");
	reader.device.source.length = (size_t)device_bytes;
	environment = (struct ext4_environment){ &reader.device, device_bytes, device_read,
		allocate, release };
	require(ext4_mount(&environment, &reader.fs) == EXT4_OK &&
		ext4_get_inode(reader.fs, EXT4_ROOT_INODE, &root) == EXT4_OK,
	    "mount portable core");
	require(posix_memalign((void **)&reader.buffer, PAGE_BYTES, SEQUENTIAL_BYTES) == 0,
	    "aligned caller buffer");
	memset(reader.buffer, 0, SEQUENTIAL_BYTES);
	for (profile = 0; profile < sizeof(names) / sizeof(names[0]); profile++) {
		reader.sparse = profile != 0;
		snprintf(path, sizeof(path), "/mnt/%s", names[profile]);
		reader.source.fd = open(path, O_RDONLY | O_CLOEXEC);
		require(reader.source.fd >= 0 && fstat(reader.source.fd, &status) == 0 &&
			status.st_size == FILE_BYTES,
		    "open Linux file");
		reader.source.length = FILE_BYTES;
		require(ext4_lookup(reader.fs, &root, (const uint8_t *)names[profile],
			    strlen(names[profile]), &reader.inode) == EXT4_OK &&
			reader.inode.size == FILE_BYTES,
		    "open core file");
		require(ext4_hold_inode(reader.fs, reader.inode.number, reader.inode.generation,
			    &reader.hold) == EXT4_OK,
		    "hold core file");
		contenders = (unsigned int)(sizeof(readers) / sizeof(readers[0]));
		if (reader.sparse) {
			contenders--;
		}
		if (!reader.sparse) {
			require(ext4_map_read(reader.fs, &reader.inode, 0, FILE_BYTES, &mapping) ==
				    EXT4_OK &&
				!mapping.hole && mapping.length == FILE_BYTES,
			    "contiguous raw diagnostic mapping");
			reader.raw_offset = mapping.device_offset;
		}
		for (cold = 0; cold < 2U; cold++) {
			for (random = 0; random < 2U; random++) {
				for (iteration = 0; iteration < SAMPLES; iteration++) {
					for (contender = 0; contender < contenders; contender++) {
						sample(&reader, names[profile],
						    readers[(iteration + contender) % contenders],
						    cold != 0, random != 0, iteration);
					}
				}
			}
		}
		require(close(reader.source.fd) == 0, "close Linux file");
		require(ext4_release_inode(reader.hold) == EXT4_OK, "release core file");
	}
	ext4_unmount(reader.fs);
	require(
	    reader.device.live == 0 && close(reader.device.source.fd) == 0 && umount("/mnt") == 0,
	    "clean benchmark teardown");
	free(reader.buffer);
	finish(true);
	return 0;
}

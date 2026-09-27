/* SPDX-License-Identifier: BSD-3-Clause */
#include "image.h"

#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define BENCHMARK_BUFFER_SIZE (1024U * 1024U)
#define BENCHMARK_MAX_REPEATS 10000U
#define BENCHMARK_HASH_OFFSET UINT64_C(14695981039346656037)
#define BENCHMARK_HASH_PRIME UINT64_C(1099511628211)

struct directory_result {
	uint64_t entries;
	uint64_t hash;
};

static void
require(bool condition, const char *operation)
{
	if (!condition) {
		fprintf(stderr, "%s\n", operation);
		exit(EXIT_FAILURE);
	}
}

static uint64_t
now(void)
{
	struct timespec time;

	require(clock_gettime(CLOCK_MONOTONIC, &time) == 0, "monotonic clock failed");
	return (uint64_t)time.tv_sec * UINT64_C(1000000000) + (uint64_t)time.tv_nsec;
}

static uint64_t
digest(uint64_t hash, const uint8_t *bytes, size_t length)
{
	size_t index;

	for (index = 0; index < length; index++) {
		hash = (hash ^ bytes[index]) * BENCHMARK_HASH_PRIME;
	}
	return hash;
}

static enum ext4_dir_action
directory_visit(void *context, const struct ext4_dir_entry *entry, uint64_t cookie)
{
	struct directory_result *result = context;
	uint8_t identity[sizeof(entry->inode)];
	size_t index;

	(void)cookie;
	for (index = 0; index < sizeof(identity); index++) {
		identity[index] = (uint8_t)(entry->inode >> (index * 8U));
	}
	result->hash = digest(result->hash, identity, sizeof(identity));
	result->hash = digest(result->hash, entry->name, entry->name_length);
	result->entries++;
	return EXT4_DIR_ACCEPT;
}

static struct ext4_inode
resolve(struct ext4_fs *fs, const char *path)
{
	struct ext4_inode inode;
	struct ext4_inode next;
	const char *end;

	require(ext4_get_inode(fs, EXT4_ROOT_INODE, &inode) == EXT4_OK, "root inode failed");
	while (*path != 0) {
		if (*path == '/') {
			path++;
			continue;
		}
		end = strchr(path, '/');
		if (end == NULL) {
			end = path + strlen(path);
		}
		require(ext4_lookup(fs, &inode, (const uint8_t *)path, (size_t)(end - path),
			    &next) == EXT4_OK,
		    "path lookup failed (symlink traversal is not part of this benchmark)");
		inode = next;
		path = end;
	}
	return inode;
}

static int
compare_time(const void *left, const void *right)
{
	uint64_t a = *(const uint64_t *)left;
	uint64_t b = *(const uint64_t *)right;

	return (a > b) - (a < b);
}

int
main(int argc, char **argv)
{
	struct ext4_posix_image image;
	struct ext4_fs *fs = NULL;
	struct ext4_inode inode;
#ifdef EXT4_BENCH_SINGLE_ENTRY
	struct ext4_dir_entry entry;
#endif
	struct directory_result result;
	uint8_t *buffer;
	uint64_t *times;
	uint64_t reads = 0;
	uint64_t allocations = 0;
	uint64_t expected_hash = 0;
	uint64_t expected_units = 0;
	uint64_t live;
	uint64_t offset;
	uint64_t start;
	uint64_t elapsed;
	uint64_t cookie;
	uint64_t hash;
	uint64_t units;
	size_t completed;
	size_t request;
	size_t iteration;
	char *end;
	unsigned long repeats;
	bool directory;
	enum ext4_result error;

	if (argc != 4) {
		fprintf(stderr, "usage: %s IMAGE PATH REPEATS\n", argv[0]);
		return 2;
	}
	errno = 0;
	repeats = strtoul(argv[3], &end, 10);
	require(errno == 0 && *argv[3] != 0 && *end == 0 && repeats > 0 &&
		repeats <= BENCHMARK_MAX_REPEATS,
	    "invalid repeat count");
	require(ext4_posix_open(&image, argv[1]) == EXT4_OK, "read-only image open failed");
	require(ext4_mount(&image.environment, &fs) == EXT4_OK, "read-only mount failed");
	inode = resolve(fs, argv[2]);
	directory = (inode.mode & EXT4_MODE_TYPE) == EXT4_MODE_DIRECTORY;
	require(directory || (inode.mode & EXT4_MODE_TYPE) == EXT4_MODE_REGULAR,
	    "expected regular file or directory");
	buffer = malloc(BENCHMARK_BUFFER_SIZE);
	times = calloc(repeats, sizeof(*times));
	require(buffer != NULL && times != NULL, "benchmark allocation failed");
	live = image.live_allocations;
	/* One untimed warmup. Subsequent timings include only core file-read calls;
	 * directory timings include the identical visitor digest on both paths. */
	for (iteration = 0; iteration <= repeats; iteration++) {
		image.read_calls = 0;
		image.allocation_calls = 0;
		elapsed = 0;
		hash = BENCHMARK_HASH_OFFSET;
		units = 0;
		if (directory) {
			result = (struct directory_result){ .hash = BENCHMARK_HASH_OFFSET };
			cookie = 0;
			start = now();
#ifdef EXT4_BENCH_SINGLE_ENTRY
			while ((error = ext4_next_dir(fs, &inode, &cookie, &entry)) == EXT4_OK) {
				directory_visit(&result, &entry, cookie);
			}
#else
			error = ext4_iterate_dir(fs, &inode, &cookie, directory_visit, &result);
#endif
			elapsed = now() - start;
			require(error == EXT4_NOT_FOUND && cookie == inode.size,
			    "directory enumeration failed");
			hash = result.hash;
			units = result.entries;
		} else {
			for (offset = 0; offset < inode.size; offset += completed) {
				request = BENCHMARK_BUFFER_SIZE;
				if (inode.size - offset < request) {
					request = (size_t)(inode.size - offset);
				}
				start = now();
				error = ext4_read(fs, &inode, offset, buffer, request, &completed);
				elapsed += now() - start;
				require(
				    error == EXT4_OK && completed == request, "file read failed");
				hash = digest(hash, buffer, completed);
				units += completed;
			}
		}
		require(image.live_allocations == live && image.write_calls == 0 &&
			image.flush_calls == 0,
		    "read-only resource balance failed");
		if (iteration == 0) {
			expected_hash = hash;
			expected_units = units;
			reads = image.read_calls;
			allocations = image.allocation_calls;
		} else {
			require(hash == expected_hash && units == expected_units &&
				reads == image.read_calls && allocations == image.allocation_calls,
			    "repeated work changed");
			times[iteration - 1U] = elapsed;
		}
	}
	qsort(times, repeats, sizeof(*times), compare_time);
	printf("{\"kind\":\"%s\",\"warmup_runs\":1,\"measured_runs\":%lu,\"units\":%" PRIu64
	       ",\"digest\":\"%016" PRIx64 "\",\"read_callbacks\":%" PRIu64
	       ",\"allocations\":%" PRIu64 ",\"min_ns\":%" PRIu64 ",\"median_ns\":%" PRIu64
	       ",\"p95_ns\":%" PRIu64 ",\"writes\":0,\"balanced_allocations\":true}\n",
	    directory ? "directory_entries" : "file_bytes", repeats, expected_units, expected_hash,
	    reads, allocations, times[0], times[repeats / 2U], times[(repeats * 95U - 1U) / 100U]);
	free(times);
	free(buffer);
	ext4_unmount(fs);
	require(image.live_allocations == 0, "unmount allocation balance failed");
	ext4_posix_close(&image);
	return 0;
}

/* SPDX-License-Identifier: BSD-3-Clause */
#include "internal.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

/* Mutate any nonzero block of a reference image through a copy-on-write overlay,
 * then run offline recovery, a read-only namespace walk and a fixed sequence of
 * writable operations. Every result is admissible; memory safety, bounded work,
 * and balanced allocations are not. Images without metadata checksums let every
 * mutated byte reach structure validation instead of stopping at a checksum. */

#define FUZZ_READ_BUDGET 8192U
#define FUZZ_WRITE_BUDGET 8192U
#define FUZZ_ALLOCATION_BUDGET (16U * 1024U * 1024U)
#define FUZZ_OVERLAY_BLOCKS 2048U
#define FUZZ_OVERLAY_SLOTS 4096U
#define FUZZ_WALK_LIMIT 96U
#define FUZZ_WALK_DEPTH 3U
#define FUZZ_READ_BYTES 8192U
#define FUZZ_MUTATION_SET 1U
#define FUZZ_EMPTY UINT64_MAX
#define FUZZ_SECONDS 1700003000

struct fuzz_mutation {
	struct ext4_le32 selector;
	struct ext4_le16 offset;
	uint8_t length;
	uint8_t flags;
};

struct fuzz_slot {
	uint64_t block;
	uint8_t *data;
};

struct fuzz_device {
	const uint8_t *image;
	size_t image_size;
	uint32_t block_size;
	uint64_t *interesting;
	uint32_t interesting_count;
	struct fuzz_slot slots[FUZZ_OVERLAY_SLOTS];
	uint8_t *pool;
	uint32_t pool_used;
	size_t live_bytes;
	uint32_t reads;
	uint32_t writes;
};

struct fuzz_walk {
	uint32_t numbers[FUZZ_WALK_LIMIT];
	uint32_t depths[FUZZ_WALK_LIMIT];
	uint32_t count;
	uint32_t depth;
};

static struct fuzz_device device;
static struct ext4_environment environment;
static struct ext4_write_environment writer;
static uint8_t scratch[FUZZ_READ_BYTES];

static void *
fuzz_allocate(void *context, size_t size)
{
	struct fuzz_device *state = context;
	void *allocation;

	if (size > FUZZ_ALLOCATION_BUDGET - state->live_bytes) {
		return NULL;
	}
	allocation = malloc(size);
	if (allocation != NULL) {
		state->live_bytes += size;
	}
	return allocation;
}

static void
fuzz_release(void *context, void *allocation, size_t size)
{
	struct fuzz_device *state = context;

	if (allocation == NULL || size > state->live_bytes) {
		abort();
	}
	state->live_bytes -= size;
	free(allocation);
}

static struct fuzz_slot *
fuzz_slot(struct fuzz_device *state, uint64_t block, bool create)
{
	uint32_t index =
	    (uint32_t)((block * UINT64_C(0x9e3779b97f4a7c15)) >> 52) % FUZZ_OVERLAY_SLOTS;
	uint32_t probe;
	struct fuzz_slot *slot;

	for (probe = 0; probe < FUZZ_OVERLAY_SLOTS; probe++) {
		slot = &state->slots[(index + probe) % FUZZ_OVERLAY_SLOTS];
		if (slot->block == block) {
			return slot;
		}
		if (slot->block == FUZZ_EMPTY) {
			if (!create || state->pool_used == FUZZ_OVERLAY_BLOCKS) {
				return NULL;
			}
			slot->block = block;
			slot->data = state->pool + (size_t)state->pool_used++ * state->block_size;
			memcpy(slot->data, state->image + block * state->block_size,
			    state->block_size);
			return slot;
		}
	}
	return NULL;
}

static enum ext4_result
fuzz_read(void *context, uint64_t offset, void *buffer, size_t length)
{
	struct fuzz_device *state = context;
	struct fuzz_slot *slot;
	uint8_t *bytes = buffer;
	uint64_t block;
	size_t within;
	size_t part;

	if (++state->reads > FUZZ_READ_BUDGET) {
		return EXT4_IO;
	}
	if (offset > state->image_size || length > state->image_size - offset) {
		return EXT4_CORRUPT;
	}
	while (length != 0) {
		block = offset / state->block_size;
		within = (size_t)(offset % state->block_size);
		part = state->block_size - within;
		if (part > length) {
			part = length;
		}
		slot = fuzz_slot(state, block, false);
		memcpy(bytes, slot != NULL ? slot->data + within : state->image + offset, part);
		bytes += part;
		offset += part;
		length -= part;
	}
	return EXT4_OK;
}

static enum ext4_result
fuzz_write(void *context, uint64_t offset, const void *buffer, size_t length)
{
	struct fuzz_device *state = context;
	struct fuzz_slot *slot;
	const uint8_t *bytes = buffer;
	uint64_t block;
	size_t within;
	size_t part;

	if (++state->writes > FUZZ_WRITE_BUDGET) {
		return EXT4_IO;
	}
	if (offset > state->image_size || length > state->image_size - offset) {
		abort();
	}
	while (length != 0) {
		block = offset / state->block_size;
		within = (size_t)(offset % state->block_size);
		part = state->block_size - within;
		if (part > length) {
			part = length;
		}
		slot = fuzz_slot(state, block, true);
		if (slot == NULL) {
			return EXT4_IO;
		}
		memcpy(slot->data + within, bytes, part);
		bytes += part;
		offset += part;
		length -= part;
	}
	return EXT4_OK;
}

static enum ext4_result
fuzz_flush(void *context)
{
	(void)context;
	return EXT4_OK;
}

static void
fuzz_reset(struct fuzz_device *state)
{
	uint32_t index;

	for (index = 0; index < FUZZ_OVERLAY_SLOTS; index++) {
		state->slots[index].block = FUZZ_EMPTY;
	}
	state->pool_used = 0;
	state->reads = 0;
	state->writes = 0;
}

static void
fuzz_mutate(struct fuzz_device *state, const uint8_t *data, size_t length)
{
	const struct fuzz_mutation *mutation;
	struct fuzz_slot *slot;
	size_t position;
	size_t index;
	uint32_t offset;

	for (position = 0; length - position >= sizeof(*mutation);) {
		mutation = (const struct fuzz_mutation *)(data + position);
		position += sizeof(*mutation);
		slot = fuzz_slot(state,
		    state->interesting[ext4_le32(&mutation->selector) % state->interesting_count],
		    true);
		if (slot == NULL) {
			return;
		}
		offset = ext4_le16(&mutation->offset) % state->block_size;
		for (index = 0; index < mutation->length && position < length; index++) {
			if (mutation->flags & FUZZ_MUTATION_SET) {
				slot->data[offset] = data[position];
			} else {
				slot->data[offset] ^= data[position];
			}
			position++;
			offset = (offset + 1U) % state->block_size;
		}
	}
}

static enum ext4_dir_action
fuzz_visit(void *context, const struct ext4_dir_entry *entry, uint64_t next_cookie)
{
	struct fuzz_walk *walk = context;

	(void)next_cookie;
	if (walk->count == FUZZ_WALK_LIMIT) {
		return EXT4_DIR_STOP;
	}
	if ((entry->name_length == 1 && entry->name[0] == '.') ||
	    (entry->name_length == 2 && entry->name[0] == '.' && entry->name[1] == '.')) {
		return EXT4_DIR_ACCEPT;
	}
	walk->numbers[walk->count] = entry->inode;
	walk->depths[walk->count] = walk->depth + 1U;
	walk->count++;
	return EXT4_DIR_ACCEPT;
}

static void
fuzz_inspect(struct ext4_fs *fs, const struct ext4_inode *inode)
{
	struct ext4_xattr_key keys[4];
	struct ext4_mapping mapping;
	size_t completed;
	size_t count;
	size_t size;

	if (ext4_list_xattrs(fs, inode->number, inode->generation, keys, 4, &count) == EXT4_OK &&
	    count != 0) {
		(void)ext4_get_xattr(fs, inode->number, inode->generation, keys[0].name_index,
		    keys[0].name, keys[0].name_length, scratch, sizeof(scratch), &size);
	}
	if ((inode->mode & EXT4_MODE_TYPE) != EXT4_MODE_REGULAR &&
	    (inode->mode & EXT4_MODE_TYPE) != EXT4_MODE_SYMLINK) {
		return;
	}
	(void)ext4_read(fs, inode, 0, scratch, sizeof(scratch), &completed);
	(void)ext4_map_read(fs, inode, 0, sizeof(scratch), &mapping);
	if (inode->size > sizeof(scratch)) {
		(void)ext4_read(
		    fs, inode, inode->size - sizeof(scratch), scratch, sizeof(scratch), &completed);
		(void)ext4_read(fs, inode, inode->size / 2U, scratch, sizeof(scratch), &completed);
	}
}

/* Breadth-first walk of a bounded prefix of the namespace. */
static void
fuzz_walk(struct ext4_fs *fs, struct fuzz_walk *walk)
{
	struct ext4_inode inode;
	struct ext4_inode found;
	struct ext4_dir_entry entry;
	uint64_t cookie;
	uint32_t index;

	walk->count = 0;
	walk->numbers[walk->count] = EXT4_ROOT_INODE;
	walk->depths[walk->count++] = 0;
	for (index = 0; index < walk->count; index++) {
		if (ext4_get_inode(fs, walk->numbers[index], &inode) != EXT4_OK) {
			continue;
		}
		fuzz_inspect(fs, &inode);
		if ((inode.mode & EXT4_MODE_TYPE) != EXT4_MODE_DIRECTORY ||
		    walk->depths[index] >= FUZZ_WALK_DEPTH) {
			continue;
		}
		walk->depth = walk->depths[index];
		cookie = 0;
		(void)ext4_iterate_dir(fs, &inode, &cookie, fuzz_visit, walk);
		cookie = 0;
		if (ext4_next_dir(fs, &inode, &cookie, &entry) == EXT4_OK) {
			(void)ext4_lookup(fs, &inode, entry.name, entry.name_length, &found);
		}
	}
}

static struct ext4_inode_update
fuzz_update(uint32_t fields)
{
	struct ext4_inode_update update = { 0 };

	update.fields = fields | EXT4_ATTR_XATTRS;
	update.permissions = 0644;
	update.uid = 1000;
	update.gid = 1000;
	update.access_time.seconds = FUZZ_SECONDS;
	update.modify_time.seconds = FUZZ_SECONDS;
	update.change_time.seconds = FUZZ_SECONDS;
	return update;
}

/* Fixed mutations on new and existing objects; any result is admissible. */
static void
fuzz_mutations(struct ext4_fs *fs, const struct fuzz_walk *walk)
{
	static const uint8_t file_name[] = "fuzz-file";
	static const uint8_t directory_name[] = "fuzz-directory";
	static const uint8_t link_name[] = "fuzz-link";
	static const uint8_t attribute_name[] = "fuzz";
	struct ext4_inode_update creation = fuzz_update(EXT4_ATTR_PERMISSIONS | EXT4_ATTR_UID |
	    EXT4_ATTR_GID | EXT4_ATTR_ACCESS_TIME | EXT4_ATTR_MODIFY_TIME | EXT4_ATTR_CHANGE_TIME);
	struct ext4_inode_update data =
	    fuzz_update(EXT4_ATTR_PERMISSIONS | EXT4_ATTR_MODIFY_TIME | EXT4_ATTR_CHANGE_TIME);
	struct ext4_inode_update attribute = fuzz_update(EXT4_ATTR_CHANGE_TIME);
	struct ext4_xattr_change change = { EXT4_XATTR_SET, EXT4_XATTR_USER, attribute_name,
		sizeof(attribute_name) - 1U, "value", 5 };
	struct ext4_timestamp time = { FUZZ_SECONDS, 0 };
	struct ext4_rename_entry source;
	struct ext4_rename_entry destination;
	struct ext4_inode root;
	struct ext4_inode file;
	struct ext4_inode directory;
	struct ext4_inode existing;
	struct ext4_inode result;
	uint64_t allocated;
	size_t completed;
	uint32_t index;
	bool have_file;
	bool have_directory;

	attribute.xattrs = &change;
	attribute.xattr_count = 1;
	if (ext4_get_inode(fs, EXT4_ROOT_INODE, &root) != EXT4_OK) {
		return;
	}
	have_file = ext4_create(fs, root.number, root.generation, file_name, sizeof(file_name) - 1U,
			&creation, &time, &file) == EXT4_OK;
	have_directory = ext4_mkdir(fs, root.number, root.generation, directory_name,
			     sizeof(directory_name) - 1U, &creation, &time, &directory) == EXT4_OK;
	if (have_file) {
		(void)ext4_write(
		    fs, file.number, file.generation, 0, scratch, 3000, &data, &completed);
		(void)ext4_write_partial(fs, file.number, file.generation, 70000, scratch,
		    sizeof(scratch), &data, &completed);
		(void)ext4_fallocate(fs, file.number, file.generation, 4096, 20000,
		    EXT4_FALLOC_KEEP_SIZE, &data, &allocated);
		(void)ext4_fallocate(fs, file.number, file.generation, 1024, 8192,
		    EXT4_FALLOC_KEEP_SIZE | EXT4_FALLOC_PUNCH_HOLE, &data, &allocated);
		(void)ext4_set_attributes(fs, file.number, file.generation, &attribute, &result);
		(void)ext4_truncate(fs, file.number, file.generation, 1000, &data, &result);
		(void)ext4_link(fs, root.number, root.generation, link_name, sizeof(link_name) - 1U,
		    file.number, file.generation, &time, &result);
	}
	if (have_file && have_directory) {
		source = (struct ext4_rename_entry){ root.number, root.generation, file_name,
			sizeof(file_name) - 1U, file.number, file.generation };
		destination = (struct ext4_rename_entry){ directory.number, directory.generation,
			file_name, sizeof(file_name) - 1U, 0, 0 };
		(void)ext4_rename(fs, &source, &destination, 0, &time, &result);
	}
	if (have_file) {
		(void)ext4_unlink(fs, root.number, root.generation, link_name,
		    sizeof(link_name) - 1U, file.number, 0, &time, &result);
	}
	for (index = 1; index < walk->count && index < 8U; index++) {
		if (ext4_get_inode(fs, walk->numbers[index], &existing) != EXT4_OK) {
			continue;
		}
		if ((existing.mode & EXT4_MODE_TYPE) == EXT4_MODE_REGULAR) {
			(void)ext4_write(fs, existing.number, existing.generation,
			    existing.size / 2U, scratch, 512, &data, &completed);
			(void)ext4_truncate(fs, existing.number, existing.generation,
			    existing.size / 3U, &data, &result);
		} else if ((existing.mode & EXT4_MODE_TYPE) == EXT4_MODE_DIRECTORY) {
			(void)ext4_create(fs, existing.number, existing.generation, file_name,
			    sizeof(file_name) - 1U, &creation, &time, &result);
		}
		(void)ext4_set_attributes(
		    fs, existing.number, existing.generation, &attribute, &result);
	}
}

int
LLVMFuzzerInitialize(int *argc, char ***argv)
{
	struct ext4_fs *fs = NULL;
	struct stat metadata;
	const char *path = NULL;
	uint64_t block;
	uint64_t blocks;
	size_t byte;
	int fd;
	int index;
	int cursor;

	for (index = 1; index < *argc; index++) {
		if (strncmp((*argv)[index], "--image=", 8) == 0) {
			path = (*argv)[index] + 8;
			for (cursor = index; cursor < *argc; cursor++) {
				(*argv)[cursor] = (*argv)[cursor + 1];
			}
			(*argc)--;
			break;
		}
	}
	if (path == NULL || (fd = open(path, O_RDONLY)) < 0) {
		fprintf(stderr, "provide --image=PATH to a generated ext4 image\n");
		exit(2);
	}
	if (fstat(fd, &metadata) != 0 || metadata.st_size <= 0 ||
	    (uint64_t)metadata.st_size > SIZE_MAX) {
		exit(2);
	}
	device.image_size = (size_t)metadata.st_size;
	device.image = mmap(NULL, device.image_size, PROT_READ, MAP_PRIVATE, fd, 0);
	close(fd);
	if (device.image == MAP_FAILED) {
		exit(2);
	}
	environment = (struct ext4_environment){ &device, device.image_size, fuzz_read,
		fuzz_allocate, fuzz_release };
	writer = (struct ext4_write_environment){ &device, fuzz_write, fuzz_flush, NULL };
	device.block_size = EXT4_MIN_BLOCK_SIZE;
	fuzz_reset(&device);
	if (ext4_mount(&environment, &fs) == EXT4_OK) {
		device.block_size = fs->info.block_size;
		ext4_unmount(fs);
	} else {
		/* A pending journal rejects a read-only mount; read the geometry directly. */
		device.block_size = EXT4_MIN_BLOCK_SIZE
		    << ext4_le32(
			   &((const struct ext4_super_disk *)(device.image + EXT4_SUPER_OFFSET))
			       ->log_block_size);
	}
	if (device.block_size < EXT4_MIN_BLOCK_SIZE || device.block_size > EXT4_MAX_BLOCK_SIZE) {
		exit(2);
	}
	blocks = device.image_size / device.block_size;
	device.interesting = malloc((size_t)blocks * sizeof(*device.interesting));
	device.pool = malloc((size_t)FUZZ_OVERLAY_BLOCKS * device.block_size);
	if (device.interesting == NULL || device.pool == NULL) {
		exit(2);
	}
	for (block = 0; block < blocks; block++) {
		for (byte = 0; byte < device.block_size; byte++) {
			if (device.image[block * device.block_size + byte] != 0) {
				device.interesting[device.interesting_count++] = block;
				break;
			}
		}
	}
	if (device.interesting_count == 0) {
		exit(2);
	}
	fprintf(stderr, "fuzzing %u nonzero %u-byte blocks\n", device.interesting_count,
	    device.block_size);
	return 0;
}

int
LLVMFuzzerTestOneInput(const uint8_t *data, size_t length)
{
	struct ext4_fs *fs = NULL;
	struct ext4_recovery_report report;
	struct fuzz_walk walk;

	fuzz_reset(&device);
	fuzz_mutate(&device, data, length);
	(void)ext4_recover(&environment, &writer, &report);
	if (device.live_bytes != 0) {
		abort();
	}
	device.reads = 0;
	walk.count = 0;
	if (ext4_mount(&environment, &fs) == EXT4_OK) {
		fuzz_walk(fs, &walk);
		ext4_unmount(fs);
	}
	if (device.live_bytes != 0) {
		abort();
	}
	device.reads = 0;
	if (ext4_mount_writable(&environment, &writer, &fs) == EXT4_OK) {
		fuzz_mutations(fs, &walk);
		(void)ext4_sync(fs);
		ext4_unmount(fs);
	}
	if (device.live_bytes != 0) {
		abort();
	}
	return 0;
}

/* SPDX-License-Identifier: BSD-3-Clause */
#include "internal.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#define FUZZ_READ_BUDGET 256U
#define FUZZ_ALLOCATION_BUDGET (8U * 1024U * 1024U)
#define FUZZ_REPAIR_CHECKSUM 1U

enum fuzz_region {
	FUZZ_SUPERBLOCK,
	FUZZ_GROUP_DESCRIPTOR,
	FUZZ_ROOT_INODE,
	FUZZ_ROOT_DIRECTORY,
	FUZZ_REGION_COUNT
};

struct fuzz_input_header {
	uint8_t region;
	uint8_t flags;
	struct ext4_le16 offset;
};

struct fuzz_resource {
	const uint8_t *image;
	size_t image_size;
	const uint8_t *input;
	size_t input_size;
	size_t live_bytes;
	unsigned int reads;
	uint64_t group_offset;
	uint64_t inode_offset;
	uint64_t directory_offset;
	uint32_t block_size;
	uint32_t checksum_seed;
	uint32_t root_seed;
};

static struct fuzz_resource resource;
static struct ext4_environment environment;

static void *
fuzz_allocate(void *context, size_t size)
{
	struct fuzz_resource *input = context;
	void *allocation;

	if (size > FUZZ_ALLOCATION_BUDGET - input->live_bytes) {
		return NULL;
	}
	allocation = malloc(size);
	if (allocation != NULL) {
		input->live_bytes += size;
	}
	return allocation;
}

static void
fuzz_release(void *context, void *allocation, size_t size)
{
	struct fuzz_resource *input = context;

	if (size > input->live_bytes) {
		abort();
	}
	input->live_bytes -= size;
	free(allocation);
}

static void
fuzz_checksum(struct fuzz_resource *input, unsigned int kind, void *buffer, size_t length)
{
	struct ext4_super_disk *super;
	struct ext4_group_disk *group;
	struct ext4_inode_disk *inode;
	struct ext4_dir_tail_disk *tail;
	struct ext4_le32 wire;
	uint32_t checksum;
	bool high;

	switch (kind) {
	case FUZZ_SUPERBLOCK:
		super = buffer;
		ext4_encode32(&super->checksum,
		    ext4_crc32c(UINT32_MAX, buffer, offsetof(struct ext4_super_disk, checksum)));
		break;
	case FUZZ_GROUP_DESCRIPTOR:
		group = buffer;
		ext4_zero(&group->checksum, sizeof(group->checksum));
		ext4_encode32(&wire, 0);
		checksum = ext4_crc32c(input->checksum_seed, &wire, sizeof(wire));
		checksum = ext4_crc32c(checksum, buffer, length);
		ext4_encode16(&group->checksum, (uint16_t)checksum);
		break;
	case FUZZ_ROOT_INODE:
		inode = buffer;
		high = length >= offsetof(struct ext4_inode_disk, checksum_hi) +
			    sizeof(inode->checksum_hi) &&
		    ext4_le16(&inode->extra_size) >= 4;
		ext4_zero(&inode->checksum_lo, sizeof(inode->checksum_lo));
		if (high) {
			ext4_zero(&inode->checksum_hi, sizeof(inode->checksum_hi));
		}
		ext4_encode32(&wire, EXT4_ROOT_INODE);
		checksum = ext4_crc32c(input->checksum_seed, &wire, sizeof(wire));
		checksum = ext4_crc32c(checksum, &inode->generation, sizeof(inode->generation));
		checksum = ext4_crc32c(checksum, buffer, length);
		ext4_encode16(&inode->checksum_lo, (uint16_t)checksum);
		if (high) {
			ext4_encode16(&inode->checksum_hi, (uint16_t)(checksum >> 16));
		}
		break;
	case FUZZ_ROOT_DIRECTORY:
		tail = (struct ext4_dir_tail_disk *)((uint8_t *)buffer + length - sizeof(*tail));
		ext4_encode32(
		    &tail->checksum, ext4_crc32c(input->root_seed, buffer, length - sizeof(*tail)));
		break;
	}
}

static enum ext4_result
fuzz_read(void *context, uint64_t offset, void *buffer, size_t length)
{
	struct fuzz_resource *input = context;
	const struct fuzz_input_header *header;
	uint8_t *bytes = buffer;
	size_t index;
	size_t position;
	unsigned int kind;
	bool selected;

	if (++input->reads > FUZZ_READ_BUDGET) {
		return EXT4_IO;
	}
	if (offset > input->image_size || length > input->image_size - offset) {
		return EXT4_CORRUPT;
	}
	memcpy(buffer, input->image + offset, length);
	if (input->input_size < sizeof(*header) || length == 0) {
		return EXT4_OK;
	}
	header = (const struct fuzz_input_header *)input->input;
	kind = header->region % FUZZ_REGION_COUNT;
	selected =
	    (kind == FUZZ_SUPERBLOCK && offset == EXT4_SUPER_OFFSET && length == EXT4_SUPER_SIZE) ||
	    (kind == FUZZ_GROUP_DESCRIPTOR && offset == input->group_offset &&
		length >= EXT4_GROUP_BASE_SIZE) ||
	    (kind == FUZZ_ROOT_INODE && offset == input->inode_offset &&
		length >= EXT4_INODE_BASE_SIZE) ||
	    (kind == FUZZ_ROOT_DIRECTORY && offset == input->directory_offset &&
		length == input->block_size);
	if (!selected) {
		return EXT4_OK;
	}
	position = ext4_le16(&header->offset) % length;
	for (index = sizeof(*header); index < input->input_size; index++) {
		bytes[position] ^= input->input[index];
		position = (position + 1) % length;
	}
	if (header->flags & FUZZ_REPAIR_CHECKSUM) {
		fuzz_checksum(input, kind, buffer, length);
	}
	return EXT4_OK;
}

int
LLVMFuzzerInitialize(int *argc, char ***argv)
{
	struct ext4_fs *fs = NULL;
	struct ext4_inode root;
	struct ext4_group_disk group;
	struct stat metadata;
	const char *path = NULL;
	uint64_t directory_block;
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
		fprintf(stderr, "provide --image=PATH to a clean generated ext4-4k.img\n");
		exit(2);
	}
	if (fstat(fd, &metadata) != 0 || metadata.st_size <= 0 ||
	    (uint64_t)metadata.st_size > SIZE_MAX) {
		exit(2);
	}
	resource.image_size = (size_t)metadata.st_size;
	resource.image = mmap(NULL, resource.image_size, PROT_READ, MAP_PRIVATE, fd, 0);
	close(fd);
	if (resource.image == MAP_FAILED) {
		exit(2);
	}
	environment.context = &resource;
	environment.size_bytes = resource.image_size;
	environment.read = fuzz_read;
	environment.allocate = fuzz_allocate;
	environment.release = fuzz_release;
	if (ext4_mount(&environment, &fs) != EXT4_OK || !fs->metadata_checksum ||
	    ext4_get_inode(fs, EXT4_ROOT_INODE, &root) != EXT4_OK ||
	    (root.flags & EXT4_INODE_INDEX) != 0 ||
	    ext4_map_block(fs, &root, 0, &directory_block) != EXT4_OK) {
		fprintf(
		    stderr, "reference image must have checksums and a linear root directory\n");
		exit(2);
	}
	resource.block_size = fs->info.block_size;
	resource.group_offset = (uint64_t)(fs->first_data_block + 1) * fs->info.block_size;
	if (ext4_device_read(fs, resource.group_offset, &group, sizeof(group)) != EXT4_OK) {
		exit(2);
	}
	resource.inode_offset = ((uint64_t)ext4_le32(&group.inode_table_lo) |
				    (uint64_t)ext4_le32(&group.inode_table_hi) << 32) *
		fs->info.block_size +
	    (EXT4_ROOT_INODE - 1) * fs->inode_size;
	resource.directory_offset = directory_block * fs->info.block_size;
	resource.checksum_seed = fs->checksum_seed;
	resource.root_seed = ext4_inode_seed(fs, &root);
	ext4_unmount(fs);
	return 0;
}

int
LLVMFuzzerTestOneInput(const uint8_t *data, size_t length)
{
	struct ext4_fs *fs = NULL;
	struct ext4_inode root;
	struct ext4_inode inode;
	struct ext4_dir_entry entry;
	struct ext4_mapping mapping;
	uint8_t buffer[4096];
	uint64_t cookie = 0;
	size_t completed;
	unsigned int count;

	resource.input = data;
	resource.input_size = length;
	resource.reads = 0;
	if (ext4_mount(&environment, &fs) == EXT4_OK &&
	    ext4_get_inode(fs, EXT4_ROOT_INODE, &root) == EXT4_OK) {
		for (count = 0; count < 32; count++) {
			if (ext4_next_dir(fs, &root, &cookie, &entry) != EXT4_OK) {
				break;
			}
			if (ext4_get_inode(fs, entry.inode, &inode) != EXT4_OK) {
				continue;
			}
			if ((inode.mode & EXT4_MODE_TYPE) == EXT4_MODE_REGULAR ||
			    (inode.mode & EXT4_MODE_TYPE) == EXT4_MODE_SYMLINK) {
				(void)ext4_read(fs, &inode, 0, buffer, sizeof(buffer), &completed);
				(void)ext4_map_read(fs, &inode, 0, sizeof(buffer), &mapping);
				if (inode.size > sizeof(buffer)) {
					(void)ext4_read(fs, &inode, inode.size - sizeof(buffer),
					    buffer, sizeof(buffer), &completed);
				}
			}
		}
	}
	ext4_unmount(fs);
	if (resource.live_bytes != 0) {
		abort();
	}
	return 0;
}

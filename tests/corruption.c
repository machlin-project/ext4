/* SPDX-License-Identifier: BSD-3-Clause */
#include "internal.h"
#include "image.h"

#include <stdio.h>
#include <string.h>

enum corruption {
	BAD_SUPER_CHECKSUM,
	BAD_SUPER_GEOMETRY,
	BAD_INODE_SIZE,
	UNKNOWN_INCOMPAT,
	NEEDS_RECOVERY,
	UNCLEAN_VOLUME,
	BAD_GROUP_CHECKSUM,
	BAD_ROOT_INODE_CHECKSUM,
	BAD_DIRECTORY_CHECKSUM,
	BAD_INODE_EXTRA_SIZE,
	BAD_TIMESTAMP,
	CORRUPTION_COUNT
};

#define EXT4_TEST_UNKNOWN_INCOMPAT 0x80000000U

struct corrupt_resource {
	struct ext4_posix_image image;
	enum corruption corruption;
	uint64_t group_offset;
	uint64_t inode_offset;
	uint64_t directory_offset;
	uint32_t block_size;
	uint32_t root_checksum_seed;
	bool changed;
};

static enum ext4_result
corrupt_read(void *context, uint64_t offset, void *buffer, size_t length)
{
	struct corrupt_resource *resource = context;
	struct ext4_super_disk *super;
	struct ext4_group_disk *group;
	struct ext4_inode_disk *inode;
	struct ext4_dir_tail_disk *tail;
	uint32_t checksum;
	enum ext4_result error;

	error = resource->image.environment.read(&resource->image, offset, buffer, length);
	if (error != EXT4_OK) {
		return error;
	}
	if (offset == EXT4_SUPER_OFFSET && length == EXT4_SUPER_SIZE &&
	    resource->corruption <= UNCLEAN_VOLUME) {
		super = buffer;
		resource->changed = true;
		switch (resource->corruption) {
		case BAD_SUPER_CHECKSUM:
			super->checksum.bytes[0] ^= 1;
			return EXT4_OK;
		case BAD_SUPER_GEOMETRY:
			ext4_zero(&super->blocks_per_group, sizeof(super->blocks_per_group));
			break;
		case BAD_INODE_SIZE:
			ext4_zero(&super->inode_size, sizeof(super->inode_size));
			break;
		case UNKNOWN_INCOMPAT:
			ext4_encode32(&super->feature_incompat,
			    ext4_le32(&super->feature_incompat) | EXT4_TEST_UNKNOWN_INCOMPAT);
			break;
		case NEEDS_RECOVERY:
			ext4_encode32(&super->feature_incompat,
			    ext4_le32(&super->feature_incompat) | EXT4_FEATURE_INCOMPAT_RECOVER);
			break;
		case UNCLEAN_VOLUME:
			ext4_zero(&super->state, sizeof(super->state));
			break;
		default:
			return EXT4_INVALID_ARGUMENT;
		}
		checksum =
		    ext4_crc32c(UINT32_MAX, super, offsetof(struct ext4_super_disk, checksum));
		ext4_encode32(&super->checksum, checksum);
	} else if (offset == resource->group_offset && resource->corruption == BAD_GROUP_CHECKSUM) {
		group = buffer;
		group->checksum.bytes[0] ^= 1;
		resource->changed = true;
	} else if (offset == resource->inode_offset &&
	    resource->corruption == BAD_ROOT_INODE_CHECKSUM) {
		inode = buffer;
		inode->checksum_lo.bytes[0] ^= 1;
		resource->changed = true;
	} else if (offset == resource->inode_offset &&
	    (resource->corruption == BAD_INODE_EXTRA_SIZE ||
		resource->corruption == BAD_TIMESTAMP)) {
		inode = buffer;
		if (resource->corruption == BAD_INODE_EXTRA_SIZE) {
			ext4_encode16(&inode->extra_size, UINT16_MAX & ~3U);
		} else {
			/* Valid checksum, invalid nanoseconds: semantic validation must reject it.
			 */
			ext4_encode32(&inode->modify_time_extra, 1000000000U << 2);
		}
		ext4_zero(&inode->checksum_lo, sizeof(inode->checksum_lo));
		ext4_zero(&inode->checksum_hi, sizeof(inode->checksum_hi));
		checksum = ext4_crc32c(resource->root_checksum_seed, buffer, length);
		ext4_encode16(&inode->checksum_lo, (uint16_t)checksum);
		ext4_encode16(&inode->checksum_hi, (uint16_t)(checksum >> 16));
		resource->changed = true;
	} else if (offset == resource->directory_offset && length == resource->block_size &&
	    resource->corruption == BAD_DIRECTORY_CHECKSUM) {
		tail = (struct ext4_dir_tail_disk *)((uint8_t *)buffer + length - sizeof(*tail));
		tail->checksum.bytes[0] ^= 1;
		resource->changed = true;
	}
	return EXT4_OK;
}

static void *
corrupt_allocate(void *context, size_t size)
{
	struct corrupt_resource *resource = context;

	return resource->image.environment.allocate(&resource->image, size);
}

static void
corrupt_release(void *context, void *allocation, size_t size)
{
	struct corrupt_resource *resource = context;

	resource->image.environment.release(&resource->image, allocation, size);
}

int
main(int argc, char **argv)
{
	struct corrupt_resource resource;
	struct ext4_environment environment;
	struct ext4_super_disk super;
	struct ext4_group_disk group;
	struct ext4_fs *fs = NULL;
	struct ext4_inode root;
	struct ext4_dir_entry entry;
	uint64_t block;
	uint64_t cookie;
	unsigned int failures = 0;
	enum corruption corruption;
	enum ext4_result error;
	enum ext4_result expected;

	if (argc != 2) {
		fprintf(stderr, "usage: ext4-corruption-test IMAGE\n");
		return 2;
	}
	memset(&resource, 0, sizeof(resource));
	if (ext4_posix_open(&resource.image, argv[1]) != EXT4_OK ||
	    ext4_mount(&resource.image.environment, &fs) != EXT4_OK) {
		fprintf(stderr, "cannot open clean reference image\n");
		ext4_posix_close(&resource.image);
		return 1;
	}
	/* Derive mutation locations from real, already validated disk metadata. */
	if (ext4_device_read(fs, EXT4_SUPER_OFFSET, &super, sizeof(super)) != EXT4_OK) {
		return 1;
	}
	resource.block_size = fs->info.block_size;
	resource.group_offset = (uint64_t)(fs->first_data_block + 1) * resource.block_size;
	if (ext4_device_read(fs, resource.group_offset, &group, sizeof(group)) != EXT4_OK ||
	    ext4_get_inode(fs, EXT4_ROOT_INODE, &root) != EXT4_OK ||
	    ext4_map_block(fs, &root, 0, &block) != EXT4_OK) {
		return 1;
	}
	resource.inode_offset = ((uint64_t)ext4_le32(&group.inode_table_lo) |
				    ((uint64_t)ext4_le32(&group.inode_table_hi) << 32)) *
		resource.block_size +
	    (EXT4_ROOT_INODE - 1) * ext4_le16(&super.inode_size);
	resource.directory_offset = block * resource.block_size;
	resource.root_checksum_seed = ext4_inode_seed(fs, &root);
	ext4_unmount(fs);
	fs = NULL;
	environment = resource.image.environment;
	environment.context = &resource;
	environment.read = corrupt_read;
	environment.allocate = corrupt_allocate;
	environment.release = corrupt_release;
	if (ext4_crc32c(UINT32_MAX, "123456789", 9) != (0xe3069283U ^ UINT32_MAX)) {
		fprintf(stderr, "CRC32C standard check vector failed\n");
		failures++;
	}
	for (corruption = 0; corruption < CORRUPTION_COUNT; corruption++) {
		resource.corruption = corruption;
		resource.changed = false;
		expected = corruption == UNKNOWN_INCOMPAT ? EXT4_UNSUPPORTED : EXT4_CORRUPT;
		if (corruption == NEEDS_RECOVERY) {
			expected = EXT4_RECOVERY_REQUIRED;
		}
		/* Without a pending journal, only e2fsck can check a volume left in use. */
		if (corruption == UNCLEAN_VOLUME) {
			expected = EXT4_CHECK_REQUIRED;
		}
		error = ext4_mount(&environment, &fs);
		if (corruption == BAD_DIRECTORY_CHECKSUM && error == EXT4_OK) {
			error = ext4_get_inode(fs, EXT4_ROOT_INODE, &root);
			if (error == EXT4_OK) {
				cookie = 0;
				error = ext4_next_dir(fs, &root, &cookie, &entry);
			}
		}
		if (!resource.changed || error != expected) {
			fprintf(stderr, "corruption %u: changed=%u, got %s, expected %s\n",
			    (unsigned int)corruption, resource.changed, ext4_result_string(error),
			    ext4_result_string(expected));
			failures++;
		}
		ext4_unmount(fs);
		fs = NULL;
		if (resource.image.live_allocations != 0) {
			fprintf(
			    stderr, "corruption %u leaked allocations\n", (unsigned int)corruption);
			failures++;
		}
	}
	ext4_posix_close(&resource.image);
	if (failures != 0) {
		return 1;
	}
	printf("PASS: malformed geometry, features, recovery state and metadata checksums\n");
	return 0;
}

/* SPDX-License-Identifier: BSD-3-Clause */
#ifndef MACHLIN_EXT4_TEST_EA_INODE_CORRUPTION_H
#define MACHLIN_EXT4_TEST_EA_INODE_CORRUPTION_H

enum ea_damage {
	EA_VALUE_SELF,
	EA_VALUE_RESERVED,
	EA_VALUE_RANGE,
	EA_VALUE_OFFSET,
	EA_VALUE_EMPTY,
	EA_VALUE_OVERSIZE,
	EA_VALUE_SIZE_MISMATCH,
	EA_ENTRY_HASH,
	EA_INODE_FLAG,
	EA_INODE_TYPE,
	EA_INODE_LINKS,
	EA_INODE_SIZE,
	EA_REFERENCE_ZERO,
	EA_REFERENCE_OVERFLOW,
	EA_RECURSIVE_ATTRIBUTES,
	EA_DATA_CHECKSUM,
	EA_PARENT_BLOCK_COUNT,
	EA_DAMAGE_COUNT
};

static void
corruption(struct device *device)
{
	struct ext4_fs *fs;
	struct ext4_inode owner;
	struct ext4_inode value;
	struct ext4_xattr_snapshot snapshot;
	struct ext4_xattr_entry_disk *entry;
	struct ext4_xattr_header_disk *header;
	struct ext4_inode_disk *parent_disk;
	struct ext4_inode_disk *value_disk;
	uint8_t *output = malloc(EXT4_XATTR_VALUE_MAX + 1U);
	uint64_t entry_offset = 0;
	uint64_t external;
	uint64_t physical;
	uint32_t number;
	uint32_t live;
	uint32_t allocations;
	uint32_t reads;
	uint32_t point;
	uint32_t limit;
	size_t returned;
	size_t index;
	unsigned int fault;
	enum ea_damage damage;

	CHECK(output != NULL);
	device_reset(device, device->base);
	EXPECT(ext4_mount(&device->environment, &fs), EXT4_OK);
	owner = lookup(fs, "body");
	number = value_number(fs, &owner, "maximum");
	CHECK(number != 0);
	EXPECT(ext4_xattr_open(fs, owner.number, owner.generation, &snapshot), EXT4_OK);
	external = snapshot.external_block;
	for (index = 0; index < snapshot.count; index++) {
		if (snapshot.records[index].value_inode == number) {
			if (snapshot.records[index].external) {
				entry_offset = external * device->block_size +
				    (uint64_t)((const uint8_t *)snapshot.records[index].entry -
					snapshot.block);
			} else {
				EXPECT(
				    ext4_inode_location(fs, owner.number, &entry_offset), EXT4_OK);
				entry_offset +=
				    (uint64_t)((const uint8_t *)snapshot.records[index].entry -
					snapshot.inode);
			}
		}
	}
	ext4_xattr_close(&snapshot);
	CHECK(entry_offset != 0);
	parent_disk = record(device, fs, owner.number);
	value_disk = record(device, fs, number);
	EXPECT(ext4_inode_decode(fs, number, value_disk, &value), EXT4_OK);
	EXPECT(ext4_map_block(fs, &value, 0, &physical), EXT4_OK);
	CHECK(physical != 0);
	entry = (struct ext4_xattr_entry_disk *)(device->cache + entry_offset);
	live = device->live;
	for (damage = EA_VALUE_SELF; damage < EA_DAMAGE_COUNT; damage++) {
		memcpy(device->cache, device->base, device->size);
		switch (damage) {
		case EA_VALUE_SELF:
			ext4_encode32(&entry->value_inode, owner.number);
			break;
		case EA_VALUE_RESERVED:
			ext4_encode32(&entry->value_inode, fs->journal_inode);
			break;
		case EA_VALUE_RANGE:
			ext4_encode32(&entry->value_inode, fs->info.inodes + 1U);
			break;
		case EA_VALUE_OFFSET:
			ext4_encode16(&entry->value_offset, EXT4_XATTR_ALIGNMENT);
			break;
		case EA_VALUE_EMPTY:
			ext4_encode32(&entry->value_size, 0);
			break;
		case EA_VALUE_OVERSIZE:
			ext4_encode32(&entry->value_size, EXT4_XATTR_VALUE_MAX + 1U);
			break;
		case EA_VALUE_SIZE_MISMATCH:
			ext4_encode32(&entry->value_size, EXT4_XATTR_VALUE_MAX - 1U);
			break;
		case EA_ENTRY_HASH:
			ext4_encode32(&entry->hash, ext4_le32(&entry->hash) ^ 1U);
			break;
		case EA_INODE_FLAG:
			ext4_encode32(&value_disk->flags,
			    ext4_le32(&value_disk->flags) & ~(uint32_t)EXT4_INODE_EA_INODE);
			break;
		case EA_INODE_TYPE:
			ext4_encode16(&value_disk->mode, EXT4_MODE_SYMLINK | 0600U);
			break;
		case EA_INODE_LINKS:
			ext4_encode16(&value_disk->links, 0);
			break;
		case EA_INODE_SIZE:
			ext4_encode32(&value_disk->size_lo, 1);
			break;
		case EA_REFERENCE_ZERO:
			ext4_encode32(&value_disk->version_lo, 0);
			ext4_encode32(&value_disk->change_time, 0);
			break;
		case EA_REFERENCE_OVERFLOW:
			ext4_encode32(&value_disk->change_time, UINT32_MAX);
			break;
		case EA_RECURSIVE_ATTRIBUTES:
			ext4_encode32(&value_disk->xattr_block_lo, (uint32_t)physical);
			break;
		case EA_DATA_CHECKSUM:
			device->cache[physical * device->block_size + 7U] ^= 1U;
			break;
		case EA_PARENT_BLOCK_COUNT:
			ext4_encode32(&parent_disk->blocks_lo, 0);
			ext4_encode16(&parent_disk->blocks_hi, 0);
			break;
		case EA_DAMAGE_COUNT:
			CHECK(false);
		}
		ext4_inode_checksum_set(fs, number, value_disk);
		ext4_inode_checksum_set(fs, owner.number, parent_disk);
		if (external != 0) {
			header = (struct ext4_xattr_header_disk *)(device->cache +
			    external * device->block_size);
			ext4_encode32(&header->hash, 0);
			ext4_xattr_checksum_set(fs, external, header);
		}
		memset(output, 0xa5, EXT4_XATTR_VALUE_MAX + 1U);
		returned = SIZE_MAX;
		EXPECT(ext4_get_xattr(fs, owner.number, owner.generation, EXT4_XATTR_USER,
			   (const uint8_t *)"maximum", 7, output, EXT4_XATTR_VALUE_MAX, &returned),
		    EXT4_CORRUPT);
		CHECK(returned == SIZE_MAX && device->live == live && device->writes == 0);
		for (index = 0; index <= EXT4_XATTR_VALUE_MAX; index++) {
			CHECK(output[index] == 0xa5);
		}
	}
	memcpy(device->cache, device->base, device->size);
	allocations = device->allocations;
	reads = device->reads;
	EXPECT(ext4_get_xattr(fs, owner.number, owner.generation, EXT4_XATTR_USER,
		   (const uint8_t *)"maximum", 7, output, EXT4_XATTR_VALUE_MAX, &returned),
	    EXT4_OK);
	allocations = device->allocations - allocations;
	reads = device->reads - reads;
	for (fault = 1; fault <= 2; fault++) {
		limit = fault == 1 ? allocations : reads;
		for (point = 1; point <= limit; point++) {
			memset(output, 0xa5, EXT4_XATTR_VALUE_MAX + 1U);
			returned = SIZE_MAX;
			if (fault == 1) {
				device->fail_allocation = device->allocations + point;
			} else {
				device->fail_read = device->reads + point;
			}
			EXPECT(ext4_get_xattr(fs, owner.number, owner.generation, EXT4_XATTR_USER,
				   (const uint8_t *)"maximum", 7, output, EXT4_XATTR_VALUE_MAX,
				   &returned),
			    fault == 1 ? EXT4_NO_MEMORY : EXT4_IO);
			CHECK(returned == SIZE_MAX && device->live == live && device->writes == 0);
			for (index = 0; index <= EXT4_XATTR_VALUE_MAX; index++) {
				CHECK(output[index] == 0xa5);
			}
			device->fail_allocation = 0;
			device->fail_read = 0;
		}
	}
	ext4_unmount(fs);
	free(output);
	printf("PASS EA_INODE corruption: %u malformed records; reader allocations=%u reads=%u; no "
	       "partial output\n",
	    EA_DAMAGE_COUNT, allocations, reads);
}

#endif

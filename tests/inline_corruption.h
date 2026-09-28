/* SPDX-License-Identifier: BSD-3-Clause */
#ifndef MACHLIN_EXT4_TEST_INLINE_CORRUPTION_H
#define MACHLIN_EXT4_TEST_INLINE_CORRUPTION_H

static void
corruption(struct device *device)
{
	struct ext4_fs *fs;
	struct ext4_inode inode;
	struct ext4_inode refreshed;
	struct ext4_inode output;
	struct ext4_inode untouched;
	struct ext4_inode_disk *disk;
	struct ext4_xattr_entry_disk *attribute;
	struct ext4_dir_header_disk *entry;
	struct ext4_inline_view view;
	struct ext4_inode_update update = attributes(false);
	struct ext4_inode_update create = attributes(true);
	struct ext4_timestamp time = { INLINE_TEST_SECONDS, 0 };
	struct ext4_dir_entry directory_entry;
	uint8_t bytes[128];
	uint8_t saved[128];
	uint64_t location;
	uint64_t cookie;
	uint32_t tail;
	uint32_t hash;
	unsigned int mode;
	size_t completed;
	enum ext4_result error;

	for (mode = 0; mode < 10; mode++) {
		device_reset(device, device->base);
		fs = mount_writer(device);
		inode = lookup(fs, mode < 6 ? "file120" : "entries");
		EXPECT(ext4_inode_location(fs, inode.number, &location), EXT4_OK);
		disk = (struct ext4_inode_disk *)(device->cache + location);
		EXPECT(ext4_inline_open(fs, &inode, disk, &view), EXT4_OK);
		tail = view.tail_offset;
		hash = view.hash_offset;
		ext4_inline_close(&view);
		attribute = (struct ext4_xattr_entry_disk *)((uint8_t *)disk + hash -
		    offsetof(struct ext4_xattr_entry_disk, hash));
		switch (mode) {
		case 0:
			((uint8_t *)(attribute + 1))[0] = 'D';
			break;
		case 1:
			ext4_encode16(&attribute->value_offset, 1);
			break;
		case 2:
			ext4_encode32(&disk->flags, inode.flags | EXT4_INODE_EXTENTS);
			break;
		case 3:
			ext4_encode32(&disk->size_lo, (uint32_t)inode.size + 1);
			break;
		case 4:
			ext4_encode32(&disk->blocks_lo, fs->info.block_size / EXT4_SECTOR_SIZE);
			break;
		case 5:
			ext4_encode32(&attribute->hash, 1);
			break;
		case 6:
			entry = (struct ext4_dir_header_disk *)(disk->block_data +
			    EXT4_INLINE_PARENT_SIZE);
			ext4_encode16(&entry->record_length, EXT4_INODE_BLOCK_BYTES);
			break;
		case 7:
			entry = (struct ext4_dir_header_disk *)((uint8_t *)disk + tail);
			ext4_encode16(&entry->record_length, sizeof(struct ext4_le32));
			ext4_zero(&attribute->hash, sizeof(attribute->hash));
			break;
		case 8:
			ext4_zero(disk->block_data, EXT4_INLINE_PARENT_SIZE);
			break;
		case 9:
			entry = (struct ext4_dir_header_disk *)((uint8_t *)disk + tail);
			((uint8_t *)(entry + 1))[0] = 0;
			ext4_zero(&attribute->hash, sizeof(attribute->hash));
			break;
		}
		ext4_inode_checksum_set(fs, inode.number, disk);
		error = ext4_get_inode(fs, inode.number, &refreshed);
		if (mode == 2) {
			EXPECT(error, EXT4_CORRUPT);
		} else {
			EXPECT(error, EXT4_OK);
			inode = refreshed;
		}
		memset(&untouched, 0xa5, sizeof(untouched));
		output = untouched;
		if (mode < 6) {
			memset(saved, 0x5a, sizeof(saved));
			memcpy(bytes, saved, sizeof(bytes));
			EXPECT(ext4_read(fs, &inode, 0, bytes, sizeof(bytes), &completed),
			    EXT4_CORRUPT);
			CHECK(completed == 0 && memcmp(bytes, saved, sizeof(bytes)) == 0);
			EXPECT(ext4_write(fs, inode.number, inode.generation, 0, bytes, 1, &update,
				   &completed),
			    EXT4_CORRUPT);
		} else {
			cookie = 0;
			EXPECT(ext4_next_dir(fs, &inode, &cookie, &directory_entry), EXT4_CORRUPT);
			CHECK(cookie == 0);
			EXPECT(ext4_lookup(fs, &inode, (const uint8_t *)"e0", 2, &output),
			    EXT4_CORRUPT);
			EXPECT(ext4_create(fs, inode.number, inode.generation,
				   (const uint8_t *)"bad", 3, &create, &time, &output),
			    EXT4_CORRUPT);
		}
		CHECK(device->writes == 0 && memcmp(&output, &untouched, sizeof(output)) == 0);
		ext4_unmount(fs);
		CHECK(device->live == 0);
	}
	puts("PASS inline corruption: 10 checksummed format/region cases reject before output or "
	     "I/O");
}

#endif

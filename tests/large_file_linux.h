/* SPDX-License-Identifier: BSD-3-Clause */
#ifndef MACHLIN_EXT4_TEST_LARGE_FILE_LINUX_H
#define MACHLIN_EXT4_TEST_LARGE_FILE_LINUX_H

static void
linux_return(struct device *device, const char *output)
{
	struct ext4_fs *fs;
	struct ext4_inode seed;
	struct ext4_inode inode;
	struct ext4_inode created;
	struct ext4_inode_update update = attributes(false);
	struct span spans[SPAN_CAPACITY] = { 0 };
	struct span native[4];
	uint64_t limit;
	size_t count;
	FILE *stream;

	EXPECT(ext4_mount_writable(&device->environment, &device->writer, &fs), EXT4_OK);
	seed = lookup(fs, "seed");
	limit = seed.size;
	count = seed_spans(spans, device->block_size, limit);
	verify(fs, &seed, limit, spans, count);
	spans[0] = (struct span){ 0, 61, 0x71 };
	spans[2].byte = 'L';
	spans[count - 1U].byte = 'N';
	created = lookup(fs, "created");
	verify(fs, &created, limit, spans, count);
	inode = lookup(fs, "linux-large");
	native[0] = (struct span){ 0, 61, 0x64 };
	native[1] = (struct span){ UNSIGNED_BYTE_BOUNDARY - 3U, 7, 'V' };
	native[2] = (struct span){ limit - 1U, 1, 'X' };
	verify(fs, &inode, limit, native, 3);
	reject_growth(device, fs, &inode, limit);
	EXPECT(ext4_truncate(fs, inode.number, inode.generation, UNSIGNED_BYTE_BOUNDARY + 1U,
		   &update, &inode),
	    EXT4_OK);
	native[1].length = 4;
	native[2].byte = 0;
	EXPECT(ext4_truncate(fs, inode.number, inode.generation, limit, &update, &inode), EXT4_OK);
	verify(fs, &inode, limit, native, 3);
	native[2].byte = 'Y';
	write_span(fs, &inode, &native[2]);
	native[3] = (struct span){ SIGNED_BYTE_BOUNDARY - 3U, 7, 'W' };
	write_span(fs, &inode, &native[3]);
	verify(fs, &inode, limit, native, 4);
	EXPECT(
	    ext4_truncate(fs, created.number, created.generation, 0, &update, &created), EXT4_OK);
	CHECK(created.size == 0 && created.blocks_512 == 0);
	EXPECT(ext4_sync(fs), EXT4_OK);
	ext4_unmount(fs);
	CHECK(device->live == 0);
	stream = fopen(output, "wbx");
	CHECK(stream != NULL && fwrite(device->stable, 1, device->size, stream) == device->size);
	CHECK(fclose(stream) == 0);
	puts("PASS Linux high-offset reads, shrink/regrowth, boundary writes and return");
}

#endif

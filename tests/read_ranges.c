/* SPDX-License-Identifier: BSD-3-Clause */
#include "internal.h"
#include "map_read.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MODEL_BLOCKS 64U
#define MODEL_NODE_FIRST 2U
#define MODEL_DATA_FIRST 16U
#define MODEL_DATA_OTHER 32U

#define CHECK(condition)                                                                           \
	do {                                                                                       \
		if (!(condition)) {                                                                \
			fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition);            \
			exit(1);                                                                   \
		}                                                                                  \
	} while (0)

struct model {
	struct ext4_fs fs;
	uint8_t *data;
	size_t size;
	size_t reads;
	size_t data_reads;
	size_t data_bytes;
	size_t allocations;
	size_t fail_read;
	size_t fail_data_read;
	size_t fail_allocation;
	size_t live;
	bool poison_failed_read;
};

static enum ext4_result
model_read(void *opaque, uint64_t offset, void *buffer, size_t length)
{
	struct model *model = opaque;

	CHECK(offset <= model->size && length <= model->size - offset);
	model->reads++;
	if (offset >= (uint64_t)MODEL_DATA_FIRST * model->fs.info.block_size) {
		model->data_reads++;
		model->data_bytes += length;
	}
	if (model->reads == model->fail_read ||
	    (model->fail_data_read != 0 && model->data_reads == model->fail_data_read)) {
		if (model->poison_failed_read) {
			memset(buffer, 0xcc, length);
		}
		return EXT4_IO;
	}
	memcpy(buffer, model->data + offset, length);
	return EXT4_OK;
}

static void *
model_allocate(void *opaque, size_t size)
{
	struct model *model = opaque;
	void *buffer;

	if (++model->allocations == model->fail_allocation) {
		return NULL;
	}
	buffer = malloc(size);
	CHECK(buffer != NULL);
	model->live++;
	return buffer;
}

static void
model_release(void *opaque, void *buffer, size_t size)
{
	struct model *model = opaque;

	CHECK(buffer != NULL && model->live != 0 && size == model->fs.info.block_size);
	model->live--;
	free(buffer);
}

static void
model_reset(struct model *model)
{
	CHECK(model->live == 0);
	model->reads = model->allocations = 0;
	model->data_reads = model->data_bytes = 0;
	model->fail_read = model->fail_allocation = 0;
	model->fail_data_read = 0;
	model->poison_failed_read = false;
}

static uint8_t *
model_block(struct model *model, uint32_t number)
{
	CHECK(number < MODEL_BLOCKS);
	return model->data + (size_t)number * model->fs.info.block_size;
}

static void
node_header(uint8_t *node, size_t size, uint16_t depth, uint16_t entries)
{
	struct ext4_extent_header_disk *header = (struct ext4_extent_header_disk *)node;

	memset(node, 0, size);
	ext4_encode16(&header->magic, EXT4_EXTENT_MAGIC);
	ext4_encode16(&header->maximum,
	    (uint16_t)((size - sizeof(*header)) / sizeof(struct ext4_extent_disk)));
	ext4_encode16(&header->depth, depth);
	ext4_encode16(&header->entries, entries);
}

static void
node_extent(
    uint8_t *node, unsigned int position, uint32_t logical, uint16_t length, uint32_t physical)
{
	struct ext4_extent_disk *extents =
	    (struct ext4_extent_disk *)(node + sizeof(struct ext4_extent_header_disk));

	ext4_encode32(&extents[position].logical, logical);
	ext4_encode16(&extents[position].length, length);
	ext4_encode32(&extents[position].physical_lo, physical);
}

static void
node_index(uint8_t *node, unsigned int position, uint32_t logical, uint32_t child)
{
	struct ext4_extent_index_disk *indexes =
	    (struct ext4_extent_index_disk *)(node + sizeof(struct ext4_extent_header_disk));

	ext4_encode32(&indexes[position].logical, logical);
	ext4_encode32(&indexes[position].child_lo, child);
}

static void
node_seal(struct model *model, const struct ext4_inode *inode, uint8_t *node)
{
	struct ext4_extent_header_disk *header = (struct ext4_extent_header_disk *)node;
	size_t tail =
	    sizeof(*header) + ext4_le16(&header->maximum) * sizeof(struct ext4_extent_disk);

	CHECK(tail + sizeof(struct ext4_le32) <= model->fs.info.block_size);
	ext4_encode32((struct ext4_le32 *)(node + tail),
	    ext4_crc32c(ext4_inode_seed(&model->fs, inode), node, tail));
}

static void
check_range(struct model *model, const struct ext4_inode *inode, uint64_t offset, size_t requested,
    uint64_t physical_offset, size_t length, bool hole)
{
	struct ext4_mapping mapping;
	struct ext4_mapping before;
	size_t reads;
	size_t allocations;
	size_t fault;
	unsigned int kind;

	model_reset(model);
	CHECK(ext4_map_read(&model->fs, inode, offset, requested, &mapping) == EXT4_OK);
	CHECK(mapping.length == length && mapping.hole == hole &&
	    mapping.device_offset == physical_offset);
	CHECK(model->allocations <= 1 && model->live == 0);
	reads = model->reads;
	allocations = model->allocations;
	for (kind = 0; kind < 2; kind++) {
		for (fault = 1; fault <= (kind == 0 ? allocations : reads); fault++) {
			model_reset(model);
			memset(&mapping, 0xa5, sizeof(mapping));
			memcpy(&before, &mapping, sizeof(before));
			model->fail_allocation = kind == 0 ? fault : 0;
			model->fail_read = kind == 1 ? fault : 0;
			CHECK(ext4_map_read(&model->fs, inode, offset, requested, &mapping) ==
			    (kind == 0 ? EXT4_NO_MEMORY : EXT4_IO));
			CHECK(memcmp(&before, &mapping, sizeof(before)) == 0 && model->live == 0);
		}
	}
	model_reset(model);
}

static void
check_window(struct model *model, const struct ext4_inode *inode, uint64_t first,
    const uint32_t *physical, size_t count)
{
	struct ext4_mapping mapping;
	uint32_t block_size = model->fs.info.block_size;
	uint64_t start = first * block_size;
	size_t bytes = count * block_size;
	size_t expected_size = (size_t)(inode->size - start);
	uint8_t *expected = calloc(1, bytes);
	uint8_t *output = malloc(bytes + 1U);
	size_t completed;
	size_t reads;
	size_t allocations;
	size_t index;
	size_t offset;
	size_t part;
	size_t fault;
	unsigned int kind;

	CHECK(expected != NULL && output != NULL && expected_size <= bytes);
	for (index = 0; index < count; index++) {
		if (physical[index] != 0) {
			memcpy(expected + index * block_size, model_block(model, physical[index]),
			    block_size);
		}
	}
	model_reset(model);
	memset(output, 0xa5, bytes + 1U);
	CHECK(ext4_read(&model->fs, inode, start, output, bytes + 1U, &completed) == EXT4_OK);
	CHECK(completed == expected_size && memcmp(expected, output, completed) == 0);
	CHECK(output[completed] == 0xa5 && model->live == 0 && model->allocations <= 1);
	reads = model->reads;
	allocations = model->allocations;
	for (kind = 0; kind < 2; kind++) {
		for (fault = 1; fault <= (kind == 0 ? allocations : reads); fault++) {
			model_reset(model);
			model->fail_allocation = kind == 0 ? fault : 0;
			model->fail_read = kind == 1 ? fault : 0;
			memset(output, 0xa5, bytes + 1U);
			CHECK(ext4_read(&model->fs, inode, start, output, bytes + 1U, &completed) ==
			    (kind == 0 ? EXT4_NO_MEMORY : EXT4_IO));
			CHECK(
			    completed <= expected_size && memcmp(expected, output, completed) == 0);
			CHECK(output[expected_size] == 0xa5 && model->live == 0);
		}
	}
	model_reset(model);
	/* Compare every byte of each returned native range against explicit logical
	 * block expectations, including partial requests and final-block padding. */
	for (offset = 37; offset < expected_size; offset += mapping.length) {
		CHECK(ext4_map_read(&model->fs, inode, start + offset, 5U * block_size + 17U,
			  &mapping) == EXT4_OK);
		CHECK(mapping.length > 0 && mapping.length <= 5U * block_size + 17U &&
		    mapping.length <= bytes - offset);
		for (index = 0; index < mapping.length; index += part) {
			part = block_size - (offset + index) % block_size;
			if (part > mapping.length - index) {
				part = mapping.length - index;
			}
			CHECK(mapping.hole == (physical[(offset + index) / block_size] == 0));
			if (!mapping.hole) {
				CHECK(mapping.device_offset + index ==
				    (uint64_t)physical[(offset + index) / block_size] * block_size +
					(offset + index) % block_size);
			}
		}
	}
	CHECK(ext4_read(&model->fs, inode, start + 37U, output, bytes, &completed) == EXT4_OK);
	CHECK(completed == expected_size - 37U && memcmp(expected + 37U, output, completed) == 0);
	CHECK(model->live == 0);
	free(output);
	free(expected);
}

static void
check_cached_run(struct model *model, const struct ext4_inode *inode, uint32_t first,
    uint64_t physical, uint64_t length)
{
	struct ext4_map_reader reader = { 0 };
	uint64_t mapped;
	uint64_t blocks;
	uint64_t offset;
	size_t reads;
	size_t allocations;
	unsigned int pass;

	model_reset(model);
	CHECK(ext4_map_reader_next(&model->fs, inode, &reader, first, &mapped, &blocks) == EXT4_OK);
	CHECK(mapped == physical && blocks == length);
	reads = model->reads;
	allocations = model->allocations;
	model->fail_read = reads + 1U;
	/* Reverse and repeated seeks inside one checked run must neither traverse
	 * indirect ancestors again nor depend on a sequential extent cursor. */
	for (pass = 0; pass < 3U; pass++) {
		for (offset = length; offset != 0; offset--) {
			CHECK(ext4_map_reader_next(&model->fs, inode, &reader,
				  first + (uint32_t)(offset - 1U), &mapped, &blocks) == EXT4_OK);
			CHECK(mapped == (physical == 0 ? 0 : physical + offset - 1U));
			CHECK(blocks == length - offset + 1U);
		}
	}
	CHECK(model->reads == reads && model->allocations == allocations);
	model->fs.aborted = true;
	CHECK(ext4_map_reader_next(&model->fs, inode, &reader, first, &mapped, &blocks) ==
	    EXT4_RECOVERY_REQUIRED);
	model->fs.aborted = false;
	ext4_map_reader_close(&model->fs, &reader);
	CHECK(!ext4_map_reader_cached(&reader, first, &mapped, &blocks));
	/* Closing discards both the decoded run and its metadata view. */
	if (reads != 0) {
		CHECK(ext4_map_reader_next(&model->fs, inode, &reader, first, &mapped, &blocks) ==
		    EXT4_IO);
	}
	ext4_map_reader_close(&model->fs, &reader);
	model_reset(model);
}

static void
extent_cases(struct model *model, struct ext4_inode *inode)
{
	const uint32_t inline_map[] = { MODEL_DATA_FIRST, MODEL_DATA_FIRST + 1U,
		MODEL_DATA_FIRST + 2U, MODEL_DATA_FIRST + 3U, 0, 0, 0, 0, 0, 0, MODEL_DATA_OTHER,
		MODEL_DATA_OTHER + 1U, MODEL_DATA_OTHER + 2U, MODEL_DATA_OTHER + 3U };
	const uint32_t tree_map[] = { MODEL_DATA_FIRST, MODEL_DATA_FIRST + 1U, 0, 0, 0, 0, 0, 0, 0,
		0, MODEL_DATA_OTHER, MODEL_DATA_OTHER + 1U, 0, 0, 0, 0 };
	uint32_t bs = model->fs.info.block_size;
	uint8_t *left = model_block(model, MODEL_NODE_FIRST);
	uint8_t *right = model_block(model, MODEL_NODE_FIRST + 1U);
	uint8_t *leaf_left = model_block(model, MODEL_NODE_FIRST + 2U);
	uint8_t *leaf_right = model_block(model, MODEL_NODE_FIRST + 3U);
	struct ext4_extent_header_disk *header;
	struct ext4_mapping mapping;
	struct ext4_mapping before;
	uint64_t saved_size;
	unsigned int damage;

	inode->flags = EXT4_INODE_EXTENTS;
	inode->size = sizeof(inline_map) / sizeof(*inline_map) * bs - 19U;
	node_header(inode->block_data, sizeof(inode->block_data), 0, 3);
	node_extent(inode->block_data, 0, 0, 4, MODEL_DATA_FIRST);
	node_extent(
	    inode->block_data, 1, 7, EXT4_EXTENT_UNWRITTEN_LIMIT + 3U, MODEL_DATA_FIRST + 4U);
	node_extent(inode->block_data, 2, 10, 4, MODEL_DATA_OTHER);
	check_window(model, inode, 0, inline_map, sizeof(inline_map) / sizeof(*inline_map));
	check_range(model, inode, bs + 37U, SIZE_MAX, (uint64_t)(MODEL_DATA_FIRST + 1U) * bs + 37U,
	    3U * bs - 37U, false);
	check_range(model, inode, 4U * bs + 5U, SIZE_MAX, 0, 3U * bs - 5U, true);
	check_range(model, inode, 8U * bs + 7U, SIZE_MAX, 0, 2U * bs - 7U, true);
	check_cached_run(model, inode, 0, MODEL_DATA_FIRST, 4);
	check_cached_run(model, inode, 4, 0, 3);
	check_cached_run(model, inode, 7, 0, 3);
	check_cached_run(model, inode, 10, MODEL_DATA_OTHER, 4);
	saved_size = inode->size;
	inode->size = bs - 19U;
	check_range(
	    model, inode, 37, SIZE_MAX, (uint64_t)MODEL_DATA_FIRST * bs + 37U, bs - 37U, false);
	inode->size = saved_size;

	node_header(inode->block_data, sizeof(inode->block_data), 2, 2);
	node_index(inode->block_data, 0, 0, MODEL_NODE_FIRST);
	node_index(inode->block_data, 1, 8, MODEL_NODE_FIRST + 1U);
	node_header(left, bs, 1, 1);
	node_index(left, 0, 0, MODEL_NODE_FIRST + 2U);
	node_header(right, bs, 1, 1);
	node_index(right, 0, 8, MODEL_NODE_FIRST + 3U);
	node_header(leaf_left, bs, 0, 1);
	node_extent(leaf_left, 0, 0, 2, MODEL_DATA_FIRST);
	node_header(leaf_right, bs, 0, 1);
	node_extent(leaf_right, 0, 10, 2, MODEL_DATA_OTHER);
	node_seal(model, inode, left);
	node_seal(model, inode, right);
	node_seal(model, inode, leaf_left);
	node_seal(model, inode, leaf_right);
	inode->size = sizeof(tree_map) / sizeof(*tree_map) * bs - 19U;
	check_window(model, inode, 0, tree_map, sizeof(tree_map) / sizeof(*tree_map));
	check_range(model, inode, 2U * bs + 3U, SIZE_MAX, 0, 6U * bs - 3U, true);
	check_range(model, inode, 8U * bs, SIZE_MAX, 0, 2U * bs, true);
	check_cached_run(model, inode, 2, 0, 6);
	check_cached_run(model, inode, 8, 0, 2);
	header = (struct ext4_extent_header_disk *)leaf_right;
	for (damage = 0; damage < 2; damage++) {
		if (damage == 0) {
			node_extent(leaf_right, 0, 10, 0, MODEL_DATA_OTHER);
		} else {
			ext4_encode16(&header->depth, 1);
		}
		node_seal(model, inode, leaf_right);
		memset(&mapping, 0xa5, sizeof(mapping));
		memcpy(&before, &mapping, sizeof(before));
		CHECK(ext4_map_read(&model->fs, inode, 8U * bs, bs, &mapping) == EXT4_CORRUPT);
		CHECK(memcmp(&before, &mapping, sizeof(before)) == 0 && model->live == 0);
		node_extent(leaf_right, 0, 10, 2, MODEL_DATA_OTHER);
		ext4_encode16(&header->depth, 0);
		node_seal(model, inode, leaf_right);
	}
	puts("PASS extent ranges, unwritten data, ancestor boundaries, EOF and structural errors");
}

static void
extent_cursor_cases(struct model *model, struct ext4_inode *inode)
{
	enum { DATA_EXTENTS = 12, LOGICAL_BLOCKS = DATA_EXTENTS * 2 };

	uint32_t physical[LOGICAL_BLOCKS];
	uint32_t bs = model->fs.info.block_size;
	uint8_t *leaf = model_block(model, MODEL_NODE_FIRST);
	uint8_t *output = malloc((size_t)LOGICAL_BLOCKS * bs);
	size_t completed;
	unsigned int index;

	CHECK(output != NULL);
	inode->flags = EXT4_INODE_EXTENTS;
	inode->size = (uint64_t)LOGICAL_BLOCKS * bs;
	node_header(inode->block_data, sizeof(inode->block_data), 1, 1);
	node_index(inode->block_data, 0, 0, MODEL_NODE_FIRST);
	node_header(leaf, bs, 0, DATA_EXTENTS);
	for (index = 0; index < DATA_EXTENTS; index++) {
		physical[index * 2U] = MODEL_DATA_FIRST + index;
		physical[index * 2U + 1U] = 0;
		node_extent(leaf, index, index * 2U, 1, physical[index * 2U]);
	}
	node_seal(model, inode, leaf);
	check_window(model, inode, 0, physical, LOGICAL_BLOCKS);
	model_reset(model);
	CHECK(ext4_read(&model->fs, inode, 0, output, (size_t)inode->size, &completed) == EXT4_OK);
	/* One checked leaf supplies every transition; neighboring physical data is
	 * batched without reading disk bytes for logical holes or allocating data. */
	CHECK(model->reads <= 4 && model->allocations == 1 && model->live == 0);
	CHECK(model->data_bytes == (size_t)DATA_EXTENTS * bs);

	/* A subsequent call sees a changed map, even with unchanged inode fields. */
	node_extent(leaf, 0, 0, 1, MODEL_DATA_OTHER);
	node_seal(model, inode, leaf);
	model_reset(model);
	CHECK(ext4_read(&model->fs, inode, 0, output, bs, &completed) == EXT4_OK);
	CHECK(completed == bs && memcmp(output, model_block(model, MODEL_DATA_OTHER), bs) == 0);
	CHECK(model->reads == 2 && model->live == 0);

	/* Validate the entire leaf before retaining it, including a corrupt record
	 * beyond this request, whose checksum is otherwise correct. */
	node_extent(leaf, DATA_EXTENTS - 1U, 0, 1, MODEL_DATA_FIRST);
	node_seal(model, inode, leaf);
	model_reset(model);
	memset(output, 0xa5, bs);
	CHECK(ext4_read(&model->fs, inode, 0, output, bs, &completed) == EXT4_CORRUPT);
	CHECK(completed == 0 && output[0] == 0xa5 && model->live == 0);
	free(output);
	puts("PASS read-local extent reuse, fresh later calls and full-leaf validation");
}

static void
batched_reads(struct model *model, struct ext4_inode *inode)
{
	enum { DATA_EXTENTS = 40, LOGICAL_BLOCKS = DATA_EXTENTS * 2 };

	struct ext4_map_reader reader = { 0 };
	const uint32_t overlapping[] = { MODEL_DATA_FIRST, 0, MODEL_DATA_FIRST + 1U,
		MODEL_DATA_FIRST + 2U, MODEL_DATA_FIRST + 3U, MODEL_DATA_FIRST + 4U,
		MODEL_DATA_FIRST + 5U, MODEL_DATA_FIRST + 6U, 0, 0 };
	uint32_t physical[LOGICAL_BLOCKS];
	uint32_t bs = model->fs.info.block_size;
	uint32_t logical;
	uint64_t mapped;
	uint64_t run;
	uint8_t *left = model_block(model, MODEL_NODE_FIRST);
	uint8_t *right = model_block(model, MODEL_NODE_FIRST + 1U);
	size_t bytes = (size_t)LOGICAL_BLOCKS * bs;
	uint8_t *expected = calloc(1, bytes);
	uint8_t *output = malloc(bytes + 1U);
	size_t completed;
	size_t window = 3U * bs + 17U;
	unsigned int index;
	unsigned int pass;

	CHECK(expected != NULL && output != NULL);
	inode->flags = EXT4_INODE_EXTENTS;
	inode->size = bytes;
	node_header(inode->block_data, sizeof(inode->block_data), 1, 1);
	node_index(inode->block_data, 0, 0, MODEL_NODE_FIRST);
	node_header(left, bs, 0, DATA_EXTENTS);
	for (index = 0; index < DATA_EXTENTS; index++) {
		physical[index * 2U] = MODEL_DATA_FIRST + index;
		physical[index * 2U + 1U] = 0;
		node_extent(left, index, index * 2U, 1, physical[index * 2U]);
		memcpy(expected + (size_t)index * 2U * bs, model_block(model, physical[index * 2U]),
		    bs);
	}
	node_seal(model, inode, left);
	check_window(model, inode, 0, physical, LOGICAL_BLOCKS);
	model_reset(model);
	/* The sequential hint and binary-search fallback must agree with the
	 * independent block model under forward, backward and permuted requests. */
	for (pass = 0; pass < 3U; pass++) {
		for (index = 0; index < LOGICAL_BLOCKS; index++) {
			logical = pass == 0 ? index
			    : pass == 1	    ? LOGICAL_BLOCKS - 1U - index
					    : index * 37U % LOGICAL_BLOCKS;
			CHECK(ext4_map_reader_next(
				  &model->fs, inode, &reader, logical, &mapped, &run) == EXT4_OK);
			CHECK(mapped == physical[logical]);
			CHECK(run ==
			    (logical == LOGICAL_BLOCKS - 1U ? (uint64_t)UINT32_MAX + 1U - logical
							    : 1U));
		}
	}
	ext4_map_reader_close(&model->fs, &reader);
	CHECK(model->reads == 1 && model->allocations == 1 && model->live == 0);
	model_reset(model);
	CHECK(ext4_read(&model->fs, inode, 0, output, bytes, &completed) == EXT4_OK);
	CHECK(completed == bytes && memcmp(output, expected, bytes) == 0);
	CHECK(model->data_reads >= 2 && model->data_reads <= DATA_EXTENTS / 4U);
	CHECK(model->data_bytes == (size_t)DATA_EXTENTS * bs && model->allocations == 1);
	/* Start inside a hole and finish inside data; both request ends are unaligned. */
	model_reset(model);
	memset(output, 0xa5, bytes + 1U);
	CHECK(ext4_read(&model->fs, inode, bs + 37U, output, window, &completed) == EXT4_OK);
	CHECK(completed == window && memcmp(output, expected + bs + 37U, window) == 0);
	CHECK(model->data_reads == 1 && output[window] == 0xa5);
	model_reset(model);
	model->fail_data_read = 1;
	model->poison_failed_read = true;
	memset(output, 0xa5, bytes + 1U);
	CHECK(ext4_read(&model->fs, inode, bs + 37U, output, window, &completed) == EXT4_IO);
	CHECK(completed == bs - 37U && memcmp(output, expected + bs + 37U, completed) == 0);
	CHECK(output[window] == 0xa5 && model->live == 0);
	/* A failed callback may have overwritten its entire packed destination. No
	 * part of that batch is reported complete, and the earlier prefix survives. */
	model_reset(model);
	model->fail_data_read = 2;
	model->poison_failed_read = true;
	memset(output, 0xa5, bytes + 1U);
	CHECK(ext4_read(&model->fs, inode, 0, output, bytes, &completed) == EXT4_IO);
	CHECK(completed != 0 && completed < bytes && memcmp(output, expected, completed) == 0);
	CHECK(output[bytes] == 0xa5 && model->live == 0);
	model_reset(model);
	/* Physical discontinuities must end a batch, even with a regular hole pattern. */
	for (index = 0; index < DATA_EXTENTS; index++) {
		physical[index * 2U] =
		    MODEL_DATA_FIRST + index / 2U + (index % 2U) * DATA_EXTENTS / 2U;
		node_extent(left, index, index * 2U, 1, physical[index * 2U]);
	}
	node_seal(model, inode, left);
	check_window(model, inode, 0, physical, LOGICAL_BLOCKS);
	model_reset(model);
	CHECK(ext4_read(&model->fs, inode, 0, output, bytes, &completed) == EXT4_OK);
	CHECK(model->data_reads == DATA_EXTENTS);

	/* The later data span overlaps its packed source; its intervening unwritten
	 * extent must become zeros, and a require-data caller must stop before it. */
	node_header(inode->block_data, sizeof(inode->block_data), 0, 3);
	node_extent(inode->block_data, 0, 0, 1, MODEL_DATA_FIRST);
	node_extent(inode->block_data, 1, 1, EXT4_EXTENT_UNWRITTEN_LIMIT + 1U, MODEL_DATA_OTHER);
	node_extent(inode->block_data, 2, 2, 6, MODEL_DATA_FIRST + 1U);
	inode->size = sizeof(overlapping) / sizeof(*overlapping) * bs - 17U;
	check_window(model, inode, 0, overlapping, sizeof(overlapping) / sizeof(*overlapping));
	model_reset(model);
	memset(output, 0xa5, bytes + 1U);
	CHECK(ext4_read_mapped(&model->fs, inode, 0, output, (size_t)inode->size, true,
		  &completed) == EXT4_CORRUPT);
	CHECK(completed == bs && memcmp(output, model_block(model, MODEL_DATA_FIRST), bs) == 0);
	CHECK(output[bs] == 0xa5 && model->data_reads == 1 && model->live == 0);

	/* Planning must not fetch an unvisited leaf early. Its error follows the
	 * successful data and hole prefix from the already validated left leaf. */
	node_header(inode->block_data, sizeof(inode->block_data), 1, 2);
	node_index(inode->block_data, 0, 0, MODEL_NODE_FIRST);
	node_index(inode->block_data, 1, 8, MODEL_NODE_FIRST + 1U);
	node_header(left, bs, 0, 4);
	for (index = 0; index < 4; index++) {
		node_extent(left, index, index * 2U, 1, MODEL_DATA_FIRST + index);
	}
	node_header(right, bs, 0, 1);
	node_extent(right, 0, 8, 0, MODEL_DATA_FIRST + 4U);
	node_seal(model, inode, left);
	node_seal(model, inode, right);
	inode->size = 10U * bs;
	model_reset(model);
	CHECK(ext4_read(&model->fs, inode, 0, output, (size_t)inode->size, &completed) ==
	    EXT4_CORRUPT);
	CHECK(completed == 8U * bs && memcmp(output, expected, completed) == 0);
	CHECK(model->data_reads == 1 && model->reads == 3 && model->live == 0);
	model_reset(model);
	model->fail_read = 3;
	model->poison_failed_read = true;
	CHECK(ext4_read(&model->fs, inode, 0, output, (size_t)inode->size, &completed) == EXT4_IO);
	CHECK(completed == 8U * bs && memcmp(output, expected, completed) == 0 && model->live == 0);
	model_reset(model);
	free(output);
	free(expected);
	puts("PASS batched sparse reads, bounded I/O, overlap, unwritten data and error prefixes");
}

static void
indirect_cases(struct model *model, struct ext4_inode *inode)
{
	const uint32_t beginning[] = { MODEL_DATA_FIRST, MODEL_DATA_FIRST + 1U, 0, MODEL_DATA_OTHER,
		0, 0 };
	const uint32_t ending[] = { MODEL_DATA_OTHER + 2U, MODEL_DATA_OTHER + 3U, 0, 0 };
	struct ext4_le32 *root = (struct ext4_le32 *)inode->block_data;
	struct ext4_le32 *pointers;
	struct ext4_map_reader reader = { 0 };
	uint32_t bs = model->fs.info.block_size;
	uint32_t per_block = bs / sizeof(*pointers);
	uint64_t first = EXT4_DIRECT_BLOCKS;
	uint64_t span = per_block;
	uint64_t mapped;
	uint64_t run;
	size_t reads;
	unsigned int depth;
	unsigned int level;

	inode->flags = 0;
	for (depth = 0; depth <= EXT4_INDIRECT_LEVELS; depth++) {
		memset(inode->block_data, 0, sizeof(inode->block_data));
		pointers = root;
		if (depth != 0) {
			ext4_encode32(&root[EXT4_DIRECT_BLOCKS + depth - 1U], MODEL_NODE_FIRST);
			for (level = 0; level < depth; level++) {
				pointers = (struct ext4_le32 *)model_block(
				    model, MODEL_NODE_FIRST + level);
				memset(pointers, 0, bs);
				if (level + 1U < depth) {
					ext4_encode32(&pointers[0], MODEL_NODE_FIRST + level + 1U);
				}
			}
		}
		ext4_encode32(&pointers[0], MODEL_DATA_FIRST);
		ext4_encode32(&pointers[1], MODEL_DATA_FIRST + 1U);
		ext4_encode32(&pointers[3], MODEL_DATA_OTHER);
		inode->size = ((depth == 0 ? 0 : first) + 6U) * bs - 19U;
		check_window(model, inode, depth == 0 ? 0 : first, beginning, 6);
		check_cached_run(
		    model, inode, depth == 0 ? 0 : (uint32_t)first, MODEL_DATA_FIRST, 2);
		if (depth != 0) {
			/* A failed miss may overwrite traversal scratch. The last decoded
			 * run remains valid, but must never satisfy the failed address. */
			CHECK(ext4_map_reader_next(&model->fs, inode, &reader, (uint32_t)first,
				  &mapped, &run) == EXT4_OK);
			model->fail_read = model->reads + depth;
			model->poison_failed_read = true;
			CHECK(ext4_map_reader_next(&model->fs, inode, &reader, (uint32_t)first + 3U,
				  &mapped, &run) == EXT4_IO);
			reads = model->reads;
			CHECK(ext4_map_reader_next(&model->fs, inode, &reader, (uint32_t)first + 1U,
				  &mapped, &run) == EXT4_OK);
			CHECK(mapped == MODEL_DATA_FIRST + 1U && run == 1 && model->reads == reads);
			model->fail_read = 0;
			CHECK(ext4_map_reader_next(&model->fs, inode, &reader, (uint32_t)first + 3U,
				  &mapped, &run) == EXT4_OK);
			CHECK(mapped == MODEL_DATA_OTHER && run == 1);
			ext4_map_reader_close(&model->fs, &reader);
			model_reset(model);
			ext4_encode32(&pointers[per_block - 2U], MODEL_DATA_OTHER + 2U);
			ext4_encode32(&pointers[per_block - 1U], MODEL_DATA_OTHER + 3U);
			inode->size = (first + per_block + 2U) * bs - 19U;
			check_window(model, inode, first + per_block - 2U, ending, 4);
			check_range(model, inode, (first + per_block) * bs + 7U, SIZE_MAX, 0,
			    2U * bs - 7U, true);
			first += span;
			span *= per_block;
		}
	}
	puts("PASS direct and single/double/triple-indirect ranges and absent ancestors");
}

/* More leaves than the cache can hold, with two external levels. Hits must
 * avoid metadata I/O; failed traversals must not publish or damage a leaf. */
static void
cached_leaves(struct model *model, struct ext4_inode *inode)
{
	struct ext4_extent_cache cache = { 0 };
	struct ext4_map_reader reader = { .cache = &cache };
	uint8_t *index_node = model_block(model, MODEL_NODE_FIRST);
	uint8_t *leaf;
	uint64_t physical;
	uint64_t blocks;
	uint32_t capacity = EXT4_READ_CACHE_BYTES / model->fs.info.block_size;
	uint32_t index;
	uint32_t pass;
	size_t reads;
	size_t allocations;

	model_reset(model);
	if (capacity > EXT4_READ_CACHE_LEAVES) {
		capacity = EXT4_READ_CACHE_LEAVES;
	}
	inode->flags = EXT4_INODE_EXTENTS;
	node_header(inode->block_data, sizeof(inode->block_data), 2, 1);
	node_index(inode->block_data, 0, 0, MODEL_NODE_FIRST);
	node_header(index_node, model->fs.info.block_size, 1, 12);
	for (index = 0; index < 12U; index++) {
		node_index(index_node, index, index * 8U, MODEL_NODE_FIRST + 1U + index);
		leaf = model_block(model, MODEL_NODE_FIRST + 1U + index);
		node_header(leaf, model->fs.info.block_size, 0, 2);
		node_extent(leaf, 0, index * 8U, 1, MODEL_DATA_FIRST + index);
		node_extent(leaf, 1, index * 8U + 2U, EXT4_EXTENT_UNWRITTEN_LIMIT + 1U,
		    MODEL_DATA_OTHER + index);
		node_seal(model, inode, leaf);
	}
	node_seal(model, inode, index_node);
	for (index = 0; index < capacity; index++) {
		CHECK(ext4_map_reader_next(
			  &model->fs, inode, &reader, index * 8U, &physical, &blocks) == EXT4_OK);
		CHECK(physical == MODEL_DATA_FIRST + index && blocks == 1);
	}
	CHECK(model->live == capacity);
	reads = model->reads;
	allocations = model->allocations;
	for (index = capacity; index != 0; index--) {
		CHECK(ext4_map_reader_next(&model->fs, inode, &reader, (index - 1U) * 8U, &physical,
			  &blocks) == EXT4_OK);
		CHECK(physical == MODEL_DATA_FIRST + index - 1U && blocks == 1);
	}
	CHECK(model->reads == reads && model->allocations == allocations);
	model->fail_allocation = model->allocations + 1U;
	CHECK(ext4_map_reader_next(&model->fs, inode, &reader, capacity * 8U, &physical, &blocks) ==
	    EXT4_NO_MEMORY);
	model->fail_allocation = 0;
	model->fail_read = model->reads + 2U;
	CHECK(ext4_map_reader_next(&model->fs, inode, &reader, capacity * 8U, &physical, &blocks) ==
	    EXT4_IO);
	model->fail_read = 0;
	leaf = model_block(model, MODEL_NODE_FIRST + 1U + capacity);
	node_extent(leaf, 1, capacity * 8U + 2U, 0, MODEL_DATA_OTHER + capacity);
	node_seal(model, inode, leaf);
	CHECK(ext4_map_reader_next(&model->fs, inode, &reader, capacity * 8U, &physical, &blocks) ==
	    EXT4_CORRUPT);
	reads = model->reads;
	/* A previous cache entry remains usable after all three failed misses. */
	CHECK(ext4_map_reader_next(&model->fs, inode, &reader, 0, &physical, &blocks) == EXT4_OK);
	CHECK(physical == MODEL_DATA_FIRST && model->reads == reads);
	node_extent(leaf, 1, capacity * 8U + 2U, EXT4_EXTENT_UNWRITTEN_LIMIT + 1U,
	    MODEL_DATA_OTHER + capacity);
	node_seal(model, inode, leaf);
	allocations = model->allocations;
	for (pass = 0; pass < 3U; pass++) {
		for (index = 0; index < 12U; index++) {
			CHECK(ext4_map_reader_next(&model->fs, inode, &reader, index * 8U,
				  &physical, &blocks) == EXT4_OK);
			CHECK(physical == MODEL_DATA_FIRST + index && blocks == 1);
			CHECK(ext4_map_reader_next(&model->fs, inode, &reader, index * 8U + 1U,
				  &physical, &blocks) == EXT4_OK);
			CHECK(physical == 0 && blocks == 1);
			CHECK(ext4_map_reader_next(&model->fs, inode, &reader, index * 8U + 2U,
				  &physical, &blocks) == EXT4_OK);
			CHECK(physical == 0 && blocks == 1);
		}
	}
	CHECK(model->live == capacity + 1U && model->allocations == allocations);
	model->fs.aborted = true;
	CHECK(ext4_map_reader_next(&model->fs, inode, &reader, 11U * 8U, &physical, &blocks) ==
	    EXT4_RECOVERY_REQUIRED);
	model->fs.aborted = false;
	ext4_map_reader_close(&model->fs, &reader);
	model_reset(model);
	puts("PASS bounded leaf cache, eviction, failed misses, whole-leaf validation and abort");
}

static void
native_mapping_guards(struct model *model, const struct ext4_inode *inode)
{
	struct ext4_read_state state = { 0 };
	struct ext4_inode_hold hold = { 0 };
	struct ext4_mapping mapping;
	struct ext4_mapping before;

	model_reset(model);
	state.inode = *inode;
	state.revision = model->fs.read_revision;
	hold.fs = &model->fs;
	hold.references = 1;
	hold.reader = &state;
	memset(&mapping, 0xa5, sizeof(mapping));
	memcpy(&before, &mapping, sizeof(before));
	CHECK(ext4_inode_can_map_read(&state.inode));
	CHECK(!ext4_inode_can_map_read(NULL));
	state.inode.flags = EXT4_INODE_EXTENTS | EXT4_INODE_ENCRYPT;
	CHECK(!ext4_inode_can_map_read(&state.inode));
	CHECK(ext4_map_read_held(&hold, 0, 1, &mapping) == EXT4_ENCRYPTED);
	state.inode.flags = EXT4_INODE_EXTENTS | EXT4_INODE_VERITY;
	CHECK(!ext4_inode_can_map_read(&state.inode));
	CHECK(ext4_map_read_held(&hold, 0, 1, &mapping) == EXT4_UNSUPPORTED);
	state.inode.flags = EXT4_INODE_INLINE_DATA;
	CHECK(!ext4_inode_can_map_read(&state.inode));
	CHECK(ext4_map_read_held(&hold, 0, 1, &mapping) == EXT4_UNSUPPORTED);
	state.inode.mode = EXT4_MODE_DIRECTORY | 0755;
	CHECK(!ext4_inode_can_map_read(&state.inode));
	CHECK(ext4_map_read_held(&hold, 0, 1, &mapping) == EXT4_INVALID_ARGUMENT);
	CHECK(memcmp(&before, &mapping, sizeof(before)) == 0);
	CHECK(model->reads == 0 && model->allocations == 0);
	puts("PASS held mappings preserve encryption, verity, inline and file-type guards");
}

static void
logical_limit(struct model *model, struct ext4_inode *inode)
{
	struct ext4_mapping mapping;
	uint32_t bs = model->fs.info.block_size;
	uint64_t limit = ((uint64_t)UINT32_MAX + 1U) * bs;
	uint8_t *output = malloc(bs + 1U);
	size_t completed;
	size_t index;

	CHECK(output != NULL);
	inode->flags = EXT4_INODE_EXTENTS;
	node_header(inode->block_data, sizeof(inode->block_data), 0, 0);
	inode->size = limit + bs;
	check_cached_run(model, inode, UINT32_MAX - 7U, 0, 8);
	node_header(inode->block_data, sizeof(inode->block_data), 0, 1);
	node_extent(inode->block_data, 0, UINT32_MAX - 7U, 8, MODEL_DATA_FIRST);
	check_cached_run(model, inode, UINT32_MAX - 7U, MODEL_DATA_FIRST, 8);
	node_header(inode->block_data, sizeof(inode->block_data), 0, 0);
	check_range(model, inode, limit - bs + 17U, SIZE_MAX, 0, bs - 17U, true);
	CHECK(ext4_map_read(&model->fs, inode, limit, bs, &mapping) == EXT4_RANGE);
	memset(output, 0xa5, bs + 1U);
	CHECK(ext4_read(&model->fs, inode, limit - bs, output, bs + 1U, &completed) == EXT4_RANGE);
	CHECK(completed == bs && output[bs] == 0xa5 && model->live == 0);
	for (index = 0; index < bs; index++) {
		CHECK(output[index] == 0);
	}
	free(output);
	puts("PASS logical address limit preserves the completed read prefix");
}

int
main(void)
{
	struct model model;
	struct ext4_inode inode;
	uint32_t bs;
	size_t offset;
	unsigned int checksum;

	for (bs = 1024; bs <= EXT4_MAX_BLOCK_SIZE; bs *= 4U) {
		for (checksum = 0; checksum < 2; checksum++) {
			memset(&model, 0, sizeof(model));
			memset(&inode, 0, sizeof(inode));
			model.size = (size_t)MODEL_BLOCKS * bs;
			model.data = malloc(model.size);
			CHECK(model.data != NULL);
			for (offset = 0; offset < model.size; offset++) {
				model.data[offset] =
				    (uint8_t)(offset / bs * 17U + offset % bs * 29U + 3U);
			}
			model.fs.environment = (struct ext4_environment){ &model, model.size,
				model_read, model_allocate, model_release };
			model.fs.info.block_size = bs;
			model.fs.info.blocks = MODEL_BLOCKS;
			model.fs.cluster_blocks = 1;
			model.fs.metadata_checksum = checksum != 0;
			inode.mode = EXT4_MODE_REGULAR | 0644;
			inode.number = EXT4_ROOT_INODE;
			inode.generation = 1;
			extent_cases(&model, &inode);
			extent_cursor_cases(&model, &inode);
			batched_reads(&model, &inode);
			cached_leaves(&model, &inode);
			indirect_cases(&model, &inode);
			native_mapping_guards(&model, &inode);
			logical_limit(&model, &inode);
			CHECK(model.live == 0);
			free(model.data);
			printf(
			    "PASS modeled read ranges block_size=%u checksum=%u\n", bs, checksum);
		}
	}
	return 0;
}

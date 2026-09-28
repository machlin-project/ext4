/* SPDX-License-Identifier: BSD-3-Clause */
#include "internal.h"

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
	size_t allocations;
	size_t fail_read;
	size_t fail_allocation;
	size_t live;
};

static enum ext4_result
model_read(void *opaque, uint64_t offset, void *buffer, size_t length)
{
	struct model *model = opaque;

	CHECK(offset <= model->size && length <= model->size - offset);
	if (++model->reads == model->fail_read) {
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
	model->fail_read = model->fail_allocation = 0;
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
indirect_cases(struct model *model, struct ext4_inode *inode)
{
	const uint32_t beginning[] = { MODEL_DATA_FIRST, MODEL_DATA_FIRST + 1U, 0, MODEL_DATA_OTHER,
		0, 0 };
	const uint32_t ending[] = { MODEL_DATA_OTHER + 2U, MODEL_DATA_OTHER + 3U, 0, 0 };
	struct ext4_le32 *root = (struct ext4_le32 *)inode->block_data;
	struct ext4_le32 *pointers;
	uint32_t bs = model->fs.info.block_size;
	uint32_t per_block = bs / sizeof(*pointers);
	uint64_t first = EXT4_DIRECT_BLOCKS;
	uint64_t span = per_block;
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
		if (depth != 0) {
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
			indirect_cases(&model, &inode);
			logical_limit(&model, &inode);
			CHECK(model.live == 0);
			free(model.data);
			printf(
			    "PASS modeled read ranges block_size=%u checksum=%u\n", bs, checksum);
		}
	}
	return 0;
}

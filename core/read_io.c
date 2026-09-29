/* SPDX-License-Identifier: BSD-3-Clause */
#include "map_read.h"

#define EXT4_READ_BATCH_SPANS 32U
#define EXT4_READ_BATCH_BYTES (256U * 1024U)

struct ext4_read_span {
	size_t offset;
	size_t length;
};

/* One contiguous device read, expanded into logical spans in the caller's
 * buffer. Holes consume no device bytes. The first span always starts at zero. */
struct ext4_read_plan {
	struct ext4_read_span spans[EXT4_READ_BATCH_SPANS];
	size_t count;
	size_t data_bytes;
	size_t logical_bytes;
};

static size_t
ext4_read_run_bytes(uint32_t block_size, uint64_t offset, uint64_t blocks, size_t remaining)
{
	uint64_t bytes = blocks * block_size - offset % block_size;

	return bytes < remaining ? (size_t)bytes : remaining;
}

static void
ext4_read_plan_build(struct ext4_fs *fs, struct ext4_map_reader *reader, uint64_t offset,
    uint64_t device_offset, size_t first, size_t length, bool require_data,
    struct ext4_read_plan *plan)
{
	struct ext4_read_span *span;
	uint64_t logical;
	uint64_t physical;
	uint64_t blocks;
	size_t position = first;
	size_t chunk;

	plan->spans[0] = (struct ext4_read_span){ 0, first };
	plan->count = 1;
	plan->data_bytes = first;
	plan->logical_bytes = first;
	while (position < length && plan->count < EXT4_READ_BATCH_SPANS &&
	    plan->data_bytes < EXT4_READ_BATCH_BYTES) {
		logical = (offset + position) / fs->info.block_size;
		if (logical > UINT32_MAX ||
		    !ext4_map_reader_cached(reader, (uint32_t)logical, &physical, &blocks)) {
			break;
		}
		chunk = ext4_read_run_bytes(
		    fs->info.block_size, offset + position, blocks, length - position);
		if (physical == 0) {
			if (require_data) {
				break;
			}
			/* Publish this gap only if a following data span can join the read. */
			position += chunk;
			continue;
		}
		physical =
		    physical * fs->info.block_size + (offset + position) % fs->info.block_size;
		if (physical != device_offset + plan->data_bytes) {
			break;
		}
		if (chunk > EXT4_READ_BATCH_BYTES - plan->data_bytes) {
			chunk = EXT4_READ_BATCH_BYTES - plan->data_bytes;
		}
		span = &plan->spans[plan->count - 1U];
		if (position == plan->logical_bytes) {
			span->length += chunk;
		} else {
			plan->spans[plan->count++] = (struct ext4_read_span){ position, chunk };
		}
		plan->data_bytes += chunk;
		position += chunk;
		plan->logical_bytes = position;
	}
}

/* Move later spans first, before zeroing the gaps. A span can overlap its packed
 * source when it is longer than the preceding holes: copy that span backwards.
 * No extra data buffer or heap allocation is needed, even on the kernel path. */
static void
ext4_read_plan_expand(const struct ext4_read_plan *plan, uint8_t *output)
{
	const struct ext4_read_span *span;
	size_t source = plan->data_bytes;
	size_t end = plan->logical_bytes;
	size_t index = plan->count;
	size_t remaining;

	while (index != 0) {
		span = &plan->spans[--index];
		source -= span->length;
		if (span->offset != source) {
			if (span->offset - source >= span->length) {
				ext4_copy(output + span->offset, output + source, span->length);
			} else {
				remaining = span->length;
				while (remaining != 0) {
					remaining--;
					output[span->offset + remaining] =
					    output[source + remaining];
				}
			}
		}
		ext4_zero(output + span->offset + span->length, end - span->offset - span->length);
		end = span->offset;
	}
}

static enum ext4_result
ext4_read_batch(struct ext4_fs *fs, struct ext4_map_reader *reader, uint64_t offset,
    uint64_t device_offset, uint8_t *output, size_t length, bool require_data, size_t *chunk)
{
	struct ext4_read_plan plan;
	enum ext4_result error;

	ext4_read_plan_build(
	    fs, reader, offset, device_offset, *chunk, length, require_data, &plan);
	error = ext4_device_read(fs, device_offset, output, plan.data_bytes);
	if (error == EXT4_OK) {
		if (plan.count > 1U) {
			ext4_read_plan_expand(&plan, output);
		}
		*chunk = plan.logical_bytes;
	}
	return error;
}

enum ext4_result
ext4_map_reader_read(struct ext4_fs *fs, const struct ext4_inode *inode,
    struct ext4_map_reader *reader, uint64_t offset, void *buffer, size_t length, bool require_data,
    size_t *completed)
{
	uint8_t *output = buffer;
	uint64_t physical;
	uint64_t logical;
	uint64_t blocks;
	size_t chunk;
	size_t remaining;
	enum ext4_result error;

	*completed = 0;
	while (*completed < length) {
		logical = offset / fs->info.block_size;
		if (logical > UINT32_MAX) {
			return EXT4_RANGE;
		}
		error =
		    ext4_map_reader_next(fs, inode, reader, (uint32_t)logical, &physical, &blocks);
		if (error != EXT4_OK) {
			return error;
		}
		remaining = length - *completed;
		chunk = ext4_read_run_bytes(fs->info.block_size, offset, blocks, remaining);
		if (physical == 0) {
			if (require_data) {
				return EXT4_CORRUPT;
			}
			ext4_zero(output + *completed, chunk);
		} else {
			physical = physical * fs->info.block_size + offset % fs->info.block_size;
			if (chunk < remaining && chunk < EXT4_READ_BATCH_BYTES &&
			    reader->cursor.leaf != NULL) {
				error = ext4_read_batch(fs, reader, offset, physical,
				    output + *completed, remaining, require_data, &chunk);
			} else {
				error = ext4_device_read(fs, physical, output + *completed, chunk);
			}
			if (error != EXT4_OK) {
				return error;
			}
		}
		*completed += chunk;
		offset += chunk;
	}
	return EXT4_OK;
}

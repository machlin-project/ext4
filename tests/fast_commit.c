/* SPDX-License-Identifier: BSD-3-Clause */
#include "fast_commit.h"
#include "image.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(expression)                                                                          \
	do {                                                                                       \
		if (!(expression)) {                                                               \
			fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #expression);           \
			exit(1);                                                                   \
		}                                                                                  \
	} while (0)
#define JOURNAL_DUMP_LIMIT (128U * 1024U * 1024U)

struct source {
	uint8_t *bytes;
	size_t size;
	uint32_t reads;
	uint32_t fail_read;
};

static enum ext4_result
read_source(void *context, uint64_t offset, void *buffer, size_t length)
{
	struct source *source = context;

	CHECK(offset <= source->size && length <= source->size - offset);
	if (++source->reads == source->fail_read) {
		memset(buffer, 0, length / 2U);
		return EXT4_IO;
	}
	memcpy(buffer, source->bytes + offset, length);
	return EXT4_OK;
}

int
main(int argc, char **argv)
{
	struct ext4_posix_image image;
	struct source source = { 0 };
	struct ext4_fs fs = { 0 };
	struct ext4_journal journal = { 0 };
	const struct ext4_jbd_super *super;
	struct ext4_fast_commit *log;
	struct ext4_fast_commit *other;
	struct ext4_fc_header_disk *head;
	struct ext4_fc_tail_disk *tail;
	const void *value;
	uint8_t *buffer;
	uint8_t *saved_record;
	uint32_t first;
	uint32_t blocks;
	uint32_t sequence;
	uint32_t expected;
	uint32_t reads;
	uint32_t index;
	uint32_t types[EXT4_FC_HEAD + 1U] = { 0 };
	uint32_t old_checksum;
	uint32_t cached_block;
	uint32_t cached_reads;
	uint32_t expected_reads;
	uint16_t old_length;
	uint8_t old_byte;

	if (argc != 4) {
		fprintf(
		    stderr, "usage: %s JOURNAL_DUMP EXPECTED_SEQUENCE EXPECTED_COMMITS\n", argv[0]);
		return 2;
	}
	sequence = (uint32_t)strtoul(argv[2], NULL, 10);
	expected = (uint32_t)strtoul(argv[3], NULL, 10);
	CHECK(expected > 1);
	CHECK(ext4_posix_open(&image, argv[1]) == EXT4_OK);
	CHECK(image.environment.size_bytes >= sizeof(*super) &&
	    image.environment.size_bytes <= JOURNAL_DUMP_LIMIT);
	source.size = (size_t)image.environment.size_bytes;
	source.bytes = malloc(source.size);
	CHECK(source.bytes != NULL);
	CHECK(image.environment.read(image.environment.context, 0, source.bytes, source.size) ==
	    EXT4_OK);
	super = (const struct ext4_jbd_super *)source.bytes;
	CHECK(ext4_be32(&super->header.magic) == EXT4_JBD_MAGIC);
	fs.environment = image.environment;
	fs.info.block_size = ext4_be32(&super->block_size);
	fs.inode_size = 256;
	CHECK(fs.info.block_size == 1024 || fs.info.block_size == 4096);
	journal.fs = &fs;
	journal.blocks = ext4_be32(&super->max_length);
	journal.first = ext4_be32(&super->first);
	CHECK((uint64_t)journal.blocks * fs.info.block_size <= source.size);
	blocks = ext4_be32(&super->fast_commit_blocks);
	CHECK(blocks > 1 && blocks < journal.blocks);
	first = journal.blocks - blocks + 1U;
	journal.external.context = &source;
	journal.external.read = read_source;
	journal.work = malloc(fs.info.block_size);
	buffer = malloc(fs.info.block_size);
	CHECK(journal.work != NULL && buffer != NULL);
	CHECK(ext4_fast_commit_load(&journal, first, journal.blocks, sequence, &log) == EXT4_OK);
	CHECK(log->commits == expected && log->count > expected);
	reads = source.reads;
	for (index = 0; index < log->count; index++) {
		CHECK(log->records[index].type <= EXT4_FC_HEAD);
		types[log->records[index].type]++;
		CHECK(
		    ext4_fast_commit_read(log, index, buffer, &value) == EXT4_OK && value != NULL);
	}
	CHECK(types[EXT4_FC_HEAD] == 1 && types[EXT4_FC_TAIL] == expected);
	cached_block = UINT32_MAX;
	cached_reads = source.reads;
	expected_reads = 0;
	for (index = 0; index < log->count; index++) {
		if (index == 0 || log->records[index].block != log->records[index - 1U].block) {
			expected_reads++;
		}
		CHECK(ext4_fast_commit_read_cached(log, index, buffer, &cached_block, &value) ==
		    EXT4_OK);
		CHECK(value != NULL && cached_block == log->records[index].block);
	}
	CHECK(source.reads - cached_reads == expected_reads && expected_reads < log->count);
	/* A failed block load must not publish a cache hit for a later retry. */
	cached_block = UINT32_MAX;
	source.fail_read = source.reads + 1U;
	CHECK(ext4_fast_commit_read_cached(log, 0, buffer, &cached_block, &value) == EXT4_IO);
	CHECK(cached_block == UINT32_MAX && value == NULL);
	source.fail_read = 0;
	CHECK(ext4_fast_commit_read_cached(log, 0, buffer, &cached_block, &value) == EXT4_OK);
	/* A partial failed read of another block also invalidates the old cache. */
	CHECK(log->records[log->count - 1U].block != log->records[0].block);
	source.fail_read = source.reads + 1U;
	CHECK(ext4_fast_commit_read_cached(log, log->count - 1U, buffer, &cached_block, &value) ==
	    EXT4_IO);
	CHECK(cached_block == UINT32_MAX && value == NULL);
	source.fail_read = 0;
	CHECK(ext4_fast_commit_read_cached(log, 0, buffer, &cached_block, &value) == EXT4_OK);
	/* A later reader must not silently consume a record changed after scan. */
	saved_record = source.bytes + (size_t)log->records[1].block * fs.info.block_size +
	    log->records[1].offset + sizeof(struct ext4_fc_header_disk);
	old_byte = *saved_record;
	*saved_record ^= 1U;
	/* The first record loads a block containing the damaged second record.
	 * A cache hit must still compare that record with the original scan. */
	CHECK(log->records[0].block == log->records[1].block);
	cached_block = UINT32_MAX;
	CHECK(ext4_fast_commit_read_cached(log, 0, buffer, &cached_block, &value) == EXT4_OK);
	CHECK(ext4_fast_commit_read_cached(log, 1, buffer, &cached_block, &value) == EXT4_CORRUPT);
	CHECK(value == NULL);
	CHECK(ext4_fast_commit_read(log, 1, buffer, &value) == EXT4_CORRUPT && value == NULL);
	/* The same alteration invalidates the first committed CRC. */
	CHECK(ext4_fast_commit_load(&journal, first, journal.blocks, sequence, &other) ==
	    EXT4_CORRUPT);
	CHECK(other == NULL);
	*saved_record = old_byte;
	CHECK(ext4_fast_commit_load(&journal, first, journal.blocks, sequence + 1U, &other) ==
	    EXT4_OK);
	CHECK(other->count == 0 && other->commits == 0);
	ext4_fast_commit_close(other);
	/* A torn last commit retains exactly the checked preceding prefix. */
	index = log->count - 1U;
	CHECK(log->records[index].type == EXT4_FC_TAIL);
	tail = (struct ext4_fc_tail_disk *)(source.bytes +
	    (size_t)log->records[index].block * fs.info.block_size + log->records[index].offset +
	    sizeof(struct ext4_fc_header_disk));
	old_checksum = ext4_le32(&tail->checksum);
	ext4_encode32(&tail->checksum, old_checksum ^ 1U);
	CHECK(ext4_fast_commit_load(&journal, first, journal.blocks, sequence, &other) == EXT4_OK);
	CHECK(other->commits == expected - 1U && other->discarded_tail);
	ext4_fast_commit_close(other);
	ext4_encode32(&tail->checksum, old_checksum);
	head = (struct ext4_fc_header_disk *)(source.bytes + (size_t)first * fs.info.block_size);
	old_length = ext4_le16(&head->length);
	ext4_encode16(&head->length, UINT16_MAX);
	CHECK(ext4_fast_commit_load(&journal, first, journal.blocks, sequence, &other) ==
	    EXT4_CORRUPT);
	CHECK(other == NULL);
	ext4_encode16(&head->length, old_length);
	ext4_fast_commit_close(log);
	for (index = 1; index <= reads; index++) {
		source.reads = 0;
		source.fail_read = index;
		CHECK(ext4_fast_commit_load(&journal, first, journal.blocks, sequence, &other) ==
		    EXT4_IO);
		CHECK(other == NULL && image.live_allocations == 0);
	}
	source.fail_read = 0;
	image.fail_allocation_at = image.allocation_calls + 1U;
	CHECK(ext4_fast_commit_load(&journal, first, journal.blocks, sequence, &other) ==
	    EXT4_NO_MEMORY);
	CHECK(other == NULL && image.live_allocations == 0);
	image.fail_allocation_at = 0;
	CHECK(ext4_fast_commit_load(&journal, first, journal.blocks, sequence, &other) == EXT4_OK);
	ext4_fast_commit_close(other);
	CHECK(image.live_allocations == 0 && image.write_calls == 0 && image.flush_calls == 0);
	printf("PASS %u fast commits; %u source read failures; immutable scanned records\n",
	    expected, reads);
	for (index = 1; index <= EXT4_FC_HEAD; index++) {
		printf("tag %u: %u\n", index, types[index]);
	}
	free(buffer);
	free(journal.work);
	free(source.bytes);
	ext4_posix_close(&image);
	return 0;
}

/* SPDX-License-Identifier: BSD-3-Clause */
#include "journal.h"

static const uint8_t *
ext4_journal_current(const struct ext4_journal *journal, uint64_t block)
{
	const uint8_t *snapshot = NULL;

	if (journal->compound != NULL) {
		snapshot = ext4_transaction_peek(journal->compound, block);
	}
	if (snapshot == NULL && journal->checkpoint != NULL) {
		snapshot = ext4_transaction_peek(journal->checkpoint, block);
	}
	return snapshot;
}

/* Held snapshots already own the latest bytes. Reading their obsolete home
 * blocks wastes I/O and makes a cached read depend on an unrelated device error.
 * The owning adapter serializes operations, so these sets remain stable here. */
enum ext4_result
ext4_journal_read_current(
    const struct ext4_journal *journal, uint64_t offset, void *buffer, size_t length)
{
	struct ext4_fs *fs = journal->fs;
	uint32_t block_size = fs->info.block_size;
	uint8_t *output = buffer;
	const uint8_t *snapshot;
	uint64_t block;
	uint64_t blocks;
	uint64_t prefix;
	size_t within;
	size_t chunk;
	enum ext4_result error;

	while (length != 0) {
		block = offset / block_size;
		within = (size_t)(offset % block_size);
		snapshot = ext4_journal_current(journal, block);
		if (snapshot != NULL) {
			chunk = block_size - within;
			if (chunk > length) {
				chunk = length;
			}
			ext4_copy(output, snapshot + within, chunk);
		} else {
			/* The caller has checked offset + length; avoid rounding that
			 * sum up, which could overflow at the end of a large device. */
			blocks = (offset + length - 1U) / block_size - block + 1U;
			prefix = ext4_journal_home_prefix(journal, block, blocks);
			chunk = prefix == blocks ? length : (size_t)(prefix * block_size - within);
			error =
			    fs->environment.read(fs->environment.context, offset, output, chunk);
			if (error != EXT4_OK) {
				return error;
			}
		}
		offset += chunk;
		output += chunk;
		length -= chunk;
	}
	return EXT4_OK;
}

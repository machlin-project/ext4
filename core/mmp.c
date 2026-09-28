/* SPDX-License-Identifier: BSD-3-Clause */
#include "journal.h"

/* Multi-mount protection follows the Linux and e2fsprogs protocol. A new owner
 * waits for any active owner's check interval, publishes a random sequence, waits
 * again and requires that sequence to survive. An active owner refreshes it at
 * least once per update interval; offline recovery publishes the checker value.
 * See the ext4 superblock and MMP documentation in the Linux kernel sources. */

static enum ext4_result
ext4_mmp_read(struct ext4_fs *fs, uint8_t *block)
{
	struct ext4_mmp_disk *mmp = (struct ext4_mmp_disk *)block;
	enum ext4_result error;

	error =
	    ext4_device_read(fs, fs->mmp_block * fs->info.block_size, block, fs->info.block_size);
	if (error != EXT4_OK) {
		return error;
	}
	if (ext4_le32(&mmp->magic) != EXT4_MMP_MAGIC ||
	    (fs->metadata_checksum &&
		ext4_crc32c(fs->checksum_seed, mmp, offsetof(struct ext4_mmp_disk, checksum)) !=
		    ext4_le32(&mmp->checksum))) {
		return EXT4_CORRUPT;
	}
	return EXT4_OK;
}

static enum ext4_result
ext4_mmp_write(struct ext4_fs *fs, uint8_t *block, uint32_t sequence)
{
	struct ext4_mmp_disk *mmp = (struct ext4_mmp_disk *)block;
	const struct ext4_mmp_environment *environment = &fs->mmp_environment;
	int64_t time = environment->now(environment->context);
	uint32_t interval = fs->mmp_interval * EXT4_MMP_CHECK_MULTIPLIER;
	enum ext4_result error;

	if (interval > EXT4_MMP_MAX_CHECK_INTERVAL) {
		interval = EXT4_MMP_MAX_CHECK_INTERVAL;
	}
	ext4_encode32(&mmp->sequence, sequence);
	ext4_encode32(&mmp->time_lo, (uint32_t)(uint64_t)time);
	ext4_encode32(&mmp->time_hi, (uint32_t)((uint64_t)time >> 32));
	ext4_copy(mmp->node_name, fs->mmp_node_name, sizeof(mmp->node_name));
	ext4_encode16(&mmp->check_interval, (uint16_t)interval);
	if (fs->metadata_checksum) {
		ext4_encode32(&mmp->checksum,
		    ext4_crc32c(fs->checksum_seed, mmp, offsetof(struct ext4_mmp_disk, checksum)));
	}
	error = fs->mmp_writer.write(fs->mmp_writer.context, fs->mmp_block * fs->info.block_size,
	    block, fs->info.block_size);
	if (error == EXT4_OK) {
		error = fs->mmp_writer.flush(fs->mmp_writer.context);
	}
	if (error == EXT4_OK) {
		fs->mmp_written = time;
	}
	return error;
}

static void
ext4_mmp_name(char *output, size_t capacity, const char *name)
{
	size_t index;

	ext4_zero(output, capacity);
	for (index = 0; name != NULL && index < capacity && name[index] != 0; index++) {
		output[index] = name[index];
	}
}

static bool
ext4_mmp_owned(const struct ext4_fs *fs, const uint8_t *block)
{
	const struct ext4_mmp_disk *mmp = (const struct ext4_mmp_disk *)block;

	return ext4_le32(&mmp->sequence) == fs->mmp_sequence &&
	    ext4_equal(mmp->node_name, fs->mmp_node_name, sizeof(mmp->node_name));
}

static uint8_t *
ext4_mmp_buffer(struct ext4_fs *fs)
{
	return fs->environment.allocate(fs->environment.context, fs->info.block_size);
}

static void
ext4_mmp_free(struct ext4_fs *fs, uint8_t *block)
{
	fs->environment.release(fs->environment.context, block, fs->info.block_size);
}

enum ext4_result
ext4_mmp_start(struct ext4_fs *fs, const struct ext4_write_environment *writer, bool checker)
{
	const struct ext4_mmp_environment *environment = writer->mmp;
	struct ext4_mmp_disk *mmp;
	uint8_t *block;
	uint32_t interval;
	uint32_t wait;
	uint32_t sequence;
	enum ext4_result error;

	if (!(fs->info.feature_incompat & EXT4_FEATURE_INCOMPAT_MMP)) {
		return EXT4_OK;
	}
	if (environment == NULL || environment->sleep == NULL || environment->random == NULL ||
	    environment->now == NULL) {
		return EXT4_UNSUPPORTED;
	}
	fs->mmp_environment = *environment;
	fs->mmp_writer = *writer;
	ext4_mmp_name((char *)fs->mmp_node_name, sizeof(fs->mmp_node_name), environment->node_name);
	block = ext4_mmp_buffer(fs);
	if (block == NULL) {
		return EXT4_NO_MEMORY;
	}
	mmp = (struct ext4_mmp_disk *)block;
	error = ext4_mmp_read(fs, block);
	if (error != EXT4_OK) {
		goto out;
	}
	interval = fs->mmp_interval * EXT4_MMP_CHECK_MULTIPLIER;
	if (ext4_le16(&mmp->check_interval) > interval) {
		interval = ext4_le16(&mmp->check_interval);
	}
	if (interval < EXT4_MMP_MIN_CHECK_INTERVAL) {
		interval = EXT4_MMP_MIN_CHECK_INTERVAL;
	}
	if (interval > EXT4_MMP_MAX_CHECK_INTERVAL) {
		interval = EXT4_MMP_MAX_CHECK_INTERVAL;
	}
	wait = 2U * interval + 1U;
	if (wait > interval + EXT4_MMP_WAIT_LIMIT) {
		wait = interval + EXT4_MMP_WAIT_LIMIT;
	}
	sequence = ext4_le32(&mmp->sequence);
	if (sequence == EXT4_MMP_SEQ_FSCK) {
		error = EXT4_BUSY;
		goto out;
	}
	if (sequence != EXT4_MMP_SEQ_CLEAN) {
		/* An owner that is still active changes its sequence within the wait. */
		error = environment->sleep(environment->context, wait);
		if (error == EXT4_OK) {
			error = ext4_mmp_read(fs, block);
		}
		if (error == EXT4_OK && ext4_le32(&mmp->sequence) != sequence) {
			error = EXT4_BUSY;
		}
		if (error != EXT4_OK) {
			goto out;
		}
	}
	sequence = environment->random(environment->context) % (EXT4_MMP_SEQ_MAX + 1U);
	ext4_mmp_name(mmp->device_name, sizeof(mmp->device_name), environment->device_name);
	error = ext4_mmp_write(fs, block, sequence);
	if (error == EXT4_OK) {
		error = environment->sleep(environment->context, wait);
	}
	if (error == EXT4_OK) {
		error = ext4_mmp_read(fs, block);
	}
	if (error == EXT4_OK && ext4_le32(&mmp->sequence) != sequence) {
		error = EXT4_BUSY;
	}
	if (error == EXT4_OK) {
		/* Like the Linux updater's first pass, restart the interval at once:
		 * the confirmed sequence is already one wait old. */
		sequence = checker		   ? EXT4_MMP_SEQ_FSCK
		    : sequence == EXT4_MMP_SEQ_MAX ? 1U
						   : sequence + 1U;
		error = ext4_mmp_write(fs, block, sequence);
	}
	if (error == EXT4_OK) {
		fs->mmp_sequence = sequence;
		fs->mmp_active = true;
	}
out:
	ext4_mmp_free(fs, block);
	return error;
}

/* Require the on-disk owner to be this instance, then publish the next value. */
static enum ext4_result
ext4_mmp_refresh(struct ext4_fs *fs)
{
	uint8_t *block;
	uint32_t sequence;
	enum ext4_result error;

	block = ext4_mmp_buffer(fs);
	if (block == NULL) {
		return EXT4_NO_MEMORY;
	}
	error = ext4_mmp_read(fs, block);
	if (error == EXT4_OK && !ext4_mmp_owned(fs, block)) {
		error = EXT4_BUSY;
	}
	if (error == EXT4_OK) {
		sequence = fs->mmp_sequence == EXT4_MMP_SEQ_MAX ? 1U : fs->mmp_sequence + 1U;
		error = ext4_mmp_write(fs, block, sequence);
		if (error == EXT4_OK) {
			fs->mmp_sequence = sequence;
		}
	}
	if (error == EXT4_BUSY || error == EXT4_IO) {
		/* Another host may own the device, or our own update is uncertain. */
		fs->aborted = true;
		if (fs->journal != NULL) {
			fs->journal->aborted = true;
		}
	}
	ext4_mmp_free(fs, block);
	return error;
}

enum ext4_result
ext4_mmp_guard(struct ext4_fs *fs)
{
	int64_t time;

	if (fs->mmp_released) {
		return EXT4_READ_ONLY;
	}
	if (!fs->mmp_active || fs->mmp_sequence == EXT4_MMP_SEQ_FSCK) {
		return EXT4_OK;
	}
	time = fs->mmp_environment.now(fs->mmp_environment.context);
	if (time >= fs->mmp_written && time - fs->mmp_written < (int64_t)fs->mmp_interval) {
		return EXT4_OK;
	}
	return ext4_mmp_refresh(fs);
}

enum ext4_result
ext4_mmp_stop(struct ext4_fs *fs)
{
	uint8_t *block;
	enum ext4_result error;

	if (!fs->mmp_active) {
		return EXT4_OK;
	}
	block = ext4_mmp_buffer(fs);
	if (block == NULL) {
		return EXT4_NO_MEMORY;
	}
	error = ext4_mmp_read(fs, block);
	/* Never mark another host's active sequence clean. */
	if (error == EXT4_OK && !ext4_mmp_owned(fs, block)) {
		error = EXT4_BUSY;
	}
	if (error == EXT4_OK) {
		error = ext4_mmp_write(fs, block, EXT4_MMP_SEQ_CLEAN);
	}
	fs->mmp_active = false;
	fs->mmp_released = true;
	ext4_mmp_free(fs, block);
	return error;
}

enum ext4_result
ext4_mmp_update(struct ext4_fs *fs)
{
	if (fs == NULL) {
		return EXT4_INVALID_ARGUMENT;
	}
	if (fs->mmp_released) {
		return EXT4_READ_ONLY;
	}
	if (!fs->mmp_active) {
		return EXT4_OK;
	}
	if (fs->aborted) {
		return EXT4_RECOVERY_REQUIRED;
	}
	return ext4_mmp_refresh(fs);
}

enum ext4_result
ext4_mmp_release(struct ext4_fs *fs)
{
	if (fs == NULL) {
		return EXT4_INVALID_ARGUMENT;
	}
	if (fs->journal != NULL && (fs->journal->transaction_active || fs->journal->start != 0)) {
		return EXT4_INVALID_ARGUMENT;
	}
	return ext4_mmp_stop(fs);
}

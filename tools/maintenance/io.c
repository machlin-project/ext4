/* SPDX-License-Identifier: BSD-3-Clause */
#include "../../adapters/fskit/Ext4CheckProtocol.h"
#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <ext2fs/ext2fs.h>

#define CHECK_INITIAL_BLOCK_SIZE 1024

struct check_channel {
	uint64_t bytes;
	int writable;
	struct struct_io_stats stats;
};

static pthread_mutex_t check_lock = PTHREAD_MUTEX_INITIALIZER;
static int check_failure;
static int check_resource_error;

extern io_manager ext4_maintenance_io_manager;

static errcode_t
check_exchange(enum ext4_check_operation operation, uint64_t offset, uint32_t length,
    uint32_t flags, void *data, struct ext4_check_message *result)
{
	struct ext4_check_message message = { 0 };
	int64_t deadline;
	uint32_t error;
	int failure;

	pthread_mutex_lock(&check_lock);
	deadline = ext4_check_deadline(EXT4_CHECK_REQUEST_SECONDS);
	failure = check_failure;
	message.magic = ext4_check_encode32(EXT4_CHECK_MAGIC);
	message.version = ext4_check_encode32(EXT4_CHECK_VERSION);
	message.operation = ext4_check_encode32(operation);
	message.offset = ext4_check_encode64(offset);
	message.length = ext4_check_encode32(length);
	message.flags = ext4_check_encode32(flags);
	if (failure == 0) {
		failure = ext4_check_send(STDIN_FILENO, &message, sizeof(message), deadline);
	}
	if (failure == 0 && operation == EXT4_CHECK_WRITE) {
		failure = ext4_check_send(STDIN_FILENO, data, length, deadline);
	}
	if (failure == 0) {
		failure = ext4_check_receive(STDIN_FILENO, &message, sizeof(message), deadline);
	}
	if (failure == 0 &&
	    (ext4_check_decode32(message.magic) != EXT4_CHECK_MAGIC ||
		ext4_check_decode32(message.version) != EXT4_CHECK_VERSION ||
		ext4_check_decode32(message.operation) != (uint32_t)operation ||
		ext4_check_decode32(message.error) > INT_MAX ||
		(ext4_check_decode32(message.flags) & ~EXT4_CHECK_WRITABLE) != 0)) {
		failure = EPROTO;
	}
	error = failure == 0 ? ext4_check_decode32(message.error) : 0;
	if (failure == 0 && error == 0 && operation != EXT4_CHECK_OPEN &&
	    (ext4_check_decode32(message.length) != length ||
		ext4_check_decode64(message.offset) != offset ||
		ext4_check_decode32(message.flags) != 0)) {
		failure = EPROTO;
	}
	if (failure == 0 && error != 0 && ext4_check_decode32(message.length) != 0) {
		failure = EPROTO;
	}
	if (failure == 0 && error == 0 && operation == EXT4_CHECK_READ) {
		failure = ext4_check_receive(STDIN_FILENO, data, length, deadline);
	}
	if (failure != 0) {
		/* After a truncated frame the connection has no recoverable boundary. */
		check_failure = failure;
	} else if (result != NULL) {
		*result = message;
	}
	if (error != 0) {
		check_resource_error = (int)error;
	}
	pthread_mutex_unlock(&check_lock);
	return failure != 0 ? failure : error;
}

int
ext4_maintenance_io_error(void)
{
	return check_failure != 0 ? check_failure : check_resource_error;
}

static errcode_t
check_open(const char *name, int flags, io_channel *channel)
{
	struct ext4_check_message response;
	struct check_channel *state;
	io_channel opened;
	errcode_t error;
	uint32_t sector;

	*channel = NULL;
	if (strcmp(name, EXT4_CHECK_RESOURCE_NAME) != 0) {
		return ENOENT;
	}
	opened = calloc(1, sizeof(*opened));
	state = calloc(1, sizeof(*state));
	if (opened == NULL || state == NULL) {
		free(opened);
		free(state);
		return ENOMEM;
	}
	opened->name = strdup(name);
	if (opened->name == NULL) {
		free(state);
		free(opened);
		return ENOMEM;
	}
	error = check_exchange(EXT4_CHECK_OPEN, 0, 0,
	    (flags & IO_FLAG_RW) != 0 ? EXT4_CHECK_WRITABLE : 0, NULL, &response);
	sector = error == 0 ? ext4_check_decode32(response.length) : 0;
	if (error == 0 &&
	    (sector == 0 || (sector & (sector - 1)) != 0 ||
		ext4_check_decode64(response.offset) == 0 ||
		((flags & IO_FLAG_RW) != 0 &&
		    (ext4_check_decode32(response.flags) & EXT4_CHECK_WRITABLE) == 0))) {
		error = EPROTO;
	}
	if (error != 0) {
		free(opened->name);
		free(opened);
		free(state);
		return error;
	}
	state->bytes = ext4_check_decode64(response.offset);
	state->writable = (flags & IO_FLAG_RW) != 0;
	state->stats.num_fields = 2;
	opened->magic = EXT2_ET_MAGIC_IO_CHANNEL;
	opened->manager = ext4_maintenance_io_manager;
	opened->block_size = CHECK_INITIAL_BLOCK_SIZE;
	opened->refcount = 1;
	opened->flags = CHANNEL_FLAGS_BLOCK_DEVICE | CHANNEL_FLAGS_THREADS |
	    CHANNEL_FLAGS_NODISCARD | CHANNEL_FLAGS_NOZEROOUT;
	opened->private_data = state;
	*channel = opened;
	return 0;
}

static errcode_t
check_close(io_channel channel)
{
	errcode_t error;

	if (--channel->refcount > 0) {
		return 0;
	}
	error = check_exchange(EXT4_CHECK_CLOSE, 0, 0, 0, NULL, NULL);
	free(channel->name);
	free(channel->private_data);
	free(channel);
	return error;
}

static errcode_t
check_blksize(io_channel channel, int size)
{
	if (size <= 0 || (size & (size - 1)) != 0) {
		return EINVAL;
	}
	channel->block_size = size;
	return 0;
}

static errcode_t
check_bytes(io_channel channel, uint64_t offset, size_t length, void *data, int writing)
{
	struct check_channel *state = channel->private_data;
	uint8_t *cursor = data;
	size_t completed = 0;
	uint32_t amount;
	errcode_t error;

	if (writing && !state->writable) {
		return EROFS;
	}
	if (offset > state->bytes || length > state->bytes - offset) {
		return EIO;
	}
	while (completed < length) {
		amount = length - completed > EXT4_CHECK_MAX_TRANSFER
		    ? EXT4_CHECK_MAX_TRANSFER
		    : (uint32_t)(length - completed);
		error = check_exchange(writing ? EXT4_CHECK_WRITE : EXT4_CHECK_READ,
		    offset + completed, amount, 0, cursor + completed, NULL);
		if (error != 0) {
			return error;
		}
		completed += amount;
	}
	pthread_mutex_lock(&check_lock);
	if (writing) {
		state->stats.bytes_written += completed;
	} else {
		state->stats.bytes_read += completed;
	}
	pthread_mutex_unlock(&check_lock);
	return 0;
}

static errcode_t
check_blocks(io_channel channel, uint64_t block, int count, void *bytes, int writing)
{
	uint64_t offset;
	uint64_t length;

	if (block > UINT64_MAX / (unsigned)channel->block_size) {
		return EOVERFLOW;
	}
	offset = block * (unsigned)channel->block_size;
	length = count < 0 ? (uint64_t)-(int64_t)count
			   : (uint64_t)(unsigned)count * (unsigned)channel->block_size;
	if (length > SIZE_MAX) {
		return EOVERFLOW;
	}
	return check_bytes(channel, offset, (size_t)length, bytes, writing);
}

static errcode_t
check_read64(io_channel channel, unsigned long long block, int count, void *bytes)
{
	return check_blocks(channel, block, count, bytes, 0);
}

static errcode_t
check_write64(io_channel channel, unsigned long long block, int count, const void *bytes)
{
	return check_blocks(channel, block, count, (void *)bytes, 1);
}

static errcode_t
check_read(io_channel channel, unsigned long block, int count, void *bytes)
{
	return check_read64(channel, block, count, bytes);
}

static errcode_t
check_write(io_channel channel, unsigned long block, int count, const void *bytes)
{
	return check_write64(channel, block, count, bytes);
}

static errcode_t
check_write_bytes(io_channel channel, unsigned long offset, int count, const void *bytes)
{
	if (count < 0) {
		return EINVAL;
	}
	return check_bytes(channel, offset, (size_t)count, (void *)bytes, 1);
}

static errcode_t
check_flush(io_channel channel)
{
	struct check_channel *state = channel->private_data;

	return state->writable ? check_exchange(EXT4_CHECK_FLUSH, 0, 0, 0, NULL, NULL) : 0;
}

static errcode_t
check_stats(io_channel channel, io_stats *stats)
{
	struct check_channel *state = channel->private_data;

	*stats = &state->stats;
	return 0;
}

/* e2fsck's device-size query normally opens a native path outside its I/O
 * manager. Route that query through the same single authorized resource. */
errcode_t
ext4_maintenance_device_size(const char *name, int blockSize, blk64_t *blocks)
{
	struct check_channel *state;
	io_channel channel;
	errcode_t error;
	errcode_t closed;

	if (blockSize <= 0 || blocks == NULL) {
		return EINVAL;
	}
	error = check_open(name, 0, &channel);
	if (error != 0) {
		return error;
	}
	state = channel->private_data;
	*blocks = state->bytes / (unsigned)blockSize;
	closed = check_close(channel);
	return closed;
}

static struct struct_io_manager check_manager = {
	.magic = EXT2_ET_MAGIC_IO_MANAGER,
	.name = "FSKit resource",
	.open = check_open,
	.close = check_close,
	.set_blksize = check_blksize,
	.read_blk = check_read,
	.write_blk = check_write,
	.flush = check_flush,
	.write_byte = check_write_bytes,
	.get_stats = check_stats,
	.read_blk64 = check_read64,
	.write_blk64 = check_write64,
};

io_manager ext4_maintenance_io_manager = &check_manager;

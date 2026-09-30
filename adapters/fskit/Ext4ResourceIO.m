/* SPDX-License-Identifier: BSD-3-Clause */
#import "Ext4ResourceIO.h"
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/param.h>

@interface Ext4ResourceIO () {
	id<Ext4BlockReader> _reader;
	id<Ext4PersistenceBarrier> _barrier;
	NSString *_deviceName;
	char _nodeName[MAXHOSTNAMELEN];
	struct ext4_mmp_environment _mmp;
	uint64_t _size;
	size_t _alignment;
}

@end

static enum ext4_result
ext4_resource_write(void *context, uint64_t offset, const void *buffer, size_t length)
{
	return [(__bridge Ext4ResourceIO *)context writeAt:offset buffer:buffer length:length];
}

static enum ext4_result
ext4_resource_flush(void *context)
{
	return [(__bridge Ext4ResourceIO *)context synchronize];
}

static int64_t
ext4_resource_time(void *context)
{
	(void)context;
	return time(NULL);
}

static uint32_t
ext4_resource_random(void *context)
{
	(void)context;
	return arc4random();
}

static enum ext4_result
ext4_resource_sleep(void *context, uint32_t seconds)
{
	struct timespec interval = { .tv_sec = seconds };

	(void)context;
	return nanosleep(&interval, NULL) == 0 ? EXT4_OK : EXT4_IO;
}

static enum ext4_result
ext4_resource_read(void *context, uint64_t offset, void *buffer, size_t length)
{
	return [(__bridge Ext4ResourceIO *)context readAt:offset buffer:buffer length:length];
}

static void *
ext4_resource_allocate(void *context, size_t size)
{
	(void)context;
	return malloc(size);
}

static void
ext4_resource_release(void *context, void *allocation, size_t size)
{
	(void)context;
	(void)size;
	free(allocation);
}

@implementation Ext4ResourceIO

- (instancetype)initWithReader:(id<Ext4BlockReader>)reader
{
	uint64_t blockSize = reader.blockSize;
	uint64_t count = reader.blockCount;
	uint64_t alignment = reader.physicalBlockSize;

	if (blockSize == 0 || count == 0 || count > INT64_MAX / blockSize || alignment == 0 ||
	    alignment > SIZE_MAX || count * blockSize % alignment != 0) {
		return nil;
	}
	self = [super init];
	if (self != nil) {
		_reader = reader;
		_size = count * blockSize;
		_alignment = (size_t)alignment;
	}
	return self;
}

- (enum ext4_result)open:(struct ext4_fs **)filesystem
{
	struct ext4_environment environment = [self environment];

	return ext4_mount(&environment, filesystem);
}

- (enum ext4_result)inspect:(struct ext4_info *)info
{
	struct ext4_environment environment = [self environment];

	return ext4_inspect(&environment, info);
}

- (struct ext4_environment)environment
{
	struct ext4_environment environment = { 0 };

	environment.context = (__bridge void *)self;
	environment.size_bytes = _size;
	environment.read = ext4_resource_read;
	environment.allocate = ext4_resource_allocate;
	environment.release = ext4_resource_release;
	return environment;
}

- (void)enableWritesWithBarrier:(id<Ext4PersistenceBarrier>)barrier deviceName:(NSString *)name
{
	_barrier = barrier;
	_deviceName = [name copy];
	if (gethostname(_nodeName, sizeof(_nodeName)) != 0) {
		strlcpy(_nodeName, "macOS", sizeof(_nodeName));
	}
	_nodeName[sizeof(_nodeName) - 1] = 0;
	_mmp.context = (__bridge void *)self;
	_mmp.sleep = ext4_resource_sleep;
	_mmp.random = ext4_resource_random;
	_mmp.now = ext4_resource_time;
	_mmp.node_name = _nodeName;
	_mmp.device_name = _deviceName.UTF8String;
}

- (struct ext4_write_environment)writer
{
	struct ext4_write_environment writer = { 0 };

	writer.context = (__bridge void *)self;
	writer.write = ext4_resource_write;
	writer.flush = ext4_resource_flush;
	writer.mmp = &_mmp;
	return writer;
}

- (enum ext4_result)openWritable:(struct ext4_fs **)filesystem
{
	struct ext4_environment environment = [self environment];
	struct ext4_write_environment writer = [self writer];
	enum ext4_result error;

	*filesystem = NULL;
	error = [self synchronize];
	return error == EXT4_OK ? ext4_mount_writable(&environment, &writer, filesystem) : error;
}

- (enum ext4_result)recover:(struct ext4_recovery_report *)report
{
	struct ext4_environment environment = [self environment];
	struct ext4_write_environment writer = [self writer];
	enum ext4_result error = [self synchronize];

	return error == EXT4_OK ? ext4_recover(&environment, &writer, report) : error;
}

- (enum ext4_result)synchronize
{
	NSError *error = nil;

	if (_barrier == nil) {
		return EXT4_READ_ONLY;
	}
	return [_barrier synchronizeWithError:&error] && error == nil ? EXT4_OK : EXT4_IO;
}

- (enum ext4_result)writeAt:(uint64_t)offset buffer:(const void *)buffer length:(size_t)length
{
	NSError *error = nil;
	uint8_t *bounce;
	uint64_t start;
	size_t prefix;
	size_t total;
	size_t completed;
	id<Ext4BlockWriter> writer = (id<Ext4BlockWriter>)_reader;

	if (_barrier == nil) {
		return EXT4_READ_ONLY;
	}
	if (offset > _size || length > _size - offset || (buffer == NULL && length != 0)) {
		return EXT4_IO;
	}
	if (length == 0) {
		return EXT4_OK;
	}
	if (offset % _alignment == 0 && length % _alignment == 0) {
		completed = [writer writeFrom:(void *)buffer
				   startingAt:(off_t)offset
				       length:length
					error:&error];
		return error == nil && completed == length ? EXT4_OK : EXT4_IO;
	}
	start = offset - offset % _alignment;
	prefix = (size_t)(offset - start);
	if (length > SIZE_MAX - prefix || length + prefix > SIZE_MAX - (_alignment - 1)) {
		return EXT4_RANGE;
	}
	total = ((length + prefix + _alignment - 1) / _alignment) * _alignment;
	if (total > _size - start) {
		return EXT4_IO;
	}
	bounce = malloc(total);
	if (bounce == NULL) {
		return EXT4_NO_MEMORY;
	}
	completed = [_reader readInto:bounce startingAt:(off_t)start length:total error:&error];
	if (error == nil && completed == total) {
		memcpy(bounce + prefix, buffer, length);
		completed = [writer writeFrom:bounce
				   startingAt:(off_t)start
				       length:total
					error:&error];
	}
	free(bounce);
	return error == nil && completed == total ? EXT4_OK : EXT4_IO;
}

- (enum ext4_result)readAt:(uint64_t)offset buffer:(void *)buffer length:(size_t)length
{
	NSError *error = nil;
	uint8_t *bounce;
	uint64_t start;
	size_t prefix;
	size_t total;
	size_t completed;

	if (offset > _size || length > _size - offset || (buffer == NULL && length != 0)) {
		return EXT4_IO;
	}
	if (length == 0) {
		return EXT4_OK;
	}
	if (offset % _alignment == 0 && length % _alignment == 0) {
		completed = [_reader readInto:buffer
				   startingAt:(off_t)offset
				       length:length
					error:&error];
		return error == nil && completed == length ? EXT4_OK : EXT4_IO;
	}
	start = offset - offset % _alignment;
	prefix = (size_t)(offset - start);
	if (length > SIZE_MAX - prefix || length + prefix > SIZE_MAX - (_alignment - 1)) {
		return EXT4_RANGE;
	}
	total = ((length + prefix + _alignment - 1) / _alignment) * _alignment;
	if (total > _size - start) {
		return EXT4_IO;
	}
	bounce = malloc(total);
	if (bounce == NULL) {
		return EXT4_NO_MEMORY;
	}
	completed = [_reader readInto:bounce startingAt:(off_t)start length:total error:&error];
	if (error == nil && completed == total) {
		memcpy(buffer, bounce + prefix, length);
	}
	free(bounce);
	return error == nil && completed == total ? EXT4_OK : EXT4_IO;
}

@end

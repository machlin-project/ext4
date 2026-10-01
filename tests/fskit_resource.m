/* SPDX-License-Identifier: BSD-3-Clause */
#import "../adapters/fskit/Ext4ResourceIO.h"
#include <assert.h>
#include <errno.h>
#include <string.h>

@interface MemoryBlocks : NSObject <Ext4BlockWriter, Ext4PersistenceBarrier>
@property(nonatomic, getter=isRevoked) BOOL revoked;
@property uint64_t blockSize;
@property uint64_t blockCount;
@property uint64_t physicalBlockSize;
@property size_t calls;
@property void *lastBuffer;
@property BOOL shortRead;
@property BOOL revokeAfterRead;
@property BOOL fail;
@property BOOL shortWrite;
@property BOOL failWrite;
@property BOOL failBarrier;
@property size_t writes;
@property size_t barriers;
@property NSMutableData *bytes;
@end

@implementation MemoryBlocks

- (BOOL)synchronizeWithError:(NSError **)error
{
	self.barriers++;
	if (self.failBarrier) {
		*error = [NSError errorWithDomain:NSPOSIXErrorDomain code:EIO userInfo:nil];
	}
	return !self.failBarrier;
}

- (size_t)writeFrom:(void *)buffer
	 startingAt:(off_t)offset
	     length:(size_t)length
	      error:(NSError **)error
{
	self.writes++;
	assert(offset >= 0 && (uint64_t)offset % self.physicalBlockSize == 0);
	assert(length % self.physicalBlockSize == 0);
	assert((uint64_t)offset + length <= self.bytes.length);
	if (self.failWrite) {
		*error = [NSError errorWithDomain:NSPOSIXErrorDomain code:EIO userInfo:nil];
		return 0;
	}
	memcpy((uint8_t *)self.bytes.mutableBytes + offset, buffer, length);
	return self.shortWrite ? length - 1 : length;
}

- (size_t)readInto:(void *)buffer
	startingAt:(off_t)offset
	    length:(size_t)length
	     error:(NSError **)error
{
	self.calls++;
	self.lastBuffer = buffer;
	assert(offset >= 0 && (uint64_t)offset % self.physicalBlockSize == 0);
	assert(length % self.physicalBlockSize == 0);
	assert((uint64_t)offset + length <= self.bytes.length);
	memcpy(buffer, (const uint8_t *)self.bytes.bytes + offset, length);
	if (self.revokeAfterRead) {
		self.revoked = YES;
	}
	if (self.fail) {
		*error = [NSError errorWithDomain:NSPOSIXErrorDomain code:EIO userInfo:nil];
	}
	return self.shortRead ? length - 1 : length;
}

@end

int
main(void)
{
	@autoreleasepool {
		MemoryBlocks *blocks = [MemoryBlocks new];
		Ext4ResourceIO *io;
		NSData *snapshot;
		uint8_t output[2048];
		uint8_t *source;
		size_t index;
		size_t calls;
		size_t writes;
		size_t barriers;

		blocks.blockSize = 512;
		blocks.physicalBlockSize = 512;
		blocks.blockCount = 8;
		blocks.bytes = [NSMutableData dataWithLength:4096];
		source = blocks.bytes.mutableBytes;
		for (index = 0; index < blocks.bytes.length; index++) {
			source[index] = (uint8_t)(index * 17);
		}
		io = [[Ext4ResourceIO alloc] initWithReader:blocks];
		assert(io != nil);
		assert([io readAt:512 buffer:output length:1024] == EXT4_OK);
		assert(blocks.lastBuffer == output && blocks.calls == 1);
		assert(memcmp(output, source + 512, 1024) == 0);
		memset(output, 0xa5, sizeof(output));
		assert([io readAt:511 buffer:output + 1 length:1025] == EXT4_OK);
		assert(blocks.lastBuffer != output + 1 && blocks.calls == 2);
		assert(output[0] == 0xa5 && output[1026] == 0xa5);
		assert(memcmp(output + 1, source + 511, 1025) == 0);
		assert([io readAt:4095 buffer:output length:1] == EXT4_OK);
		assert(output[0] == source[4095]);
		calls = blocks.calls;
		assert([io readAt:4096 buffer:NULL length:0] == EXT4_OK);
		assert([io readAt:4096 buffer:output length:1] == EXT4_IO);
		assert([io readAt:UINT64_MAX buffer:output length:1] == EXT4_IO);
		assert([io readAt:1 buffer:output length:SIZE_MAX] == EXT4_IO);
		assert([io readAt:0 buffer:NULL length:1] == EXT4_IO);
		assert(blocks.calls == calls);
		blocks.shortRead = YES;
		assert([io readAt:0 buffer:output length:512] == EXT4_IO);
		memset(output, 0xa5, sizeof(output));
		assert([io readAt:1 buffer:output length:1] == EXT4_IO && output[0] == 0xa5);
		blocks.shortRead = NO;
		blocks.fail = YES;
		assert([io readAt:0 buffer:output length:512] == EXT4_IO);
		memset(output, 0xa5, sizeof(output));
		assert([io readAt:1 buffer:output length:1] == EXT4_IO && output[0] == 0xa5);
		blocks.fail = NO;
		assert([io writeAt:0 buffer:output length:512] == EXT4_READ_ONLY);
		assert([io synchronize] == EXT4_READ_ONLY && blocks.writes == 0);
		[io enableWritesWithBarrier:blocks deviceName:@"memory"];
		memset(output, 0x5a, sizeof(output));
		assert([io writeAt:1024 buffer:output length:512] == EXT4_OK);
		assert(memcmp(source + 1024, output, 512) == 0);
		assert(
		    source[1023] == (uint8_t)(1023 * 17) && source[1536] == (uint8_t)(1536 * 17));
		assert([io writeAt:511 buffer:output length:514] == EXT4_OK);
		assert(
		    memcmp(source + 511, output, 514) == 0 && source[510] == (uint8_t)(510 * 17));
		assert([io writeAt:4095 buffer:output length:1] == EXT4_OK && source[4095] == 0x5a);
		calls = blocks.writes;
		assert([io writeAt:4096 buffer:NULL length:0] == EXT4_OK);
		assert([io writeAt:4096 buffer:output length:1] == EXT4_IO);
		assert([io writeAt:UINT64_MAX buffer:output length:1] == EXT4_IO);
		assert([io writeAt:1 buffer:output length:SIZE_MAX] == EXT4_IO);
		assert([io writeAt:0 buffer:NULL length:1] == EXT4_IO && blocks.writes == calls);
		blocks.fail = YES;
		assert([io writeAt:1 buffer:output length:1] == EXT4_IO && blocks.writes == calls);
		blocks.fail = NO;
		blocks.shortRead = YES;
		assert([io writeAt:1 buffer:output length:1] == EXT4_IO && blocks.writes == calls);
		blocks.shortRead = NO;
		blocks.shortWrite = YES;
		assert([io writeAt:0 buffer:output length:512] == EXT4_IO);
		blocks.shortWrite = NO;
		blocks.failWrite = YES;
		assert([io writeAt:1 buffer:output length:1] == EXT4_IO);
		blocks.failWrite = NO;
		assert([io synchronize] == EXT4_OK && blocks.barriers == 1);
		blocks.failBarrier = YES;
		assert([io synchronize] == EXT4_IO && blocks.barriers == 2);
		blocks.failBarrier = NO;
		snapshot = blocks.bytes.copy;
		calls = blocks.calls;
		writes = blocks.writes;
		barriers = blocks.barriers;
		blocks.revoked = YES;
		assert(io.isRevoked && [[Ext4ResourceIO alloc] initWithReader:blocks] == nil);
		memset(output, 0xa5, sizeof(output));
		assert([io readAt:0 buffer:output length:512] == EXT4_IO && output[0] == 0xa5);
		assert([io readAt:1 buffer:output length:1] == EXT4_IO && output[0] == 0xa5);
		assert([io readAt:0 buffer:NULL length:0] == EXT4_IO);
		assert([io writeAt:0 buffer:output length:512] == EXT4_IO);
		assert([io writeAt:1 buffer:output length:1] == EXT4_IO);
		assert([io writeAt:0 buffer:NULL length:0] == EXT4_IO);
		assert([io synchronize] == EXT4_IO);
		assert(blocks.calls == calls && blocks.writes == writes &&
		    blocks.barriers == barriers);
		assert([blocks.bytes isEqualToData:snapshot]);
		/* Exercise revocation between the read and write of an unaligned RMW.
		 * Only the fake reader can reset this irreversible resource state. */
		blocks.revoked = NO;
		blocks.revokeAfterRead = YES;
		assert([io writeAt:1 buffer:output length:1] == EXT4_IO);
		assert(io.isRevoked && blocks.calls == calls + 1 && blocks.writes == writes);
		assert([blocks.bytes isEqualToData:snapshot]);
		blocks.revoked = NO;
		blocks.revokeAfterRead = NO;
		blocks.physicalBlockSize = 0;
		assert([[Ext4ResourceIO alloc] initWithReader:blocks] == nil);
		blocks.physicalBlockSize = 511;
		assert([[Ext4ResourceIO alloc] initWithReader:blocks] == nil);
		blocks.physicalBlockSize = 512;
		blocks.blockCount = UINT64_MAX;
		assert([[Ext4ResourceIO alloc] initWithReader:blocks] == nil);
		blocks.blockCount = 8;
		blocks.blockSize = 0;
		assert([[Ext4ResourceIO alloc] initWithReader:blocks] == nil);
		puts("PASS FSKit resource: aligned direct I/O, unaligned bounds, short reads and "
		     "failures; write guards, read-only admission, persistence failures and "
		     "revocation");
	}
	return 0;
}

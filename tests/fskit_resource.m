/* SPDX-License-Identifier: BSD-3-Clause */
#import "../adapters/fskit/Ext4ResourceIO.h"
#include <assert.h>
#include <errno.h>
#include <string.h>

@interface MemoryBlocks : NSObject <Ext4BlockReader>
@property uint64_t blockSize;
@property uint64_t blockCount;
@property uint64_t physicalBlockSize;
@property size_t calls;
@property void *lastBuffer;
@property BOOL shortRead;
@property BOOL fail;
@property NSMutableData *bytes;
@end

@implementation MemoryBlocks

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
		uint8_t output[2048];
		uint8_t *source;
		size_t index;
		size_t calls;

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
		     "failures");
	}
	return 0;
}

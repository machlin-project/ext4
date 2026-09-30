/* SPDX-License-Identifier: BSD-3-Clause */
#import <FSKit/FSKit.h>
#include <ext4/ext4.h>

/* The resource owner keeps this object alive through ext4_unmount. Tests use the
 * same interface with a bounded memory device; no FSKit proxy is manufactured. */
@protocol Ext4BlockReader <NSObject>
@property(readonly) uint64_t blockSize;
@property(readonly) uint64_t blockCount;
@property(readonly) uint64_t physicalBlockSize;
- (size_t)readInto:(void *)buffer
	startingAt:(off_t)offset
	    length:(size_t)length
	     error:(NSError **)error;
@end

@interface Ext4ResourceIO : NSObject
- (instancetype)initWithReader:(id<Ext4BlockReader>)reader;
- (enum ext4_result)open:(struct ext4_fs **)filesystem;
- (enum ext4_result)readAt:(uint64_t)offset buffer:(void *)buffer length:(size_t)length;
@end

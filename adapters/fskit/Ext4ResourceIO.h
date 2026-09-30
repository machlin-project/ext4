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

@protocol Ext4BlockWriter <Ext4BlockReader>
- (size_t)writeFrom:(void *)buffer
	 startingAt:(off_t)offset
	     length:(size_t)length
	      error:(NSError **)error;
@end

/* A device persistence barrier is a stronger contract than metadataFlush. */
@protocol Ext4PersistenceBarrier <NSObject>
- (BOOL)synchronizeWithError:(NSError **)error;
@end

@interface Ext4ResourceIO : NSObject
- (instancetype)initWithReader:(id<Ext4BlockReader>)reader;
- (void)enableWritesWithBarrier:(id<Ext4PersistenceBarrier>)barrier deviceName:(NSString *)name;
- (enum ext4_result)open:(struct ext4_fs **)filesystem;
- (enum ext4_result)openWritable:(struct ext4_fs **)filesystem;
- (enum ext4_result)recover:(struct ext4_recovery_report *)report;
- (enum ext4_result)readAt:(uint64_t)offset buffer:(void *)buffer length:(size_t)length;
- (enum ext4_result)writeAt:(uint64_t)offset buffer:(const void *)buffer length:(size_t)length;
- (enum ext4_result)synchronize;
@end

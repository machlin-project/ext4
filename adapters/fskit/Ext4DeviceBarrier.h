/* SPDX-License-Identifier: BSD-3-Clause */
#import <Foundation/Foundation.h>

#define EXT4_BARRIER_SERVICE @"group.org.machlin.ext4.device-barrier"
#define EXT4_BARRIER_IDENTIFIER @"org.machlin.ext4.device-barrier"
#define EXT4_EXTENSION_IDENTIFIER @"org.machlin.ext4.filesystem"
#define EXT4_APP_IDENTIFIER @"org.machlin.ext4"

/* No raw data, file descriptors or general-purpose ioctls cross this interface. */
@protocol Ext4DeviceBarrierProtocol
- (void)checkServiceWithReply:(void (^)(NSError *))reply;
- (void)openDevice:(NSString *)name
	 blockSize:(uint64_t)blockSize
	blockCount:(uint64_t)blockCount
	     reply:(void (^)(NSError *))reply;
- (void)synchronizeWithReply:(void (^)(NSError *))reply;
@end

NSString *ext4_peer_requirement(NSString *identifier);

@interface Ext4DeviceBarrier : NSObject
+ (BOOL)isServiceAvailable;
- (instancetype)initWithDevice:(NSString *)name
		     blockSize:(uint64_t)blockSize
		    blockCount:(uint64_t)blockCount
			 error:(NSError **)error;
- (BOOL)synchronizeWithError:(NSError **)error;
@end

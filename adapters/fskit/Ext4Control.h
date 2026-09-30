/* SPDX-License-Identifier: BSD-3-Clause */
#import <Foundation/Foundation.h>

NS_ASSUME_NONNULL_BEGIN

FOUNDATION_EXPORT NSString *const Ext4AppGroup;
FOUNDATION_EXPORT NSDictionary *ext4_control_error(int code, NSString *message);

/* One endpoint per activated instance, independent of duplicate volume UUIDs.
 * Stop before teardown. The callback enters the same owner as filesystem I/O. */
@interface Ext4ControlServer : NSObject
- (nullable instancetype)initWithDirectory:(NSURL *)directory
				  volumeID:(NSString *)volumeID
				   handler:(NSDictionary * (^)(NSDictionary *))handler
				     error:(NSError *_Nullable *_Nullable)error;
@property(readonly) NSURL *manifestURL;
- (void)stop;
@end

@interface Ext4ControlClient : NSObject
+ (NSArray<NSURL *> *)endpointsInDirectory:(NSURL *)directory
    NS_SWIFT_NAME(endpoints(inDirectory:));
+ (nullable NSDictionary *)request:(NSDictionary *)request
			  endpoint:(NSURL *)manifestURL
			     error:(NSError *_Nullable *_Nullable)error;
@end

NS_ASSUME_NONNULL_END

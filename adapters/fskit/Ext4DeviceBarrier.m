/* SPDX-License-Identifier: BSD-3-Clause */
#import "Ext4DeviceBarrier.h"
#include <errno.h>

@interface Ext4DeviceBarrier () {
	NSXPCConnection *_connection;
}

@end

@implementation Ext4DeviceBarrier

- (BOOL)perform:(void (^)(id<Ext4DeviceBarrierProtocol>, void (^)(NSError *)))operation
	  error:(NSError **)error
{
	dispatch_semaphore_t completed = dispatch_semaphore_create(0);
	NSObject *gate = [[NSObject alloc] init];
	__block NSError *failure = nil;
	__block BOOL finished = NO;
	id<Ext4DeviceBarrierProtocol> proxy;
	void (^finish)(NSError *);

	/* A lost or wedged service must fail the journal barrier, never acknowledge it. */
	finish = ^(NSError *value) {
	  @synchronized(gate) {
		  if (!finished) {
			  finished = YES;
			  failure = value;
			  dispatch_semaphore_signal(completed);
		  }
	  }
	};
	proxy = [_connection remoteObjectProxyWithErrorHandler:finish];
	operation(proxy, finish);
	if (dispatch_semaphore_wait(
		completed, dispatch_time(DISPATCH_TIME_NOW, 10 * NSEC_PER_SEC)) != 0) {
		@synchronized(gate) {
			finished = YES;
		}
		[_connection invalidate];
		if (error != NULL) {
			*error = [NSError errorWithDomain:NSPOSIXErrorDomain
						     code:ETIMEDOUT
						 userInfo:nil];
		}
		return NO;
	}
	@synchronized(gate) {
		if (error != NULL) {
			*error = failure;
		}
		return failure == nil;
	}
}

- (instancetype)initWithError:(NSError **)error
{
	NSString *requirement = ext4_peer_requirement(EXT4_BARRIER_IDENTIFIER);

	if (requirement == nil) {
		if (error != NULL) {
			*error = [NSError errorWithDomain:NSPOSIXErrorDomain
						     code:EACCES
						 userInfo:nil];
		}
		return nil;
	}
	self = [super init];
	if (self != nil) {
		_connection =
		    [[NSXPCConnection alloc] initWithMachServiceName:EXT4_BARRIER_SERVICE
							     options:NSXPCConnectionPrivileged];
		_connection.remoteObjectInterface =
		    [NSXPCInterface interfaceWithProtocol:@protocol(Ext4DeviceBarrierProtocol)];
		[_connection setCodeSigningRequirement:requirement];
		[_connection activate];
	}
	return self;
}

+ (BOOL)isServiceAvailable
{
	Ext4DeviceBarrier *service = [[self alloc] initWithError:NULL];

	return service != nil &&
	    [service
		perform:^(id<Ext4DeviceBarrierProtocol> proxy, void (^finish)(NSError *)) {
		  [proxy checkServiceWithReply:finish];
		}
		  error:NULL];
}

- (instancetype)initWithDevice:(NSString *)name
		     blockSize:(uint64_t)blockSize
		    blockCount:(uint64_t)blockCount
			 error:(NSError **)error
{
	self = [self initWithError:error];
	if (self != nil) {
		if (![self
			perform:^(id<Ext4DeviceBarrierProtocol> proxy, void (^finish)(NSError *)) {
			  [proxy openDevice:name
				  blockSize:blockSize
				 blockCount:blockCount
				      reply:finish];
			}
			  error:error]) {
			return nil;
		}
	}
	return self;
}

- (BOOL)synchronizeWithError:(NSError **)error
{
	return [self
	    perform:^(id<Ext4DeviceBarrierProtocol> proxy, void (^finish)(NSError *)) {
	      [proxy synchronizeWithReply:finish];
	    }
	      error:error];
}

- (void)dealloc
{
	[_connection invalidate];
}

@end

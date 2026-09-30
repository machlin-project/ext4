/* SPDX-License-Identifier: BSD-3-Clause */
#import "../adapters/fskit/Ext4DeviceBarrier.h"
#include <assert.h>

/* Run as an unsigned ordinary-user tool in the disposable installed-test guest.
 * A preceding signed-app health query must prove the service is actually alive. */
int
main(void)
{
	@autoreleasepool {
		NSXPCConnection *connection =
		    [[NSXPCConnection alloc] initWithMachServiceName:EXT4_BARRIER_SERVICE
							     options:NSXPCConnectionPrivileged];
		dispatch_semaphore_t completed = dispatch_semaphore_create(0);
		__block BOOL rejected = NO;
		id<Ext4DeviceBarrierProtocol> proxy;

		connection.remoteObjectInterface =
		    [NSXPCInterface interfaceWithProtocol:@protocol(Ext4DeviceBarrierProtocol)];
		[connection activate];
		proxy = [connection remoteObjectProxyWithErrorHandler:^(NSError *error) {
		  rejected = error != nil;
		  dispatch_semaphore_signal(completed);
		}];
		[proxy checkServiceWithReply:^(NSError *error) {
		  (void)error;
		  dispatch_semaphore_signal(completed);
		}];
		assert(dispatch_semaphore_wait(
			   completed, dispatch_time(DISPATCH_TIME_NOW, 5 * NSEC_PER_SEC)) == 0);
		assert(rejected);
		[connection invalidate];
		puts("PASS unsigned client rejected by device-barrier service");
	}
	return 0;
}

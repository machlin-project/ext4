/* SPDX-License-Identifier: BSD-3-Clause */
#import <Foundation/Foundation.h>
#import "Ext4ResourceIO.h"

@class FSTask;

typedef NS_ENUM(NSUInteger, Ext4CheckMode) { Ext4CheckVerify, Ext4CheckRepair, Ext4CheckPreen };

/* One separate checker inherits the extension sandbox and only this resource
 * connection. Cancellation never releases ownership before the child is reaped. */
@interface Ext4CheckTask : NSObject
- (instancetype)initWithResource:(Ext4ResourceIO *)resource
			    mode:(Ext4CheckMode)mode
		      executable:(NSURL *)executable;
- (NSError *)runWithTask:(FSTask *)task;
- (void)cancel;
@end

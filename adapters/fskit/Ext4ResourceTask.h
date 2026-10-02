/* SPDX-License-Identifier: BSD-3-Clause */
#import <Foundation/Foundation.h>
#import "Ext4ResourceIO.h"

@class FSTask;

/* One resource-bound subprocess inherits the extension sandbox. Reaping always
 * precedes release, including cancellation and malformed or incomplete I/O. */
@interface Ext4ResourceTask : NSObject
- (instancetype)initWithResource:(Ext4ResourceIO *)resource
		      executable:(NSURL *)executable
		       arguments:(NSArray<NSString *> *)arguments
			writable:(BOOL)writable
	  successfulExitStatuses:(NSIndexSet *)statuses;
- (NSError *)runWithTask:(FSTask *)task;
- (void)cancel;
@end

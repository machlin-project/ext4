/* SPDX-License-Identifier: BSD-3-Clause */
#import "Ext4ResourceTask.h"

/* The module chooses a supported feature set. No arbitrary mke2fs arguments,
 * device names, configuration files or population paths cross this boundary. */
@interface Ext4FormatTask : Ext4ResourceTask
- (instancetype)initWithResource:(Ext4ResourceIO *)resource
		       blockSize:(NSUInteger)blockSize
			    name:(NSString *)name
			    uuid:(NSUUID *)uuid
		      executable:(NSURL *)executable;
@end

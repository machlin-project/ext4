/* SPDX-License-Identifier: BSD-3-Clause */
#import "Ext4CheckTask.h"

@implementation Ext4CheckTask

- (instancetype)initWithResource:(Ext4ResourceIO *)resource
			    mode:(Ext4CheckMode)mode
		      executable:(NSURL *)executable
{
	NSString *operation;
	NSIndexSet *statuses;

	if (mode > Ext4CheckPreen) {
		return nil;
	}
	operation =
	    mode == Ext4CheckVerify ? @"verify" : (mode == Ext4CheckRepair ? @"repair" : @"preen");
	/* e2fsck may report successful correction only for writable modes. */
	statuses = mode == Ext4CheckVerify
	    ? [NSIndexSet indexSetWithIndex:0]
	    : [NSIndexSet indexSetWithIndexesInRange:NSMakeRange(0, 2)];
	return [super initWithResource:resource
			    executable:executable
			     arguments:@[ operation ]
			      writable:mode != Ext4CheckVerify
		successfulExitStatuses:statuses];
}

@end

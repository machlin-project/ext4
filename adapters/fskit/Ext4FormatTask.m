/* SPDX-License-Identifier: BSD-3-Clause */
#import "Ext4FormatTask.h"
#include <string.h>

@implementation Ext4FormatTask

- (instancetype)initWithResource:(Ext4ResourceIO *)resource
		       blockSize:(NSUInteger)blockSize
			    name:(NSString *)name
			    uuid:(NSUUID *)uuid
		      executable:(NSURL *)executable
{
	NSData *encoded = [name dataUsingEncoding:NSUTF8StringEncoding];
	NSString *size;

	if ((blockSize != 1024 && blockSize != 2048 && blockSize != 4096) || encoded == nil ||
	    encoded.length > EXT4_VOLUME_NAME_SIZE || name.UTF8String == NULL ||
	    strlen(name.UTF8String) != encoded.length || uuid == nil) {
		return nil;
	}
	size = [NSString stringWithFormat:@"%lu", (unsigned long)blockSize];
	return [super initWithResource:resource
			    executable:executable
			     arguments:@[ @"format", size, name, uuid.UUIDString ]
			      writable:YES
		successfulExitStatuses:[NSIndexSet indexSetWithIndex:0]];
}

@end

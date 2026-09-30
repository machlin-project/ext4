/* SPDX-License-Identifier: BSD-3-Clause */
#import <FSKit/FSKit.h>
#include <ext4/ext4.h>

@interface Ext4Volume : FSVolume <FSVolumeOperations>
/* Takes ownership of filesystem on success. ResourceOwner retains callback
 * storage through the last item and ext4_unmount. The device view is exclusive. */
- (instancetype)initWithResource:(FSBlockDeviceResource *)resource
		      filesystem:(struct ext4_fs *)filesystem
		   resourceOwner:(id)resourceOwner;
- (NSError *)checkMountEligibility;
- (void)invalidate;

@end

@interface Ext4Volume (FileIO) <FSVolumeReadWriteOperations, FSVolumeXattrOperations,
    FSVolumeKernelOffloadedIOOperations>
@end

@interface Ext4Volume (Control)
- (NSDictionary *)controlRequest:(NSDictionary *)request;
@end

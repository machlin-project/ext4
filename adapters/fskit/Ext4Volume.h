/* SPDX-License-Identifier: BSD-3-Clause */
#import <FSKit/FSKit.h>
#include <ext4/ext4.h>

struct ext4_native_crypto;

@interface Ext4Volume : FSVolume <FSVolumeOperations>
/* Takes ownership of filesystem on success. ResourceOwner retains callback
 * storage through the last item and ext4_unmount. The device view is exclusive. */
- (instancetype)initWithResource:(FSBlockDeviceResource *)resource
		      filesystem:(struct ext4_fs *)filesystem
		   resourceOwner:(id)resourceOwner;
/* Also takes crypto ownership on success, with keys fixed for this mount. */
- (instancetype)initWithResource:(FSBlockDeviceResource *)resource
		      filesystem:(struct ext4_fs *)filesystem
		   resourceOwner:(id)resourceOwner
			  crypto:(struct ext4_native_crypto *)crypto;
@property(nonatomic, strong) NSError *keyStoreError;
- (NSError *)checkMountEligibility;
- (void)invalidate;

@end

@interface Ext4Volume (FileIO) <FSVolumeReadWriteOperations, FSVolumeXattrOperations,
    FSVolumeKernelOffloadedIOOperations>
@end

@interface Ext4Volume (Control)
- (NSDictionary *)controlRequest:(NSDictionary *)request;
@end

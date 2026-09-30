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
/* Writable ownership starts at construction, including its MMP heartbeat. */
- (instancetype)initWithResource:(FSBlockDeviceResource *)resource
		      filesystem:(struct ext4_fs *)filesystem
		   resourceOwner:(id)resourceOwner
			  crypto:(struct ext4_native_crypto *)crypto
			writable:(BOOL)writable;
@property(nonatomic, strong) NSError *keyStoreError;
@property(nonatomic, strong) NSError *writeAvailabilityError;
@property(nonatomic, readonly) BOOL writable;
- (NSError *)checkMountEligibility;
/* Release a writable load used only for maintenance, before resource unload. */
- (NSError *)finishUnloadedResource;
- (void)invalidate;

@end

@interface Ext4Volume (FileIO) <FSVolumeOpenCloseOperations, FSVolumeXattrOperations,
    FSVolumeKernelOffloadedIOOperations, FSVolumeItemDeactivation>
@end

@interface Ext4Volume (Control)
- (NSDictionary *)controlRequest:(NSDictionary *)request;
@end

@interface Ext4Volume (Mutation) <FSVolumePreallocateOperations, FSVolumeRenameOperations>
@end

/* Read/write protocols reuse selectors with incompatible reply blocks. Sibling
 * classes keep each volume's reply ABI fixed for its entire lifetime. */
@interface Ext4LegacyVolume : Ext4Volume <FSVolumeReadWriteOperations>
@end

#if defined(__MAC_27_0) && __MAC_OS_X_VERSION_MAX_ALLOWED >= __MAC_27_0
API_AVAILABLE(macos(27.0))
@interface Ext4ModernVolume : Ext4Volume <FSVolumeReadWriteHandler>
@end
#endif

/* Select the runtime API, retaining the same ownership contract as init. */
Ext4Volume *ext4_volume_create(FSBlockDeviceResource *resource, struct ext4_fs *filesystem,
    id resourceOwner, struct ext4_native_crypto *crypto, BOOL writable);

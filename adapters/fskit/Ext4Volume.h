/* SPDX-License-Identifier: BSD-3-Clause */
#import <FSKit/FSKit.h>
#include <ext4/ext4.h>

struct ext4_native_crypto;

@interface Ext4Volume : FSVolume <FSVolumePathConfOperations>
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
@property(nonatomic, readonly) FSVolumeSupportedCapabilities *supportedVolumeCapabilities;
@property(nonatomic, readonly) FSStatFSResult *volumeStatistics;
@property(nonatomic, readonly) FSItemDeactivationOptions itemDeactivationPolicy;
- (void)mountWithOptions:(FSTaskOptions *)options replyHandler:(void (^)(NSError *))reply;
- (void)unmountWithReplyHandler:(void (^)(void))reply;
- (void)synchronizeWithFlags:(FSSyncFlags)flags replyHandler:(void (^)(NSError *))reply;
- (void)reclaimItem:(FSItem *)item replyHandler:(void (^)(NSError *))reply;
- (NSError *)checkMountEligibility;
/* Release a writable load used only for maintenance, before resource unload. */
- (NSError *)finishUnloadedResource;
- (void)invalidate;

@end

@interface Ext4Volume (Control)
- (NSDictionary *)controlRequest:(NSDictionary *)request;
@end

/* Select one complete protocol family. Some selectors have incompatible reply
 * ABIs; conformance belongs to the runtime-specific classes, never their owner. */
@interface Ext4LegacyVolume
    : Ext4Volume <FSVolumeOperations, FSVolumeReadWriteOperations, FSVolumeOpenCloseOperations,
	  FSVolumeXattrOperations, FSVolumeItemDeactivation, FSVolumePreallocateOperations,
	  FSVolumeRenameOperations>
@end

/* Only immutable read-only owners export disk mappings. Writable owners use
 * core I/O exclusively and do not negotiate a kernel block-mapping protocol. */
@interface Ext4LegacyMappedVolume : Ext4LegacyVolume <FSVolumeKernelOffloadedIOOperations>
@end

#if defined(__MAC_27_0) && __MAC_OS_X_VERSION_MAX_ALLOWED >= __MAC_27_0
API_AVAILABLE(macos(27.0))
@interface Ext4ModernVolume
    : Ext4Volume <FSVolumeHandler, FSVolumeReadWriteHandler, FSVolumeOpenCloseHandler,
	  FSVolumeXattrHandler, FSVolumeItemDeactivationHandler, FSVolumePreallocateHandler,
	  FSVolumeRenameHandler, FSVolumeSeekRegionHandler>
@end

API_AVAILABLE(macos(27.0))
@interface Ext4ModernMappedVolume : Ext4ModernVolume <FSVolumeKernelOffloadedIOHandler>
@end
#endif

/* Select the runtime API, retaining the same ownership contract as init. */
Ext4Volume *ext4_volume_create(FSBlockDeviceResource *resource, struct ext4_fs *filesystem,
    id resourceOwner, struct ext4_native_crypto *crypto, BOOL writable);

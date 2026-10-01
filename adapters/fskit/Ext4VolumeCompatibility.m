/* SPDX-License-Identifier: BSD-3-Clause */
#import "Ext4VolumeInternal.h"
#import "Ext4Support.h"

@implementation Ext4LegacyVolume

- (void)readFromFile:(FSItem *)item
	      offset:(off_t)offset
	      length:(size_t)length
	  intoBuffer:(FSMutableFileDataBuffer *)buffer
	replyHandler:(void (^)(size_t, NSError *))reply
{
	[self readFile:item offset:offset length:length intoBuffer:buffer replyHandler:reply];
}

- (void)writeContents:(NSData *)contents
	       toFile:(FSItem *)item
	     atOffset:(off_t)offset
	 replyHandler:(void (^)(size_t, NSError *))reply
{
	[self writeFile:item contents:contents offset:offset replyHandler:reply];
}

- (void)blockmapFile:(FSItem *)file
	      offset:(off_t)offset
	      length:(size_t)length
	       flags:(FSBlockmapFlags)flags
	 operationID:(FSOperationID)operationID
	      packer:(FSExtentPacker *)packer
	replyHandler:(void (^)(NSError *))reply
{
	[self mapFile:file
		  offset:offset
		  length:length
		   flags:flags
	     operationID:operationID
		  packer:packer
	    replyHandler:reply];
}

- (void)completeIOForFile:(FSItem *)file
		   offset:(off_t)offset
		   length:(size_t)length
		   status:(NSError *)status
		    flags:(FSCompleteIOFlags)flags
	      operationID:(FSOperationID)operationID
	     replyHandler:(void (^)(NSError *))reply
{
	[self finishIOForFile:file
		       offset:offset
		       length:length
		       status:status
			flags:flags
		  operationID:operationID
		 replyHandler:reply];
}

@end

Ext4Volume *
ext4_volume_create(FSBlockDeviceResource *resource, struct ext4_fs *filesystem, id resourceOwner,
    struct ext4_native_crypto *crypto, BOOL writable)
{
	Class volumeClass = Ext4LegacyVolume.class;

#if defined(__MAC_27_0) && __MAC_OS_X_VERSION_MAX_ALLOWED >= __MAC_27_0
	if (@available(macOS 27.0, *)) {
		volumeClass = Ext4ModernVolume.class;
	}
#endif
	return [[volumeClass alloc] initWithResource:resource
					  filesystem:filesystem
				       resourceOwner:resourceOwner
					      crypto:crypto
					    writable:writable];
}

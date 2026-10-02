/* SPDX-License-Identifier: BSD-3-Clause */
#import "Ext4Volume.h"
#import "Ext4Control.h"
#import "Ext4ResourceIO.h"
#import "Ext4CheckTask.h"
#import "Ext4FormatTask.h"
#include "Ext4Crypto.h"

@interface Ext4Item : FSItem {
      @public
	struct ext4_inode inode;
	struct ext4_inode_hold *hold;
	Ext4Volume *owner;
	FSDirectoryVerifier directoryVersion;
}
@end

@interface Ext4Volume () {
	struct ext4_fs *_fs;
	struct ext4_native_crypto *_crypto;
	struct ext4_info _info;
	FSBlockDeviceResource *_resource;
	Ext4ResourceIO *_resourceOwner;
	Ext4ResourceTask *_maintenance;
	enum ext4_result _openError;
	BOOL _active;
	BOOL _maintenanceOnly;
	BOOL _mounted;
	BOOL _writeClosed;
	BOOL _retainReadState;
	BOOL _memoryPressureRaised;
	dispatch_source_t _memoryPressureSource;
	Ext4ControlServer *_control;
	NSError *_controlError;
	NSMapTable<NSNumber *, Ext4Item *> *_items;
	uint64_t _directoryVersion;
	dispatch_source_t _mmpTimer;
	enum ext4_result _lifetimeError;
}

@end

@interface Ext4Volume (Maintenance)
- (Ext4CheckTask *)beginCheck:(Ext4CheckMode)mode
		   executable:(NSURL *)executable
			error:(NSError **)error;
- (NSError *)finishCheck:(Ext4CheckTask *)check error:(NSError *)error;
- (Ext4FormatTask *)beginFormatWithBlockSize:(NSUInteger)blockSize
					name:(NSString *)name
					uuid:(NSUUID *)uuid
				  executable:(NSURL *)executable
				       error:(NSError **)error;
- (NSError *)beginResourceMaintenance:(Ext4ResourceTask *)task writable:(BOOL)writable;
- (NSError *)finishFormat:(Ext4FormatTask *)format error:(NSError *)error;
@end

/* Namespace and metadata implementation, shared by the versioned FSKit entry points. */
@interface Ext4Volume (Mutations)
- (void)createNamed:(FSFileName *)name
	       type:(FSItemType)type
	     parent:(FSItem *)directory
	 attributes:(FSItemSetAttributesRequest *)attributes
	       link:(FSFileName *)link
	      reply:(void(NS_NOESCAPE ^)(FSItem *, FSFileName *, NSError *))reply;
- (void)linkItem:(FSItem *)file
	   named:(FSFileName *)name
     inDirectory:(FSItem *)directory
    replyHandler:(void(NS_NOESCAPE ^)(FSFileName *, NSError *))reply;
- (void)deleteItem:(FSItem *)file
	     named:(FSFileName *)name
     fromDirectory:(FSItem *)directory
      replyHandler:(void(NS_NOESCAPE ^)(NSError *))reply;
- (void)moveItem:(FSItem *)file
     inDirectory:(FSItem *)sourceDirectory
	   named:(FSFileName *)sourceName
       toNewName:(FSFileName *)destinationName
     inDirectory:(FSItem *)destinationDirectory
	overItem:(FSItem *)overFile
    replyHandler:(void(NS_NOESCAPE ^)(FSFileName *, NSError *))reply;
- (void)changeAttributes:(FSItemSetAttributesRequest *)request
		  onItem:(FSItem *)file
	    replyHandler:(void(NS_NOESCAPE ^)(FSItemAttributes *, NSError *))reply;
@end

@interface Ext4Volume (OwnerOperations)
- (void)readFile:(FSItem *)item
	  offset:(off_t)offset
	  length:(size_t)length
      intoBuffer:(FSMutableFileDataBuffer *)buffer
    replyHandler:(void(NS_NOESCAPE ^)(size_t, NSError *))reply;
- (void)writeFile:(FSItem *)item
	 contents:(NSData *)contents
	   offset:(off_t)offset
     replyHandler:(void(NS_NOESCAPE ^)(size_t, NSError *))reply;
- (void)startResourceMaintenance;
- (void)releaseHold:(struct ext4_inode_hold *)hold;
- (enum ext4_result)ownerError;
- (enum ext4_result)validateItem:(Ext4Item *)item;
- (enum ext4_result)validateMutation:(Ext4Item *)item;
- (Ext4Item *)itemForInode:(const struct ext4_inode *)inode error:(enum ext4_result *)error;
- (void)changedDirectory:(Ext4Item *)directory;
- (void)writeUpdateForItem:(Ext4Item *)item
		    update:(struct ext4_inode_update *)update
		    change:(struct ext4_xattr_change *)change;
- (BOOL)canOffload:(const struct ext4_inode *)inode;
- (FSItemAttributes *)attributesForInode:(const struct ext4_inode *)inode;
- (enum ext4_result)readItem:(Ext4Item *)item
		      offset:(uint64_t)offset
		      buffer:(void *)buffer
		      length:(size_t)length
		   completed:(size_t *)completed;
- (enum ext4_result)seekItem:(Ext4Item *)item
		      offset:(uint64_t)offset
		      region:(enum ext4_seek_region)region
		      result:(uint64_t *)result;
@end

@interface Ext4Volume (ControlLifecycle)
- (void)startControl;
@end

/* Read metadata is disposable; inode holds and dirty journal state are not. */
@interface Ext4Volume (ReadState)
- (void)startReadStateMaintenance;
- (void)stopReadStateMaintenance;
- (void)updateMemoryPressure:(dispatch_source_memorypressure_flags_t)flags;
- (BOOL)readStateRetentionActive;
- (void)finishReadStateForItem:(Ext4Item *)item;
- (void)dropReadState;
@end

/* Engine callbacks are synchronous under the volume monitor. Version-specific
 * protocol adapters translate their results before releasing that ownership. */
@interface Ext4Volume (SharedCallbacks)
- (void)createSymbolicLinkNamed:(FSFileName *)name
		    inDirectory:(FSItem *)directory
		     attributes:(FSItemSetAttributesRequest *)attributes
		   linkContents:(FSFileName *)contents
		   replyHandler:(void(NS_NOESCAPE ^)(FSItem *, FSFileName *, NSError *))reply;
- (void)createLinkToItem:(FSItem *)item
		   named:(FSFileName *)name
	     inDirectory:(FSItem *)directory
	    replyHandler:(void(NS_NOESCAPE ^)(FSFileName *, NSError *))reply;
- (void)removeItem:(FSItem *)item
	     named:(FSFileName *)name
     fromDirectory:(FSItem *)directory
      replyHandler:(void(NS_NOESCAPE ^)(NSError *))reply;
- (void)renameItem:(FSItem *)item
       inDirectory:(FSItem *)sourceDirectory
	     named:(FSFileName *)sourceName
	 toNewName:(FSFileName *)destinationName
       inDirectory:(FSItem *)destinationDirectory
	  overItem:(FSItem *)overItem
      replyHandler:(void(NS_NOESCAPE ^)(FSFileName *, NSError *))reply;
- (void)setAttributes:(FSItemSetAttributesRequest *)request
	       onItem:(FSItem *)item
	 replyHandler:(void(NS_NOESCAPE ^)(FSItemAttributes *, NSError *))reply;
- (void)createFileNamed:(FSFileName *)name
	    inDirectory:(FSItem *)directory
	     attributes:(FSItemSetAttributesRequest *)attributes
		 packer:(FSExtentPacker *)packer
	   replyHandler:(void(NS_NOESCAPE ^)(FSItem *, FSFileName *, NSError *))reply;
- (void)createItemNamed:(FSFileName *)name
		   type:(FSItemType)type
	    inDirectory:(FSItem *)directory
	     attributes:(FSItemSetAttributesRequest *)attributes
	   replyHandler:(void(NS_NOESCAPE ^)(FSItem *, FSFileName *, NSError *))reply;
- (void)activateWithOptions:(FSTaskOptions *)options
	       replyHandler:(void(NS_NOESCAPE ^)(FSItem *, NSError *))reply;
- (void)deactivateWithOptions:(FSDeactivateOptions)options
		 replyHandler:(void(NS_NOESCAPE ^)(NSError *))reply;
- (void)lookupItemNamed:(FSFileName *)name
	    inDirectory:(FSItem *)directory
	   replyHandler:(void(NS_NOESCAPE ^)(FSItem *, FSFileName *, NSError *))reply;
- (void)getAttributes:(FSItemGetAttributesRequest *)desiredAttributes
	       ofItem:(FSItem *)item
	 replyHandler:(void(NS_NOESCAPE ^)(FSItemAttributes *, NSError *))reply;
- (void)enumerateDirectory:(FSItem *)directory
	  startingAtCookie:(FSDirectoryCookie)cookie
		  verifier:(FSDirectoryVerifier)verifier
       providingAttributes:(FSItemGetAttributesRequest *)attributes
	       usingPacker:(FSDirectoryEntryPacker *)packer
	      replyHandler:(void(NS_NOESCAPE ^)(FSDirectoryVerifier, NSError *))reply;
- (void)readSymbolicLink:(FSItem *)item
	    replyHandler:(void(NS_NOESCAPE ^)(FSFileName *, NSError *))reply;
- (void)deactivateItem:(FSItem *)file replyHandler:(void(NS_NOESCAPE ^)(NSError *))reply;
- (void)openItem:(FSItem *)item
       withModes:(FSVolumeOpenModes)modes
    replyHandler:(void(NS_NOESCAPE ^)(NSError *))reply;
- (void)closeItem:(FSItem *)item
     keepingModes:(FSVolumeOpenModes)modes
     replyHandler:(void(NS_NOESCAPE ^)(NSError *))reply;
- (void)lookupItemNamed:(FSFileName *)name
	    inDirectory:(FSItem *)directory
		 packer:(FSExtentPacker *)packer
	   replyHandler:(void(NS_NOESCAPE ^)(FSItem *, FSFileName *, NSError *))reply;
- (void)mapFile:(FSItem *)file
	  offset:(off_t)offset
	  length:(size_t)length
	   flags:(FSBlockmapFlags)flags
     operationID:(FSOperationID)operationID
	  packer:(FSExtentPacker *)packer
    replyHandler:(void(NS_NOESCAPE ^)(NSError *))reply;
- (void)finishIOForFile:(FSItem *)file
		 offset:(off_t)offset
		 length:(size_t)length
		 status:(NSError *)status
		  flags:(FSCompleteIOFlags)flags
	    operationID:(FSOperationID)operationID
	   replyHandler:(void(NS_NOESCAPE ^)(NSError *))reply;
- (void)getXattrNamed:(FSFileName *)name
	       ofItem:(FSItem *)file
	 replyHandler:(void(NS_NOESCAPE ^)(NSData *, NSError *))reply;
- (void)listXattrsOfItem:(FSItem *)file
	    replyHandler:(void(NS_NOESCAPE ^)(NSArray<FSFileName *> *, NSError *))reply;
- (void)setXattrNamed:(FSFileName *)name
	       toData:(NSData *)value
	       onItem:(FSItem *)item
	       policy:(FSSetXattrPolicy)policy
	 replyHandler:(void(NS_NOESCAPE ^)(NSError *))reply;
- (void)setVolumeName:(FSFileName *)name
	 replyHandler:(void(NS_NOESCAPE ^)(FSFileName *, NSError *))reply;
- (void)preallocateSpaceForItem:(FSItem *)file
		       atOffset:(off_t)offset
			 length:(size_t)length
			  flags:(FSPreallocateFlags)flags
		   replyHandler:(void(NS_NOESCAPE ^)(size_t, NSError *))reply;
@end

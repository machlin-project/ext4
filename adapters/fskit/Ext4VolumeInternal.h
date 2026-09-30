/* SPDX-License-Identifier: BSD-3-Clause */
#import "Ext4Volume.h"
#import "Ext4Control.h"
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
	id _resourceOwner;
	BOOL _active;
	BOOL _mounted;
	BOOL _retainReadState;
	Ext4ControlServer *_control;
	NSError *_controlError;
	NSMapTable<NSNumber *, Ext4Item *> *_items;
	uint64_t _directoryVersion;
	dispatch_source_t _mmpTimer;
	enum ext4_result _lifetimeError;
}

@end

/* Namespace and metadata implementation, shared by the versioned FSKit entry points. */
@interface Ext4Volume (Mutations)
- (void)createNamed:(FSFileName *)name
	       type:(FSItemType)type
	     parent:(FSItem *)directory
	 attributes:(FSItemSetAttributesRequest *)attributes
	       link:(FSFileName *)link
	      reply:(void (^)(FSItem *, FSFileName *, NSError *))reply;
- (void)linkItem:(FSItem *)file
	   named:(FSFileName *)name
     inDirectory:(FSItem *)directory
    replyHandler:(void (^)(FSFileName *, NSError *))reply;
- (void)deleteItem:(FSItem *)file
	     named:(FSFileName *)name
     fromDirectory:(FSItem *)directory
      replyHandler:(void (^)(NSError *))reply;
- (void)moveItem:(FSItem *)file
     inDirectory:(FSItem *)sourceDirectory
	   named:(FSFileName *)sourceName
       toNewName:(FSFileName *)destinationName
     inDirectory:(FSItem *)destinationDirectory
	overItem:(FSItem *)overFile
    replyHandler:(void (^)(FSFileName *, NSError *))reply;
- (void)changeAttributes:(FSItemSetAttributesRequest *)request
		  onItem:(FSItem *)file
	    replyHandler:(void (^)(FSItemAttributes *, NSError *))reply;
@end

@interface Ext4Volume (OwnerOperations)
- (void)releaseHold:(struct ext4_inode_hold *)hold;
- (enum ext4_result)validateItem:(Ext4Item *)item;
- (enum ext4_result)validateMutation:(Ext4Item *)item;
- (Ext4Item *)itemForInode:(const struct ext4_inode *)inode error:(enum ext4_result *)error;
- (void)changedDirectory:(Ext4Item *)directory;
- (enum ext4_result)writeUpdateForItem:(Ext4Item *)item
				update:(struct ext4_inode_update *)update
				change:(struct ext4_xattr_change *)change;
- (BOOL)canOffload:(const struct ext4_inode *)inode;
- (FSItemAttributes *)attributesForInode:(const struct ext4_inode *)inode;
- (enum ext4_result)readItem:(Ext4Item *)item
		      offset:(uint64_t)offset
		      buffer:(void *)buffer
		      length:(size_t)length
		   completed:(size_t *)completed;
@end

@interface Ext4Volume (ControlLifecycle)
- (void)startControl;
@end

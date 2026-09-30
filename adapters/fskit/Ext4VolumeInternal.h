/* SPDX-License-Identifier: BSD-3-Clause */
#import "Ext4Volume.h"
#import "Ext4Control.h"

@interface Ext4Item : FSItem {
      @public
	struct ext4_inode inode;
	struct ext4_inode_hold *hold;
	Ext4Volume *owner;
}
@end

@interface Ext4Volume () {
	struct ext4_fs *_fs;
	struct ext4_info _info;
	FSBlockDeviceResource *_resource;
	id _resourceOwner;
	BOOL _active;
	BOOL _mounted;
	BOOL _retainReadState;
	Ext4ControlServer *_control;
	NSError *_controlError;
	NSMapTable<NSNumber *, Ext4Item *> *_items;
}

@end

@interface Ext4Volume (OwnerOperations)
- (void)releaseHold:(struct ext4_inode_hold *)hold;
- (enum ext4_result)validateItem:(Ext4Item *)item;
- (BOOL)canOffload:(const struct ext4_inode *)inode;
- (enum ext4_result)readItem:(Ext4Item *)item
		      offset:(uint64_t)offset
		      buffer:(void *)buffer
		      length:(size_t)length
		   completed:(size_t *)completed;
@end

@interface Ext4Volume (ControlLifecycle)
- (void)startControl;
@end

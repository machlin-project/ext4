/* SPDX-License-Identifier: BSD-3-Clause */
#import "Ext4VolumeInternal.h"
#import "Ext4Support.h"
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/param.h>
#include <sys/stat.h>

static FSItemType
ext4_item_type(uint16_t mode)
{
	switch (mode & S_IFMT) {
	case S_IFREG:
		return FSItemTypeFile;
	case S_IFDIR:
		return FSItemTypeDirectory;
	case S_IFLNK:
		return FSItemTypeSymlink;
	case S_IFIFO:
		return FSItemTypeFIFO;
	case S_IFCHR:
		return FSItemTypeCharDevice;
	case S_IFBLK:
		return FSItemTypeBlockDevice;
	case S_IFSOCK:
		return FSItemTypeSocket;
	default:
		return FSItemTypeUnknown;
	}
}

@implementation Ext4Item

- (void)dealloc
{
	[owner releaseHold:hold];
}

@end

@implementation Ext4Volume

- (instancetype)initWithResource:(FSBlockDeviceResource *)resource
		      filesystem:(struct ext4_fs *)fs
		   resourceOwner:(id)resourceOwner
{
	return [self initWithResource:resource
			   filesystem:fs
			resourceOwner:resourceOwner
			       crypto:NULL];
}

- (instancetype)initWithResource:(FSBlockDeviceResource *)resource
		      filesystem:(struct ext4_fs *)fs
		   resourceOwner:(id)resourceOwner
			  crypto:(struct ext4_native_crypto *)crypto
{
	struct ext4_crypto_environment environment;
	struct ext4_info info;
	NSUUID *uuid;
	FSVolumeIdentifier *identifier;
	FSFileName *name;

	if (crypto != NULL) {
		ext4_native_crypto_seal(crypto);
		environment = ext4_native_crypto_environment(crypto);
		if (ext4_set_crypto(fs, &environment) != EXT4_OK) {
			return nil;
		}
	}
	ext4_get_info(fs, &info);
	uuid = [[NSUUID alloc] initWithUUIDBytes:info.uuid];
	identifier = [[FSVolumeIdentifier alloc] initWithUUID:uuid];
	name = [FSFileName nameWithBytes:info.volume_name
				  length:strnlen(info.volume_name, EXT4_VOLUME_NAME_SIZE)];
	self = [super initWithVolumeID:identifier volumeName:name];
	if (self != nil) {
		_fs = fs;
		_crypto = crypto;
		_info = info;
		_resource = resource;
		_resourceOwner = resourceOwner;
		_active = YES;
		_retainReadState = YES;
		_items = [NSMapTable strongToWeakObjectsMapTable];
	}
	return self;
}

- (void)dealloc
{
	[_control stop];
	ext4_unmount(_fs);
	ext4_native_crypto_destroy(_crypto);
}

- (void)releaseHold:(struct ext4_inode_hold *)hold
{
	@synchronized(self) {
		if (hold != NULL) {
			(void)ext4_release_inode(hold);
		}
	}
}

/* Called under the volume lock, including publication through the reply block. */
- (Ext4Item *)itemForInode:(const struct ext4_inode *)inode error:(enum ext4_result *)error
{
	Ext4Item *item = [_items objectForKey:@(inode->number)];
	struct ext4_inode_hold *hold = NULL;
	size_t aclSize = 0;

	if (item != nil) {
		if (item->inode.generation != inode->generation || item->hold == NULL) {
			*error = EXT4_STALE;
			return nil;
		}
		return item;
	}
	*error = ext4_get_xattr(_fs, inode->number, inode->generation, EXT4_XATTR_POSIX_ACL_ACCESS,
	    NULL, 0, NULL, 0, &aclSize);
	if (*error != EXT4_NOT_FOUND) {
		if (*error == EXT4_OK) {
			*error = EXT4_UNSUPPORTED;
		}
		return nil;
	}
	*error = ext4_hold_inode(_fs, inode->number, inode->generation, &hold);
	if (*error != EXT4_OK) {
		return nil;
	}
	item = [[Ext4Item alloc] init];
	item->inode = *inode;
	item->hold = hold;
	item->owner = self;
	[_items setObject:item forKey:@(inode->number)];
	return item;
}

- (enum ext4_result)validateItem:(Ext4Item *)item
{
	if (!_active || ![item isKindOfClass:Ext4Item.class] || item->owner != self ||
	    item->hold == NULL) {
		return EXT4_STALE;
	}
	return EXT4_OK;
}

- (BOOL)canOffload:(const struct ext4_inode *)inode
{
	/* FSKit retains mappings after blockmap returns. Until writable cache
	 * invalidation is accepted, only immutable read-only mappings are supplied.
	 * Partial EOF blocks stay on the core path, which always clips to size. */
	return _resource != nil && ext4_inode_can_map_read(inode) &&
	    inode->size % _info.block_size == 0;
}

- (FSItemAttributes *)attributesForInode:(const struct ext4_inode *)inode
{
	@synchronized(self) {
		FSItemAttributes *attributes = [[FSItemAttributes alloc] init];
		struct timespec time;

		attributes.uid = inode->uid;
		attributes.gid = inode->gid;
		attributes.mode = inode->mode & ALLPERMS;
		attributes.type = ext4_item_type(inode->mode);
		attributes.linkCount = inode->links;
		attributes.size = inode->size;
		attributes.allocSize = inode->blocks_512 * 512U;
		attributes.fileID = inode->number;
		attributes.inhibitKernelOffloadedIO = ![self canOffload:inode];
		time.tv_nsec = inode->modify_time.nanoseconds;
		time.tv_sec = inode->modify_time.seconds;
		attributes.modifyTime = time;
		time.tv_nsec = inode->change_time.nanoseconds;
		time.tv_sec = inode->change_time.seconds;
		attributes.changeTime = time;
		time.tv_nsec = inode->access_time.nanoseconds;
		time.tv_sec = inode->access_time.seconds;
		attributes.accessTime = time;
		if (inode->birth_time_valid) {
			time.tv_nsec = inode->birth_time.nanoseconds;
			time.tv_sec = inode->birth_time.seconds;
			attributes.birthTime = time;
		}
		return attributes;
	}
}

- (NSInteger)maximumLinkCount
{
	return EXT4_LINK_MAX;
}

- (NSInteger)maximumNameLength
{
	return EXT4_NAME_MAX;
}

- (BOOL)restrictsOwnershipChanges
{
	return YES;
}

- (BOOL)truncatesLongNames
{
	return NO;
}

- (NSInteger)maximumXattrSize
{
	return 65536;
}

- (uint64_t)maximumFileSize
{
	return (uint64_t)UINT32_MAX * _info.block_size;
}

- (FSMountOptions)requestedMountOptions
{
#if DEBUG
	NSLog(@"Machlin ext4 requested read-only mount flags");
#endif
	return FSMountOptionsReadOnly;
}

- (FSVolumeSupportedCapabilities *)supportedVolumeCapabilities
{
	FSVolumeSupportedCapabilities *capabilities = [[FSVolumeSupportedCapabilities alloc] init];

	capabilities.supportsPersistentObjectIDs = YES;
	capabilities.supportsSymbolicLinks = YES;
	capabilities.supportsHardLinks = YES;
	capabilities.supportsSparseFiles = YES;
	capabilities.supportsFastStatFS = YES;
	capabilities.doesNotSupportSettingFilePermissions = YES;
	capabilities.doesNotSupportImmutableFiles = YES;
	capabilities.caseFormat = FSVolumeCaseFormatSensitive;
	return capabilities;
}

- (FSStatFSResult *)volumeStatistics
{
	@synchronized(self) {
		FSStatFSResult *statistics =
		    [[FSStatFSResult alloc] initWithFileSystemTypeName:@"machlin_ext4"];

		statistics.blockSize = _info.block_size;
		statistics.ioSize = _info.block_size;
		statistics.totalBlocks = _info.blocks;
		statistics.freeBlocks = _info.free_blocks;
		statistics.availableBlocks = 0;
		statistics.usedBlocks = _info.blocks - _info.free_blocks;
		statistics.totalFiles = _info.inodes;
		statistics.freeFiles = _info.free_inodes;
		return statistics;
	}
}

- (void)mountWithOptions:(FSTaskOptions *)options replyHandler:(void (^)(NSError *))reply
{
#if DEBUG
	NSLog(@"Machlin ext4 mount callback");
#endif
	@synchronized(self) {
		(void)options;
		if (!_active) {
			reply(ext4_error(EXT4_STALE));
			return;
		}
		_mounted = YES;
		[self startControl];
		reply(nil);
	}
}

- (void)unmountWithReplyHandler:(void (^)(void))reply
{
#if DEBUG
	NSLog(@"Machlin ext4 unmount callback");
#endif
	@synchronized(self) {
		_mounted = NO;
		[_control stop];
		_control = nil;
		reply();
	}
}

- (void)synchronizeWithFlags:(FSSyncFlags)flags replyHandler:(void (^)(NSError *))reply
{
	@synchronized(self) {
		(void)flags;
		/* The read-only implementation has no pending writes or dirty metadata. */
		reply(nil);
	}
}

- (void)activateWithOptions:(FSTaskOptions *)options
	       replyHandler:(void (^)(FSItem *, NSError *))reply
{
#if DEBUG
	NSLog(@"Machlin ext4 activate callback");
#endif
	@synchronized(self) {
		struct ext4_inode root;
		Ext4Item *item = nil;
		enum ext4_result error;

		(void)options;
		error = _active ? ext4_get_inode(_fs, EXT4_ROOT_INODE, &root) : EXT4_STALE;
		if (error == EXT4_OK) {
			item = [self itemForInode:&root error:&error];
		}
		reply(item, ext4_error(error));
	}
}

- (void)deactivateWithOptions:(FSDeactivateOptions)options replyHandler:(void (^)(NSError *))reply
{
	@synchronized(self) {
		(void)options;
		[self invalidate];
		reply(nil);
	}
}

- (void)reclaimItem:(FSItem *)item replyHandler:(void (^)(NSError *))reply
{
	@synchronized(self) {
		if (![item isKindOfClass:Ext4Item.class]) {
			reply(ext4_error(EXT4_INVALID_ARGUMENT));
			return;
		}
#if defined(__MAC_27_0) && __MAC_OS_X_VERSION_MAX_ALLOWED >= __MAC_27_0
		if (@available(macOS 27.0, *)) {
			Ext4Item *owned = (Ext4Item *)item;

			[item tryReclaimWithBlock:^{
			  if (owned->owner == self) {
				  [self->_items removeObjectForKey:@(owned->inode.number)];
				  [self releaseHold:owned->hold];
				  owned->hold = NULL;
			  }
			}];
		}
#endif
		/* Older FSKit has no conditional reclaim. Retain the core hold until the
		 * framework releases its last strong FSItem reference; the index is weak. */
		reply(nil);
	}
}

- (NSError *)checkMountEligibility
{
	@synchronized(self) {
		struct ext4_inode root;
		enum ext4_result error;

		error = _active ? ext4_get_inode(_fs, EXT4_ROOT_INODE, &root) : EXT4_STALE;
		return ext4_error(error);
	}
}

- (void)lookupItemNamed:(FSFileName *)name
	    inDirectory:(FSItem *)directory
	   replyHandler:(void (^)(FSItem *, FSFileName *, NSError *))reply
{
	@synchronized(self) {
		Ext4Item *parent = (Ext4Item *)directory;
		struct ext4_inode inode;
		NSData *bytes = name.data;
		Ext4Item *found = nil;
		enum ext4_result error;

		error = [self validateItem:parent];
		if (error == EXT4_OK) {
			error = ext4_lookup(_fs, &parent->inode, bytes.bytes, bytes.length, &inode);
		}
		if (error == EXT4_OK) {
			found = [self itemForInode:&inode error:&error];
		}
		reply(found, found != nil ? name : nil, ext4_error(error));
	}
}

- (void)getAttributes:(FSItemGetAttributesRequest *)desiredAttributes
	       ofItem:(FSItem *)item
	 replyHandler:(void (^)(FSItemAttributes *, NSError *))reply
{
	@synchronized(self) {
		Ext4Item *owned = (Ext4Item *)item;
		enum ext4_result result = [self validateItem:owned];

		(void)desiredAttributes;
		reply(result == EXT4_OK ? [self attributesForInode:&owned->inode] : nil,
		    ext4_error(result));
	}
}

- (void)enumerateDirectory:(FSItem *)directory
	  startingAtCookie:(FSDirectoryCookie)cookie
		  verifier:(FSDirectoryVerifier)verifier
       providingAttributes:(FSItemGetAttributesRequest *)attributes
	       usingPacker:(FSDirectoryEntryPacker *)packer
	      replyHandler:(void (^)(FSDirectoryVerifier, NSError *))reply
{
	@synchronized(self) {
		Ext4Item *parent = (Ext4Item *)directory;
		struct ext4_dir_entry entry;
		struct ext4_inode inode;
		FSItemAttributes *itemAttributes;
		FSFileName *name;
		uint64_t next = cookie;
		FSDirectoryVerifier current = FSDirectoryVerifierInitial;
		enum ext4_result error;

		error = [self validateItem:parent];
		if (error != EXT4_OK) {
			reply(current, ext4_error(error));
			return;
		}
		current = (uint64_t)parent->inode.generation + 1;
		if (verifier != FSDirectoryVerifierInitial && verifier != current) {
			reply(current,
			    [NSError errorWithDomain:NSPOSIXErrorDomain code:ESTALE userInfo:nil]);
			return;
		}
		while ((error = ext4_next_dir(_fs, &parent->inode, &next, &entry)) == EXT4_OK) {
			if (attributes != nil &&
			    ((entry.name_length == 1 && entry.name[0] == '.') ||
				(entry.name_length == 2 && entry.name[0] == '.' &&
				    entry.name[1] == '.'))) {
				continue;
			}
			error = ext4_get_inode(_fs, entry.inode, &inode);
			if (error != EXT4_OK) {
				break;
			}
			name = [FSFileName nameWithBytes:(const char *)entry.name
						  length:entry.name_length];
			itemAttributes = attributes == nil ? nil : [self attributesForInode:&inode];
			if (![packer packEntryWithName:name
					      itemType:ext4_item_type(inode.mode)
						itemID:inode.number
					    nextCookie:next
					    attributes:itemAttributes]) {
				break;
			}
		}
		reply(current, error == EXT4_NOT_FOUND ? nil : ext4_error(error));
	}
}

- (void)readSymbolicLink:(FSItem *)item replyHandler:(void (^)(FSFileName *, NSError *))reply
{
	@synchronized(self) {
		Ext4Item *owned = (Ext4Item *)item;
		NSMutableData *bytes;
		size_t completed = 0;
		enum ext4_result error;

		error = [self validateItem:owned];
		if (error != EXT4_OK) {
			reply(nil, ext4_error(error));
			return;
		}
		if ((owned->inode.mode & EXT4_MODE_TYPE) != EXT4_MODE_SYMLINK) {
			reply(nil, ext4_error(EXT4_INVALID_ARGUMENT));
			return;
		}
		if (owned->inode.size >= _info.block_size) {
			reply(nil, ext4_error(EXT4_UNSUPPORTED));
			return;
		}
		/* Encryption stores a length header and padding, while a no-key name
		 * can be longer than a short ciphertext. Read the whole bounded target. */
		bytes = [NSMutableData dataWithLength:_info.block_size];
		error = [self readItem:owned
				offset:0
				buffer:bytes.mutableBytes
				length:bytes.length
			     completed:&completed];
		if (error == EXT4_OK && completed > bytes.length) {
			error = EXT4_IO;
		}
		if (error == EXT4_OK) {
			bytes.length = completed;
		}
		reply(error == EXT4_OK ? [FSFileName nameWithData:bytes] : nil, ext4_error(error));
	}
}

- (void)createItemNamed:(FSFileName *)name
		   type:(FSItemType)type
	    inDirectory:(FSItem *)directory
	     attributes:(FSItemSetAttributesRequest *)newAttributes
	   replyHandler:(void (^)(FSItem *, FSFileName *, NSError *))reply
{
	(void)name;
	(void)type;
	(void)directory;
	(void)newAttributes;
	reply(nil, nil, ext4_error(EXT4_READ_ONLY));
}

- (void)createSymbolicLinkNamed:(FSFileName *)name
		    inDirectory:(FSItem *)directory
		     attributes:(FSItemSetAttributesRequest *)newAttributes
		   linkContents:(FSFileName *)contents
		   replyHandler:(void (^)(FSItem *, FSFileName *, NSError *))reply
{
	(void)name;
	(void)directory;
	(void)newAttributes;
	(void)contents;
	reply(nil, nil, ext4_error(EXT4_READ_ONLY));
}

- (void)createLinkToItem:(FSItem *)item
		   named:(FSFileName *)name
	     inDirectory:(FSItem *)directory
	    replyHandler:(void (^)(FSFileName *, NSError *))reply
{
	(void)item;
	(void)name;
	(void)directory;
	reply(nil, ext4_error(EXT4_READ_ONLY));
}

- (void)renameItem:(FSItem *)item
       inDirectory:(FSItem *)sourceDirectory
	     named:(FSFileName *)sourceName
	 toNewName:(FSFileName *)destinationName
       inDirectory:(FSItem *)destinationDirectory
	  overItem:(FSItem *)overItem
      replyHandler:(void (^)(FSFileName *, NSError *))reply
{
	(void)item;
	(void)sourceDirectory;
	(void)sourceName;
	(void)destinationName;
	(void)destinationDirectory;
	(void)overItem;
	reply(nil, ext4_error(EXT4_READ_ONLY));
}

- (void)removeItem:(FSItem *)item
	     named:(FSFileName *)name
     fromDirectory:(FSItem *)directory
      replyHandler:(void (^)(NSError *))reply
{
	(void)item;
	(void)name;
	(void)directory;
	reply(ext4_error(EXT4_READ_ONLY));
}

- (void)setAttributes:(FSItemSetAttributesRequest *)newAttributes
	       onItem:(FSItem *)item
	 replyHandler:(void (^)(FSItemAttributes *, NSError *))reply
{
	(void)newAttributes;
	(void)item;
	reply(nil, ext4_error(EXT4_READ_ONLY));
}

- (void)invalidate
{
	@synchronized(self) {
		_active = NO;
		_mounted = NO;
		[_control stop];
		_control = nil;
	}
}

@end

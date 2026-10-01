/* SPDX-License-Identifier: BSD-3-Clause */
#import "Ext4VolumeInternal.h"
#import "Ext4Support.h"
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/param.h>
#include <sys/stat.h>

enum { Ext4PreferredIOSize = 128 * 1024 };

/* Borrowed only for the synchronous visit under the volume monitor. */
struct ext4_directory_visit {
	struct ext4_fs *fs;
	__unsafe_unretained Ext4Volume *volume;
	__unsafe_unretained FSDirectoryEntryPacker *packer;
	BOOL attributes;
	enum ext4_result error;
};

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

static enum ext4_dir_action
ext4_pack_directory_entry(void *context, const struct ext4_dir_entry *entry, uint64_t next_cookie)
{
	struct ext4_directory_visit *visit = context;
	struct ext4_inode inode;
	FSItemAttributes *attributes;
	FSFileName *name;

	/* FSKit requests dot entries only for enumeration without attributes. */
	if (visit->attributes &&
	    ((entry->name_length == 1 && entry->name[0] == '.') ||
		(entry->name_length == 2 && entry->name[0] == '.' && entry->name[1] == '.'))) {
		return EXT4_DIR_ACCEPT;
	}
	visit->error = ext4_get_inode(visit->fs, entry->inode, &inode);
	if (visit->error == EXT4_NOT_FOUND) {
		visit->error = EXT4_CORRUPT;
	}
	if (visit->error != EXT4_OK) {
		return EXT4_DIR_STOP;
	}
	name = [FSFileName nameWithBytes:(const char *)entry->name length:entry->name_length];
	attributes = visit->attributes ? [visit->volume attributesForInode:&inode] : nil;
	return [visit->packer packEntryWithName:name
				       itemType:ext4_item_type(inode.mode)
					 itemID:inode.number
				     nextCookie:next_cookie
				     attributes:attributes]
	    ? EXT4_DIR_ACCEPT
	    : EXT4_DIR_STOP;
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
		   resourceOwner:(Ext4ResourceIO *)resourceOwner
{
	return [self initWithResource:resource
			   filesystem:fs
			resourceOwner:resourceOwner
			       crypto:NULL];
}

- (instancetype)initWithResource:(FSBlockDeviceResource *)resource
		      filesystem:(struct ext4_fs *)fs
		   resourceOwner:(Ext4ResourceIO *)resourceOwner
			  crypto:(struct ext4_native_crypto *)crypto
{
	return [self initWithResource:resource
			   filesystem:fs
			resourceOwner:resourceOwner
			       crypto:crypto
			     writable:NO];
}

- (instancetype)initWithResource:(FSBlockDeviceResource *)resource
		      filesystem:(struct ext4_fs *)fs
		   resourceOwner:(Ext4ResourceIO *)resourceOwner
			  crypto:(struct ext4_native_crypto *)crypto
			writable:(BOOL)writable
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
		_writable = writable;
		_retainReadState = YES;
		_items = [NSMapTable strongToWeakObjectsMapTable];
		[self startResourceMaintenance];
	}
	return self;
}

- (void)dealloc
{
	[self stopReadStateMaintenance];
	if (_mmpTimer != nil) {
		dispatch_source_cancel(_mmpTimer);
	}
	[_control stop];
	ext4_unmount(_fs);
	ext4_native_crypto_destroy(_crypto);
}

- (void)releaseHold:(struct ext4_inode_hold *)hold
{
	@synchronized(self) {
		/* A late FSItem release cannot write after journal/MMP teardown. The
		 * core retains this hold until invalidate frees the entire owner. */
		if (hold != NULL && !_writeClosed && [self ownerError] == EXT4_OK) {
			enum ext4_result error = ext4_release_inode(hold);

			if (error != EXT4_OK && _lifetimeError == EXT4_OK) {
				_lifetimeError = error;
			}
		}
	}
}

/* Called under the volume lock, including publication through the reply block. */
- (enum ext4_result)ownerError
{
	if (!_active) {
		return EXT4_STALE;
	}
	/* Do not serve retained core state or release an orphan through a revoked
	 * device. This owner cannot recover when a different device is attached. */
	if (_lifetimeError == EXT4_OK && _resourceOwner.isRevoked) {
		_lifetimeError = EXT4_IO;
		if (_mmpTimer != nil) {
			dispatch_source_cancel(_mmpTimer);
			_mmpTimer = nil;
		}
	}
	return _lifetimeError;
}

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
		item->inode = *inode;
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
	[self changedDirectory:item];
	[_items setObject:item forKey:@(inode->number)];
	return item;
}

- (enum ext4_result)validateItem:(Ext4Item *)item
{
	enum ext4_result error;

	if (!_active || (self.writable && _writeClosed) || ![item isKindOfClass:Ext4Item.class] ||
	    item->owner != self || item->hold == NULL) {
		return EXT4_STALE;
	}
	error = [self ownerError];
	if (error != EXT4_OK) {
		return error;
	}
	return self.writable ? ext4_refresh_inode(item->hold, &item->inode) : EXT4_OK;
}

- (BOOL)canOffload:(const struct ext4_inode *)inode
{
	/* FSKit retains mappings after blockmap returns. Until writable cache
	 * invalidation is accepted, only immutable read-only mappings are supplied.
	 * Partial EOF blocks stay on the core path, which always clips to size. */
	return !self.writable && _resource != nil && ext4_inode_can_map_read(inode) &&
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
		attributes.flags = ((inode->flags & EXT4_INODE_IMMUTABLE) ? SF_IMMUTABLE : 0) |
		    ((inode->flags & EXT4_INODE_APPEND) ? SF_APPEND : 0) |
		    ((inode->flags & EXT4_INODE_NODUMP) ? UF_NODUMP : 0);
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

- (void)createItemNamed:(FSFileName *)name
		   type:(FSItemType)type
	    inDirectory:(FSItem *)directory
	     attributes:(FSItemSetAttributesRequest *)attributes
	   replyHandler:(void(NS_NOESCAPE ^)(FSItem *, FSFileName *, NSError *))reply
{
	[self createNamed:name
		     type:type
		   parent:directory
	       attributes:attributes
		     link:nil
		    reply:reply];
}

- (void)createSymbolicLinkNamed:(FSFileName *)name
		    inDirectory:(FSItem *)directory
		     attributes:(FSItemSetAttributesRequest *)attributes
		   linkContents:(FSFileName *)contents
		   replyHandler:(void(NS_NOESCAPE ^)(FSItem *, FSFileName *, NSError *))reply
{
	[self createNamed:name
		     type:FSItemTypeSymlink
		   parent:directory
	       attributes:attributes
		     link:contents
		    reply:reply];
}

- (void)createLinkToItem:(FSItem *)item
		   named:(FSFileName *)name
	     inDirectory:(FSItem *)directory
	    replyHandler:(void(NS_NOESCAPE ^)(FSFileName *, NSError *))reply
{
	[self linkItem:item named:name inDirectory:directory replyHandler:reply];
}

- (void)removeItem:(FSItem *)item
	     named:(FSFileName *)name
     fromDirectory:(FSItem *)directory
      replyHandler:(void(NS_NOESCAPE ^)(NSError *))reply
{
	[self deleteItem:item named:name fromDirectory:directory replyHandler:reply];
}

- (void)renameItem:(FSItem *)item
       inDirectory:(FSItem *)sourceDirectory
	     named:(FSFileName *)sourceName
	 toNewName:(FSFileName *)destinationName
       inDirectory:(FSItem *)destinationDirectory
	  overItem:(FSItem *)overItem
      replyHandler:(void(NS_NOESCAPE ^)(FSFileName *, NSError *))reply
{
	[self moveItem:item
	     inDirectory:sourceDirectory
		   named:sourceName
	       toNewName:destinationName
	     inDirectory:destinationDirectory
		overItem:overItem
	    replyHandler:reply];
}

- (void)setAttributes:(FSItemSetAttributesRequest *)request
	       onItem:(FSItem *)item
	 replyHandler:(void(NS_NOESCAPE ^)(FSItemAttributes *, NSError *))reply
{
	[self changeAttributes:request onItem:item replyHandler:reply];
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
	NSLog(
	    @"Machlin ext4 requested %@ mount flags", self.writable ? @"read-write" : @"read-only");
#endif
	return self.writable ? 0 : FSMountOptionsReadOnly;
}

- (FSVolumeSupportedCapabilities *)supportedVolumeCapabilities
{
	FSVolumeSupportedCapabilities *capabilities = [[FSVolumeSupportedCapabilities alloc] init];

	capabilities.supportsPersistentObjectIDs = YES;
	capabilities.supportsSymbolicLinks = YES;
	capabilities.supportsHardLinks = YES;
	capabilities.supportsSparseFiles = YES;
	capabilities.supportsFastStatFS = YES;
	/* Read-only is a mount policy; ext4 retains POSIX permissions and owners. */
	capabilities.doesNotSupportSettingFilePermissions = NO;
	capabilities.doesNotSupportImmutableFiles = NO;
	capabilities.caseFormat = FSVolumeCaseFormatSensitive;
	return capabilities;
}

- (FSStatFSResult *)volumeStatistics
{
	@synchronized(self) {
		FSStatFSResult *statistics =
		    [[FSStatFSResult alloc] initWithFileSystemTypeName:@"machlinext4"];
		enum ext4_result error = [self ownerError];

		if (_fs != NULL) {
			ext4_get_info(_fs, &_info);
		}
		statistics.blockSize = _info.block_size;
		/* Match the ext4 personality in the extension's Info.plist. */
		statistics.fileSystemSubType = 0;
		/* Resource geometry describes accounting, not the preferred transfer.
		 * Amortize the userspace crossing over multiple filesystem blocks. */
		statistics.ioSize = MAX(_info.block_size, Ext4PreferredIOSize);
		statistics.totalBlocks = _info.blocks;
		statistics.freeBlocks = _info.free_blocks;
		statistics.availableBlocks =
		    self.writable && error == EXT4_OK && _info.free_blocks > _info.reserved_blocks
		    ? _info.free_blocks - _info.reserved_blocks
		    : 0;
		statistics.usedBlocks = _info.blocks - _info.free_blocks;
		statistics.totalFiles = _info.inodes;
		statistics.freeFiles = _info.free_inodes;
		return statistics;
	}
}

- (void)mountWithOptions:(FSTaskOptions *)options replyHandler:(void(NS_NOESCAPE ^)(NSError *))reply
{
#if DEBUG
	NSLog(@"Machlin ext4 mount callback");
#endif
	@synchronized(self) {
		enum ext4_result error = [self ownerError];

		(void)options;
		if (error != EXT4_OK || (self.writable && _writeClosed)) {
			reply(ext4_error(error != EXT4_OK ? error : EXT4_STALE));
			return;
		}
		_mounted = YES;
		[self startControl];
		reply(nil);
	}
}

- (void)startResourceMaintenance
{
	[self startReadStateMaintenance];
	if (self.writable && _info.mmp_interval != 0 && _mmpTimer == nil) {
		__weak Ext4Volume *weakSelf = self;
		uint64_t interval = (uint64_t)_info.mmp_interval * NSEC_PER_SEC / 2;

		/* Ownership begins on writable load, before activation or mount. Keep
		 * it alive during maintenance-only loads as well as mounted I/O. */
		_mmpTimer = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0,
		    dispatch_get_global_queue(QOS_CLASS_UTILITY, 0));
		dispatch_source_set_timer(
		    _mmpTimer, dispatch_time(DISPATCH_TIME_NOW, (int64_t)interval), interval, 0);
		dispatch_source_set_event_handler(_mmpTimer, ^{
		  Ext4Volume *volume = weakSelf;

		  if (volume == nil) {
			  return;
		  }
		  @synchronized(volume) {
			  if (!volume->_writeClosed && [volume ownerError] == EXT4_OK) {
				  volume->_lifetimeError = ext4_mmp_update(volume->_fs);
			  }
		  }
		});
		dispatch_resume(_mmpTimer);
	}
}

- (void)unmountWithReplyHandler:(void(NS_NOESCAPE ^)(void))reply
{
#if DEBUG
	NSLog(@"Machlin ext4 unmount callback");
#endif
	@synchronized(self) {
		if (self.writable && !_writeClosed && [self ownerError] == EXT4_OK) {
			_lifetimeError = ext4_sync(_fs);
			if (_lifetimeError == EXT4_OK) {
				_lifetimeError = ext4_mmp_release(_fs);
			}
		}
		_writeClosed = YES;
		if (_mmpTimer != nil) {
			dispatch_source_cancel(_mmpTimer);
			_mmpTimer = nil;
		}
		_mounted = NO;
		[_control stop];
		_control = nil;
		reply();
	}
}

- (NSError *)finishUnloadedResource
{
	@synchronized(self) {
		(void)[self ownerError];
		if (_mounted && !_resourceOwner.isRevoked) {
			return [NSError errorWithDomain:NSPOSIXErrorDomain code:EBUSY userInfo:nil];
		}
		/* A load used only for checking never receives the mounted-volume
		 * unmount callback. It still owns a writable journal and possibly MMP. */
		if (_active && !_writeClosed && (self.writable || _resourceOwner.isRevoked)) {
			[self unmountWithReplyHandler:^{
			}];
		}
		return ext4_error(_lifetimeError);
	}
}

- (void)synchronizeWithFlags:(FSSyncFlags)flags replyHandler:(void(NS_NOESCAPE ^)(NSError *))reply
{
	@synchronized(self) {
		enum ext4_result error = [self ownerError];

		(void)flags;
		if (error == EXT4_OK && self.writable && !_writeClosed) {
			error = ext4_sync(_fs);
			if (error != EXT4_OK) {
				_lifetimeError = error;
			}
		}
		reply(ext4_error(error));
	}
}

- (void)activateWithOptions:(FSTaskOptions *)options
	       replyHandler:(void(NS_NOESCAPE ^)(FSItem *, NSError *))reply
{
#if DEBUG
	NSLog(@"Machlin ext4 activate callback");
#endif
	@synchronized(self) {
		struct ext4_inode root;
		Ext4Item *item = nil;
		enum ext4_result error;

		(void)options;
		error = _writeClosed ? EXT4_STALE : [self ownerError];
		if (error == EXT4_OK) {
			error = ext4_get_inode(_fs, EXT4_ROOT_INODE, &root);
		}
		if (error == EXT4_OK) {
			item = [self itemForInode:&root error:&error];
		}
		reply(item, ext4_error(error));
	}
}

- (void)deactivateWithOptions:(FSDeactivateOptions)options
		 replyHandler:(void(NS_NOESCAPE ^)(NSError *))reply
{
	@synchronized(self) {
		(void)options;
		[self invalidate];
		reply(nil);
	}
}

- (void)reclaimItem:(FSItem *)item replyHandler:(void(NS_NOESCAPE ^)(NSError *))reply
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

		error = _writeClosed ? EXT4_STALE : [self ownerError];
		if (error == EXT4_OK) {
			error = ext4_get_inode(_fs, EXT4_ROOT_INODE, &root);
		}
		return ext4_error(error);
	}
}

- (void)lookupItemNamed:(FSFileName *)name
	    inDirectory:(FSItem *)directory
	   replyHandler:(void(NS_NOESCAPE ^)(FSItem *, FSFileName *, NSError *))reply
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
	 replyHandler:(void(NS_NOESCAPE ^)(FSItemAttributes *, NSError *))reply
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
	      replyHandler:(void(NS_NOESCAPE ^)(FSDirectoryVerifier, NSError *))reply
{
	@synchronized(self) {
		Ext4Item *parent = (Ext4Item *)directory;
		struct ext4_directory_visit visit = { _fs, self, packer, attributes != nil,
			EXT4_OK };
		uint64_t next = cookie;
		FSDirectoryVerifier current = FSDirectoryVerifierInitial;
		enum ext4_result error;

		error = [self validateItem:parent];
		if (error != EXT4_OK) {
			reply(current, ext4_error(error));
			return;
		}
		current = parent->directoryVersion;
		if (verifier != FSDirectoryVerifierInitial && verifier != current) {
			reply(current,
			    [NSError errorWithDomain:NSPOSIXErrorDomain code:ESTALE userInfo:nil]);
			return;
		}
		error =
		    ext4_iterate_dir(_fs, &parent->inode, &next, ext4_pack_directory_entry, &visit);
		if (visit.error != EXT4_OK) {
			reply(current, ext4_error(visit.error));
			return;
		}
		reply(current, error == EXT4_NOT_FOUND ? nil : ext4_error(error));
	}
}

- (void)readSymbolicLink:(FSItem *)item
	    replyHandler:(void(NS_NOESCAPE ^)(FSFileName *, NSError *))reply
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

- (void)invalidate
{
	@synchronized(self) {
		[self stopReadStateMaintenance];
		if (_mmpTimer != nil) {
			dispatch_source_cancel(_mmpTimer);
			_mmpTimer = nil;
		}
		_active = NO;
		_mounted = NO;
		_writeClosed = YES;
		[_control stop];
		_control = nil;
		/* Deactivation follows FSKit's final sync and reclaim. Abandon any
		 * remaining holds without I/O, including forced-removal orphans. Late
		 * framework references must not retain an open device or release freed
		 * core state from their eventual Objective-C dealloc. */
		for (Ext4Item *item in _items.objectEnumerator) {
			item->hold = NULL;
		}
		[_items removeAllObjects];
		ext4_unmount(_fs);
		_fs = NULL;
		_resource = nil;
		_resourceOwner = nil;
	}
}

@end

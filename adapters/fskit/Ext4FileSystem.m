/* SPDX-License-Identifier: BSD-3-Clause */
#import "Ext4FileSystem.h"
#include <ext4/ext4.h>

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/param.h>
#include <sys/stat.h>

static NSError *
ext4_error(enum ext4_result result)
{
	int error;

	switch (result) {
	case EXT4_OK:
		return nil;
	case EXT4_NO_MEMORY:
		error = ENOMEM;
		break;
	case EXT4_NOT_FOUND:
		error = ENOENT;
		break;
	case EXT4_NOT_DIRECTORY:
		error = ENOTDIR;
		break;
	case EXT4_NAME_TOO_LONG:
		error = ENAMETOOLONG;
		break;
	case EXT4_READ_ONLY:
		error = EROFS;
		break;
	case EXT4_IS_DIRECTORY:
		error = EISDIR;
		break;
	case EXT4_UNSUPPORTED:
		error = ENOTSUP;
		break;
	case EXT4_INVALID_ARGUMENT:
	case EXT4_NOT_EXT4:
		error = EINVAL;
		break;
	case EXT4_RANGE:
		error = EOVERFLOW;
		break;
	case EXT4_STALE:
		error = ESTALE;
		break;
	case EXT4_NO_SPACE:
		error = ENOSPC;
		break;
	case EXT4_EXISTS:
		error = EEXIST;
		break;
	case EXT4_TOO_MANY_LINKS:
		error = EMLINK;
		break;
	case EXT4_NOT_EMPTY:
		error = ENOTEMPTY;
		break;
	case EXT4_PERMISSION_DENIED:
		error = EPERM;
		break;
	case EXT4_CORRUPT:
	case EXT4_RECOVERY_REQUIRED:
	case EXT4_IO:
	default:
		error = EIO;
		break;
	}
	return [NSError errorWithDomain:NSPOSIXErrorDomain
				   code:error
			       userInfo:@{
				       NSLocalizedDescriptionKey : @(ext4_result_string(result))
			       }];
}

static enum ext4_result
ext4_resource_read(void *context, uint64_t offset, void *buffer, size_t length)
{
	FSBlockDeviceResource *resource = (__bridge FSBlockDeviceResource *)context;
	NSError *error = nil;
	uint8_t *bounce;
	uint64_t alignment = resource.physicalBlockSize;
	uint64_t start;
	uint64_t total;
	size_t completed;

	if (alignment == 0 || alignment > SIZE_MAX || offset > INT64_MAX ||
	    length > (uint64_t)INT64_MAX - offset) {
		return EXT4_IO;
	}
	if (length == 0) {
		return EXT4_OK;
	}
	start = offset - offset % alignment;
	total = offset - start + length;
	if (total > SIZE_MAX - (alignment - 1)) {
		return EXT4_RANGE;
	}
	total = ((total + alignment - 1) / alignment) * alignment;
	if (start > resource.blockCount * resource.blockSize ||
	    total > resource.blockCount * resource.blockSize - start) {
		return EXT4_IO;
	}
	bounce = malloc((size_t)total);
	if (bounce == NULL) {
		return EXT4_NO_MEMORY;
	}
	completed = [resource readInto:bounce
			    startingAt:(off_t)start
				length:(size_t)total
				 error:&error];
	if (error == nil && completed == total) {
		memcpy(buffer, bounce + (offset - start), length);
	}
	free(bounce);
	return error == nil && completed == total ? EXT4_OK : EXT4_IO;
}

static void *
ext4_resource_allocate(void *context, size_t size)
{
	(void)context;
	return malloc(size);
}

static void
ext4_resource_release(void *context, void *allocation, size_t size)
{
	(void)context;
	(void)size;
	free(allocation);
}

static enum ext4_result
ext4_open_resource(FSResource *resource, struct ext4_fs **fs)
{
	FSBlockDeviceResource *block;
	struct ext4_environment environment;

	*fs = NULL;
	if (![resource isKindOfClass:FSBlockDeviceResource.class]) {
		return EXT4_UNSUPPORTED;
	}
	block = (FSBlockDeviceResource *)resource;
	if (block.blockSize == 0 || block.blockCount > UINT64_MAX / block.blockSize) {
		return EXT4_CORRUPT;
	}
	environment.context = (__bridge void *)block;
	environment.size_bytes = block.blockCount * block.blockSize;
	environment.read = ext4_resource_read;
	environment.allocate = ext4_resource_allocate;
	environment.release = ext4_resource_release;
	return ext4_mount(&environment, fs);
}

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

@interface Ext4Item : FSItem {
      @public
	struct ext4_inode inode;
}
@end

@implementation Ext4Item
@end

@interface Ext4Volume : FSVolume <FSVolumeOperations, FSVolumeReadWriteOperations> {
	struct ext4_fs *_fs;
	struct ext4_info _info;
	FSResource *_resource;
	NSLock *_itemLock;
	NSMapTable<NSNumber *, Ext4Item *> *_items;
}
- (instancetype)initWithResource:(FSResource *)resource filesystem:(struct ext4_fs *)fs;
- (NSError *)checkMountEligibility;
@end

@implementation Ext4Volume

- (instancetype)initWithResource:(FSResource *)resource filesystem:(struct ext4_fs *)fs
{
	struct ext4_info info;
	NSUUID *uuid;
	FSVolumeIdentifier *identifier;
	FSFileName *name;

	ext4_get_info(fs, &info);
	uuid = [[NSUUID alloc] initWithUUIDBytes:info.uuid];
	identifier = [[FSVolumeIdentifier alloc] initWithUUID:uuid];
	name = [FSFileName nameWithBytes:info.volume_name
				  length:strnlen(info.volume_name, EXT4_VOLUME_NAME_SIZE)];
	self = [super initWithVolumeID:identifier volumeName:name];
	if (self != nil) {
		_fs = fs;
		_info = info;
		_resource = resource;
		_itemLock = [[NSLock alloc] init];
		_items = [NSMapTable strongToWeakObjectsMapTable];
	}
	return self;
}

- (void)dealloc
{
	ext4_unmount(_fs);
}

- (Ext4Item *)itemForInode:(const struct ext4_inode *)inode
{
	Ext4Item *item;
	NSNumber *key = @(inode->number);

	[_itemLock lock];
	item = [_items objectForKey:key];
	if (item == nil) {
		item = [[Ext4Item alloc] init];
		item->inode = *inode;
		[_items setObject:item forKey:key];
	}
	[_itemLock unlock];
	return item;
}

- (FSItemAttributes *)attributesForInode:(const struct ext4_inode *)inode
{
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
	attributes.inhibitKernelOffloadedIO = YES;
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

- (uint64_t)maximumFileSize
{
	return (uint64_t)UINT32_MAX * _info.block_size;
}

- (FSMountOptions)requestedMountOptions
{
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

- (void)mountWithOptions:(FSTaskOptions *)options replyHandler:(void (^)(NSError *))reply
{
	(void)options;
	reply(nil);
}

- (void)unmountWithReplyHandler:(void (^)(void))reply
{
	reply();
}

- (void)synchronizeWithFlags:(FSSyncFlags)flags replyHandler:(void (^)(NSError *))reply
{
	(void)flags;
	/* The read-only implementation has no pending writes or dirty metadata. */
	reply(nil);
}

- (void)activateWithOptions:(FSTaskOptions *)options
	       replyHandler:(void (^)(FSItem *, NSError *))reply
{
	struct ext4_inode root;
	enum ext4_result error;

	(void)options;
	error = ext4_get_inode(_fs, EXT4_ROOT_INODE, &root);
	reply(error == EXT4_OK ? [self itemForInode:&root] : nil, ext4_error(error));
}

- (void)deactivateWithOptions:(FSDeactivateOptions)options replyHandler:(void (^)(NSError *))reply
{
	(void)options;
	reply(nil);
}

- (void)reclaimItem:(FSItem *)item replyHandler:(void (^)(NSError *))reply
{
	/* The weak identity table expires after the last concurrent holder exits. */
	(void)item;
	reply(nil);
}

- (NSError *)checkMountEligibility
{
	struct ext4_inode root;
	enum ext4_result error;

	error = ext4_get_inode(_fs, EXT4_ROOT_INODE, &root);
	return ext4_error(error);
}

- (void)lookupItemNamed:(FSFileName *)name
	    inDirectory:(FSItem *)directory
	   replyHandler:(void (^)(FSItem *, FSFileName *, NSError *))reply
{
	Ext4Item *parent = (Ext4Item *)directory;
	struct ext4_inode inode;
	NSData *bytes = name.data;
	enum ext4_result error;

	error = ext4_lookup(_fs, &parent->inode, bytes.bytes, bytes.length, &inode);
	reply(error == EXT4_OK ? [self itemForInode:&inode] : nil, error == EXT4_OK ? name : nil,
	    ext4_error(error));
}

- (void)getAttributes:(FSItemGetAttributesRequest *)desiredAttributes
	       ofItem:(FSItem *)item
	 replyHandler:(void (^)(FSItemAttributes *, NSError *))reply
{
	Ext4Item *owned = (Ext4Item *)item;

	(void)desiredAttributes;
	reply([self attributesForInode:&owned->inode], nil);
}

- (void)enumerateDirectory:(FSItem *)directory
	  startingAtCookie:(FSDirectoryCookie)cookie
		  verifier:(FSDirectoryVerifier)verifier
       providingAttributes:(FSItemGetAttributesRequest *)attributes
	       usingPacker:(FSDirectoryEntryPacker *)packer
	      replyHandler:(void (^)(FSDirectoryVerifier, NSError *))reply
{
	Ext4Item *parent = (Ext4Item *)directory;
	struct ext4_dir_entry entry;
	struct ext4_inode inode;
	FSItemAttributes *itemAttributes;
	FSFileName *name;
	uint64_t next = cookie;
	FSDirectoryVerifier current = (uint64_t)parent->inode.generation + 1;
	enum ext4_result error;

	if (verifier != FSDirectoryVerifierInitial && verifier != current) {
		reply(
		    current, [NSError errorWithDomain:NSPOSIXErrorDomain code:ESTALE userInfo:nil]);
		return;
	}
	while ((error = ext4_next_dir(_fs, &parent->inode, &next, &entry)) == EXT4_OK) {
		if (attributes != nil &&
		    ((entry.name_length == 1 && entry.name[0] == '.') ||
			(entry.name_length == 2 && entry.name[0] == '.' && entry.name[1] == '.'))) {
			continue;
		}
		error = ext4_get_inode(_fs, entry.inode, &inode);
		if (error != EXT4_OK) {
			break;
		}
		name = [FSFileName nameWithBytes:(const char *)entry.name length:entry.name_length];
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

- (void)readFromFile:(FSItem *)item
	      offset:(off_t)offset
	      length:(size_t)length
	  intoBuffer:(FSMutableFileDataBuffer *)buffer
	replyHandler:(void (^)(size_t, NSError *))reply
{
	Ext4Item *owned = (Ext4Item *)item;
	size_t completed = 0;
	enum ext4_result error;

	if (offset < 0 || length > buffer.length) {
		reply(0, ext4_error(EXT4_INVALID_ARGUMENT));
		return;
	}
	if ((owned->inode.mode & EXT4_MODE_TYPE) != EXT4_MODE_REGULAR) {
		reply(0, ext4_error(EXT4_INVALID_ARGUMENT));
		return;
	}
	error = ext4_read(
	    _fs, &owned->inode, (uint64_t)offset, buffer.mutableBytes, length, &completed);
	reply(completed, ext4_error(error));
}

- (void)readSymbolicLink:(FSItem *)item replyHandler:(void (^)(FSFileName *, NSError *))reply
{
	Ext4Item *owned = (Ext4Item *)item;
	NSMutableData *bytes;
	size_t completed = 0;
	enum ext4_result error;

	if ((owned->inode.mode & EXT4_MODE_TYPE) != EXT4_MODE_SYMLINK) {
		reply(nil, ext4_error(EXT4_INVALID_ARGUMENT));
		return;
	}
	if (owned->inode.size >= _info.block_size) {
		reply(nil, ext4_error(EXT4_UNSUPPORTED));
		return;
	}
	bytes = [NSMutableData dataWithLength:(NSUInteger)owned->inode.size];
	error = ext4_read(_fs, &owned->inode, 0, bytes.mutableBytes, bytes.length, &completed);
	if (error == EXT4_OK && completed != bytes.length) {
		error = EXT4_IO;
	}
	reply(error == EXT4_OK ? [FSFileName nameWithData:bytes] : nil, ext4_error(error));
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

- (void)writeContents:(NSData *)contents
	       toFile:(FSItem *)item
	     atOffset:(off_t)offset
	 replyHandler:(void (^)(size_t, NSError *))reply
{
	(void)contents;
	(void)item;
	(void)offset;
	reply(0, ext4_error(EXT4_READ_ONLY));
}

@end

@implementation Ext4FileSystem {
	Ext4Volume *_volume;
}

- (NSProgress *)startCheckWithTask:(FSTask *)task
			   options:(FSTaskOptions *)options
			     error:(NSError **)error
{
	Ext4Volume *volume;
	NSProgress *progress;

	/* A quick check admits only clean read-only media; it is not an fsck repair. */
	if (![options.taskOptions containsObject:@"-q"]) {
		if (error != NULL) {
			*error = [NSError
			    errorWithDomain:NSPOSIXErrorDomain
				       code:ENOTSUP
				   userInfo:@{
					   NSLocalizedDescriptionKey :
					       @"Only a quick read-only check is supported."
				   }];
		}
		return nil;
	}
	@synchronized(self) {
		volume = _volume;
	}
	if (volume == nil) {
		if (error != NULL) {
			*error = [NSError errorWithDomain:NSPOSIXErrorDomain
						     code:ENXIO
						 userInfo:nil];
		}
		return nil;
	}
	progress = [NSProgress progressWithTotalUnitCount:1];
	progress.cancellable = NO;
	dispatch_async(dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{
	  NSError *checkError;

	  checkError = [volume checkMountEligibility];
	  progress.completedUnitCount = 1;
	  [task didCompleteWithError:checkError];
	});
	return progress;
}

- (NSProgress *)startFormatWithTask:(FSTask *)task
			    options:(FSTaskOptions *)options
			      error:(NSError **)error
{
	(void)task;
	(void)options;
	if (error != NULL) {
		*error = ext4_error(EXT4_READ_ONLY);
	}
	return nil;
}

- (void)probeResource:(FSResource *)resource
	 replyHandler:(void (^)(FSProbeResult *, NSError *))reply
{
	struct ext4_fs *fs = NULL;
	struct ext4_info info;
	NSUUID *uuid;
	NSString *name;
	FSContainerIdentifier *identifier;
	enum ext4_result error;

	error = ext4_open_resource(resource, &fs);
	if (error != EXT4_OK) {
		reply(error == EXT4_NOT_EXT4 ? FSProbeResult.notRecognizedProbeResult : nil,
		    ext4_error(error == EXT4_NOT_EXT4 ? EXT4_OK : error));
		return;
	}
	ext4_get_info(fs, &info);
	name = [[NSString alloc] initWithBytes:info.volume_name
					length:strnlen(info.volume_name, EXT4_VOLUME_NAME_SIZE)
				      encoding:NSUTF8StringEncoding];
	if (name.length == 0) {
		name = @"ext4";
	}
	uuid = [[NSUUID alloc] initWithUUIDBytes:info.uuid];
	identifier = [[FSContainerIdentifier alloc] initWithUUID:uuid];
	ext4_unmount(fs);
	reply([FSProbeResult usableButLimitedProbeResultWithName:name containerID:identifier], nil);
}

- (void)loadResource:(FSResource *)resource
	     options:(FSTaskOptions *)options
	replyHandler:(void (^)(FSVolume *, NSError *))reply
{
	struct ext4_fs *fs = NULL;
	Ext4Volume *volume = nil;
	NSError *loadError = nil;
	enum ext4_result error;

	(void)options;
	@synchronized(self) {
		if (_volume != nil) {
			loadError = [NSError errorWithDomain:NSPOSIXErrorDomain
							code:EBUSY
						    userInfo:nil];
		} else {
			error = ext4_open_resource(resource, &fs);
			if (error == EXT4_OK) {
				volume = [[Ext4Volume alloc] initWithResource:resource
								   filesystem:fs];
				if (volume == nil) {
					ext4_unmount(fs);
					error = EXT4_NO_MEMORY;
				}
			}
			loadError = ext4_error(error);
			_volume = volume;
			self.containerStatus = loadError == nil
			    ? FSContainerStatus.ready
			    : [FSContainerStatus blockedWithStatus:loadError];
		}
	}
	reply(volume, loadError);
}

- (void)unloadResource:(FSResource *)resource
	       options:(FSTaskOptions *)options
	  replyHandler:(void (^)(NSError *))reply
{
	(void)resource;
	(void)options;
	@synchronized(self) {
		_volume = nil;
		self.containerStatus = [FSContainerStatus
		    notReadyWithStatus:[NSError errorWithDomain:NSPOSIXErrorDomain
							   code:ENXIO
						       userInfo:nil]];
	}
	reply(nil);
}

@end

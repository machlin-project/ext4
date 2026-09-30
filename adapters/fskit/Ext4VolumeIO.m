/* SPDX-License-Identifier: BSD-3-Clause */
#import "Ext4VolumeInternal.h"
#import "Ext4Support.h"
#include <errno.h>
#include <sys/xattr.h>
#include <stdlib.h>
#include <string.h>

@implementation Ext4Volume (FileIO)

- (BOOL)isOpenCloseInhibited
{
	return NO;
}

- (void)openItem:(FSItem *)item
       withModes:(FSVolumeOpenModes)modes
    replyHandler:(void (^)(NSError *))reply
{
	@synchronized(self) {
		enum ext4_result result = [self validateItem:(Ext4Item *)item];

		/* Refuse write access before the kernel admits cached writes or shared
		 * writable mappings, independently of the exported mount flags. */
		if (result == EXT4_OK && (modes & FSVolumeOpenModesWrite) != 0) {
			result = EXT4_READ_ONLY;
		}
		reply(ext4_error(result));
	}
}

- (void)closeItem:(FSItem *)item
     keepingModes:(FSVolumeOpenModes)modes
     replyHandler:(void (^)(NSError *))reply
{
	(void)modes;
	@synchronized(self) {
		/* FSItem lifetime, rather than the open count, owns the inode hold. */
		reply(ext4_error([self validateItem:(Ext4Item *)item]));
	}
}

/* The caller holds the volume monitor through completion. This is also used
 * by symbolic links, so every held read obeys the same retention policy. */
- (enum ext4_result)readItem:(Ext4Item *)item
		      offset:(uint64_t)offset
		      buffer:(void *)buffer
		      length:(size_t)length
		   completed:(size_t *)completed
{
	enum ext4_result result = ext4_read_held(item->hold, offset, buffer, length, completed);

	if (!_retainReadState) {
		ext4_drop_read_cache(item->hold);
	}
	return result;
}

- (void)readFromFile:(FSItem *)item
	      offset:(off_t)offset
	      length:(size_t)length
	  intoBuffer:(FSMutableFileDataBuffer *)buffer
	replyHandler:(void (^)(size_t, NSError *))reply
{
	@synchronized(self) {
		Ext4Item *owned = (Ext4Item *)item;
		size_t completed = 0;
		enum ext4_result error;

		if (offset < 0 || length > buffer.length) {
			reply(0, ext4_error(EXT4_INVALID_ARGUMENT));
			return;
		}
		error = [self validateItem:owned];
		if (error != EXT4_OK) {
			reply(0, ext4_error(error));
			return;
		}
		if ((owned->inode.mode & EXT4_MODE_TYPE) != EXT4_MODE_REGULAR) {
			reply(0, ext4_error(EXT4_INVALID_ARGUMENT));
			return;
		}
		error = [self readItem:owned
				offset:(uint64_t)offset
				buffer:buffer.mutableBytes
				length:length
			     completed:&completed];
		reply(completed, ext4_error(error));
	}
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

- (void)createFileNamed:(FSFileName *)name
	    inDirectory:(FSItem *)directory
	     attributes:(FSItemSetAttributesRequest *)attributes
		 packer:(FSExtentPacker *)packer
	   replyHandler:(void (^)(FSItem *, FSFileName *, NSError *))reply
{
	(void)packer;
	[self createItemNamed:name
			 type:FSItemTypeFile
		  inDirectory:directory
		   attributes:attributes
		 replyHandler:reply];
}

- (void)lookupItemNamed:(FSFileName *)name
	    inDirectory:(FSItem *)directory
		 packer:(FSExtentPacker *)packer
	   replyHandler:(void (^)(FSItem *, FSFileName *, NSError *))reply
{
	(void)packer;
	[self lookupItemNamed:name inDirectory:directory replyHandler:reply];
}

- (void)blockmapFile:(FSItem *)file
	      offset:(off_t)offset
	      length:(size_t)length
	       flags:(FSBlockmapFlags)flags
	 operationID:(FSOperationID)operationID
	      packer:(FSExtentPacker *)packer
	replyHandler:(void (^)(NSError *))reply
{
	@synchronized(self) {
		Ext4Item *item = (Ext4Item *)file;
		struct ext4_mapping mapping;
		enum ext4_result result = [self validateItem:item];
		uint64_t cursor;
		size_t remaining = length;
		size_t amount;
		BOOL more;

		(void)operationID;
		if (result == EXT4_OK && (flags & FSBlockmapFlagsWrite) != 0) {
			result = EXT4_READ_ONLY;
		}
		if (result == EXT4_OK && (offset < 0 || ![self canOffload:&item->inode])) {
			result = EXT4_INVALID_ARGUMENT;
		}
		if (result != EXT4_OK) {
			reply(ext4_error(result));
			return;
		}
		cursor = (uint64_t)offset;
		if (cursor > item->inode.size || remaining > item->inode.size - cursor) {
			reply(ext4_error(EXT4_RANGE));
			return;
		}
		while (remaining != 0) {
			amount = MIN(remaining, (size_t)UINT32_MAX);
			result = ext4_map_read_held(item->hold, cursor, amount, &mapping);
			if (result != EXT4_OK) {
				break;
			}
			if (mapping.length == 0 || mapping.device_offset > INT64_MAX) {
				result = EXT4_CORRUPT;
				break;
			}
			more = [packer packExtentWithResource:(FSBlockDeviceResource *)_resource
							 type:mapping.hole ? FSExtentTypeZeroFill
									   : FSExtentTypeData
						logicalOffset:(off_t)cursor
					       physicalOffset:(off_t)mapping.device_offset
						       length:mapping.length];
			cursor += mapping.length;
			remaining -= mapping.length;
			if (!more) {
				break;
			}
		}
		if (!_retainReadState) {
			ext4_drop_read_cache(item->hold);
		}
		reply(ext4_error(result));
	}
}

- (void)completeIOForFile:(FSItem *)file
		   offset:(off_t)offset
		   length:(size_t)length
		   status:(NSError *)status
		    flags:(FSCompleteIOFlags)flags
	      operationID:(FSOperationID)operationID
	     replyHandler:(void (^)(NSError *))reply
{
	(void)file;
	(void)offset;
	(void)length;
	(void)operationID;
	reply((flags & FSCompleteIOFlagsWrite) != 0 ? ext4_error(EXT4_READ_ONLY) : status);
}

/* macOS names map to Linux's user namespace without a second "user." prefix.
 * Linux ACL, trusted and security namespaces are never exported as user xattrs. */
- (void)getXattrNamed:(FSFileName *)name
	       ofItem:(FSItem *)file
	 replyHandler:(void (^)(NSData *, NSError *))reply
{
	@synchronized(self) {
		Ext4Item *item = (Ext4Item *)file;
		NSData *key = name.data;
		NSMutableData *value = nil;
		size_t size = 0;
		enum ext4_result result = [self validateItem:item];

		if (result == EXT4_OK &&
		    (key.length == 0 || key.length > XATTR_MAXNAMELEN ||
			memchr(key.bytes, 0, key.length) != NULL)) {
			result = EXT4_INVALID_ARGUMENT;
		}
		if (result == EXT4_OK) {
			result = ext4_get_xattr(_fs, item->inode.number, item->inode.generation,
			    EXT4_XATTR_USER, key.bytes, key.length, NULL, 0, &size);
		}
		if (result == EXT4_OK && size > (size_t)self.maximumXattrSize) {
			result = EXT4_RANGE;
		}
		if (result == EXT4_OK) {
			value = [NSMutableData dataWithLength:size];
			result = ext4_get_xattr(_fs, item->inode.number, item->inode.generation,
			    EXT4_XATTR_USER, key.bytes, key.length, value.mutableBytes, size,
			    &size);
		}
		reply(result == EXT4_OK ? value : nil,
		    result == EXT4_NOT_FOUND ? [NSError errorWithDomain:NSPOSIXErrorDomain
								   code:ENOATTR
							       userInfo:nil]
					     : ext4_error(result));
	}
}

- (void)listXattrsOfItem:(FSItem *)file
	    replyHandler:(void (^)(NSArray<FSFileName *> *, NSError *))reply
{
	@synchronized(self) {
		Ext4Item *item = (Ext4Item *)file;
		NSMutableArray<FSFileName *> *names = [NSMutableArray array];
		struct ext4_xattr_key *keys = NULL;
		size_t count = 0;
		size_t index;
		enum ext4_result result = [self validateItem:item];

		if (result == EXT4_OK) {
			result = ext4_list_xattrs(
			    _fs, item->inode.number, item->inode.generation, NULL, 0, &count);
		}
		if (result == EXT4_OK && count > EXT4_XATTR_MAX_CHANGES) {
			result = EXT4_RANGE;
		}
		if (result == EXT4_OK && count != 0) {
			keys = calloc(count, sizeof(*keys));
			result = keys == NULL ? EXT4_NO_MEMORY
					      : ext4_list_xattrs(_fs, item->inode.number,
						    item->inode.generation, keys, count, &count);
		}
		for (index = 0; result == EXT4_OK && index < count; index++) {
			if (keys[index].name_index == EXT4_XATTR_USER &&
			    keys[index].name_length <= XATTR_MAXNAMELEN) {
				[names addObject:[FSFileName
						     nameWithBytes:(const char *)keys[index].name
							    length:keys[index].name_length]];
			}
		}
		free(keys);
		reply(result == EXT4_OK ? names : nil, ext4_error(result));
	}
}

- (void)setXattrNamed:(FSFileName *)name
	       toData:(NSData *)value
	       onItem:(FSItem *)item
	       policy:(FSSetXattrPolicy)policy
	 replyHandler:(void (^)(NSError *))reply
{
	(void)name;
	(void)value;
	(void)item;
	(void)policy;
	reply(ext4_error(EXT4_READ_ONLY));
}

@end

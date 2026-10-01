/* SPDX-License-Identifier: BSD-3-Clause */
#import "Ext4VolumeInternal.h"
#import "Ext4Support.h"
#include <errno.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

static struct ext4_timestamp
ext4_current_time(void)
{
	struct timespec now;

	clock_gettime(CLOCK_REALTIME, &now);
	return (struct ext4_timestamp){ now.tv_sec, (uint32_t)now.tv_nsec };
}

static struct ext4_timestamp
ext4_item_time(struct timespec value)
{
	return (struct ext4_timestamp){ value.tv_sec, (uint32_t)value.tv_nsec };
}

static void
ext4_remove_capability(struct ext4_inode_update *update, struct ext4_xattr_change *change)
{
	static const uint8_t name[] = "capability";

	/* Resolve existence inside the same transaction as the data/owner change. */
	*change = (struct ext4_xattr_change){ .policy = EXT4_XATTR_REMOVE_IF_PRESENT,
		.name_index = EXT4_XATTR_SECURITY,
		.name = name,
		.name_length = sizeof(name) - 1 };
	update->fields |= EXT4_ATTR_XATTRS;
	update->xattrs = change;
	update->xattr_count = 1;
}

static FSItemAttribute
ext4_supplied_attributes(FSItemSetAttributesRequest *request)
{
	FSItemAttribute result = 0;
	FSItemAttribute attribute;

	for (attribute = FSItemAttributeType; attribute <= FSItemAttributeInhibitKernelOffloadedIO;
	    attribute <<= 1) {
		if ([request isValid:attribute]) {
			result |= attribute;
		}
	}
	return result;
}

static enum ext4_result
ext4_copy_attribute_request(FSItemSetAttributesRequest *request, struct ext4_inode_update *update)
{
	if ([request isValid:FSItemAttributeMode]) {
		if ((request.mode & ~(uint32_t)ALLPERMS) != 0) {
			return EXT4_INVALID_ARGUMENT;
		}
		update->fields |= EXT4_ATTR_PERMISSIONS;
		update->permissions = (uint16_t)request.mode;
	}
	if ([request isValid:FSItemAttributeUID]) {
		update->fields |= EXT4_ATTR_UID;
		update->uid = request.uid;
	}
	if ([request isValid:FSItemAttributeGID]) {
		update->fields |= EXT4_ATTR_GID;
		update->gid = request.gid;
	}
	if ([request isValid:FSItemAttributeAccessTime]) {
		update->fields |= EXT4_ATTR_ACCESS_TIME;
		update->access_time = ext4_item_time(request.accessTime);
	}
	if ([request isValid:FSItemAttributeModifyTime]) {
		update->fields |= EXT4_ATTR_MODIFY_TIME;
		update->modify_time = ext4_item_time(request.modifyTime);
	}
	if ([request isValid:FSItemAttributeChangeTime]) {
		update->fields |= EXT4_ATTR_CHANGE_TIME;
		update->change_time = ext4_item_time(request.changeTime);
	}
	if ([request isValid:FSItemAttributeBirthTime]) {
		update->fields |= EXT4_ATTR_BIRTH_TIME;
		update->birth_time = ext4_item_time(request.birthTime);
	}
	return EXT4_OK;
}

@implementation Ext4Volume (Mutation)

- (BOOL)isVolumeRenameInhibited
{
	return !self.writable;
}

- (void)setVolumeName:(FSFileName *)name
	 replyHandler:(void(NS_NOESCAPE ^)(FSFileName *, NSError *))reply
{
	@synchronized(self) {
		NSData *bytes = name.data;
		enum ext4_result error = [self ownerError];

		if (error == EXT4_OK) {
			error = !self.writable ? EXT4_READ_ONLY
			    : _writeClosed     ? EXT4_STALE
					       : EXT4_OK;
		}

		if (error == EXT4_OK && (bytes == nil || bytes.length == 0)) {
			error = EXT4_INVALID_ARGUMENT;
		}
		if (error == EXT4_OK) {
			error = ext4_set_volume_name(_fs, bytes.bytes, bytes.length);
		}
		if (error == EXT4_OK) {
			ext4_get_info(_fs, &_info);
			self.name = name;
		}
		reply(error == EXT4_OK ? name : nil, ext4_error(error));
	}
}

- (enum ext4_result)validateMutation:(Ext4Item *)item
{
	enum ext4_result error = [self ownerError];

	if (error != EXT4_OK) {
		return error;
	}

	/* Preserve EROFS even for an operation whose unused arguments are absent. */
	if (!self.writable) {
		return EXT4_READ_ONLY;
	}
	if (_writeClosed) {
		return EXT4_STALE;
	}
	error = [self validateItem:item];
	return error == EXT4_OK ? _lifetimeError : error;
}

- (void)changedDirectory:(Ext4Item *)directory
{
	if (++_directoryVersion == FSDirectoryVerifierInitial) {
		_directoryVersion++;
	}
	directory->directoryVersion = _directoryVersion;
}

- (void)writeUpdateForItem:(Ext4Item *)item
		    update:(struct ext4_inode_update *)update
		    change:(struct ext4_xattr_change *)change
{
	struct ext4_timestamp now = ext4_current_time();

	/* The 26.x callback has no caller credentials. Never infer privilege from
	 * the extension process; content changes conservatively remove set-ID bits. */
	*update = (struct ext4_inode_update){ .fields = EXT4_ATTR_PERMISSIONS |
		    EXT4_ATTR_MODIFY_TIME | EXT4_ATTR_CHANGE_TIME,
		.permissions = item->inode.mode & ALLPERMS & ~(S_ISUID | S_ISGID),
		.modify_time = now,
		.change_time = now };
	ext4_remove_capability(update, change);
}

- (void)createNamed:(FSFileName *)name
	       type:(FSItemType)type
	     parent:(FSItem *)directory
	 attributes:(FSItemSetAttributesRequest *)attributes
	       link:(FSFileName *)link
	      reply:(void(NS_NOESCAPE ^)(FSItem *, FSFileName *, NSError *))reply
{
	@synchronized(self) {
		Ext4Item *parent = (Ext4Item *)directory;
		Ext4Item *created = nil;
		struct ext4_inode inode = { 0 };
		struct ext4_special_file special = { 0 };
		struct ext4_timestamp now = ext4_current_time();
		struct ext4_inode_update update = { .fields = EXT4_ATTR_PERMISSIONS |
			    EXT4_ATTR_UID | EXT4_ATTR_GID | EXT4_ATTR_ACCESS_TIME |
			    EXT4_ATTR_MODIFY_TIME | EXT4_ATTR_CHANGE_TIME | EXT4_ATTR_XATTRS,
			.access_time = now,
			.modify_time = now,
			.change_time = now };
		NSData *bytes = name.data;
		NSData *target = link.data;
		size_t defaultACLSize = 0;
		FSItemAttribute supplied = ext4_supplied_attributes(attributes);
		enum ext4_result error = [self validateMutation:parent];

		if (error == EXT4_OK &&
		    ((supplied & (FSItemAttributeMode | FSItemAttributeUID | FSItemAttributeGID)) !=
			    (FSItemAttributeMode | FSItemAttributeUID | FSItemAttributeGID) ||
			([attributes isValid:FSItemAttributeSize] && attributes.size != 0) ||
			([attributes isValid:FSItemAttributeFlags] && attributes.flags != 0))) {
			error = EXT4_INVALID_ARGUMENT;
		}
		if (error == EXT4_OK) {
			error = ext4_get_xattr(_fs, parent->inode.number, parent->inode.generation,
			    EXT4_XATTR_POSIX_ACL_DEFAULT, NULL, 0, NULL, 0, &defaultACLSize);
			error = error == EXT4_NOT_FOUND ? EXT4_OK
			    : error == EXT4_OK		? EXT4_UNSUPPORTED
							: error;
		}
		if (error == EXT4_OK) {
			error = ext4_copy_attribute_request(attributes, &update);
		}
		if (error == EXT4_OK) {
			switch (type) {
			case FSItemTypeFile:
				error =
				    ext4_create(_fs, parent->inode.number, parent->inode.generation,
					bytes.bytes, bytes.length, &update, &now, &inode);
				break;
			case FSItemTypeDirectory:
				error =
				    ext4_mkdir(_fs, parent->inode.number, parent->inode.generation,
					bytes.bytes, bytes.length, &update, &now, &inode);
				break;
			case FSItemTypeSymlink:
				error = ext4_symlink(_fs, parent->inode.number,
				    parent->inode.generation, bytes.bytes, bytes.length,
				    target.bytes, target.length, &update, &now, &inode);
				break;
			case FSItemTypeFIFO:
			case FSItemTypeSocket:
				special.type =
				    type == FSItemTypeFIFO ? EXT4_FT_FIFO : EXT4_FT_SOCKET;
				error =
				    ext4_mknod(_fs, parent->inode.number, parent->inode.generation,
					bytes.bytes, bytes.length, &special, &update, &now, &inode);
				break;
			default:
				error = EXT4_UNSUPPORTED;
				break;
			}
		}
		if (error == EXT4_OK) {
			[self changedDirectory:parent];
			created = [self itemForInode:&inode error:&error];
			attributes.consumedAttributes = supplied &
			    (FSItemAttributeMode | FSItemAttributeUID | FSItemAttributeGID |
				FSItemAttributeAccessTime | FSItemAttributeModifyTime |
				FSItemAttributeChangeTime | FSItemAttributeBirthTime |
				FSItemAttributeSize | FSItemAttributeFlags);
		}
		reply(created, created != nil ? name : nil, ext4_error(error));
	}
}

- (void)linkItem:(FSItem *)file
	   named:(FSFileName *)name
     inDirectory:(FSItem *)directory
    replyHandler:(void(NS_NOESCAPE ^)(FSFileName *, NSError *))reply
{
	@synchronized(self) {
		Ext4Item *item = (Ext4Item *)file;
		Ext4Item *parent = (Ext4Item *)directory;
		struct ext4_inode inode = { 0 };
		struct ext4_timestamp now = ext4_current_time();
		NSData *bytes = name.data;
		enum ext4_result error = [self validateMutation:parent];

		if (error == EXT4_OK) {
			error = [self validateItem:item];
		}
		if (error == EXT4_OK) {
			error = ext4_link(_fs, parent->inode.number, parent->inode.generation,
			    bytes.bytes, bytes.length, item->inode.number, item->inode.generation,
			    &now, &inode);
		}
		if (error == EXT4_OK) {
			item->inode = inode;
			[self changedDirectory:parent];
		}
		reply(error == EXT4_OK ? name : nil, ext4_error(error));
	}
}

- (void)deleteItem:(FSItem *)file
	     named:(FSFileName *)name
     fromDirectory:(FSItem *)directory
      replyHandler:(void(NS_NOESCAPE ^)(NSError *))reply
{
	@synchronized(self) {
		Ext4Item *item = (Ext4Item *)file;
		Ext4Item *parent = (Ext4Item *)directory;
		struct ext4_inode inode = { 0 };
		struct ext4_timestamp now = ext4_current_time();
		NSData *bytes = name.data;
		enum ext4_result error = [self validateMutation:parent];

		if (error == EXT4_OK) {
			error = [self validateItem:item];
		}
		if (error == EXT4_OK) {
			if ((item->inode.mode & EXT4_MODE_TYPE) == EXT4_MODE_DIRECTORY) {
				error = ext4_rmdir(_fs, parent->inode.number,
				    parent->inode.generation, bytes.bytes, bytes.length,
				    item->inode.number, item->inode.generation, &now, &inode);
			} else {
				error = ext4_unlink(_fs, parent->inode.number,
				    parent->inode.generation, bytes.bytes, bytes.length,
				    item->inode.number, item->inode.generation, &now, &inode);
			}
		}
		if (error == EXT4_OK) {
			item->inode = inode;
			[self changedDirectory:parent];
		}
		reply(ext4_error(error));
	}
}

- (void)moveItem:(FSItem *)file
     inDirectory:(FSItem *)sourceDirectory
	   named:(FSFileName *)sourceName
       toNewName:(FSFileName *)destinationName
     inDirectory:(FSItem *)destinationDirectory
	overItem:(FSItem *)overFile
    replyHandler:(void(NS_NOESCAPE ^)(FSFileName *, NSError *))reply
{
	@synchronized(self) {
		Ext4Item *item = (Ext4Item *)file;
		Ext4Item *source = (Ext4Item *)sourceDirectory;
		Ext4Item *destination = (Ext4Item *)destinationDirectory;
		Ext4Item *over = (Ext4Item *)overFile;
		struct ext4_rename_entry oldEntry;
		struct ext4_rename_entry newEntry;
		struct ext4_inode inode = { 0 };
		struct ext4_timestamp now = ext4_current_time();
		NSData *oldBytes = sourceName.data;
		NSData *newBytes = destinationName.data;
		enum ext4_result error = [self validateMutation:source];

		if (error == EXT4_OK) {
			error = [self validateItem:destination];
		}
		if (error == EXT4_OK) {
			error = [self validateItem:item];
		}
		if (error == EXT4_OK && over != nil) {
			error = [self validateItem:over];
		}
		if (error == EXT4_OK) {
			oldEntry = (struct ext4_rename_entry){ source->inode.number,
				source->inode.generation, oldBytes.bytes, oldBytes.length,
				item->inode.number, item->inode.generation };
			newEntry = (struct ext4_rename_entry){ destination->inode.number,
				destination->inode.generation, newBytes.bytes, newBytes.length,
				over != nil ? over->inode.number : 0,
				over != nil ? over->inode.generation : 0 };
			error = ext4_rename(_fs, &oldEntry, &newEntry, 0, &now, &inode);
		}
		if (error == EXT4_OK) {
			item->inode = inode;
			[self changedDirectory:source];
			if (destination != source) {
				[self changedDirectory:destination];
			}
		}
		reply(error == EXT4_OK ? destinationName : nil, ext4_error(error));
	}
}

- (void)changeAttributes:(FSItemSetAttributesRequest *)request
		  onItem:(FSItem *)file
	    replyHandler:(void(NS_NOESCAPE ^)(FSItemAttributes *, NSError *))reply
{
	@synchronized(self) {
		Ext4Item *item = (Ext4Item *)file;
		struct ext4_inode inode = { 0 };
		struct ext4_inode_update update = { .fields =
							EXT4_ATTR_CHANGE_TIME | EXT4_ATTR_XATTRS,
			.change_time = ext4_current_time() };
		struct ext4_xattr_change change;
		FSItemAttribute supplied = ext4_supplied_attributes(request);
		FSItemAttribute supported = FSItemAttributeMode | FSItemAttributeUID |
		    FSItemAttributeGID | FSItemAttributeSize | FSItemAttributeAccessTime |
		    FSItemAttributeModifyTime | FSItemAttributeChangeTime |
		    FSItemAttributeBirthTime;
		BOOL truncate;
		enum ext4_result error = [self validateMutation:item];

		if (error == EXT4_OK &&
		    (supplied &
			(FSItemAttributeType | FSItemAttributeLinkCount | FSItemAttributeAllocSize |
			    FSItemAttributeFileID | FSItemAttributeParentID)) != 0) {
			error = EXT4_INVALID_ARGUMENT;
		}
		if (error == EXT4_OK && (item->inode.mode & EXT4_MODE_TYPE) != EXT4_MODE_REGULAR) {
			/* FSKit requires non-file sizes to be ignored, even in a request
			 * that also changes supported metadata. Do not consume this field. */
			supported &= ~FSItemAttributeSize;
		}
		if (error == EXT4_OK) {
			error = ext4_copy_attribute_request(request, &update);
			if (!item->inode.birth_time_valid) {
				/* Old inode formats cannot represent creation time. Leave it
				 * unconsumed so FSKit can handle the unsupported attribute. */
				supported &= ~FSItemAttributeBirthTime;
				update.fields &= ~EXT4_ATTR_BIRTH_TIME;
			}
		}
		if (error != EXT4_OK) {
			reply(nil, ext4_error(error));
			return;
		}
		if ((supplied & FSItemAttributeFlags) != 0) {
			uint32_t flags = 0;

			/* System flags preserve ext4's privileged immutable/append policy.
			 * FSKit authorizes the change before dispatching this callback. */
			if ((request.flags & ~(SF_IMMUTABLE | SF_APPEND | UF_NODUMP)) != 0 ||
			    (supplied & supported) != 0) {
				reply(nil, ext4_error(EXT4_UNSUPPORTED));
				return;
			}
			flags |= (request.flags & SF_IMMUTABLE) ? EXT4_INODE_IMMUTABLE : 0;
			flags |= (request.flags & SF_APPEND) ? EXT4_INODE_APPEND : 0;
			flags |= (request.flags & UF_NODUMP) ? EXT4_INODE_NODUMP : 0;
			error =
			    ext4_set_inode_flags(_fs, item->inode.number, item->inode.generation,
				EXT4_INODE_IMMUTABLE | EXT4_INODE_APPEND | EXT4_INODE_NODUMP, flags,
				&update.change_time, &inode);
			if (error == EXT4_OK) {
				item->inode = inode;
				request.consumedAttributes = FSItemAttributeFlags;
			}
			reply(error == EXT4_OK ? [self attributesForInode:&item->inode] : nil,
			    ext4_error(error));
			return;
		}
		if ((supplied & supported) == 0) {
			/* Leave unsupported fields unconsumed without changing ctime. */
			reply([self attributesForInode:&item->inode], nil);
			return;
		}
		truncate = (supplied & supported & FSItemAttributeSize) != 0;
		if (truncate || (supplied & (FSItemAttributeUID | FSItemAttributeGID)) != 0) {
			ext4_remove_capability(&update, &change);
			if ((supplied & FSItemAttributeMode) == 0) {
				update.fields |= EXT4_ATTR_PERMISSIONS;
				update.permissions =
				    item->inode.mode & ALLPERMS & ~(S_ISUID | S_ISGID);
			}
		}
		if (truncate) {
			/* The size operation atomically carries mode and timestamps. Other
			 * requested metadata cannot be split into a second transaction. */
			if ((supplied &
				(FSItemAttributeUID | FSItemAttributeGID |
				    FSItemAttributeAccessTime | FSItemAttributeBirthTime)) != 0) {
				error = EXT4_UNSUPPORTED;
			}
			if ((supplied & FSItemAttributeModifyTime) == 0) {
				update.fields |= EXT4_ATTR_MODIFY_TIME;
				update.modify_time = update.change_time;
			}
		}
		if (error == EXT4_OK) {
			error = truncate
			    ? ext4_truncate(_fs, item->inode.number, item->inode.generation,
				  request.size, &update, &inode)
			    : ext4_set_attributes(
				  _fs, item->inode.number, item->inode.generation, &update, &inode);
		}
		if (error == EXT4_OK) {
			item->inode = inode;
			request.consumedAttributes = supplied & supported;
		}
		reply(error == EXT4_OK ? [self attributesForInode:&item->inode] : nil,
		    ext4_error(error));
	}
}

- (BOOL)isPreallocateInhibited
{
	return !self.writable;
}

- (void)preallocateSpaceForItem:(FSItem *)file
		       atOffset:(off_t)offset
			 length:(size_t)length
			  flags:(FSPreallocateFlags)flags
		   replyHandler:(void(NS_NOESCAPE ^)(size_t, NSError *))reply
{
	@synchronized(self) {
		Ext4Item *item = (Ext4Item *)file;
		struct ext4_inode_update update;
		struct ext4_xattr_change change;
		uint64_t completed = 0;
		uint64_t start = (uint64_t)offset;
		enum ext4_result error = [self validateMutation:item];

		if (error == EXT4_OK && offset < 0 && (flags & FSPreallocateFlagsFromEOF) == 0) {
			error = EXT4_INVALID_ARGUMENT;
		}
		/* Contiguous/all-or-nothing need explicit core reservation contracts. */
		if (error == EXT4_OK &&
		    (flags & ~(FSPreallocateFlagsPersist | FSPreallocateFlagsFromEOF)) != 0) {
			error = EXT4_UNSUPPORTED;
		}
		if (error == EXT4_OK && (flags & FSPreallocateFlagsFromEOF) != 0) {
			error = ext4_allocation_end(_fs, &item->inode, &start);
		}
		if (error == EXT4_OK) {
			[self writeUpdateForItem:item update:&update change:&change];
			error = ext4_fallocate(_fs, item->inode.number, item->inode.generation,
			    start, length, EXT4_FALLOC_KEEP_SIZE, &update, &completed);
		}
		/* Allocation shortage can leave a usable reservation. Device, journal
		 * and format failures must remain errors even after earlier checkpoints. */
		if (completed != 0 && (error == EXT4_NO_SPACE || error == EXT4_QUOTA_EXCEEDED)) {
			error = EXT4_OK;
		}
		reply((size_t)completed, ext4_error(error));
	}
}

@end

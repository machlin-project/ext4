/* SPDX-License-Identifier: BSD-3-Clause */
#import "../adapters/fskit/Ext4ResourceIO.h"
#import "../adapters/fskit/Ext4VolumeInternal.h"
#include <assert.h>
#include <errno.h>
#include <stdatomic.h>
#include <string.h>
#include <sys/stat.h>

/* A transaction starting from a clean volume includes the recovery-marker barrier. */
enum mutation_barrier {
	MutationRecoveryMarker = 1,
	MutationJournalStart,
	MutationJournalData,
	MutationJournalCommit,
	MutationHomeBlocks,
	MutationJournalReset
};

enum security_mutation {
	SecurityWrite,
	SecurityTruncate,
	SecurityOwner,
	SecurityGroup,
	SecurityPreallocate,
	SecurityMutationCount
};

/* FSTaskOptions has no public initializer. The adapter only reads taskOptions. */
@interface MutationOptions : NSObject
@property NSArray<NSString *> *taskOptions;
@end
@implementation MutationOptions
@end

@interface WritableImage : NSObject <Ext4BlockWriter, Ext4PersistenceBarrier> {
	atomic_uint _active;
}
@property NSMutableData *bytes;
@property NSUInteger reads;
@property NSUInteger writes;
@property NSUInteger barriers;
@property BOOL failWrite;
@property BOOL failBarrier;
@property NSUInteger failBarrierAt;
@end

@implementation WritableImage

- (uint64_t)blockSize
{
	return 512;
}

- (uint64_t)physicalBlockSize
{
	return 512;
}

- (uint64_t)blockCount
{
	return self.bytes.length / 512;
}

- (size_t)readInto:(void *)buffer
	startingAt:(off_t)offset
	    length:(size_t)length
	     error:(NSError **)error
{
	(void)error;
	assert(atomic_fetch_add(&_active, 1) == 0);
	assert(offset >= 0 && (uint64_t)offset + length <= self.bytes.length);
	self.reads++;
	memcpy(buffer, (const uint8_t *)self.bytes.bytes + offset, length);
	assert(atomic_fetch_sub(&_active, 1) == 1);
	return length;
}

- (size_t)writeFrom:(void *)buffer
	 startingAt:(off_t)offset
	     length:(size_t)length
	      error:(NSError **)error
{
	assert(atomic_fetch_add(&_active, 1) == 0);
	assert(offset >= 0 && (uint64_t)offset + length <= self.bytes.length);
	self.writes++;
	if (!self.failWrite) {
		memcpy((uint8_t *)self.bytes.mutableBytes + offset, buffer, length);
	} else {
		*error = [NSError errorWithDomain:NSPOSIXErrorDomain code:EIO userInfo:nil];
	}
	assert(atomic_fetch_sub(&_active, 1) == 1);
	return self.failWrite ? 0 : length;
}

- (BOOL)synchronizeWithError:(NSError **)error
{
	self.barriers++;
	if (self.failBarrier || self.barriers == self.failBarrierAt) {
		*error = [NSError errorWithDomain:NSPOSIXErrorDomain code:EIO userInfo:nil];
		return NO;
	}
	return YES;
}

@end

static FSFileName *
name(NSString *value)
{
	return [FSFileName nameWithString:value];
}

static Ext4Volume *
open_volume_class(WritableImage *image, BOOL writable, struct ext4_fs **core, Class volumeClass)
{
	Ext4ResourceIO *io = [[Ext4ResourceIO alloc] initWithReader:image];
	struct ext4_fs *fs = NULL;
	Ext4Volume *volume;

	assert(io != nil);
	if (writable) {
		[io enableWritesWithBarrier:image deviceName:@"memory"];
		assert([io openWritable:&fs] == EXT4_OK);
	} else {
		assert([io open:&fs] == EXT4_OK);
	}
	volume = [[volumeClass alloc] initWithResource:nil
					    filesystem:fs
					 resourceOwner:io
						crypto:NULL
					      writable:writable];
	assert(volume != nil);
	if (core != NULL) {
		*core = fs;
	}
	return volume;
}

static Ext4LegacyVolume *
open_volume_with_core(WritableImage *image, BOOL writable, struct ext4_fs **core)
{
	return (Ext4LegacyVolume *)open_volume_class(image, writable, core, Ext4LegacyVolume.class);
}

static Ext4LegacyVolume *
open_volume(WritableImage *image, BOOL writable)
{
	return open_volume_with_core(image, writable, NULL);
}

static FSItem *
root_item(Ext4Volume *volume)
{
	__block FSItem *root = nil;
	MutationOptions *options = [MutationOptions new];

	options.taskOptions = @[];

	[volume activateWithOptions:(FSTaskOptions *)options
		       replyHandler:^(FSItem *item, NSError *error) {
			 assert(error == nil && item != nil);
			 root = item;
		       }];
	return root;
}

static FSItem *
create_item(Ext4Volume *volume, FSItem *parent, NSString *filename, FSItemType type)
{
	FSItemSetAttributesRequest *attributes = [FSItemSetAttributesRequest new];
	__block FSItem *created = nil;

	attributes.mode = type == FSItemTypeDirectory ? 0750 : 06740;
	attributes.uid = 501;
	attributes.gid = 20;
	[volume createItemNamed:name(filename)
			   type:type
		    inDirectory:parent
		     attributes:attributes
		   replyHandler:^(FSItem *item, FSFileName *actualName, NSError *error) {
		     assert(error == nil && item != nil &&
			 [actualName.data isEqualToData:name(filename).data]);
		     created = item;
		   }];
	assert([attributes wasAttributeConsumed:FSItemAttributeMode]);
	return created;
}

static FSItem *
lookup(Ext4LegacyVolume *volume, FSItem *parent, NSString *filename)
{
	__block FSItem *found = nil;

	[volume lookupItemNamed:name(filename)
		    inDirectory:parent
		   replyHandler:^(FSItem *item, FSFileName *actualName, NSError *error) {
		     (void)actualName;
		     assert(error == nil && item != nil);
		     found = item;
		   }];
	return found;
}

static FSItemAttributes *
attributes(Ext4Volume *volume, FSItem *item)
{
	__block FSItemAttributes *result = nil;
	FSItemGetAttributesRequest *request = [FSItemGetAttributesRequest new];

	[volume getAttributes:request
		       ofItem:item
		 replyHandler:^(FSItemAttributes *value, NSError *error) {
		   assert(error == nil && value != nil);
		   result = value;
		 }];
	return result;
}

static void
write_bytes(Ext4LegacyVolume *volume, FSItem *file, NSData *data, off_t offset)
{
	[volume writeContents:data
		       toFile:file
		     atOffset:offset
		 replyHandler:^(size_t size, NSError *error) {
		   assert(error == nil && size == data.length);
		 }];
}

static void
check_bytes(Ext4LegacyVolume *volume, FSItem *file, NSData *expected)
{
	NSMutableData *actual = [NSMutableData dataWithLength:expected.length];
	Ext4Item *item = (Ext4Item *)file;
	size_t completed = 0;

	assert([volume validateItem:item] == EXT4_OK);
	assert([volume readItem:item
			 offset:0
			 buffer:actual.mutableBytes
			 length:actual.length
		      completed:&completed] == EXT4_OK);
	assert(completed == expected.length && [actual isEqualToData:expected]);
}

static void
set_size(Ext4Volume *volume, FSItem *file, uint64_t size)
{
	FSItemSetAttributesRequest *request = [FSItemSetAttributesRequest new];

	request.size = size;
	[volume setAttributes:request
		       onItem:file
		 replyHandler:^(FSItemAttributes *value, NSError *error) {
		   assert(error == nil && value.size == size);
		 }];
	assert([request wasAttributeConsumed:FSItemAttributeSize]);
}

static void
sync_volume(Ext4Volume *volume)
{
	[volume synchronizeWithFlags:0
			replyHandler:^(NSError *error) {
			  assert(error == nil);
			}];
}

static void
check_mutations(WritableImage *image)
{
	Ext4LegacyVolume *volume = open_volume(image, YES);
	FSItem *root = root_item(volume);
	FSItem *directory = create_item(volume, root, @"fskit-written", FSItemTypeDirectory);
	FSItem *file = create_item(volume, directory, @"payload", FSItemTypeFile);
	FSItem *replacement;
	FSItem *special;
	FSItemSetAttributesRequest *request;
	NSMutableData *expected = [NSMutableData dataWithLength:9001];
	NSData *value = [@"attribute bytes" dataUsingEncoding:NSUTF8StringEncoding];
	uint8_t *bytes = expected.mutableBytes;
	NSUInteger index;
	FSDirectoryVerifier verifier = ((Ext4Item *)directory)->directoryVersion;
	FSStatFSResult *statistics = volume.volumeStatistics;
	struct ext4_info info;
	Ext4ResourceIO *inspector = [[Ext4ResourceIO alloc] initWithReader:image];

	assert([inspector inspect:&info] == EXT4_OK);
	[volume setVolumeName:name(@"Machlin ext4")
		 replyHandler:^(FSFileName *actual, NSError *error) {
		   assert(error == nil && [actual.data isEqualToData:name(@"Machlin ext4").data]);
		 }];
	assert([volume.name.data isEqualToData:name(@"Machlin ext4").data]);
	[volume setVolumeName:name(@"label exceeds sixteen bytes")
		 replyHandler:^(FSFileName *actual, NSError *error) {
		   assert(actual == nil && error.code == ENAMETOOLONG);
		 }];
	assert(statistics.freeBlocks == info.free_blocks);
	assert(statistics.availableBlocks == info.free_blocks - info.reserved_blocks);
	special = create_item(volume, directory, @"fifo", FSItemTypeFIFO);
	assert(attributes(volume, special).type == FSItemTypeFIFO);
	special = create_item(volume, directory, @"socket", FSItemTypeSocket);
	assert(attributes(volume, special).type == FSItemTypeSocket);
	request = [FSItemSetAttributesRequest new];
	request.size = 1;
	[volume setAttributes:request
		       onItem:directory
		 replyHandler:^(FSItemAttributes *result, NSError *error) {
		   assert(error == nil && result.type == FSItemTypeDirectory && result.size != 1);
		 }];
	assert(![request wasAttributeConsumed:FSItemAttributeSize]);
	request = [FSItemSetAttributesRequest new];
	request.size = 1;
	request.mode = 0750;
	[volume setAttributes:request
		       onItem:directory
		 replyHandler:^(FSItemAttributes *result, NSError *error) {
		   assert(error == nil && result.mode == 0750 && result.size != 1);
		 }];
	assert(![request wasAttributeConsumed:FSItemAttributeSize]);
	assert([request wasAttributeConsumed:FSItemAttributeMode]);

	for (index = 0; index < expected.length; index++) {
		bytes[index] = (uint8_t)(index * 17 + 3);
	}
	write_bytes(volume, file, expected, 0);
	check_bytes(volume, file, expected);
	assert(attributes(volume, file).mode == 0740);
	assert(attributes(volume, file).inhibitKernelOffloadedIO);
	[volume createLinkToItem:file
			   named:name(@"hardlink")
		     inDirectory:directory
		    replyHandler:^(FSFileName *actualName, NSError *error) {
		      assert(error == nil && actualName != nil);
		    }];
	assert(attributes(volume, file).linkCount == 2);
	assert(((Ext4Item *)directory)->directoryVersion != verifier);
	assert(lookup(volume, directory, @"hardlink") == file);
	request = [FSItemSetAttributesRequest new];
	request.mode = 0777;
	request.uid = 501;
	request.gid = 20;
	[volume createSymbolicLinkNamed:name(@"symlink")
			    inDirectory:directory
			     attributes:request
			   linkContents:name(@"payload")
			   replyHandler:^(FSItem *item, FSFileName *actualName, NSError *error) {
			     assert(error == nil && item != nil && actualName != nil);
			   }];
	[volume readSymbolicLink:lookup(volume, directory, @"symlink")
		    replyHandler:^(FSFileName *target, NSError *error) {
		      assert(error == nil && [target.data isEqualToData:name(@"payload").data]);
		    }];
	request = [FSItemSetAttributesRequest new];
	request.size = 0;
	[volume setAttributes:request
		       onItem:lookup(volume, directory, @"symlink")
		 replyHandler:^(FSItemAttributes *result, NSError *error) {
		   assert(error == nil && result.type == FSItemTypeSymlink && result.size == 7);
		 }];
	assert(![request wasAttributeConsumed:FSItemAttributeSize]);
	[volume setXattrNamed:name(@"test.attribute")
		       toData:value
		       onItem:file
		       policy:FSSetXattrPolicyMustCreate
		 replyHandler:^(NSError *error) {
		   assert(error == nil);
		 }];
	[volume setXattrNamed:name(@"test.attribute")
		       toData:value
		       onItem:file
		       policy:FSSetXattrPolicyMustCreate
		 replyHandler:^(NSError *error) {
		   assert(error.code == EEXIST);
		 }];
	[volume getXattrNamed:name(@"test.attribute")
		       ofItem:file
		 replyHandler:^(NSData *result, NSError *error) {
		   assert(error == nil && [result isEqualToData:value]);
		 }];
	[volume setXattrNamed:name(@"test.attribute")
		       toData:nil
		       onItem:file
		       policy:FSSetXattrPolicyDelete
		 replyHandler:^(NSError *error) {
		   assert(error == nil);
		 }];
	[volume setXattrNamed:name(@"test.attribute")
		       toData:value
		       onItem:file
		       policy:FSSetXattrPolicyMustReplace
		 replyHandler:^(NSError *error) {
		   assert(error.code == ENOATTR);
		 }];
	set_size(volume, file, 1025);
	expected.length = 1025;
	set_size(volume, file, 20000);
	expected.length = 20000;
	check_bytes(volume, file, expected);
	[volume preallocateSpaceForItem:file
			       atOffset:0
				 length:32768
				  flags:FSPreallocateFlagsFromEOF | FSPreallocateFlagsPersist
			   replyHandler:^(size_t allocated, NSError *error) {
			     assert(error == nil && allocated == 32768);
			   }];
	assert(attributes(volume, file).size == 20000);
	assert(attributes(volume, file).allocSize >= 32768);
	check_bytes(volume, file, expected);
	request = [FSItemSetAttributesRequest new];
	request.flags = SF_IMMUTABLE;
	[volume setAttributes:request
		       onItem:file
		 replyHandler:^(FSItemAttributes *result, NSError *error) {
		   assert(error == nil && result.flags == SF_IMMUTABLE);
		 }];
	[volume writeContents:value
		       toFile:file
		     atOffset:0
		 replyHandler:^(size_t size, NSError *error) {
		   assert(size == 0 && error.code == EPERM);
		 }];
	request = [FSItemSetAttributesRequest new];
	request.flags = 0;
	[volume setAttributes:request
		       onItem:file
		 replyHandler:^(FSItemAttributes *result, NSError *error) {
		   assert(error == nil && result.flags == 0);
		 }];
	replacement = create_item(volume, directory, @"replacement", FSItemTypeFile);
	write_bytes(volume, replacement, value, 0);
	[volume renameItem:file
	       inDirectory:directory
		     named:name(@"payload")
		 toNewName:name(@"replacement")
	       inDirectory:directory
		  overItem:replacement
	      replyHandler:^(FSFileName *actualName, NSError *error) {
		assert(error == nil && actualName != nil);
	      }];
	check_bytes(volume, replacement, value);
	assert(attributes(volume, replacement).linkCount == 0);
	[volume deactivateItem:replacement
		  replyHandler:^(NSError *error) {
		    assert(error == nil);
		  }];
	assert(((Ext4Item *)replacement)->hold == NULL);
	[volume removeItem:file
		     named:name(@"hardlink")
	     fromDirectory:directory
	      replyHandler:^(NSError *error) {
		assert(error == nil);
	      }];
	assert(attributes(volume, file).linkCount == 1);
	dispatch_apply(
	    16, dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^(size_t worker) {
	      @autoreleasepool {
		      NSData *part = [NSData dataWithBytes:&worker length:sizeof(worker)];

		      write_bytes(volume, file, part, (off_t)(worker * sizeof(worker)));
	      }
	    });
	for (index = 0; index < 16; index++) {
		memcpy((uint8_t *)expected.mutableBytes + index * sizeof(index), &index,
		    sizeof(index));
	}
	check_bytes(volume, file, expected);
	sync_volume(volume);
	assert(image.writes != 0 && image.barriers != 0);
	[volume unmountWithReplyHandler:^{
	}];
	index = image.writes;
	assert([volume finishUnloadedResource] == nil && image.writes == index);
	assert([volume validateItem:(Ext4Item *)file] == EXT4_STALE);
	/* FSKit may retain items beyond unmount. A late release must not perform I/O. */
	[volume releaseHold:((Ext4Item *)file)->hold];
	assert(image.writes == index);
	[volume invalidate];
}

static void
check_security_mutations(NSData *fixture)
{
	static const uint8_t capability[] = "capability";
	static const uint8_t marker[] = "preserved";
	static const uint8_t value[] = "opaque security metadata";
	static const char *const operations[] = { "write", "truncate", "owner", "group",
		"preallocate" };
	WritableImage *image;
	Ext4LegacyVolume *volume;
	Ext4Item *file;
	FSItemSetAttributesRequest *request;
	struct ext4_fs *fs;
	struct ext4_inode inode;
	struct ext4_xattr_change changes[2];
	struct ext4_inode_update update;
	uint8_t returned[sizeof(value)];
	size_t size;
	NSUInteger reads;
	unsigned int state;
	enum security_mutation operation;

	for (operation = SecurityWrite; operation < SecurityMutationCount; operation++) {
		for (state = 0; state < 3; state++) {
			image = [WritableImage new];
			image.bytes = [fixture mutableCopy];
			volume = open_volume_with_core(image, YES, &fs);
			file = (Ext4Item *)create_item(
			    volume, root_item(volume), @"security-policy", FSItemTypeFile);
			changes[0] = (struct ext4_xattr_change){ .policy = EXT4_XATTR_CREATE,
				.name_index = EXT4_XATTR_USER,
				.name = marker,
				.name_length = sizeof(marker) - 1,
				.value = value,
				.value_size = sizeof(value) };
			changes[1] = changes[0];
			changes[1].name_index = EXT4_XATTR_SECURITY;
			changes[1].name = capability;
			changes[1].name_length = sizeof(capability) - 1;
			update = (struct ext4_inode_update){ .fields = EXT4_ATTR_XATTRS |
				    EXT4_ATTR_CHANGE_TIME,
				.change_time = file->inode.change_time,
				.xattrs = changes,
				.xattr_count = state };
			assert(ext4_set_attributes(fs, file->inode.number, file->inode.generation,
				   &update, &inode) == EXT4_OK);
			request = [FSItemSetAttributesRequest new];
			reads = image.reads;
			switch (operation) {
			case SecurityWrite:
				write_bytes(volume, file,
				    [NSData dataWithBytes:value length:sizeof(value)], 0);
				break;
			case SecurityTruncate:
				set_size(volume, file, sizeof(value));
				break;
			case SecurityOwner:
			case SecurityGroup:
				if (operation == SecurityOwner) {
					request.uid = 502;
				} else {
					request.gid = 21;
				}
				[volume setAttributes:request
					       onItem:file
					 replyHandler:^(FSItemAttributes *result, NSError *error) {
					   assert(error == nil && result != nil);
					 }];
				break;
			case SecurityPreallocate:
				[volume
				    preallocateSpaceForItem:file
						   atOffset:0
						     length:4096
						      flags:FSPreallocateFlagsPersist
					       replyHandler:^(size_t allocated, NSError *error) {
						 assert(error == nil && allocated == 4096);
					       }];
				break;
			case SecurityMutationCount:
				assert(false);
			}
			printf("FSKit security mutation: operation=%s xattr-state=%u reads=%lu\n",
			    operations[operation], state, (unsigned long)(image.reads - reads));
			assert(ext4_get_inode(fs, file->inode.number, &inode) == EXT4_OK);
			assert((inode.mode & ALLPERMS) == 0740);
			assert(inode.uid == (operation == SecurityOwner ? 502 : 501));
			assert(inode.gid == (operation == SecurityGroup ? 21 : 20));
			assert(ext4_get_xattr(fs, inode.number, inode.generation,
				   EXT4_XATTR_SECURITY, capability, sizeof(capability) - 1, NULL, 0,
				   &size) == EXT4_NOT_FOUND);
			if (state != 0) {
				assert(ext4_get_xattr(fs, inode.number, inode.generation,
					   EXT4_XATTR_USER, marker, sizeof(marker) - 1, returned,
					   sizeof(returned), &size) == EXT4_OK);
				assert(size == sizeof(value) && memcmp(returned, value, size) == 0);
			}
			sync_volume(volume);
			[volume invalidate];
		}
	}
	puts("PASS FSKit security transitions: set-ID and optional capabilities removed, unrelated "
	     "attributes preserved");
}

static void
check_remount(WritableImage *image)
{
	Ext4LegacyVolume *volume = open_volume(image, NO);
	FSItem *directory = lookup(volume, root_item(volume), @"fskit-written");
	FSItem *file = lookup(volume, directory, @"replacement");
	NSUInteger writes = image.writes;
	NSMutableData *expected = [NSMutableData dataWithLength:20000];
	uint8_t *bytes = expected.mutableBytes;
	NSUInteger index;

	assert([volume.name.data isEqualToData:name(@"Machlin ext4").data]);
	[volume setVolumeName:name(@"read-only")
		 replyHandler:^(FSFileName *actual, NSError *error) {
		   assert(actual == nil && error.code == EROFS);
		 }];
	for (index = 0; index < 1025; index++) {
		bytes[index] = (uint8_t)(index * 17 + 3);
	}
	for (index = 0; index < 16; index++) {
		memcpy(bytes + index * sizeof(index), &index, sizeof(index));
	}
	check_bytes(volume, file, expected);
	assert(attributes(volume, file).linkCount == 1 && attributes(volume, file).uid == 501);
	[volume writeContents:expected
		       toFile:file
		     atOffset:0
		 replyHandler:^(size_t size, NSError *error) {
		   assert(size == 0 && error.code == EROFS);
		 }];
	assert(image.writes == writes);
}

static void
check_failed_mutation(NSData *fixture, NSUInteger barrier, BOOL renameVolume, NSString *exportPath)
{
	WritableImage *image = [WritableImage new];
	Ext4LegacyVolume *volume;
	FSItem *file;
	NSData *data = [@"failure" dataUsingEncoding:NSUTF8StringEncoding];
	NSData *interrupted;
	Ext4ResourceIO *io;
	struct ext4_fs *fs = NULL;
	struct ext4_info info;
	struct ext4_info original;
	struct ext4_recovery_report report = { 0 };
	NSUInteger writes;
	enum ext4_result error;

	image.bytes = [fixture mutableCopy];
	volume = open_volume(image, YES);
	io = [[Ext4ResourceIO alloc] initWithReader:image];
	assert([io inspect:&original] == EXT4_OK);
	file = create_item(volume, root_item(volume), @"failure", FSItemTypeFile);
	sync_volume(volume);
	image.failWrite = barrier == 0;
	image.failBarrierAt = barrier != 0 ? image.barriers + barrier : 0;
	if (renameVolume) {
		[volume setVolumeName:name(@"recovered-label")
			 replyHandler:^(FSFileName *actual, NSError *failure) {
			   assert(actual == nil && failure != nil);
			 }];
	} else {
		[volume writeContents:data
			       toFile:file
			     atOffset:0
			 replyHandler:^(size_t size, NSError *error) {
			   assert(size == 0 && error != nil);
			 }];
	}
	[volume synchronizeWithFlags:0
			replyHandler:^(NSError *error) {
			  assert(error != nil);
			}];
	/* Discard the failed owner without flushing, as after an extension crash. */
	[volume invalidate];
	image.failWrite = NO;
	image.failBarrierAt = 0;
	interrupted = image.bytes.copy;
	writes = image.writes;
	io = [[Ext4ResourceIO alloc] initWithReader:image];
	error = [io open:&fs];
	assert(error == EXT4_OK || error == EXT4_RECOVERY_REQUIRED);
	ext4_unmount(fs);
	assert([io inspect:&info] == EXT4_OK && info.blocks != 0);
	assert([io recover:&report] == EXT4_READ_ONLY);
	assert(image.writes == writes && [image.bytes isEqualToData:interrupted]);
	if (barrier == MutationJournalCommit && !renameVolume) {
		/* The commit record exists, but no home block has been checkpointed. */
		assert(error == EXT4_RECOVERY_REQUIRED);
		if (exportPath != nil) {
			assert([interrupted writeToFile:[exportPath stringByAppendingPathComponent:
								@"fskit-recovery-required.img"]
					     atomically:YES]);
		}
	}
	[io enableWritesWithBarrier:image deviceName:@"memory"];
	assert([io recover:&report] == EXT4_OK);
	if (barrier == MutationJournalCommit) {
		assert(report.transactions == 1 && report.replayed_blocks != 0);
		if (exportPath != nil) {
			assert([image.bytes
			    writeToFile:[exportPath
					    stringByAppendingPathComponent:@"fskit-recovered.img"]
			     atomically:YES]);
		}
	}
	volume = open_volume(image, NO);
	if (renameVolume) {
		assert([io inspect:&info] == EXT4_OK);
		assert(memcmp(info.volume_name, original.volume_name, EXT4_VOLUME_NAME_SIZE) == 0 ||
		    strcmp(info.volume_name, "recovered-label") == 0);
		if (barrier == MutationJournalCommit) {
			assert(strcmp(info.volume_name, "recovered-label") == 0);
		}
	}
	file = lookup(volume, root_item(volume), @"failure");
	assert(attributes(volume, file).size == 0 || attributes(volume, file).size == data.length);
	check_bytes(volume, file, attributes(volume, file).size != 0 ? data : [NSData data]);
	if (barrier == MutationJournalCommit && !renameVolume) {
		assert(attributes(volume, file).size == data.length);
	}
	[volume invalidate];
}

static void
check_maintenance(WritableImage *image)
{
	Ext4ResourceIO *io = [[Ext4ResourceIO alloc] initWithReader:image];
	struct ext4_info info;
	Ext4LegacyVolume *volume;
	NSUInteger writes;
	NSDate *deadline;

	assert([io inspect:&info] == EXT4_OK && info.mmp_interval > 0 && info.mmp_interval <= 10);
	volume = open_volume(image, YES);
	writes = image.writes;
	deadline = [NSDate dateWithTimeIntervalSinceNow:info.mmp_interval + 2];
	while (image.writes == writes && deadline.timeIntervalSinceNow > 0) {
		[NSThread sleepForTimeInterval:0.05];
	}
	assert(image.writes > writes); /* Heartbeat without activation or mount. */
	assert([volume finishUnloadedResource] == nil);
	[volume invalidate];
	volume = open_volume(image, YES); /* The preceding maintenance load released MMP. */
	assert([volume finishUnloadedResource] == nil);
	[volume invalidate];
	puts("PASS FSKit maintenance-only MMP heartbeat and ownership release");
}

static void
check_api_selection(NSData *fixture)
{
	WritableImage *image = [WritableImage new];
	Ext4ResourceIO *io;
	struct ext4_fs *fs = NULL;
	Ext4Volume *volume;

	image.bytes = [fixture mutableCopy];
	io = [[Ext4ResourceIO alloc] initWithReader:image];
	assert([io open:&fs] == EXT4_OK);
	volume = ext4_volume_create(nil, fs, io, NULL, NO);
	assert(volume != nil);
#if defined(__MAC_27_0) && __MAC_OS_X_VERSION_MAX_ALLOWED >= __MAC_27_0
	if (@available(macOS 27.0, *)) {
		assert([volume conformsToProtocol:@protocol(FSVolumeHandler)]);
		assert([volume conformsToProtocol:@protocol(FSVolumeReadWriteHandler)]);
		assert([volume conformsToProtocol:@protocol(FSVolumeKernelOffloadedIOHandler)]);
		assert([volume conformsToProtocol:@protocol(FSVolumeXattrHandler)]);
		assert([volume conformsToProtocol:@protocol(FSVolumePreallocateHandler)]);
		assert([volume conformsToProtocol:@protocol(FSVolumeSeekRegionHandler)]);
		assert(![volume conformsToProtocol:@protocol(FSVolumeOperations)]);
		assert(![volume conformsToProtocol:@protocol(FSVolumeReadWriteOperations)]);
		assert(![volume conformsToProtocol:@protocol(FSVolumeKernelOffloadedIOOperations)]);
	} else
#endif
	{
		assert([volume isKindOfClass:Ext4LegacyVolume.class]);
		assert([volume conformsToProtocol:@protocol(FSVolumeOperations)]);
		assert([volume conformsToProtocol:@protocol(FSVolumeReadWriteOperations)]);
	}
	[volume invalidate];
	[volume unmountWithReplyHandler:^{
	}];
	assert(image.writes == 0 && image.barriers == 0);
}

static void
check_capacity(WritableImage *image)
{
	Ext4LegacyVolume *volume = open_volume(image, YES);
	FSItem *file = create_item(volume, root_item(volume), @"capacity", FSItemTypeFile);
	NSMutableData *data = [NSMutableData dataWithLength:256 * 1024 - 1];
	NSMutableData *readback = [NSMutableData dataWithLength:data.length];
	Ext4Item *item = (Ext4Item *)file;
	__block size_t completed;
	__block NSError *failure;
	uint64_t total = 0;
	uint64_t offset;
	size_t count;
	BOOL partialError = NO;

	memset(data.mutableBytes, 0x6d, data.length);
	do {
		completed = 0;
		failure = nil;
		[volume writeContents:data
			       toFile:file
			     atOffset:(off_t)total
			 replyHandler:^(size_t written, NSError *error) {
			   completed = written;
			   failure = error;
			 }];
		assert(completed <= data.length);
		total += completed;
		assert(total <= image.bytes.length);
		if (failure != nil) {
			assert(failure.code == ENOSPC);
			partialError = completed != 0;
		} else {
			assert(completed == data.length);
		}
	} while (failure == nil);
	assert(partialError && attributes(volume, file).size == total);
	assert([volume validateItem:item] == EXT4_OK);
	for (offset = 0; offset < total; offset += completed) {
		count = (size_t)MIN(total - offset, data.length);
		assert([volume readItem:item
				 offset:offset
				 buffer:readback.mutableBytes
				 length:count
			      completed:&completed] == EXT4_OK);
		assert(completed == count && memcmp(readback.bytes, data.bytes, count) == 0);
	}
	set_size(volume, file, 0);
	write_bytes(volume, file, data, 0);
	sync_volume(volume);
	[volume invalidate];
	puts("PASS FSKit partial ENOSPC reports its durable prefix and error, readback and space "
	     "reuse");
}

#if defined(__MAC_27_0) && __MAC_OS_X_VERSION_MAX_ALLOWED >= __MAC_27_0
API_AVAILABLE(macos(27.0))

static void
check_capacity_modern(WritableImage *image)
{
	Ext4ModernVolume *volume =
	    (Ext4ModernVolume *)open_volume_class(image, YES, NULL, Ext4ModernVolume.class);
	FSItem *file = create_item(volume, root_item(volume), @"capacity", FSItemTypeFile);
	Ext4Item *item = (Ext4Item *)file;
	NSMutableData *data = [NSMutableData dataWithLength:256 * 1024 - 1];
	NSMutableData *readback = [NSMutableData dataWithLength:data.length];
	__block NSError *failure;
	__block BOOL replied;
	uint64_t total = 0;
	uint64_t offset;
	size_t count;
	size_t completed;

	memset(data.mutableBytes, 0x6d, data.length);
	do {
		offset = total;
		replied = NO;
		[volume writeContents:data
			       toFile:file
			     atOffset:(off_t)offset
			 replyHandler:^(FSWriteFileResult *result, NSError *error) {
			   assert(!replied);
			   replied = YES;
			   failure = error;
			   assert((result != nil) == (error == nil));
			 }];
		assert(replied && [volume validateItem:item] == EXT4_OK);
		total = item->inode.size;
		assert(total >= offset && total - offset <= data.length);
		assert(total <= image.bytes.length);
		if (failure == nil) {
			assert(total - offset == data.length);
		} else {
			assert([failure.domain isEqualToString:NSPOSIXErrorDomain]);
			assert(failure.code == ENOSPC && total > offset);
		}
	} while (failure == nil);
	/* The modern reply cannot publish partial bytes alongside an error, but
	 * the engine must retain exactly the committed prefix and remain usable. */
	for (offset = 0; offset < total; offset += completed) {
		count = (size_t)MIN(total - offset, data.length);
		assert([volume readItem:item
				 offset:offset
				 buffer:readback.mutableBytes
				 length:count
			      completed:&completed] == EXT4_OK);
		assert(completed == count && memcmp(readback.bytes, data.bytes, count) == 0);
	}
	[volume writeContents:data
		       toFile:file
		     atOffset:(off_t)total
		 replyHandler:^(FSWriteFileResult *result, NSError *error) {
		   assert(result == nil && [error.domain isEqualToString:NSPOSIXErrorDomain]);
		   assert(error.code == ENOSPC);
		 }];
	assert(attributes(volume, file).size == total);
	set_size(volume, file, 0);
	[volume writeContents:data
		       toFile:file
		     atOffset:0
		 replyHandler:^(FSWriteFileResult *result, NSError *error) {
		   assert(result != nil && error == nil);
		 }];
	assert(attributes(volume, file).size == data.length);
	sync_volume(volume);
	[volume invalidate];
	puts("PASS FSKit modern partial and zero-progress ENOSPC, committed prefix readback and "
	     "space reuse");
}
#endif

int
main(int argc, const char *argv[])
{
	@autoreleasepool {
		NSData *fixture;
		WritableImage *image = [WritableImage new];
		NSUInteger barrier;

		assert(argc == 2 || argc == 3 ||
		    (argc == 4 &&
			(strcmp(argv[2], "--capacity") == 0 ||
			    strcmp(argv[2], "--modern-capacity") == 0)));
		fixture = [NSData dataWithContentsOfFile:@(argv[1])];
		assert(fixture != nil);
		image.bytes = [fixture mutableCopy];
		if (argc >= 3 && strcmp(argv[2], "--modern-capacity") == 0) {
#if defined(__MAC_27_0) && __MAC_OS_X_VERSION_MAX_ALLOWED >= __MAC_27_0
			if (@available(macOS 27.0, *)) {
				check_capacity_modern(image);
				if (argc == 4) {
					assert([image.bytes writeToFile:@(argv[3]) atomically:YES]);
				}
				return 0;
			}
#endif
			fputs("Modern capacity acceptance requires macOS 27 and its SDK\n", stderr);
			return 1;
		}
		if (argc >= 3 && strcmp(argv[2], "--capacity") == 0) {
			check_capacity(image);
			if (argc == 4) {
				assert([image.bytes writeToFile:@(argv[3]) atomically:YES]);
			}
			return 0;
		}
		if (argc == 3 && strcmp(argv[2], "--maintenance") == 0) {
			check_maintenance(image);
			return 0;
		}
		check_api_selection(fixture);
		check_security_mutations(fixture);
		@autoreleasepool {
			check_mutations(image);
		}
		@autoreleasepool {
			check_remount(image);
		}
		if (argc == 3) {
			assert([image.bytes writeToFile:@(argv[2]) atomically:YES]);
		}
		/* One failed write, then every barrier from recovery marker to empty log. */
		for (barrier = 0; barrier <= MutationJournalReset; barrier++) {
			@autoreleasepool {
				check_failed_mutation(fixture, barrier, NO,
				    argc == 3 ? [@(argv[2]) stringByDeletingLastPathComponent]
					      : nil);
			}
			@autoreleasepool {
				check_failed_mutation(fixture, barrier, YES, nil);
			}
		}
		puts("PASS FSKit mutation: namespace, metadata, xattrs, resize, preallocation, "
		     "flags, "
		     "open-unlinked lifetime, concurrent writes, remount and I/O failure "
		     "propagation, recovery at every barrier and immutable read-only inspection");
	}
	return 0;
}

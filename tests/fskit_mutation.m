/* SPDX-License-Identifier: BSD-3-Clause */
#import "../adapters/fskit/Ext4ResourceIO.h"
#import "../adapters/fskit/Ext4VolumeInternal.h"
#include <assert.h>
#include <errno.h>
#include <stdatomic.h>
#include <string.h>
#include <sys/stat.h>

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
@property NSUInteger writes;
@property NSUInteger barriers;
@property BOOL failWrite;
@property BOOL failBarrier;
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
	if (self.failBarrier) {
		*error = [NSError errorWithDomain:NSPOSIXErrorDomain code:EIO userInfo:nil];
	}
	return !self.failBarrier;
}

@end

static FSFileName *
name(NSString *value)
{
	return [FSFileName nameWithString:value];
}

static Ext4Volume *
open_volume(WritableImage *image, BOOL writable)
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
	volume = [[Ext4Volume alloc] initWithResource:nil filesystem:fs resourceOwner:io];
	assert(volume != nil);
	volume.writable = writable;
	return volume;
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
lookup(Ext4Volume *volume, FSItem *parent, NSString *filename)
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
write_bytes(Ext4Volume *volume, FSItem *file, NSData *data, off_t offset)
{
	[volume writeContents:data
		       toFile:file
		     atOffset:offset
		 replyHandler:^(size_t size, NSError *error) {
		   assert(error == nil && size == data.length);
		 }];
}

static void
check_bytes(Ext4Volume *volume, FSItem *file, NSData *expected)
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
	Ext4Volume *volume = open_volume(image, YES);
	FSItem *root = root_item(volume);
	FSItem *directory = create_item(volume, root, @"fskit-written", FSItemTypeDirectory);
	FSItem *file = create_item(volume, directory, @"payload", FSItemTypeFile);
	FSItem *replacement;
	FSItemSetAttributesRequest *request;
	NSMutableData *expected = [NSMutableData dataWithLength:9001];
	NSData *value = [@"attribute bytes" dataUsingEncoding:NSUTF8StringEncoding];
	uint8_t *bytes = expected.mutableBytes;
	NSUInteger index;
	FSDirectoryVerifier verifier = ((Ext4Item *)directory)->directoryVersion;

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
}

static void
check_remount(WritableImage *image)
{
	Ext4Volume *volume = open_volume(image, NO);
	FSItem *directory = lookup(volume, root_item(volume), @"fskit-written");
	FSItem *file = lookup(volume, directory, @"replacement");
	NSUInteger writes = image.writes;
	NSMutableData *expected = [NSMutableData dataWithLength:20000];
	uint8_t *bytes = expected.mutableBytes;
	NSUInteger index;

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
check_failed_write(NSData *fixture, BOOL barrier)
{
	WritableImage *image = [WritableImage new];
	Ext4Volume *volume;
	FSItem *file;
	NSData *data = [@"failure" dataUsingEncoding:NSUTF8StringEncoding];

	image.bytes = [fixture mutableCopy];
	volume = open_volume(image, YES);
	file = create_item(volume, root_item(volume), @"failure", FSItemTypeFile);
	image.failWrite = !barrier;
	image.failBarrier = barrier;
	[volume writeContents:data
		       toFile:file
		     atOffset:0
		 replyHandler:^(size_t size, NSError *error) {
		   assert(size == 0 && error != nil);
		 }];
	[volume synchronizeWithFlags:0
			replyHandler:^(NSError *error) {
			  assert(error != nil);
			}];
}

int
main(int argc, const char *argv[])
{
	@autoreleasepool {
		NSData *fixture;
		WritableImage *image = [WritableImage new];

		assert(argc == 2 || argc == 3);
		fixture = [NSData dataWithContentsOfFile:@(argv[1])];
		assert(fixture != nil);
		image.bytes = [fixture mutableCopy];
		@autoreleasepool {
			check_mutations(image);
		}
		@autoreleasepool {
			check_remount(image);
		}
		if (argc == 3) {
			assert([image.bytes writeToFile:@(argv[2]) atomically:YES]);
		}
		@autoreleasepool {
			check_failed_write(fixture, NO);
		}
		@autoreleasepool {
			check_failed_write(fixture, YES);
		}
		puts("PASS FSKit mutation: namespace, metadata, xattrs, resize, preallocation, "
		     "flags, "
		     "open-unlinked lifetime, concurrent writes, remount and I/O failure "
		     "propagation");
	}
	return 0;
}

/* SPDX-License-Identifier: BSD-3-Clause */
#import "../adapters/fskit/Ext4ResourceIO.h"
#import "../adapters/fskit/Ext4Volume.h"
#import "../adapters/fskit/Ext4VolumeInternal.h"
#import "../adapters/fskit/Ext4Control.h"
#include "../adapters/fskit/Ext4Crypto.h"
#include "../core/sha.h"
#include "../core/internal.h"
#include <stdlib.h>
#include <unistd.h>
#include <assert.h>
#include <stdatomic.h>
#include <string.h>

@interface ImageBlocks : NSObject <Ext4BlockReader> {
	atomic_uint _activeReads;
}
@property NSData *image;
@property(nonatomic, getter=isRevoked) BOOL revoked;
@property size_t reads;
@property size_t failRead;
@end

@implementation ImageBlocks

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
	return self.image.length / 512;
}

- (size_t)readInto:(void *)buffer
	startingAt:(off_t)offset
	    length:(size_t)length
	     error:(NSError **)error
{
	unsigned active = atomic_fetch_add(&_activeReads, 1);

	assert(active == 0);
	assert(offset >= 0 && (uint64_t)offset + length <= self.image.length);
	self.reads++;
	if (self.failRead != 0 && self.reads == self.failRead) {
		*error = [NSError errorWithDomain:NSPOSIXErrorDomain code:EIO userInfo:nil];
		assert(atomic_fetch_sub(&_activeReads, 1) == 1);
		return 0;
	}
	memcpy(buffer, (const uint8_t *)self.image.bytes + offset, length);
	assert(atomic_fetch_sub(&_activeReads, 1) == 1);
	return length;
}

@end

@interface TestDirectoryPacker : NSObject
@property NSMutableArray<NSDictionary *> *entries;
@property NSUInteger capacity;
@property BOOL stopWhenFull;
@property ImageBlocks *device;
@property NSUInteger failAfterEntries;
- (BOOL)packEntryWithName:(FSFileName *)name
		 itemType:(FSItemType)type
		   itemID:(FSItemID)itemID
	       nextCookie:(FSDirectoryCookie)cookie
	       attributes:(FSItemAttributes *)attributes;
@end

@implementation TestDirectoryPacker

- (BOOL)packEntryWithName:(FSFileName *)name
		 itemType:(FSItemType)type
		   itemID:(FSItemID)itemID
	       nextCookie:(FSDirectoryCookie)cookie
	       attributes:(FSItemAttributes *)attributes
{
	if (self.entries.count == self.capacity) {
		return NO;
	}
	[self.entries addObject:@{
		@"name" : [[NSString alloc] initWithData:name.data encoding:NSUTF8StringEncoding],
		@"type" : @(type),
		@"inode" : @(itemID),
		@"cookie" : @(cookie),
		@"attributes" : attributes ?: NSNull.null
	}];
	if (self.entries.count == self.failAfterEntries) {
		self.device.failRead = self.device.reads + 1;
	}
	return !self.stopWhenFull || self.entries.count < self.capacity;
}

@end

/* Only these public selectors are used by the adapter. A fake buffer keeps the
 * test independent of the kernel's FSKit shared-memory allocation machinery. */
@interface ReadBuffer : NSObject
@property NSMutableData *data;
- (size_t)length;
- (void *)mutableBytes;
@end
@implementation ReadBuffer

- (size_t)length
{
	return self.data.length;
}

- (void *)mutableBytes
{
	return self.data.mutableBytes;
}

@end

@interface TestOptions : NSObject
- (NSArray<NSString *> *)taskOptions;
@end
@implementation TestOptions

- (NSArray<NSString *> *)taskOptions
{
	return @[];
}

@end

@interface TestExtentPacker : NSObject
@property NSMutableArray<NSDictionary *> *extents;
@property NSUInteger capacity;
- (BOOL)packExtentWithResource:(FSBlockDeviceResource *)resource
			  type:(FSExtentType)type
		 logicalOffset:(off_t)logical
		physicalOffset:(off_t)physical
			length:(size_t)length;
@end
@implementation TestExtentPacker

- (BOOL)packExtentWithResource:(FSBlockDeviceResource *)resource
			  type:(FSExtentType)type
		 logicalOffset:(off_t)logical
		physicalOffset:(off_t)physical
			length:(size_t)length
{
	assert(resource != nil && length != 0 && length <= UINT32_MAX);
	[self.extents addObject:@{
		@"type" : @(type),
		@"logical" : @(logical),
		@"physical" : @(physical),
		@"length" : @(length)
	}];
	return self.extents.count < self.capacity;
}

@end

static void
check_mappings(Ext4LegacyMappedVolume *volume, FSItem *sparse, FSItem *partialEOF, NSData *image)
{
	TestExtentPacker *packer = [TestExtentPacker new];
	__block unsigned replies = 0;
	NSMutableData *reconstructed = [NSMutableData dataWithLength:2 * 1024 * 1024];
	uint64_t next = 0;
	size_t index;
	uint8_t expected;

	packer.extents = [NSMutableArray array];
	packer.capacity = 2;
	while (next < reconstructed.length) {
		[packer.extents removeAllObjects];
		[volume blockmapFile:sparse
			      offset:(off_t)next
			      length:reconstructed.length - (size_t)next
			       flags:FSBlockmapFlagsRead
			 operationID:FSOperationIDUnspecified
			      packer:(FSExtentPacker *)packer
			replyHandler:^(NSError *error) {
			  assert(error == nil);
			  replies++;
			}];
		assert(packer.extents.count > 0 && packer.extents.count <= packer.capacity);
		for (NSDictionary *extent in packer.extents) {
			uint64_t logical = [extent[@"logical"] unsignedLongLongValue];
			uint64_t physical = [extent[@"physical"] unsignedLongLongValue];
			size_t length = [extent[@"length"] unsignedLongLongValue];

			assert(logical == next && length <= reconstructed.length - next);
			if ([extent[@"type"] intValue] == FSExtentTypeData) {
				assert(physical + length <= image.length);
				memcpy((uint8_t *)reconstructed.mutableBytes + logical,
				    (const uint8_t *)image.bytes + physical, length);
			} else {
				assert([extent[@"type"] intValue] == FSExtentTypeZeroFill);
			}
			next += length;
		}
	}
	for (index = 0; index < reconstructed.length; index++) {
		expected =
		    index / 65536 < 12 && index % 65536 < 4096 ? (uint8_t)(index / 65536 + 1) : 0;
		assert(((const uint8_t *)reconstructed.bytes)[index] == expected);
	}
	assert(replies > 1);
	[volume blockmapFile:partialEOF
		      offset:0
		      length:1
		       flags:FSBlockmapFlagsRead
		 operationID:FSOperationIDUnspecified
		      packer:(FSExtentPacker *)packer
		replyHandler:^(NSError *error) {
		  assert(error != nil);
		}];
	[volume blockmapFile:sparse
		      offset:0
		      length:4096
		       flags:FSBlockmapFlagsWrite
		 operationID:FSOperationIDUnspecified
		      packer:(FSExtentPacker *)packer
		replyHandler:^(NSError *error) {
		  assert(error.code == EROFS);
		}];
}

static enum ext4_result
test_read(void *context, uint64_t offset, void *buffer, size_t length)
{
	return [(__bridge Ext4ResourceIO *)context readAt:offset buffer:buffer length:length];
}

static enum ext4_result
test_write(void *context, uint64_t offset, const void *buffer, size_t length)
{
	NSMutableData *image = (__bridge NSMutableData *)context;

	assert(offset <= image.length && length <= image.length - offset);
	memcpy((uint8_t *)image.mutableBytes + offset, buffer, length);
	return EXT4_OK;
}

static enum ext4_result
test_flush(void *context)
{
	(void)context;
	return EXT4_OK;
}

static void *
test_allocate(void *context, size_t size)
{
	(void)context;
	return malloc(size);
}

static void
test_release(void *context, void *allocation, size_t size)
{
	(void)context;
	(void)size;
	free(allocation);
}

/* In-memory fixture writer, never a native device durability capability. */
static void
check_acl_admission(NSData *source)
{
	NSMutableData *image = [source mutableCopy];
	ImageBlocks *device = [ImageBlocks new];
	Ext4ResourceIO *resource;
	struct ext4_environment environment = { 0 };
	struct ext4_write_environment writer = { 0 };
	struct ext4_fs *fs = NULL;
	struct ext4_inode root;
	struct ext4_inode file;
	struct ext4_xattr_change acl = { 0 };
	struct ext4_inode_update update = { 0 };
	Ext4LegacyMappedVolume *volume;
	__block FSItem *rootItem;
	__block unsigned replies = 0;
	const uint8_t opaqueACL[] = { 0 };

	device.image = image;
	resource = [[Ext4ResourceIO alloc] initWithReader:device];
	environment.context = (__bridge void *)resource;
	environment.size_bytes = image.length;
	environment.read = test_read;
	environment.allocate = test_allocate;
	environment.release = test_release;
	writer.context = (__bridge void *)image;
	writer.write = test_write;
	writer.flush = test_flush;
	assert(ext4_mount_writable(&environment, &writer, &fs) == EXT4_OK);
	assert(ext4_get_inode(fs, EXT4_ROOT_INODE, &root) == EXT4_OK);
	assert(ext4_lookup(fs, &root, (const uint8_t *)"hello.txt", strlen("hello.txt"), &file) ==
	    EXT4_OK);
	/* Even malformed ACL bytes must not fall back to mode-only authorization. */
	acl.policy = EXT4_XATTR_SET;
	acl.name_index = EXT4_XATTR_POSIX_ACL_ACCESS;
	acl.value = opaqueACL;
	acl.value_size = sizeof(opaqueACL);
	update.fields = EXT4_ATTR_XATTRS | EXT4_ATTR_CHANGE_TIME;
	update.change_time = file.change_time;
	update.xattrs = &acl;
	update.xattr_count = 1;
	assert(ext4_set_attributes(fs, file.number, file.generation, &update, &file) == EXT4_OK);
	assert(ext4_sync(fs) == EXT4_OK);
	ext4_unmount(fs);
	assert([resource open:&fs] == EXT4_OK);
	volume = [[Ext4LegacyMappedVolume alloc] initWithResource:(FSBlockDeviceResource *)device
						       filesystem:fs
						    resourceOwner:resource];
	[volume activateWithOptions:(FSTaskOptions *)[TestOptions new]
		       replyHandler:^(FSItem *item, NSError *error) {
			 assert(error == nil);
			 rootItem = item;
		       }];
	[volume lookupItemNamed:[FSFileName nameWithString:@"hello.txt"]
		    inDirectory:rootItem
		   replyHandler:^(FSItem *item, FSFileName *name, NSError *error) {
		     assert(item == nil && name == nil && error.code == ENOTSUP);
		     replies++;
		   }];
	assert(replies == 1);
	[volume invalidate];
}

static void
check_item_lifetime(NSData *image)
{
	__weak Ext4LegacyMappedVolume *weakVolume;
	__weak Ext4ResourceIO *weakResource;
	__block FSItem *survivor = nil;

	@autoreleasepool {
		ImageBlocks *device = [ImageBlocks new];
		Ext4ResourceIO *resource;
		struct ext4_fs *fs = NULL;
		Ext4LegacyMappedVolume *volume;

		device.image = image;
		resource = [[Ext4ResourceIO alloc] initWithReader:device];
		assert([resource open:&fs] == EXT4_OK);
		volume =
		    [[Ext4LegacyMappedVolume alloc] initWithResource:(FSBlockDeviceResource *)device
							  filesystem:fs
						       resourceOwner:resource];
		weakVolume = volume;
		weakResource = resource;
		[volume activateWithOptions:(FSTaskOptions *)[TestOptions new]
			       replyHandler:^(FSItem *item, NSError *error) {
				 assert(error == nil);
				 survivor = item;
			       }];
	}
	@autoreleasepool {
		assert(survivor != nil && weakVolume != nil && weakResource != nil);
		[weakVolume invalidate];
	}
	survivor = nil;
	@autoreleasepool {
		assert(weakVolume == nil && weakResource == nil);
	}
}

static FSItem *
lookup(Ext4LegacyMappedVolume *volume, FSItem *directory, NSString *name)
{
	__block FSItem *found = nil;
	__block unsigned calls = 0;

	[volume lookupItemNamed:[FSFileName nameWithString:name]
		    inDirectory:directory
		   replyHandler:^(FSItem *item, FSFileName *resolved, NSError *error) {
		     assert(error == nil && item != nil && resolved != nil);
		     calls++;
		     found = item;
		   }];
	assert(calls == 1);
	return found;
}

static void
check_open(
    Ext4LegacyMappedVolume *volume, FSItem *item, FSVolumeOpenModes modes, NSInteger expected)
{
	__block unsigned calls = 0;

	[volume openItem:item
	       withModes:modes
	    replyHandler:^(NSError *error) {
	      assert(expected == 0 ? error == nil
				   : error != nil && error.code == expected &&
			  [error.domain isEqual:NSPOSIXErrorDomain]);
	      calls++;
	    }];
	assert(calls == 1);
	if (expected == 0) {
		[volume closeItem:item
		     keepingModes:0
		     replyHandler:^(NSError *error) {
		       assert(error == nil);
		       calls++;
		     }];
		assert(calls == 2);
	}
}

static FSDirectoryVerifier
enumerate(Ext4LegacyMappedVolume *volume, FSItem *directory, FSDirectoryCookie cookie,
    FSDirectoryVerifier verifier, FSItemGetAttributesRequest *attributes,
    TestDirectoryPacker *packer, NSInteger expectedError)
{
	__block unsigned calls = 0;
	__block FSDirectoryVerifier result = FSDirectoryVerifierInitial;

	[volume enumerateDirectory:directory
		  startingAtCookie:cookie
			  verifier:verifier
	       providingAttributes:attributes
		       usingPacker:(FSDirectoryEntryPacker *)packer
		      replyHandler:^(FSDirectoryVerifier current, NSError *error) {
			assert(expectedError == 0 ? error == nil
						  : error.code == expectedError &&
				    [error.domain isEqual:NSPOSIXErrorDomain]);
			result = current;
			calls++;
		      }];
	assert(calls == 1);
	return result;
}

static void
check_directory(Ext4LegacyMappedVolume *volume, FSItem *root, ImageBlocks *device)
{
	FSItem *directory = lookup(volume, root, @"many");
	TestDirectoryPacker *packer = [TestDirectoryPacker new];
	FSItemGetAttributesRequest *attributes;
	NSMutableSet<NSString *> *expected;
	NSMutableSet<NSString *> *seen;
	FSDirectoryCookie cookie;
	FSDirectoryVerifier verifier;
	const NSUInteger capacities[] = { 1, 7, 512 };
	NSUInteger capacity;
	unsigned mode;
	unsigned number;
	size_t reads;

	packer.entries = [NSMutableArray array];
	packer.device = device;
	for (mode = 0; mode < 4; mode++) {
		attributes = mode & 1 ? [FSItemGetAttributesRequest new] : nil;
		packer.stopWhenFull = (mode & 2) != 0;
		expected = [NSMutableSet set];
		for (number = 0; number < 400; number++) {
			[expected addObject:[NSString stringWithFormat:@"entry-%04u", number]];
		}
		if (attributes == nil) {
			[expected addObject:@"."];
			[expected addObject:@".."];
		}
		for (capacity = 0; capacity < sizeof(capacities) / sizeof(capacities[0]);
		    capacity++) {
			packer.capacity = capacities[capacity];
			cookie = FSDirectoryCookieInitial;
			verifier = FSDirectoryVerifierInitial;
			seen = [NSMutableSet set];
			reads = device.reads;
			for (;;) {
				[packer.entries removeAllObjects];
				verifier = enumerate(
				    volume, directory, cookie, verifier, attributes, packer, 0);
				assert(verifier != FSDirectoryVerifierInitial);
				if (packer.entries.count == 0) {
					break;
				}
				for (NSDictionary *entry in packer.entries) {
					NSString *name = entry[@"name"];
					NSString *contents;
					BOOL dot = [name isEqual:@"."] || [name isEqual:@".."];
					FSItemAttributes *actual = entry[@"attributes"];
					FSDirectoryCookie next =
					    [entry[@"cookie"] unsignedLongLongValue];

					assert([expected containsObject:name] &&
					    ![seen containsObject:name]);
					assert([entry[@"type"] intValue] ==
					    (dot ? FSItemTypeDirectory : FSItemTypeFile));
					assert([entry[@"inode"] unsignedLongLongValue] >=
					    EXT4_ROOT_INODE);
					assert(next > cookie);
					if (attributes != nil) {
						assert(
						    [actual isKindOfClass:FSItemAttributes.class]);
						assert(actual.fileID ==
						    [entry[@"inode"] unsignedLongLongValue]);
						assert(actual.type == FSItemTypeFile);
						contents = [NSString stringWithFormat:@"%d\n",
						    [name substringFromIndex:6].intValue];
						assert(actual.size == contents.length);
					} else {
						assert((id)actual == NSNull.null);
					}
					[seen addObject:name];
					cookie = next;
				}
			}
			assert([seen isEqual:expected]);
			if (packer.capacity == 512 && !packer.stopWhenFull) {
				printf("FSKit directory: attributes=%u entries=%zu "
				       "resource-reads=%zu\n",
				    mode & 1, (size_t)seen.count, device.reads - reads);
			}
		}
	}
	/* Neither a stale verifier nor a failed read may become a successful EOF. */
	[packer.entries removeAllObjects];
	reads = device.reads;
	(void)enumerate(volume, directory, 0, verifier + 1, nil, packer, ESTALE);
	assert(packer.entries.count == 0 && device.reads == reads);
	device.failRead = device.reads + 1;
	(void)enumerate(volume, directory, 0, verifier, nil, packer, EIO);
	assert(packer.entries.count == 0);
	device.failRead = 0;
	packer.failAfterEntries = 3;
	(void)enumerate(volume, directory, 0, verifier, nil, packer, EIO);
	assert(packer.entries.count == 3);
	device.failRead = 0;
	packer.failAfterEntries = 0;
	[packer.entries removeAllObjects];
	(void)enumerate(volume, directory, 0, verifier, nil, packer, 0);
	assert(packer.entries.count == 402);
	puts("PASS FSKit directory: paginated names and attributes, dot policy, verifier and read "
	     "errors");
}

static void
check_dangling_directory_entry(NSData *source)
{
	NSMutableData *image = [source mutableCopy];
	ImageBlocks *device = [ImageBlocks new];
	Ext4ResourceIO *resource;
	struct ext4_fs *fs = NULL;
	struct ext4_inode root;
	struct ext4_inode directory;
	struct ext4_inode file;
	struct ext4_inode_disk *disk;
	uint64_t offset;
	Ext4LegacyMappedVolume *volume;
	__block FSItem *rootItem = nil;
	TestDirectoryPacker *packer = [TestDirectoryPacker new];

	device.image = image;
	resource = [[Ext4ResourceIO alloc] initWithReader:device];
	assert([resource open:&fs] == EXT4_OK);
	assert(ext4_get_inode(fs, EXT4_ROOT_INODE, &root) == EXT4_OK);
	assert(
	    ext4_lookup(fs, &root, (const uint8_t *)"many", strlen("many"), &directory) == EXT4_OK);
	assert(ext4_lookup(fs, &directory, (const uint8_t *)"entry-0000", strlen("entry-0000"),
		   &file) == EXT4_OK);
	assert(ext4_inode_location(fs, file.number, &offset) == EXT4_OK);
	assert(offset <= image.length && fs->inode_size <= image.length - offset);
	/* Preserve the directory entry and valid metadata checksums, but leave its
	 * allocated inode without a live mode. Build the malformed fixture before
	 * giving a fresh, immutable view to the adapter. */
	disk = (struct ext4_inode_disk *)((uint8_t *)image.mutableBytes + offset);
	ext4_encode16(&disk->mode, 0);
	ext4_inode_checksum_set(fs, file.number, disk);
	ext4_unmount(fs);
	assert([resource open:&fs] == EXT4_OK);
	assert(ext4_get_inode(fs, file.number, &file) == EXT4_NOT_FOUND);
	volume = [[Ext4LegacyMappedVolume alloc] initWithResource:(FSBlockDeviceResource *)device
						       filesystem:fs
						    resourceOwner:resource];
	[volume activateWithOptions:(FSTaskOptions *)[TestOptions new]
		       replyHandler:^(FSItem *item, NSError *error) {
			 assert(error == nil && item != nil);
			 rootItem = item;
		       }];
	assert(rootItem != nil);
	packer.entries = [NSMutableArray array];
	packer.capacity = 512;
	(void)enumerate(volume, lookup(volume, rootItem, @"many"), FSDirectoryCookieInitial,
	    FSDirectoryVerifierInitial, [FSItemGetAttributesRequest new], packer, EIO);
	assert(packer.entries.count < 400);
	[volume invalidate];
	puts("PASS FSKit directory: dangling inode reports EIO rather than successful EOF");
}

static void
read_file(Ext4LegacyMappedVolume *volume, FSItem *file, NSData *expected)
{
	ReadBuffer *buffer = [ReadBuffer new];
	__block unsigned calls = 0;

	buffer.data = [NSMutableData dataWithLength:expected.length + 16];
	memset(buffer.data.mutableBytes, 0xa5, buffer.data.length);
	[volume readFromFile:file
		      offset:0
		      length:buffer.length
		  intoBuffer:(FSMutableFileDataBuffer *)buffer
		replyHandler:^(size_t completed, NSError *error) {
		  assert(error == nil && completed == expected.length);
		  calls++;
		}];
	assert(calls == 1 && memcmp(buffer.data.bytes, expected.bytes, expected.length) == 0);
	assert(((const uint8_t *)buffer.data.bytes)[expected.length] == 0xa5);
}

static void
check_read_state_pressure(Ext4LegacyMappedVolume *volume, FSItem *file, FSItem *other,
    NSData *expected, NSData *image, ImageBlocks *device)
{
	Ext4Item *owned = (Ext4Item *)file;
	Ext4Item *untouched = (Ext4Item *)other;
	struct ext4_read_state *oldReader;
	struct ext4_read_state *otherReader;
	NSDictionary *state;
	uint64_t position;
	size_t reads;

	/* Component events must not race the host's unrelated memory conditions. */
	[volume stopReadStateMaintenance];
	[volume updateMemoryPressure:DISPATCH_MEMORYPRESSURE_NORMAL];
	state = [volume controlRequest:@{ @"command" : @"getSettings" }][@"result"];
	assert([state[@"retainReadState"] isEqual:@YES]);
	assert([state[@"readStateRetentionActive"] isEqual:@YES]);
	read_file(volume, file, expected);
	oldReader = owned->hold->reader;
	otherReader = untouched->hold->reader;
	assert(oldReader != NULL && otherReader != NULL);
	reads = device.reads;
	[volume updateMemoryPressure:DISPATCH_MEMORYPRESSURE_WARN];
	assert(device.reads == reads && owned->hold->reader == oldReader &&
	    untouched->hold->reader == otherReader);
	state = [volume controlRequest:@{ @"command" : @"getSettings" }][@"result"];
	assert([state[@"retainReadState"] isEqual:@YES]);
	assert([state[@"readStateRetentionActive"] isEqual:@NO]);
	read_file(volume, file, expected);
	assert(owned->hold->reader == NULL && untouched->hold->reader == otherReader);
	assert([volume seekItem:owned offset:0 region:EXT4_SEEK_DATA result:&position] == EXT4_OK);
	assert(position == 0 && owned->hold->reader == NULL);
	check_mappings(volume, other, file, image);
	assert(owned->hold->reader == NULL && untouched->hold->reader == NULL);
	[volume
	    updateMemoryPressure:DISPATCH_MEMORYPRESSURE_CRITICAL | DISPATCH_MEMORYPRESSURE_NORMAL];
	state = [volume controlRequest:@{
		@"command" : @"setSettings",
		@"arguments" : @{ @"retainReadState" : @YES }
	}][@"result"];
	assert([state[@"retainReadState"] isEqual:@YES]);
	assert([state[@"readStateRetentionActive"] isEqual:@NO]);
	read_file(volume, file, expected);
	assert(owned->hold->reader == NULL);
	state = [volume controlRequest:@{
		@"command" : @"setSettings",
		@"arguments" : @{ @"retainReadState" : @NO }
	}][@"result"];
	assert([state[@"readStateRetentionActive"] isEqual:@NO]);
	[volume updateMemoryPressure:DISPATCH_MEMORYPRESSURE_NORMAL];
	state = [volume controlRequest:@{ @"command" : @"getSettings" }][@"result"];
	assert([state[@"retainReadState"] isEqual:@NO]);
	assert([state[@"readStateRetentionActive"] isEqual:@NO]);
	read_file(volume, file, expected);
	assert(owned->hold->reader == NULL);
	state = [volume controlRequest:@{
		@"command" : @"setSettings",
		@"arguments" : @{ @"retainReadState" : @YES }
	}][@"result"];
	assert([state[@"readStateRetentionActive"] isEqual:@YES]);
	read_file(volume, file, expected);
	assert(owned->hold->reader != NULL);
	[volume updateMemoryPressure:0];
	assert([volume readStateRetentionActive]);
	puts("PASS FSKit memory pressure: lazy read/seek/map release, coalesced notifications and "
	     "preserved user preference");
}

static void
check_revoked_resource(NSData *image)
{
	ImageBlocks *device = [ImageBlocks new];
	Ext4ResourceIO *resource;
	struct ext4_fs *fs = NULL;
	Ext4LegacyMappedVolume *volume;
	__block FSItem *root = nil;
	FSItem *file;
	ReadBuffer *buffer = [ReadBuffer new];
	NSData *expected = [@"Machlin ext4\n" dataUsingEncoding:NSUTF8StringEncoding];
	size_t reads;
	__block unsigned replies = 0;

	device.image = image;
	resource = [[Ext4ResourceIO alloc] initWithReader:device];
	assert([resource open:&fs] == EXT4_OK);
	volume = [[Ext4LegacyMappedVolume alloc] initWithResource:(FSBlockDeviceResource *)device
						       filesystem:fs
						    resourceOwner:resource];
	[volume activateWithOptions:(FSTaskOptions *)[TestOptions new]
		       replyHandler:^(FSItem *item, NSError *error) {
			 assert(item != nil && error == nil);
			 root = item;
		       }];
	file = lookup(volume, root, @"hello.txt");
	read_file(volume, file, expected); /* Populate the retained inode and read state. */
	reads = device.reads;
	device.revoked = YES;
	buffer.data = [NSMutableData dataWithLength:32];
	memset(buffer.data.mutableBytes, 0xa5, buffer.length);
	[volume readFromFile:file
		      offset:0
		      length:buffer.length
		  intoBuffer:(FSMutableFileDataBuffer *)buffer
		replyHandler:^(size_t completed, NSError *error) {
		  assert(completed == 0 && error.code == EIO);
		  replies++;
		}];
	assert(((const uint8_t *)buffer.data.bytes)[0] == 0xa5);
	[volume getAttributes:[FSItemGetAttributesRequest new]
		       ofItem:file
		 replyHandler:^(FSItemAttributes *attributes, NSError *error) {
		   assert(attributes == nil && error.code == EIO);
		   replies++;
		 }];
	[volume lookupItemNamed:[FSFileName nameWithString:@"hello.txt"]
		    inDirectory:root
		   replyHandler:^(FSItem *item, FSFileName *name, NSError *error) {
		     assert(item == nil && name == nil && error.code == EIO);
		     replies++;
		   }];
	check_open(volume, file, FSVolumeOpenModesRead, EIO);
	[volume synchronizeWithFlags:0
			replyHandler:^(NSError *error) {
			  assert(error.code == EIO);
			  replies++;
			}];
	assert([volume checkMountEligibility].code == EIO);
	assert([[volume controlRequest:@{ @"command" : @"getInfo" }][@"error"][@"code"]
	    isEqual:@(EIO)]);
	assert(replies == 4 && device.reads == reads);
	/* A different resource must have a different owner, even if a fake device
	 * claims to regain access. A terminated owner cannot revive cached items. */
	device.revoked = NO;
	assert([volume checkMountEligibility].code == EIO && device.reads == reads);
	[volume invalidate];
	check_open(volume, file, FSVolumeOpenModesRead, ESTALE);
	assert(device.reads == reads);
	puts("PASS FSKit revoked read-only resource: retained inode, cached reads and late items");
}

static FSItem *
lookup_path(Ext4LegacyMappedVolume *volume, FSItem *root, NSString *path)
{
	FSItem *current = root;

	for (NSString *component in [path componentsSeparatedByString:@"/"]) {
		__block FSItem *next = nil;

		[volume lookupItemNamed:[FSFileName nameWithString:component]
			    inDirectory:current
			   replyHandler:^(FSItem *item, FSFileName *name, NSError *error) {
			     assert(error == nil && name != nil && item != nil);
			     next = item;
			   }];
		assert(next != nil);
		current = next;
	}
	return current;
}

static void
check_encrypted_volume(const char *imagePath, const char *manifestPath)
{
	ImageBlocks *device = [ImageBlocks new];
	Ext4ResourceIO *resource;
	struct ext4_fs *fs = NULL;
	struct ext4_native_crypto *crypto = ext4_native_crypto_create();
	uint8_t master[EXT4_NATIVE_MASTER_SIZE];
	uint8_t identifier[EXT4_NATIVE_IDENTIFIER_SIZE];
	Ext4LegacyMappedVolume *volume;
	__block FSItem *root = nil;
	NSString *manifest;
	ReadBuffer *buffer = [ReadBuffer new];
	size_t index;
	size_t checked = 0;

	device.image = [NSData dataWithContentsOfFile:[NSString stringWithUTF8String:imagePath]];
	assert(device.image != nil && crypto != NULL);
	resource = [[Ext4ResourceIO alloc] initWithReader:device];
	assert([resource open:&fs] == EXT4_OK);
	for (index = 0; index < sizeof(master); index++) {
		master[index] = (uint8_t)(index * 7 + 3);
	}
	assert(ext4_native_crypto_identifier(master, sizeof(master), identifier) == EXT4_OK);
	assert(ext4_native_crypto_add(
		   crypto, 2, identifier, sizeof(identifier), master, sizeof(master)) == EXT4_OK);
	volume = [[Ext4LegacyMappedVolume alloc] initWithResource:(FSBlockDeviceResource *)device
						       filesystem:fs
						    resourceOwner:resource
							   crypto:crypto];
	assert(volume != nil);
	[volume activateWithOptions:(FSTaskOptions *)[TestOptions new]
		       replyHandler:^(FSItem *item, NSError *error) {
			 assert(error == nil);
			 root = item;
		       }];
	assert(root != nil);
	manifest = [NSString stringWithContentsOfFile:[NSString stringWithUTF8String:manifestPath]
					     encoding:NSUTF8StringEncoding
						error:NULL];
	assert(manifest != nil);
	[volume stopReadStateMaintenance];
	[volume updateMemoryPressure:DISPATCH_MEMORYPRESSURE_WARN];
	assert(![volume readStateRetentionActive]);
	for (NSString *line in [manifest componentsSeparatedByString:@"\n"]) {
		NSArray<NSString *> *fields = [line componentsSeparatedByString:@" "];
		FSItem *item;

		if (line.length == 0) {
			continue;
		}
		assert(fields.count >= 3);
		item = lookup_path(volume, root, fields[1]);
		if ([fields[0] isEqual:@"symlink"]) {
			__block BOOL called = NO;

			assert(fields.count == 3);
			[volume readSymbolicLink:item
				    replyHandler:^(FSFileName *name, NSError *error) {
				      assert(error == nil &&
					  [name.data
					      isEqual:[fields[2]
							  dataUsingEncoding:NSUTF8StringEncoding]]);
				      called = YES;
				    }];
			assert(called);
		} else {
			struct ext4_sha256 hash;
			uint8_t digest[EXT4_SHA256_DIGEST_SIZE];
			NSMutableString *hex = [NSMutableString string];
			size_t remaining = (size_t)fields[2].longLongValue;
			off_t offset = 0;

			assert([fields[0] isEqual:@"file"] && fields.count == 4 &&
			    remaining <= 1024 * 1024);
			ext4_sha256_init(&hash);
			while (remaining != 0) {
				size_t amount = MIN(remaining, 8191U);
				__block size_t transferred = 0;

				buffer.data = [NSMutableData dataWithLength:amount];
				[volume readFromFile:item
					      offset:offset
					      length:amount
					  intoBuffer:(FSMutableFileDataBuffer *)buffer
					replyHandler:^(size_t completed, NSError *error) {
					  assert(error == nil && completed == amount);
					  transferred = completed;
					}];
				assert(transferred == amount);
				ext4_sha256_update(&hash, buffer.data.bytes, amount);
				offset += amount;
				remaining -= amount;
			}
			ext4_sha256_final(&hash, digest);
			for (index = 0; index < sizeof(digest); index++) {
				[hex appendFormat:@"%02x", digest[index]];
			}
			assert([hex isEqual:fields[3]]);
		}
		assert(((Ext4Item *)item)->hold->reader == NULL);
		checked++;
	}
	assert(checked > 30);
	assert([[[volume controlRequest:@{ @"command" : @"getInfo" }]
		   objectForKey:@"result"][@"loadedKeys"] unsignedIntegerValue] == 1);
	[volume updateMemoryPressure:DISPATCH_MEMORYPRESSURE_NORMAL];
	assert([volume readStateRetentionActive]);
	[volume invalidate];
	root = nil;
	volume = nil;
	/* A new mount without keys must still refuse encrypted contents, including
	 * an encrypted inode linked into an unencrypted directory. */
	assert([resource open:&fs] == EXT4_OK);
	volume = [[Ext4LegacyMappedVolume alloc] initWithResource:(FSBlockDeviceResource *)device
						       filesystem:fs
						    resourceOwner:resource];
	[volume activateWithOptions:(FSTaskOptions *)[TestOptions new]
		       replyHandler:^(FSItem *item, NSError *error) {
			 assert(error == nil);
			 root = item;
		       }];
	[volume readFromFile:lookup_path(volume, root, @"plain/block-out")
		      offset:0
		      length:1
		  intoBuffer:(FSMutableFileDataBuffer *)buffer
		replyHandler:^(size_t completed, NSError *error) {
		  assert(completed == 0 && error.code == EACCES);
		}];
	[volume invalidate];
	printf(
	    "PASS FSKit native crypto under memory pressure: %zu manifest entries, file digests, "
	    "encrypted names and symlinks; new keyless mount denied\n",
	    checked);
}

int
main(int argc, const char **argv)
{
	@autoreleasepool {
		ImageBlocks *device;
		Ext4ResourceIO *resource;
		struct ext4_fs *filesystem = NULL;
		Ext4LegacyMappedVolume *volume;
		__block FSItem *root;
		FSItem *hello;
		FSItem *alias;
		FSItem *link;
		FSItem *sparse;
		NSData *expected = [@"Machlin ext4\n" dataUsingEncoding:NSUTF8StringEncoding];
		__block unsigned replies = 0;
		NSDictionary *response;
		size_t reads;
		struct ext4_inode ordinary = { .mode = EXT4_MODE_REGULAR };
		struct ext4_inode directory = { .mode = EXT4_MODE_DIRECTORY };
		ReadBuffer *buffer;
		NSData *image;
		char ipcPath[] = "/tmp/ext4-volume-XXXXXX";
		Ext4ControlServer *server;
		NSError *ipcError = nil;

		assert(argc == 2 || argc == 4);
		if (argc == 4) {
			check_encrypted_volume(argv[2], argv[3]);
		}
		image = [NSData dataWithContentsOfFile:@(argv[1])];
		assert(image != nil);
		check_acl_admission(image);
		check_item_lifetime(image);
		check_revoked_resource(image);
		check_dangling_directory_entry(image);
		device = [ImageBlocks new];
		device.image = image;
		resource = [[Ext4ResourceIO alloc] initWithReader:device];
		assert([resource open:&filesystem] == EXT4_OK);
		volume =
		    [[Ext4LegacyMappedVolume alloc] initWithResource:(FSBlockDeviceResource *)device
							  filesystem:filesystem
						       resourceOwner:resource];
		assert(volume != nil);
		assert(ext4_inode_can_map_read(&ordinary));
		assert(!ext4_inode_can_map_read(&directory) && !ext4_inode_can_map_read(NULL));
		assert(volume.requestedMountOptions & FSMountOptionsReadOnly);
		assert([volume checkMountEligibility] == nil);
		[volume activateWithOptions:(FSTaskOptions *)[TestOptions new]
			       replyHandler:^(FSItem *item, NSError *error) {
				 assert(error == nil && item != nil);
				 root = item;
				 replies++;
			       }];
		assert(replies == 1);
		check_directory(volume, root, device);
		hello = lookup(volume, root, @"hello.txt");
		alias = lookup(volume, root, @"hello-hardlink");
		assert(hello == alias);
		assert(!volume.isOpenCloseInhibited);
		check_open(volume, hello, FSVolumeOpenModesRead, 0);
		check_open(volume, hello, FSVolumeOpenModesWrite, EROFS);
		check_open(volume, hello, FSVolumeOpenModesRead | FSVolumeOpenModesWrite, EROFS);
		read_file(volume, hello, expected);
		reads = device.reads;
		read_file(volume, alias, expected);
		assert(device.reads - reads < reads);
		link = lookup(volume, root, @"hello-link");
		[volume readSymbolicLink:link
			    replyHandler:^(FSFileName *name, NSError *error) {
			      assert(error == nil &&
				  [name.data isEqual:[@"hello.txt"
							 dataUsingEncoding:NSUTF8StringEncoding]]);
			      replies++;
			    }];
		[volume getXattrNamed:[FSFileName nameWithString:@"absent"]
			       ofItem:hello
			 replyHandler:^(NSData *value, NSError *error) {
			   assert(value == nil && error.code == ENOATTR);
			   replies++;
			 }];
		[volume listXattrsOfItem:hello
			    replyHandler:^(NSArray<FSFileName *> *names, NSError *error) {
			      assert(error == nil && names != nil);
			      replies++;
			    }];
		[volume writeContents:expected
			       toFile:hello
			     atOffset:0
			 replyHandler:^(size_t completed, NSError *error) {
			   assert(completed == 0 && error.code == EROFS);
			   replies++;
			 }];
		sparse = lookup(volume, root, @"sparse.bin");
		check_mappings(volume, sparse, hello, image);
		buffer = [ReadBuffer new];
		buffer.data = [NSMutableData dataWithLength:4096];
		memset(buffer.data.mutableBytes, 0xa5, buffer.data.length);
		[volume readFromFile:sparse
			      offset:4096
			      length:4096
			  intoBuffer:(FSMutableFileDataBuffer *)buffer
			replyHandler:^(size_t completed, NSError *error) {
			  assert(error == nil && completed == 4096);
			  replies++;
			}];
		assert([buffer.data isEqual:[NSMutableData dataWithLength:4096]]);
		check_read_state_pressure(volume, hello, sparse, expected, image, device);
		response = [volume controlRequest:@{
			@"command" : @"setSettings",
			@"arguments" : @{ @"retainReadState" : @NO }
		}];
		assert([response[@"result"][@"retainReadState"] isEqual:@NO]);
		response = [volume controlRequest:@{
			@"command" : @"setSettings",
			@"arguments" : @{ @"retainReadState" : @0 }
		}];
		assert([response[@"error"][@"code"] intValue] == EINVAL);
		assert(mkdtemp(ipcPath) != NULL);
		server = [[Ext4ControlServer alloc]
		    initWithDirectory:[NSURL fileURLWithPath:@(ipcPath)]
			     volumeID:@"test-volume"
			      handler:^NSDictionary *(NSDictionary *value) {
				return [volume controlRequest:value];
			      }
				error:&ipcError];
		assert(server != nil && ipcError == nil);
		response = [Ext4ControlClient request:@{ @"command" : @"getInfo" }
					     endpoint:server.manifestURL
						error:&ipcError];
		assert(ipcError == nil && response[@"result"] != nil);
		assert(CFGetTypeID((__bridge CFTypeRef)response[@"result"][@"readOnly"]) ==
		    CFBooleanGetTypeID());
		assert(CFGetTypeID((__bridge CFTypeRef)response[@"result"][@"keyStoreAvailable"]) ==
		    CFBooleanGetTypeID());
		response = [Ext4ControlClient request:@{ @"command" : @"getCapabilities" }
					     endpoint:server.manifestURL
						error:&ipcError];
		assert(ipcError == nil &&
		    CFGetTypeID((__bridge CFTypeRef)response[@"result"][@"kernelReadMapping"]) ==
			CFBooleanGetTypeID());
		dispatch_apply(
		    16, dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^(size_t index) {
		      @autoreleasepool {
			      NSDictionary *state;

			      [volume updateMemoryPressure:index % 2 == 0
				      ? DISPATCH_MEMORYPRESSURE_WARN
				      : DISPATCH_MEMORYPRESSURE_NORMAL];
			      read_file(volume, hello, expected);
			      state = [volume controlRequest:@{
				      @"command" : index % 2 == 0 ? @"dropReadState" : @"getInfo"
			      }];
			      assert(state[@"result"] != nil);
		      }
		    });
		assert(replies == 6);
		[server stop];
		assert(rmdir(ipcPath) == 0);
		[volume invalidate];
		[volume updateMemoryPressure:DISPATCH_MEMORYPRESSURE_NORMAL];
		check_open(volume, hello, FSVolumeOpenModesRead, ESTALE);
		response = [volume controlRequest:@{ @"command" : @"ping" }];
		assert([response[@"error"][@"code"] intValue] == ENXIO);
		[volume readFromFile:hello
			      offset:0
			      length:0
			  intoBuffer:(FSMutableFileDataBuffer *)buffer
			replyHandler:^(size_t completed, NSError *error) {
			  assert(completed == 0 && error.code == ESTALE);
			  replies++;
			}];
		/* Invalidate must not free holds still owned by FSItems. Their releases
		 * retain the resource owner until the last filesystem reference dies. */
		assert(replies == 7);
		assert([volume checkMountEligibility].code == ESTALE);
		[volume activateWithOptions:(FSTaskOptions *)[TestOptions new]
			       replyHandler:^(FSItem *item, NSError *error) {
				 assert(item == nil && error.code == ESTALE);
				 replies++;
			       }];
		[volume mountWithOptions:(FSTaskOptions *)[TestOptions new]
			    replyHandler:^(NSError *error) {
			      assert(error.code == ESTALE);
			      replies++;
			    }];
		assert(replies == 9);
		puts("PASS FSKit volume: held identity, reads, sparse EOF, xattrs, IPC "
		     "serialization and invalidation");
	}
	return 0;
}

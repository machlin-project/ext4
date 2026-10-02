/* SPDX-License-Identifier: BSD-3-Clause */
#import "../adapters/fskit/Ext4VolumeInternal.h"
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

enum { CheckSectorSize = 512, CheckWaitSeconds = 15 };

/* Component resources and task sinks use the adapter's own boundary. No fake
 * FSKit device proxy, native mount, host block device or system service is used. */
@interface CheckImage : NSObject <Ext4BlockWriter, Ext4PersistenceBarrier>
@property NSMutableData *bytes;
@property(nonatomic, getter=isRevoked) BOOL revoked;
@property BOOL failRead;
@property BOOL failWrite;
@property BOOL failFlush;
@property NSUInteger writes;
@property NSUInteger flushes;
@property dispatch_semaphore_t entered;
@property dispatch_semaphore_t resume;
@property BOOL gateWrites;
@end

@implementation CheckImage

- (uint64_t)blockSize
{
	return CheckSectorSize;
}

- (uint64_t)physicalBlockSize
{
	return CheckSectorSize;
}

- (uint64_t)blockCount
{
	return self.bytes.length / CheckSectorSize;
}

- (size_t)readInto:(void *)buffer
	startingAt:(off_t)offset
	    length:(size_t)length
	     error:(NSError **)error
{
	assert(offset >= 0 && (uint64_t)offset <= self.bytes.length);
	assert(length <= self.bytes.length - (uint64_t)offset);
	if (self.entered != nil && !self.gateWrites) {
		dispatch_semaphore_signal(self.entered);
		assert(dispatch_semaphore_wait(self.resume,
			   dispatch_time(DISPATCH_TIME_NOW, CheckWaitSeconds * NSEC_PER_SEC)) == 0);
		self.entered = nil;
	}
	if (self.failRead) {
		*error = [NSError errorWithDomain:NSPOSIXErrorDomain code:EIO userInfo:nil];
		return 0;
	}
	memcpy(buffer, (const uint8_t *)self.bytes.bytes + offset, length);
	return length;
}

- (size_t)writeFrom:(void *)buffer
	 startingAt:(off_t)offset
	     length:(size_t)length
	      error:(NSError **)error
{
	assert(offset >= 0 && (uint64_t)offset <= self.bytes.length);
	assert(length <= self.bytes.length - (uint64_t)offset);
	if (self.entered != nil && self.gateWrites) {
		dispatch_semaphore_signal(self.entered);
		assert(dispatch_semaphore_wait(self.resume,
			   dispatch_time(DISPATCH_TIME_NOW, CheckWaitSeconds * NSEC_PER_SEC)) == 0);
		self.entered = nil;
	}
	if (self.failWrite) {
		*error = [NSError errorWithDomain:NSPOSIXErrorDomain code:EIO userInfo:nil];
		return 0;
	}
	self.writes++;
	memcpy((uint8_t *)self.bytes.mutableBytes + offset, buffer, length);
	return length;
}

- (BOOL)synchronizeWithError:(NSError **)error
{
	self.flushes++;
	if (self.failFlush) {
		*error = [NSError errorWithDomain:NSPOSIXErrorDomain code:EIO userInfo:nil];
	}
	return !self.failFlush;
}

@end

@interface CheckLog : NSObject
@property NSMutableArray<NSString *> *messages;
@end
@implementation CheckLog

- (void)logMessage:(NSString *)message
{
	[self.messages addObject:message];
}

@end

static Ext4ResourceIO *
check_resource(CheckImage *image, BOOL writable)
{
	Ext4ResourceIO *resource = [[Ext4ResourceIO alloc] initWithReader:image];

	assert(resource != nil);
	if (writable) {
		[resource enableWritesWithBarrier:image deviceName:@"component-check-resource"];
	}
	return resource;
}

static Ext4CheckTask *
check_task(Ext4ResourceIO *resource, Ext4CheckMode mode, NSURL *executable)
{
	Ext4CheckTask *task = [[Ext4CheckTask alloc] initWithResource:resource
								 mode:mode
							   executable:executable];

	assert(task != nil);
	return task;
}

int
main(int argc, char **argv)
{
	@autoreleasepool {
		NSURL *helper;
		NSURL *peer;
		NSData *clean;
		NSData *damaged;
		NSData *before;
		NSString *embeddedNull = [[NSString alloc] initWithBytes:"a\0b"
								  length:3
								encoding:NSUTF8StringEncoding];
		CheckImage *image = [CheckImage new];
		CheckLog *log = [CheckLog new];
		Ext4ResourceIO *resource;
		Ext4CheckTask *check;
		Ext4FormatTask *format;
		NSUUID *formatUUID = NSUUID.UUID;
		NSUInteger formatBlockSize;
		Ext4Volume *volume;
		struct ext4_fs *filesystem = NULL;
		struct ext4_info info;
		NSError *error = nil;
		__block NSError *cancelError;
		__block FSItem *root;
		dispatch_group_t work;
		Ext4CheckMode mode;
		int expected;

		assert(argc == 7);
		helper = [NSURL fileURLWithPath:@(argv[1])];
		peer = [NSURL fileURLWithPath:@(argv[2])];
		clean = [NSData dataWithContentsOfFile:@(argv[3])];
		damaged = [NSData dataWithContentsOfFile:@(argv[4])];
		assert(clean != nil && damaged != nil);
		log.messages = [NSMutableArray array];
		image.bytes = [clean mutableCopy];
		resource = check_resource(image, NO);
		assert([resource open:&filesystem] == EXT4_OK);
		volume = ext4_volume_create(nil, filesystem, resource, NULL, NO);
		assert(volume != nil);
		check = [volume beginCheck:Ext4CheckVerify executable:helper error:&error];
		assert(check != nil && error == nil);
		assert([volume checkMountEligibility].code == EBUSY);
		assert([volume finishUnloadedResource].code == EBUSY);
		assert([check runWithTask:(FSTask *)log] == nil);
		assert([volume finishCheck:check error:nil] == nil);
		assert([volume checkMountEligibility] == nil);
		assert(
		    [image.bytes isEqualToData:clean] && image.writes == 0 && image.flushes == 0);
		[volume activateWithOptions:nil
			       replyHandler:^(FSItem *item, NSError *failure) {
				 assert(failure == nil && item != nil);
				 root = item;
			       }];
		assert([volume beginCheck:Ext4CheckVerify executable:helper error:&error] == nil);
		assert(error.code == EBUSY);
		[volume invalidate];
		root = nil;

		image.failRead = YES;
		error = [check_task(resource, Ext4CheckVerify, helper) runWithTask:(FSTask *)log];
		assert(error.code == EIO && [image.bytes isEqualToData:clean]);
		image.failRead = NO;
		for (mode = Ext4CheckVerify; mode <= Ext4CheckPreen; mode++) {
			resource = check_resource(image, mode != Ext4CheckVerify);
			error = [check_task(resource, mode, peer) runWithTask:(FSTask *)log];
			expected = mode == Ext4CheckVerify ? EROFS : EPROTO;
			assert(error.code == expected && image.writes == 0);
		}

		resource = check_resource(image, NO);
		check = check_task(resource, Ext4CheckVerify, helper);
		[check cancel];
		assert([check runWithTask:(FSTask *)log].code == ECANCELED);
		image.entered = dispatch_semaphore_create(0);
		image.resume = dispatch_semaphore_create(0);
		check = check_task(resource, Ext4CheckVerify, helper);
		work = dispatch_group_create();
		dispatch_group_async(work, dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{
		  cancelError = [check runWithTask:(FSTask *)log];
		});
		assert(dispatch_semaphore_wait(image.entered,
			   dispatch_time(DISPATCH_TIME_NOW, CheckWaitSeconds * NSEC_PER_SEC)) == 0);
		[check cancel];
		dispatch_semaphore_signal(image.resume);
		assert(dispatch_group_wait(work,
			   dispatch_time(DISPATCH_TIME_NOW, CheckWaitSeconds * NSEC_PER_SEC)) == 0);
		assert(cancelError.code == ECANCELED && [image.bytes isEqualToData:clean]);

		image.bytes = [damaged mutableCopy];
		resource = check_resource(image, YES);
		image.failWrite = YES;
		assert([check_task(resource, Ext4CheckRepair, helper) runWithTask:(FSTask *)log]
			   .code == EIO);
		assert([image.bytes isEqualToData:damaged]);
		image.failWrite = NO;
		image.failFlush = YES;
		assert([check_task(resource, Ext4CheckRepair, helper) runWithTask:(FSTask *)log]
			   .code == EIO);
		image.failFlush = NO;
		image.bytes = [damaged mutableCopy];
		assert([resource inspect:&info] == EXT4_OK);
		volume =
		    ext4_volume_create_for_check(nil, &info, resource, NULL, YES, EXT4_CORRUPT);
		assert(volume != nil);
		assert([volume checkMountEligibility].code == EIO);
		check = [volume beginCheck:Ext4CheckRepair executable:helper error:&error];
		assert(check != nil);
		error = [check runWithTask:(FSTask *)log];
		assert([volume finishCheck:check error:error] == nil);
		assert([volume checkMountEligibility] == nil && image.writes != 0 &&
		    image.flushes != 0);
		assert([volume finishUnloadedResource] == nil);
		[volume invalidate];
		assert([image.bytes writeToFile:@(argv[5]) atomically:YES]);
		resource = check_resource(image, NO);
		assert([check_task(resource, Ext4CheckVerify, helper) runWithTask:(FSTask *)log] ==
		    nil);

		/* Formatting replaces the old identity only after exclusive admission.
		 * Its resource remains valid after the previous volume is invalidated. */
		assert([resource inspect:&info] == EXT4_OK);
		formatBlockSize = info.block_size;
		assert([[Ext4FormatTask alloc] initWithResource:resource
						      blockSize:8192
							   name:@""
							   uuid:formatUUID
						     executable:helper] == nil);
		assert([[Ext4FormatTask alloc] initWithResource:resource
						      blockSize:formatBlockSize
							   name:@"abcdefghijklmnopq"
							   uuid:formatUUID
						     executable:helper] == nil);
		assert([[Ext4FormatTask alloc] initWithResource:resource
						      blockSize:formatBlockSize
							   name:embeddedNull
							   uuid:formatUUID
						     executable:helper] == nil);
		assert([resource open:&filesystem] == EXT4_OK);
		volume = ext4_volume_create(nil, filesystem, resource, NULL, NO);
		assert([volume beginFormatWithBlockSize:formatBlockSize
						   name:@""
						   uuid:formatUUID
					     executable:helper
						  error:&error] == nil);
		assert(error.code == EROFS);
		[volume invalidate];
		image.bytes = [clean mutableCopy];
		resource = check_resource(image, YES);
		assert([resource openWritable:&filesystem] == EXT4_OK);
		volume = ext4_volume_create(nil, filesystem, resource, NULL, YES);
		assert(volume != nil);
		@autoreleasepool {
			[volume activateWithOptions:nil
				       replyHandler:^(FSItem *item, NSError *failure) {
					 assert(item != nil && failure == nil);
					 root = item;
				       }];
			assert([volume beginFormatWithBlockSize:formatBlockSize
							   name:@""
							   uuid:formatUUID
						     executable:helper
							  error:&error] == nil);
			assert(error.code == EBUSY);
			[volume reclaimItem:root
			       replyHandler:^(NSError *failure) {
				 assert(failure == nil);
			       }];
			root = nil;
		}

		format = [volume beginFormatWithBlockSize:formatBlockSize
						     name:@"Formatted"
						     uuid:formatUUID
					       executable:helper
						    error:&error];
		assert(format != nil && error == nil);
		assert([volume checkMountEligibility].code == EBUSY);
		assert([volume finishUnloadedResource].code == EBUSY);
		assert([format runWithTask:(FSTask *)log] == nil);
		assert([volume finishFormat:format error:nil] == nil);
		assert([volume checkMountEligibility].code == ESTALE);
		assert([resource synchronize] == EXT4_OK);
		assert([resource open:&filesystem] == EXT4_OK);
		ext4_get_info(filesystem, &info);
		assert([formatUUID isEqual:[[NSUUID alloc] initWithUUIDBytes:info.uuid]]);
		assert(info.block_size == formatBlockSize &&
		    memcmp(info.volume_name, "Formatted", sizeof("Formatted")) == 0);
		ext4_unmount(filesystem);
		assert([image.bytes writeToFile:@(argv[6]) atomically:YES]);

		image.bytes = [NSMutableData dataWithLength:clean.length];
		before = [image.bytes copy];
		image.failWrite = YES;
		format = [[Ext4FormatTask alloc] initWithResource:resource
							blockSize:formatBlockSize
							     name:@""
							     uuid:formatUUID
						       executable:helper];
		assert([format runWithTask:(FSTask *)log].code == EIO);
		assert([image.bytes isEqualToData:before]);
		image.failWrite = NO;
		image.failFlush = YES;
		format = [[Ext4FormatTask alloc] initWithResource:resource
							blockSize:formatBlockSize
							     name:@""
							     uuid:formatUUID
						       executable:helper];
		assert([format runWithTask:(FSTask *)log].code == EIO);
		image.failFlush = NO;
		image.bytes = [NSMutableData dataWithLength:clean.length];
		image.gateWrites = YES;
		image.entered = dispatch_semaphore_create(0);
		image.resume = dispatch_semaphore_create(0);
		format = [[Ext4FormatTask alloc] initWithResource:resource
							blockSize:formatBlockSize
							     name:@""
							     uuid:formatUUID
						       executable:helper];
		dispatch_group_async(work, dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{
		  cancelError = [format runWithTask:(FSTask *)log];
		});
		assert(dispatch_semaphore_wait(image.entered,
			   dispatch_time(DISPATCH_TIME_NOW, CheckWaitSeconds * NSEC_PER_SEC)) == 0);
		[format cancel];
		dispatch_semaphore_signal(image.resume);
		assert(dispatch_group_wait(work,
			   dispatch_time(DISPATCH_TIME_NOW, CheckWaitSeconds * NSEC_PER_SEC)) == 0);
		assert(cancelError.code == ECANCELED);
		assert(log.messages.count != 0);
		puts("FSKit check ownership, resource failures, dishonest peers, cancellation and "
		     "repair/format passed");
	}
	return 0;
}

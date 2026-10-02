/* SPDX-License-Identifier: BSD-3-Clause */
#import "../adapters/fskit/Ext4VolumeInternal.h"
#import "../adapters/fskit/Ext4FileSystemInternal.h"
#import "../adapters/fskit/Ext4DeviceBarrier.h"
#import <Security/Security.h>
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

/* The filesystem entry points are linked, but these host-only components must
 * never access a Keychain or connect to the privileged device service. */
OSStatus
SecItemCopyMatching(CFDictionaryRef query, CFTypeRef *result)
{
	(void)query;
	(void)result;
	assert(NO);
	return errSecUnimplemented;
}

OSStatus
SecItemAdd(CFDictionaryRef query, CFTypeRef *result)
{
	(void)query;
	(void)result;
	assert(NO);
	return errSecUnimplemented;
}

OSStatus
SecItemDelete(CFDictionaryRef query)
{
	(void)query;
	assert(NO);
	return errSecUnimplemented;
}

NSString *
ext4_peer_requirement(NSString *identifier)
{
	(void)identifier;
	assert(NO);
	return nil;
}

@interface CheckOptions : NSObject
@property(nonatomic, copy) NSArray<NSString *> *taskOptions;
@end

@implementation CheckOptions
@end

@interface CheckCompletion : CheckLog
@property(nonatomic, copy) NSError * (^cancellationHandler)(void);
@property dispatch_semaphore_t completed;
@property NSError *failure;
@property NSUInteger completionCount;
@end

@implementation CheckCompletion

- (void)didCompleteWithError:(NSError *)error
{
	@synchronized(self) {
		self.failure = error;
		self.completionCount++;
		/* Match the public task's completion-time handler release. */
		self.cancellationHandler = nil;
	}
	dispatch_semaphore_signal(self.completed);
}

@end

/* Inject the adapter's resource boundary after load, without manufacturing an
 * FSKit device proxy. Exercise the same temporary unary-volume factory used by
 * forced loads; actual daemon admission still requires the native suite. */
@interface Ext4FileSystem (ComponentTesting)
- (void)installComponentResource:(Ext4ResourceIO *)resource;
- (BOOL)componentMaintenanceActive;
- (Ext4Volume *)componentVolume;
@end

@implementation Ext4FileSystem (ComponentTesting)

- (void)installComponentResource:(Ext4ResourceIO *)resource
{
	@synchronized(self) {
		FSStatFSResult *statistics;

		assert(_resourceOwner == nil && _maintenanceTask == nil && _volume == nil);
		_resourceOwner = resource;
		_volume = ext4_volume_create_for_check(
		    nil, NULL, resource, NULL, resource.writable, EXT4_CORRUPT);
		assert(_volume != nil && _volume.maintenanceOnly);
		statistics = _volume.volumeStatistics;
		assert(statistics != nil && statistics.blockSize == (NSInteger)resource.blockSize);
		assert(statistics.ioSize == statistics.blockSize);
		assert(statistics.totalBlocks == 0 && statistics.totalFiles == 0);
		assert(statistics.freeBlocks == 0 && statistics.availableBlocks == 0);
		assert(statistics.usedBlocks == 0 && statistics.freeFiles == 0);
		assert(_volume.maximumFileSize == 0);
		assert(!_volume.supportedVolumeCapabilities.supportsPersistentObjectIDs);
		assert([_volume checkMountEligibility].code == EIO);
		[_volume activateWithOptions:nil
				replyHandler:^(FSItem *item, NSError *failure) {
				  assert(item == nil && failure.code == EIO);
				}];
		self.containerStatus =
		    [FSContainerStatus blockedWithStatus:[NSError errorWithDomain:NSPOSIXErrorDomain
									     code:EIO
									 userInfo:nil]];
	}
}

- (BOOL)componentMaintenanceActive
{
	@synchronized(self) {
		return _maintenanceTask != nil;
	}
}

- (Ext4Volume *)componentVolume
{
	@synchronized(self) {
		return _volume;
	}
}

@end

@interface CheckFileSystem : Ext4FileSystem
@property CheckImage *image;
@property BOOL failFinalBarrier;
@property BOOL failValidationRead;
@property NSUInteger validations;
@end

@implementation CheckFileSystem

- (NSError *)validateResourceAfterMaintenanceWriting:(BOOL)writing uuid:(NSUUID *)uuid
{
	self.validations++;
	/* Inject only after the child has completed successfully. This separates
	 * final parent validation from an error reported inside the helper. */
	self.image.failFlush = self.failFinalBarrier;
	self.image.failRead = self.failValidationRead;
	return [super validateResourceAfterMaintenanceWriting:writing uuid:uuid];
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

static CheckCompletion *
check_completion(void)
{
	CheckCompletion *task = [CheckCompletion new];

	task.messages = [NSMutableArray array];
	task.completed = dispatch_semaphore_create(0);
	return task;
}

static FSTaskOptions *
check_options(NSArray<NSString *> *arguments)
{
	CheckOptions *options = [CheckOptions new];

	options.taskOptions = arguments;
	return (FSTaskOptions *)options;
}

static void
check_completed(CheckCompletion *task, NSProgress *progress, NSInteger error)
{
	assert(dispatch_semaphore_wait(task.completed,
		   dispatch_time(DISPATCH_TIME_NOW, CheckWaitSeconds * NSEC_PER_SEC)) == 0);
	assert(task.completionCount == 1 && task.failure.code == error);
	assert(progress.completedUnitCount == progress.totalUnitCount);
}

static void
check_refusal(CheckCompletion *task, NSProgress *progress, NSError *failure, NSInteger error)
{
	assert(progress != nil && !progress.cancellable && failure == nil);
	check_completed(task, progress, error);
	assert(task.cancellationHandler == nil);
}

static NSError *
check_unload(Ext4FileSystem *filesystem)
{
	FSResource *transport = [[FSGenericURLResource alloc]
	    initWithURL:[NSURL URLWithString:@"machlin-component://maintenance"]];
	dispatch_semaphore_t completed = dispatch_semaphore_create(0);
	__block NSError *failure;

	/* The public method requires transport arguments even though this adapter
	 * releases the separately retained resource boundary, not this URL. */
	assert(transport != nil);
	[filesystem unloadResource:transport
			   options:check_options(@[])
		      replyHandler:^(NSError *error) {
			failure = error;
			dispatch_semaphore_signal(completed);
		      }];
	assert(dispatch_semaphore_wait(completed,
		   dispatch_time(DISPATCH_TIME_NOW, CheckWaitSeconds * NSEC_PER_SEC)) == 0);
	return failure;
}

static void
check_filesystem_maintenance(
    NSData *clean, NSData *damaged, NSUInteger blockSize, NSString *export, NSString *embeddedNull)
{
	CheckImage *image = [CheckImage new];
	Ext4ResourceIO *resource;
	CheckFileSystem *filesystem;
	Ext4Volume *temporary;
	CheckCompletion *task;
	CheckCompletion *other;
	NSProgress *progress;
	NSProgress *refusal;
	NSError *failure = nil;
	NSArray *invalid = @[
		@[ @"-b" ], @[ @"-b", @"8192" ], @[ @"-L" ], @[ @"--unknown" ],
		@[ @"-b1024", @"-b4096" ], @[ @"-Lone", @"-Ltwo" ], @[ @"-Labcdefghijklmnopq" ],
		@[ @"-L", embeddedNull ]
	];
	NSArray *arguments = @[
		[NSString stringWithFormat:@"-b%lu", (unsigned long)blockSize], @"-Lfilesystem-test"
	];
	NSArray *invalidCheck =
	    @[ @[ @"--unknown" ], @[ @"-n", @"-y" ], @[ @"-n", @"-p" ], @[ @"-y", @"-p" ] ];
	struct ext4_fs *engine = NULL;
	struct ext4_info info;
	NSUInteger index;
	NSUInteger fault;
	NSUInteger writes;
	NSUInteger flushes;
	__block NSError *cancelError;
	dispatch_group_t cancellation;
	NSError * (^cancel)(void);
	dispatch_block_t cancelProgress;

	image.bytes = [NSMutableData dataWithLength:clean.length];
	filesystem = [CheckFileSystem new];
	task = check_completion();
	refusal = [filesystem startFormatWithTask:(FSTask *)task
					  options:check_options(arguments)
					    error:&failure];
	check_refusal(task, refusal, failure, ENXIO);
	[filesystem installComponentResource:check_resource(image, NO)];
	temporary = filesystem.componentVolume;
	task = check_completion();
	refusal = [filesystem startFormatWithTask:(FSTask *)task
					  options:check_options(arguments)
					    error:&failure];
	check_refusal(task, refusal, failure, EROFS);
	assert(image.writes == 0 && image.flushes == 0);
	assert(check_unload(filesystem) == nil);
	/* FSKit may retain a retired identity after its resource is released. */
	assert(temporary.volumeStatistics.blockSize > 0);
	assert(temporary.volumeStatistics.totalBlocks == 0);

	resource = check_resource(image, YES);
	[filesystem installComponentResource:resource];
	filesystem.image = image;
	for (index = 0; index < invalid.count; index++) {
		task = check_completion();
		refusal = [filesystem startFormatWithTask:(FSTask *)task
						  options:check_options(invalid[index])
						    error:&failure];
		check_refusal(task, refusal, failure, EINVAL);
		assert(!filesystem.componentMaintenanceActive);
		assert(image.writes == 0 && image.flushes == 0);
	}
	for (index = 0; index < invalidCheck.count; index++) {
		task = check_completion();
		refusal = [filesystem startCheckWithTask:(FSTask *)task
						 options:check_options(invalidCheck[index])
						   error:&failure];
		check_refusal(task, refusal, failure, EINVAL);
		assert(!filesystem.componentMaintenanceActive);
		assert(image.writes == 0 && image.flushes == 0);
	}
	image.entered = dispatch_semaphore_create(0);
	image.resume = dispatch_semaphore_create(0);
	image.gateWrites = YES;
	failure = nil;
	task = check_completion();
	progress = [filesystem startFormatWithTask:(FSTask *)task
					   options:check_options(arguments)
					     error:&failure];
	assert(progress != nil && failure == nil);
	assert(dispatch_semaphore_wait(image.entered,
		   dispatch_time(DISPATCH_TIME_NOW, CheckWaitSeconds * NSEC_PER_SEC)) == 0);
	other = check_completion();
	refusal = [filesystem startFormatWithTask:(FSTask *)other
					  options:check_options(arguments)
					    error:&failure];
	check_refusal(other, refusal, failure, EBUSY);
	assert(filesystem.componentMaintenanceActive);
	other = check_completion();
	refusal = [filesystem startCheckWithTask:(FSTask *)other
					 options:check_options(@[ @"-f", @"-n" ])
					   error:&failure];
	check_refusal(other, refusal, failure, EBUSY);
	assert(check_unload(filesystem).code == EBUSY && filesystem.componentMaintenanceActive);
	dispatch_semaphore_signal(image.resume);
	check_completed(task, progress, 0);
	assert(filesystem.containerStatus.state == FSContainerStateReady);
	assert(!filesystem.componentMaintenanceActive && filesystem.validations == 1);
	assert(filesystem.componentVolume == nil);
	assert([resource open:&engine] == EXT4_OK);
	ext4_get_info(engine, &info);
	assert(info.block_size == blockSize && strcmp(info.volume_name, "filesystem-test") == 0);
	ext4_unmount(engine);
	assert([image.bytes writeToFile:export atomically:YES]);
	assert(check_unload(filesystem) == nil);

	/* Successful maintenance releases the temporary unary identity. Validation
	 * cannot turn that identity into a mountable filesystem or reuse its UUID. */
	writes = image.writes;
	flushes = image.flushes;
	[filesystem installComponentResource:check_resource(image, NO)];
	task = check_completion();
	failure = nil;
	progress = [filesystem startCheckWithTask:(FSTask *)task
					  options:check_options(@[ @"-f", @"-n" ])
					    error:&failure];
	assert(progress != nil && failure == nil);
	check_completed(task, progress, 0);
	assert(image.writes == writes && image.flushes == flushes);
	assert(filesystem.componentVolume == nil);
	assert(check_unload(filesystem) == nil);
	image.bytes = [damaged mutableCopy];
	[filesystem installComponentResource:check_resource(image, YES)];
	task = check_completion();
	progress = [filesystem startCheckWithTask:(FSTask *)task
					  options:check_options(@[ @"-f", @"-y" ])
					    error:&failure];
	assert(progress != nil);
	check_completed(task, progress, 0);
	assert(filesystem.containerStatus.state == FSContainerStateReady);
	assert(filesystem.componentVolume == nil);
	assert(check_unload(filesystem) == nil);

	for (fault = 0; fault < 2; fault++) {
		image.bytes = [NSMutableData dataWithLength:clean.length];
		filesystem.failFinalBarrier = fault == 0;
		filesystem.failValidationRead = fault == 1;
		[filesystem installComponentResource:check_resource(image, YES)];
		task = check_completion();
		failure = nil;
		progress = [filesystem startFormatWithTask:(FSTask *)task
						   options:check_options(nil)
						     error:&failure];
		assert(progress != nil && failure == nil);
		check_completed(task, progress, EIO);
		assert(filesystem.containerStatus.state == FSContainerStateBlocked);
		assert(!filesystem.componentMaintenanceActive);
		image.failRead = NO;
		image.failFlush = NO;
		assert(check_unload(filesystem) == nil);
	}

	filesystem.failFinalBarrier = NO;
	filesystem.failValidationRead = NO;
	image.bytes = [NSMutableData dataWithLength:clean.length];
	image.entered = dispatch_semaphore_create(0);
	image.resume = dispatch_semaphore_create(0);
	[filesystem installComponentResource:check_resource(image, YES)];
	task = check_completion();
	progress = [filesystem startFormatWithTask:(FSTask *)task
					   options:check_options(arguments)
					     error:&failure];
	assert(progress != nil);
	assert(dispatch_semaphore_wait(image.entered,
		   dispatch_time(DISPATCH_TIME_NOW, CheckWaitSeconds * NSEC_PER_SEC)) == 0);
	cancel = task.cancellationHandler;
	cancelProgress = progress.cancellationHandler;
	assert(cancel != nil);
	assert(cancelProgress != nil);
	/* Cancel through progress before releasing the gated I/O. The task
	 * handler then exercises idempotent cancellation and waits for reaping. */
	cancelProgress();
	cancellation = dispatch_group_create();
	dispatch_group_async(cancellation, dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{
	  cancelError = cancel();
	});
	dispatch_semaphore_signal(image.resume);
	assert(dispatch_group_wait(cancellation,
		   dispatch_time(DISPATCH_TIME_NOW, CheckWaitSeconds * NSEC_PER_SEC)) == 0);
	check_completed(task, progress, ECANCELED);
	assert(cancelError == nil && !filesystem.componentMaintenanceActive);
	assert(filesystem.containerStatus.state == FSContainerStateBlocked);
	assert(check_unload(filesystem) == nil);
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

		assert(argc == 8);
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
		check_filesystem_maintenance(
		    clean, damaged, formatBlockSize, @(argv[7]), embeddedNull);
		puts("FSKit check ownership, resource failures, dishonest peers, cancellation and "
		     "repair/format passed");
	}
	return 0;
}

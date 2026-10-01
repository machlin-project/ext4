/* SPDX-License-Identifier: BSD-3-Clause */
#import "Ext4FileSystem.h"
#import "Ext4Volume.h"
#import "Ext4VolumeInternal.h"
#import "Ext4ResourceIO.h"
#import "Ext4Support.h"
#import "Ext4KeyStore.h"
#import "Ext4DeviceBarrier.h"
#include "Ext4Crypto.h"
#include <errno.h>

static enum ext4_result
ext4_open_resource(FSResource *resource, Ext4ResourceIO **owner, struct ext4_fs **fs)
{
	*fs = NULL;
	*owner = nil;
	if (![resource isKindOfClass:FSBlockDeviceResource.class]) {
		return EXT4_UNSUPPORTED;
	}
	*owner = [[Ext4ResourceIO alloc] initWithReader:(id<Ext4BlockReader>)resource];
	if (*owner == nil) {
		return EXT4_CORRUPT;
	}
	return [*owner open:fs];
}

@implementation Ext4FileSystem {
	Ext4Volume *_volume;
	BOOL _recoveredOnLoad;
}

- (NSProgress *)startCheckWithTask:(FSTask *)task
			   options:(FSTaskOptions *)options
			     error:(NSError **)error
{
	Ext4Volume *volume;
	NSProgress *progress;
	Ext4CheckTask *check;
	dispatch_group_t work;
	NSURL *executable;
	BOOL automaticRecovery;
	BOOL quick = [options.taskOptions containsObject:@"-q"];
	BOOL force = [options.taskOptions containsObject:@"-f"];
	BOOL verify = [options.taskOptions containsObject:@"-n"];
	BOOL repair = [options.taskOptions containsObject:@"-y"];
	BOOL preen = [options.taskOptions containsObject:@"-p"];
	Ext4CheckMode mode = repair ? Ext4CheckRepair : (preen ? Ext4CheckPreen : Ext4CheckVerify);

	@synchronized(self) {
		volume = _volume;
		automaticRecovery = _recoveredOnLoad && volume.writable;
	}
	for (NSString *option in options.taskOptions) {
		if (![@[ @"-q", @"-n", @"-p", @"-y", @"-f" ] containsObject:option] ||
		    (verify && (repair || preen)) || (repair && preen)) {
			if (error != NULL) {
				*error = ext4_error(EXT4_INVALID_ARGUMENT);
			}
			return nil;
		}
		if (![option isEqualToString:@"-q"] && ![option isEqualToString:@"-n"]) {
			quick = NO;
		}
		if (![option isEqualToString:@"-p"] && ![option isEqualToString:@"-y"]) {
			automaticRecovery = NO;
		}
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
	/* Keep Disk Arbitration's ordinary quick/recovery path small. Explicit
	 * forced checks always run the separate full checker. */
	if (!force && (quick || automaticRecovery)) {
		progress.cancellable = NO;
		dispatch_async(dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{
		  NSError *checkError = [volume checkMountEligibility];

		  progress.completedUnitCount = 1;
		  [task didCompleteWithError:checkError];
		});
		return progress;
	}
	executable = [NSBundle.mainBundle.bundleURL
	    URLByAppendingPathComponent:@"Contents/Helpers/Ext4CheckResource"];
	check = [volume beginCheck:mode executable:executable error:error];
	if (check == nil) {
		return nil;
	}
	work = dispatch_group_create();
	dispatch_group_enter(work);
	progress.cancellable = YES;
	progress.localizedDescription =
	    mode == Ext4CheckVerify ? @"Checking ext4" : @"Repairing ext4";
	progress.cancellationHandler = ^{
	  [check cancel];
	};
	task.cancellationHandler = ^NSError * {
	  [check cancel];
	  if (dispatch_group_wait(work, dispatch_time(DISPATCH_TIME_NOW, 45 * NSEC_PER_SEC)) != 0) {
		  return [NSError errorWithDomain:NSPOSIXErrorDomain code:ETIMEDOUT userInfo:nil];
	  }
	  return nil;
	};
	dispatch_async(dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{
	  NSError *checkError;

	  checkError = [volume finishCheck:check error:[check runWithTask:task]];
	  @synchronized(self) {
		  if (self->_volume == volume) {
			  self.containerStatus = checkError == nil
			      ? FSContainerStatus.ready
			      : [FSContainerStatus blockedWithStatus:checkError];
		  }
	  }
	  progress.completedUnitCount = 1;
	  progress.cancellationHandler = nil;
	  [task didCompleteWithError:checkError];
	  dispatch_group_leave(work);
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
		*error = ext4_error(EXT4_UNSUPPORTED);
	}
	return nil;
}

- (void)probeResource:(FSResource *)resource
	 replyHandler:(void (^)(FSProbeResult *, NSError *))reply
{
	struct ext4_fs *fs = NULL;
	__attribute__((objc_precise_lifetime)) Ext4ResourceIO *owner = nil;
	struct ext4_info info;
	NSUUID *uuid;
	NSString *name;
	FSContainerIdentifier *identifier;
	BOOL needsCheck;
	enum ext4_result error;

	error = ext4_open_resource(resource, &owner, &fs);
	needsCheck = error == EXT4_RECOVERY_REQUIRED || error == EXT4_CORRUPT;
	if (needsCheck) {
		error = [owner inspect:&info];
	}
	if (error != EXT4_OK) {
		reply(error == EXT4_NOT_EXT4 ? FSProbeResult.notRecognizedProbeResult : nil,
		    ext4_error(error == EXT4_NOT_EXT4 ? EXT4_OK : error));
		return;
	}
	if (!needsCheck) {
		ext4_get_info(fs, &info);
	}
	name = [[NSString alloc] initWithBytes:info.volume_name
					length:strnlen(info.volume_name, EXT4_VOLUME_NAME_SIZE)
				      encoding:NSUTF8StringEncoding];
	if (name.length == 0) {
		name = @"ext4";
	}
	uuid = [[NSUUID alloc] initWithUUIDBytes:info.uuid];
	identifier = [[FSContainerIdentifier alloc] initWithUUID:uuid];
	ext4_unmount(fs);
	/* A validated superblock identifies supported media even when its root
	 * needs repair. Load still cannot publish items or mount a damaged root. */
	reply([FSProbeResult usableProbeResultWithName:name containerID:identifier], nil);
}

- (void)loadResource:(FSResource *)resource
	     options:(FSTaskOptions *)options
	replyHandler:(void (^)(FSVolume *, NSError *))reply
{
	struct ext4_fs *fs = NULL;
	__attribute__((objc_precise_lifetime)) Ext4ResourceIO *owner = nil;
	Ext4Volume *volume = nil;
	struct ext4_native_crypto *crypto = NULL;
	struct ext4_info info;
	NSError *keyError = nil;
	NSError *loadError = nil;
	NSError *writeError = nil;
	BOOL writable = NO;
	BOOL inspectable = NO;
	enum ext4_result error;

	@synchronized(self) {
		if (_volume != nil) {
			loadError = [NSError errorWithDomain:NSPOSIXErrorDomain
							code:EBUSY
						    userInfo:nil];
		} else {
			_recoveredOnLoad = NO;
			error = ext4_open_resource(resource, &owner, &fs);
			if (error == EXT4_OK || error == EXT4_RECOVERY_REQUIRED ||
			    error == EXT4_CORRUPT) {
				/* Keychain access must not delay the heartbeat of a claimed MMP
				 * owner. Resolve keys before writable admission and recovery. */
				if (error == EXT4_OK) {
					ext4_get_info(fs, &info);
					inspectable = YES;
				} else {
					enum ext4_result inspected = [owner inspect:&info];

					if (inspected != EXT4_OK) {
						error = inspected;
					} else {
						inspectable = YES;
					}
				}
				if (inspectable) {
					crypto = [Ext4KeyStore
					    loadCryptoForVolume:[[NSUUID alloc]
								    initWithUUIDBytes:info.uuid]
							  error:&keyError];
				}
			}
#if DEBUG
			NSLog(@"Machlin ext4 load build %@: readonly=%d resourceWritable=%d "
			      @"result=%d",
			    [NSBundle bundleForClass:self.class].infoDictionary[@"CFBundleVersion"],
			    [options.taskOptions containsObject:@"--rdonly"],
			    [resource isKindOfClass:FSBlockDeviceResource.class] &&
				[(FSBlockDeviceResource *)resource isWritable],
			    error);
#endif
			if (inspectable && ![options.taskOptions containsObject:@"--rdonly"] &&
			    [(FSBlockDeviceResource *)resource isWritable]) {
				FSBlockDeviceResource *device = (FSBlockDeviceResource *)resource;
				Ext4DeviceBarrier *barrier =
				    [[Ext4DeviceBarrier alloc] initWithDevice:device.BSDName
								    blockSize:device.blockSize
								   blockCount:device.blockCount
									error:&writeError];

				if (barrier != nil) {
					struct ext4_recovery_report report;

					ext4_unmount(fs);
					fs = NULL;
					[owner enableWritesWithBarrier:(id<Ext4PersistenceBarrier>)
									   barrier
							    deviceName:device.BSDName];
					/* The writable FSKit resource is exclusively owned during
					 * load. Recovery is a separate capability, never a
					 * read-only side effect. */
					if (error == EXT4_RECOVERY_REQUIRED) {
						error = [owner recover:&report];
						_recoveredOnLoad = error == EXT4_OK;
					}
					if (error == EXT4_OK) {
						error = [owner openWritable:&fs];
					}
					writable = error == EXT4_OK;
				}
			}
			if (error == EXT4_OK) {
				volume = ext4_volume_create(
				    (FSBlockDeviceResource *)resource, fs, owner, crypto, writable);
				if (volume == nil) {
					if (writable && ext4_sync(fs) == EXT4_OK) {
						(void)ext4_mmp_release(fs);
					}
					ext4_unmount(fs);
					error = EXT4_NO_MEMORY;
				}
			} else if (inspectable &&
			    (error == EXT4_CORRUPT || error == EXT4_RECOVERY_REQUIRED)) {
				volume =
				    ext4_volume_create_for_check((FSBlockDeviceResource *)resource,
					&info, owner, crypto, owner.writable, error);
				if (volume == nil) {
					error = EXT4_NO_MEMORY;
				}
			}
			if (volume == nil) {
				ext4_native_crypto_destroy(crypto);
			}
			volume.keyStoreError = keyError;
			volume.writeAvailabilityError = writeError;
			loadError = volume != nil ? nil : ext4_error(error);
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
	NSError *error;

	(void)resource;
	(void)options;
	@synchronized(self) {
		error = [_volume finishUnloadedResource];
		if (error.code == EBUSY) {
			reply(error);
			return;
		}
		[_volume invalidate];
		_volume = nil;
		_recoveredOnLoad = NO;
		self.containerStatus = [FSContainerStatus
		    notReadyWithStatus:[NSError errorWithDomain:NSPOSIXErrorDomain
							   code:ENXIO
						       userInfo:nil]];
	}
	reply(error);
}

@end

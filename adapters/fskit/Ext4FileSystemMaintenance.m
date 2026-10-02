/* SPDX-License-Identifier: BSD-3-Clause */
#import "Ext4FileSystemInternal.h"
#import "Ext4VolumeInternal.h"
#import "Ext4Support.h"
#include <errno.h>
#include <string.h>

enum { Ext4MaintenanceCancelSeconds = 45, Ext4FormatDefaultBlockSize = 4096 };

static NSURL *
ext4_maintenance_executable(void)
{
	return [NSBundle.mainBundle.bundleURL
	    URLByAppendingPathComponent:@"Contents/Helpers/Ext4CheckResource"];
}

@implementation Ext4FileSystem (Maintenance)

- (NSProgress *)runMaintenance:(Ext4ResourceTask *)operation
			  task:(FSTask *)task
		   description:(NSString *)description
		    completion:(NSError * (^)(NSError *))completion
{
	NSProgress *progress = [NSProgress progressWithTotalUnitCount:1];
	dispatch_group_t work = dispatch_group_create();

	dispatch_group_enter(work);
	progress.cancellable = YES;
	progress.localizedDescription = description;
	progress.cancellationHandler = ^{
	  [operation cancel];
	};
	task.cancellationHandler = ^NSError * {
	  [operation cancel];
	  if (dispatch_group_wait(work,
		  dispatch_time(DISPATCH_TIME_NOW, Ext4MaintenanceCancelSeconds * NSEC_PER_SEC)) !=
	      0) {
		  return [NSError errorWithDomain:NSPOSIXErrorDomain code:ETIMEDOUT userInfo:nil];
	  }
	  return nil;
	};
	dispatch_async(dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{
	  NSError *failure = [operation runWithTask:task];

	  @synchronized(self) {
		  failure = completion(failure);
		  if (self->_maintenanceTask == operation) {
			  self->_maintenanceTask = nil;
			  self.containerStatus = failure == nil
			      ? FSContainerStatus.ready
			      : [FSContainerStatus blockedWithStatus:failure];
		  }
	  }
	  progress.completedUnitCount = 1;
	  progress.cancellationHandler = nil;
	  [task didCompleteWithError:failure];
	  dispatch_group_leave(work);
	});
	return progress;
}

- (NSError *)validateResourceAfterMaintenanceWriting:(BOOL)writing uuid:(NSUUID *)uuid
{
	struct ext4_fs *filesystem = NULL;
	struct ext4_info info;
	uuid_t expected;
	enum ext4_result result;

	/* Child success does not override a missing final persistence barrier or
	 * an unsupported/unmountable result. This validation does not claim a
	 * writable engine or reuse the old volume's identity or crypto state. */
	result = writing ? [_resourceOwner synchronize] : EXT4_OK;
	if (result == EXT4_OK) {
		result = [_resourceOwner open:&filesystem];
	}
	if (result == EXT4_OK && uuid != nil) {
		ext4_get_info(filesystem, &info);
		[uuid getUUIDBytes:expected];
		if (memcmp(info.uuid, expected, sizeof(expected)) != 0) {
			result = EXT4_CORRUPT;
		}
	}
	ext4_unmount(filesystem);
	return ext4_error(result);
}

- (NSProgress *)startCheckWithTask:(FSTask *)task
			   options:(FSTaskOptions *)options
			     error:(NSError **)error
{
	Ext4Volume *volume;
	NSProgress *progress;
	Ext4CheckTask *check = nil;
	NSError *failure = nil;
	BOOL automaticRecovery;
	BOOL quick = [options.taskOptions containsObject:@"-q"];
	BOOL force = [options.taskOptions containsObject:@"-f"];
	BOOL verify = [options.taskOptions containsObject:@"-n"];
	BOOL repair = [options.taskOptions containsObject:@"-y"];
	BOOL preen = [options.taskOptions containsObject:@"-p"];
	Ext4CheckMode mode = repair ? Ext4CheckRepair : (preen ? Ext4CheckPreen : Ext4CheckVerify);

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
	}
	@synchronized(self) {
		volume = _volume;
		automaticRecovery = _recoveredOnLoad && volume.writable;
		for (NSString *option in options.taskOptions) {
			if (![option isEqualToString:@"-p"] && ![option isEqualToString:@"-y"]) {
				automaticRecovery = NO;
			}
		}
		if (_maintenanceTask != nil) {
			failure = ext4_error(EXT4_BUSY);
		} else if (_resourceOwner == nil) {
			failure = [NSError errorWithDomain:NSPOSIXErrorDomain
						      code:ENXIO
						  userInfo:nil];
		} else if (volume != nil && !force && (quick || automaticRecovery)) {
			/* Preserve the ordinary Disk Arbitration quick/recovery path. */
			progress = [NSProgress progressWithTotalUnitCount:1];
			progress.cancellable = NO;
			dispatch_async(dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{
			  NSError *checkError = [volume checkMountEligibility];

			  progress.completedUnitCount = 1;
			  [task didCompleteWithError:checkError];
			});
			return progress;
		} else if (volume != nil) {
			check = [volume beginCheck:mode
					executable:ext4_maintenance_executable()
					     error:&failure];
		} else if (mode != Ext4CheckVerify && !_resourceOwner.writable) {
			failure = _resourceWriteError ?: ext4_error(EXT4_READ_ONLY);
		} else if (mode == Ext4CheckVerify && _resourceOwner.writable) {
			failure = ext4_error(EXT4_BUSY);
		} else {
			/* Successful maintenance retires its temporary volume. Further
			 * explicit tasks may use the retained exclusive resource directly. */
			check =
			    [[Ext4CheckTask alloc] initWithResource:_resourceOwner
							       mode:mode
							 executable:ext4_maintenance_executable()];
			if (check == nil) {
				failure = ext4_error(EXT4_INVALID_ARGUMENT);
			}
		}
		if (failure != nil) {
			if (error != NULL) {
				*error = failure;
			}
			return nil;
		}
		_maintenanceTask = check;
		return [self
		    runMaintenance:check
			      task:task
		       description:mode == Ext4CheckVerify ? @"Checking ext4" : @"Repairing ext4"
			completion:^NSError *(NSError *checkError) {
			  if (volume != nil) {
				  checkError = [volume finishCheck:check error:checkError];
				  if (!volume.maintenanceOnly || checkError != nil) {
					  return checkError;
				  }
				  self->_volume = nil;
				  self->_recoveredOnLoad = NO;
			  }
			  return checkError
			      ?: [self
				     validateResourceAfterMaintenanceWriting:mode != Ext4CheckVerify
									uuid:nil];
			}];
	}
}

- (NSProgress *)startFormatWithTask:(FSTask *)task
			    options:(FSTaskOptions *)options
			      error:(NSError **)error
{
	NSArray<NSString *> *arguments = options.taskOptions;
	NSString *name = @"";
	NSString *value;
	NSString *option;
	NSUInteger blockSize = Ext4FormatDefaultBlockSize;
	NSUInteger index;
	BOOL haveBlockSize = NO;
	BOOL haveName = NO;
	Ext4Volume *volume;
	Ext4FormatTask *format = nil;
	NSUUID *uuid = NSUUID.UUID;
	NSError *failure = nil;

	for (index = 0; index < arguments.count; index++) {
		option = arguments[index];
		if ([option isEqualToString:@"-b"] || [option isEqualToString:@"-L"]) {
			if (++index == arguments.count) {
				failure = ext4_error(EXT4_INVALID_ARGUMENT);
				break;
			}
			value = arguments[index];
		} else if ([option hasPrefix:@"-b"] || [option hasPrefix:@"-L"]) {
			value = [option substringFromIndex:2];
			option = [option substringToIndex:2];
		} else {
			failure = ext4_error(EXT4_INVALID_ARGUMENT);
			break;
		}
		if ([option isEqualToString:@"-b"] && !haveBlockSize &&
		    [@[ @"1024", @"2048", @"4096" ] containsObject:value]) {
			blockSize = value.integerValue;
			haveBlockSize = YES;
		} else if ([option isEqualToString:@"-L"] && !haveName) {
			name = value;
			haveName = YES;
		} else {
			failure = ext4_error(EXT4_INVALID_ARGUMENT);
			break;
		}
	}
	@synchronized(self) {
		volume = _volume;
		if (failure == nil && _maintenanceTask != nil) {
			failure = ext4_error(EXT4_BUSY);
		} else if (failure == nil && _resourceOwner == nil) {
			failure = [NSError errorWithDomain:NSPOSIXErrorDomain
						      code:ENXIO
						  userInfo:nil];
		} else if (failure == nil && !_resourceOwner.writable) {
			failure = _resourceWriteError ?: ext4_error(EXT4_READ_ONLY);
		}
		if (failure == nil && volume != nil) {
			format = [volume beginFormatWithBlockSize:blockSize
							     name:name
							     uuid:uuid
						       executable:ext4_maintenance_executable()
							    error:&failure];
		} else if (failure == nil) {
			format =
			    [[Ext4FormatTask alloc] initWithResource:_resourceOwner
							   blockSize:blockSize
								name:name
								uuid:uuid
							  executable:ext4_maintenance_executable()];
			if (format == nil) {
				failure = ext4_error(EXT4_INVALID_ARGUMENT);
			}
		}
		if (failure != nil) {
			if (error != NULL) {
				*error = failure;
			}
			return nil;
		}
		_maintenanceTask = format;
		return [self
		    runMaintenance:format
			      task:task
		       description:@"Formatting ext4"
			completion:^NSError *(NSError *formatError) {
			  if (volume != nil) {
				  formatError = [volume finishFormat:format error:formatError];
				  self->_volume = nil;
			  }
			  self->_recoveredOnLoad = NO;
			  return formatError
			      ?: [self validateResourceAfterMaintenanceWriting:YES uuid:uuid];
			}];
	}
}

@end

/* SPDX-License-Identifier: BSD-3-Clause */
#import "Ext4FileSystem.h"
#import "Ext4Volume.h"
#import "Ext4ResourceIO.h"
#import "Ext4Support.h"
#import "Ext4KeyStore.h"
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
	__attribute__((objc_precise_lifetime)) Ext4ResourceIO *owner = nil;
	struct ext4_info info;
	NSUUID *uuid;
	NSString *name;
	FSContainerIdentifier *identifier;
	enum ext4_result error;

	error = ext4_open_resource(resource, &owner, &fs);
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
	__attribute__((objc_precise_lifetime)) Ext4ResourceIO *owner = nil;
	Ext4Volume *volume = nil;
	struct ext4_native_crypto *crypto = NULL;
	struct ext4_info info;
	NSError *keyError = nil;
	NSError *loadError = nil;
	enum ext4_result error;

	(void)options;
	@synchronized(self) {
		if (_volume != nil) {
			loadError = [NSError errorWithDomain:NSPOSIXErrorDomain
							code:EBUSY
						    userInfo:nil];
		} else {
			error = ext4_open_resource(resource, &owner, &fs);
			if (error == EXT4_OK) {
				ext4_get_info(fs, &info);
				crypto = [Ext4KeyStore
				    loadCryptoForVolume:[[NSUUID alloc] initWithUUIDBytes:info.uuid]
						  error:&keyError];
				volume = [[Ext4Volume alloc]
				    initWithResource:(FSBlockDeviceResource *)resource
					  filesystem:fs
				       resourceOwner:owner
					      crypto:crypto];
				if (volume == nil) {
					ext4_unmount(fs);
					ext4_native_crypto_destroy(crypto);
					error = EXT4_NO_MEMORY;
				}
			}
			volume.keyStoreError = keyError;
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
		[_volume invalidate];
		_volume = nil;
		self.containerStatus = [FSContainerStatus
		    notReadyWithStatus:[NSError errorWithDomain:NSPOSIXErrorDomain
							   code:ENXIO
						       userInfo:nil]];
	}
	reply(nil);
}

@end

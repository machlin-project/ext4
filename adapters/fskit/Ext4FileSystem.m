/* SPDX-License-Identifier: BSD-3-Clause */
#import "Ext4FileSystemInternal.h"
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

@implementation Ext4FileSystem

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
	BOOL maintenanceOnly = NO;
	BOOL force = [options.taskOptions containsObject:@"-f"];
	enum ext4_result error;

	@synchronized(self) {
		if (_resourceOwner != nil || _maintenanceTask != nil) {
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
			maintenanceOnly = force && owner != nil && !inspectable &&
			    (error == EXT4_NOT_EXT4 || error == EXT4_CORRUPT ||
				error == EXT4_RECOVERY_REQUIRED || error == EXT4_UNSUPPORTED);
#if DEBUG
			NSLog(
			    @"Machlin ext4 load build %@: readonly=%d force=%d resourceWritable=%d "
			    @"result=%d maintenanceOnly=%d",
			    [NSBundle bundleForClass:self.class].infoDictionary[@"CFBundleVersion"],
			    [options.taskOptions containsObject:@"--rdonly"], force,
			    [resource isKindOfClass:FSBlockDeviceResource.class] &&
				[(FSBlockDeviceResource *)resource isWritable],
			    error, maintenanceOnly);
#endif
			if ((inspectable || maintenanceOnly) &&
			    ![options.taskOptions containsObject:@"--rdonly"] &&
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
					if (!maintenanceOnly && error == EXT4_RECOVERY_REQUIRED) {
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
			/* Force admits a physical resource for offline maintenance without
			 * claiming that unreadable filesystem metadata describes a volume.
			 * The blocked state prevents activation until maintenance succeeds. */
			loadError = volume != nil || maintenanceOnly ? nil : ext4_error(error);
			_volume = volume;
			_resourceOwner = loadError == nil ? owner : nil;
			_resourceWriteError = loadError == nil ? writeError : nil;
			self.containerStatus = volume != nil
			    ? FSContainerStatus.ready
			    : [FSContainerStatus blockedWithStatus:ext4_error(error)];
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
		if (_maintenanceTask != nil) {
			reply(ext4_error(EXT4_BUSY));
			return;
		}
		error = [_volume finishUnloadedResource];
		if (error.code == EBUSY) {
			reply(error);
			return;
		}
		[_volume invalidate];
		_volume = nil;
		_resourceOwner = nil;
		_resourceWriteError = nil;
		_recoveredOnLoad = NO;
		self.containerStatus = [FSContainerStatus
		    notReadyWithStatus:[NSError errorWithDomain:NSPOSIXErrorDomain
							   code:ENXIO
						       userInfo:nil]];
	}
	reply(error);
}

@end

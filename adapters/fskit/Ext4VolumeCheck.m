/* SPDX-License-Identifier: BSD-3-Clause */
#import "Ext4VolumeInternal.h"
#import "Ext4Support.h"
#include <errno.h>
#include <string.h>

@implementation Ext4Volume (Maintenance)

- (NSError *)beginResourceMaintenance:(Ext4ResourceTask *)task writable:(BOOL)writable
{
	@synchronized(self) {
		enum ext4_result result = EXT4_OK;

		if (task == nil) {
			result = EXT4_INVALID_ARGUMENT;
		} else if (!_active || _writeClosed) {
			result = EXT4_STALE;
		} else if (_resourceOwner.isRevoked || _lifetimeError != EXT4_OK) {
			result = _lifetimeError != EXT4_OK ? _lifetimeError : EXT4_IO;
		} else if (_maintenance != nil || _mounted ||
		    _items.objectEnumerator.nextObject != nil) {
			result = EXT4_BUSY;
		} else if (!writable && self.writable) {
			/* A read-only scan must not sync or release a writable journal/MMP
			 * owner. The client must load a read-only resource for verification. */
			result = EXT4_BUSY;
		} else if (writable && !self.writable) {
			result = EXT4_READ_ONLY;
		}
		if (result != EXT4_OK) {
			return ext4_error(result);
		}
		if (_fs != NULL && self.writable) {
			result = ext4_sync(_fs);
			if (result == EXT4_OK) {
				result = ext4_mmp_release(_fs);
			}
		}
		if (result != EXT4_OK) {
			_lifetimeError = result;
			return ext4_error(result);
		}
		[self stopReadStateMaintenance];
		if (_mmpTimer != nil) {
			dispatch_source_cancel(_mmpTimer);
			_mmpTimer = nil;
		}
		/* There are no native items. Release every journal, MMP owner and
		 * metadata cache before separate maintenance may change the media. */
		ext4_unmount(_fs);
		_fs = NULL;
		_openError = EXT4_CORRUPT;
		_maintenance = task;
		return nil;
	}
}

- (Ext4CheckTask *)beginCheck:(Ext4CheckMode)mode
		   executable:(NSURL *)executable
			error:(NSError **)error
{
	@synchronized(self) {
		Ext4CheckTask *check = [[Ext4CheckTask alloc] initWithResource:_resourceOwner
									  mode:mode
								    executable:executable];
		NSError *failure = [self beginResourceMaintenance:check
							 writable:mode != Ext4CheckVerify];

		if (error != NULL) {
			*error = failure;
		}
		return failure == nil ? check : nil;
	}
}

- (Ext4FormatTask *)beginFormatWithBlockSize:(NSUInteger)blockSize
					name:(NSString *)name
					uuid:(NSUUID *)uuid
				  executable:(NSURL *)executable
				       error:(NSError **)error
{
	@synchronized(self) {
		Ext4FormatTask *format = [[Ext4FormatTask alloc] initWithResource:_resourceOwner
									blockSize:blockSize
									     name:name
									     uuid:uuid
								       executable:executable];
		NSError *failure = [self beginResourceMaintenance:format writable:YES];

		if (error != NULL) {
			*error = failure;
		}
		return failure == nil ? format : nil;
	}
}

- (NSError *)finishFormat:(Ext4FormatTask *)format error:(NSError *)error
{
	@synchronized(self) {
		if (format == nil || _maintenance != format) {
			return ext4_error(EXT4_STALE);
		}
		_maintenance = nil;
		/* A new filesystem has a new identity. Never reopen this volume using
		 * its former UUID, keys or native item references. The filesystem-level
		 * resource owner remains alive until the child is reaped and unloaded. */
		[self invalidate];
		return error;
	}
}

- (NSError *)finishCheck:(Ext4CheckTask *)check error:(NSError *)error
{
	@synchronized(self) {
		struct ext4_fs *filesystem = NULL;
		struct ext4_info info = { 0 };
		struct ext4_crypto_environment crypto;
		enum ext4_result result;

		if (check == nil || _maintenance != check) {
			return ext4_error(EXT4_STALE);
		}
		_maintenance = nil;
		if (!_active || _writeClosed || _resourceOwner.isRevoked) {
			return ext4_error(EXT4_IO);
		}
		if (error != nil) {
			/* Interrupted repair or incomplete verification leaves no mountable
			 * engine. Another explicit check may retry the same exclusive view. */
			return error;
		}
		if (_maintenanceOnly) {
			/* There is no original filesystem UUID or geometry to reopen.
			 * The parent validates the repaired resource, then the next native
			 * load creates a volume from its actual superblock and keys. */
			[self invalidate];
			return nil;
		}
		result = self.writable ? [_resourceOwner openWritable:&filesystem]
				       : [_resourceOwner open:&filesystem];
		if (result == EXT4_OK) {
			ext4_get_info(filesystem, &info);
			if (memcmp(info.uuid, _info.uuid, sizeof(info.uuid)) != 0) {
				result = EXT4_CORRUPT;
			}
		}
		if (result == EXT4_OK && _crypto != NULL) {
			ext4_native_crypto_seal(_crypto);
			crypto = ext4_native_crypto_environment(_crypto);
			result = ext4_set_crypto(filesystem, &crypto);
		}
		_openError = result;
		if (result != EXT4_OK) {
			if (self.writable && filesystem != NULL &&
			    ext4_sync(filesystem) == EXT4_OK) {
				(void)ext4_mmp_release(filesystem);
			}
			ext4_unmount(filesystem);
			return ext4_error(result);
		}
		_fs = filesystem;
		_info = info;
		[self startResourceMaintenance];
		return nil;
	}
}

@end

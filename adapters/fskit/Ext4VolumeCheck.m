/* SPDX-License-Identifier: BSD-3-Clause */
#import "Ext4VolumeInternal.h"
#import "Ext4Support.h"
#include <errno.h>
#include <string.h>

@implementation Ext4Volume (Checks)

- (Ext4CheckTask *)beginCheck:(Ext4CheckMode)mode
		   executable:(NSURL *)executable
			error:(NSError **)error
{
	@synchronized(self) {
		Ext4CheckTask *check;
		enum ext4_result result = EXT4_OK;

		if (!_active || _writeClosed) {
			result = EXT4_STALE;
		} else if (_resourceOwner.isRevoked || _lifetimeError != EXT4_OK) {
			result = _lifetimeError != EXT4_OK ? _lifetimeError : EXT4_IO;
		} else if (_check != nil || _mounted || _items.objectEnumerator.nextObject != nil) {
			result = EXT4_BUSY;
		} else if (mode == Ext4CheckVerify && self.writable) {
			/* A read-only scan must not sync or release a writable journal/MMP
			 * owner. The client must load a read-only resource for verification. */
			result = EXT4_BUSY;
		} else if (mode != Ext4CheckVerify && !self.writable) {
			result = EXT4_READ_ONLY;
		}
		if (result != EXT4_OK) {
			if (error != NULL) {
				*error = ext4_error(result);
			}
			return nil;
		}
		check = [[Ext4CheckTask alloc] initWithResource:_resourceOwner
							   mode:mode
						     executable:executable];
		if (check == nil) {
			if (error != NULL) {
				*error = ext4_error(EXT4_INVALID_ARGUMENT);
			}
			return nil;
		}
		if (_fs != NULL && self.writable) {
			result = ext4_sync(_fs);
			if (result == EXT4_OK) {
				result = ext4_mmp_release(_fs);
			}
		}
		if (result != EXT4_OK) {
			_lifetimeError = result;
			if (error != NULL) {
				*error = ext4_error(result);
			}
			return nil;
		}
		[self stopReadStateMaintenance];
		if (_mmpTimer != nil) {
			dispatch_source_cancel(_mmpTimer);
			_mmpTimer = nil;
		}
		/* There are no native items. Release every journal, MMP owner and
		 * metadata cache before the separate checker may change the media. */
		ext4_unmount(_fs);
		_fs = NULL;
		_openError = EXT4_CORRUPT;
		_check = check;
		return check;
	}
}

- (NSError *)finishCheck:(Ext4CheckTask *)check error:(NSError *)error
{
	@synchronized(self) {
		struct ext4_fs *filesystem = NULL;
		struct ext4_info info = { 0 };
		struct ext4_crypto_environment crypto;
		enum ext4_result result;

		if (_check != check) {
			return ext4_error(EXT4_STALE);
		}
		_check = nil;
		if (!_active || _writeClosed || _resourceOwner.isRevoked) {
			return ext4_error(EXT4_IO);
		}
		if (error != nil) {
			/* Interrupted repair or incomplete verification leaves no mountable
			 * engine. Another explicit check may retry the same exclusive view. */
			return error;
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

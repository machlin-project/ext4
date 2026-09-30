/* SPDX-License-Identifier: BSD-3-Clause */
#import "Ext4Support.h"
#include <errno.h>

NSError *
ext4_error(enum ext4_result result)
{
	int error;

	switch (result) {
	case EXT4_OK:
		return nil;
	case EXT4_NO_MEMORY:
		error = ENOMEM;
		break;
	case EXT4_NOT_FOUND:
		error = ENOENT;
		break;
	case EXT4_NOT_DIRECTORY:
		error = ENOTDIR;
		break;
	case EXT4_NAME_TOO_LONG:
		error = ENAMETOOLONG;
		break;
	case EXT4_READ_ONLY:
		error = EROFS;
		break;
	case EXT4_IS_DIRECTORY:
		error = EISDIR;
		break;
	case EXT4_UNSUPPORTED:
		error = ENOTSUP;
		break;
	case EXT4_INVALID_ARGUMENT:
	case EXT4_NOT_EXT4:
		error = EINVAL;
		break;
	case EXT4_RANGE:
		error = EOVERFLOW;
		break;
	case EXT4_STALE:
		error = ESTALE;
		break;
	case EXT4_NO_SPACE:
		error = ENOSPC;
		break;
	case EXT4_EXISTS:
		error = EEXIST;
		break;
	case EXT4_TOO_MANY_LINKS:
		error = EMLINK;
		break;
	case EXT4_NOT_EMPTY:
		error = ENOTEMPTY;
		break;
	case EXT4_PERMISSION_DENIED:
		error = EPERM;
		break;
	case EXT4_BUSY:
		error = EBUSY;
		break;
	case EXT4_ENCRYPTED:
		error = EACCES;
		break;
	case EXT4_CROSS_PROJECT:
	case EXT4_CROSS_POLICY:
		error = EXDEV;
		break;
	case EXT4_QUOTA_EXCEEDED:
		error = EDQUOT;
		break;
	case EXT4_CORRUPT:
	case EXT4_RECOVERY_REQUIRED:
	case EXT4_IO:
	default:
		error = EIO;
		break;
	}
	return [NSError errorWithDomain:NSPOSIXErrorDomain
				   code:error
			       userInfo:@{
				       NSLocalizedDescriptionKey : @(ext4_result_string(result))
			       }];
}

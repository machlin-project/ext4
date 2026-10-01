/* SPDX-License-Identifier: BSD-3-Clause */
#import "Ext4ExtensionSettings.h"
#import <FSKit/FSKit.h>

BOOL
Ext4OpenFileSystemExtensionsSettings(void)
{
#if defined(__MAC_27_0) && __MAC_OS_X_VERSION_MAX_ALLOWED >= __MAC_27_0
	if (@available(macOS 27.0, *)) {
		return [[FSClient sharedInstance] openFileSystemExtensionsSettings];
	}
#endif
	return NO;
}

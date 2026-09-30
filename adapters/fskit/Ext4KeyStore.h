/* SPDX-License-Identifier: BSD-3-Clause */
#import <Foundation/Foundation.h>

struct ext4_native_crypto;

NS_ASSUME_NONNULL_BEGIN

/* Shares only fscrypt master keys belonging to this product and volume.
 * Live mounts take an immutable snapshot; edits apply to the next mount. */
@interface Ext4KeyStore : NSObject
+ (nullable struct ext4_native_crypto *)loadCryptoForVolume:(NSUUID *)volume
						      error:(NSError *_Nullable *_Nullable)error;
+ (nullable NSArray<NSString *> *)keysForVolume:(NSUUID *)volume
					  error:(NSError *_Nullable *_Nullable)error;
+ (nullable NSString *)importKeyFromURL:(NSURL *)url
				 volume:(NSUUID *)volume
			   v1Descriptor:(NSString *)descriptor
				  error:(NSError *_Nullable *_Nullable)error;
/* Reads exactly one raw key followed by EOF. The caller retains the descriptor. */
+ (nullable NSString *)importKeyFromFileDescriptor:(int)fd
					    volume:(NSUUID *)volume
				      v1Descriptor:(NSString *)descriptor
					     error:(NSError *_Nullable *_Nullable)error;
+ (BOOL)removeKey:(NSString *)identifier
	   volume:(NSUUID *)volume
	    error:(NSError *_Nullable *_Nullable)error;
@end

NS_ASSUME_NONNULL_END

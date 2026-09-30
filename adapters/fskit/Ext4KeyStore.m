/* SPDX-License-Identifier: BSD-3-Clause */
#import "Ext4KeyStore.h"
#import "Ext4Control.h"
#include "Ext4Crypto.h"
#import <Security/Security.h>
#import <LocalAuthentication/LocalAuthentication.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

static NSMutableDictionary *
ext4_key_query(NSUUID *volume)
{
	return [@{
		(__bridge id)kSecClass : (__bridge id)kSecClassGenericPassword,
		(__bridge id)kSecAttrService :
		    [@"org.machlin.ext4.fscrypt." stringByAppendingString:volume.UUIDString],
		(__bridge id)kSecAttrAccessGroup : Ext4AppGroup,
		(__bridge id)kSecUseDataProtectionKeychain : @YES,
		(__bridge id)kSecAttrSynchronizable : @NO
	} mutableCopy];
}

static void
ext4_key_error(NSError **error, NSInteger code, BOOL security)
{
	if (error != NULL) {
		*error =
		    [NSError errorWithDomain:security ? NSOSStatusErrorDomain : NSPOSIXErrorDomain
					code:code
				    userInfo:nil];
	}
}

static int
ext4_key_hex(unichar value)
{
	if (value >= '0' && value <= '9') {
		return value - '0';
	}
	if (value >= 'a' && value <= 'f') {
		return value - 'a' + 10;
	}
	if (value >= 'A' && value <= 'F') {
		return value - 'A' + 10;
	}
	return -1;
}

static BOOL
ext4_key_decode(NSString *text, uint8_t *bytes, size_t size)
{
	size_t index;
	int high;
	int low;

	if (![text isKindOfClass:NSString.class] || text.length != size * 2) {
		return NO;
	}
	for (index = 0; index < size; index++) {
		high = ext4_key_hex([text characterAtIndex:index * 2]);
		low = ext4_key_hex([text characterAtIndex:index * 2 + 1]);
		if (high < 0 || low < 0) {
			return NO;
		}
		bytes[index] = (uint8_t)((high << 4) | low);
	}
	return YES;
}

static BOOL
ext4_key_account(NSString *account, uint8_t *version, uint8_t *identifier, size_t *size)
{
	if (![account isKindOfClass:NSString.class] || account.length < 3) {
		return NO;
	}
	if ([account hasPrefix:@"v1:"]) {
		*version = 1;
		*size = EXT4_NATIVE_DESCRIPTOR_SIZE;
	} else if ([account hasPrefix:@"v2:"]) {
		*version = 2;
		*size = EXT4_NATIVE_IDENTIFIER_SIZE;
	} else {
		return NO;
	}
	return ext4_key_decode([account substringFromIndex:3], identifier, *size);
}

static NSArray *
ext4_key_records(NSUUID *volume, BOOL data, NSError **error)
{
	NSMutableDictionary *query = ext4_key_query(volume);
	CFTypeRef result = NULL;
	LAContext *authentication = [LAContext new];
	OSStatus status;
	id records;

	query[(__bridge id)kSecMatchLimit] = (__bridge id)kSecMatchLimitAll;
	query[(__bridge id)kSecReturnAttributes] = @YES;
	query[(__bridge id)kSecReturnData] = @(data);
	/* A filesystem request must never wait for Keychain UI. */
	authentication.interactionNotAllowed = YES;
	query[(__bridge id)kSecUseAuthenticationContext] = authentication;
	status = SecItemCopyMatching((__bridge CFDictionaryRef)query, &result);
	records = CFBridgingRelease(result);
	if (status == errSecItemNotFound) {
		return @[];
	}
	if (status != errSecSuccess) {
		ext4_key_error(error, status, YES);
		return nil;
	}
	if (![records isKindOfClass:NSArray.class] || [records count] > EXT4_NATIVE_MAX_KEYS) {
		ext4_key_error(error, EOVERFLOW, NO);
		return nil;
	}
	return records;
}

@implementation Ext4KeyStore

+ (struct ext4_native_crypto *)loadCryptoForVolume:(NSUUID *)volume error:(NSError **)error
{
	NSArray *records = ext4_key_records(volume, YES, error);
	struct ext4_native_crypto *crypto;
	uint8_t identifier[EXT4_NATIVE_IDENTIFIER_SIZE];
	uint8_t version = 0;
	size_t size = 0;
	enum ext4_result result;

	if (records == nil) {
		return NULL;
	}
	crypto = ext4_native_crypto_create();
	if (crypto == NULL) {
		ext4_key_error(error, ENOMEM, NO);
		return NULL;
	}
	for (id record in records) {
		NSData *key;

		if (![record isKindOfClass:NSDictionary.class] ||
		    !ext4_key_account(
			record[(__bridge id)kSecAttrAccount], &version, identifier, &size)) {
			ext4_key_error(error, EINVAL, NO);
			goto failed;
		}
		key = record[(__bridge id)kSecValueData];
		if (![key isKindOfClass:NSData.class] || key.length != EXT4_NATIVE_MASTER_SIZE) {
			ext4_key_error(error, EINVAL, NO);
			goto failed;
		}
		result = ext4_native_crypto_add(
		    crypto, version, identifier, size, key.bytes, key.length);
		if (result != EXT4_OK) {
			ext4_key_error(error, result == EXT4_NO_MEMORY ? ENOMEM : EINVAL, NO);
			goto failed;
		}
	}
	ext4_native_crypto_seal(crypto);
	return crypto;
failed:
	ext4_native_crypto_destroy(crypto);
	return NULL;
}

+ (NSArray<NSString *> *)keysForVolume:(NSUUID *)volume error:(NSError **)error
{
	NSArray *records = ext4_key_records(volume, NO, error);
	NSMutableArray<NSString *> *keys = [NSMutableArray array];
	uint8_t identifier[EXT4_NATIVE_IDENTIFIER_SIZE];
	uint8_t version = 0;
	size_t size = 0;

	if (records == nil) {
		return nil;
	}
	for (id record in records) {
		NSString *account;

		if (![record isKindOfClass:NSDictionary.class]) {
			ext4_key_error(error, EINVAL, NO);
			return nil;
		}
		account = record[(__bridge id)kSecAttrAccount];
		if (!ext4_key_account(account, &version, identifier, &size)) {
			ext4_key_error(error, EINVAL, NO);
			return nil;
		}
		[keys addObject:account];
	}
	return [keys sortedArrayUsingSelector:@selector(compare:)];
}

+ (NSString *)importKeyFromURL:(NSURL *)url
			volume:(NSUUID *)volume
		  v1Descriptor:(NSString *)descriptor
			 error:(NSError **)error
{
	struct stat status;
	int fd;
	BOOL scoped = [url startAccessingSecurityScopedResource];
	NSString *answer = nil;

	fd = open(url.fileSystemRepresentation, O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
	if (fd < 0) {
		ext4_key_error(error, errno, NO);
	} else if (fstat(fd, &status) != 0) {
		ext4_key_error(error, errno, NO);
	} else if (!S_ISREG(status.st_mode) || status.st_size != EXT4_NATIVE_MASTER_SIZE) {
		ext4_key_error(error, EINVAL, NO);
	} else {
		answer = [self importKeyFromFileDescriptor:fd
						    volume:volume
					      v1Descriptor:descriptor
						     error:error];
	}
	if (fd >= 0) {
		close(fd);
	}
	if (scoped) {
		[url stopAccessingSecurityScopedResource];
	}
	return answer;
}

+ (NSString *)importKeyFromFileDescriptor:(int)fd
				   volume:(NSUUID *)volume
			     v1Descriptor:(NSString *)descriptor
				    error:(NSError **)error
{
	/* One extra byte detects an oversized stream without buffering its contents. */
	uint8_t master[EXT4_NATIVE_MASTER_SIZE + 1];
	uint8_t identifier[EXT4_NATIVE_IDENTIFIER_SIZE];
	volatile uint8_t *wipe = master;
	uint8_t version = descriptor.length == 0 ? 2 : 1;
	size_t size = version == 2 ? EXT4_NATIVE_IDENTIFIER_SIZE : EXT4_NATIVE_DESCRIPTOR_SIZE;
	size_t index;
	size_t completed = 0;
	ssize_t count;
	NSMutableDictionary *query;
	NSMutableString *account;
	NSMutableData *secret = nil;
	NSArray *keys;
	NSString *answer = nil;
	OSStatus saved;

	if (version == 1 && !ext4_key_decode(descriptor, identifier, size)) {
		ext4_key_error(error, EINVAL, NO);
		goto done;
	}
	while (completed < sizeof(master)) {
		count = read(fd, master + completed, sizeof(master) - completed);
		if (count < 0 && errno == EINTR) {
			continue;
		}
		if (count < 0) {
			ext4_key_error(error, errno, NO);
			goto done;
		}
		if (count == 0) {
			break;
		}
		completed += (size_t)count;
	}
	if (completed != EXT4_NATIVE_MASTER_SIZE) {
		ext4_key_error(error, EINVAL, NO);
		goto done;
	}
	if (version == 2) {
		ext4_native_crypto_identifier(master, EXT4_NATIVE_MASTER_SIZE, identifier);
	}
	account = [NSMutableString stringWithFormat:@"v%u:", version];
	for (index = 0; index < size; index++) {
		[account appendFormat:@"%02x", identifier[index]];
	}
	keys = [self keysForVolume:volume error:error];
	if (keys == nil) {
		goto done;
	}
	if (keys.count >= EXT4_NATIVE_MAX_KEYS) {
		ext4_key_error(error, ENOSPC, NO);
		goto done;
	}
	query = ext4_key_query(volume);
	secret = [NSMutableData dataWithBytes:master length:EXT4_NATIVE_MASTER_SIZE];
	query[(__bridge id)kSecAttrAccount] = account;
	query[(__bridge id)kSecValueData] = secret;
	query[(__bridge id)kSecAttrAccessible] =
	    (__bridge id)kSecAttrAccessibleAfterFirstUnlockThisDeviceOnly;
	saved = SecItemAdd((__bridge CFDictionaryRef)query, NULL);
	if (saved == errSecSuccess) {
		answer = account;
	} else {
		ext4_key_error(error, saved, YES);
	}
done:
	[secret resetBytesInRange:NSMakeRange(0, secret.length)];
	for (index = 0; index < sizeof(master); index++) {
		wipe[index] = 0;
	}
	return answer;
}

+ (BOOL)removeKey:(NSString *)identifier volume:(NSUUID *)volume error:(NSError **)error
{
	NSMutableDictionary *query = ext4_key_query(volume);
	uint8_t decoded[EXT4_NATIVE_IDENTIFIER_SIZE];
	uint8_t version = 0;
	size_t size = 0;
	OSStatus status;

	if (!ext4_key_account(identifier, &version, decoded, &size)) {
		ext4_key_error(error, EINVAL, NO);
		return NO;
	}
	query[(__bridge id)kSecAttrAccount] = identifier;
	status = SecItemDelete((__bridge CFDictionaryRef)query);
	if (status != errSecSuccess && status != errSecItemNotFound) {
		ext4_key_error(error, status, YES);
		return NO;
	}
	return YES;
}

@end

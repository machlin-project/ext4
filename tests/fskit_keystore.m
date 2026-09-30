/* SPDX-License-Identifier: BSD-3-Clause */
#import "../adapters/fskit/Ext4KeyStore.h"
#import "../adapters/fskit/Ext4Control.h"
#include "../adapters/fskit/Ext4Crypto.h"
#import <Security/Security.h>
#import <LocalAuthentication/LocalAuthentication.h>
#include <assert.h>
#include <fcntl.h>
#include <stdlib.h>
#include <unistd.h>

/* The test build renames these three Security entry points in both this file
 * and Ext4KeyStore.m. No test ever reads or mutates the host's Keychain. */
static NSMutableArray<NSDictionary *> *records;
static OSStatus injected;

static NSDictionary *
check_query(CFDictionaryRef value)
{
	NSDictionary *query = (__bridge NSDictionary *)value;

	assert([query[(__bridge id)kSecClass] isEqual:(__bridge id)kSecClassGenericPassword]);
	assert([query[(__bridge id)kSecAttrAccessGroup] isEqual:Ext4AppGroup]);
	assert([query[(__bridge id)kSecUseDataProtectionKeychain] boolValue]);
	assert(![query[(__bridge id)kSecAttrSynchronizable] boolValue]);
	assert([query[(__bridge id)kSecAttrService] hasPrefix:@"org.machlin.ext4.fscrypt."]);
	return query;
}

OSStatus
SecItemCopyMatching(CFDictionaryRef value, CFTypeRef *result)
{
	NSDictionary *query = check_query(value);
	LAContext *authentication = query[(__bridge id)kSecUseAuthenticationContext];
	NSMutableArray *matched = [NSMutableArray array];

	assert(authentication.interactionNotAllowed);
	assert([query[(__bridge id)kSecMatchLimit] isEqual:(__bridge id)kSecMatchLimitAll]);
	assert([query[(__bridge id)kSecReturnAttributes] boolValue]);
	if (injected != errSecSuccess) {
		return injected;
	}
	for (NSDictionary *record in records) {
		NSMutableDictionary *copy;

		if (![record[(__bridge id)kSecAttrService]
			isEqual:query[(__bridge id)kSecAttrService]]) {
			continue;
		}
		copy = [record mutableCopy];
		if (![query[(__bridge id)kSecReturnData] boolValue]) {
			[copy removeObjectForKey:(__bridge id)kSecValueData];
		}
		[matched addObject:copy];
	}
	if (matched.count == 0) {
		return errSecItemNotFound;
	}
	*result = CFBridgingRetain(matched);
	return errSecSuccess;
}

OSStatus
SecItemAdd(CFDictionaryRef value, CFTypeRef *result)
{
	NSDictionary *query = check_query(value);
	NSMutableDictionary *copy = [query mutableCopy];

	(void)result;
	assert([query[(__bridge id)kSecAttrAccessible]
	    isEqual:(__bridge id)kSecAttrAccessibleAfterFirstUnlockThisDeviceOnly]);
	for (NSDictionary *record in records) {
		if ([record[(__bridge id)kSecAttrService]
			isEqual:query[(__bridge id)kSecAttrService]] &&
		    [record[(__bridge id)kSecAttrAccount]
			isEqual:query[(__bridge id)kSecAttrAccount]]) {
			return errSecDuplicateItem;
		}
	}
	copy[(__bridge id)kSecValueData] = [query[(__bridge id)kSecValueData] copy];
	[records addObject:copy];
	return errSecSuccess;
}

OSStatus
SecItemDelete(CFDictionaryRef value)
{
	NSDictionary *query = check_query(value);
	NSUInteger index;

	assert(query[(__bridge id)kSecAttrAccount] != nil);
	for (index = 0; index < records.count; index++) {
		NSDictionary *record = records[index];

		if ([record[(__bridge id)kSecAttrService]
			isEqual:query[(__bridge id)kSecAttrService]] &&
		    [record[(__bridge id)kSecAttrAccount]
			isEqual:query[(__bridge id)kSecAttrAccount]]) {
			[records removeObjectAtIndex:index];
			return errSecSuccess;
		}
	}
	return errSecItemNotFound;
}

int
main(void)
{
	@autoreleasepool {
		char directory[] = "/tmp/ext4-keychain-XXXXXX";
		NSUUID *volume = NSUUID.UUID;
		NSUUID *otherVolume = NSUUID.UUID;
		NSURL *url;
		NSURL *link;
		NSString *identifier;
		NSArray *keys;
		NSError *error = nil;
		uint8_t master[EXT4_NATIVE_MASTER_SIZE];
		uint8_t idBytes[EXT4_NATIVE_IDENTIFIER_SIZE];
		struct ext4_native_crypto *provider;
		struct ext4_crypto_environment environment;
		void *handle = NULL;
		NSMutableDictionary *malformed;
		size_t index;
		int fd;

		records = [NSMutableArray array];
		assert(mkdtemp(directory) != NULL);
		url = [[NSURL fileURLWithFileSystemRepresentation:directory
						      isDirectory:YES
						    relativeToURL:nil]
		    URLByAppendingPathComponent:@"raw"];
		link =
		    [[url URLByDeletingLastPathComponent] URLByAppendingPathComponent:@"symlink"];
		for (index = 0; index < sizeof(master); index++) {
			master[index] = (uint8_t)(index * 7 + 3);
		}
		fd = open(url.fileSystemRepresentation, O_WRONLY | O_CREAT | O_EXCL, 0600);
		assert(fd >= 0 && write(fd, master, sizeof(master)) == sizeof(master));
		assert(close(fd) == 0);
		assert(symlink(url.fileSystemRepresentation, link.fileSystemRepresentation) == 0);
		assert([Ext4KeyStore importKeyFromURL:link
					       volume:volume
					 v1Descriptor:@""
						error:&error] == nil);
		assert(error != nil && records.count == 0);
		error = nil;
		identifier = [Ext4KeyStore importKeyFromURL:url
						     volume:volume
					       v1Descriptor:@""
						      error:&error];
		assert([identifier isEqual:@"v2:a525b310d975604e26c761134e6c35d1"] && error == nil);
		keys = [Ext4KeyStore keysForVolume:volume error:&error];
		assert(keys.count == 1 && [keys[0] isEqual:identifier]);
		assert([Ext4KeyStore keysForVolume:otherVolume error:&error].count == 0);
		provider = [Ext4KeyStore loadCryptoForVolume:volume error:&error];
		assert(provider != NULL && ext4_native_crypto_count(provider) == 1);
		assert(ext4_native_crypto_identifier(master, sizeof(master), idBytes) == EXT4_OK);
		environment = ext4_native_crypto_environment(provider);
		assert(environment.find_key(provider, 2, idBytes, sizeof(idBytes), &handle) ==
		    EXT4_OK);
		environment.release_key(provider, handle);
		/* Deleting the saved entry does not revoke a mounted snapshot. */
		assert([Ext4KeyStore removeKey:identifier volume:otherVolume error:&error]);
		assert(records.count == 1);
		assert([Ext4KeyStore removeKey:identifier volume:volume error:&error]);
		assert(records.count == 0);
		assert(environment.find_key(provider, 2, idBytes, sizeof(idBytes), &handle) ==
		    EXT4_OK);
		environment.release_key(provider, handle);
		ext4_native_crypto_destroy(provider);
		provider = [Ext4KeyStore loadCryptoForVolume:volume error:&error];
		assert(provider != NULL && ext4_native_crypto_count(provider) == 0);
		ext4_native_crypto_destroy(provider);
		assert([Ext4KeyStore importKeyFromURL:url
					       volume:volume
					 v1Descriptor:@"0102030405060708"
						error:&error] != nil);
		assert([Ext4KeyStore importKeyFromURL:url
					       volume:volume
					 v1Descriptor:@"0102030405060708"
						error:&error] == nil);
		assert(error.code == errSecDuplicateItem && records.count == 1);
		error = nil;
		assert([Ext4KeyStore importKeyFromURL:url
					       volume:volume
					 v1Descriptor:@"not-hex"
						error:&error] == nil);
		assert(error.code == EINVAL && records.count == 1);
		provider = [Ext4KeyStore loadCryptoForVolume:volume error:&error];
		assert(provider != NULL && ext4_native_crypto_count(provider) == 1);
		ext4_native_crypto_destroy(provider);
		malformed = [records[0] mutableCopy];
		malformed[(__bridge id)kSecValueData] = [NSData dataWithBytes:master length:1];
		records[0] = malformed;
		assert([Ext4KeyStore loadCryptoForVolume:volume error:&error] == NULL);
		assert(error.code == EINVAL);
		injected = errSecInteractionNotAllowed;
		assert([Ext4KeyStore loadCryptoForVolume:volume error:&error] == NULL);
		assert(error.code == errSecInteractionNotAllowed);
		injected = errSecSuccess;
		assert(unlink(link.fileSystemRepresentation) == 0);
		assert(unlink(url.fileSystemRepresentation) == 0);
		assert(rmdir(directory) == 0);
		puts("PASS Keychain boundary: scoped queries, import validation, immutable mount "
		     "keys, removal and failures (Security calls replaced)");
	}
	return 0;
}

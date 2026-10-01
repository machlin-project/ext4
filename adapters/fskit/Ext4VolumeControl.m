/* SPDX-License-Identifier: BSD-3-Clause */
#import "Ext4VolumeInternal.h"
#import "Ext4Support.h"
#include <errno.h>

@implementation Ext4Volume (Control)

- (void)startControl
{
	NSURL *directory;
	NSError *error = nil;
	__weak Ext4Volume *weakSelf = self;

	if (_control != nil) {
		return;
	}
	directory = [NSFileManager.defaultManager
	    containerURLForSecurityApplicationGroupIdentifier:Ext4AppGroup];
	_control = [[Ext4ControlServer alloc]
	    initWithDirectory:directory
		     volumeID:[[NSUUID alloc] initWithUUIDBytes:_info.uuid].UUIDString
		      handler:^NSDictionary *(NSDictionary *request) {
			return [weakSelf controlRequest:request];
		      }
			error:&error];
	_controlError = error;
	/* An unavailable control endpoint must not make ordinary disk reads fail. */
	if (_control == nil) {
		NSLog(@"Machlin ext4 control endpoint unavailable: %@", _controlError);
	}
}

- (NSDictionary *)controlRequest:(NSDictionary *)request
{
	@synchronized(self) {
		NSString *command = request[@"command"];
		NSDictionary *arguments = request[@"arguments"] ?: @{};

		if (!_active) {
			return ext4_control_error(ENXIO, @"Volume is no longer active.");
		}
		if ([command isEqual:@"ping"]) {
			return @{ @"result" : @{ @"reply" : @"pong" } };
		}
		if ([command isEqual:@"getInfo"]) {
			ext4_get_info(_fs, &_info);
			return @{
				@"result" : @{
					@"volume" : [[NSUUID alloc] initWithUUIDBytes:_info.uuid]
					    .UUIDString,
					@"blockSize" : @(_info.block_size),
					@"blocks" : @(_info.blocks),
					@"freeBlocks" : @(_info.free_blocks),
					@"inodes" : @(_info.inodes),
					@"freeInodes" : @(_info.free_inodes),
					@"mounted" : @(_mounted),
					@"loadedKeys" : @(ext4_native_crypto_count(_crypto)),
					@"keyStoreAvailable" : self.keyStoreError == nil ? @YES
											 : @NO,
					@"readOnly" : self.writable ? @NO : @YES,
					@"writeUnavailableReason" :
						self.writeAvailabilityError.localizedDescription
					    ?: NSNull.null,
					@"featuresCompat" : @(_info.feature_compat),
					@"featuresIncompat" : @(_info.feature_incompat),
					@"featuresReadOnlyCompat" : @(_info.feature_ro_compat)
				}
			};
		}
		if ([command isEqual:@"getCapabilities"]) {
			return @{
				@"result" : @{
					@"commands" : @[
						@"ping", @"getInfo", @"getCapabilities",
						@"getSettings", @"setSettings", @"dropReadState"
					],
					@"durableWrites" : @(self.writable),
					@"kernelReadMapping" : self.writable ? @NO : @YES,
					@"aclAuthorization" : @NO,
					@"keyManagement" : @YES,
					@"keyStorage" : @"keychain",
					@"liveKeyChanges" : @NO,
					@"onlineFeatureChanges" : @NO
				}
			};
		}
		if ([command isEqual:@"setSettings"]) {
			id retain = arguments[@"retainReadState"];

			if (arguments.count != 1 || ![retain isKindOfClass:NSNumber.class] ||
			    CFGetTypeID((__bridge CFTypeRef)retain) != CFBooleanGetTypeID()) {
				return ext4_control_error(
				    EINVAL, @"Expected only boolean retainReadState.");
			}
			_retainReadState = [retain boolValue];
		}
		if ([command isEqual:@"dropReadState"] ||
		    ([command isEqual:@"setSettings"] && !_retainReadState)) {
			for (Ext4Item *item in _items.objectEnumerator) {
				if (item->hold != NULL) {
					ext4_drop_read_cache(item->hold);
				}
			}
		}
		if ([command isEqual:@"getSettings"] || [command isEqual:@"setSettings"] ||
		    [command isEqual:@"dropReadState"]) {
			return @{ @"result" : @{ @"retainReadState" : @(_retainReadState) } };
		}
		return ext4_control_error(ENOTSUP, @"Unknown or unavailable command.");
	}
}

@end

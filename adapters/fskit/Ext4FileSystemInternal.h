/* SPDX-License-Identifier: BSD-3-Clause */
#import "Ext4FileSystem.h"
#import "Ext4ResourceTask.h"
#import "Ext4Volume.h"

@interface Ext4FileSystem () {
	Ext4Volume *_volume;
	Ext4ResourceIO *_resourceOwner;
	Ext4ResourceTask *_maintenanceTask;
	NSError *_resourceWriteError;
	BOOL _recoveredOnLoad;
}

@end

@interface Ext4FileSystem (MaintenanceSupport)
- (NSProgress *)runMaintenance:(Ext4ResourceTask *)operation
			  task:(FSTask *)task
		   description:(NSString *)description
		    completion:(NSError * (^)(NSError *))completion;
- (NSError *)validateResourceAfterMaintenanceWriting:(BOOL)writing uuid:(NSUUID *)uuid;
@end

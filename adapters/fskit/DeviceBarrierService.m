/* SPDX-License-Identifier: BSD-3-Clause */
#import "Ext4DeviceBarrier.h"
#include <errno.h>
#include <fcntl.h>
#include <sys/disk.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>

@interface Ext4BarrierSession : NSObject <Ext4DeviceBarrierProtocol> {
	int _device;
}
- (void)close;
@end

static NSError *
barrier_error(int code)
{
	return code == 0 ? nil
			 : [NSError errorWithDomain:NSPOSIXErrorDomain code:code userInfo:nil];
}

@implementation Ext4BarrierSession

- (void)checkServiceWithReply:(void (^)(NSError *))reply
{
	reply(nil);
}

- (instancetype)init
{
	self = [super init];
	if (self != nil) {
		_device = -1;
	}
	return self;
}

- (void)openDevice:(NSString *)name
	 blockSize:(uint64_t)expectedSize
	blockCount:(uint64_t)expectedCount
	     reply:(void (^)(NSError *))reply
{
	@synchronized(self) {
		struct stat status;
		uint32_t blockSize;
		uint64_t blockCount;
		int device;
		int error = 0;
		NSString *path;

		if (_device >= 0) {
			reply(barrier_error(EBUSY));
			return;
		}
		if (![name isKindOfClass:NSString.class] || name.length > 64 ||
		    [name rangeOfString:@"\\Adisk[0-9]+(s[0-9]+)*\\z"
				options:NSRegularExpressionSearch]
			    .location == NSNotFound) {
			reply(barrier_error(EINVAL));
			return;
		}
		path = [@"/dev/" stringByAppendingString:name];
		device = open(path.fileSystemRepresentation, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
		if (device < 0) {
			reply(barrier_error(errno));
			return;
		}
		if (fstat(device, &status) != 0 ||
		    ioctl(device, DKIOCGETBLOCKSIZE, &blockSize) != 0 ||
		    ioctl(device, DKIOCGETBLOCKCOUNT, &blockCount) != 0) {
			error = errno;
		} else if (!S_ISBLK(status.st_mode) || blockSize != expectedSize ||
		    blockCount != expectedCount) {
			error = ENXIO;
		} else if (ioctl(device, DKIOCSYNCHRONIZECACHE) != 0) {
			error = errno;
		}
		if (error != 0) {
			close(device);
		} else {
			_device = device;
		}
		reply(barrier_error(error));
	}
}

- (void)synchronizeWithReply:(void (^)(NSError *))reply
{
	@synchronized(self) {
		int error = _device < 0 ? ENXIO : 0;

		if (error == 0 && ioctl(_device, DKIOCSYNCHRONIZECACHE) != 0) {
			error = errno;
		}
		reply(barrier_error(error));
	}
}

- (void)close
{
	@synchronized(self) {
		if (_device >= 0) {
			close(_device);
			_device = -1;
		}
	}
}

- (void)dealloc
{
	[self close];
}

@end

@interface Ext4BarrierListener : NSObject <NSXPCListenerDelegate>
@property NSString *requirement;
@end

@implementation Ext4BarrierListener

- (BOOL)listener:(NSXPCListener *)listener shouldAcceptNewConnection:(NSXPCConnection *)connection
{
	Ext4BarrierSession *session;

	(void)listener;
	if (self.requirement == nil) {
		return NO;
	}
	[connection setCodeSigningRequirement:self.requirement];
	session = [[Ext4BarrierSession alloc] init];
	connection.exportedInterface =
	    [NSXPCInterface interfaceWithProtocol:@protocol(Ext4DeviceBarrierProtocol)];
	connection.exportedObject = session;
	connection.invalidationHandler = ^{
	  [session close];
	};
	[connection activate];
	return YES;
}

@end

int
main(void)
{
	@autoreleasepool {
		Ext4BarrierListener *delegate = [[Ext4BarrierListener alloc] init];
		NSXPCListener *listener;
		NSString *extension = ext4_peer_requirement(EXT4_EXTENSION_IDENTIFIER);
		NSString *app = ext4_peer_requirement(EXT4_APP_IDENTIFIER);

		if (geteuid() != 0) {
			return EXIT_FAILURE;
		}
		if (extension == nil || app == nil) {
			return EXIT_FAILURE;
		}
		delegate.requirement = [NSString stringWithFormat:@"(%@) or (%@)", extension, app];
		listener = [[NSXPCListener alloc] initWithMachServiceName:EXT4_BARRIER_SERVICE];
		listener.delegate = delegate;
		[listener activate];
		[NSRunLoop.currentRunLoop run];
	}
	return EXIT_SUCCESS;
}

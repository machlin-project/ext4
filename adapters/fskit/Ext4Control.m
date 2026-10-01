/* SPDX-License-Identifier: BSD-3-Clause */
#import "Ext4Control.h"
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdlib.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

NSString *const Ext4AppGroup = @"group.org.machlin.ext4";
static const uint32_t ext4_control_version = 1;
static const size_t ext4_control_max_frame = 65536;
static const int64_t ext4_control_timeout_ms = 2000;

@interface Ext4ControlServer () {
	dispatch_source_t _listener;
	NSDictionary * (^_handler)(NSDictionary *);
	NSURL *_socketURL;
	NSURL *_manifestURL;
	int _manifestFD;
	NSString *_token;
	NSString *_instance;
	uid_t _uid;
}

@end

static NSError *
ext4_control_errno(int code)
{
	return [NSError errorWithDomain:NSPOSIXErrorDomain code:code userInfo:nil];
}

NSDictionary *
ext4_control_error(int code, NSString *message)
{
	return @{@"error" : @{ @"code" : @(code), @"message" : message }};
}

static int64_t
ext4_control_clock(void)
{
	struct timespec now;

	clock_gettime(CLOCK_MONOTONIC, &now);
	return (int64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000;
}

static BOOL
ext4_control_transfer(int fd, void *buffer, size_t length, BOOL writing, int64_t deadline)
{
	uint8_t *cursor = buffer;
	struct pollfd pending = { .fd = fd, .events = writing ? POLLOUT : POLLIN };
	int64_t remaining;
	ssize_t count;
	int ready;

	while (length != 0) {
		remaining = deadline - ext4_control_clock();
		if (remaining <= 0) {
			errno = ETIMEDOUT;
			return NO;
		}
		ready = poll(&pending, 1, (int)remaining);
		if (ready < 0 && errno == EINTR) {
			continue;
		}
		if (ready <= 0) {
			if (ready == 0) {
				errno = ETIMEDOUT;
			}
			return NO;
		}
		count = writing ? send(fd, cursor, length, 0) : recv(fd, cursor, length, 0);
		if (count < 0 && (errno == EINTR || errno == EAGAIN)) {
			continue;
		}
		if (count <= 0) {
			if (count == 0) {
				errno = ECONNRESET;
			}
			return NO;
		}
		cursor += count;
		length -= (size_t)count;
	}
	return YES;
}

static NSDictionary *
ext4_control_receive(int fd, int64_t deadline)
{
	uint32_t length;
	NSMutableData *data;
	id value;

	if (!ext4_control_transfer(fd, &length, sizeof(length), NO, deadline)) {
		return nil;
	}
	length = ntohl(length);
	if (length == 0 || length > ext4_control_max_frame) {
		errno = EMSGSIZE;
		return nil;
	}
	data = [NSMutableData dataWithLength:length];
	if (!ext4_control_transfer(fd, data.mutableBytes, length, NO, deadline)) {
		return nil;
	}
	value = [NSJSONSerialization JSONObjectWithData:data options:0 error:NULL];
	if (![value isKindOfClass:NSDictionary.class]) {
		errno = EPROTO;
		return nil;
	}
	return value;
}

static BOOL
ext4_control_send(int fd, NSDictionary *value, int64_t deadline)
{
	NSData *data = [NSJSONSerialization dataWithJSONObject:value options:0 error:NULL];
	uint32_t length;

	if (data == nil || data.length == 0 || data.length > ext4_control_max_frame) {
		errno = EMSGSIZE;
		return NO;
	}
	length = htonl((uint32_t)data.length);
	return ext4_control_transfer(fd, &length, sizeof(length), YES, deadline) &&
	    ext4_control_transfer(fd, (void *)data.bytes, data.length, YES, deadline);
}

static BOOL
ext4_control_address(NSURL *url, struct sockaddr_un *address)
{
	const char *path = url.fileSystemRepresentation;
	size_t length = strlen(path);

	if (length >= sizeof(address->sun_path)) {
		errno = ENAMETOOLONG;
		return NO;
	}
	memset(address, 0, sizeof(*address));
	address->sun_family = AF_UNIX;
	address->sun_len = sizeof(*address);
	memcpy(address->sun_path, path, length + 1);
	return YES;
}

static BOOL
ext4_control_prepare(int fd)
{
	int enabled = 1;

	return fcntl(fd, F_SETFD, FD_CLOEXEC) == 0 && fcntl(fd, F_SETFL, O_NONBLOCK) == 0 &&
	    setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &enabled, sizeof(enabled)) == 0;
}

/* Never follow a manifest symlink or accept another user's capability. */
static NSDictionary *
ext4_control_manifest(NSURL *url)
{
	int fd = open(url.fileSystemRepresentation, O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
	struct stat status;
	NSMutableData *data;
	ssize_t count;
	id value;

	if (fd < 0) {
		return nil;
	}
	if (fstat(fd, &status) < 0 || !S_ISREG(status.st_mode) || status.st_uid != geteuid() ||
	    (status.st_mode & 077) != 0 || status.st_size <= 0 || status.st_size > 4096) {
		close(fd);
		errno = EACCES;
		return nil;
	}
	data = [NSMutableData dataWithLength:(NSUInteger)status.st_size];
	count = read(fd, data.mutableBytes, data.length);
	close(fd);
	if (count != (ssize_t)data.length) {
		errno = EIO;
		return nil;
	}
	value = [NSJSONSerialization JSONObjectWithData:data options:0 error:NULL];
	if (![value isKindOfClass:NSDictionary.class]) {
		errno = EPROTO;
		return nil;
	}
	return value;
}

/* A held advisory lock marks an instance's lifetime without trusting a PID,
 * which can be reused after a crash or reboot. Never remove a live lease. */
static BOOL
ext4_control_live_endpoint(NSURL *url, NSDictionary *manifest)
{
	NSString *name = manifest[@"socket"];
	NSURL *socketURL;
	struct sockaddr_un address;
	struct stat opened;
	struct stat current;
	int fd;
	int saved;
	BOOL live = YES;

	if (![name isKindOfClass:NSString.class] || name.length != 17 || ![name hasPrefix:@"s"] ||
	    [[name substringFromIndex:1] rangeOfCharacterFromSet:
		    [NSCharacterSet characterSetWithCharactersInString:@"0123456789abcdefABCDEF"]
			.invertedSet]
		    .location != NSNotFound ||
	    ![url.lastPathComponent isEqualToString:[name stringByAppendingString:@".json"]]) {
		return NO;
	}
	socketURL = [[url URLByDeletingLastPathComponent] URLByAppendingPathComponent:name];
	if ([manifest[@"lease"] isEqual:@1]) {
		fd = open(url.fileSystemRepresentation, O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
		if (fd < 0) {
			return NO;
		}
		if (fstat(fd, &opened) == 0 && S_ISREG(opened.st_mode) &&
		    opened.st_uid == geteuid() && (opened.st_mode & 077) == 0 &&
		    flock(fd, LOCK_EX | LOCK_NB) == 0) {
			live = NO;
			/* Only this exact owned manifest and its corresponding socket are
			 * eligible. A replaced inode or a non-socket is never deleted. */
			if (lstat(url.fileSystemRepresentation, &current) == 0 &&
			    current.st_dev == opened.st_dev && current.st_ino == opened.st_ino) {
				if (lstat(socketURL.fileSystemRepresentation, &current) == 0 &&
				    S_ISSOCK(current.st_mode) && current.st_uid == geteuid() &&
				    (current.st_mode & 077) == 0) {
					(void)unlink(socketURL.fileSystemRepresentation);
				}
				(void)unlink(url.fileSystemRepresentation);
			}
		}
		close(fd);
		return live;
	}
	/* Older modules did not hold a lease. Filter definitively dead sockets,
	 * preserving their manifests rather than guessing ownership for deletion.
	 * Other failures remain visible to the authenticated request path. */
	if (!ext4_control_address(socketURL, &address)) {
		return NO;
	}
	fd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (fd < 0) {
		return YES;
	}
	if (ext4_control_prepare(fd) &&
	    connect(fd, (struct sockaddr *)&address, sizeof(address)) < 0) {
		saved = errno;
		live = saved != ECONNREFUSED && saved != ENOENT;
	}
	close(fd);
	return live;
}

@implementation Ext4ControlServer

- (instancetype)initWithDirectory:(NSURL *)directory
			 volumeID:(NSString *)volumeID
			  handler:(NSDictionary * (^)(NSDictionary *))handler
			    error:(NSError **)error
{
	struct sockaddr_un address;
	struct stat status;
	uint8_t random[32];
	NSString *name;
	NSData *manifest;
	int fd = -1;
	int manifestFD = -1;
	int saved;
	BOOL bound = NO;
	BOOL created = NO;

	self = [super init];
	if (self == nil) {
		return nil;
	}
	_manifestFD = -1;
	/* Production supplies FileManager's App Group URL, never a guessed path. */
	if (directory == nil || lstat(directory.fileSystemRepresentation, &status) < 0 ||
	    !S_ISDIR(status.st_mode) || status.st_uid != geteuid() ||
	    (status.st_mode & 0022) != 0) {
		errno = EACCES;
		goto failed;
	}
	_uid = geteuid();
	_instance = NSUUID.UUID.UUIDString;
	name = [@"s" stringByAppendingString:[[_instance stringByReplacingOccurrencesOfString:@"-"
										   withString:@""]
						 substringToIndex:16]];
	_socketURL = [directory URLByAppendingPathComponent:name];
	_manifestURL =
	    [directory URLByAppendingPathComponent:[name stringByAppendingString:@".json"]];
	arc4random_buf(random, sizeof(random));
	_token = [[NSData dataWithBytes:random
				 length:sizeof(random)] base64EncodedStringWithOptions:0];
	if (!ext4_control_address(_socketURL, &address)) {
		goto failed;
	}
	fd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (fd < 0 || !ext4_control_prepare(fd) ||
	    bind(fd, (struct sockaddr *)&address, sizeof(address)) < 0) {
		goto failed;
	}
	bound = YES;
	if (chmod(_socketURL.fileSystemRepresentation, 0600) < 0 || listen(fd, 8) < 0) {
		goto failed;
	}
	manifest = [NSJSONSerialization dataWithJSONObject:@{
		@"version" : @(ext4_control_version),
		@"lease" : @1,
		@"instance" : _instance,
		@"volume" : volumeID,
		@"socket" : name,
		@"token" : _token
	}
						   options:0
						     error:NULL];
	manifestFD = open(_manifestURL.fileSystemRepresentation,
	    O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
	if (manifestFD < 0) {
		goto failed;
	}
	created = YES;
	if (flock(manifestFD, LOCK_EX | LOCK_NB) < 0) {
		goto failed;
	}
	if (write(manifestFD, manifest.bytes, manifest.length) != (ssize_t)manifest.length) {
		errno = EIO;
		goto failed;
	}
	_manifestFD = manifestFD;
	manifestFD = -1;
	_handler = [handler copy];
	[self listenOnDescriptor:fd];
	return self;
failed:
	saved = errno;
	if (fd >= 0) {
		close(fd);
	}
	if (manifestFD >= 0) {
		close(manifestFD);
	}
	if (bound) {
		unlink(_socketURL.fileSystemRepresentation);
	}
	if (created) {
		unlink(_manifestURL.fileSystemRepresentation);
	}
	_socketURL = nil;
	_manifestURL = nil;
	if (error != NULL) {
		*error = ext4_control_errno(saved);
	}
	return nil;
}

- (void)listenOnDescriptor:(int)fd
{
	dispatch_queue_t queue =
	    dispatch_queue_create("org.machlin.ext4.control", DISPATCH_QUEUE_SERIAL);
	__weak Ext4ControlServer *weakSelf = self;

	_listener = dispatch_source_create(DISPATCH_SOURCE_TYPE_READ, (uintptr_t)fd, 0, queue);
	dispatch_source_set_event_handler(_listener, ^{
	  @autoreleasepool {
		  [weakSelf acceptRequest:fd];
	  }
	});
	dispatch_source_set_cancel_handler(_listener, ^{
	  close(fd);
	});
	dispatch_resume(_listener);
}

- (NSURL *)manifestURL
{
	return _manifestURL;
}

- (void)acceptRequest:(int)listener
{
	int fd = accept(listener, NULL, NULL);
	uid_t uid;
	gid_t gid;
	int64_t deadline = ext4_control_clock() + ext4_control_timeout_ms;
	NSDictionary *request;
	NSDictionary *response;
	NSMutableDictionary *envelope;

	if (fd < 0) {
		return;
	}
	if (!ext4_control_prepare(fd) || getpeereid(fd, &uid, &gid) < 0 || uid != _uid) {
		close(fd);
		return;
	}
	request = ext4_control_receive(fd, deadline);
	if (request == nil) {
		close(fd);
		return;
	}
	if (![request[@"token"] isKindOfClass:NSString.class] ||
	    ![request[@"token"] isEqualToString:_token]) {
		response = ext4_control_error(EACCES, @"Invalid capability.");
	} else if (![request[@"version"] isEqual:@(ext4_control_version)] ||
	    ![request[@"instance"] isEqual:_instance]) {
		response = ext4_control_error(EPROTO, @"Protocol or instance mismatch.");
	} else if (![request[@"command"] isKindOfClass:NSString.class] ||
	    (request[@"arguments"] != nil &&
		![request[@"arguments"] isKindOfClass:NSDictionary.class])) {
		response = ext4_control_error(EINVAL, @"Invalid request.");
	} else {
		response = _handler(request);
	}
	envelope = [response mutableCopy];
	if (envelope == nil) {
		envelope = [ext4_control_error(ENXIO, @"Volume is no longer active.") mutableCopy];
	}
	envelope[@"version"] = @(ext4_control_version);
	envelope[@"instance"] = _instance;
	(void)ext4_control_send(fd, envelope, deadline);
	close(fd);
}

- (void)stop
{
	@synchronized(self) {
		if (_listener != nil) {
			dispatch_source_cancel(_listener);
			_listener = nil;
			unlink(_socketURL.fileSystemRepresentation);
			unlink(_manifestURL.fileSystemRepresentation);
		}
		if (_manifestFD >= 0) {
			close(_manifestFD);
			_manifestFD = -1;
		}
	}
}

- (void)dealloc
{
	[self stop];
}

@end

@implementation Ext4ControlClient

+ (NSArray<NSURL *> *)endpointsInDirectory:(NSURL *)directory
{
	NSMutableArray<NSURL *> *endpoints = [NSMutableArray array];
	NSArray<NSURL *> *entries;

	if (directory == nil) {
		return endpoints;
	}
	entries = [NSFileManager.defaultManager
	      contentsOfDirectoryAtURL:directory
	    includingPropertiesForKeys:nil
			       options:NSDirectoryEnumerationSkipsHiddenFiles
				 error:NULL];
	for (NSURL *entry in entries) {
		NSDictionary *manifest;

		if ([entry.lastPathComponent hasPrefix:@"s"] &&
		    [entry.pathExtension isEqualToString:@"json"] &&
		    (manifest = ext4_control_manifest(entry)) != nil &&
		    ext4_control_live_endpoint(entry, manifest)) {
			[endpoints addObject:entry];
		}
	}
	return endpoints;
}

+ (NSDictionary *)request:(NSDictionary *)request
		 endpoint:(NSURL *)manifestURL
		    error:(NSError **)error
{
	NSDictionary *manifest = ext4_control_manifest(manifestURL);
	NSMutableDictionary *envelope;
	NSDictionary *response = nil;
	NSString *name = manifest[@"socket"];
	struct sockaddr_un address;
	int fd = -1;
	int connection;
	int saved;
	uid_t uid;
	gid_t gid;
	int64_t deadline = ext4_control_clock() + ext4_control_timeout_ms;

	if (manifest == nil) {
		goto done;
	}
	if (![name isKindOfClass:NSString.class] || name.length == 0 ||
	    ![name.lastPathComponent isEqualToString:name] || [name isEqualToString:@"."] ||
	    [name isEqualToString:@".."] || [name rangeOfString:@"\0"].location != NSNotFound ||
	    ![manifest[@"token"] isKindOfClass:NSString.class] ||
	    ![manifest[@"instance"] isKindOfClass:NSString.class] ||
	    ![manifest[@"version"] isEqual:@(ext4_control_version)]) {
		errno = EPROTO;
		goto done;
	}
	if (!ext4_control_address(
		[[manifestURL URLByDeletingLastPathComponent] URLByAppendingPathComponent:name],
		&address)) {
		goto done;
	}
	fd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (fd < 0 || !ext4_control_prepare(fd)) {
		goto done;
	}
	connection = connect(fd, (struct sockaddr *)&address, sizeof(address));
	if (connection < 0 && errno != EINPROGRESS) {
		goto done;
	}
	if (connection < 0) {
		struct pollfd pending = { .fd = fd, .events = POLLOUT };
		int status = 0;
		socklen_t size = sizeof(status);

		if (poll(&pending, 1, (int)ext4_control_timeout_ms) <= 0 ||
		    getsockopt(fd, SOL_SOCKET, SO_ERROR, &status, &size) < 0 || status != 0) {
			errno = status != 0 ? status : ETIMEDOUT;
			goto done;
		}
	}
	if (getpeereid(fd, &uid, &gid) < 0 || uid != geteuid()) {
		errno = EACCES;
		goto done;
	}
	envelope = [request mutableCopy];
	envelope[@"version"] = @(ext4_control_version);
	envelope[@"instance"] = manifest[@"instance"];
	envelope[@"token"] = manifest[@"token"];
	if (!ext4_control_send(fd, envelope, deadline)) {
		goto done;
	}
	response = ext4_control_receive(fd, deadline);
	if (response != nil &&
	    (![response[@"version"] isEqual:@(ext4_control_version)] ||
		![response[@"instance"] isEqual:manifest[@"instance"]])) {
		response = nil;
		errno = EPROTO;
	}
done:
	saved = errno;
	if (fd >= 0) {
		close(fd);
	}
	if (response == nil && error != NULL) {
		*error = ext4_control_errno(saved);
	}
	return response;
}

@end

/* SPDX-License-Identifier: BSD-3-Clause */
#import "../adapters/fskit/Ext4Control.h"
#include <arpa/inet.h>
#include <assert.h>
#include <errno.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

static NSDictionary *
request(NSURL *endpoint, NSString *command)
{
	NSError *error = nil;
	NSDictionary *response = [Ext4ControlClient request:@{ @"command" : command }
						   endpoint:endpoint
						      error:&error];

	assert(error == nil && response != nil);
	return response;
}

static int
raw_connection(NSURL *manifestURL, NSDictionary **manifest)
{
	struct sockaddr_un address = { 0 };
	NSData *data = [NSData dataWithContentsOfURL:manifestURL];
	NSURL *socketURL;
	struct timeval timeout = { .tv_sec = 4, .tv_usec = 0 };
	int fd;

	*manifest = [NSJSONSerialization JSONObjectWithData:data options:0 error:NULL];
	socketURL = [[manifestURL URLByDeletingLastPathComponent]
	    URLByAppendingPathComponent:(*manifest)[@"socket"]];
	address.sun_family = AF_UNIX;
	address.sun_len = sizeof(address);
	strlcpy(address.sun_path, socketURL.fileSystemRepresentation, sizeof(address.sun_path));
	fd = socket(AF_UNIX, SOCK_STREAM, 0);
	assert(fd >= 0);
	assert(setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) == 0);
	assert(connect(fd, (struct sockaddr *)&address, sizeof(address)) == 0);
	return fd;
}

static NSDictionary *
raw_request(NSURL *endpoint, NSString *field, id value)
{
	NSDictionary *manifest;
	int fd = raw_connection(endpoint, &manifest);
	NSMutableDictionary *wire = [manifest mutableCopy];
	NSData *data;
	NSMutableData *reply;
	uint32_t length;
	size_t index;
	uint8_t *bytes;

	wire[@"command"] = @"ping";
	wire[field] = value;
	data = [NSJSONSerialization dataWithJSONObject:wire options:0 error:NULL];
	length = htonl((uint32_t)data.length);
	bytes = (uint8_t *)&length;
	for (index = 0; index < sizeof(length); index++) {
		assert(write(fd, bytes + index, 1) == 1);
	}
	for (index = 0; index < data.length; index++) {
		assert(write(fd, (const uint8_t *)data.bytes + index, 1) == 1);
	}
	assert(recv(fd, &length, sizeof(length), MSG_WAITALL) == sizeof(length));
	length = ntohl(length);
	assert(length <= 65536);
	reply = [NSMutableData dataWithLength:length];
	assert(recv(fd, reply.mutableBytes, length, MSG_WAITALL) == length);
	close(fd);
	return [NSJSONSerialization JSONObjectWithData:reply options:0 error:NULL];
}

static void
idle_peer(NSURL *endpoint)
{
	NSDictionary *manifest;
	int fd = raw_connection(endpoint, &manifest);
	char byte;

	/* The server must close a silent peer within the socket's four-second test
	 * timeout, without needing a second request to make progress. */
	assert(recv(fd, &byte, 1, 0) == 0);
	close(fd);
}

static void
malformed(NSURL *endpoint, uint32_t size, const char *body)
{
	NSDictionary *manifest;
	int fd = raw_connection(endpoint, &manifest);
	uint32_t length = htonl(size);
	char result;

	assert(write(fd, &length, sizeof(length)) == sizeof(length));
	if (body != NULL) {
		assert(write(fd, body, strlen(body)) == (ssize_t)strlen(body));
	}
	shutdown(fd, SHUT_WR);
	assert(recv(fd, &result, 1, 0) == 0);
	close(fd);
}

static void
crashed_instance(NSString *executable, NSURL *directory, NSUInteger liveCount)
{
	NSTask *child = [NSTask new];
	NSPipe *output = [NSPipe pipe];
	NSData *ready;
	NSString *path;
	NSURL *endpoint;
	NSDictionary *manifest;
	NSURL *socketURL;
	NSError *error = nil;

	child.executableURL = [NSURL fileURLWithPath:executable];
	child.arguments = @[ @"--crash-server", directory.path ];
	child.standardOutput = output;
	assert([child launchAndReturnError:&error]);
	ready = output.fileHandleForReading.availableData;
	path = [[[NSString alloc] initWithData:ready encoding:NSUTF8StringEncoding]
	    stringByTrimmingCharactersInSet:NSCharacterSet.newlineCharacterSet];
	assert([path hasPrefix:directory.path] && [path hasSuffix:@".json"]);
	endpoint = [NSURL fileURLWithPath:path];
	manifest = [NSJSONSerialization JSONObjectWithData:[NSData dataWithContentsOfURL:endpoint]
						   options:0
						     error:NULL];
	socketURL = [directory URLByAppendingPathComponent:manifest[@"socket"]];
	assert([Ext4ControlClient endpointsInDirectory:directory].count == liveCount + 1);
	assert(kill(child.processIdentifier, SIGKILL) == 0);
	[child waitUntilExit];
	assert(child.terminationReason == NSTaskTerminationReasonUncaughtSignal);
	assert(access(endpoint.fileSystemRepresentation, F_OK) == 0);
	assert(access(socketURL.fileSystemRepresentation, F_OK) == 0);
	assert([Ext4ControlClient endpointsInDirectory:directory].count == liveCount);
	assert(access(endpoint.fileSystemRepresentation, F_OK) < 0 && errno == ENOENT);
	assert(access(socketURL.fileSystemRepresentation, F_OK) < 0 && errno == ENOENT);
}

static void
stale_candidates(NSURL *directory, NSUInteger liveCount)
{
	NSString *name = @"s0123456789abcdef";
	NSURL *endpoint =
	    [directory URLByAppendingPathComponent:[name stringByAppendingString:@".json"]];
	NSURL *socketURL = [directory URLByAppendingPathComponent:name];
	NSMutableDictionary *manifest = [@{
		@"socket" : name,
		@"version" : @1,
		@"instance" : @"legacy-instance",
		@"token" : @"synthetic-test-capability"
	} mutableCopy];
	struct sockaddr_un address = { 0 };
	NSData *bytes;
	int fd;

	address.sun_family = AF_UNIX;
	address.sun_len = sizeof(address);
	strlcpy(address.sun_path, socketURL.fileSystemRepresentation, sizeof(address.sun_path));
	fd = socket(AF_UNIX, SOCK_STREAM, 0);
	assert(fd >= 0 && bind(fd, (struct sockaddr *)&address, sizeof(address)) == 0);
	assert(close(fd) == 0);
	bytes = [NSJSONSerialization dataWithJSONObject:manifest options:0 error:NULL];
	assert([bytes writeToURL:endpoint atomically:YES]);
	assert(chmod(endpoint.fileSystemRepresentation, 0600) == 0);
	/* An old-format crash cannot hide live volumes or trigger speculative deletion. */
	assert([Ext4ControlClient endpointsInDirectory:directory].count == liveCount);
	assert(access(endpoint.fileSystemRepresentation, F_OK) == 0);
	assert(access(socketURL.fileSystemRepresentation, F_OK) == 0);
	assert(unlink(socketURL.fileSystemRepresentation) == 0);
	manifest[@"lease"] = @1;
	bytes = [NSJSONSerialization dataWithJSONObject:manifest options:0 error:NULL];
	assert([bytes writeToURL:endpoint atomically:YES]);
	assert(chmod(endpoint.fileSystemRepresentation, 0600) == 0);
	assert([[@"keep this file" dataUsingEncoding:NSUTF8StringEncoding] writeToURL:socketURL
									   atomically:YES]);
	/* Only socket nodes are eligible for removal; ordinary content stays intact. */
	assert([Ext4ControlClient endpointsInDirectory:directory].count == liveCount);
	assert([[NSData dataWithContentsOfURL:socketURL]
	    isEqualToData:[@"keep this file" dataUsingEncoding:NSUTF8StringEncoding]]);
	assert(access(endpoint.fileSystemRepresentation, F_OK) < 0 && errno == ENOENT);
	assert(unlink(socketURL.fileSystemRepresentation) == 0);
	/* Reject a FIFO before a blocking read can hide every other endpoint. */
	assert(mkfifo(endpoint.fileSystemRepresentation, 0600) == 0);
	assert([Ext4ControlClient endpointsInDirectory:directory].count == liveCount);
	assert(unlink(endpoint.fileSystemRepresentation) == 0);
}

int
main(int argc, const char **argv)
{
	@autoreleasepool {
		char path[] = "/tmp/ext4-ipc-XXXXXX";
		NSURL *directory;
		NSError *error = nil;
		Ext4ControlServer *first;
		Ext4ControlServer *second;
		NSURL *endpoint;
		NSDictionary *response;
		struct stat status;
		NSTask *child;
		NSPipe *output;
		int calls = 0;
		int index;
		NSURL *copied;
		NSData *manifest;
		NSString *longName;
		NSURL *longDirectory;

		if (argc == 3 && strcmp(argv[1], "--crash-server") == 0) {
			first = [[Ext4ControlServer alloc]
			    initWithDirectory:[NSURL fileURLWithPath:@(argv[2]) isDirectory:YES]
				     volumeID:@"crash-test"
				      handler:^NSDictionary *(NSDictionary *value) {
					(void)value;
					return @{@"result" : @{}};
				      }
					error:&error];
			assert(first != nil && error == nil);
			printf("%s\n", first.manifestURL.fileSystemRepresentation);
			fflush(stdout);
			/* Keep the owning object live until the parent kills this process. */
			while (first != nil) {
				pause();
			}
			return 0;
		}
		if (argc == 3 && strcmp(argv[1], "--client") == 0) {
			response = request([NSURL fileURLWithPath:@(argv[2])], @"ping");
			assert([response[@"result"][@"reply"] isEqual:@"pong"]);
			puts("PASS independent IPC client process");
			return 0;
		}
		assert(mkdtemp(path) != NULL);
		directory = [NSURL fileURLWithPath:@(path) isDirectory:YES];
		first = [[Ext4ControlServer alloc]
		    initWithDirectory:directory
			     volumeID:@"same-uuid"
			      handler:^NSDictionary *(NSDictionary *value) {
				if ([value[@"command"] isEqual:@"ping"]) {
					return @{@"result" : @{ @"reply" : @"pong" }};
				}
				return ext4_control_error(ENOTSUP, @"Unknown command.");
			      }
				error:&error];
		assert(first != nil && error == nil);
		second = [[Ext4ControlServer alloc]
		    initWithDirectory:directory
			     volumeID:@"same-uuid"
			      handler:^NSDictionary *(NSDictionary *value) {
				(void)value;
				return @{@"result" : @{}};
			      }
				error:&error];
		assert(second != nil && ![first.manifestURL isEqual:second.manifestURL]);
		assert([Ext4ControlClient endpointsInDirectory:directory].count == 2);
		crashed_instance(@(argv[0]), directory, 2);
		stale_candidates(directory, 2);
		endpoint = first.manifestURL;
		assert(lstat(endpoint.fileSystemRepresentation, &status) == 0 &&
		    (status.st_mode & 0777) == 0600);
		response = request(endpoint, @"ping");
		assert([response[@"result"][@"reply"] isEqual:@"pong"]);
		response = request(endpoint, @"unknown");
		assert([response[@"error"][@"code"] intValue] == ENOTSUP);
		assert([raw_request(endpoint, @"token", @"wrong")[@"error"][@"code"] intValue] ==
		    EACCES);
		assert([raw_request(endpoint, @"version", @999)[@"error"][@"code"] intValue] ==
		    EPROTO);
		assert([raw_request(endpoint, @"instance", @"old-instance")[@"error"][@"code"]
			   intValue] == EPROTO);
		assert([raw_request(endpoint, @"arguments", @[])[@"error"][@"code"] intValue] ==
		    EINVAL);
		assert(
		    [raw_request(endpoint, @"command", @42)[@"error"][@"code"] intValue] == EINVAL);
		malformed(endpoint, 65537, NULL);
		malformed(endpoint, 0, NULL);
		malformed(endpoint, 2, "[]");
		malformed(endpoint, 1, "{");
		malformed(endpoint, 40, "{");
		idle_peer(endpoint);
		for (index = 0; index < 8; index++) {
			response = request(endpoint, @"ping");
			calls += [response[@"result"][@"reply"] isEqual:@"pong"];
		}
		assert(calls == 8);
		child = [NSTask new];
		child.executableURL = [NSURL fileURLWithPath:@(argv[0])];
		child.arguments = @[ @"--client", endpoint.path ];
		output = [NSPipe pipe];
		child.standardOutput = output;
		assert([child launchAndReturnError:&error]);
		[child waitUntilExit];
		assert(child.terminationStatus == 0);
		/* Untrusted manifest permissions and symlinks never expose capabilities. */
		manifest = [NSData dataWithContentsOfURL:endpoint];
		copied = [directory URLByAppendingPathComponent:@"s-copy.json"];
		assert([manifest writeToURL:copied atomically:YES]);
		assert(chmod(copied.fileSystemRepresentation, 0644) == 0);
		error = nil;
		assert([Ext4ControlClient request:@{ @"command" : @"ping" }
					 endpoint:copied
					    error:&error] == nil &&
		    error.code == EACCES);
		assert(unlink(copied.fileSystemRepresentation) == 0);
		assert(symlink(endpoint.fileSystemRepresentation,
			   copied.fileSystemRepresentation) == 0);
		error = nil;
		assert([Ext4ControlClient request:@{ @"command" : @"ping" }
					 endpoint:copied
					    error:&error] == nil);
		assert(unlink(copied.fileSystemRepresentation) == 0);
		longName = [@"" stringByPaddingToLength:110 withString:@"a" startingAtIndex:0];
		longDirectory = [directory URLByAppendingPathComponent:longName];
		assert(mkdir(longDirectory.fileSystemRepresentation, 0700) == 0);
		error = nil;
		assert([[Ext4ControlServer alloc]
			   initWithDirectory:longDirectory
				    volumeID:@"test"
				     handler:^NSDictionary *(NSDictionary *value) {
				       return value;
				     }
				       error:&error] == nil);
		assert(error.code == ENAMETOOLONG);
		assert(rmdir(longDirectory.fileSystemRepresentation) == 0);
		[first stop];
		[first stop];
		[second stop];
		assert([Ext4ControlClient endpointsInDirectory:directory].count == 0);
		error = nil;
		assert([Ext4ControlClient request:@{ @"command" : @"ping" }
					 endpoint:endpoint
					    error:&error] == nil);
		assert(rmdir(path) == 0);
		puts("PASS IPC framing, independent process, authentication, version, bounds and "
		     "cleanup");
	}
	return 0;
}

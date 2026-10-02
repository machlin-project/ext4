/* SPDX-License-Identifier: BSD-3-Clause */
#import "Ext4ResourceTask.h"
#import "Ext4CheckProtocol.h"
#import "Ext4Support.h"
#import <FSKit/FSKit.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

enum {
	Ext4ResourceIdleSeconds = 300,
	Ext4ResourceCancelSeconds = 5,
	Ext4ResourcePollMilliseconds = 250,
	Ext4ResourceLogBytes = 4096,
	Ext4ResourceMaxChannels = 32,
	Ext4ResourceMaxArguments = 8,
	Ext4ResourceArgumentBytes = 1024,
	Ext4ResourceCanceled = 32
};

@interface Ext4ResourceTask () {
	Ext4ResourceIO *_resource;
	NSURL *_executable;
	NSArray<NSString *> *_arguments;
	NSIndexSet *_successfulExitStatuses;
	BOOL _writable;
	pid_t _pid;
	int _socket;
	BOOL _cancelled;
	BOOL _started;
	int _ioError;
}

@end

static NSError *
ext4_resource_error(int code)
{
	return [NSError errorWithDomain:NSPOSIXErrorDomain code:code userInfo:nil];
}

static void
ext4_resource_log(
    FSTask *task, NSMutableData *pending, const void *bytes, size_t length, BOOL finished)
{
	const uint8_t *cursor = bytes;
	NSString *message;
	size_t index;
	uint8_t byte;

	for (index = 0; index < length; index++) {
		byte = cursor[index];
		if (byte != '\n' && byte != '\r' && pending.length < Ext4ResourceLogBytes) {
			[pending appendBytes:&byte length:1];
		}
		if (byte == '\n' || byte == '\r' || pending.length == Ext4ResourceLogBytes) {
			message = [[NSString alloc] initWithData:pending
							encoding:NSUTF8StringEncoding];
			if (message == nil) {
				message = [[NSString alloc] initWithData:pending
								encoding:NSISOLatin1StringEncoding];
			}
			if (message.length != 0) {
				[task logMessage:message];
			}
			pending.length = 0;
		}
	}
	if (finished && pending.length != 0) {
		ext4_resource_log(task, pending, "\n", 1, NO);
	}
}

@implementation Ext4ResourceTask

- (instancetype)initWithResource:(Ext4ResourceIO *)resource
		      executable:(NSURL *)executable
		       arguments:(NSArray<NSString *> *)arguments
			writable:(BOOL)writable
	  successfulExitStatuses:(NSIndexSet *)statuses
{
	if (resource == nil || !executable.isFileURL || arguments.count == 0 ||
	    arguments.count > Ext4ResourceMaxArguments || statuses.count == 0 ||
	    ![statuses containsIndex:0] || statuses.lastIndex > UINT8_MAX ||
	    resource.blockSize == 0 || resource.blockSize > UINT32_MAX ||
	    (resource.blockSize & (resource.blockSize - 1)) != 0) {
		return nil;
	}
	for (NSString *argument in arguments) {
		NSData *encoded;

		if (![argument isKindOfClass:NSString.class]) {
			return nil;
		}
		encoded = [argument dataUsingEncoding:NSUTF8StringEncoding];
		if (encoded == nil || argument.UTF8String == NULL ||
		    encoded.length > Ext4ResourceArgumentBytes ||
		    strlen(argument.UTF8String) != encoded.length) {
			return nil;
		}
	}
	self = [super init];
	if (self != nil) {
		_resource = resource;
		_writable = writable;
		_arguments = [[NSArray alloc] initWithArray:arguments copyItems:YES];
		_successfulExitStatuses = [statuses copy];
		_executable = executable;
		_socket = -1;
	}
	return self;
}

- (void)cancel
{
	@synchronized(self) {
		_cancelled = YES;
		if (_socket >= 0) {
			(void)shutdown(_socket, SHUT_RDWR);
		}
		if (_pid > 0) {
			(void)kill(_pid, SIGTERM);
		}
	}
}

- (BOOL)reapWithStatus:(int *)status error:(int *)error
{
	@synchronized(self) {
		pid_t result = waitpid(_pid, status, WNOHANG);

		if (result == _pid || (result < 0 && errno != EINTR)) {
			*error = result < 0 ? errno : 0;
			/* Reaping and clearing the PID share cancellation's monitor; no
			 * signal can target a PID after ownership of that child ends. */
			_pid = 0;
			return YES;
		}
		return NO;
	}
}

- (int)serveWithBuffer:(void *)buffer channels:(unsigned *)channels
{
	struct ext4_check_message request;
	struct ext4_check_message response;
	uint32_t operation;
	uint32_t length;
	uint32_t flags;
	uint64_t offset;
	int64_t deadline = ext4_check_deadline(EXT4_CHECK_REQUEST_SECONDS);
	int error;
	enum ext4_result result = EXT4_OK;

	error = ext4_check_receive(_socket, &request, sizeof(request), deadline);
	if (error != 0) {
		if (_ioError == 0) {
			_ioError = error;
		}
		return error;
	}
	operation = ext4_check_decode32(request.operation);
	length = ext4_check_decode32(request.length);
	flags = ext4_check_decode32(request.flags);
	offset = ext4_check_decode64(request.offset);
	if (ext4_check_decode32(request.magic) != EXT4_CHECK_MAGIC ||
	    ext4_check_decode32(request.version) != EXT4_CHECK_VERSION ||
	    ext4_check_decode32(request.error) != 0 || operation < EXT4_CHECK_OPEN ||
	    operation > EXT4_CHECK_CLOSE ||
	    (flags != 0 && (operation != EXT4_CHECK_OPEN || flags != EXT4_CHECK_WRITABLE))) {
		return EPROTO;
	}
	response = request;
	response.flags = ext4_check_encode32(0);
	if (operation == EXT4_CHECK_OPEN) {
		if (offset != 0 || length != 0 || *channels == Ext4ResourceMaxChannels) {
			return EPROTO;
		}
		if (flags != 0 && (!_writable || !_resource.writable)) {
			error = EROFS;
		} else {
			(*channels)++;
			response.offset = ext4_check_encode64(_resource.sizeBytes);
			response.length = ext4_check_encode32((uint32_t)_resource.blockSize);
			response.flags = ext4_check_encode32(
			    _writable && _resource.writable ? EXT4_CHECK_WRITABLE : 0);
		}
	} else if (*channels == 0) {
		return EPROTO;
	} else if (operation == EXT4_CHECK_READ || operation == EXT4_CHECK_WRITE) {
		if (length == 0 || length > EXT4_CHECK_MAX_TRANSFER ||
		    offset > _resource.sizeBytes || length > _resource.sizeBytes - offset) {
			return EPROTO;
		}
		if (operation == EXT4_CHECK_WRITE) {
			error = ext4_check_receive(_socket, buffer, length, deadline);
			if (error != 0) {
				return error;
			}
			result = !_writable
			    ? EXT4_READ_ONLY
			    : [_resource writeAt:offset buffer:buffer length:length];
		} else {
			result = [_resource readAt:offset buffer:buffer length:length];
		}
	} else {
		if (offset != 0 || length != 0) {
			return EPROTO;
		}
		if (operation == EXT4_CHECK_FLUSH) {
			result = !_writable ? EXT4_READ_ONLY : [_resource synchronize];
		} else {
			(*channels)--;
		}
	}
	if (error == 0 && result != EXT4_OK) {
		error = (int)ext4_error(result).code;
	}
	if (error != 0) {
		response.offset = ext4_check_encode64(0);
		if (_ioError == 0) {
			_ioError = error;
		}
		response.length = ext4_check_encode32(0);
		response.flags = ext4_check_encode32(0);
	}
	response.error = ext4_check_encode32((uint32_t)error);
	error = ext4_check_send(_socket, &response, sizeof(response), deadline);
	if (error == 0 && result == EXT4_OK && operation == EXT4_CHECK_READ) {
		error = ext4_check_send(_socket, buffer, length, deadline);
	}
	return error;
}

- (NSError *)runWithTask:(FSTask *)task
{
	posix_spawn_file_actions_t actions;
	posix_spawnattr_t attributes;
	sigset_t signals;
	int connection[2] = { -1, -1 };
	int logs[2] = { -1, -1 };
	struct pollfd descriptors[2];
	NSMutableData *pending = [NSMutableData data];
	char **arguments = NULL;
	NSUInteger index;
	char *environment[] = { "PATH=/usr/bin:/bin", "LC_ALL=C", NULL };
	uint8_t logBytes[Ext4ResourceLogBytes];
	void *buffer = NULL;
	ssize_t count;
	unsigned channels = 0;
	int error = 0;
	int status = 0;
	int waitError = 0;
	BOOL exited = NO;
	BOOL socketEOF = NO;
	BOOL logEOF = NO;
	BOOL actionsReady = NO;
	BOOL attributesReady = NO;
	int64_t idle = ext4_check_deadline(Ext4ResourceIdleSeconds);
	int64_t stopping = 0;

	@synchronized(self) {
		if (_started || _cancelled) {
			return ext4_resource_error(_cancelled ? ECANCELED : EBUSY);
		}
		_started = YES;
	}
	if (_resource.isRevoked) {
		return ext4_resource_error(EIO);
	}
	if (_writable && !_resource.writable) {
		return ext4_resource_error(EROFS);
	}
	arguments = calloc(_arguments.count + 2, sizeof(*arguments));
	buffer = malloc(EXT4_CHECK_MAX_TRANSFER);
	if (arguments == NULL || buffer == NULL) {
		error = ENOMEM;
		goto finish;
	}
	arguments[0] = (char *)_executable.fileSystemRepresentation;
	for (index = 0; index < _arguments.count; index++) {
		arguments[index + 1] = (char *)_arguments[index].UTF8String;
	}
	if (socketpair(AF_UNIX, SOCK_STREAM, 0, connection) != 0 || pipe(logs) != 0) {
		error = errno;
		goto finish;
	}
	error = posix_spawn_file_actions_init(&actions);
	if (error != 0) {
		goto finish;
	}
	actionsReady = YES;
	error = posix_spawnattr_init(&attributes);
	if (error != 0) {
		goto finish;
	}
	attributesReady = YES;
	sigemptyset(&signals);
	error = posix_spawnattr_setsigmask(&attributes, &signals);
	if (error == 0) {
		error = posix_spawnattr_setflags(
		    &attributes, POSIX_SPAWN_CLOEXEC_DEFAULT | POSIX_SPAWN_SETSIGMASK);
	}
	if (error == 0) {
		error = posix_spawn_file_actions_adddup2(&actions, connection[1], STDIN_FILENO);
	}
	if (error == 0) {
		error = posix_spawn_file_actions_adddup2(&actions, logs[1], STDOUT_FILENO);
	}
	if (error == 0) {
		error = posix_spawn_file_actions_adddup2(&actions, logs[1], STDERR_FILENO);
	}
	if (error != 0) {
		goto finish;
	}
	@synchronized(self) {
		_socket = connection[0];
		error = _cancelled ? ECANCELED
				   : posix_spawn(&_pid, arguments[0], &actions, &attributes,
					 arguments, environment);
	}
	if (error != 0) {
		goto finish;
	}
	close(connection[1]);
	connection[1] = -1;
	close(logs[1]);
	logs[1] = -1;
	while (!exited || !logEOF) {
		@synchronized(self) {
			if (_cancelled && error == 0) {
				error = ECANCELED;
			}
		}
		if (!exited) {
			exited = [self reapWithStatus:&status error:&waitError];
			if (waitError != 0 && error == 0) {
				error = waitError;
			}
		}
		if (error == 0 && !exited && ext4_check_deadline(0) >= idle) {
			error = ETIMEDOUT;
		}
		if (error != 0 && !exited) {
			@synchronized(self) {
				if (stopping == 0) {
					stopping = ext4_check_deadline(Ext4ResourceCancelSeconds);
					(void)shutdown(_socket, SHUT_RDWR);
					(void)kill(_pid, SIGTERM);
				} else if (ext4_check_deadline(0) >= stopping) {
					(void)kill(_pid, SIGKILL);
				}
			}
		}
		descriptors[0] =
		    (struct pollfd){ .fd = exited || socketEOF || error != 0 ? -1 : _socket,
			    .events = POLLIN };
		descriptors[1] = (struct pollfd){ .fd = logEOF ? -1 : logs[0], .events = POLLIN };
		if (poll(descriptors, 2, Ext4ResourcePollMilliseconds) < 0) {
			if (errno != EINTR && error == 0) {
				error = errno;
			}
			continue;
		}
		if (descriptors[1].revents & (POLLIN | POLLHUP)) {
			count = read(logs[0], logBytes, sizeof(logBytes));
			if (count > 0) {
				ext4_resource_log(task, pending, logBytes, (size_t)count, NO);
				idle = ext4_check_deadline(Ext4ResourceIdleSeconds);
			} else if (count == 0 || (count < 0 && errno != EINTR)) {
				logEOF = YES;
				if (count < 0 && error == 0) {
					error = errno;
				}
			}
		}
		if (descriptors[0].revents & (POLLIN | POLLHUP)) {
			count = recv(_socket, logBytes, 1, MSG_PEEK | MSG_DONTWAIT);
			if (count == 0) {
				socketEOF = YES;
			} else if (count > 0) {
				error = [self serveWithBuffer:buffer channels:&channels];
				idle = ext4_check_deadline(Ext4ResourceIdleSeconds);
			} else if (errno != EAGAIN && errno != EINTR) {
				error = errno;
			}
		}
	}
	ext4_resource_log(task, pending, NULL, 0, YES);
	if (error == 0 && _ioError != 0) {
		error = _ioError;
	}
	if (error == 0 &&
	    (channels != 0 || recv(_socket, logBytes, 1, MSG_PEEK | MSG_DONTWAIT) > 0)) {
		/* A child cannot return success while abandoning channels or queued
		 * requests. Do not execute residual writes after the child exits. */
		error = EPROTO;
	}
	if (error == 0) {
		if (!WIFEXITED(status)) {
			error = EIO;
		} else if (WEXITSTATUS(status) == Ext4ResourceCanceled) {
			error = ECANCELED;
		} else if (![_successfulExitStatuses
			       containsIndex:(NSUInteger)WEXITSTATUS(status)]) {
			error = EIO;
			[task logMessage:[NSString stringWithFormat:
						 @"The maintenance tool returned status %d.",
					     WEXITSTATUS(status)]];
		}
	}
finish:
	@synchronized(self) {
		_socket = -1;
		if (_cancelled) {
			error = ECANCELED;
		}
	}
	if (actionsReady) {
		posix_spawn_file_actions_destroy(&actions);
	}
	if (attributesReady) {
		posix_spawnattr_destroy(&attributes);
	}
	if (connection[0] >= 0) {
		close(connection[0]);
	}
	if (connection[1] >= 0) {
		close(connection[1]);
	}
	if (logs[0] >= 0) {
		close(logs[0]);
	}
	if (logs[1] >= 0) {
		close(logs[1]);
	}
	free(buffer);
	free(arguments);
	return error == 0 ? nil : ext4_resource_error(error);
}

@end

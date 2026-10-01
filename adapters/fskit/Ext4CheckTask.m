/* SPDX-License-Identifier: BSD-3-Clause */
#import "Ext4CheckTask.h"
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
	Ext4CheckIdleSeconds = 300,
	Ext4CheckCancelSeconds = 5,
	Ext4CheckPollMilliseconds = 250,
	Ext4CheckLogBytes = 4096,
	Ext4CheckMaxChannels = 32,
	Ext4CheckCorrected = 1,
	Ext4CheckCanceled = 32
};

@interface Ext4CheckTask () {
	Ext4ResourceIO *_resource;
	NSURL *_executable;
	Ext4CheckMode _mode;
	pid_t _pid;
	int _socket;
	BOOL _cancelled;
	BOOL _started;
	int _ioError;
}

@end

static NSError *
ext4_check_error(int code)
{
	return [NSError errorWithDomain:NSPOSIXErrorDomain code:code userInfo:nil];
}

static void
ext4_check_log(
    FSTask *task, NSMutableData *pending, const void *bytes, size_t length, BOOL finished)
{
	const uint8_t *cursor = bytes;
	NSString *message;
	size_t index;
	uint8_t byte;

	for (index = 0; index < length; index++) {
		byte = cursor[index];
		if (byte != '\n' && byte != '\r' && pending.length < Ext4CheckLogBytes) {
			[pending appendBytes:&byte length:1];
		}
		if (byte == '\n' || byte == '\r' || pending.length == Ext4CheckLogBytes) {
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
		ext4_check_log(task, pending, "\n", 1, NO);
	}
}

@implementation Ext4CheckTask

- (instancetype)initWithResource:(Ext4ResourceIO *)resource
			    mode:(Ext4CheckMode)mode
		      executable:(NSURL *)executable
{
	if (resource == nil || !executable.isFileURL || mode > Ext4CheckPreen ||
	    resource.blockSize == 0 || resource.blockSize > UINT32_MAX ||
	    (resource.blockSize & (resource.blockSize - 1)) != 0) {
		return nil;
	}
	self = [super init];
	if (self != nil) {
		_resource = resource;
		_mode = mode;
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
		if (offset != 0 || length != 0 || *channels == Ext4CheckMaxChannels) {
			return EPROTO;
		}
		if (flags != 0 && (_mode == Ext4CheckVerify || !_resource.writable)) {
			error = EROFS;
		} else {
			(*channels)++;
			response.offset = ext4_check_encode64(_resource.sizeBytes);
			response.length = ext4_check_encode32((uint32_t)_resource.blockSize);
			response.flags = ext4_check_encode32(
			    _mode != Ext4CheckVerify && _resource.writable ? EXT4_CHECK_WRITABLE
									   : 0);
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
			result = _mode == Ext4CheckVerify
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
			result =
			    _mode == Ext4CheckVerify ? EXT4_READ_ONLY : [_resource synchronize];
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
	char *mode =
	    _mode == Ext4CheckVerify ? "verify" : (_mode == Ext4CheckRepair ? "repair" : "preen");
	char *arguments[] = { (char *)_executable.fileSystemRepresentation, mode, NULL };
	char *environment[] = { "PATH=/usr/bin:/bin", "LC_ALL=C", NULL };
	uint8_t logBytes[Ext4CheckLogBytes];
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
	int64_t idle = ext4_check_deadline(Ext4CheckIdleSeconds);
	int64_t stopping = 0;

	@synchronized(self) {
		if (_started || _cancelled) {
			return ext4_check_error(_cancelled ? ECANCELED : EBUSY);
		}
		_started = YES;
	}
	if (_resource.isRevoked) {
		return ext4_check_error(EIO);
	}
	if (_mode != Ext4CheckVerify && !_resource.writable) {
		return ext4_check_error(EROFS);
	}
	buffer = malloc(EXT4_CHECK_MAX_TRANSFER);
	if (buffer == NULL) {
		return ext4_check_error(ENOMEM);
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
					stopping = ext4_check_deadline(Ext4CheckCancelSeconds);
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
		if (poll(descriptors, 2, Ext4CheckPollMilliseconds) < 0) {
			if (errno != EINTR && error == 0) {
				error = errno;
			}
			continue;
		}
		if (descriptors[1].revents & (POLLIN | POLLHUP)) {
			count = read(logs[0], logBytes, sizeof(logBytes));
			if (count > 0) {
				ext4_check_log(task, pending, logBytes, (size_t)count, NO);
				idle = ext4_check_deadline(Ext4CheckIdleSeconds);
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
				idle = ext4_check_deadline(Ext4CheckIdleSeconds);
			} else if (errno != EAGAIN && errno != EINTR) {
				error = errno;
			}
		}
	}
	ext4_check_log(task, pending, NULL, 0, YES);
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
		} else if (WEXITSTATUS(status) == Ext4CheckCanceled) {
			error = ECANCELED;
		} else if (WEXITSTATUS(status) != 0 &&
		    (WEXITSTATUS(status) != Ext4CheckCorrected || _mode == Ext4CheckVerify)) {
			error = EIO;
			[task logMessage:[NSString
					     stringWithFormat:@"The checker returned status %d.",
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
	return error == 0 ? nil : ext4_check_error(error);
}

@end

/* SPDX-License-Identifier: BSD-3-Clause */
#include "Ext4CheckProtocol.h"
#include <errno.h>
#include <limits.h>
#include <poll.h>
#include <sys/socket.h>
#include <time.h>

static int64_t
check_time(void)
{
	struct timespec now;

	if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
		return -1;
	}
	return (int64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000;
}

int64_t
ext4_check_deadline(unsigned seconds)
{
	int64_t now = check_time();

	return now < 0 ? -1 : now + (int64_t)seconds * 1000;
}

static int
check_wait(int socket, short events, int64_t deadline)
{
	struct pollfd descriptor = { socket, events, 0 };
	int64_t remaining;
	int result;

	for (;;) {
		remaining = check_time();
		if (remaining < 0) {
			return errno;
		}
		remaining = deadline - remaining;
		if (remaining <= 0) {
			return ETIMEDOUT;
		}
		result = poll(&descriptor, 1, remaining > INT_MAX ? INT_MAX : (int)remaining);
		if (result < 0) {
			if (errno == EINTR) {
				continue;
			}
			return errno;
		}
		if (result == 0) {
			continue;
		}
		if (descriptor.revents & POLLNVAL) {
			return EBADF;
		}
		if (descriptor.revents & (events | POLLHUP | POLLERR)) {
			return 0;
		}
	}
}

static int
check_transfer(int socket, void *bytes, size_t length, int64_t deadline, int sending)
{
	uint8_t *cursor = bytes;
	size_t completed = 0;
	ssize_t amount;
	int error;

	while (completed < length) {
		error = check_wait(socket, sending ? POLLOUT : POLLIN, deadline);
		if (error != 0) {
			return error;
		}
		amount = sending
		    ? send(socket, cursor + completed, length - completed,
			  MSG_NOSIGNAL | MSG_DONTWAIT)
		    : recv(socket, cursor + completed, length - completed, MSG_DONTWAIT);
		if (amount < 0) {
			if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) {
				continue;
			}
			return errno;
		}
		if (amount == 0) {
			return EPIPE;
		}
		completed += (size_t)amount;
	}
	return 0;
}

int
ext4_check_receive(int socket, void *bytes, size_t length, int64_t deadline)
{
	return check_transfer(socket, bytes, length, deadline, 0);
}

int
ext4_check_send(int socket, const void *bytes, size_t length, int64_t deadline)
{
	return check_transfer(socket, (void *)bytes, length, deadline, 1);
}

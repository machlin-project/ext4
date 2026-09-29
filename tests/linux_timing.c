/* SPDX-License-Identifier: BSD-3-Clause */
#define _GNU_SOURCE
#include <errno.h>
#include <inttypes.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/auxv.h>
#include <sys/mount.h>
#include <sys/reboot.h>
#include <sys/utsname.h>
#include <time.h>
#include <unistd.h>

/* Diskless PID 1 probe for qualifying the Linux reference VM's clocks. */
static volatile uint64_t work_result;

static uint64_t
clock_ns(clockid_t clock)
{
	struct timespec value;

	if (clock_gettime(clock, &value) != 0) {
		perror("clock_gettime");
		abort();
	}
	return (uint64_t)value.tv_sec * UINT64_C(1000000000) + (uint64_t)value.tv_nsec;
}

static void
clocksource(void)
{
	FILE *file;
	char name[128];

	file = fopen("/sys/devices/system/clocksource/clocksource0/current_clocksource", "r");
	if (file == NULL || fgets(name, sizeof(name), file) == NULL) {
		perror("clocksource");
		abort();
	}
	printf("CLOCKSOURCE=%s", name);
	fclose(file);
}

static void
sleep_sample(unsigned int milliseconds)
{
	struct timespec delay;
	uint64_t mono = clock_ns(CLOCK_MONOTONIC);
	uint64_t raw = clock_ns(CLOCK_MONOTONIC_RAW);

	delay.tv_sec = milliseconds / 1000U;
	delay.tv_nsec = (long)(milliseconds % 1000U) * 1000000L;
	while (nanosleep(&delay, &delay) != 0) {
		if (errno != EINTR) {
			perror("nanosleep");
			abort();
		}
	}
	printf("SLEEP requested_ms=%u mono_ns=%" PRIu64 " raw_ns=%" PRIu64 "\n", milliseconds,
	    clock_ns(CLOCK_MONOTONIC) - mono, clock_ns(CLOCK_MONOTONIC_RAW) - raw);
}

static void
cpu_sample(uint64_t iterations)
{
	uint64_t state = UINT64_C(0x123456789abcdef);
	uint64_t mono = clock_ns(CLOCK_MONOTONIC);
	uint64_t raw = clock_ns(CLOCK_MONOTONIC_RAW);
	uint64_t cpu = clock_ns(CLOCK_PROCESS_CPUTIME_ID);
	uint64_t index;

	for (index = 0; index < iterations; index++) {
		state ^= state << 13;
		state ^= state >> 7;
		state ^= state << 17;
	}
	work_result = state;
	printf("CPU iterations=%" PRIu64 " mono_ns=%" PRIu64 " raw_ns=%" PRIu64 " cpu_ns=%" PRIu64
	       " result=%016" PRIx64 "\n",
	    iterations, clock_ns(CLOCK_MONOTONIC) - mono, clock_ns(CLOCK_MONOTONIC_RAW) - raw,
	    clock_ns(CLOCK_PROCESS_CPUTIME_ID) - cpu, state);
}

int
main(void)
{
	static const clockid_t clocks[] = { CLOCK_MONOTONIC, CLOCK_MONOTONIC_RAW, CLOCK_BOOTTIME,
		CLOCK_PROCESS_CPUTIME_ID };
	struct utsname identity;
	struct timespec resolution;
	cpu_set_t cpus;
	uint64_t begin;
	uint64_t raw_begin;
	unsigned int index;
	unsigned int repeat;

	if (getpid() != 1) {
		fprintf(stderr, "Run only as PID 1 in the dedicated disposable Linux guest\n");
		return 2;
	}
	setvbuf(stdout, NULL, _IONBF, 0);
	if (mount("proc", "/proc", "proc", 0, NULL) != 0 ||
	    mount("sysfs", "/sys", "sysfs", 0, NULL) != 0 || uname(&identity) != 0) {
		perror("guest initialization");
		abort();
	}
	printf("GUEST=%s %s %s HWCAP=%lx\n", identity.sysname, identity.release, identity.machine,
	    getauxval(AT_HWCAP));
	clocksource();
	CPU_ZERO(&cpus);
	CPU_SET(0, &cpus);
	if (sched_setaffinity(0, sizeof(cpus), &cpus) != 0) {
		perror("sched_setaffinity");
		abort();
	}
	for (index = 0; index < sizeof(clocks) / sizeof(clocks[0]); index++) {
		if (clock_getres(clocks[index], &resolution) != 0) {
			perror("clock_getres");
			abort();
		}
		printf("RESOLUTION clock=%d seconds=%ld ns=%ld\n", clocks[index],
		    (long)resolution.tv_sec, resolution.tv_nsec);
	}
	puts("TIMING_BEGIN");
	begin = clock_ns(CLOCK_MONOTONIC);
	raw_begin = clock_ns(CLOCK_MONOTONIC_RAW);
	sleep_sample(50);
	sleep_sample(250);
	sleep_sample(1000);
	for (repeat = 0; repeat < 3U; repeat++) {
		for (index = 1; index <= 4U; index *= 2U) {
			cpu_sample(UINT64_C(10000000) * index);
		}
	}
	printf("TIMING_END mono_ns=%" PRIu64 " raw_ns=%" PRIu64 "\n",
	    clock_ns(CLOCK_MONOTONIC) - begin, clock_ns(CLOCK_MONOTONIC_RAW) - raw_begin);
	puts("LINUX_TIMING_RESULT=PASS");
	reboot(RB_POWER_OFF);
	return 1;
}

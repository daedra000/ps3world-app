/* host shim for tools/https_test.c: lv2's clock and random numbers on Linux */
#ifndef SHIM_LV2_SYSTIME_H
#define SHIM_LV2_SYSTIME_H
#include <stdint.h>
#include <stdio.h>
#include <time.h>

static inline uint64_t sysGetSystemTime(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

static inline int sysGetRandomNumber(void *buf, size_t n)
{
	FILE *f = fopen("/dev/urandom", "rb");
	int ok = f && fread(buf, 1, n, f) == n;
	if (f) fclose(f);
	return ok ? 0 : -1;
}
#endif

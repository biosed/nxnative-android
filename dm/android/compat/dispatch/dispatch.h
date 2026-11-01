// libdispatch subset on Linux (see ../README.md): semaphores and time only; queues and sources (blocks) have Android code at their callers
#pragma once
#include <stdint.h>
typedef struct dm_dispatch_sem *dispatch_semaphore_t;
typedef uint64_t dispatch_time_t;
#define DISPATCH_TIME_NOW 0ull
#define DISPATCH_TIME_FOREVER (~0ull)
#define NSEC_PER_SEC 1000000000ull
#define NSEC_PER_MSEC 1000000ull
#define NSEC_PER_USEC 1000ull
dispatch_time_t dispatch_time(dispatch_time_t when, int64_t delta);
dispatch_semaphore_t dispatch_semaphore_create(long value);
long dispatch_semaphore_wait(dispatch_semaphore_t s, dispatch_time_t timeout);
long dispatch_semaphore_signal(dispatch_semaphore_t s);
void dispatch_release(void *obj);

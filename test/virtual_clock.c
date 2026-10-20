/* virtual_clock.c - test-only deterministic clock for paced transport tests.
 *
 * Link with -Wl,--wrap=clock_gettime,--wrap=nanosleep. Time starts at one
 * second and advances by exactly the requested sleep. A test therefore keeps
 * the pacing logic under test without waiting in real time.
 *
 * This file is not part of the driver library. It would report physical
 * timing if it were used for anything else.
 */
#include <stdint.h>
#include <time.h>

static uint64_t virtual_ns = 1000000000u;

int __wrap_clock_gettime(clockid_t clock_id, struct timespec *out)
{
    (void)clock_id;
    out->tv_sec = (time_t)(virtual_ns / 1000000000u);
    out->tv_nsec = (long)(virtual_ns % 1000000000u);
    return 0;
}

int __wrap_nanosleep(const struct timespec *request, struct timespec *remaining)
{
    (void)remaining;
    virtual_ns += (uint64_t)request->tv_sec * 1000000000u +
                  (uint64_t)request->tv_nsec;
    return 0;
}

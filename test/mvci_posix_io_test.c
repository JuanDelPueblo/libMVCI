/* Public POSIX transport contracts through fully scripted syscalls.
 * This translation unit includes the shipped backend solely to construct its
 * opaque handle. Every descriptor operation is wrapped; no device is opened. */
#include "../src/io.c"
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>

static uint64_t clock_ns;
static int mode, polls, writes, reads, closes, setup_fail;
static short events;
enum { READY, INTERRUPT, TIMEOUT, HANGUP, ZERO, PARTIAL, WOULD_BLOCK };
int __wrap_clock_gettime(clockid_t id, struct timespec *t)
{
    (void)id; t->tv_sec = clock_ns / 1000000000; t->tv_nsec = clock_ns % 1000000000; return 0;
}
int __wrap_nanosleep(const struct timespec *t, struct timespec *rest)
{
    (void)rest; clock_ns += (uint64_t)t->tv_sec * 1000000000 + t->tv_nsec; return 0;
}
int __wrap_open(const char *path, int flags, ...)
{
    assert(strcmp(path, "script-only") == 0);
    assert(flags & O_NONBLOCK);
    return 123;
}
int __wrap_close(int fd) { assert(fd == 123); closes++; return 0; }
int __wrap_tcsetattr(int fd, int action, const struct termios *t)
{
    assert(fd == 123 && action == TCSANOW && t->c_cc[VMIN] == 1);
    return setup_fail ? -1 : 0;
}
int __wrap_tcflush(int fd, int action) { assert(fd == 123); (void)action; return 0; }
int __wrap_ioctl(int fd, unsigned long op, ...)
{
    assert(fd == 123);
    va_list args; va_start(args, op); int *bits = va_arg(args, int *); va_end(args);
    if (op == TIOCMGET) *bits = 0;
    else assert(op == TIOCMSET);
    return 0;
}
int __wrap_poll(struct pollfd *fds, nfds_t count, int timeout)
{
    assert(count == 1 && fds[0].fd == 123 && timeout >= 0);
    events = fds[0].events;
    polls++;
    if (mode == INTERRUPT && polls == 1) {
        clock_ns += 3000000; errno = EINTR; return -1;
    }
    if (mode == TIMEOUT || mode == WOULD_BLOCK || (mode == PARTIAL && polls > 1)) {
        clock_ns += (uint64_t)timeout * 1000000;
        return 0;
    }
    fds[0].revents = mode == HANGUP ? POLLHUP : events;
    return 1;
}
ssize_t __wrap_write(int fd, const void *buf, size_t n)
{
    assert(fd == 123 && buf && n && events == POLLOUT);
    writes++;
    if (mode == ZERO) return 0;
    if (mode == PARTIAL) return 2;
    return n;
}
ssize_t __wrap_read(int fd, void *buf, size_t n)
{
    assert(fd == 123 && buf && n && events == POLLIN);
    reads++;
    if (mode == ZERO) return 0;
    if (mode == PARTIAL) n = 2;
    memset(buf, 0x48, n); return n;
}
static void reset(int next)
{
    mode = next; polls = reads = writes = 0; clock_ns = 0;
}
int main(void)
{
    struct mvci_io io = {123}; uint8_t bytes[8] = {0};
    reset(READY);
    assert(mvci_io_write_timeout(&io, bytes, 8, 0) == 8 && polls == 1 && clock_ns == 0);
    assert(mvci_io_read(&io, bytes, 8, 0) == 8 && clock_ns == 0);
    reset(TIMEOUT);
    assert(mvci_io_write_timeout(&io, bytes, 8, 11) == 0 && clock_ns == 11000000 && writes == 0);
    reset(INTERRUPT);
    assert(mvci_io_read(&io, bytes, 8, 11) == 8 && polls == 2 && clock_ns == 3000000);
    reset(PARTIAL);
    assert(mvci_io_write_timeout(&io, bytes, 8, 11) == 2 && clock_ns == 11000000);
    reset(PARTIAL);
    assert(mvci_io_read(&io, bytes, 8, 11) == 2 && clock_ns == 11000000);
    reset(ZERO);
    assert(mvci_io_write_timeout(&io, bytes, 8, 11) == -1 && writes == 1);
    assert(mvci_io_read(&io, bytes, 8, 11) == -1 && reads == 1);
    reset(HANGUP);
    assert(mvci_io_read(&io, bytes, 8, 11) == -1 && reads == 0);
    assert(mvci_io_write_timeout(&io, bytes, 8, 11) == -1 && writes == 0);
    reset(TIMEOUT);
    assert(mvci_io_read(&io, bytes, 8, 0) == 0 && clock_ns == 0);
    setup_fail = 1;
    assert(mvci_io_open("script-only") == NULL && closes == 1);
    setup_fail = 0;
    mvci_io_t *opened = mvci_io_open("script-only");
    assert(opened); mvci_io_close(opened); assert(closes == 2);
    puts("POSIX scripted I/O: deadline, zero timeout, EINTR, partial I/O, EOF, zero write, HUP and setup errors passed; skips=0");
    return 0;
}

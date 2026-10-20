/* mvci_io.c — transport: termios (Linux/macOS) or FTDI D2XX (Windows). */

/* termios cfmakeraw + POSIX clock/nanosleep — defined here so the transport
 * compiles standalone (no reliance on build-system -D flags), no warnings. */
#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE 1
#endif
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
/* On macOS, cfmakeraw()/cfsetspeed() and the TIOCM* ioctls are only exposed
 * under the Darwin extensions (otherwise hidden by _POSIX_C_SOURCE above). */
#ifndef _DARWIN_C_SOURCE
#define _DARWIN_C_SOURCE 1
#endif

#include "io.h"
#include "compat.h"

#include <stdlib.h>
#include <string.h>

/* ====================================================================== */
#ifdef _WIN32
/* -------------------- Windows: FTDI D2XX (loaded at runtime) ----------
 * ftd2xx.dll is resolved with LoadLibrary/GetProcAddress, so the build needs
 * neither ftd2xx.h nor ftd2xx.lib — only the FTDI driver runtime (which ships
 * ftd2xx.dll) on the target machine. We declare the minimal D2XX surface here.
 */
typedef PVOID  FT_HANDLE;
typedef ULONG  FT_STATUS;
#define FT_OK                   0
#define FT_OPEN_BY_DESCRIPTION  2
#define FT_PURGE_RX             1
#define FT_BITS_8               8
#define FT_STOP_BITS_1          0
#define FT_PARITY_NONE          0
#define FT_FLOW_NONE            0

typedef FT_STATUS (WINAPI *PFN_OpenEx)(PVOID, DWORD, FT_HANDLE *);
typedef FT_STATUS (WINAPI *PFN_H)(FT_HANDLE);
typedef FT_STATUS (WINAPI *PFN_HU)(FT_HANDLE, ULONG);
typedef FT_STATUS (WINAPI *PFN_HUU)(FT_HANDLE, ULONG, ULONG);
typedef FT_STATUS (WINAPI *PFN_Chars)(FT_HANDLE, UCHAR, UCHAR, UCHAR);
typedef FT_STATUS (WINAPI *PFN_Flow)(FT_HANDLE, USHORT, UCHAR, UCHAR);
typedef FT_STATUS (WINAPI *PFN_Lat)(FT_HANDLE, UCHAR);
typedef FT_STATUS (WINAPI *PFN_RW)(FT_HANDLE, LPVOID, DWORD, LPDWORD);
typedef FT_STATUS (WINAPI *PFN_Q)(FT_HANDLE, LPDWORD);

static struct {
    HMODULE  dll;
    PFN_OpenEx OpenEx;
    PFN_H    Close, ResetDevice, ClrRts, SetDtr, ClrDtr;
    PFN_HU   SetBaudRate, Purge;
    PFN_HUU  SetTimeouts;
    PFN_Chars SetDataCharacteristics;
    PFN_Flow SetFlowControl;
    PFN_Lat  SetLatencyTimer;
    PFN_RW   Write, Read;
    PFN_Q    GetQueueStatus;
} FT;

static int ft_load(void)
{
    if (FT.dll) return 0;
    HMODULE m = LoadLibraryA("ftd2xx.dll");
    if (!m) return -1;

    FT.OpenEx                 = (PFN_OpenEx)GetProcAddress(m, "FT_OpenEx");
    FT.Close                  = (PFN_H)    GetProcAddress(m, "FT_Close");
    FT.ResetDevice            = (PFN_H)    GetProcAddress(m, "FT_ResetDevice");
    FT.ClrRts                 = (PFN_H)    GetProcAddress(m, "FT_ClrRts");
    FT.SetDtr                 = (PFN_H)    GetProcAddress(m, "FT_SetDtr");
    FT.ClrDtr                 = (PFN_H)    GetProcAddress(m, "FT_ClrDtr");
    FT.SetBaudRate            = (PFN_HU)   GetProcAddress(m, "FT_SetBaudRate");
    FT.Purge                  = (PFN_HU)   GetProcAddress(m, "FT_Purge");
    FT.SetTimeouts            = (PFN_HUU)  GetProcAddress(m, "FT_SetTimeouts");
    FT.SetDataCharacteristics = (PFN_Chars)GetProcAddress(m, "FT_SetDataCharacteristics");
    FT.SetFlowControl         = (PFN_Flow) GetProcAddress(m, "FT_SetFlowControl");
    FT.SetLatencyTimer        = (PFN_Lat)  GetProcAddress(m, "FT_SetLatencyTimer");
    FT.Write                  = (PFN_RW)   GetProcAddress(m, "FT_Write");
    FT.Read                   = (PFN_RW)   GetProcAddress(m, "FT_Read");
    FT.GetQueueStatus         = (PFN_Q)    GetProcAddress(m, "FT_GetQueueStatus");

    if (!FT.OpenEx || !FT.Close || !FT.ResetDevice || !FT.ClrRts || !FT.SetDtr ||
        !FT.ClrDtr || !FT.SetBaudRate || !FT.Purge || !FT.SetTimeouts ||
        !FT.SetDataCharacteristics || !FT.SetFlowControl || !FT.SetLatencyTimer ||
        !FT.Write || !FT.Read || !FT.GetQueueStatus) {
        FreeLibrary(m);
        return -1;
    }
    FT.dll = m;
    return 0;
}

/* purge_failed: mvci_io_purge_rx() returns void, so a failed purge is kept here
 * and fails reads and writes until a later purge succeeds, instead of leaving
 * stale bytes to be framed. */
struct mvci_io { FT_HANDLE h; int purge_failed; ULONG rd_ms, wr_ms; };

#define FT_TRY(call)  do { if ((call) != FT_OK) goto fail; } while (0)

mvci_io_t *mvci_io_open(const char *port)
{
    const char *desc = (port && *port) ? port : "M-VCI";
    if (ft_load() != 0) return NULL;

    mvci_io_t *io = (mvci_io_t *)calloc(1, sizeof(*io));
    if (!io) return NULL;

    if (FT.OpenEx((PVOID)desc, FT_OPEN_BY_DESCRIPTION, &io->h) != FT_OK) {
        free(io);
        return NULL;
    }

    FT_TRY(FT.SetBaudRate(io->h, 115200));
    FT_TRY(FT.SetDataCharacteristics(io->h, FT_BITS_8, FT_STOP_BITS_1, FT_PARITY_NONE));
    FT_TRY(FT.SetFlowControl(io->h, FT_FLOW_NONE, 0, 0));
    FT_TRY(FT.SetLatencyTimer(io->h, 2));
    FT_TRY(FT.SetTimeouts(io->h, 100, 1000));   /* default rx/tx ms */
    io->rd_ms = 100;
    io->wr_ms = 1000;

    FT_TRY(FT.ResetDevice(io->h));
    FT_TRY(FT.Purge(io->h, FT_PURGE_RX));
    FT_TRY(FT.ResetDevice(io->h));

    /* DTR pulse = MCU reset (FT_ClrRts, FT_SetDtr, 15 ms, FT_ClrDtr) */
    FT_TRY(FT.ClrRts(io->h));
    FT_TRY(FT.SetDtr(io->h));
    mvci_sleep_ms(15);
    FT_TRY(FT.ClrDtr(io->h));

    mvci_sleep_ms(1000);                        /* MCU boot */
    FT_TRY(FT.Purge(io->h, FT_PURGE_RX));
    return io;

fail:
    FT.Close(io->h);
    free(io);
    return NULL;
}

void mvci_io_close(mvci_io_t *io)
{
    if (!io) return;
    if (io->h) FT.Close(io->h);
    free(io);
}

/* D2XX documents no value for a timeout of 0 (the D2XX Programmer's Guide, v1.5,
 * describes FT_SetTimeouts only as "in milliseconds"), so 0 is never passed.
 * The guide's FT_Write section says only FT_OK or an error code. A short count
 * on a write timeout is documented for FT_W32_WriteFile (FT_W32_SetCommTimeouts)
 * and is INFERRED here for FT_Write: UNPROVEN until a real D2XX run shows it.
 * Either way is safe: a short count is returned as the partial accepted count,
 * and a failed status is -1 even when bytes were accepted, which the caller must
 * treat as ambiguous. The count is host acceptance, not wire transmission.
 * This function never retries. */
int mvci_io_write_timeout(mvci_io_t *io, const uint8_t *buf, int n, int timeout_ms)
{
    DWORD written = 0;
    if (!io || n < 0 || (n > 0 && !buf) || timeout_ms < 0 || io->purge_failed) return -1;
    if (n == 0) return 0;
    /* No documented way to offer bytes without a wait: accept none. */
    if (timeout_ms == 0) return 0;
    if (FT.SetTimeouts(io->h, io->rd_ms, (ULONG)timeout_ms) != FT_OK) return -1;
    io->wr_ms = (ULONG)timeout_ms;
    if (FT.Write(io->h, (LPVOID)buf, (DWORD)n, &written) != FT_OK) return -1;
    return written <= (DWORD)n ? (int)written : -1;
}

int mvci_io_write(mvci_io_t *io, const uint8_t *buf, int n)
{
    int written = mvci_io_write_timeout(io, buf, n, 1000);
    return written == n ? n : -1;
}

/* timeout_ms == 0 drains only what the receive queue already holds: the guide
 * documents that FT_Read returns at once when asked for no more than
 * FT_GetQueueStatus reports, so no D2XX timeout is involved. */
int mvci_io_read(mvci_io_t *io, uint8_t *buf, int n, int timeout_ms)
{
    DWORD got = 0;
    if (!io || n < 0 || (n > 0 && !buf) || timeout_ms < 0 || io->purge_failed) return -1;
    if (n == 0) return 0;
    if (timeout_ms == 0) {
        DWORD queued = 0;
        if (FT.GetQueueStatus(io->h, &queued) != FT_OK) return -1;
        if (queued == 0) return 0;
        if (queued > (DWORD)n) queued = (DWORD)n;
        if (FT.Read(io->h, buf, queued, &got) != FT_OK) return -1;
        return got <= queued ? (int)got : -1;
    }
    if (FT.SetTimeouts(io->h, (ULONG)timeout_ms, io->wr_ms) != FT_OK) return -1;
    io->rd_ms = (ULONG)timeout_ms;
    if (FT.Read(io->h, buf, (DWORD)n, &got) != FT_OK) return -1;
    return got <= (DWORD)n ? (int)got : -1;     /* got < n is a timeout */
}

void mvci_io_purge_rx(mvci_io_t *io)
{
    if (io) io->purge_failed = (FT.Purge(io->h, FT_PURGE_RX) != FT_OK);
}

/* ====================================================================== */
#else
/* ------------------------- Linux / macOS: termios -------------------- */
#include <errno.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>
#include <poll.h>

struct mvci_io { int fd; };

mvci_io_t *mvci_io_open(const char *port)
{
    mvci_io_t *io = (mvci_io_t *)calloc(1, sizeof(*io));
    if (!io) return NULL;

    io->fd = open(port, O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (io->fd < 0) { free(io); return NULL; }

    struct termios t;
    memset(&t, 0, sizeof t);
    cfmakeraw(&t);
    t.c_cflag = CS8 | CLOCAL | CREAD;
    if (cfsetispeed(&t, B115200) || cfsetospeed(&t, B115200)) goto fail;
    t.c_cc[VMIN] = 1;
    t.c_cc[VTIME] = 0;
    if (tcsetattr(io->fd, TCSANOW, &t) || tcflush(io->fd, TCIOFLUSH)) goto fail;

    /* DTR pulse = MCU reset (ClrRts, SetDtr, 15 ms, ClrDtr) */
    int s = 0;
    if (ioctl(io->fd, TIOCMGET, &s)) goto fail;
    s &= ~TIOCM_RTS; if (ioctl(io->fd, TIOCMSET, &s)) goto fail;
    s |=  TIOCM_DTR; if (ioctl(io->fd, TIOCMSET, &s)) goto fail;
    mvci_sleep_ms(15);
    s &= ~TIOCM_DTR; if (ioctl(io->fd, TIOCMSET, &s)) goto fail;

    mvci_sleep_ms(1000);                        /* MCU boot */
    if (tcflush(io->fd, TCIOFLUSH)) goto fail;
    return io;
fail:
    close(io->fd);
    free(io);
    return NULL;
}

void mvci_io_close(mvci_io_t *io)
{
    if (!io) return;
    if (io->fd >= 0) close(io->fd);
    free(io);
}

static int wait_ready(mvci_io_t *io, short events, uint32_t deadline)
{
    for (;;) {
        int remaining = (int32_t)(deadline - mvci_now_ms());
        struct pollfd fd = { io->fd, events, 0 };
        int r = poll(&fd, 1, remaining > 0 ? remaining : 0);
        if (r < 0 && errno == EINTR) {
            if ((int32_t)(deadline - mvci_now_ms()) <= 0) return 0;
            continue;
        }
        if (r <= 0) return r;
        if (fd.revents & (POLLERR | POLLHUP | POLLNVAL)) return -1;
        if (fd.revents & events) return 1;
        return -1;
    }
}

int mvci_io_write_timeout(mvci_io_t *io, const uint8_t *buf, int n, int timeout_ms)
{
    if (!io || !buf || n < 0 || timeout_ms < 0) return -1;
    uint32_t deadline = mvci_now_ms() + (uint32_t)timeout_ms;
    int off = 0;
    while (off < n) {
        int ready = wait_ready(io, POLLOUT, deadline);
        if (ready < 0) return -1;
        if (!ready || (timeout_ms && (int32_t)(deadline - mvci_now_ms()) <= 0)) break;
        int w = (int)write(io->fd, buf + off, (size_t)(n - off));
        if (w < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) {
            if ((int32_t)(deadline - mvci_now_ms()) <= 0) break;
            continue;
        }
        if (w <= 0) return -1; /* zero progress must not spin */
        off += w;
        if ((int32_t)(deadline - mvci_now_ms()) <= 0) break;
    }
    /* Do not tcdrain: it has no deadline and is not a physical ACK. */
    return off;
}

int mvci_io_write(mvci_io_t *io, const uint8_t *buf, int n)
{
    int written = mvci_io_write_timeout(io, buf, n, 1000);
    return written == n ? n : -1;
}

int mvci_io_read(mvci_io_t *io, uint8_t *buf, int n, int timeout_ms)
{
    if (!io || !buf || n < 0 || timeout_ms < 0) return -1;
    uint32_t deadline = mvci_now_ms() + (uint32_t)timeout_ms;
    int got = 0;
    while (got < n) {
        int ready = wait_ready(io, POLLIN, deadline);
        if (ready < 0) return -1;
        if (!ready || (timeout_ms && (int32_t)(deadline - mvci_now_ms()) <= 0)) break;
        int r = (int)read(io->fd, buf + got, (size_t)(n - got));
        if (r < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) {
            if ((int32_t)(deadline - mvci_now_ms()) <= 0) break;
            continue;
        }
        if (r <= 0) return -1; /* EOF or device loss, never an empty queue */
        got += r;
        if ((int32_t)(deadline - mvci_now_ms()) <= 0) break;
    }
    return got;
}

void mvci_io_purge_rx(mvci_io_t *io)
{
    tcflush(io->fd, TCIFLUSH);
}

#endif

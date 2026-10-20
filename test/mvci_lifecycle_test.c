/* Offline public-API lifecycle and concurrency regression for the J2534 provider.
 *
 * Build ( this file supplies its own mvci_io_* transport, virtual
 * clock and thread-safe device model; do not link test/mock_io.c or io.c, and
 * wrap mvci_set_read_cancel to observe the real teardown signal):
 *   cc -std=gnu11 -DMVCI_TESTING -Iinclude -Isrc test/mvci_lifecycle_test.c src/passthru.c \
 *      src/serial.c src/des.c -Wl,--wrap=mvci_set_read_cancel -lcrypto -lpthread
 *
 * Interleavings are forced with events, not with sleeps. A thread parks inside
 * the scripted transport (a write waiting for its ACK, or an empty read) and
 * the test releases it explicitly. The virtual clock is mutex-protected and
 * moves only when production code sleeps or a parked read is granted a slice.
 * The only timer is a watchdog that fails a hang; nothing paces on it.
 * Everything here is host behavior against a scripted device. It proves no
 * cable timing and no ECU behavior. */
#include <mvci/j2534.h>
#include <mvci/serial.h>
#include "io.h"
#include "compat.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifdef _WIN32

int main(void)
{
    puts("SKIP: POSIX-only test (interposed clock and signal watchdog); 1 skip");
    return 0;
}
#else
#include <signal.h>
#include <time.h>
#include <unistd.h>

void mvci_test_set_next_handle(uint32_t next);   /* passthru.c test seam */

/* ---- check bookkeeping --------------------------------------------- */
static int g_pass, g_fail;
#define CHECK(cond, name) do { \
    if (cond) g_pass++; else { g_fail++; printf("[FAIL] %s (line %d)\n", name, __LINE__); } \
} while (0)

/* ---- virtual clock (thread-safe, interposes the libc entry points) --- */
static pthread_mutex_t CK = PTHREAD_MUTEX_INITIALIZER;
static void sleep_event(void);
static uint64_t clock_ns = 1000000000ull;
static unsigned sleep_calls;
int clock_gettime(clockid_t id, struct timespec *t)
{
    (void)id;
    pthread_mutex_lock(&CK);
    t->tv_sec = (time_t)(clock_ns / 1000000000ull);
    t->tv_nsec = (long)(clock_ns % 1000000000ull);
    pthread_mutex_unlock(&CK);
    return 0;
}
int nanosleep(const struct timespec *t, struct timespec *rest)
{
    (void)rest;
    pthread_mutex_lock(&CK);
    clock_ns += (uint64_t)t->tv_sec * 1000000000ull + (uint64_t)t->tv_nsec;
    sleep_calls++;
    pthread_mutex_unlock(&CK);
    sleep_event();                      /* lets a test wait for "someone slept" */
    return 0;
}
static uint64_t vnow(void)
{
    pthread_mutex_lock(&CK);
    uint64_t v = clock_ns;
    pthread_mutex_unlock(&CK);
    return v;
}
static unsigned vsleeps(void)
{
    pthread_mutex_lock(&CK);
    unsigned v = sleep_calls;
    pthread_mutex_unlock(&CK);
    return v;
}

/* ---- watchdog: fails a hang, never paces the test --------------------- */
static void on_alarm(int sig)
{
    (void)sig;
    static const char m[] = "WATCHDOG: test hung\n";
    if (write(2, m, sizeof m - 1) < 0) { /* nothing to do */ }
    _exit(2);
}

/* ---- scripted device ------------------------------------------------- */
static const uint8_t KEY[8] = { 0xb0, 0xcb, 0x49, 0x68, 0x07, 0x45, 0xc8, 0x7f };
#define NDEV 16
#define RXQ 8192

struct mvci_io {
    int used, closed, inside;
    uint8_t rxq[RXQ];
    int qh, qt;
    uint8_t asmb[300];
    int asml;
    struct { uint8_t d[64]; int n; uint8_t st; } recs[16];
    int nrecs;
};

static mvci_mutex_t TM;      /* guards every field below and all device state */
static mvci_cond_t  TC;
static struct mvci_io devs[NDEV];
static struct {
    int opens, closes, close_violation, double_close;
    int op_count[256];
    int park_op, park_left, parked, release_park;
    int mute_op, reject_op;
    int gate_reads, reads_parked, slice_tokens;
    int gate_nonblocking, nonblocking_parked, release_nonblocking;
    int drip_positive, positive_reads, gate_positive, positive_parked, release_positive;
    int cancel_seen;
    int purge_fail;
    int go;                       /* start barrier */
    uint32_t msgids[64]; int nmsgids;
} X;

static void T_lock(void)   { mvci_mutex_lock(&TM); }
static void T_unlock(void) { mvci_mutex_unlock(&TM); }
#define T_WAIT(expr) while (!(expr)) mvci_cond_wait(&TC, &TM)
static void T_signal(void) { mvci_cond_broadcast(&TC); }

/* Observe the production cancellation signal from a real public Disconnect.
 * This wrapper never changes it or calls a test-only cancellation setter. */
void __real_mvci_set_read_cancel(mvci_ctx_t *, int);
void __wrap_mvci_set_read_cancel(mvci_ctx_t *ctx, int on)
{
    __real_mvci_set_read_cancel(ctx, on);
    T_lock();
    if (on) X.cancel_seen = 1;
    T_signal();
    T_unlock();
}

static int sleep_events;                       /* guarded by TM */

static void sleep_event(void)
{
    T_lock();
    sleep_events++;
    T_signal();
    T_unlock();
}

static void x_reset(void)
{
    T_lock();
    memset(&X, 0, sizeof X);
    X.park_op = X.mute_op = X.reject_op = -1;
    T_unlock();
}

static void q_bytes(struct mvci_io *d, const uint8_t *b, int n)
{
    if (d->qt + n > RXQ) return;
    memcpy(d->rxq + d->qt, b, (size_t)n);
    d->qt += n;
}
static void q_inner(struct mvci_io *d, const uint8_t *inner, size_t len)
{
    uint8_t f[MVCI_MAX_FRAME];
    int n = mvci_frame_enc(KEY, inner, len, f, sizeof f);
    if (n > 0) q_bytes(d, f, n);
}
static void q_status(struct mvci_io *d, uint8_t op, uint8_t st)
{
    uint8_t in[8] = { 2, 0, op, st, 0, 0, 0, 0 };
    q_inner(d, in, sizeof in);
}
static void q_record(struct mvci_io *d, const uint8_t *data, int len, uint8_t st)
{
    uint8_t in[80] = {0};
    in[0] = (uint8_t)(len + 9); in[2] = 9; in[3] = st;
    memcpy(in + 11, data, (size_t)len);
    q_inner(d, in, (size_t)len + 11);
}
static uint32_t get_u32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* caller holds TM */
static void answer(struct mvci_io *d, const uint8_t *in, int len)
{
    if (len < 4) return;
    uint8_t op = in[2];
    X.op_count[op]++;
    if (op == 0x0b && X.nmsgids < 64 && len >= 11) X.msgids[X.nmsgids++] = get_u32(in + 7);
    if ((int)op == X.park_op && X.park_left > 0) {
        X.park_left--;
        X.parked++;
        T_signal();
        T_WAIT(X.release_park);
        X.parked--;
        T_signal();
    }
    if ((int)op == X.mute_op) return;                    /* no reply at all */
    if (op == 0x02) {                                    /* plaintext Disconnect reply */
        static const uint8_t pl[4] = { 0x02, 0x00, 0x02, 0x00 };
        uint8_t f[MVCI_MAX_FRAME];
        int n = mvci_frame_plain(pl, sizeof pl, f, sizeof f);
        if (n > 0) q_bytes(d, f, n);
        d->nrecs = 0;
        return;
    }
    if (op == 0x09) {                                    /* read poll */
        if (d->nrecs) {
            q_record(d, d->recs[0].d, d->recs[0].n, d->recs[0].st);
            d->nrecs--;
            memmove(d->recs, d->recs + 1, (size_t)d->nrecs * sizeof d->recs[0]);
        } else q_status(d, 9, 0x10);                     /* captured empty poll */
        return;
    }
    q_status(d, op, (int)op == X.reject_op ? 8 : 0);
}

static void on_frame(struct mvci_io *d, const uint8_t *f, int n)
{
    if (n == 3 && f[0] == 0x03) return;                  /* reset */
    if (n == 12 && f[2] == 0x07 && f[3] == 0x00 && f[4] == 0x01) {
        uint8_t pl[11] = { 0x09, 0x00, 0x01 };
        memcpy(pl + 3, KEY, 8);
        uint8_t fr[MVCI_MAX_FRAME];
        int m = mvci_frame_plain(pl, sizeof pl, fr, sizeof fr);
        if (m > 0) q_bytes(d, fr, m);
        X.op_count[0xf0]++;                              /* identify */
        return;
    }
    if (n >= 3 && ((n - 3) % 8) == 0) {
        uint8_t tmp[300];
        memcpy(tmp, f + 2, (size_t)n - 3);
        mvci_des_decrypt(KEY, tmp, ((size_t)n - 3) / 8);
        answer(d, tmp, n - 3);
    }
}

mvci_io_t *mvci_io_open(const char *port)
{
    (void)port;
    T_lock();
    struct mvci_io *d = NULL;
    for (int i = 0; i < NDEV && !d; i++) if (!devs[i].used) d = &devs[i];
    if (d) { memset(d, 0, sizeof *d); d->used = 1; X.opens++; }
    T_unlock();
    return d;
}

void mvci_io_close(mvci_io_t *io)
{
    T_lock();
    if (io->closed) X.double_close++;
    else {
        io->closed = 1;
        X.closes++;
        if (io->inside) X.close_violation++;     /* freed under an active call */
    }
    io->used = 0;
    T_signal();
    T_unlock();
}

static int io_write_locked(struct mvci_io *d, const uint8_t *buf, int n)
{
    d->inside++;
    for (int i = 0; i < n; i++) {
        if (d->asml < (int)sizeof d->asmb) d->asmb[d->asml++] = buf[i];
        if (d->asml >= 1 && d->asmb[0] < 3) { d->asml = 0; continue; }
        if (d->asml >= 1 && d->asml >= d->asmb[0] && d->asmb[0] >= 3) {
            int len = d->asmb[0];
            d->asml = 0;
            uint8_t fr[300];
            memcpy(fr, d->asmb, (size_t)len);
            on_frame(d, fr, len);
        }
    }
    d->inside--;
    T_signal();
    return n;
}
int mvci_io_write(mvci_io_t *io, const uint8_t *buf, int n)
{
    if (!buf || n <= 0) return -1;
    T_lock();
    int r = io_write_locked(io, buf, n);
    T_unlock();
    return r;
}
int mvci_io_write_timeout(mvci_io_t *io, const uint8_t *buf, int n, int timeout_ms)
{
    (void)timeout_ms;
    return mvci_io_write(io, buf, n);
}

int mvci_io_read(mvci_io_t *io, uint8_t *buf, int n, int timeout_ms)
{
    if (!buf || n <= 0 || timeout_ms < 0) return -1;
    T_lock();
    io->inside++;
    int avail = io->qt - io->qh;
    if (!timeout_ms && avail <= 0 && X.gate_nonblocking) {
        X.gate_nonblocking = 0;
        X.nonblocking_parked = 1;
        T_signal();
        T_WAIT(X.release_nonblocking);
        X.nonblocking_parked = 0;
        avail = io->qt - io->qh;
    }
    if (avail <= 0 && timeout_ms > 0 && X.gate_reads) {
        /* Park as a blocked transport read until the test grants a slice. */
        X.reads_parked++;
        T_signal();
        T_WAIT(X.slice_tokens > 0);
        X.slice_tokens--;
        X.reads_parked--;
        pthread_mutex_lock(&CK);
        clock_ns += (uint64_t)timeout_ms * 1000000ull;
        pthread_mutex_unlock(&CK);
        T_signal();
        avail = io->qt - io->qh;
    }
    if (avail > 0) {
        if (X.drip_positive && timeout_ms > 0 && timeout_ms <= 20) {
            if (X.positive_reads == 1 && X.gate_positive) {
                X.positive_parked = 1;
                T_signal();
                T_WAIT(X.release_positive);
                X.positive_parked = 0;
            }
            X.positive_reads++;
            if (n > 1) n = 1;
        }
        if (n > avail) n = avail;
        memcpy(buf, io->rxq + io->qh, (size_t)n);
        io->qh += n;
        if (io->qh == io->qt) io->qh = io->qt = 0;
    } else n = 0;
    io->inside--;
    T_signal();
    T_unlock();
    return n;
}

int mvci_io_purge_rx_checked(mvci_io_t *io)
{
    T_lock();
    int r = X.purge_fail ? -1 : 0;
    if (!r) io->qh = io->qt = 0;
    T_unlock();
    return r;
}
void mvci_io_purge_rx(mvci_io_t *io) { (void)mvci_io_purge_rx_checked(io); }

/* ---- device-side helpers -------------------------------------------- */
static struct mvci_io *live_dev(void)
{
    for (int i = 0; i < NDEV; i++) if (devs[i].used) return &devs[i];
    return NULL;
}
static void dev_push_poll_record(struct mvci_io *d, const uint8_t *data, int len)
{
    T_lock();
    memcpy(d->recs[d->nrecs].d, data, (size_t)len);
    d->recs[d->nrecs].n = len;
    d->recs[d->nrecs].st = 0;
    d->nrecs++;
    T_unlock();
}
static void dev_push_unsolicited(struct mvci_io *d, const uint8_t *data, int len)
{
    T_lock();
    q_record(d, data, len, 0);
    T_unlock();
}
static void dev_push_raw(struct mvci_io *d, const uint8_t *b, int n)
{
    T_lock();
    q_bytes(d, b, n);
    T_unlock();
}
static int frames_op(int op)
{
    T_lock();
    int v = X.op_count[op];
    T_unlock();
    return v;
}

/* ---- jobs: one public call on its own thread ------------------------- */
enum { J_OPEN, J_CLOSE, J_CONNECT, J_DISCONNECT, J_READ, J_WRITE, J_CLEAR_PERIODIC,
       J_START_FILTER, J_SET_LOOPBACK };
typedef struct {
    int kind, done, started;
    unsigned long id, timeout, n;
    J2534_ULONG out;
    long rc;
    PASSTHRU_MSG msg[4];
    mvci_thread_t th;
} job_t;

static MVCI_THREAD_RET job_main(void *p)
{
    job_t *j = (job_t *)p;
    T_lock();
    T_WAIT(X.go || !j->started);          /* start barrier: all jobs begin together */
    T_unlock();
    switch (j->kind) {
    case J_OPEN:    j->rc = PassThruOpen((void *)"lifecycle", &j->out); break;
    case J_CLOSE:   j->rc = PassThruClose((J2534_ULONG)j->id); break;
    case J_CONNECT: j->rc = PassThruConnect((J2534_ULONG)j->id, ISO9141, 0x1200, 10400, &j->out); break;
    case J_DISCONNECT: j->rc = PassThruDisconnect((J2534_ULONG)j->id); break;
    case J_READ: {
        J2534_ULONG n = (J2534_ULONG)j->n;
        j->rc = PassThruReadMsgs((J2534_ULONG)j->id, j->msg, &n, (J2534_ULONG)j->timeout);
        j->n = n;
        break;
    }
    case J_WRITE: {
        J2534_ULONG n = (J2534_ULONG)j->n;
        j->rc = PassThruWriteMsgs((J2534_ULONG)j->id, j->msg, &n, (J2534_ULONG)j->timeout);
        j->n = n;
        break;
    }
    case J_CLEAR_PERIODIC: j->rc = PassThruIoctl((J2534_ULONG)j->id, CLEAR_PERIODIC_MSGS, NULL, NULL); break;
    case J_START_FILTER: {
        PASSTHRU_MSG mask, pat;
        memset(&mask, 0, sizeof mask); memset(&pat, 0, sizeof pat);
        mask.DataSize = pat.DataSize = 1;
        mask.Data[0] = 0xff; pat.Data[0] = (uint8_t)j->n;
        j->rc = PassThruStartMsgFilter((J2534_ULONG)j->id, PASS_FILTER, &mask, &pat, NULL, &j->out);
        break;
    }
    default: break;
    }
    T_lock();
    j->done = 1;
    T_signal();
    T_unlock();
    return 0;
}
/* Every input is fixed before the thread exists. */
static void job_startx(job_t *j, int kind, unsigned long id, unsigned long n, unsigned long timeout)
{
    memset(j, 0, sizeof *j);
    j->kind = kind; j->id = id; j->n = n; j->timeout = timeout;
    j->started = 1;
    for (int i = 0; i < 4; i++) {
        j->msg[i].ProtocolID = ISO9141;
        j->msg[i].DataSize = 5;
        memcpy(j->msg[i].Data, "\x82\x11\xf1\x01\x00", 5);
    }
    int rc = mvci_thread_create(&j->th, job_main, j);
    CHECK(rc == 0, "thread created");
}
static void job_start(job_t *j, int kind, unsigned long id) { job_startx(j, kind, id, 1, 100); }
static void release_start(void) { T_lock(); X.go = 1; T_signal(); T_unlock(); }
static void job_join(job_t *j) { mvci_thread_join(j->th); }
static int job_done(job_t *j) { T_lock(); int d = j->done; T_unlock(); return d; }
static void wait_parked(int n) { T_lock(); T_WAIT(X.parked >= n); T_unlock(); }
static void wait_reads_parked(int n) { T_lock(); T_WAIT(X.reads_parked >= n); T_unlock(); }
static void release_parks(void)
{
    T_lock();
    X.release_park = 1;
    T_signal();
    T_WAIT(X.parked == 0);
    X.release_park = 0;
    X.park_op = -1;
    T_unlock();
}
static void arm_park(int op, int count)
{
    T_lock(); X.park_op = op; X.park_left = count; X.release_park = 0; T_unlock();
}

/* Grant transport slices to parked reads until `j` finishes. Each grant is one
 * expired slice, so a read that no one cancels would keep going; a Close or
 * Disconnect that is waiting ends it at the next slice boundary. */
static void grant_slices_until_done(job_t *j)
{
    T_lock();
    while (!j->done) {
        X.slice_tokens++;
        T_signal();
        T_WAIT(X.slice_tokens == 0 || j->done);
    }
    X.slice_tokens = 0;
    T_unlock();
}

/* ---- scenario helpers ------------------------------------------------ */
static J2534_ULONG open_dev(void)
{
    J2534_ULONG d = 0;
    CHECK(PassThruOpen((void *)"lifecycle", &d) == 0 && d, "Open");
    return d;
}
static J2534_ULONG connect_dev(J2534_ULONG d)
{
    J2534_ULONG ch = 0;
    long rc = PassThruConnect(d, ISO9141, 0x1200, 10400, &ch);
    CHECK(rc == 0 && ch, "Connect");
    return ch;
}
static void close_dev(J2534_ULONG d) { CHECK(PassThruClose(d) == 0, "Close"); }

/* ====================================================================== */

static void test_open_close_races(void)
{
    x_reset();
    enum { N = 8 };
    job_t o[N];
    for (int i = 0; i < N; i++) job_start(&o[i], J_OPEN, 0);
    release_start();
    int ok = 0, full = 0;
    unsigned long ids[N];
    for (int i = 0; i < N; i++) {
        job_join(&o[i]);
        if (o[i].rc == 0) ids[ok++] = o[i].out;
        else if (o[i].rc == ERR_FAILED) full++;
    }
    CHECK(ok == 4 && full == 4, "racing Opens fill exactly four slots");
    int distinct = 1;
    for (int i = 0; i < ok; i++)
        for (int k = i + 1; k < ok; k++) if (ids[i] == ids[k]) distinct = 0;
    CHECK(distinct, "racing Opens return distinct device ids");
    CHECK(frames_op(0xf0) == 4, "each successful Open handshook exactly once");

    /* Six threads close one device: one winner, the rest see an invalid id. */
    x_reset();
    T_lock(); X.opens = 0; T_unlock();
    job_t c[6];
    for (int i = 0; i < 6; i++) job_start(&c[i], J_CLOSE, ids[0]);
    release_start();
    int winners = 0, invalid = 0;
    for (int i = 0; i < 6; i++) {
        job_join(&c[i]);
        if (c[i].rc == 0) winners++;
        else if (c[i].rc == ERR_INVALID_DEVICE_ID) invalid++;
    }
    T_lock(); int closes = X.closes, dbl = X.double_close, viol = X.close_violation; T_unlock();
    CHECK(winners == 1 && invalid == 5, "racing Closes on one device have exactly one winner");
    CHECK(closes == 1 && dbl == 0 && viol == 0, "context closed once, with no active call");
    for (int i = 1; i < ok; i++) close_dev((J2534_ULONG)ids[i]);
    T_lock(); closes = X.closes; dbl = X.double_close; T_unlock();
    CHECK(closes == 4 && dbl == 0, "every device closed exactly once");
    puts("done: open/close races");
}

/* A write is inside the transport and holds the I/O lock. A read waits for
 * that lock; it is the event that shows a queued Close or Disconnect, because
 * the read gives way (without sending anything) when one queues. */
static void busy_write_scenario(int use_disconnect)
{
    x_reset();
    J2534_ULONG d = open_dev(), ch = connect_dev(d);
    arm_park(0x0a, 1);
    job_t w, r, x;
    job_startx(&w, J_WRITE, ch, 1, 0x7FFFFFFF);
    release_start();
    wait_parked(1);                                  /* write is inside the transport */
    T_lock(); int slept = sleep_events; T_unlock();
    job_startx(&r, J_READ, ch, 1, 0x7FFFFFFF);       /* waits for the I/O lock */
    T_lock(); T_WAIT(sleep_events > slept); T_unlock();   /* it is in the lock wait now */
    job_start(&x, use_disconnect ? J_DISCONNECT : J_CLOSE, use_disconnect ? ch : d);
    T_lock(); T_WAIT(r.done); T_unlock();            /* the exclusive call is queued */
    job_join(&r);
    CHECK(r.rc == ERR_FAILED && r.n == 0, "the waiting read gave way with no messages");
    uint64_t t0 = vnow();
    unsigned s0 = vsleeps();
    PASSTHRU_MSG m;
    J2534_ULONG n = 1;
    long rc = PassThruReadMsgs(ch, &m, &n, 5000);
    CHECK(rc == ERR_INVALID_CHANNEL_ID && n == 0, "Read behind a queued exclusive call fails at once");
    n = 1;
    rc = PassThruWriteMsgs(ch, w.msg, &n, 5000);
    CHECK(rc == ERR_INVALID_CHANNEL_ID && n == 0, "Write behind a queued exclusive call fails at once");
    n = 1;
    CHECK(PassThruIoctl(ch, CLEAR_RX_BUFFER, NULL, NULL) == ERR_INVALID_CHANNEL_ID, "Ioctl behind a queued exclusive call fails at once");
    CHECK(vnow() == t0 && vsleeps() == s0, "the refusals did not wait or sleep");
    T_lock(); int closes_while_parked = X.closes; T_unlock();
    CHECK(closes_while_parked == 0 && !job_done(&x), "the exclusive call waits for the active write");
    release_parks();
    job_join(&w); job_join(&x);
    CHECK(w.rc == 0 && w.n == 1, "the active write completed normally");
    CHECK(x.rc == 0, "the exclusive call finished after the write left");
    if (use_disconnect) {
        /* The read was canceled while waiting for the lock: no command was
         * outstanding, so the session is not poisoned. */
        J2534_ULONG ch2 = 0;
        CHECK(PassThruConnect(d, ISO9141, 0x1200, 10400, &ch2) == 0 && ch2 && ch2 != ch,
              "reconnect works: a read canceled at the lock leaves no unknown result");
        close_dev(d);
    }
    T_lock(); int closes = X.closes, viol = X.close_violation; T_unlock();
    CHECK(closes == 1 && viol == 0, "context freed only after the write left it");
}

static void test_close_waits_for_active_write(void)
{
    busy_write_scenario(0);
    puts("done: close waits for active write");
}

static void test_disconnect_waits_for_active_write(void)
{
    busy_write_scenario(1);
    puts("done: disconnect waits for active write");
}

static void test_close_during_connect(void)
{
    x_reset();
    J2534_ULONG d = open_dev();
    arm_park(0x07, 1);
    job_t k, c;
    job_start(&k, J_CONNECT, d);
    release_start();
    wait_parked(1);
    job_start(&c, J_CLOSE, d);
    release_parks();
    job_join(&k); job_join(&c);
    T_lock(); int closes = X.closes, viol = X.close_violation; T_unlock();
    CHECK(k.rc == 0 && k.out && c.rc == 0, "Connect finishes, then Close finishes");
    CHECK(closes == 1 && viol == 0, "Close did not free the context under Connect");
    J2534_ULONG n = 1;
    PASSTHRU_MSG m;
    CHECK(PassThruReadMsgs((J2534_ULONG)k.out, &m, &n, 0) == ERR_INVALID_CHANNEL_ID && n == 0,
          "channel from the finished Connect is dead after Close");
    puts("done: close during connect");
}

/* A read parked in the transport with its poll command unanswered. */
static void test_close_cancels_read(void)
{
    x_reset();
    J2534_ULONG d = open_dev(), ch = connect_dev(d);
    T_lock(); X.mute_op = 9; X.gate_reads = 1; T_unlock();
    job_t r, c;
    job_startx(&r, J_READ, ch, 1, 0x7FFFFFFF);
    release_start();
    wait_reads_parked(1);
    job_start(&c, J_CLOSE, d);
    grant_slices_until_done(&c);
    job_join(&r); job_join(&c);
    T_lock(); int closes = X.closes, viol = X.close_violation, dbl = X.double_close; T_unlock();
    CHECK(c.rc == 0, "Close returned while a read had an effectively infinite timeout");
    CHECK(r.rc == ERR_FAILED && r.n == 0, "the canceled read reports an interrupted read with no messages");
    CHECK(closes == 1 && viol == 0 && dbl == 0, "context closed once, after the read left the transport");
    J2534_ULONG n = 1;
    PASSTHRU_MSG m;
    CHECK(PassThruReadMsgs(ch, &m, &n, 10) == ERR_INVALID_CHANNEL_ID && n == 0, "stale channel is invalid");
    puts("done: close cancels a parked read");
}

/* Cancellation during a nonblocking preflight drain must stop before the
 * poll command crosses the send boundary. The session remains reconnectable. */
static void test_cancel_during_preflight_sends_no_poll(void)
{
    x_reset();
    J2534_ULONG d = open_dev(), ch = connect_dev(d);
    T_lock(); X.mute_op = 9; X.gate_nonblocking = 1; X.cancel_seen = 0; T_unlock();
    job_t r, dis;
    job_startx(&r, J_READ, ch, 1, 10000);
    release_start();
    T_lock(); T_WAIT(X.nonblocking_parked); T_unlock();
    job_start(&dis, J_DISCONNECT, ch);
    T_lock(); T_WAIT(X.cancel_seen); X.release_nonblocking = 1; T_signal(); T_unlock();
    job_join(&r); job_join(&dis);
    CHECK(r.rc == ERR_FAILED && r.n == 0, "preflight cancellation interrupts the public read");
    CHECK(frames_op(9) == 0, "cancellation before the send boundary emits no read poll");
    CHECK(dis.rc == 0, "Disconnect after a pre-send cancellation");
    T_lock(); X.mute_op = -1; T_unlock();
    J2534_ULONG ch2 = 0;
    CHECK(PassThruConnect(d, ISO9141, 0x1200, 10400, &ch2) == 0 && ch2 && ch2 != ch,
          "cancel before sending does not poison reconnect");
    close_dev(d);
    puts("done: preflight cancel sends no poll");
}

/* A positive byte is not a reason to ignore cancellation. Stop at the next
 * byte boundary, preserve the fact that the sent poll has an unknown result. */
static void test_cancel_between_positive_reads(void)
{
    x_reset();
    J2534_ULONG d = open_dev(), ch = connect_dev(d);
    const uint8_t data[6] = { 0x48, 0x6b, 0x10, 0x41, 0x0c, 0x55 };
    dev_push_poll_record(live_dev(), data, sizeof data);
    T_lock(); X.drip_positive = 1; X.gate_positive = 1; X.cancel_seen = 0; T_unlock();
    job_t r, dis;
    job_startx(&r, J_READ, ch, 1, 10000);
    release_start();
    T_lock(); T_WAIT(X.positive_parked); T_unlock();
    job_start(&dis, J_DISCONNECT, ch);
    T_lock(); T_WAIT(X.cancel_seen); X.release_positive = 1; T_signal(); T_unlock();
    job_join(&r); job_join(&dis);
    T_lock(); int positive_reads = X.positive_reads; X.drip_positive = 0; T_unlock();
    CHECK(r.rc == ERR_FAILED && r.n == 0, "post-send cancellation interrupts the public read");
    CHECK(positive_reads == 2, "canceled read stops after the in-flight byte and before another transport read");
    CHECK(dis.rc == 0, "Disconnect after a post-send cancellation");
    int connects = frames_op(0x07);
    J2534_ULONG ch2 = 0;
    CHECK(PassThruConnect(d, ISO9141, 0x1200, 10400, &ch2) != 0 && ch2 == 0,
          "cancel after sending preserves the poisoned session");
    CHECK(frames_op(0x07) == connects, "poisoned reconnect sends no Connect command");
    close_dev(d);
    puts("done: positive-read cancellation poisons sent poll");
}

/* Cancel after the read poll was sent: its result is unknown, so the context
 * stays poisoned past a clean Disconnect. Only Close/Open recovers. */
static void test_disconnect_after_ambiguous_cancel(void)
{
    x_reset();
    J2534_ULONG d = open_dev(), ch = connect_dev(d);
    T_lock(); X.mute_op = 9; X.gate_reads = 1; T_unlock();
    job_t r, x;
    job_startx(&r, J_READ, ch, 1, 0x7FFFFFFF);
    release_start();
    wait_reads_parked(1);
    job_start(&x, J_DISCONNECT, ch);
    grant_slices_until_done(&x);
    job_join(&r); job_join(&x);
    CHECK(x.rc == 0, "Disconnect returned and was acknowledged cleanly");
    CHECK(r.rc == ERR_FAILED && r.n == 0, "the canceled read reports no messages");
    int connects = frames_op(0x07);
    T_lock(); X.mute_op = -1; X.gate_reads = 0; T_unlock();
    J2534_ULONG ch2 = 0;
    long rc = PassThruConnect(d, ISO9141, 0x1200, 10400, &ch2);
    CHECK(rc != 0 && ch2 == 0, "reconnect after an ambiguous cancel is refused");
    CHECK(frames_op(0x07) == connects, "the refused reconnect sent nothing to the adapter");
    close_dev(d);
    J2534_ULONG d2 = open_dev();
    J2534_ULONG ch3 = connect_dev(d2);
    CHECK(ch3 != ch, "Close/Open recovers with a new channel handle");
    close_dev(d2);
    puts("done: ambiguous cancel keeps the session poisoned");
}

/* Cancel with no command outstanding (a partial unsolicited frame): the
 * session stays usable and the partial bytes do not leak into the next one. */
static void test_disconnect_clears_partial_frame(void)
{
    x_reset();
    J2534_ULONG d = open_dev(), ch = connect_dev(d);
    struct mvci_io *dv = live_dev();
    uint8_t f[MVCI_MAX_FRAME], data[6] = { 0x48, 0x6b, 0x10, 0x41, 0x0c, 0x55 };
    T_lock(); q_record(dv, data, 6, 0); T_unlock();   /* build one record frame ... */
    T_lock();
    int len = dv->qt;                                   /* ... and keep only 3 bytes */
    memcpy(f, dv->rxq, (size_t)len);
    dv->qh = dv->qt = 0;
    T_unlock();
    dev_push_raw(dv, f, 3);
    T_lock(); X.gate_reads = 1; T_unlock();
    job_t r, x;
    job_startx(&r, J_READ, ch, 1, 0x7FFFFFFF);
    release_start();
    wait_reads_parked(1);
    job_start(&x, J_DISCONNECT, ch);
    grant_slices_until_done(&x);
    job_join(&r); job_join(&x);
    CHECK(x.rc == 0 && r.rc == ERR_FAILED && r.n == 0, "read canceled while waiting for the rest of a frame");
    T_lock(); X.gate_reads = 0; T_unlock();
    J2534_ULONG ch2 = 0;
    CHECK(PassThruConnect(d, ISO9141, 0x1200, 10400, &ch2) == 0 && ch2 && ch2 != ch,
          "reconnect works: no command was outstanding");
    dev_push_poll_record(dv, data, 6);
    PASSTHRU_MSG m;
    J2534_ULONG n = 1;
    long rc = PassThruReadMsgs(ch2, &m, &n, 100);
    CHECK(rc == 0 && n == 1 && m.DataSize == 6 && memcmp(m.Data, data, 6) == 0 && m.RxStatus == 0,
          "the new session reads a clean frame, not one joined to stale bytes");
    close_dev(d);
    puts("done: partial frame does not survive a session boundary");
}

static void test_stale_queued_frame(void)
{
    x_reset();
    J2534_ULONG d = open_dev(), ch = connect_dev(d);
    struct mvci_io *dv = live_dev();
    const uint8_t a[5] = { 0x48, 0x6b, 0x10, 0x41, 0x01 };
    dev_push_unsolicited(dv, a, 5);
    dev_push_unsolicited(dv, a, 5);
    /* A command transaction queues unsolicited records on the host. */
    CHECK(PassThruIoctl(ch, CLEAR_PERIODIC_MSGS, NULL, NULL) == 0, "command with queued unsolicited frames");
    CHECK(PassThruDisconnect(ch) == 0, "Disconnect");
    J2534_ULONG ch2 = connect_dev(d);
    PASSTHRU_MSG m;
    J2534_ULONG n = 1;
    long rc = PassThruReadMsgs(ch2, &m, &n, 0);
    CHECK(rc == ERR_BUFFER_EMPTY && n == 0, "old frames do not reach the next session");
    close_dev(d);
    puts("done: stale queued frames");
}

static void test_duplicate_connect(void)
{
    x_reset();
    J2534_ULONG d = open_dev();
    arm_park(0x07, 1);
    job_t a, b;
    job_start(&a, J_CONNECT, d);
    release_start();
    wait_parked(1);                                  /* A owns the slot, inside the wire exchange */
    job_start(&b, J_CONNECT, d);
    release_parks();
    job_join(&a); job_join(&b);
    CHECK(a.rc == 0 && a.out, "first Connect succeeds");
    CHECK(b.rc == ERR_CHANNEL_IN_USE, "concurrent duplicate Connect is refused");
    CHECK(frames_op(0x07) == 1, "only one Connect reached the adapter");
    J2534_ULONG ch = 0;
    CHECK(PassThruConnect(d, ISO9141, 0x1200, 10400, &ch) == ERR_CHANNEL_IN_USE && ch == 0,
          "later duplicate Connect is refused");
    CHECK(frames_op(0x07) == 1, "still one Connect on the wire");
    /* One channel per device. A second device is independent. */
    J2534_ULONG d2 = open_dev();
    CHECK(connect_dev(d2) != 0, "another device connects");
    close_dev(d2); close_dev(d);
    puts("done: duplicate connect");
}

static void test_disconnect_race(void)
{
    x_reset();
    J2534_ULONG d = open_dev(), ch = connect_dev(d);
    arm_park(0x02, 1);
    job_t a, b;
    job_start(&a, J_DISCONNECT, ch);
    release_start();
    wait_parked(1);
    job_start(&b, J_DISCONNECT, ch);
    release_parks();
    job_join(&a); job_join(&b);
    CHECK(a.rc == 0 && b.rc == ERR_INVALID_CHANNEL_ID, "second Disconnect of the same channel is invalid");
    CHECK(frames_op(0x02) == 1, "one Disconnect reached the adapter");
    close_dev(d);
    puts("done: disconnect race");
}

static void test_admission(void)
{
    x_reset();
    J2534_ULONG d = open_dev();
    struct { J2534_ULONG proto, flags, baud; long rc; const char *name; } t[] = {
        { J1850VPW, 0, 10400, ERR_NOT_SUPPORTED, "J1850VPW" },
        { 0x06 /*CAN*/, 0, 500000, ERR_NOT_SUPPORTED, "CAN" },
        { 0x07 /*ISO15765*/, 0, 500000, ERR_NOT_SUPPORTED, "ISO15765" },
        { SCI_B_TRANS, 0, 7812, ERR_NOT_SUPPORTED, "SCI_B_TRANS" },
        { 0, 0, 10400, ERR_INVALID_PROTOCOL_ID, "protocol 0" },
        { 0x0B, 0, 10400, ERR_INVALID_PROTOCOL_ID, "undefined protocol" },
        { 0xFFFFFFFFu, 0, 10400, ERR_INVALID_PROTOCOL_ID, "protocol -1" },
        { ISO9141, 0x1, 10400, ERR_INVALID_FLAGS, "unknown flag bit" },
        { ISO9141, 0x80000000u, 10400, ERR_INVALID_FLAGS, "high flag bit" },
        { ISO14230, 0x1200 | 0x0400, 10400, ERR_INVALID_FLAGS, "extra flag next to the allowed ones" },
        { ISO9141, 0x1200, 9600, ERR_INVALID_BAUDRATE, "9600 baud" },
        { ISO9141, 0x1200, 0, ERR_INVALID_BAUDRATE, "baud 0" },
        { ISO14230, 0, 0xFFFFFFFFu, ERR_INVALID_BAUDRATE, "baud -1" },
    };
    int before = frames_op(0x07);
    for (unsigned i = 0; i < sizeof t / sizeof *t; i++) {
        J2534_ULONG ch = 0xdead;
        long rc = PassThruConnect(d, t[i].proto, t[i].flags, t[i].baud, &ch);
        CHECK(rc == t[i].rc && ch == 0xdead, t[i].name);
    }
    CHECK(frames_op(0x07) == before, "no refused combination reached the adapter");
    CHECK(PassThruConnect(d, ISO14230, 0, 10400, NULL) == ERR_NULL_PARAMETER, "NULL channel pointer");
    J2534_ULONG ch = 0;
    CHECK(PassThruConnect(d, ISO14230, 0, 10400, &ch) == 0 && ch, "ISO14230 / no flags / 10400 is admitted");
    CHECK(PassThruDisconnect(ch) == 0, "Disconnect");
    CHECK(PassThruConnect(d, ISO9141, 0x1200, 10400, &ch) == 0 && ch, "ISO9141 / 0x1200 / 10400 is admitted");
    close_dev(d);
    puts("done: admission");
}

static void test_stale_handles(void)
{
    x_reset();
    J2534_ULONG d1 = open_dev();
    close_dev(d1);
    J2534_ULONG d2 = open_dev();
    CHECK(d2 != d1, "a reopened device gets a new handle");
    J2534_ULONG ch = 0;
    char a[80], b[80], c[80];
    CHECK(PassThruConnect(d1, ISO9141, 0x1200, 10400, &ch) == ERR_INVALID_DEVICE_ID, "stale device: Connect");
    CHECK(PassThruClose(d1) == ERR_INVALID_DEVICE_ID, "stale device: Close");
    CHECK(PassThruReadVersion(d1, a, b, c) == ERR_INVALID_DEVICE_ID, "stale device: ReadVersion");
    CHECK(PassThruSetProgrammingVoltage(d1, 15, 0xFFFFFFFEu) == ERR_INVALID_DEVICE_ID, "stale device: voltage");
    CHECK(PassThruIoctl(d1, READ_VBATT, NULL, NULL) == ERR_INVALID_DEVICE_ID, "stale device: READ_VBATT");
    CHECK(PassThruReadVersion(d2, a, b, c) == 0, "live device: ReadVersion");
    CHECK(PassThruIoctl(d2, READ_VBATT, NULL, NULL) == ERR_NOT_SUPPORTED, "live device: READ_VBATT stays unsupported");

    J2534_ULONG ch_a = connect_dev(d2);
    CHECK(PassThruIoctl(ch_a, READ_VBATT, NULL, NULL) == ERR_INVALID_DEVICE_ID, "a channel handle is not a device handle");
    CHECK(PassThruReadVersion(ch_a, a, b, c) == ERR_INVALID_DEVICE_ID, "a channel handle is not a device handle (version)");
    CHECK(PassThruDisconnect(ch_a) == 0, "Disconnect");
    J2534_ULONG ch_b = connect_dev(d2);
    CHECK(ch_b != ch_a, "a reconnect gets a new channel handle");
    PASSTHRU_MSG m, mask, pat;
    memset(&m, 0, sizeof m); memset(&mask, 0, sizeof mask); memset(&pat, 0, sizeof pat);
    m.ProtocolID = ISO9141; m.DataSize = 5;
    mask.DataSize = pat.DataSize = 1;
    J2534_ULONG n = 9, f = 0;
    CHECK(PassThruReadMsgs(ch_a, &m, &n, 10) == ERR_INVALID_CHANNEL_ID && n == 0, "stale channel: Read");
    n = 9;
    CHECK(PassThruWriteMsgs(ch_a, &m, &n, 10) == ERR_INVALID_CHANNEL_ID && n == 0, "stale channel: Write");
    CHECK(PassThruIoctl(ch_a, CLEAR_RX_BUFFER, NULL, NULL) == ERR_INVALID_CHANNEL_ID, "stale channel: Ioctl");
    CHECK(PassThruStartMsgFilter(ch_a, PASS_FILTER, &mask, &pat, NULL, &f) == ERR_INVALID_CHANNEL_ID, "stale channel: StartMsgFilter");
    CHECK(PassThruStopMsgFilter(ch_a, 1) == ERR_INVALID_CHANNEL_ID, "stale channel: StopMsgFilter");
    CHECK(PassThruDisconnect(ch_a) == ERR_INVALID_CHANNEL_ID, "stale channel: Disconnect");

    /* Malformed ids never index anything. */
    const J2534_ULONG bad[] = { 0, 0xFFFFFFFFu, 0x80000000u, 0x7FFFFFFFu, 0xFFFFFFF0u, 0x00FFFFFFu };
    for (unsigned i = 0; i < sizeof bad / sizeof *bad; i++) {
        if (bad[i] == d2 || bad[i] == ch_b) continue;
        n = 9;
        int ok = PassThruConnect(bad[i], ISO9141, 0x1200, 10400, &ch) == ERR_INVALID_DEVICE_ID &&
                 PassThruClose(bad[i]) == ERR_INVALID_DEVICE_ID &&
                 PassThruReadMsgs(bad[i], &m, &n, 0) == ERR_INVALID_CHANNEL_ID && n == 0 &&
                 PassThruDisconnect(bad[i]) == ERR_INVALID_CHANNEL_ID &&
                 PassThruReadVersion(bad[i], a, b, c) == ERR_INVALID_DEVICE_ID;
        CHECK(ok, "malformed handle is refused by every entry point");
    }
    CHECK(PassThruDisconnect(d2) == ERR_INVALID_CHANNEL_ID, "a device handle is not a channel handle");
    close_dev(d2);
    puts("done: stale handles");
}

static void test_handle_exhaustion(void)
{
    x_reset();
    mvci_test_set_next_handle(0xFFFFFFFDu);
    J2534_ULONG d = 0, ch = 0;
    CHECK(PassThruOpen((void *)"lifecycle", &d) == 0 && d == 0xFFFFFFFDu, "Open takes the second-to-last handle");
    CHECK(PassThruConnect(d, ISO9141, 0x1200, 10400, &ch) == 0 && ch == 0xFFFFFFFEu, "Connect takes the last handle");
    CHECK(PassThruDisconnect(ch) == 0, "Disconnect");
    int connects = frames_op(0x07);
    ch = 0;
    CHECK(PassThruConnect(d, ISO9141, 0x1200, 10400, &ch) == ERR_FAILED && ch == 0, "Connect refuses once handles run out");
    CHECK(frames_op(0x07) == connects, "exhausted Connect sent nothing to the adapter");
    T_lock(); int opens = X.opens; T_unlock();
    J2534_ULONG d2 = 0;
    CHECK(PassThruOpen((void *)"lifecycle", &d2) == ERR_FAILED && d2 == 0, "Open refuses once handles run out");
    T_lock(); int opens2 = X.opens; T_unlock();
    CHECK(opens2 == opens, "exhausted Open never opened the transport");
    close_dev(d);
    mvci_test_set_next_handle(0xFFFFFFFFu);
    CHECK(PassThruOpen((void *)"lifecycle", &d2) == ERR_FAILED, "the counter does not wrap past the last handle");
    mvci_test_set_next_handle(0x1000u);
    puts("done: handle exhaustion");
}

static void test_session_state_reset(void)
{
    x_reset();
    J2534_ULONG d = open_dev(), ch = connect_dev(d);
    SCONFIG one = { LOOPBACK, 0 };
    SCONFIG_LIST list = { 1, &one };
    CHECK(PassThruIoctl(ch, SET_CONFIG, &list, NULL) == 0, "LOOPBACK off");
    one.Value = 99;
    CHECK(PassThruIoctl(ch, GET_CONFIG, &list, NULL) == 0 && one.Value == 0, "LOOPBACK reads back off");
    PASSTHRU_MSG mask, pat;
    memset(&mask, 0, sizeof mask); memset(&pat, 0, sizeof pat);
    mask.DataSize = pat.DataSize = 1; mask.Data[0] = 0xff; pat.Data[0] = 1;
    J2534_ULONG f1 = 0;
    CHECK(PassThruStartMsgFilter(ch, PASS_FILTER, &mask, &pat, NULL, &f1) == 0 && f1 == 1, "filter started");
    CHECK(PassThruDisconnect(ch) == 0, "Disconnect");
    J2534_ULONG ch2 = connect_dev(d);
    one.Value = 99;
    CHECK(PassThruIoctl(ch2, GET_CONFIG, &list, NULL) == 0 && one.Value == 1, "a new channel starts with the default LOOPBACK policy");
    CHECK(PassThruStopMsgFilter(ch2, f1) == ERR_INVALID_FILTER_ID, "a filter from the old channel is gone");
    J2534_ULONG f2 = 0;
    CHECK(PassThruStartMsgFilter(ch2, PASS_FILTER, &mask, &pat, NULL, &f2) == 0 && f2 == 1, "new channel numbers filters from the start");
    CHECK(PassThruStopMsgFilter(ch2, f2) == 0, "the new filter stops");
    close_dev(d);
    puts("done: session state reset");
}

static void test_connect_result_not_retried(void)
{
    x_reset();
    J2534_ULONG d = open_dev();
    T_lock(); X.mute_op = 7; T_unlock();
    J2534_ULONG ch = 0;
    CHECK(PassThruConnect(d, ISO9141, 0x1200, 10400, &ch) == ERR_FAILED && ch == 0, "missing Connect ACK fails");
    CHECK(frames_op(0x07) == 1, "the Connect was sent once and never retried");
    T_lock(); X.mute_op = -1; T_unlock();
    CHECK(PassThruConnect(d, ISO9141, 0x1200, 10400, &ch) != 0 && ch == 0, "a context with an unknown result refuses Connect");
    CHECK(frames_op(0x07) == 1, "the refusal sent nothing");
    close_dev(d);
    d = open_dev();
    CHECK(connect_dev(d) != 0, "Close/Open recovers");
    close_dev(d);

    /* A complete rejection is a result, not an unknown: no automatic retry. */
    x_reset();
    d = open_dev();
    T_lock(); X.reject_op = 7; T_unlock();
    CHECK(PassThruConnect(d, ISO9141, 0x1200, 10400, &ch) == ERR_FAILED, "rejected Connect fails");
    CHECK(frames_op(0x07) == 1, "the rejected Connect was not retried");
    T_lock(); X.reject_op = -1; T_unlock();
    CHECK(connect_dev(d) != 0, "an explicit later Connect works after a complete rejection");
    close_dev(d);
    puts("done: connect result not retried");
}

static void test_purge_failure_at_connect(void)
{
    x_reset();
    J2534_ULONG d = open_dev();
    T_lock(); X.purge_fail = 1; T_unlock();
    J2534_ULONG ch = 0;
    long rc = PassThruConnect(d, ISO9141, 0x1200, 10400, &ch);
    CHECK(rc == ERR_DEVICE_NOT_CONNECTED && ch == 0, "failed host purge refuses Connect");
    CHECK(frames_op(0x07) == 0, "nothing was sent after the failed purge");
    T_lock(); X.purge_fail = 0; T_unlock();
    CHECK(connect_dev(d) != 0, "Connect works after a later successful purge");
    close_dev(d);
    puts("done: purge failure at connect");
}

/* Two state changes on one channel never overlap. The first start is parked in
 * the transport while the second is requested. The device must see distinct
 * message handles, and both filters must stop. */
static void test_filter_serialization(void)
{
    x_reset();
    J2534_ULONG d = open_dev(), ch = connect_dev(d);
    arm_park(0x0b, 1);
    job_t a, b;
    job_startx(&a, J_START_FILTER, ch, 1, 0);
    release_start();
    wait_parked(1);
    job_startx(&b, J_START_FILTER, ch, 2, 0);
    release_parks();
    job_join(&a); job_join(&b);
    CHECK(a.rc == 0 && b.rc == 0, "both filters start");
    CHECK(a.out != b.out && a.out && b.out, "filter ids are distinct");
    T_lock(); int ids_distinct = X.nmsgids == 2 && X.msgids[0] != X.msgids[1]; T_unlock();
    CHECK(ids_distinct, "the device saw two different filter handles");
    CHECK(PassThruStopMsgFilter(ch, (J2534_ULONG)a.out) == 0 && PassThruStopMsgFilter(ch, (J2534_ULONG)b.out) == 0,
          "both filters stop");
    close_dev(d);
    puts("done: filter serialization");
}

static void test_budgets(void)
{
    x_reset();
    J2534_ULONG d = open_dev(), ch = connect_dev(d);
    PASSTHRU_MSG m;
    memset(&m, 0, sizeof m);
    m.ProtocolID = ISO9141; m.DataSize = 5;
    memcpy(m.Data, "\x82\x11\xf1\x01\x00", 5);

    /* Timeouts above INT_MAX are refused, never widened into "forever". */
    const J2534_ULONG huge[] = { 0x80000000u, 0xFFFFFFFFu };
    uint64_t t0 = vnow();
    unsigned s0 = vsleeps();
    int frames_before = frames_op(0x09) + frames_op(0x0a);
    for (unsigned i = 0; i < 2; i++) {
        J2534_ULONG n = 7;
        CHECK(PassThruReadMsgs(ch, &m, &n, huge[i]) == ERR_INVALID_TIME_INTERVAL && n == 0, "Read: timeout above INT_MAX");
        n = 7;
        CHECK(PassThruWriteMsgs(ch, &m, &n, huge[i]) == ERR_INVALID_TIME_INTERVAL && n == 0, "Write: timeout above INT_MAX");
    }
    CHECK(vnow() == t0 && vsleeps() == s0, "refusals consumed no time");
    CHECK(frames_op(0x09) + frames_op(0x0a) == frames_before, "refusals sent nothing");

    /* The largest accepted timeout still works when data is ready. */
    uint8_t data[4] = { 0x48, 0x6b, 0x10, 0x41 };
    dev_push_poll_record(live_dev(), data, 4);
    J2534_ULONG n = 1;
    CHECK(PassThruReadMsgs(ch, &m, &n, 0x7FFFFFFFu) == 0 && n == 1, "INT_MAX timeout is accepted");

    /* Every refusal reports zero messages. */
    n = 7;
    CHECK(PassThruReadMsgs(0xdeadbeefu, &m, &n, 10) == ERR_INVALID_CHANNEL_ID && n == 0, "invalid channel zeroes the count");
    n = 7;
    CHECK(PassThruReadMsgs(ch, NULL, &n, 10) == ERR_NULL_PARAMETER && n == 0, "NULL buffer zeroes the count");
    n = 0;
    CHECK(PassThruWriteMsgs(ch, &m, &n, 10) == ERR_NULL_PARAMETER && n == 0, "empty count is refused");
    CHECK(PassThruReadMsgs(ch, &m, NULL, 10) == ERR_NULL_PARAMETER, "NULL count pointer");

    /* A write holds the I/O lock. A read's budget includes waiting for it. */
    arm_park(0x0a, 1);
    job_t w;
    job_startx(&w, J_WRITE, ch, 1, 5000);
    release_start();
    wait_parked(1);
    t0 = vnow(); s0 = vsleeps();
    n = 1;
    long rc = PassThruReadMsgs(ch, &m, &n, 0);
    CHECK(rc == ERR_TIMEOUT && n == 0, "zero-timeout Read behind a busy lock does not wait");
    CHECK(vnow() == t0 && vsleeps() == s0, "zero-timeout Read never slept");
    n = 1;
    rc = PassThruReadMsgs(ch, &m, &n, 50);
    uint64_t waited = vnow() - t0;
    CHECK(rc == ERR_TIMEOUT && n == 0, "timed Read behind a busy lock times out");
    CHECK(waited == 50ull * 1000000ull, "the wait spent exactly the caller's 50 ms budget");
    release_parks();
    job_join(&w);
    CHECK(w.rc == 0 && w.n == 1, "the parked write completes");
    close_dev(d);
    puts("done: budgets");
}


/* A Disconnect that got no exact reply has an unknown result. The context
 * refuses a new session until Close/Open, even after a later clean reply. */
static void test_failed_disconnect_poisons(void)
{
    x_reset();
    J2534_ULONG d = open_dev(), ch = connect_dev(d);
    T_lock(); X.mute_op = 2; T_unlock();
    CHECK(PassThruDisconnect(ch) == ERR_FAILED, "unanswered Disconnect fails");
    PASSTHRU_MSG m;
    J2534_ULONG n = 1;
    memset(&m, 0, sizeof m);
    CHECK(PassThruReadMsgs(ch, &m, &n, 0) != ERR_INVALID_CHANNEL_ID, "the channel stays connected after a failed Disconnect");
    T_lock(); X.mute_op = -1; T_unlock();
    CHECK(PassThruDisconnect(ch) == 0, "a later Disconnect gets the exact reply");
    int connects = frames_op(0x07);
    J2534_ULONG ch2 = 0;
    CHECK(PassThruConnect(d, ISO9141, 0x1200, 10400, &ch2) != 0 && ch2 == 0, "a clean reply does not undo the earlier unknown result");
    CHECK(frames_op(0x07) == connects, "the refused Connect sent nothing");
    close_dev(d);
    d = open_dev();
    CHECK(connect_dev(d) != 0, "Close/Open recovers");
    close_dev(d);
    puts("done: failed disconnect poisons");
}

/* Read waits in slices but keeps the caller's whole budget. */
static void test_sliced_read_keeps_budget(void)
{
    x_reset();
    J2534_ULONG d = open_dev(), ch = connect_dev(d);
    struct mvci_io *dv = live_dev();
    T_lock(); X.mute_op = 9; X.gate_reads = 1; T_unlock();
    job_t r;
    job_startx(&r, J_READ, ch, 1, 100);
    release_start();
    wait_reads_parked(1);
    uint8_t data[4] = { 0x48, 0x6b, 0x10, 0x41 };
    for (int i = 0; i < 3; i++) {                     /* three slices of 20 ms pass */
        T_lock();
        X.slice_tokens = 1;
        T_signal();
        T_WAIT(X.slice_tokens == 0 && X.reads_parked >= 1);
        T_unlock();
    }
    T_lock();
    q_record(dv, data, 4, 0);                         /* the poll's reply, inside the 100 ms budget */
    X.slice_tokens = 1;
    T_signal();
    T_unlock();
    job_join(&r);
    CHECK(r.rc == 0 && r.n == 1 && r.msg[0].DataSize == 4 && memcmp(r.msg[0].Data, data, 4) == 0,
          "a reply that arrives after several slices is delivered");
    T_lock(); X.mute_op = -1; X.gate_reads = 0; T_unlock();
    PASSTHRU_MSG m;
    J2534_ULONG n = 1;
    dev_push_poll_record(dv, data, 4);
    CHECK(PassThruReadMsgs(ch, &m, &n, 100) == 0 && n == 1, "slicing left the session usable");

    /* No reply at all: the read ends at its own deadline, not at a slice. */
    T_lock(); X.mute_op = 9; X.gate_reads = 1; X.slice_tokens = 0; T_unlock();
    uint64_t t0 = vnow();
    job_startx(&r, J_READ, ch, 1, 100);
    wait_reads_parked(1);
    for (int i = 0; i < 5; i++) {
        T_lock();
        X.slice_tokens = 1;
        T_signal();
        T_WAIT(X.slice_tokens == 0 && (X.reads_parked >= 1 || r.done));
        T_unlock();
    }
    job_join(&r);
    CHECK(r.rc == ERR_TIMEOUT && r.n == 0, "an unanswered read times out at its deadline");
    CHECK(vnow() - t0 == 100ull * 1000000ull, "the wait used the whole 100 ms and no more");
    T_lock(); X.mute_op = -1; X.gate_reads = 0; T_unlock();
    close_dev(d);
    puts("done: sliced read keeps its budget");
}

int main(void)
{
    mvci_mutex_init(&TM);
    mvci_cond_init(&TC);
    signal(SIGALRM, on_alarm);
    alarm(120);

    test_open_close_races();
    test_close_waits_for_active_write();
    test_disconnect_waits_for_active_write();
    test_close_during_connect();
    test_close_cancels_read();
    test_cancel_during_preflight_sends_no_poll();
    test_cancel_between_positive_reads();
    test_disconnect_after_ambiguous_cancel();
    test_disconnect_clears_partial_frame();
    test_stale_queued_frame();
    test_duplicate_connect();
    test_disconnect_race();
    test_admission();
    test_stale_handles();
    test_handle_exhaustion();
    test_session_state_reset();
    test_connect_result_not_retried();
    test_purge_failure_at_connect();
    test_filter_serialization();
    test_budgets();
    test_failed_disconnect_poisons();
    test_sliced_read_keeps_budget();

    printf("%d passed, %d failed, 0 skipped\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
#endif /* !_WIN32 */

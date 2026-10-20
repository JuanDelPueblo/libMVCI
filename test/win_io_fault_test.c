/* win_io_fault_test.c — status handling of the Windows FTDI D2XX transport
 * (the _WIN32 half of src/io.c), run on any host.
 *
 * src/io.c is compiled into this file as _WIN32, against a fake <windows.h>
 * and a scripted ftd2xx module (test/fake/win/windows.h). Time is virtual:
 * Sleep() only adds to a counter. This proves the call order of the open
 * sequence is unchanged, that every D2XX failure reaches the caller, and that
 * a failed open releases its handle. Reads and writes are checked against a
 * model of the documented D2XX contract (FTDI D2XX Programmer's Guide v1.5):
 * FT_Read returns at once when asked for no more than FT_GetQueueStatus
 * reports, otherwise it waits up to the read timeout and returns FT_OK with a
 * short count. FT_Write is modeled to report a write timeout as a short count:
 * the guide documents that only for FT_W32_WriteFile, so it is an inference
 * for FT_Write, and the status-failure case is tested as well. The guide
 * gives no meaning for a timeout of 0, so the model hangs forever there: any
 * path that could reach it fails the test. This does NOT prove D2XX or Windows
 * behavior: the real driver stays UNPROVEN offline.
 *
 * Build (no hardware, no arguments, no D2XX installation):
 *   cc -std=gnu11 -D_WIN32 -Iinclude -Isrc -Itest/fake/win \
 *      test/win_io_fault_test.c -o <dir>/win_io_fault_test
 */
#include "../src/io.c"

#include <stdio.h>

static int g_fail, g_pass;
#define CHECK(cond, name) do { \
    if (cond) g_pass++; else { g_fail++; printf("[FAIL] %s (line %d)\n", name, __LINE__); } \
} while (0)

/* ---- virtual time -------------------------------------------------- */
static uint32_t g_now;
static int      g_sleeps[8], g_nsleeps;
void  Sleep(DWORD ms) { if (g_nsleeps < 8) g_sleeps[g_nsleeps++] = (int)ms; g_now += ms; }
DWORD GetTickCount(void) { return g_now; }

/* ---- scripted ftd2xx module ---------------------------------------- */
enum { C_OPEN, C_BAUD, C_DATA, C_FLOW, C_LAT, C_TMO, C_RESET, C_PURGE, C_CLRRTS,
       C_SETDTR, C_CLRDTR, C_WRITE, C_READ, C_QUEUE, C_CLOSE, C_NCALLS };
static const char *const CNAME[C_NCALLS] = {
    "OpenEx","SetBaudRate","SetDataCharacteristics","SetFlowControl","SetLatencyTimer",
    "SetTimeouts","ResetDevice","Purge","ClrRts","SetDtr","ClrDtr","Write","Read","GetQueueStatus","Close" };

static int  g_log[64], g_nlog;
static int  g_fail_index;            /* fail the Nth non-Close call; -1 = never */
static int  g_issued;                /* non-Close calls so far */
static int  g_handles;               /* opened minus closed */
static int  g_dll_present, g_missing_symbol, g_freelib;
static ULONG g_rd_tmo = 100, g_wr_tmo = 1000;   /* as last set through SetTimeouts */
static int  g_write_short, g_write_over, g_write_fail_partial, g_read_avail, g_hang;
static DWORD g_last_read_req, g_last_write_req;
static ULONG g_setup_ms;

#define FT_IO_ERROR 4
static FT_STATUS step(int id)
{
    g_log[g_nlog < 64 ? g_nlog++ : 0] = id;
    if (id == C_CLOSE) return FT_OK;
    return (g_issued++ == g_fail_index) ? FT_IO_ERROR : FT_OK;
}

static FT_STATUS WINAPI f_open(PVOID d, DWORD fl, FT_HANDLE *h)
{ (void)d; (void)fl; FT_STATUS s = step(C_OPEN); if (s == FT_OK) { *h = (FT_HANDLE)&g_handles; g_handles++; } return s; }
static FT_STATUS WINAPI f_close(FT_HANDLE h) { (void)h; g_handles--; return step(C_CLOSE); }
static FT_STATUS WINAPI f_reset(FT_HANDLE h) { (void)h; return step(C_RESET); }
static FT_STATUS WINAPI f_clrrts(FT_HANDLE h) { (void)h; return step(C_CLRRTS); }
static FT_STATUS WINAPI f_setdtr(FT_HANDLE h) { (void)h; return step(C_SETDTR); }
static FT_STATUS WINAPI f_clrdtr(FT_HANDLE h) { (void)h; return step(C_CLRDTR); }
static FT_STATUS WINAPI f_baud(FT_HANDLE h, ULONG b) { (void)h; return b == 115200 ? step(C_BAUD) : FT_IO_ERROR; }
static FT_STATUS WINAPI f_purge(FT_HANDLE h, ULONG m) { (void)h; return m == FT_PURGE_RX ? step(C_PURGE) : FT_IO_ERROR; }
static FT_STATUS WINAPI f_tmo(FT_HANDLE h, ULONG r, ULONG w)
{
    (void)h; FT_STATUS s = step(C_TMO);
    g_now += g_setup_ms;
    if (s == FT_OK) { g_rd_tmo = r; g_wr_tmo = w; }
    return s;
}
static FT_STATUS WINAPI f_data(FT_HANDLE h, UCHAR w, UCHAR s, UCHAR p)
{ (void)h; return (w == 8 && s == 0 && p == 0) ? step(C_DATA) : FT_IO_ERROR; }
static FT_STATUS WINAPI f_flow(FT_HANDLE h, USHORT f, UCHAR a, UCHAR b)
{ (void)h; return (f == 0 && a == 0 && b == 0) ? step(C_FLOW) : FT_IO_ERROR; }
static FT_STATUS WINAPI f_lat(FT_HANDLE h, UCHAR t) { (void)h; return t == 2 ? step(C_LAT) : FT_IO_ERROR; }
static FT_STATUS WINAPI f_write(FT_HANDLE h, LPVOID b, DWORD n, LPDWORD w)
{
    (void)h; (void)b; FT_STATUS s = step(C_WRITE); g_last_write_req = n;
    *w = g_write_short ? n - 1 : g_write_over ? n + 1 : n;
    if (g_write_fail_partial) *w = n - 1;
    if (*w < n) { if (g_wr_tmo == 0) g_hang++; else g_now += g_wr_tmo; }   /* write timeout */
    return s;
}
static FT_STATUS WINAPI f_read(FT_HANDLE h, LPVOID b, DWORD n, LPDWORD g)
{
    (void)h; FT_STATUS s = step(C_READ); g_last_read_req = n;
    DWORD k = (DWORD)g_read_avail < n ? (DWORD)g_read_avail : n;
    if (k < n) { if (g_rd_tmo == 0) g_hang++; else g_now += g_rd_tmo; }     /* read timeout */
    memset(b, 0x5a, k); *g = k; g_read_avail -= (int)k; return s;
}
static FT_STATUS WINAPI f_queue(FT_HANDLE h, LPDWORD q)
{ (void)h; FT_STATUS s = step(C_QUEUE); *q = (DWORD)g_read_avail; return s; }

static const struct { const char *name; FARPROC fn; } SYMS[] = {
    {"FT_OpenEx",(FARPROC)f_open},{"FT_Close",(FARPROC)f_close},{"FT_ResetDevice",(FARPROC)f_reset},
    {"FT_ClrRts",(FARPROC)f_clrrts},{"FT_SetDtr",(FARPROC)f_setdtr},{"FT_ClrDtr",(FARPROC)f_clrdtr},
    {"FT_SetBaudRate",(FARPROC)f_baud},{"FT_Purge",(FARPROC)f_purge},{"FT_SetTimeouts",(FARPROC)f_tmo},
    {"FT_SetDataCharacteristics",(FARPROC)f_data},{"FT_SetFlowControl",(FARPROC)f_flow},
    {"FT_SetLatencyTimer",(FARPROC)f_lat},{"FT_Write",(FARPROC)f_write},{"FT_Read",(FARPROC)f_read},
    {"FT_GetQueueStatus",(FARPROC)f_queue},
};

HMODULE LoadLibraryA(LPCSTR n) { return (g_dll_present && !strcmp(n, "ftd2xx.dll")) ? (HMODULE)&g_dll_present : NULL; }
int     FreeLibrary(HMODULE m) { (void)m; g_freelib++; return 1; }
FARPROC GetProcAddress(HMODULE m, LPCSTR n)
{
    (void)m;
    if (g_missing_symbol && !strcmp(n, g_missing_symbol == 1 ? "FT_Purge" :
                                         g_missing_symbol == 2 ? "FT_Read" : "FT_GetQueueStatus")) return NULL;
    for (size_t i = 0; i < sizeof SYMS / sizeof SYMS[0]; i++)
        if (!strcmp(n, SYMS[i].name)) return SYMS[i].fn;
    return NULL;
}

static void fresh(void)
{
    g_setup_ms = 0;
    g_nlog = g_issued = g_nsleeps = 0; g_fail_index = -1; g_write_short = g_write_over = 0;
    g_write_fail_partial = 0; g_read_avail = 0; g_hang = 0; g_now = 0;
    g_dll_present = 1; g_missing_symbol = 0; g_freelib = 0;
}
static int count(int id) { int c = 0; for (int i = 0; i < g_nlog; i++) if (g_log[i] == id) c++; return c; }

int main(void)
{
    static const int OPEN_SEQ[] = { C_OPEN, C_BAUD, C_DATA, C_FLOW, C_LAT, C_TMO, C_RESET,
                                    C_PURGE, C_RESET, C_CLRRTS, C_SETDTR, C_CLRDTR, C_PURGE };
    const int NOPEN = (int)(sizeof OPEN_SEQ / sizeof OPEN_SEQ[0]);
    uint8_t buf[16];
    printf("=== Windows D2XX transport fault test (fake module, virtual time; no hardware) ===\n");

    /* the missing-runtime cases come first and reset the cached module */
    fresh(); g_dll_present = 0; FT.dll = NULL;
    CHECK(mvci_io_open(NULL) == NULL && g_nlog == 0, "no ftd2xx.dll -> open fails");
    fresh(); g_missing_symbol = 1; FT.dll = NULL;
    CHECK(mvci_io_open(NULL) == NULL && g_freelib == 1 && g_nlog == 0 && FT.dll == NULL,
          "missing FT_Purge symbol -> open fails, module released");
    fresh(); g_missing_symbol = 2; FT.dll = NULL;
    CHECK(mvci_io_open(NULL) == NULL, "missing FT_Read symbol -> open fails");
    fresh(); g_missing_symbol = 3; FT.dll = NULL;
    CHECK(mvci_io_open(NULL) == NULL && g_nlog == 0, "missing FT_GetQueueStatus symbol -> open fails");

    /* success keeps the existing call order and the 15 ms / 1000 ms waits */
    fresh(); FT.dll = NULL;
    mvci_io_t *io = mvci_io_open("M-VCI");
    CHECK(io != NULL && g_handles == 1, "open succeeds");
    int same = (g_nlog == NOPEN);
    for (int i = 0; same && i < NOPEN; i++) same = (g_log[i] == OPEN_SEQ[i]);
    CHECK(same, "open call order unchanged");
    CHECK(g_nsleeps == 2 && g_sleeps[0] == 15 && g_sleeps[1] == 1000, "DTR pulse 15 ms then 1000 ms boot wait");
    mvci_io_close(io);
    CHECK(g_handles == 0 && count(C_CLOSE) == 1, "close releases the handle once");
    mvci_io_close(NULL);

    /* a failure at each step of open: NULL, handle released, no later calls */
    for (int k = 0; k < NOPEN; k++) {
        char name[96];
        fresh(); g_fail_index = k; g_handles = 0;
        io = mvci_io_open(NULL);
        snprintf(name, sizeof name, "open fails when %s (step %d) fails", CNAME[OPEN_SEQ[k]], k);
        CHECK(io == NULL, name);
        CHECK(g_handles == 0, "failed open leaves no D2XX handle open");
        CHECK(count(C_CLOSE) == (k == 0 ? 0 : 1), "Close only after a successful OpenEx");
        CHECK(g_nlog == k + 1 + (k == 0 ? 0 : 1), "no D2XX call after the failing step except Close");
        if (k <= 10) CHECK(g_nsleeps == 0, "no wait after a failure before the DTR pulse");
        if (k == 11) CHECK(g_nsleeps == 1 && g_sleeps[0] == 15, "ClrDtr failure skips the boot wait");
    }

    /* writes */
    fresh(); io = mvci_io_open(NULL); fresh();
    const uint8_t msg[3] = { 3, 0, 3 };
    CHECK(mvci_io_write(io, msg, 3) == 3 && g_wr_tmo == 1000, "legacy write returns the full count, 1000 ms");
    fresh(); g_fail_index = 0;
    CHECK(mvci_io_write(io, msg, 3) == -1, "write SetTimeouts failure -> -1");
    CHECK(count(C_WRITE) == 0, "no write after a failed SetTimeouts");
    fresh(); g_fail_index = 1;
    CHECK(mvci_io_write(io, msg, 3) == -1 && count(C_WRITE) == 1, "write status failure -> -1");
    fresh(); g_write_short = 1;
    CHECK(mvci_io_write(io, msg, 3) == -1 && count(C_WRITE) == 1, "short legacy write -> -1, never retried");
    fresh();
    CHECK(mvci_io_write(io, msg, 0) == 0 && mvci_io_write(io, NULL, 3) == -1 &&
          mvci_io_write(io, msg, -1) == -1 && mvci_io_write(NULL, msg, 3) == -1 && g_nlog == 0,
          "write argument errors never reach D2XX");

    /* timed write: the caller's budget is the D2XX write timeout, the read timeout is kept */
    fresh(); g_read_avail = 1; mvci_io_read(io, buf, 1, 321); fresh();
    CHECK(mvci_io_write_timeout(io, msg, 3, 50) == 3 && g_wr_tmo == 50 && g_rd_tmo == 321 &&
          g_last_write_req == 3 && g_now == 0 && count(C_WRITE) == 1,
          "timed write accepts all bytes with the caller's timeout, read timeout kept");
    fresh(); g_write_short = 1;
    CHECK(mvci_io_write_timeout(io, msg, 3, 50) == 2 && g_now == 50 && count(C_WRITE) == 1 && g_hang == 0,
          "timeout returns the partial accepted count after exactly the budget (virtual time), no retry");
    fresh(); g_fail_index = 1; g_write_fail_partial = 1;
    CHECK(mvci_io_write_timeout(io, msg, 3, 50) == -1 && count(C_WRITE) == 1,
          "status failure is -1 even with bytes accepted, never retried");
    fresh(); g_write_over = 1;
    CHECK(mvci_io_write_timeout(io, msg, 3, 50) == -1, "impossible count above n -> -1");
    fresh(); g_fail_index = 0; g_wr_tmo = 1234;
    CHECK(mvci_io_write_timeout(io, msg, 3, 50) == -1 && count(C_WRITE) == 0 && g_wr_tmo == 1234,
          "timed write SetTimeouts failure -> -1, no write");
    fresh(); g_write_short = 1;
    CHECK(mvci_io_write_timeout(io, msg, 3, 0) == 0 && g_nlog == 0 && g_now == 0 && g_hang == 0,
          "timeout 0 accepts nothing, never waits and never reaches D2XX");
    CHECK(mvci_io_write_timeout(io, msg, 0, 0) == 0 && mvci_io_write_timeout(io, msg, 3, -1) == -1 &&
          mvci_io_write_timeout(io, NULL, 3, 5) == -1 && mvci_io_write_timeout(NULL, msg, 3, 5) == -1 &&
          g_nlog == 0, "timed write argument errors never reach D2XX");
    fresh();
    CHECK(mvci_io_write_timeout(io, msg, 3, 0x7fffffff) == 3 && g_wr_tmo == 0x7fffffffu,
          "largest int timeout is passed unchanged");

    /* Timeout setup must not offer bytes after the caller budget expires. */
    fresh(); g_setup_ms = 7;
    CHECK(mvci_io_write_timeout(io, msg, 3, 5) == 0 && g_now == 7 && count(C_WRITE) == 0,
          "expired setup refuses a write before offering any bytes");
    fresh(); g_setup_ms = 2; g_write_short = 1;
    CHECK(mvci_io_write_timeout(io, msg, 3, 10) == 2 && g_now == 10 &&
          count(C_TMO) == 2 && count(C_WRITE) == 1,
          "setup and one partial write share the original deadline");
    fresh(); g_setup_ms = 7;
    CHECK(mvci_io_read(io, buf, 3, 5) == 0 && g_now == 7 && count(C_READ) == 0,
          "expired setup refuses a blocking read");
    fresh(); g_setup_ms = 2;
    CHECK(mvci_io_read(io, buf, 3, 10) == 0 && g_now == 10 && count(C_TMO) == 2 && count(C_READ) == 1,
          "setup and one timed read share the original deadline");

    /* reads */
    fresh(); g_read_avail = 16;
    CHECK(mvci_io_read(io, buf, 5, 250) == 5 && g_rd_tmo == 250 && g_now == 0 && g_hang == 0,
          "read returns requested bytes");
    fresh(); g_read_avail = 2;
    CHECK(mvci_io_read(io, buf, 5, 250) == 2 && g_now == 250, "partial read returns the count after the timeout");
    fresh(); g_read_avail = 0;
    CHECK(mvci_io_read(io, buf, 5, 250) == 0 && g_now == 250, "read timeout returns 0");
    fresh(); g_fail_index = 0;
    CHECK(mvci_io_read(io, buf, 5, 250) == -1 && count(C_READ) == 0, "SetTimeouts failure -> -1, no read");
    fresh(); g_fail_index = 1;
    CHECK(mvci_io_read(io, buf, 5, 250) == -1 && count(C_READ) == 1, "Read status failure -> -1");
    fresh();
    mvci_io_write_timeout(io, msg, 3, 77); fresh(); g_read_avail = 5;
    mvci_io_read(io, buf, 5, 40);
    CHECK(g_rd_tmo == 40 && g_wr_tmo == 77, "read keeps the last write timeout");
    fresh();
    CHECK(mvci_io_read(io, buf, 0, 10) == 0 && mvci_io_read(io, NULL, 1, 10) == -1 &&
          mvci_io_read(NULL, buf, 1, 10) == -1 && mvci_io_read(io, buf, 1, -7) == -1 && g_nlog == 0,
          "read argument errors (incl. negative timeout) never reach D2XX");

    /* timeout 0: drain the queue, never wait, never reach the undefined D2XX timeout 0 */
    fresh(); g_read_avail = 0;
    CHECK(mvci_io_read(io, buf, 5, 0) == 0 && g_nlog == 1 && count(C_QUEUE) == 1 && g_now == 0 && g_hang == 0,
          "empty queue at timeout 0 -> 0 with one queue query only");
    fresh(); g_read_avail = 16;
    CHECK(mvci_io_read(io, buf, 5, 0) == 5 && g_last_read_req == 5 && g_read_avail == 11 &&
          g_hang == 0 && g_now == 0 && count(C_TMO) == 0, "queue larger than n -> reads exactly n, no timeout call");
    fresh(); g_read_avail = 2;
    CHECK(mvci_io_read(io, buf, 5, 0) == 2 && g_last_read_req == 2 && g_hang == 0 && g_now == 0,
          "queue smaller than n -> reads only the queued bytes");
    fresh(); g_rd_tmo = 0; g_read_avail = 2;
    CHECK(mvci_io_read(io, buf, 5, 0) == 2 && g_hang == 0, "drain does not depend on the stored read timeout");
    g_rd_tmo = 250;
    fresh(); g_read_avail = 3; g_fail_index = 0;
    CHECK(mvci_io_read(io, buf, 5, 0) == -1 && count(C_READ) == 0 && g_hang == 0,
          "queue status failure -> -1, no read");
    fresh(); g_read_avail = 3; g_fail_index = 1;
    CHECK(mvci_io_read(io, buf, 5, 0) == -1 && count(C_READ) == 1 && g_hang == 0,
          "read status failure after the queue query -> -1");

    /* purge: void API, so a failure fails the next transfer */
    fresh();
    mvci_io_purge_rx(io);
    CHECK(count(C_PURGE) == 1 && mvci_io_write(io, msg, 3) == 3, "successful purge does not disturb the link");
    fresh(); g_fail_index = 0;
    mvci_io_purge_rx(io);
    fresh(); g_read_avail = 4;
    CHECK(mvci_io_read(io, buf, 4, 10) == -1 && mvci_io_write(io, msg, 3) == -1 && g_nlog == 0,
          "failed purge makes the next read and write fail without touching D2XX");
    fresh();
    mvci_io_purge_rx(io);                       /* a later good purge resynchronizes */
    CHECK(mvci_io_write(io, msg, 3) == 3, "successful purge clears an earlier purge failure");
    mvci_io_purge_rx(NULL);
    mvci_io_close(io);
    CHECK(g_handles == 0, "no handle left at the end");

    printf("%d passed, %d failed, 0 skipped\n", g_pass, g_fail);
    return g_fail != 0;
}

/* win_io_fault_test.c — status handling of the Windows FTDI D2XX transport
 * (the _WIN32 half of src/io.c), run on any host.
 *
 * src/io.c is compiled into this file as _WIN32, against a fake <windows.h>
 * and a scripted ftd2xx module (test/fake/win/windows.h). Time is virtual:
 * Sleep() only adds to a counter. This proves the call order of the open
 * sequence is unchanged, that every D2XX failure reaches the caller, and that
 * a failed open releases its handle. It does NOT prove D2XX or Windows
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
       C_SETDTR, C_CLRDTR, C_WRITE, C_READ, C_CLOSE, C_NCALLS };
static const char *const CNAME[C_NCALLS] = {
    "OpenEx","SetBaudRate","SetDataCharacteristics","SetFlowControl","SetLatencyTimer",
    "SetTimeouts","ResetDevice","Purge","ClrRts","SetDtr","ClrDtr","Write","Read","Close" };

static int  g_log[64], g_nlog;
static int  g_fail_index;            /* fail the Nth non-Close call; -1 = never */
static int  g_issued;                /* non-Close calls so far */
static int  g_handles;               /* opened minus closed */
static int  g_dll_present, g_missing_symbol, g_freelib;
static ULONG g_last_read_timeout;
static int  g_write_short, g_read_avail;

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
static FT_STATUS WINAPI f_tmo(FT_HANDLE h, ULONG r, ULONG w) { (void)h; (void)w; g_last_read_timeout = r; return step(C_TMO); }
static FT_STATUS WINAPI f_data(FT_HANDLE h, UCHAR w, UCHAR s, UCHAR p)
{ (void)h; return (w == 8 && s == 0 && p == 0) ? step(C_DATA) : FT_IO_ERROR; }
static FT_STATUS WINAPI f_flow(FT_HANDLE h, USHORT f, UCHAR a, UCHAR b)
{ (void)h; return (f == 0 && a == 0 && b == 0) ? step(C_FLOW) : FT_IO_ERROR; }
static FT_STATUS WINAPI f_lat(FT_HANDLE h, UCHAR t) { (void)h; return t == 2 ? step(C_LAT) : FT_IO_ERROR; }
static FT_STATUS WINAPI f_write(FT_HANDLE h, LPVOID b, DWORD n, LPDWORD w)
{ (void)h; (void)b; FT_STATUS s = step(C_WRITE); *w = g_write_short ? n - 1 : n; return s; }
static FT_STATUS WINAPI f_read(FT_HANDLE h, LPVOID b, DWORD n, LPDWORD g)
{
    (void)h; FT_STATUS s = step(C_READ);
    DWORD k = (DWORD)g_read_avail < n ? (DWORD)g_read_avail : n;
    memset(b, 0x5a, k); *g = k; return s;
}

static const struct { const char *name; FARPROC fn; } SYMS[] = {
    {"FT_OpenEx",(FARPROC)f_open},{"FT_Close",(FARPROC)f_close},{"FT_ResetDevice",(FARPROC)f_reset},
    {"FT_ClrRts",(FARPROC)f_clrrts},{"FT_SetDtr",(FARPROC)f_setdtr},{"FT_ClrDtr",(FARPROC)f_clrdtr},
    {"FT_SetBaudRate",(FARPROC)f_baud},{"FT_Purge",(FARPROC)f_purge},{"FT_SetTimeouts",(FARPROC)f_tmo},
    {"FT_SetDataCharacteristics",(FARPROC)f_data},{"FT_SetFlowControl",(FARPROC)f_flow},
    {"FT_SetLatencyTimer",(FARPROC)f_lat},{"FT_Write",(FARPROC)f_write},{"FT_Read",(FARPROC)f_read},
};

HMODULE LoadLibraryA(LPCSTR n) { return (g_dll_present && !strcmp(n, "ftd2xx.dll")) ? (HMODULE)&g_dll_present : NULL; }
int     FreeLibrary(HMODULE m) { (void)m; g_freelib++; return 1; }
FARPROC GetProcAddress(HMODULE m, LPCSTR n)
{
    (void)m;
    if (g_missing_symbol && !strcmp(n, g_missing_symbol == 1 ? "FT_Purge" : "FT_Read")) return NULL;
    for (size_t i = 0; i < sizeof SYMS / sizeof SYMS[0]; i++)
        if (!strcmp(n, SYMS[i].name)) return SYMS[i].fn;
    return NULL;
}

static void fresh(void)
{
    g_nlog = g_issued = g_nsleeps = 0; g_fail_index = -1; g_write_short = 0; g_read_avail = 0;
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

    /* transfers */
    fresh(); io = mvci_io_open(NULL); fresh();
    const uint8_t msg[3] = { 3, 0, 3 };
    CHECK(mvci_io_write(io, msg, 3) == 3, "write returns the full count");
    g_fail_index = 0; g_issued = 0;
    CHECK(mvci_io_write(io, msg, 3) == -1, "write status failure -> -1");
    fresh(); g_write_short = 1;
    CHECK(mvci_io_write(io, msg, 3) == -1, "short write (D2XX timeout) -> -1");
    fresh();
    CHECK(mvci_io_write(io, msg, 0) == 0 && mvci_io_write(io, NULL, 3) == -1 &&
          mvci_io_write(io, msg, -1) == -1 && mvci_io_write(NULL, msg, 3) == -1 && g_nlog == 0,
          "write argument errors never reach D2XX");

    fresh(); g_read_avail = 16;
    CHECK(mvci_io_read(io, buf, 5, 250) == 5 && g_last_read_timeout == 250, "read returns requested bytes");
    fresh(); g_read_avail = 2;
    CHECK(mvci_io_read(io, buf, 5, 250) == 2, "partial read returns the byte count");
    fresh(); g_read_avail = 0;
    CHECK(mvci_io_read(io, buf, 5, 250) == 0, "read timeout returns 0");
    fresh(); g_fail_index = 0;
    CHECK(mvci_io_read(io, buf, 5, 250) == -1 && count(C_READ) == 0, "SetTimeouts failure -> -1, no read");
    fresh(); g_fail_index = 1;
    CHECK(mvci_io_read(io, buf, 5, 250) == -1 && count(C_READ) == 1, "Read status failure -> -1");
    fresh(); g_read_avail = 1;
    mvci_io_read(io, buf, 1, 0);
    CHECK(g_last_read_timeout == 1, "timeout 0 is passed as 1 ms (D2XX 0 may block forever)");
    mvci_io_read(io, buf, 1, -7);
    CHECK(g_last_read_timeout == 1, "negative timeout is passed as 1 ms");
    fresh();
    CHECK(mvci_io_read(io, buf, 0, 10) == 0 && mvci_io_read(io, NULL, 1, 10) == -1 &&
          mvci_io_read(NULL, buf, 1, 10) == -1 && g_nlog == 0, "read argument errors never reach D2XX");

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

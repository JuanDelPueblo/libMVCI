/* Exported API -> real session -> shipped Windows transport -> fake D2XX.
 * Host POSIX locks/OpenSSL only; no installed provider or OS runtime. */
#ifdef NDEBUG
#undef NDEBUG
#endif
#define _WIN32
#define main win_suite_main
#include "win_io_fault_test.c"
#undef main
#undef _WIN32
#include <mvci/j2534.h>
#include <assert.h>
#include <time.h>
typedef struct cable_io cable_io_t;
#define mvci_io cable_io
#define mvci_io_t cable_io_t
#define mvci_io_open cable_open
#define mvci_io_close cable_close
#define mvci_io_write cable_write
#define mvci_io_write_timeout cable_write_timeout
#define mvci_io_read cable_read
#define mvci_io_purge_rx cable_purge
#define mvci_io_purge_rx_checked cable_purge_checked
#include "mock_io.c"
#undef mvci_io
#undef mvci_io_t
#undef mvci_io_open
#undef mvci_io_close
#undef mvci_io_write
#undef mvci_io_write_timeout
#undef mvci_io_read
#undef mvci_io_purge_rx
#undef mvci_io_purge_rx_checked
static ULONG setup_ms, offered_at;
int __wrap_clock_gettime(clockid_t c,struct timespec *t)
{(void)c;t->tv_sec=g_now/1000;t->tv_nsec=(g_now%1000)*1000000;return 0;}
int __wrap_nanosleep(const struct timespec *t,struct timespec *r)
{(void)r;g_now+=(ULONG)(t->tv_sec*1000+t->tv_nsec/1000000);return 0;}
static FT_STATUS WINAPI cable_openex(PVOID d,DWORD flags,FT_HANDLE *h)
{cable_open("mock");return f_open(d,flags,h);}
static FT_STATUS WINAPI cable_timeouts(FT_HANDLE h,ULONG r,ULONG w)
{FT_STATUS rc=f_tmo(h,r,w);g_now+=setup_ms;return rc;}
static FT_STATUS WINAPI cable_tx(FT_HANDLE h,LPVOID b,DWORD n,LPDWORD w)
{(void)h;offered_at=g_now;*w=cable_write(&g_io,b,(int)n);return FT_OK;}
static FT_STATUS WINAPI cable_rx(FT_HANDLE h,LPVOID b,DWORD n,LPDWORD got)
{(void)h;*got=cable_read(&g_io,b,(int)n,0);if(*got<n)g_now+=g_rd_tmo;return FT_OK;}
static FT_STATUS WINAPI cable_queue(FT_HANDLE h,LPDWORD q)
{(void)h;*q=g_qtail-g_qhead;return FT_OK;}
static FT_STATUS WINAPI cable_flush(FT_HANDLE h,ULONG f)
{(void)h;(void)f;cable_purge(&g_io);return FT_OK;}
int main(void)
{
 fresh();FT.dll=NULL;assert(ft_load()==0);
 FT.OpenEx=cable_openex;FT.SetTimeouts=cable_timeouts;
 FT.Write=cable_tx;FT.Read=cable_rx;FT.GetQueueStatus=cable_queue;FT.Purge=cable_flush;
 J2534_ULONG dev=0,ch=0,count=1;
 assert(PassThruOpen((void*)"script-only",&dev)==0);
 assert(PassThruConnect(dev,ISO9141,0,10400,&ch)==0);
 PASSTHRU_MSG msg={0};msg.ProtocolID=ISO9141;msg.DataSize=5;
 memcpy(msg.Data,"\x68\x6a\xf1\x01\x00",5);
 setup_ms=7;offered_at=UINT32_MAX;ULONG began=g_now;int before=mock_frame_count();
 long rc=PassThruWriteMsgs(ch,&msg,&count,5);
 printf("public_WriteMsgs budget=5 setup=7 rc=%ld count=%u elapsed=%u offered_at=%u frames=%d\n",rc,count,g_now-began,offered_at-began,mock_frame_count()-before);
 assert(rc==ERR_TIMEOUT && count==0 && offered_at==UINT32_MAX && mock_frame_count()==before);
 count=1;before=mock_frame_count();
 assert(PassThruWriteMsgs(ch,&msg,&count,5)==ERR_FAILED && count==0 && mock_frame_count()==before);
 setup_ms=0;assert(PassThruClose(dev)==0);
 puts("Windows public write: expired setup sends no command, preserves count zero and refuses retry; skips=0");return 0;
}

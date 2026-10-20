/* Host-side stand-in for <windows.h>: only the types and calls that src/io.c,
 * src/des.c and src/compat.h use, so their Windows paths build and run on a
 * Linux host. It checks how the driver handles status values. It does not
 * prove how Windows behaves. */
#ifndef FAKE_WINDOWS_H
#define FAKE_WINDOWS_H

#include <stddef.h>
#include <stdint.h>
#include <wchar.h>

#define WINAPI
#define __stdcall
#define INFINITE 0xFFFFFFFFu

typedef void     *PVOID, *LPVOID, *HANDLE, *HMODULE;
typedef uint32_t  ULONG, DWORD, *LPDWORD;
typedef uint16_t  USHORT;
typedef uint8_t   UCHAR, *PUCHAR;
typedef wchar_t   WCHAR;
typedef int32_t   NTSTATUS;
typedef int       CRITICAL_SECTION;
typedef int       CONDITION_VARIABLE;
typedef unsigned long (*LPTHREAD_START_ROUTINE)(void *);
typedef void *LPSECURITY_ATTRIBUTES;
typedef size_t SIZE_T;
typedef const char *LPCSTR;
typedef void     *FARPROC;

/* supplied by the test (virtual time, scripted module) */
void      Sleep(DWORD ms);
DWORD     GetTickCount(void);
HMODULE   LoadLibraryA(LPCSTR name);
FARPROC   GetProcAddress(HMODULE m, LPCSTR name);
int       FreeLibrary(HMODULE m);

/* threads are never started by these tests */
static inline HANDLE CreateThread(LPSECURITY_ATTRIBUTES a, SIZE_T s, LPTHREAD_START_ROUTINE f,
                                  LPVOID arg, DWORD fl, LPDWORD id)
{ (void)a; (void)s; (void)f; (void)arg; (void)fl; (void)id; return NULL; }
static inline DWORD WaitForSingleObject(HANDLE h, DWORD ms) { (void)h; (void)ms; return 0; }
static inline int   CloseHandle(HANDLE h) { (void)h; return 1; }

static inline void InitializeCriticalSection(CRITICAL_SECTION *c) { (void)c; }
static inline void EnterCriticalSection(CRITICAL_SECTION *c)      { (void)c; }
static inline void LeaveCriticalSection(CRITICAL_SECTION *c)      { (void)c; }
static inline void DeleteCriticalSection(CRITICAL_SECTION *c)     { (void)c; }

/* Declared only so unused compat wrappers compile. These single-threaded
 * backend fakes do not implement or exercise Windows condition variables. A
 * test that calls one must supply its model, or fail to link. */
void InitializeConditionVariable(CONDITION_VARIABLE *c);
int SleepConditionVariableCS(CONDITION_VARIABLE *c, CRITICAL_SECTION *m, DWORD ms);
void WakeAllConditionVariable(CONDITION_VARIABLE *c);

static inline PVOID InterlockedCompareExchangePointer(PVOID volatile *dst, PVOID xchg, PVOID cmp)
{ return __sync_val_compare_and_swap(dst, cmp, xchg); }

#endif

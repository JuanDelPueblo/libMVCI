/* Public J2534 diagnostics contract tests. Requires only the scripted I/O. */
#include <mvci/j2534.h>

#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static int failures;

static void check(int condition, const char *name)
{
    printf("[%s] %s\n", condition ? "PASS" : "FAIL", name);
    if (!condition) failures++;
}

static void *thread_error(void *unused)
{
    (void)unused;
    char error[80];
    int good = PassThruReadVersion(0, error, error, error) == ERR_INVALID_DEVICE_ID &&
               PassThruGetLastError(error) == STATUS_NOERROR &&
               strcmp(error, "invalid device id") == 0;
    return (void *)(intptr_t)good;
}

int main(void)
{
    J2534_ULONG device = 0;
    check(PassThruOpen((void *)"mock", &device) == STATUS_NOERROR,
          "open scripted device");

    char firmware[80], library[80], api[80];
    memset(firmware, 0xa5, sizeof firmware);
    memset(library, 0xa5, sizeof library);
    memset(api, 0xa5, sizeof api);
    check(PassThruReadVersion(device, firmware, library, api) == STATUS_NOERROR,
          "version succeeds with all outputs");
    check(strcmp(firmware, "Unknown (not queried)") == 0,
          "firmware identity remains explicitly unknown");
    check(strcmp(library, "libMVCI 1.0.0") == 0,
          "library reports its own build identity");
    check(strcmp(api, "04.04") == 0, "API family version remains 04.04");

    char error[80];
    check(PassThruReadVersion(device, NULL, library, api) == ERR_NULL_PARAMETER,
          "missing version output fails");
    check(PassThruGetLastError(error) == STATUS_NOERROR &&
          strcmp(error, "NULL version output") == 0,
          "version failure reports accurate text");
    check(PassThruReadVersion(0, firmware, library, api) == ERR_INVALID_DEVICE_ID,
          "invalid device version request fails");
    check(PassThruGetLastError(error) == STATUS_NOERROR &&
          strcmp(error, "invalid device id") == 0,
          "invalid device failure replaces stale text");

    pthread_t thread;
    int created = pthread_create(&thread, NULL, thread_error, NULL) == 0;
    check(created, "create diagnostics isolation thread");
    if (created) {
        void *thread_result = NULL;
        pthread_join(thread, &thread_result);
        check((intptr_t)thread_result == 1, "thread reads its own last error");
    }
    check(PassThruGetLastError(error) == STATUS_NOERROR &&
          strcmp(error, "invalid device id") == 0,
          "other thread does not overwrite caller error");

    check(PassThruClose(device) == STATUS_NOERROR, "close scripted device");
    return failures ? 1 : 0;
}

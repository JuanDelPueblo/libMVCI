/* Internal additive deadline/status API. Legacy serial.h calls remain valid. */
#ifndef MVCI_PIPELINE_H
#define MVCI_PIPELINE_H
#include <mvci/serial.h>
#define MVCI_PIPE_TIMEOUT (-10)
#define MVCI_PIPE_IO (-11)
#define MVCI_PIPE_OVERFLOW (-12)
#define MVCI_PIPE_DESYNC (-13)
#define MVCI_PIPE_REJECTED (-14)
#define MVCI_PIPE_MAX_DATA 64
int mvci_write_msg_timeout(mvci_ctx_t *, const uint8_t *, size_t, int timeout_ms);
#endif

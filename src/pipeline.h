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
/* Host work limit, not a cable timing claim. Count empty statuses and
 * suppressed echoes against the same public nonblocking read budget. */
#define MVCI_PIPE_READ_BUDGET 32u
int mvci_poll_until(mvci_ctx_t *, uint8_t *, size_t, uint8_t *,
                    uint32_t deadline, int timed, unsigned *work_left);
int mvci_write_msg_timeout(mvci_ctx_t *, const uint8_t *, size_t, int timeout_ms);
#endif

/* Internal additive deadline/status API. Legacy serial.h calls remain valid. */
#ifndef MVCI_PIPELINE_H
#define MVCI_PIPELINE_H
#include <mvci/serial.h>
#define MVCI_PIPE_TIMEOUT (-10)
#define MVCI_PIPE_IO (-11)
#define MVCI_PIPE_OVERFLOW (-12)
#define MVCI_PIPE_DESYNC (-13)
#define MVCI_PIPE_REJECTED (-14)
#define MVCI_PIPE_CANCELED (-15)
#define MVCI_PIPE_MAX_DATA 64
/* Host work limit, not a cable timing claim. Count empty statuses and
 * suppressed echoes against the same public nonblocking read budget. */
#define MVCI_PIPE_READ_BUDGET 32u
int mvci_poll_until(mvci_ctx_t *, uint8_t *, size_t, uint8_t *,
                    uint32_t deadline, int timed, unsigned *work_left);
/* Cancel the read poll that is running or about to run on ctx. A waiting
 * Close/Disconnect sets it so a long read gives way. The poll stops at its next
 * transport slice and returns MVCI_PIPE_CANCELED. Only mvci_poll_until
 * honours it: a filter, IOCTL or write transaction is never cut short. A poll
 * canceled after its command was sent leaves an unknown result: the context
 * is poisoned, as for any lost reply. The caller clears the flag when the
 * Close/Disconnect no longer waits. */
void mvci_set_read_cancel(mvci_ctx_t *, int on);
/* Start a new session on a context with no outstanding command. Checked host
 * RX purge, then drop queued, partial and overflowed receive state. Sends
 * nothing. MVCI_PIPE_DESYNC if the context is poisoned (the unknown result
 * stays: only Close/Open recovers); MVCI_PIPE_IO if the purge fails (the
 * context then refuses I/O until a later purge succeeds). Returns 0. */
int mvci_reset_session(mvci_ctx_t *);
int mvci_write_msg_timeout(mvci_ctx_t *, const uint8_t *, size_t, int timeout_ms);
#endif

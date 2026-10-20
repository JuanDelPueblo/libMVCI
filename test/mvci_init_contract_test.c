/* Public init IOCTL contracts against the scripted in-memory transport. */
#include <mvci/j2534.h>
#include <stdio.h>
#include <string.h>
void mock_five_baud_success(int enabled);
void mock_fast_reply_length(int length);
int mock_frame_count(void);
static int failures;
#define CHECK(expr) do { if (!(expr)) { fprintf(stderr, "FAIL: %s\n", #expr); failures++; } } while (0)
int main(void)
{
    J2534_ULONG dev = 0, ch = 0;
    CHECK(PassThruOpen((void *)"mock", &dev) == STATUS_NOERROR);
    CHECK(PassThruConnect(dev, ISO9141, 0, 10400, &ch) == STATUS_NOERROR);
    uint8_t addr = 0x33, key[2] = {0xA5, 0xA5};
    SBYTE_ARRAY in = {1, &addr}, out = {1, key};
    mock_five_baud_success(1);
    int before = mock_frame_count();
    CHECK(PassThruIoctl(ch, FIVE_BAUD_INIT, &in, &out) == ERR_FAILED);
    CHECK(out.NumOfBytes == 2 && key[0] == 0xA5 && key[1] == 0xA5);
    CHECK(mock_frame_count() == before);
    CHECK(PassThruIoctl(ch, FIVE_BAUD_INIT, &in, NULL) == ERR_NULL_PARAMETER);
    CHECK(mock_frame_count() == before);
    out.NumOfBytes = 2;
    CHECK(PassThruIoctl(ch, FIVE_BAUD_INIT, &in, &out) == STATUS_NOERROR);
    CHECK(out.NumOfBytes == 2 && key[0] == 0x08 && key[1] == 0x08);

    PASSTHRU_MSG fi = {0}, fo = {0};
    fi.DataSize = 1; fi.Data[0] = 0x33;
    mock_fast_reply_length(40);
    CHECK(PassThruIoctl(ch, FAST_INIT, &fi, &fo) == STATUS_NOERROR);
    CHECK(fo.DataSize == 40 && fo.Data[0] == 0x80 && fo.Data[39] == (uint8_t)(0x80 + 39));
    CHECK(fo.ExtraDataIndex == fo.DataSize);
    mock_fast_reply_length(0);
    fo.Data[0] = 0xA5;
    CHECK(PassThruIoctl(ch, FAST_INIT, &fi, &fo) == ERR_FAILED);
    CHECK(fo.Data[0] == 0xA5);
    memset(&fi, 0, sizeof fi);
    before = mock_frame_count();
    CHECK(PassThruIoctl(ch, FAST_INIT, &fi, &fo) == ERR_NOT_SUPPORTED);
    CHECK(PassThruIoctl(ch, FAST_INIT, NULL, &fo) == ERR_NOT_SUPPORTED);
    CHECK(mock_frame_count() == before);
    CHECK(PassThruDisconnect(ch) == STATUS_NOERROR);
    CHECK(PassThruClose(dev) == STATUS_NOERROR);
    return failures ? 1 : 0;
}

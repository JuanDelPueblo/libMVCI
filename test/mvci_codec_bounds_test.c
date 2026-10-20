/* Focused offline regression checks for codec pointer, length and wire bounds. */
#include <mvci/serial.h>

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define TEST_MAX_WIRE_CIPHER (((UINT8_MAX - 3u) / 8u) * 8u)

static unsigned failures;

static void check(int ok, const char *name)
{
    if (!ok) {
        fprintf(stderr, "FAIL: %s\n", name);
        failures++;
    }
}

int main(void)
{
    static const uint8_t key[8] = {0xb0,0xcb,0x49,0x68,0x07,0x45,0xc8,0x7f};
    uint8_t frame[MVCI_MAX_FRAME], inner[MVCI_MAX_INNER];
    uint8_t in[UINT8_MAX - 3u];
    memset(frame, 0xa5, sizeof frame);
    memset(inner, 0x5a, sizeof inner);
    memset(in, 0x3c, sizeof in);

    check(mvci_frame_plain(NULL, 0, frame, sizeof frame) == 3,
          "empty plain frame remains supported");
    check(mvci_frame_plain(NULL, 1, frame, sizeof frame) < 0,
          "plain frame refuses null nonempty payload");
    check(mvci_frame_plain(in, SIZE_MAX, frame, sizeof frame) < 0,
          "plain frame rejects size_t overflow before addition");
    check(mvci_frame_plain(in, 253, frame, sizeof frame) < 0,
          "plain frame respects one-byte total-length limit");
    check(mvci_frame_plain(in, 252, frame, sizeof frame) == 255,
          "plain frame accepts largest representable payload");

    check(mvci_frame_enc(NULL, in, 1, frame, sizeof frame) < 0,
          "encrypted frame refuses null key");
    check(mvci_frame_enc(key, NULL, 1, frame, sizeof frame) < 0,
          "encrypted frame refuses null nonempty input");
    check(mvci_frame_enc(key, in, 1, NULL, sizeof frame) < 0,
          "encrypted frame refuses null output");
    check(mvci_frame_enc(key, in, 0, frame, sizeof frame) < 0,
          "encrypted frame refuses empty inner body");
    check(mvci_frame_enc(key, in, SIZE_MAX, frame, sizeof frame) < 0,
          "encrypted frame rejects huge length without arithmetic wrap");
    check(mvci_frame_enc(key, in, TEST_MAX_WIRE_CIPHER, frame, sizeof frame) == 251,
          "largest DES body fits the one-byte wire frame");
    check(mvci_frame_enc(key, in, TEST_MAX_WIRE_CIPHER + 1u, frame, sizeof frame) < 0,
          "DES padding cannot exceed the one-byte wire frame");

    const uint8_t *payload = NULL;
    check(mvci_frame_payload(NULL, 3, &payload) < 0,
          "frame parser refuses null frame");
    check(mvci_frame_payload(frame, (size_t)UINT8_MAX + 1u, &payload) < 0,
          "frame parser refuses lengths outside the wire field");
    check(mvci_frame_decrypt(NULL, frame, 3, inner, sizeof inner) < 0,
          "decrypt refuses null key");
    check(mvci_frame_decrypt(key, NULL, 3, inner, sizeof inner) < 0,
          "decrypt refuses null frame");
    check(mvci_frame_decrypt(key, frame, 3, NULL, sizeof inner) < 0,
          "decrypt refuses null output");

    check(mvci_inner_fast_init(3, NULL, 1, inner) < 0,
          "fast init refuses null nonempty payload");
    check(mvci_inner_fast_init(3, in, SIZE_MAX, inner) < 0,
          "fast init rejects huge payload length");
    check(mvci_inner_fast_init(3, in, TEST_MAX_WIRE_CIPHER - 8u, inner) ==
              (int)TEST_MAX_WIRE_CIPHER,
          "fast init maximum produces an encodable inner body");
    check(mvci_inner_fast_init(3, in, TEST_MAX_WIRE_CIPHER - 7u, inner) < 0,
          "fast init refuses first unencodable inner size");
    check(mvci_inner_five_baud_init(3, in, SIZE_MAX, inner) < 0,
          "five baud init rejects huge payload length");
    check(mvci_inner_five_baud_init(3, in, TEST_MAX_WIRE_CIPHER - 8u, inner) ==
              (int)TEST_MAX_WIRE_CIPHER,
          "five baud maximum produces an encodable inner body");
    check(mvci_inner_write_msg(3, NULL, 1, inner) < 0,
          "write builder refuses null nonempty message");
    check(mvci_inner_write_msg(3, in, SIZE_MAX, inner) < 0,
          "write builder rejects huge message length");
    check(mvci_inner_write_msg(3, in, TEST_MAX_WIRE_CIPHER - 11u, inner) ==
              (int)TEST_MAX_WIRE_CIPHER,
          "write maximum produces an encodable inner body");
    check(mvci_inner_write_msg(3, in, TEST_MAX_WIRE_CIPHER - 10u, inner) < 0,
          "write builder refuses first unencodable inner size");

    check(mvci_inner_set_config(3, 1, 2, NULL) < 0 &&
          mvci_inner_connect(3, 0, 10400, NULL) < 0 &&
          mvci_inner_start_filter(3, 1, 1, 0, 0, NULL) < 0 &&
          mvci_inner_clear_periodic(3, NULL) < 0 &&
          mvci_inner_prog_voltage(15, 0xfffffffeu, NULL) < 0 &&
          mvci_inner_stop_filter(3, 1, NULL) < 0 &&
          mvci_inner_read_poll(3, NULL) < 0,
          "fixed-size builders refuse null output buffers");

    static const uint8_t truncated[] = {0x0f,0,0x09,0};
    static const uint8_t reply[] = {0x0f,0,0x09,0,0,0,0,0,0,0,0,0x83,0xf0,0x19,0x41,0x0d,0};
    uint8_t msg[8], rx = 0xff;
    memset(msg, 0xa5, sizeof msg);
    check(mvci_parse_read_reply(NULL, 16, msg, sizeof msg, &rx) < 0,
          "read parser refuses null inner body");
    check(mvci_parse_read_reply(truncated, sizeof truncated, msg, sizeof msg, &rx) < 0,
          "read parser refuses truncated message body");
    check(rx == 0xff && msg[0] == 0xa5,
          "truncated read reply leaves caller outputs untouched");
    check(mvci_parse_read_reply(reply, sizeof reply, NULL, sizeof msg, &rx) < 0,
          "read parser refuses null message destination");
    check(rx == 0xff,
          "read parser does not publish status for malformed message");
    check(mvci_parse_read_reply(reply, sizeof reply, msg, 5, &rx) < 0,
          "read parser refuses output truncation");
    check(mvci_parse_read_reply(reply, sizeof reply, msg, sizeof msg, &rx) == 6 &&
          rx == 0 && memcmp(msg, reply + 11, 6) == 0,
          "valid read reply preserves message and status");

    if (failures) {
        fprintf(stderr, "%u codec bound check(s) failed\n", failures);
        return 1;
    }
    puts("codec bounds: all checks passed (0 skips)");
    return 0;
}

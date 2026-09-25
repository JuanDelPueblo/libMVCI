# MVCI Wire Protocol Specification

This document describes the serial protocol spoken by the FTDI‑based "M‑VCI"
(Mini‑VCI) diagnostic cable, as implemented by `libMVCI`. It is provided for
interoperability and documentation purposes.

All multi‑byte integers are **little‑endian** unless noted. Byte values are
hexadecimal. "Inner" refers to a decrypted command/response body; "frame" refers
to the bytes actually on the wire.

---

## 1. Link layer

- USB: FTDI FT232R, VID `0403` / PID `6001`, USB product description `M-VCI`.
- Serial line: **115200 baud, 8 data bits, no parity, 1 stop bit**, no flow control.
- **MCU reset on open:** clear RTS, set DTR, wait 15 ms, clear DTR; then wait
  ~1 s for the adapter to boot before the first frame.

## 2. Frame format

Every frame, in both directions, has the form:

```
+-------+-------+---------------------------+--------+
| LEN   | 0x00  | PAYLOAD (LEN-3 bytes)     | XORSUM |
+-------+-------+---------------------------+--------+
  u8      u8      LEN-3 bytes                 u8
```

- **LEN** — total frame length in bytes, counting all four parts
  (`LEN = 3 + len(PAYLOAD)`). Maximum observed frame is well under 255 bytes.
- **0x00** — reserved, always zero.
- **PAYLOAD** — see §4 (plaintext for the handshake, otherwise DES‑encrypted).
- **XORSUM** — exclusive‑OR of every byte that precedes it (i.e. of `LEN`,
  `0x00`, and all `PAYLOAD` bytes).

### 2.1 Checksum rule

`XORSUM` is computed over the bytes as they appear on the wire — that is, over
the **payload as transmitted**:

- Frames the host sends, and device **status** replies (§6.1), checksum over the
  (encrypted) payload.
- Device **message** replies (§6.2) checksum over the **decrypted** inner body.

A robust receiver accepts a frame if `XORSUM` matches *either* the encrypted
payload or the decrypted inner. (`libMVCI` decrypts, then accepts if either rule
holds.)

## 3. Session establishment (handshake)

The handshake frames are **plaintext** (no encryption). Immediately after the
link‑layer reset (§1):

| Step | Direction | Frame | Notes |
|------|-----------|-------|-------|
| 1 | host → dev | `03 00 03` | Reset. No payload. Wait ~110 ms afterwards. |
| 2 | host → dev | `0C 00 07 00 01 4D 56 43 49 2D 54 62` | Identify. Payload = `07 00 01` + ASCII `"MVCI-T"`. |
| 3 | dev → host | `0E 00 09 00 01 K0 K1 K2 K3 K4 K5 K6 K7 cs` | Challenge. Payload = `09 00 01` + 8 key bytes. |

The **DES key for the rest of the session is `K0..K7`** — the 8 bytes following
`09 00 01` in the challenge reply (the trailing byte `cs` is the frame `XORSUM`).

The challenge is a per‑connection nonce: it advances on each handshake and resets
to a fixed starting value when the adapter is power‑cycled. The key is always
exactly the challenge bytes the device just sent, so no derivation is required.

## 4. Encryption

After the handshake, the `PAYLOAD` of every frame is:

```
PAYLOAD = DES-ECB( key, zero-pad(INNER, 8) )
```

- Cipher: **single DES in ECB mode** (FIPS 46‑3), standard S‑boxes, 8‑byte
  blocks, key parity bits ignored. Any standard DES implementation interoperates.
- `key` is the 8 bytes from the handshake (§3).
- `INNER` (the logical command/response, §5) is zero‑padded up to a multiple of
  8 bytes before encryption; the resulting ciphertext length is the payload
  length.

Decryption of received frames is the same operation in reverse
(`DES-ECB-decrypt`).

## 5. Inner command format

A decrypted inner body has the envelope:

```
+-------+-------+-------+----------------------+
| ILEN  | 0x00  | CMD   | ARGS ...             |
+-------+-------+-------+----------------------+
  u8      u8      u8      (ILEN-1) bytes
```

- **ILEN** — length of `CMD` + `ARGS` (i.e. `ILEN = 1 + len(ARGS)`).
- **CMD** — command opcode (§5.1).
- The full inner body is `ILEN + 2` bytes, then zero‑padded to 8 for encryption.

Some opcodes carry a **sub‑function** in the first argument byte (notably
`CMD 0x0E` and `CMD 0x09`). The `[proto u32]` field is the connected J2534
**protocol id** (ISO9141 = 3, ISO14230 = 4). The host must send the id of the
open channel. It must not hardcode one id.

### 5.1 Command reference (host → device)

Byte templates below are the complete inner bodies (before padding). `[..]`
denotes a little‑endian field.

| Function | Opcode | Inner template | Reply (§6) |
|----------|--------|----------------|------------|
| Connect | `07` | `0D 00 07` `[proto u32]` `[flags u32]` `[baud u32]` | status `02 00 07` |
| Start message filter | `0B` | `10 00 0B` `[proto u32]` `[msgID u32]` `[type u32]` `mask(1)` `pattern(1)` | status `02 00 0B` |
| Set config (one param) | `0E`/`02` | `0E 00 0E 02` `[proto u32]` `[param u32]` `[value u32]` | status `02 00 0E` |
| Clear periodic msgs | `0E`/`09` | `06 00 0E 09` `[proto u32]` | status `02 00 0E` |
| Fast init | `0E`/`05` | `ILEN 00 0E 05` `[proto u32]` `init bytes…` | message reply (ECU key bytes), or status §6.3 |
| Five-baud init | `0E`/`04` | `ILEN 00 0E 04` `[proto u32]` `address bytes…` | status §6.3; success wire shape unobserved |
| Prog voltage | `0D` | `09 00 0D` `[pin u32]` `[voltage u32]` | status `02 00 0D` |
| Write message | `0A` | `0E 00 0A` `[proto u32]` `[flags u32]` `msg bytes…` | status `02 00 0A` |
| Read poll | `09`/`04` | `05 00 09 04 00 00 00 00` | message reply, or empty status |
| Keepalive | `09`/`06` | `05 00 09 06 00 00 00 00` | status `02 00 09` |
| Disconnect | `02` | `01 00 02 00 00 00 00 00` | status `02 00 02` |

Notes:
- **Filter `type`** values follow J2534: `1` = PASS, `2` = BLOCK, `3` = FLOW_CONTROL.
- **Set config** sends one parameter per command; J2534 parameter ids and values
  pass through unchanged (`param`, `value`). The Corolla programming path uses
  ISO9141 id 3. Example inners: `0E 00 0E 02 03 00 00 00 01 00 00 00 80 25 00 00`
  (DATA_RATE=9600), `0E 00 0E 02 03 00 00 00 0C 00 00 00 00 00 00 00` (P4_MIN=0),
  `0E 00 0E 02 03 00 00 00 14 00 00 00 46 00 00 00` (TINIL=70),
  `0E 00 0E 02 03 00 00 00 15 00 00 00 50 00 00 00` (TWUP=80).
- **Five-baud init** builds `ILEN = 6 + address count`, then `00 0E 04`,
  the connected proto u32, and the caller address bytes. One-byte example:
  `07 00 0E 04 03 00 00 00 33`. The host pads the inner to 8, encrypts it with
  the current session key, and frames it. It must never replay captured
  ciphertext. The key changes each session.
- **Prog voltage** is proven only for Pin 15 `SHORT_TO_GROUND`
  (`0xFFFFFFFE`): `09 00 0D 0F 00 00 00 FE FF FF FF`. All other pin/value pairs
  stay unsupported. No positive voltage is implemented.
- **Write message** `msg bytes` is the raw protocol message, e.g. an ISO14230
  request `82 <addr> F0 01 <pid>`.
- The host should issue **Clear periodic msgs** immediately before each Write
  message to arm a fresh K‑line transaction.
- The vendor waits about 2000 ms before it sends the five-baud command. The
  adapter answer for one address byte took about 2670 ms with OBD disconnected.
  The host must allow at least 5000 ms for the reply.

## 6. Responses (device → host)

After decryption, replies take one of two forms.

### 6.1 Status reply

```
02 00 <CMD> 00 00 00 00 <status>          (8 bytes, 1 block)
```

`<CMD>` echoes the command being acknowledged; `<status>` is a result byte.
Checksummed over the encrypted payload (§2.1).

### 6.2 Message reply (read poll / fast init)

```
+------+------+------+----------+----------------+-----------------+
| ILEN | 0x00 | 09   | RxStatus | 00 × 7         | MSG (ILEN-9)    |
+------+------+------+----------+----------------+-----------------+
   0      1      2       3         4..10            11..
```

- **RxStatus** (offset 3): `00` = received data message, `02` = transmit echo
  (loopback of the message the host just sent).
- **MSG** begins at offset 11; its length is `ILEN - 9`.
- If no message is available, the device returns a short status‑style reply
  (`ILEN` < 9, e.g. `02 00 09 10 …`) and no MSG.

Checksummed over the decrypted inner (§2.1).

A typical request/response exchange (read poll loop) returns, in order: zero or
more "no data" replies, one echo (`RxStatus = 02`), then the ECU data
(`RxStatus = 00`).

### 6.3 Proven command construction vs adapter-only failures

Proven construction (decrypts with the current session key, matches captures):

- five-baud `07 00 0E 04 03 00 00 00 33` (T254 frame 550);
- Pin 15 ground `09 00 0D 0F 00 00 00 FE FF FF FF` (T255 frame 336);
- Corolla SCONFIG inners with proto 3 (T255 frames 354/372/390/408);
- Corolla FAST_INIT `10 00 0E 05 03 00 00 00 00 00 13 00 01 00 27 6F 57 BC`
  (T255 frame 438).

Adapter-only failure replies (OBD disconnected, no ECU traffic):

- five-baud `02 00 0E 08 00 00 00 28` maps to J2534 `ERR_DEVICE_NOT_CONNECTED (8)`.
  The host must not fabricate key bytes.
- FAST_INIT `02 00 0E 08 00 00 00 F0` maps to J2534 `ERR_TIMEOUT (9)` with zero
  output bytes. The prior code returned success with zero bytes. That was wrong.
- Unknown status replies fail closed. The bytes `08` and `F0`/`28` have no
  proven general meaning. Do not infer a firmware code map from them.

Still unobserved:

- No successful five-baud wire reply exists in captures. Static evidence says a
  success returns two key bytes with `STATUS_NOERROR`, but the wire shape was
  never recorded. The code leaves any other five-baud reply fail-closed. Live
  five-baud success against an ECU remains unevidenced. Do not claim it.

## 7. Keepalive

While a device is open the host must keep the adapter alive by transmitting
periodically (≈ every 15 ms); the adapter resets if traffic stops. The dedicated
keepalive command (§5.1) is used when idle. During an active read‑poll loop the
poll frames themselves satisfy this requirement.

## 8. Teardown

To close a session cleanly: send **Disconnect** (`01 00 02 …`), then close the
serial port / `FT_Close`. The next open performs the link‑layer reset (§1),
which re‑initialises the adapter MCU.

## 9. Worked example

`SET_CONFIG(param = 7, value = 0)` with session key `B0 CB 49 68 07 45 C8 7F`:

```
inner  = 0E 00 0E 02 04 00 00 00 07 00 00 00 00 00 00 00   (16 bytes, 2 blocks)
cipher = DES-ECB(key, inner)
       = 1A 7C EF A7 56 16 8C BC  3F 7D 9A 06 8E 58 87 E6
LEN    = 3 + 16 = 0x13
XORSUM = XOR(13, 00, cipher…) = 24
frame  = 13 00 1A 7C EF A7 56 16 8C BC 3F 7D 9A 06 8E 58 87 E6 24
```

The device acknowledges with a status reply that decrypts to
`02 00 0E 00 00 00 00 28`.

---

*This specification was determined by observation of the adapter's behaviour and
is documented here for interoperability. It describes an independent
implementation and contains no third‑party code.*

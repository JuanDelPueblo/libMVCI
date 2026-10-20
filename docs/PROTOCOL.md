# MVCI Wire Protocol Specification

This document describes the serial protocol spoken by the FTDI‑based "M‑VCI"
(Mini‑VCI) diagnostic cable, as implemented by `libMVCI`. It is provided for
interoperability and documentation purposes.

All multi‑byte integers are **little‑endian** unless noted. Byte values are
hexadecimal. "Inner" refers to a decrypted command/response body; "frame" refers
to the bytes actually on the wire.

This document records the protocol as observed and implemented. It is not a
claim of J2534 conformance, firmware capability, or electrical behavior. The
[README status](../README.md#status) lists the current API coverage and
limitations.

---

## 1. Link layer

- USB: FTDI FT232R, VID `0403` / PID `6001`, USB product description `M-VCI`.
- Serial line: **115200 baud, 8 data bits, no parity, 1 stop bit**, no flow control.
- **MCU reset on open:** clear RTS, set DTR, wait 15 ms, clear DTR; then wait
  ~1 s for the adapter to boot before the first frame.

### 1.1 Identity and firmware version

The proven command set has no firmware‑version query. The host observes only
the USB VID/PID and product description, the serial node on Linux and macOS, and
the per‑connection handshake challenge. A firmware build label seen in a capture
belongs to that capture.

The J2534 `PassThruReadVersion` export reports firmware `Unknown (not queried)`,
DLL `libMVCI 1.0.0`, and API `04.04`. These are host implementation labels. Do
not infer the connected firmware build or cable capabilities from them.

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
| Stop message filter | `0C` | `09 00 0C` `[proto u32]` `[msgID u32]` | status `02 00 0C` |
| Set config (one param) | `0E`/`02` | `0E 00 0E 02` `[proto u32]` `[param u32]` `[value u32]` | status `02 00 0E` |
| Clear periodic msgs | `0E`/`09` | `06 00 0E 09` `[proto u32]` | status `02 00 0E` |
| Fast init | `0E`/`05` | `ILEN 00 0E 05` `[proto u32]` `init bytes…` | message reply (ECU key bytes), or status §6.3 |
| Five-baud init | `0E`/`04` | `ILEN 00 0E 04` `[proto u32]` `address bytes…` | status §6.3; ISO9141 success shape below |
| Prog voltage | `0D` | `09 00 0D` `[pin u32]` `[voltage u32]` | status `02 00 0D` |
| Write message | `0A` | `0E 00 0A` `[proto u32]` `[flags u32]` `msg bytes…` | status `02 00 0A` |
| Read poll | `09` | `05 00 09` `[proto u32]` `00` | message reply, or empty status |
| Keepalive | `09`/`06` | `05 00 09 06 00 00 00 00` | status `02 00 09` |
| Disconnect | `02` | `01 00 02 00 00 00 00 00` | status `02 00 02` |

Notes:
- **Connect admission.** The host admits `ISO9141` (3) or `ISO14230` (4) only,
  at exactly 10400 baud, with flags that contain no bit outside
  `ISO9141_NO_CHECKSUM | ISO9141_K_LINE_ONLY` (`0x0200 | 0x1000` = `0x1200`).
  Another known protocol id returns `ERR_NOT_SUPPORTED`, an undefined id
  returns `ERR_INVALID_PROTOCOL_ID`, other flag bits return `ERR_INVALID_FLAGS`,
  and another baud rate returns `ERR_INVALID_BAUDRATE`. A refused Connect sends
  no frame. One device carries one channel: a second Connect on a connected
  device returns `ERR_CHANNEL_IN_USE` without waiting.
- **Filter `type`** values follow J2534: `1` = PASS, `2` = BLOCK, `3` = FLOW_CONTROL.
- **Set config** sends one parameter per command; J2534 parameter ids and values
  pass through unchanged (`param`, `value`). The captured programming path uses
  ISO9141 id 3. Example inners: `0E 00 0E 02 03 00 00 00 01 00 00 00 80 25 00 00`
  (DATA_RATE=9600), `0E 00 0E 02 03 00 00 00 0C 00 00 00 00 00 00 00` (P4_MIN=0),
  `0E 00 0E 02 03 00 00 00 14 00 00 00 46 00 00 00` (TINIL=70),
  `0E 00 0E 02 03 00 00 00 15 00 00 00 50 00 00 00` (TWUP=80).
  The provider transmits these values, but no evidenced read command or
  measurement shows that the cable firmware applies them.
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
- The original KWP helper issues **Clear periodic msgs** before each Write.
  This is not a general ISO9141 requirement: the captured rate benchmark used
  no such call.
- The original read-poll capture used protocol 4. Its four argument bytes fit
  the same little-endian ProtocolID field as other channel operations. One
  vehicle session connected and wrote with protocol 3 but polled 4 and got
  no frames. Changing only the poll argument to 3 returned data. The poll
  therefore uses the connected ProtocolID; the protocol-4 packet stays
  identical to the captured vector. The observation covers one captured
  adapter build and one vehicle calibration; it does not establish other
  protocol ids or firmware builds.
- The vendor waits about 2000 ms before it sends the five-baud command. The
  adapter answer for one address byte took about 2670 ms with OBD disconnected.
  The host must allow at least 5000 ms for the reply.

## 6. Responses (device → host)

After decryption, replies take one of two forms.

### 6.1 Status reply

```
02 00 <CMD> <result> <opaque bytes x 4>   (8 bytes, 1 block)
```

`<CMD>` echoes the command opcode. The provider accepts supported generic ACKs
only when offset 3 is zero. It refuses nonzero results without guessing a
status-code map. This shared result layout is STRONG_INFERENCE from the
captured configuration and stop replies; complete captured success examples
are not available here for every opcode. Scripted fixtures are not cable proof.

Offset 7 is opaque (UNKNOWN), not a result byte. Captured successful replies
can end in `28` or `B8`. Initialization data and empty read polls have separate
classifiers; do not apply the generic zero-result rule to them. Status replies
use the checksum over the encrypted payload (§2.1).

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

### Configuration read-back

`GET_CONFIG` uses the caller's input `SCONFIG_LIST`, as required by J2534.
The provider validates the whole list before it writes any output values.
An empty list succeeds. A disconnected channel or a null parameter array for
a nonempty list is refused. The call sends no cable command.

Only `LOOPBACK` is reportable. It is a host delivery policy, not a cable
setting: `0` suppresses device transmit echoes; `1` forwards real device echoes
with standard `TX_MSG_TYPE` metadata. The driver never creates an echo from a
requested write, and the policy does not prove that the cable firmware generates
echoes. A new channel
starts at `1`, which preserves the original provider behavior.
`SET_CONFIG(LOOPBACK)` accepts only `0` and `1` and sends no cable command. The
one-byte `48` receive prefix with status `02` is preserved in either mode; it is
not a transmit echo. Its `ExtraDataIndex` is zero. Complete data and echo records
have `ExtraDataIndex=DataSize`. Their payload bytes remain unchanged and their
unmeasured adapter `Timestamp` is zero.

No cable read-configuration command is evidenced. `GET_CONFIG` therefore
returns `ERR_NOT_SUPPORTED` for every other parameter, including `DATA_RATE`,
`P1_MAX`, `P3_MIN` and `P4_MIN`. The error text identifies the parameter.
This remains true after a `SET_CONFIG` success ACK. The provider does not
cache requested values and label them as applied values. A nonzero cable
status on `SET_CONFIG` returns `ERR_FAILED`.

Offline tests exercise these API and delivery rules. They do not show whether
the cable firmware applies K-line timing. A measured cable result or an
evidenced read command is required before adding timing read-back support.

### Receive queue handling

The session decodes each frame into the full wire buffer before it matches a
command reply. It preserves up to 16 intervening diagnostic records, each with
an admitted payload of 1 to 64 bytes. It retains partial frames across
nonblocking reads. Malformed frames, unrelated command replies and overflow
are explicit errors; they do not become empty buffers.

One caller deadline covers timed read/write batches, including waiting for the
I/O owner and transport writes. `NumMsgs` counts delivered records or completed
adapter ACKs. A timed partial batch returns `ERR_TIMEOUT` with its actual count.
A zero-timeout read drains host-buffered input without issuing a poll.
Queue-only writes (`Timeout=0`) return `ERR_NOT_SUPPORTED` and count zero.

The wire has no sequence field and `0E` replies do not echo the subfunction.
Preflight rejects buffered orphan ACKs before sending a new command. A missing
result, partial write, ambiguous framing or malformed reply poisons the session:
later commands are refused until Close/Open. A purge, quiet interval, later
clean ACK or a new Connect in the same session cannot establish recovery. A
checked host RX purge runs before Connect; a poisoned context or a failed purge
refuses the Connect, and neither sends a cable command. Never treat the session
as safe after an unknown result. See
[session lifecycle](#8-teardown-and-session-lifecycle).
Arbitrarily delayed duplicates from
already-completed commands remain indistinguishable if they arrive during a
later command with the same reply opcode. No deterministic recovery or physical
ACK latency bound is claimed.

`CLEAR_RX_BUFFER` purges the host serial input before it drops the software
queue and partial frame. If the host purge fails, the call fails and reads and
commands are blocked until a successful clear. Clearing RX never clears an
ambiguous-transaction latch. It sends no adapter or vehicle command and does
not guarantee that the cable mailbox is empty. An adapter ACK does not prove
bus transmission or ECU acceptance.

### 6.3 Proven command construction vs adapter-only failures

Proven construction (decrypts with the current session key, matches captures):

- Five-baud `07 00 0E 04 03 00 00 00 33` (T254 frame 550);
- Pin 15 ground `09 00 0D 0F 00 00 00 FE FF FF FF` (T255 frame 336);
- Captured SCONFIG inners with proto 3 (T255 frames 354/372/390/408);
- Captured FAST_INIT `10 00 0E 05 03 00 00 00 00 00 13 00 01 00 27 6F 57 BC`
  (T255 frame 438).

Adapter-only failure replies (OBD disconnected, no ECU traffic):

- five-baud `02 00 0E 08 00 00 00 28` maps to J2534 `ERR_DEVICE_NOT_CONNECTED (8)`.
  The host must not fabricate key bytes.
- FAST_INIT `02 00 0E 08 00 00 00 F0` maps to J2534 `ERR_TIMEOUT (9)` with zero
  output bytes. Zero output bytes are not success.
- Unknown status replies fail closed. The bytes `08` and `F0`/`28` have no
  proven general meaning. Do not infer a firmware code map from them.

Vehicle observation for one ISO9141 K-line session (stationary, engine on):

- `FIVE_BAUD_INIT(0x33)` on ISO9141 returned decrypted inner
  `03 00 0E 08 08 00 00 00`. The two key bytes are `08 08`. The provider
  accepts only this exact success reply and leaves other shapes fail-closed.
- With the original protocol-4 read poll, a functional Mode 09 `0903` request
  returned no ECU frame within the allowed window. With protocol-3 polling,
  identity reads and a sustained RPM/TPS sequence completed without a reported
  error. This is one vehicle and one adapter build; it is not a general rate or
  compatibility claim.

## 7. Keepalive

The keepalive command (§5.1) exists, and `mvci_keepalive()` sends one on
demand. The J2534 provider path sends no background traffic: T254 held a
2.7 s five-baud wait and T255 held the full programming-entry sequence with
zero keepalive commands on the wire and no adapter reset, so an autonomous
15 ms keepalive would only add unevidenced traffic between characterized
calls. A caller that holds a device open and idle far beyond the evidenced
windows may call `mvci_start_keepalive()` explicitly.

## 8. Teardown and session lifecycle

### 8.1 Disconnect

To disconnect cleanly, send **Disconnect** (`01 00 02 …`). The device answers in
plaintext with the exact frame `07 00 02 00 02 00 07`. Only that exact reply is
the session boundary: the provider retires the channel handle permanently,
drops queued records, partial frames and overflow state, and resets channel
filters and the LOOPBACK policy. Any other answer returns `ERR_FAILED`; the
channel stays connected. Any attempted write without the exact reply, including
a short write or transport error, leaves an unknown result and poisons the
session (§8.3). A failed host RX purge sends nothing and blocks queued delivery
until a checked purge succeeds. A later
exact reply completes the teardown but does not clear the earlier unknown
result.

### 8.2 Close and Open

`PassThruClose` marks the device closing, so later lookups fail at once, then
waits for running calls to leave. It detaches the context, sends Disconnect when
the channel is connected, and closes the serial port or `FT_Close`. Close does
not require the Disconnect acknowledgement. The next Open performs the
link‑layer reset (§1), which re‑initialises the adapter MCU.

Device and channel handles are unique 32-bit values. They are never reused and
never derived from a slot offset, so a stale handle from an earlier Open or
Connect cannot name a new object. When the identifier space is exhausted, a new
Open or Connect is refused instead of wrapping.

### 8.3 Recovery after an unknown result

A lost or ambiguous result poisons the session. A read poll that passes a public
read deadline remains pending for a later read; no new cable command may pass
it. This covers a timeout with an outstanding non-poll command,
a partial write, a malformed or desynchronised reply, an unanswered Disconnect,
and a read poll cancelled after it was sent. A purge, quiet interval, later
clean ACK, or a new Connect in the same session cannot prove recovery. Only
Close/Open recovers. Before Connect, the provider performs a checked host RX
purge and refuses a poisoned context; a refusal sends no cable command.
`CLEAR_RX_BUFFER` and a zero-timeout drain never clear the poison.

### 8.4 Read cancellation and shared calls

Reads run as shared calls. Connect, Disconnect and Close run exclusively. When a
Disconnect or Close is waiting, a read gives way at its next host I/O slice of
20 ms. The slice only lets the read check for cancellation: it does not shorten
the caller deadline when nothing cancels, and it is not a cable poll interval or
a K-line timing measurement. Records already delivered are returned with
`STATUS_NOERROR`; otherwise the read reports `ERR_FAILED`. A poll cancelled
before it sent anything leaves the session usable. A poll cancelled after its
command was sent has an unknown result and poisons the session (§8.3).

A channel-scoped shared call that meets a waiting Disconnect or Close fails at
once with `ERR_INVALID_CHANNEL_ID`, without consuming its caller's budget.
Device-scoped shared calls (version and voltage) wait for a running exclusive
call. Writes, filters and IOCTLs are not cancelled; a waiting Close may wait for
their own transport timeouts.

### 8.5 Reconnection scope

No preemptive re‑handshake is implemented. The handshake (§3) runs in Open only.
Same-context reconnect after Disconnect is not established on hardware. A
scripted close and reopen, or any fake transport reconnect, does not prove that a physical
cable reconnects or that the adapter accepts a new session; that needs cable and
vehicle evidence. The offline lifecycle tests do not establish Windows runtime
behavior, and the POSIX transport takes no exclusive ownership of the serial
node, so another process can open the same port.

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

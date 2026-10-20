# libMVCI — an open J2534 driver for the Mini-VCI USB adapter

Cross-platform implementation of the **SAE J2534 PassThru API** for the
common FTDI FT232R–based *Mini‑VCI / "M‑VCI"* diagnostic cable, so the hardware
you own can be used with standard J2534 diagnostic software — including on
**Linux** and **macOS**, where no vendor driver exists.

| Platform | Output | Serial backend | Crypto backend |
|---|---|---|---|
| Linux   | `libMVCI.so` | termios (`/dev/ttyUSBx`) | OpenSSL libcrypto |
| macOS   | `libMVCI.dylib` | termios (`/dev/cu.usbserial-*`) | CommonCrypto (system) |
| Windows | `MVCI32.dll` (x86), `MVCI64.dll` (x64) | FTDI D2XX (`ftd2xx.dll`, loaded at runtime) | Windows CNG (BCrypt) |

Built with CMake; the library lands in the build tree (e.g. `build/`). See
**[BUILD.md](BUILD.md)**.

## Why this exists — interoperability

This project was created **for interoperability**. The "M‑VCI" cable ships with a
closed, Windows‑only J2534 DLL, which means owners of the hardware cannot use it
with J2534 tools on Linux, nor with their own software, and have no documentation
of how the device communicates.

`libMVCI` is an independent, re‑implementation of the publicly‑observable
J2534 interface and the cable's serial wire protocol, written so that:

- the same physical adapter works across operating systems (notably Linux);
- it can be loaded by existing J2534 diagnostic applications (loading it is not
  a claim of full J2534 conformance; see [Known limitations](#known-limitations)); and
- the device's behaviour is documented and not locked to one vendor's binary.

It contains **no vendor code** — only an original implementation that speaks the
same wire protocol.

## Status

`libMVCI` is a narrow K‑line provider. It is not a conformance‑complete J2534
implementation, and a documented command sequence is not proof that the cable
firmware applies the requested behavior.

- ✅ **ISO14230 / KWP2000 (K‑line)** — connect, message filters, `SET_CONFIG`,
  five‑baud/fast‑init, and synchronous request/response are implemented. The
  emitted command sequences are checked against captured vectors by the
  hardware‑free sequence test. A stationary vehicle read session completed over
  this path.
- ✅ **ISO9141 initialization** — `SET_CONFIG` uses the connected id,
  `FIVE_BAUD_INIT`, Pin 15 `SHORT_TO_GROUND`, and `FAST_INIT` match captured
  Mini‑VCI frames. OBD‑disconnected failures map to J2534 errors 8/9. Read polls
  use the connected `ProtocolID`. Offline receive tests cover queued replies
  around write acknowledgements.
- ⬜ **ISO15765 / CAN** — not implemented. CAN ECUs are not reachable.
- **J2534 API coverage is partial.** Synchronous write batches use one caller
  deadline and report the completed adapter ACK count. Queue-only writes,
  unsupported flags and mismatched message protocols are refused.
- **Configuration read-back** — `GET_CONFIG` reports only the host‑enforced
  `LOOPBACK` delivery policy. Cable timing parameters return `ERR_NOT_SUPPORTED`,
  even after a successful `SET_CONFIG`. A cable ACK does not prove that its
  firmware applies the requested timing. See [configuration limits](docs/PROTOCOL.md#configuration-read-back).
- The wire framing (DES‑based) and session handling are implemented once and
  shared across platforms; only the protocol/transport/crypto *backends* differ
  per OS, selected at compile time. Crypto failures stop frame construction or
  decoding. Platform runtime behavior still needs separate validation.

## Known limitations

These are facts about the current source, not a conformance claim. Offline
tests run the public API against a scripted transport. They do not prove
physical timing, electrical behavior, or ECU acceptance.

- **Acknowledgements have no sequence field.** Supported status replies require
  a zero result byte. An already-buffered orphan ACK or a missing result stops
  later commands. Close and reopen after ambiguous ordering. An arbitrarily
  delayed duplicate from a completed command can still resemble a later reply;
  this is unresolved. An adapter ACK does not establish physical transmission
  or ECU acceptance.
- **Write path.** A batch is validated before sending. Nonzero-timeout writes
  complete synchronously. A zero timeout returns `ERR_NOT_SUPPORTED` with count
  zero because no asynchronous TX queue is implemented. Counts describe adapter
  ACKs. Payload admission remains limited to 64 bytes.
- **Read path.** A zero timeout drains buffered input without a cable poll. A
  timed partial batch returns `ERR_TIMEOUT` and the actual count. Corruption,
  transport loss and queue overflow are errors. The pending queue holds 16
  records, and receive payloads are limited to 64 bytes. Adapter timestamps are
  not established; no timestamp is synthesized from an assumed cable clock.
- **Filters and clear operations.** Only one-byte PASS filters are supported.
  BLOCK, FLOW_CONTROL, longer descriptors and unsupported flags are refused.
  Stopping a filter frees its slot. Filter clear uses individual stop commands
  and retains failed entries. TX clear is unsupported. RX clear covers only
  host state and serial input, and reports a host purge failure. See
  [filter and clear contracts](docs/filter-and-clear-contracts.md).
- **Platform validation.** Host-side fakes cover CNG, CommonCrypto and D2XX
  status handling. Cross-compilation checks Windows ABI and exports. These
  checks do not prove the real Windows or macOS runtimes, USB timing, or cable
  electrical behavior.
- **Unsupported capabilities are refused.** ISO15765/CAN, periodic transmission,
  battery-voltage reads, pattern-only init and positive programming voltage
  remain unsupported. The provider models one channel per open device.

## Hardware

FTDI FT232R, USB VID `0x0403` / PID `0x6001`, USB description **`M-VCI`**
(115200 8N1). On Linux it enumerates through the kernel `ftdi_sio` driver as
`/dev/ttyUSBx`; on macOS through Apple's FTDI VCP driver as `/dev/cu.usbserial-*`;
on Windows it is opened directly through `ftd2xx.dll`. No `libusb` dependency exists.

### Identity and firmware version

The library's own identity is its file name (`libMVCI.so`, `libMVCI.dylib`,
`MVCI32.dll`/`MVCI64.dll`) and its project version (`1.0.0`). The adapter is
identified by the FTDI VID/PID and USB product description on Windows, or by the
serial node on Linux and macOS.

`PassThruReadVersion` reports firmware `Unknown (not queried)`, DLL
`libMVCI 1.0.0`, and API `04.04`. The firmware string is not a measurement.
The supported command set has no evidenced firmware-version request.
`PassThruGetLastError` reports a bounded string for the calling thread.

## Building

CMake is the cross-platform build (Linux, macOS, Windows). See
**[BUILD.md](BUILD.md)** for per-platform notes and how to consume MVCI from
another CMake project.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build            # supported offline regressions, no hardware
```

The offline tests and hardware‑free verification command are described in
[BUILD.md](BUILD.md#verification-without-hardware). Passing them does not prove
physical cable timing or ECU acceptance.

On Windows, build a specific architecture with the bundled presets (from the
matching Native Tools Command Prompt, or by selecting the preset in your IDE):

```bat
cmake --preset win64   &&  cmake --build --preset win64   :: MVCI64.dll
cmake --preset win32   &&  cmake --build --preset win32   :: MVCI32.dll
```

Reuse it from another CMake project — either vendored:

```cmake
add_subdirectory(external/libmvci)
target_link_libraries(your_target PRIVATE libmvci)
```

or installed:

```cmake
find_package(libmvci CONFIG REQUIRED)
target_link_libraries(your_target PRIVATE libMVCI::libmvci)
```

## Using it

The library exports the standard J2534 `PassThru*` functions, so a J2534 host
application can load it. Loading it does not imply that every J2534 contract is
implemented; see [Known limitations](#known-limitations).

- **Windows:** register/point your tool at `MVCI32.dll` (32‑bit hosts) or
  `MVCI64.dll` (64‑bit). It opens the adapter by its USB description `M-VCI`.
- **Linux:** load `libMVCI.so`. Tell it which serial node to use via the
  `pName` argument to `PassThruOpen`, or the `MVCI_PORT` environment variable
  (default `/dev/ttyUSB0`). Add yourself to the `dialout` group for port access.
- **macOS:** load `libMVCI.dylib`. Same as Linux, but the FTDI VCP node is
  `/dev/cu.usbserial-*` (`MVCI_PORT` default `/dev/cu.usbserial`).

The MVCI session layer can also be used directly via `#include <mvci/serial.h>`
(see `test/mvci_test.c` for an example: handshake → connect → fast‑init → read
PIDs).

## Layout

```
include/mvci/    public headers      ->  #include <mvci/j2534.h>, <mvci/serial.h>
src/             sources + private headers (io.h, compat.h, mvci.def)
test/            mvci_test.c codec/live harness, mvci_seq_test.c provider
                 sequence test, mock_io.c scripted transport
docs/            protocol specification (PROTOCOL.md)
cmake/           package-config template (libmvciConfig.cmake.in)
CMakeLists.txt   cross-platform build
CMakePresets.json  win32/win64 + default build presets
build/           build output (generated)
```

## Documentation

The wire protocol and frame format are specified in
**[docs/PROTOCOL.md](docs/PROTOCOL.md)**.

## License

[LICENSE.md](LICENSE.md)

## Disclaimer

This is an independent, unofficial project for **interoperability** with hardware
the user already owns. It is not affiliated with, authorised by, or endorsed by
Toyota, XHorse, FTDI, or any J2534 software vendor. All product and company names
and trademarks are the property of their respective owners. Provided as‑is, with
no warranty; use at your own risk.

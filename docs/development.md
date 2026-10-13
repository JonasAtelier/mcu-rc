# Architecture, driver development and tests

[Back to README](../README.md)

Paths mentioned in this guide are relative to the repository root.

## Repository layout

```
include/                    public API, the only directory apps include
  rc.h                      struct rc_dev, rc_update(), getters, link, feed, idle
  rc_log.h                  rc_err/warn/info/dbg, rc_log_link()
  crsf.h                    struct crsf and its config, crsf_*(), crsf_us()
  sbus.h                    struct sbus and its config, sbus_*(), the flags
  dr16.h                    struct dr16 and its config, dr16_*(), the input, keys
src/                        private to the library
  rc.c                      the core: update, normalising, link, the ring and idle mark
  rc_log.c                  rc_log_link()
  rc_priv.h                 bus ops, attach and probe, errno map, the ring, rc_input,
                            rc_unpack_11bit(), rc_frame_done/error()
  rc_port_esp.c             the bus layer and rc_input_open() on ESP-IDF; empty elsewhere
  rc_port_stm.c             the same on the STM32 HAL; empty under ESP-IDF
  crsf/                     crsf.c  crsf_proto.h
  sbus/                     sbus.c  sbus_proto.h
  dr16/                     dr16.c  dr16_proto.h
test/                       host tests, `make -C test test`
  test_crsf_proto.c         esp-crsf-rc's test_parse.c, plus the specification's CRC table
  test_sbus_proto.c         the frame, flags and footers; the channels against Bolder Flight's unpack
  test_dr16_proto.c         a frame packed by hand, the switch order, the ranges
  test_crsf.c               esp-crsf-rc's test_driver.c on a fake UART, and fed
  test_sbus.c               the line, flags, footers, resync, idle
  test_dr16.c               fed only, burst lengths, ranges, frames between updates
  core_cases.h              core cases, shared by esp/ and stm/
  esp/                      suites on idf_stubs/, through rc_port_esp.c
  stm/                      suites on hal_stubs/, through rc_port_stm.c
  idf_stubs/  hal_stubs/    the slice of ESP-IDF and of the HAL the code calls
examples/
  esp/<name>/               ESP-IDF projects: crsf, sbus, dr16
  stm/<name>/               STM32 snippets: sbus_fed, dr16_fed
docs/                       the five guides
CMakeLists.txt              ESP-IDF only; stm-forge globs src/**/*.c instead
Kconfig                     RC and RC_<PROTOCOL>, for ESP-IDF and both forges' menus
idf_component.yml           ESP-IDF component manifest
LICENSE                     MIT
```

Naming inside `src/<protocol>/`, the same in every category library:

| File | Holds |
|---|---|
| `<protocol>.c` | the driver: init, the ops, the protocol's calls |
| `<protocol>_regs.h` | register map, each value with its source citation |
| `<protocol>_proto.h` | serial protocol, for parts without registers |
| `<protocol>_math.h` | pure conversions, compiled and asserted on the host |
| `<protocol>_priv.h` | driver state, only if needed |

Every protocol here is a `_proto.h`: they have no registers. `crsf_proto.h`
and the two ported tests keep esp-crsf-rc's four-space style; the library's
own files use tabs.

**Names.** Everything public is `rc_*`, `RC_*`, `crsf_*`, `CRSF_*`,
`sbus_*`, `SBUS_*`, `dr16_*` or `DR16_*`. `crsf.h` and `crsf_init()` are
also esp-crsf-rc's, which this library replaces; the two do not link
together.

## Handles

Each protocol's handle is opaque and on the heap, as esp-crsf-rc's was:
`<protocol>_init()` allocates it, `<protocol>_del()` frees it. It holds a
`struct rc_dev`, whose `priv` points back, and a `struct rc_input`, the
byte source. The handle is opaque so that the platform's UART types and the
ring stay out of the public headers.

`rc_dev` holds what every protocol shares: the raw channels, the nominal
range they normalise on, the failsafe flag, the frame and error counts and
the stamp of the last frame. A protocol's `parse` op writes `raw[]` and
calls `rc_frame_done()` for each good frame and `rc_frame_error()` for each
bad one; the optional `idle` op hears the idle mark. The core stamps a new
frame in `rc_update()`.

**No clock is read.** `rc_update()` stamps with the app's `now_ms` and
`rc_get_link()` ages with it, as mcu-esc's `dt_s` does.

## Platform ports

The drivers in `src/` are the same C on both platforms; what differs is in
`src/rc_port_esp.c` and `src/rc_port_stm.c`: the bus ops, attach,
`rc_i2c_probe()`, and `rc_input_open()` / `rc_input_close()`. On ESP-IDF a
read init installs the UART driver, sets the line from the protocol's
`struct rc_line`, inverts RX when the line is inverted, and attaches the
bus with timeout 0. On the HAL it attaches the app's handle; the line is
the app's.

## Bus abstraction

The bus layer is mcu-esc's: `struct rc_bus_ops` with `transfer()`,
`exchange()`, `receive()` and `detach()`. Every category library offers
I2C, SPI and UART, used or not, and `test/{esp,stm}/test_port_*` check all
three. On the HAL the read path uses `receive()`, one byte with timeout 0
at a time. On ESP-IDF it does not: `receive()` throws away a short read,
so `rc_input_read()` in `src/rc_port_esp.c` asks
`uart_get_buffered_data_len()` what the driver holds and reads exactly
that with 0 ticks -- `rc_ticks(0)`, like mcu-esc's, is one tick, which
would make `rc_update()` wait.

**The ring.** Fed bytes wait in `struct rc_ring` (mcu-lidar's, fixed at 512
bytes): one writer, one reader, `__atomic` acquire and release on the two
indices. The idle mark is a position and a count: the writer stores the
position, then publishes the count. The reader loads the head before the
count, so a mark it has not seen yet was made at or after that head, and
lands at the tail next time, never behind it.

## Adding a protocol

1. `include/foo.h`: the config with a `struct rc_port_config port` and a
   `timeout_ms`, `foo_init()`, `foo_del()`, `foo_rc()`, and getters for
   what the protocol carries beyond the channels.
2. `src/foo/foo.c`: include `rc_priv.h` first, then wrap in
   `#if CONFIG_RC_FOO`.
   - Embed a `struct rc_dev` and a `struct rc_input` in the handle.
   - `rc_input_open()` with the protocol's `struct rc_line`, then
     `rc_dev_init()` with the nominal raw range.
   - A `parse` op that keeps a partial frame between calls; an `idle` op
     if the line's quiet gaps mean something.
3. Cite every byte, range and flag to a source in `src/foo/foo_proto.h`,
   and hold them in a `test_foo_proto.c`.
4. Add `CONFIG_RC_FOO` to:
   - the no-protocol checks in `CMakeLists.txt` and `src/rc_port_stm.c`;
   - its source in `CMakeLists.txt`;
   - `Kconfig`;
   - both forges' defconfigs;
   - `test/Makefile`.

## Building the examples

The ESP-IDF examples pull the library in through `EXTRA_COMPONENT_DIRS`
pointing three directories up, and a local component is named after its
directory, so the checkout must be a directory named `rc` -- the name
esp-forge gives it, and what each `main/CMakeLists.txt` `REQUIRES`:

```sh
git clone https://github.com/JonasAtelier/mcu-rc rc
cd rc/examples/esp/crsf          # or sbus, dr16
idf.py set-target esp32s3
idf.py build
```

A checkout named `mcu-rc` fails at configure time with no component `rc`.
The STM32 examples are snippets: copy one into a project set up as in
[installation](installation.md#stm32-hal).

## Tests

```sh
make -C test test
```

Host-only; no toolchain, HAL, ESP-IDF or receiver needed. Nothing is
checked against a real receiver.

Each change is checked by a test that fails without it: 25 mutations of the
source were run, each on a scratch copy, and all 25 fail the suite. They
cover the 11-bit unpack's last word, CRSF's resync step and both length
rules, the SBUS footers, header-after-footer rule, flags and inversion,
the DR16's switch order, burst length, range ends and wheel offset, the
idle mark's position and once-only delivery, the timeout boundary, a
`now_ms` before the stamp, the two halves of normalising, the drop count,
the failsafe state, the ESP-IDF inversion and cleanup, and a UART error
on the read path. (One of them, the 0-tick poll, went with the change
below to batched ESP-IDF reads.) A second round on the timeout latch, the
idle-mark FIFO and the batched reads ran 12 more: 11 fail the suite -- the
one that stops advancing the FIFO by hanging it, caught by a timeout -- and
the 12th was an equivalent rewrite, run as a control.

| Binary | Checks |
|---|---|
| `test_crsf_proto` | esp-crsf-rc's test_parse.c: **the CRC against the hand-derived table, the specification's first row (crsf.md:195) and the CRC-8/DVB-S2 check value**; linearity; the channel unpack against two hand-packed payloads; every length past 62 refused; the link statistics fields; `crsf_us()` |
| `test_crsf` | esp-crsf-rc's test_driver.c on errno and `rc_update()`: **init at once, a silent receiver as no frame**; a failed install gives the port back; buffer size and baud; every address; **22, 23 and 26-byte channel frames**; **resync one byte at a time past a false sync with a plausible length**; a corrupt frame counted and skipped; short channel and link frames counted; the newest of three; unknown types stepped over; a longer link frame; a frame split across updates; **the same stream fed a byte at a time**; the age into a timeout |
| `test_sbus_proto` | **the 16 channels against Bolder Flight's per-channel shifts on 10000 random frames**; header and every footer the sources name, and others refused; each flag on its own bit |
| `test_sbus` | 100000 8E2, inverted unless `not_inverted`; channels and flags; **frame lost stays up, failsafe is RC_LINK_FAILSAFE**; the SBUS2 footers, another refused; **a false header after channel data passed over, one after a 0x00 counted, the real frames still decoded**; a split frame; normalising on Betaflight's fit against ArduPilot's ends; **fed: a partial frame at an idle dropped and counted, junk before an idle forgotten** |
| `test_dr16_proto` | **a frame packed by hand bit by bit**: sticks, switches, mouse, keys, wheel; **S1 is the high pair**; a stick one past either end and a switch at 0 refused, nothing written |
| `test_dr16` | **-EOPNOTSUPP unless fed**; no frame without an idle; normalised at 364, 1024, 1684; up then timeout; **a tail, two frames in one burst, 17 bytes: counted, not decoded**; an out-of-range frame keeps the last good one; **three frames between two updates all decoded, the newest kept; nine, past the idle marks kept: the first two thrown away as one, the rest decoded** |
| `esp/test_core`, `stm/test_core` | `rc_update()` fed: -EAGAIN, the stamp, the newest of several; **the ring many times round, full, the drop count**; **the idle mark between the right bytes, once, several in one update; more than `RC_IDLE_MARKS`: the oldest lost, their bytes run into the first kept**; normalising at the ends, each half its own scale, past the ends; before the first frame; **the link states, the timeout boundary, stale beats failsafe, a now before the stamp is a timeout, the 32-bit wrap; dead for 2^31 + 1 ms, and for 2^32 + 10 ms with the age wrapped to 10: still a timeout, latched until the next frame**; **the read path: 0-tick reads, more than a chunk, a UART fault and the byte read again after it; ESP-IDF: only what the driver holds, in 64-byte chunks**; NULLs; the log line |
| `esp/test_port_esp`, `stm/test_port_stm` | the bus layer, as in every category library; `rc_input_open()`: install, line, pin, inversion, attach with timeout 0, cleanup on each failure, fed touching nothing (ESP-IDF); the app's handle, none refused, fed (HAL) |

All build under `-std=gnu11 -Wall -Wextra -Werror -pedantic` with UBSan
and ASan, and `-Wl,--wrap` (GNU ld) to capture `printf()` on the HAL side.

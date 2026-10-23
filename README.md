# mcu-rc

<p align="center">
  <img alt="platform: ESP-IDF | STM32 HAL" src="https://img.shields.io/badge/platform-ESP--IDF%20%7C%20STM32%20HAL-70e1b2?style=flat-square&labelColor=0d2238">
  <img alt="language: C" src="https://img.shields.io/badge/language-C-70e1b2?style=flat-square&labelColor=0d2238">
  <img alt="license: MIT" src="https://img.shields.io/badge/license-MIT-f2bd68?style=flat-square&labelColor=0d2238">
</p>

A small C library for **RC receivers** on **ESP-IDF 5.2+** and the **STM32
HAL**: receivers that stream stick positions over one serial wire. Each
protocol keeps its own API and hands out one `struct rc_dev`: the channels
raw and normalised to -1..1, the link state with the receiver's failsafe
flag where the protocol has one, and the age of the last frame.

This library replaces `esp-crsf-rc`, whose CRSF parser it keeps; its API
changed (see [changes from esp-crsf-rc](docs/usage.md#changes-from-esp-crsf-rc)).

The repository is named `mcu-rc`; the C API, `CONFIG_RC_*` options and
forge package key are `rc`.

> **Hardware unverified.** Host tests use fake receivers, a fake UART and
> platform stubs. Nothing here has heard a real radio.

[Supported receivers](#supported-receivers) · [Drivers](#drivers) ·
[Install](#install) ·
[Quick start](#quick-start) · [Examples](#examples) ·
[Documentation](#documentation) · [Tests](#tests) · [License](#license)

## Supported receivers

| Protocol | Bus | Notes |
|---|---|---|
| **CRSF** | UART | TBS Crossfire and ExpressLRS serial protocol |
| **SBUS** | UART | Futaba serial protocol |
| **DJI DR16** | UART | DJI RoboMaster receiver protocol |

One Kconfig option per protocol: `RC_CRSF`, `RC_SBUS`, `RC_DR16`. Every
byte offset, scale and flag is cited in the
[protocol reference](docs/chips.md); wiring is in the
[hardware guide](docs/hardware.md).

## Drivers

<details><summary><b>CRSF</b> — UART</summary>

- Receivers: TBS Crossfire, ExpressLRS
- Line: 420000 8N1 (configurable)
- Channels: 16 + link statistics
- Failsafe: none on the wire: timeout
- Bytes in: read or fed
- Examples: ESP-IDF [crsf](examples/esp/crsf/), read on UART1 · STM32 HAL none
- Chip reference: [CRSF](docs/chips.md#crsf)

</details>

<details><summary><b>SBUS</b> — UART</summary>

- Receivers: Futaba, FrSky and others
- Line: 100000 8E2, inverted
- Channels: 16 + 2 digital
- Failsafe: the receiver's flag
- Bytes in: read or fed
- Examples: ESP-IDF [sbus](examples/esp/sbus/), read, inverted by the UART ·
  STM32 HAL [sbus_fed](examples/stm/sbus_fed/main.c), circular DMA
- **No SBUS fast mode** (200000 baud) and no SBUS2 telemetry slots; an SBUS2
  footer is accepted.
- Chip reference: [SBUS](docs/chips.md#sbus)

</details>

<details><summary><b>DJI DR16</b> — UART</summary>

- Receivers: DT7 radio, RoboMaster
- Line: 100000 8E1, inverted
- Channels: 4 sticks + switches, mouse, keys
- Failsafe: none on the wire: timeout
- Bytes in: fed, with the idle line
- Examples: ESP-IDF [dr16](examples/esp/dr16/), fed from the UART event queue
  · STM32 HAL [dr16_fed](examples/stm/dr16_fed/main.c), DMA to idle, as on a
  RoboMaster board
- **DR16 is fed only**: a DBUS frame has no header and no checksum, and
  only the idle line between frames says where one starts.
- Chip reference: [DJI DR16 (DBUS)](docs/chips.md#dji-dr16-dbus)

</details>

## Install

### ESP-IDF

```sh
git clone https://github.com/JonasAtelier/mcu-rc components/rc
```

```ini
CONFIG_RC=y
CONFIG_RC_CRSF=y
```

Other components declare `REQUIRES rc driver`.

### esp-forge / stm-forge

```make
rc_GIT=https://github.com/JonasAtelier/mcu-rc@stable
```

```make
PKG_rc              = y
RC_CRSF             = y
```

**No protocol is enabled by default.**

## Quick start

A CRSF receiver on UART1, read by the library:

```c
#include "crsf.h"

struct crsf_config config = {
    .port = { .uart = UART_NUM_1, .rx_gpio = 16 },
};
struct crsf *receiver;
struct rc_link link;
float sticks[4];

crsf_init(&config, &receiver);
struct rc_dev *rc = crsf_rc(receiver);

/* every loop */
rc_update(rc, now_ms);                  /* never waits */
rc_get_link(rc, now_ms, &link);
if (link.state != RC_LINK_UP)
    stop_everything();                  /* the failsafe is yours */
else if (rc_get_channels(rc, sticks, 4) == 0)
    steer(sticks[0]);                   /* -1..1 */
```

Fed instead, when the app owns the UART:

```c
struct sbus_config config = { .port = { .fed = true } };

/* in the UART's interrupt or DMA callback */
rc_rx_feed(rc, bytes, len);
rc_rx_idle(rc);                         /* the line went quiet; DR16 needs it */
```

- **0 or a negative errno** from every call, on both platforms.
- **No clock is read here.** The app passes `now_ms` to `rc_update()`,
  which stamps each new frame, and to `rc_get_link()`, which ages it. The
  link is `RC_LINK_TIMEOUT` after `timeout_ms` without a frame, 100 ms by
  default -- our policy, not a protocol's.
- **No blocking.** `rc_update()` takes what is there and returns.

## Examples

Each driver's examples are listed under [Drivers](#drivers).

ESP-IDF examples find this library through `EXTRA_COMPONENT_DIRS`, which
assumes it is checked out as `rc`:

```sh
cd examples/esp/crsf
idf.py set-target esp32s3 # Replace with your board's target.
idf.py build
```

STM32 examples are snippets to integrate into your application.

## Scope and limitations

- **No telemetry back to the radio**, and no CRSF parameter, MSP or
  device frames: framed, checked and dropped.
- **No channel mapping**, reversing, expo or deadband: channels in wire
  order.
- **No failsafe policy.** The library reports; the app decides.
- **No locks.** One task per receiver; only `rc_rx_feed()` and
  `rc_rx_idle()` may run in an interrupt.

## Documentation

| Guide | Contents |
|---|---|
| [Installation](docs/installation.md) | Forge, ESP-IDF, STM32 HAL, build options, logging |
| [Hardware](docs/hardware.md) | Wiring, levels and inversion per protocol, the UART settings on each platform |
| [API usage](docs/usage.md) | The `rc_dev`, read and fed, the idle mark, each protocol's extras, errors, changes from esp-crsf-rc |
| [Protocol reference](docs/chips.md) | Every frame, offset, range and flag, each cited; where the sources disagree |
| [Development](docs/development.md) | Layout, ports, bus abstraction, adding a protocol and test coverage |

## Tests

```sh
make -C test test
```

Host-only; no toolchain, HAL, ESP-IDF or receiver needed. See the
[test reference](docs/development.md#tests). Log lines during the suite are
expected.

## License

[MIT](LICENSE).

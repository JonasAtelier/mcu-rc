# Installation

[Back to README](../README.md)

Paths mentioned in this guide are relative to the repository root.

The repository is `mcu-rc`; the forge package key, C API and configuration
options are `rc`. The clone command below names the ESP-IDF component
directory `rc`. It replaces `esp-crsf-rc`, which no forge catalog
lists: a project that `REQUIRES esp-crsf-rc` now `REQUIRES rc`,
and moves to the API in [usage](usage.md#changes-from-esp-crsf-rc). The two
cannot link into one app: both define `crsf.h` and `crsf_init()`.

No dependency beyond ESP-IDF's `driver` component or the HAL's UART module.

## With forge

With [esp-forge](https://github.com/JonasAtelier/esp-forge) or
[stm-forge](https://github.com/JonasAtelier/stm-forge). The catalog line in
`profiles/pkgversion` is the same for both:

```make
rc_GIT=https://github.com/JonasAtelier/mcu-rc@stable
```

and a target turns the library and its protocols on in
`configs/<target>_defconfig`:

```make
PKG_rc              = y
RC_SBUS             = y
# RC_CRSF is not set
# RC_DR16 is not set
# RC_DEBUG is not set
```

or with `make menu`. esp-forge turns each line into `CONFIG_RC_<PROTOCOL>=y`
in `sdkconfig.defaults`; stm-forge passes it as `-DCONFIG_RC_<PROTOCOL>=1`.
`RC_DEBUG = y` turns on `rc_dbg()` lines on the HAL; on ESP-IDF the log
level decides.

Neither forge's catalog has this line yet: it goes in once the repository
is on GitHub.

## ESP-IDF

Clone into your project's `components/` directory. A local component is
named after its directory, and that name is what you `REQUIRES`:

```sh
git clone https://github.com/JonasAtelier/mcu-rc components/rc
```

```cmake
idf_component_register(SRCS "main.c" REQUIRES rc driver)
```

Enable the protocols under `idf.py menuconfig` → **Component config → RC
receiver subsystem**, or put `CONFIG_RC_<PROTOCOL>=y` in your
`sdkconfig.defaults` as each example in `examples/esp/` does. With the
subsystem on and no protocol picked, the build warns.

**The UART is the library's when it reads.** With `fed` false, the init
installs the port's driver with no TX buffer and no event queue, sets the
line and the RX pin, inverts RX for SBUS, and del deletes the driver. Do
not install or share that port yourself. With `fed` true the library never
touches a UART.

## STM32 HAL

Copy `include/` and `src/` into an STM32 project, compile every `.c` under
`src/`, protocol folders included, add `include/` and `src/` to the include
path (`src/` only needs to reach this library's own sources), and define
what the forge would: `CMSIS_device_header` as your family header
(`"stm32f4xx.h"`), `USE_HAL_DRIVER`, and `CONFIG_RC_<PROTOCOL>=1`.

**No protocol is built by default.** The core is always built and each
driver is opt-in: a protocol whose option is absent or 0 compiles to
nothing. With the library on and no protocol picked, `src/rc_port_stm.c`
emits a `#warning`.

**The HAL modules are yours to enable:**

| Use | Needs in hal_conf | Gives |
|---|---|---|
| fed (recommended) | whatever your UART receive uses: `HAL_UART_MODULE_ENABLED`, `HAL_DMA_MODULE_ENABLED` | `rc_rx_feed()`, `rc_rx_idle()` |
| read | `HAL_UART_MODULE_ENABLED` | `port.uart`; without it a read init is `-EOPNOTSUPP` |

The UART is always the app's on the HAL: `HAL_UART_Init()` it at the
protocol's line before the init ([hardware](hardware.md)). Reading is a
blocking `HAL_UART_Receive()` of one byte with timeout 0 per byte, which
listens only while it runs: bytes that arrive between two `rc_update()`
calls are lost. Feed from DMA instead.

**printf() is the log.** Lines go out through `printf()`, so retarget
newlib's `_write()` to a UART.

## Logging

`rc_err()`, `rc_warn()`, `rc_info()` and `rc_dbg()` prefix every line with
the protocol's name. On ESP-IDF they are `ESP_LOGx` under the one tag
`"rc"`, so `esp_log_level_set("rc", ...)` controls every receiver. On the
HAL they are `printf("rc: <name>: ...")`, and `rc_dbg()` is compiled in only
with `CONFIG_RC_DEBUG`. `rc_log_link()` prints the link:

```
rc: crsf: up, age 4 ms, frames 1520 errors 3 dropped 0
```

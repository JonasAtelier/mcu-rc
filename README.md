# mcu-rc

Read stick positions and link status from a CRSF (Crossfire or ExpressLRS), SBUS or DJI DR16 radio receiver over UART.

**Platforms** · ESP-IDF 5.2+ and STM32 HAL. Hardware testing is pending. The host tests use simulated devices.

## Contents

- [Quick start](#quick-start)
- [Receivers and examples](#receivers-and-examples)
- [Using the library in your project](#using-the-library-in-your-project)
- [License](#license)

## Quick start

For CRSF, connect the receiver’s non-inverted TX to the configured RX pin and share ground. SBUS and DR16 use different serial settings and wiring.

In a terminal with ESP-IDF activated, get the example and select the target for an ESP32. Change the target if your board uses another ESP32 chip.

```sh
git clone https://github.com/JonasAtelier/mcu-rc.git rc
cd rc/examples/esp/crsf
idf.py set-target esp32
```

Check the [wiring guide](docs/hardware.md) and edit the settings in [main.c](examples/esp/crsf/main/main.c) to match your board. Connect the board by USB, then build, upload and open the serial monitor.

```sh
idf.py build flash monitor
```

Move a stick to watch the first four channels in the monitor. Check link state and frame age in your application and choose the safe response to a timeout or receiver failsafe.

## Receivers and examples

Use the common API for normalised channels, link state and the age of the latest frame. The protocol headers expose additional data, such as CRSF link statistics, SBUS flags and DR16 mouse or keyboard input. [Shared API](include/rc.h)

| Protocol | Driver header | ESP-IDF project | STM32 HAL example |
|---|---|---|---|
| [CRSF](docs/chips.md#crsf) | [crsf.h](include/crsf.h) | [UART channels and link quality](examples/esp/crsf/) | — |
| [SBUS](docs/chips.md#sbus) | [sbus.h](include/sbus.h) | [UART channels and flags](examples/esp/sbus/) | [Feed received bytes](examples/stm/sbus_fed/main.c) |
| [DJI DR16](docs/chips.md#dji-dr16-dbus) | [dr16.h](include/dr16.h) | [Feed UART events](examples/esp/dr16/) | [Feed UART DMA data](examples/stm/dr16_fed/main.c) |

A dash marks a hardware and platform combination without a dedicated example.

## Using the library in your project

- Add the component and enable the driver for your hardware. [Installation and build options](docs/installation.md)
- Open the ESP-IDF project for your hardware, or copy the STM32 example into a project with board setup already in place. [All ESP-IDF examples](examples/esp/) · [All STM32 examples](examples/stm/)
- Look up calls and chip-specific settings. [API usage](docs/usage.md) · [Hardware reference](docs/chips.md)

To run the simulated tests on your computer, run `make -C test test` from the library root. [Test requirements and coverage](docs/development.md)

Migrating from `esp-crsf-rc`? [API changes](docs/usage.md#changes-from-esp-crsf-rc)

## License

© 2026 JonasAtelier, [GPL-3.0-only](LICENSE).

[Browse the workshop catalog · ESP32 and STM32 drivers](https://github.com/JonasAtelier/jonas-atelier-libraries#esp32-and-stm32-drivers)

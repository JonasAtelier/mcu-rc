# mcu-rc

Read stick positions and link status from a CRSF (Crossfire or ExpressLRS), SBUS or DJI DR16 radio receiver over UART.

**Platforms** · ESP-IDF 5.2+ and STM32 HAL. Hardware testing is pending. The host tests use simulated devices.

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

## Using the library in your project

- Add the component and enable the driver for your hardware. [Installation and build options](docs/installation.md)
- Start from an example for your chip. The STM32 examples fit into a project with board setup already in place. [ESP-IDF examples](examples/esp/) · [STM32 examples](examples/stm/)
- Look up calls and chip-specific settings. [API usage](docs/usage.md) · [Hardware reference](docs/chips.md)

To run the simulated tests on your computer, run `make -C test test` from the library root. [Test requirements and coverage](docs/development.md)

Migrating from `esp-crsf-rc`? [API changes](docs/usage.md#changes-from-esp-crsf-rc)

## License

[MIT](LICENSE).

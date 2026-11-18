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

Three step-by-step guides come first: [ESP-IDF](#esp-idf-step-by-step),
[STM32 with CMake](#with-cmake-a-cubemx-cmake-project) and
[STM32 without CMake](#without-cmake-stm32cubeide-a-makefile). None
needs a forge; if you build with esp-forge or stm-forge, see
[With forge](#with-forge). The steps use an SBUS receiver (Futaba, FrSky);
for CRSF or the DJI DR16, change the protocol name and start from its
example. The reference follows the guides.

**Arduino is not supported yet.** TODO: make the library usable from
Arduino (Arduino IDE and PlatformIO).

## ESP-IDF, step by step

**You need** ESP-IDF 5.2 or newer, with `idf.py` working in your terminal, and
an ESP-IDF project: a folder with a `CMakeLists.txt` and a `main/` folder.
`idf.py create-project my-project` makes one. On the wire: the receiver's
SBUS output straight to a GPIO, with a 3.3 V signal (some receivers put out
5 V); the ESP32's UART inverts SBUS in hardware.

ESP-IDF calls a library a **component**: a folder with its own
`CMakeLists.txt` that the build finds and compiles. This library is the
component `rc`.

1. **Get the library.** Pick one way.

   - **Component manager**, the simplest: the first build downloads the
     library into `managed_components/rc`. Create `main/idf_component.yml`:

     ```yaml
     dependencies:
       rc:
         git: https://github.com/JonasAtelier/mcu-rc.git
         version: stable
     ```

   - **Clone**, if you want to edit the library or build offline. In the
     project folder:

     ```sh
     git clone https://github.com/JonasAtelier/mcu-rc components/rc
     ```

     Keep the folder name `rc`: ESP-IDF names a component after its folder.

   ```
   my-project/
   ├── CMakeLists.txt
   ├── sdkconfig.defaults       step 2
   ├── main/
   │   ├── CMakeLists.txt       step 3
   │   ├── idf_component.yml    step 1, component manager
   │   └── my-project.c         step 4
   └── components/
       └── rc/                  step 1, clone
   ```

2. **Turn your protocol on.** No protocol is built until you do. Create
   `sdkconfig.defaults` in the project folder, a text file of settings you
   choose, with one line per protocol:

   ```ini
   CONFIG_RC_SBUS=y
   ```

   The other protocols are `CONFIG_RC_CRSF` and `CONFIG_RC_DR16`. ESP-IDF
   copies these lines into `sdkconfig`, the project's full settings file,
   only when it creates that file: **if `sdkconfig` already exists, delete
   it** so your line is read. `idf.py menuconfig` → **Component config → RC
   receiver subsystem** does the same from a menu.

3. **Let your code see the library.** A new project's `main/CMakeLists.txt`
   has no `REQUIRES` and already sees every component. If yours has a
   `REQUIRES` list, add `rc`, `driver` and `esp_timer` (the example's clock)
   to it:

   ```cmake
   idf_component_register(SRCS "main.c" REQUIRES rc driver esp_timer)
   ```

4. **Write the smallest test.** Copy
   [examples/esp/sbus/main/main.c](../examples/esp/sbus/main/main.c) over
   your main source file and set `RC_UART` and `RC_RX_GPIO` to your wiring.
   It reads the receiver and prints the link and the first four channels
   once a second. [API usage](usage.md) explains the calls.

5. **Build.**

   ```sh
   idf.py set-target esp32    # your chip: esp32s3, esp32c3, ...
   idf.py build
   ```

   A good build ends with `Project build complete.` Flash it as any ESP-IDF
   app and open the serial monitor: once a second a line ends in
   `rc: sbus: up, age 12 ms, frames 300 errors 2 dropped 0`, followed by
   the sticks. `no frame` instead of `up` means nothing arrives: check the
   pin and the receiver's binding.

**If it fails**

| The build prints | Why | Fix |
|---|---|---|
| `Failed to resolve component 'rc' required by component 'main': unknown name.` | The clone is not in a folder named `rc` | Rename it to `components/rc` |
| `fatal error: rc.h: No such file or directory` | `main/CMakeLists.txt` has a `REQUIRES` list without `rc` | Step 3 |
| `rc: CONFIG_RC is on but no protocol is selected; set CONFIG_RC_<PROTOCOL>=y (esp-forge: RC_<PROTOCOL> = y)` (a CMake warning) | No protocol turned on, often because `sdkconfig` existed before you edited `sdkconfig.defaults` | Step 2: delete `sdkconfig`, build again |

## STM32, step by step

**You need** a project for an STM32F4 that uses ST's **HAL**, the vendor
library behind `HAL_Init()`, `HAL_UART_Init()` and friends, with a UART set
up for SBUS. STM32CubeMX makes such a project; with **Toolchain / IDE:
CMake** it makes a CMake one, the main case below. On the wire: SBUS idles
low and the F4's USART cannot invert its input, so an inverting buffer goes
between the receiver and the RX pin.

Two terms. A **HAL module** is the part of the HAL for one peripheral: UART,
DMA. It is compiled only when it is switched on in **hal_conf**, the file
`Core/Inc/stm32f4xx_hal_conf.h`. CubeMX switches a module on when you enable
that peripheral.

### With CMake (a CubeMX CMake project)

1. **Get the library.** In the project folder, next to the top-level
   `CMakeLists.txt`:

   ```sh
   git clone https://github.com/JonasAtelier/mcu-rc
   ```

   ```
   my-board/
   ├── CMakeLists.txt                  step 2
   ├── cmake/stm32cubemx/              made by CubeMX
   ├── Core/
   │   ├── Inc/stm32f4xx_hal_conf.h    hal_conf, step 3
   │   └── Src/main.c                  step 4
   ├── Drivers/                        the HAL, copied in by CubeMX
   └── mcu-rc/                         step 1
   ```

2. **Add it to the build.** In the top `CMakeLists.txt`, after the line
   `add_subdirectory(cmake/stm32cubemx)`, add:

   ```cmake
   set(CONFIG_RC_SBUS ON)              # your protocol; one line per protocol
   set(MCU_HAL_TARGET stm32cubemx)
   set(MCU_DEVICE_HEADER stm32f4xx.h)
   add_subdirectory(mcu-rc)
   ```

   and add `rc` to the link line CubeMX already wrote, so it reads:

   ```cmake
   target_link_libraries(${CMAKE_PROJECT_NAME} stm32cubemx rc)
   ```

   Do not add `PRIVATE`: CubeMX's line has no such keyword, and CMake refuses
   to mix the two forms on one target.

   `stm32cubemx` is a CMake *target*, a named bundle of settings, that CubeMX
   makes to carry the HAL's include folders and defines; `MCU_HAL_TARGET`
   lends those to the library, which works with any CubeMX version.
   `MCU_DEVICE_HEADER` is your chip family's main header.

3. **Set up the UART.** The receiver is fed from DMA, so hal_conf needs
   `#define HAL_UART_MODULE_ENABLED` and `#define HAL_DMA_MODULE_ENABLED`;
   CubeMX writes both when you set the UART up as below. The test uses
   USART3; in CubeMX:

   - mode Asynchronous, 100000 baud, word length 9 bits (the HAL counts the
     parity bit), parity even, 2 stop bits, receive only;
   - a DMA request for USART3_RX in circular mode;
   - the USART3 global interrupt enabled.

   The [HAL module table](#stm32-hal) lists what reading without DMA needs.

4. **Write the smallest test.** It is
   [examples/stm/sbus_fed/main.c](../examples/stm/sbus_fed/main.c) fitted
   into CubeMX's `Core/Src/main.c`. Paste each part between the
   `USER CODE BEGIN` and `END` comments named, so a regenerate keeps it.

   After `/* USER CODE BEGIN Includes */`:

   ```c
   #include <stdio.h>

   #include "rc.h"
   #include "rc_log.h"
   #include "sbus.h"
   ```

   After `/* USER CODE BEGIN 0 */`, just above `int main(void)`:

   ```c
   static struct sbus *receiver;
   static uint8_t dma_buf[128];
   static uint16_t dma_old; /* where the last event left off */
   ```

   After `/* USER CODE BEGIN 2 */`, which comes after `MX_USART3_UART_Init()`:

   ```c
     struct sbus_config config = { .port = { .fed = true } };

     if (sbus_init(&config, &receiver))
       Error_Handler();
     HAL_UARTEx_ReceiveToIdle_DMA(&huart3, dma_buf, sizeof(dma_buf));
   ```

   After `/* USER CODE BEGIN 3 */`, inside the `while (1)` loop:

   ```c
       static uint32_t last_log;
       struct rc_dev *rc = sbus_rc(receiver);
       struct rc_link link;
       float sticks[4];

       rc_update(rc, HAL_GetTick());
       if (HAL_GetTick() - last_log < 1000)
         continue;
       last_log = HAL_GetTick();
       rc_get_link(rc, last_log, &link);
       rc_log_link(rc, &link);
       if (rc_get_channels(rc, sticks, 4) == 0)
         printf("sticks %+.2f %+.2f %+.2f %+.2f\n", (double)sticks[0],
                (double)sticks[1], (double)sticks[2], (double)sticks[3]);
   ```

   After `/* USER CODE BEGIN 4 */`:

   ```c
   /*
    * ReceiveToIdle in circular mode: size is the position the DMA has
    * written up to; hand over everything since the last call, wrapping at
    * the end.
    */
   void HAL_UARTEx_RxEventCallback(UART_HandleTypeDef *huart, uint16_t size)
   {
     struct rc_dev *rc = sbus_rc(receiver);

     if (huart != &huart3)
       return;
     if (size > dma_old) {
       rc_rx_feed(rc, dma_buf + dma_old, size - dma_old);
     } else if (size < dma_old) {
       rc_rx_feed(rc, dma_buf + dma_old, sizeof(dma_buf) - dma_old);
       rc_rx_feed(rc, dma_buf, size);
     }
     dma_old = size == sizeof(dma_buf) ? 0 : size;
   }

   /*
    * The HAL aborts a DMA reception on any receive error (parity, framing,
    * noise, overrun): start it again, or one glitch ends reception for good.
    */
   void HAL_UART_ErrorCallback(UART_HandleTypeDef *huart)
   {
     if (huart != &huart3)
       return;
     rc_rx_idle(sbus_rc(receiver));
     dma_old = 0;
     HAL_UARTEx_ReceiveToIdle_DMA(&huart3, dma_buf, sizeof(dma_buf));
   }
   ```

   The lines go out through `printf()`: send that to a UART by writing
   newlib's `_write()`, and add this to `CMakeLists.txt` so numbers are
   printed:

   ```cmake
   target_link_options(${CMAKE_PROJECT_NAME} PRIVATE -u _printf_float)
   ```

   [API usage](usage.md) explains the calls.

5. **Build** as before, for example `cmake --build` with your build folder.
   A good build shows a `Linking C executable` line and no error. On the
   UART, once a second:
   `rc: sbus: up, age 12 ms, frames 300 errors 2 dropped 0`, then the
   sticks. `no frame` instead of `up` means nothing arrives: check the
   inverter, the pin and the UART settings.

**If it fails**

| You see | Why | Fix |
|---|---|---|
| `rc: no protocol is selected; set(CONFIG_RC_<PROTOCOL> ON) before add_subdirectory()` (a CMake warning), then ``undefined reference to `sbus_init'`` | No protocol turned on, or turned on after `add_subdirectory(mcu-rc)` | Step 2 |
| `#error "rc: CMSIS_device_header is not defined; ...` | `MCU_DEVICE_HEADER` is missing | Step 2 |
| `fatal error: stm32f4xx.h: No such file or directory` | `MCU_HAL_TARGET` is missing | Step 2 |
| `Target "stm32cubmx" not found.` (a CMake error) | `MCU_HAL_TARGET` is misspelt | Step 2 |
| `The plain signature for target_link_libraries has already been used with the target` | `PRIVATE` added to the link line | Step 2 |

### Without CMake (STM32CubeIDE, a Makefile)

1. Copy `include/` and `src/` of this repository into your project, for
   example to `Middlewares/mcu-rc/`, keeping the protocol folders inside
   `src/`.
2. Add every `.c` file under `src/` to the build, protocol folders included.
3. Add two include paths, `Middlewares/mcu-rc/include` and
   `Middlewares/mcu-rc/src`, and two defines:
   `CMSIS_device_header="stm32f4xx.h"`, quotes included, and
   `CONFIG_RC_SBUS=1`, one per protocol. CubeMX already defines
   `USE_HAL_DRIVER` and your device, such as `STM32F405xx`.
4. Steps 3 to 5 of the CMake guide apply as they are; `-u _printf_float`
   goes in the linker flags.

Where these settings usually live: in STM32CubeIDE, **Project → Properties →
C/C++ Build → Settings → Tool Settings → MCU GCC Compiler** (Include paths,
Preprocessor); in a CubeMX Makefile, `C_SOURCES`, `C_INCLUDES` and `C_DEFS`.
The build errors are the ones in the table above; the fix is the matching
define or include path instead of a CMake line. One more:
`#include expects "FILENAME" or <FILENAME>` means `CMSIS_device_header` lost
its quotes.

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

## ESP-IDF

Setup: [ESP-IDF, step by step](#esp-idf-step-by-step). For reference:

- In `main/idf_component.yml`, `version` is a git ref; `stable` is the
  released one.
- A clone in `components/` is a local component, named after its folder;
  that name is what other components `REQUIRES`. The examples in
  `examples/esp/` need no name: their `main` lists no `REQUIRES`, and ESP-IDF
  gives `main` every component, so they build from any checkout name.
- With the subsystem on and no protocol picked, the build warns.

**The UART is the library's when it reads.** With `fed` false, the init
installs the port's driver with no TX buffer and no event queue, sets the
line and the RX pin, inverts RX for SBUS, and del deletes the driver. Do
not install or share that port yourself. With `fed` true the library never
touches a UART.

## STM32 HAL

Setup: [With CMake](#with-cmake-a-cubemx-cmake-project) or
[Without CMake](#without-cmake-stm32cubeide-a-makefile). The
`add_subdirectory()` route builds a static library `rc`. For reference:

- The family is the F4: `CMSIS_device_header` is `"stm32f4xx.h"`.
- Each `CONFIG_RC_<PROTOCOL>` (and `CONFIG_RC_DEBUG`) that is true reaches
  the compiler as `CONFIG_RC_<PROTOCOL>=1`, for the library and your app.
- `MCU_HAL_TARGET` is a target that carries your `USE_HAL_DRIVER`, device
  define (`STM32F405xx`, ...) and the include dirs of the HAL, CMSIS and your
  `stm32f4xx_hal_conf.h`. The library copies only those include dirs and
  defines, never the target's sources, so this works with any CubeMX
  version. `MCU_DEVICE_HEADER` becomes `CMSIS_device_header`, which CubeMX
  does not define. Every `mcu-*` library reads these two, so set them once.
  Leave them unset if your project already provides both.
- The CPU flags (`-mcpu`, `-mfloat-abi`, ...) must reach the library too;
  CubeMX's toolchain file sets them for every target.
- Without CMake, `src/` only needs to be on the include path of this
  library's own sources; the defines are what stm-forge passes.

**No protocol is built by default.** The core is always built and each
driver is opt-in: a protocol whose option is absent or 0 compiles to
nothing. With the library on and no protocol picked, the CMakeLists warns
(ESP-IDF and `add_subdirectory()`) and `src/rc_port_stm.c` emits a
`#warning`.

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

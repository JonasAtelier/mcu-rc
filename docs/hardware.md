# Hardware

[Back to README](../README.md)

Paths mentioned in this guide are relative to the repository root. Sources
are the short names of the [protocol reference](chips.md#sources).

Every receiver here talks and never listens: one signal wire into a UART
RX pin, plus power and a shared ground. Nothing below has been checked on
a bench.

## The line, per protocol

| Protocol | Baud | Frame | Polarity | Source |
|---|---|---|---|---|
| CRSF | 420000 (416666, 400000) | 8N1 | not inverted, 3.0-3.3 V | TBS crsf.md:134, :138; BF crsf_protocol.h:33-37 |
| SBUS | 100000 | 8E2 | inverted (idle low) | BF sbus.c:59, :82, :205-207; BFS sbus.cpp:59-63 |
| DBUS (DR16) | 100000 | 8E1 | inverted | TUT p.116 |

**Inverted** means a 1 is low and the idle line is low, the opposite of a
UART. The receiver's pad needs inverting somewhere before the UART samples
it.

## ESP-IDF

The ESP32 UART inverts in hardware (`uart_set_line_inverse()` with
`UART_SIGNAL_RXD_INV`), and any GPIO can be RX. So:

- **CRSF:** receiver TX to the RX GPIO. The pin names cross over: the
  receiver's TX is the wire that carries data.
- **SBUS:** the SBUS pad straight to the RX GPIO; `sbus_init()` turns the
  inversion on. `not_inverted = true` for a receiver with an uninverted
  SBUS pad.
- **DR16:** fed; the app sets the UART up (100000, 8 data bits, even
  parity, 1 stop bit, RX inverted), as `examples/esp/dr16/` does.

**Check the signal level first.** The ESP32 is a 3.3 V part. TBS gives
3.0-3.3 V for CRSF (crsf.md:138). None of the SBUS or DBUS sources here
gives a level, and the DR16 runs on 4-8.4 V (DT7 p.2): measure its output
before it meets a pin.

## STM32 HAL

The UART is the app's. Two things differ from ESP-IDF:

**The word length counts the parity bit.** The F4 HAL documents parity as
"inserted at the MSB position ... 9th bit when the word length is set to 9
data bits; 8th bit when the word length is set to 8 data bits"
(stm32f4xx_hal_uart.h:63-66, STM32F4 HAL v1.7.12). So 8 data bits with
even parity is `UART_WORDLENGTH_9B`:

| Protocol | `WordLength` | `Parity` | `StopBits` |
|---|---|---|---|
| CRSF | `UART_WORDLENGTH_8B` | `UART_PARITY_NONE` | `UART_STOPBITS_1` |
| SBUS | `UART_WORDLENGTH_9B` | `UART_PARITY_EVEN` | `UART_STOPBITS_2` |
| DBUS | `UART_WORDLENGTH_9B` | `UART_PARITY_EVEN` | `UART_STOPBITS_1` |

DJI's own board C example sets `UART_WORDLENGTH_8B` with even parity for
DBUS (BC usart.c:36-39), which by the note above is 7 data bits and a
parity bit. Our reading of why it still decodes: the 8th data bit sits
where that setting looks for parity, and the HAL hands the received
parity bit back as the MSB (stm32f4xx_hal_uart.c:1794-1795), so the bytes
come out right while parity and framing errors fire -- which DJI's
interrupt handler clears (BC remote_control.c:83-91). Unverified on
hardware; we recommend 9B.

**Inversion.** The F4's USART has no RX inversion: the F4 HAL has no
`AdvancedInit` (none in stm32f4xx_hal_uart.h, v1.7.12). Put an inverter
in front of the pin -- RoboMaster development boards have one on the DBUS
port (TUT p.116) -- or, on a family whose HAL has
`UART_ADVFEATURE_RXINVERT_INIT`, set that in `AdvancedInit`.

**Receive with DMA.** Feed the library from `HAL_UARTEx_RxEventCallback()`:
circular DMA for CRSF and SBUS (`examples/stm/sbus_fed/`), normal-mode DMA
to the idle line for the DR16 (`examples/stm/dr16_fed/`). In the F4 HAL
v1.7.12 that callback also fires at half and full buffer
(stm32f4xx_hal_uart.c:3066, :3101) without saying which, so the DR16
example turns the half-transfer interrupt off and treats a short count as
the idle line. In DMA reception the HAL aborts the transfer on any receive
error -- parity, framing, noise, overrun (stm32f4xx_hal_uart.c:2409-2414)
-- so both examples restart it from `HAL_UART_ErrorCallback()`; without
that, one glitch on the line is a permanent `RC_LINK_TIMEOUT`.

## DJI DR16 on a RoboMaster board

The DR16's DBUS lead plugs into the board's DBUS port, which DJI's
examples read on USART3 RX (BC usart.c:35, :64), through the board's
inverter (TUT p.116). The receiver's power comes from the same port.

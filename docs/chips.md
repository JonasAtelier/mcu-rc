# Protocol reference

[Back to README](../README.md)

Paths mentioned in this guide are relative to the repository root.

**Scope.** mcu-rc covers RC receivers that stream channels over one serial
wire. PWM and PPM receivers, transmitter-side modules, and telemetry back
to the radio are out of scope.

Every value is cited where it is used: `src/<protocol>/<protocol>_proto.h`
for the wire, `include/<protocol>.h` for ranges and defaults. This page
collects the sources and the places they disagree.

## Sources

None of these is kept in `docs/`. Line numbers are of the file at the
commit given; page numbers are the PDF's own, counted from 1.

| Short | Document | Licence: how it is used |
|---|---|---|
| TBS | TBS CRSF specification, `tbs-fpv/tbs-crsf-spec` `crsf.md`, commit 4e5fe882 | the specification |
| WIKI | `crsf-wg/crsf` wiki, commit b1bd5f1f: Message-Format, Physical-Layer, CRSF_FRAMETYPE_RC_CHANNELS_PACKED | facts |
| ELRS | ExpressLRS `crsf_protocol.h`, tags 3.5.3 (`src/lib/CrsfProtocol/`) and 4.1.0 (`src/include/`) | GPL-3.0: facts only, no code |
| BF | Betaflight `src/main/rx/`: `crsf.c`, `crsf_protocol.h`, `sbus.c`, `sbus_channels.c`, `sbus_channels.h`, commit cd8e8386 | GPL-3.0: facts only, no code |
| AP | ArduPilot `libraries/AP_RCProtocol/AP_RCProtocol_SBUS.cpp`, commit 3297d9f1 | GPL-3.0: facts only, no code |
| BFS | Bolder Flight Systems `sbus`, `src/sbus.h`, `src/sbus.cpp`, commit 003c5c33 | MIT: its channel unpack is transcribed in `test/test_sbus_proto.c` as an oracle |
| TUT | RoboMaster 开发板 C 型嵌入式软件教程文档 (Development Board C tutorial), DJI 2020, in `RoboMaster/Development-Board-C-Examples` commit 59d12b1a | facts |
| BC | the same repository, `9.remote_control_dma/`: `applications/remote_control.c`, `.h`, `Src/usart.c` | GPL-3.0: facts only, no code |
| RTS | `RoboMaster/RoboRTS-Firmware` `components/devices/dbus.c`, `.h`, commit 8cffd936 | GPL-3.0: facts only, no code |
| A | RoboMaster 竞赛机器人 2020 自组装版 A 型 用户手册 (Type A robot user manual), in the TUT repository, `doc/` | facts |
| DT7 | DJI DT7 & DR16 2.4 GHz RC System User Manual v2.0, 2015.02 | facts |
| HAL | STM32CubeF4 HAL driver v1.7.12, `stm32f4xx_hal_uart.h`, `.c` | for the HAL notes in [hardware](hardware.md) |

CRSF facts were first gathered by esp-crsf-rc against the same kinds of
source; they are re-cited here against the files above.

## CRSF

```
+------+-----+------+-----------------+------+
| sync | len | type |     payload     | crc8 |      len: type + payload + crc, 2..62
+------+-----+------+-----------------+------+      crc8: poly 0xD5, init 0, over type + payload
```

| Fact | Value | Source |
|---|---|---|
| sync / address bytes accepted | 0xC8, 0x00, 0xEE | TBS crsf.md:166-169, :223, :245, :253 |
| frame at most | 64 bytes, len 2..62 | TBS crsf.md:148, :173 |
| CRC | poly 0xD5, init 0 | TBS crsf.md:189, :212-217; BF crsf.c:334 |
| RC_CHANNELS_PACKED | type 0x16, 22 bytes, 16 x 11 bits LSB first | TBS crsf.md:520-548; WIKI ...RC_CHANNELS_PACKED.md:3 |
| LINK_STATISTICS | type 0x14, 10 bytes, uplink first | TBS crsf.md:484-499; ELRS 4.1.0 :394-406 |
| raw range | 172 / 992 / 1811 for 988 / 1500 / 2012 us | ELRS 4.1.0 :20-24; TBS crsf.md:528 |
| baud | 420000 default | BF crsf_protocol.h:33-37; WIKI Physical-Layer.md:28 |

**Where the sources disagree.**

- **Baud.** TBS says 416666 dual-wire and 400000 single-wire
  (crsf.md:134, :138); Betaflight and ExpressLRS use 420000, with 416666
  behind a build flag (BF crsf_protocol.h:30-37). Default 420000;
  `config.baud` on ESP-IDF.
- **The channel frame's length.** TBS says 22 bytes; ExpressLRS 4 may add
  an arm-status byte (WIKI ...RC_CHANNELS_PACKED.md:5; ELRS 4.1.0
  :203-207), and the wiki puts it on the handset-to-module side. TBS
  crsf.md:177 says a longer frame must not be refused. Accepted: 22 or
  more, first 22 read. Tested at 22, 23 and 26.
- **Microseconds.** Betaflight's 0.62477 x raw + 881 (crsf.c:624-627) gives
  2012 at 1811; TBS's `TICKS_TO_US` (crsf.md:525) gives 2011 in integer
  arithmetic. `crsf_us()` takes Betaflight's.
- **"Big endian"** (crsf.md:175) is about multi-byte fields; the channels
  are LSB-first bit fields by every other source.

**Failsafe.** CRSF has no flag: in "cut" mode the receiver stops sending
channel frames (crsf.md:522); in other modes it sends its failsafe values
as normal channels, which no frame field tells apart. mcu-rc reports
`RC_LINK_TIMEOUT` and the link quality.

## SBUS

| Byte | Field | Source |
|---|---|---|
| 0 | header 0x0F | BF sbus.c:71; AP :94; BFS sbus.h:82 |
| 1-22 | 16 channels x 11 bits, LSB first | BF sbus_channels.h:30-49; BFS sbus.cpp:112-147 |
| 23 | bit 0 ch 17, bit 1 ch 18, bit 2 frame lost, bit 3 failsafe | BF sbus_channels.c:38-39, sbus_channels.h:27-28; AP :62-64, :108-110; BFS sbus.h:85-88 |
| 24 | footer | see below |

Line: 100000 baud, 8E2, inverted (BF sbus.c:59, :82, :205-207; BFS
sbus.cpp:59-63). 25 bytes take about 3 ms (BF sbus.c:61).

**Where the sources disagree.**

- **The footer.** BFS accepts 0x00 or a low nibble of 0x04 (sbus.h:83-84,
  sbus.cpp:110); BF says 0x00 on FrSky and some Futaba receivers and a
  telemetry slot marker on some SBUS2 ones (sbus.c:97-104), and neither BF
  nor AP checks the byte: they frame by the gap before a frame (BF
  sbus.c:130-135; AP :79, :172-187). mcu-rc reads no clock, so it frames
  as BFS does: a header right after a footer (BFS sbus.cpp:99-100), a
  footer 24 bytes on, or an idle mark when fed. **A receiver with another
  footer value gets no frames here**; it would show as errors.
- **The raw range.** BF fits 173 / 993 / 1812 to 988 / 1500 / 2012 us
  against an FrSky X4R (sbus_channels.c:41-42, :92-94); AP maps 200..1800
  to 1000..2000 us (:66-76). mcu-rc normalises on BF's; AP's ends come out
  at -0.967 and +0.985. Both are in `test/test_sbus.c`.
- **Bad data as failsafe.** AP also treats a frame as failsafe when any
  of its first four channels maps to 875 us or less (:116-127). mcu-rc
  reports the receiver's flag only.

**What SBUS framing without a clock cannot rule out.** 0x0F and 0x00 both
occur in channel data, so a 0x00 followed by 0x0F with a footer-like byte
24 bytes later can pass for a frame mid-stream. The header-after-footer
rule makes that rare in a stream that is already in step, which is when it
matters; feeding with idle marks removes it.

## DJI DR16 (DBUS)

The DT7 & DR16 manual shows the DBUS port and no protocol (DT7 p.2); the
Type A manual refers to an attachment for it (A p.5) that we do not have.
The layout below is DJI's own code and tutorial, with RoboRTS as a second
reading.

| Bits | Field | Source |
|---|---|---|
| 0-43 | channels 0-3, 11 bits each, LSB first | TUT p.124; BC remote_control.c:174-178; RTS dbus.c:93-99 |
| 44-45 | switch S2, the left one | BC .c:179; RTS dbus.c:121 |
| 46-47 | switch S1, the right one | BC .c:180; RTS dbus.c:120 |
| 48-95 | mouse x, y, z, int16 little-endian | BC .c:181-183; RTS dbus.c:132-134 |
| 96-111 | mouse left, right button, a byte each | BC .c:184-185; RTS dbus.c:136-137 |
| 112-127 | keyboard, bit 0 W ... bit 15 B | BC .c:186, remote_control.h:40-55; RTS dbus.c:139, dbus.h:74-89 |
| 128-143 | wheel, 1024 at rest | RTS dbus.c:140 ("wheel"); BC .c:187, :193 ("NULL") |

| Fact | Value | Source |
|---|---|---|
| line | 100 kbit/s, 8 data, even parity, 1 stop, inverted | TUT p.116 |
| frame | 18 bytes, no header, no checksum | TUT p.117; BC remote_control.h:26 |
| framing | the bytes between two idle interrupts, exactly 18 | BC remote_control.c:81-152, :117, :145 |
| stick range | 364 / 1024 / 1684 | BC remote_control.h:28-30; RTS dbus.c:123-130 (660 either side) |
| switch values | 1 up, 3 middle, 2 down | BC remote_control.h:33-35 |
| S1 is the right switch | | A p.12 ("右上方的三位开关 S1"); DT7 p.2 labels S1 and S2 |

**Where the sources disagree.**

- **Parity.** TUT p.116 says even; TUT p.118, step 4, says "no parity" for
  the same USART3; BC usart.c:39 sets even. mcu-rc takes even.
- **Word length.** BC sets `UART_WORDLENGTH_8B` with even parity
  (usart.c:37-39), 7 data bits by the HAL's own note. See
  [hardware](hardware.md#stm32-hal).
- **Switch names.** BC calls bits 44-45 "Switch left" and RTS calls them
  `sw2`; both agree S1 is bits 46-47. Tested in `test/test_dr16_proto.c`.
- **Bytes 16-17.** RTS reads them as the wheel minus 1024; BC reads them
  minus 1024 and calls them "NULL". Exposed as `wheel`, its range unknown.
- **Zero deadband.** RTS zeroes sticks within 5 of centre (dbus.c:102-118);
  BC does not. mcu-rc does not: that is the app's.

**Failsafe.** DBUS has no flag in either source. mcu-rc reports
`RC_LINK_TIMEOUT` when frames stop. What the DR16 sends when the radio is
off, if anything, no source here says.

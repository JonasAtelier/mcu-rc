# API usage

[Back to README](../README.md)

Paths mentioned in this guide are relative to the repository root.

## The rc_dev

Every protocol hands out one `struct rc_dev` (`crsf_rc()`, `sbus_rc()`,
`dr16_rc()`), and these calls work on any of them:

| Call | Does |
|---|---|
| `rc_update(rc, now_ms)` | takes every byte waiting, decodes what is complete, stamps a new channel frame with `now_ms`. 0 for a new frame, `-EAGAIN` for none. Never waits |
| `rc_get_channels(rc, out, n)` | the first `n` channels, -1..1 at the protocol's nominal ends, 0 at the centre, linear beyond -- not clamped |
| `rc_get_raw(rc, out, n)` | the same channels in wire units |
| `rc_get_link(rc, now_ms, &link)` | state, age, and the frame, error and drop counters |
| `rc_rx_feed(rc, bytes, len)` | fed only: hand over received bytes; ISR-safe |
| `rc_rx_idle(rc)` | fed only: the line went quiet here; ISR-safe |

`rc->channel_count` says how many channels there are: 16 for CRSF and
SBUS, 4 for the DR16. Asking for more is `-EINVAL`; asking before the first
frame is `-ENODATA`.

**The link.**

| `link.state` | When |
|---|---|
| `RC_LINK_NO_FRAME` | no channel frame since init; `age_ms` is `UINT32_MAX` |
| `RC_LINK_UP` | the last frame is at most `timeout_ms` old, no failsafe flag |
| `RC_LINK_FAILSAFE` | as up, but the receiver flagged failsafe (SBUS only): the channels are its failsafe values |
| `RC_LINK_TIMEOUT` | the last frame is older than `timeout_ms` |

`timeout_ms` is each config's; 0 means `RC_DEFAULT_TIMEOUT_MS`, 100 ms.
That is our policy, not a protocol's: the TBS specification suggests a
flight controller waits a second (crsf.md:522); a robot on the ground
usually wants to stop sooner.

**Time.** No clock is read in the library. Read yours just before the
call: a stamp taken before the frame arrived makes `age_ms` read longer,
never shorter. `uint32_t` milliseconds wrap after 49.7 days: the age is
right across the clock's wrap, and a `now_ms` from before the stamp reads
as a huge age, so `RC_LINK_TIMEOUT`. A link dead for longer than the age
can count stays a timeout: the first `rc_update()` that finds the last
frame older than `timeout_ms` latches it (`rc->expired`) until a new frame
arrives. So call `rc_update()` at least once every 49 days.

## Read and fed

The `port` of every config picks where bytes come from.

**Read** (`fed = false`): `rc_update()` takes bytes off the UART itself,
at most 1024 per call, never waiting: on ESP-IDF what
`uart_get_buffered_data_len()` says the driver holds, in chunks; on the
HAL a byte at a time with timeout 0. On ESP-IDF the init
installs the UART driver, which buffers in the background (2048 bytes by
default, `rx_buffer_len`), so reading is lossless if `rc_update()` runs
before the buffer fills. On the HAL a blocking read listens only while it
runs, so bytes between two calls are lost: feed there.

**Fed** (`fed = true`): the app's receive path calls `rc_rx_feed()`. The
bytes wait in a 512-byte ring (`RC_RING_LEN`) -- 12 ms of CRSF at 420000
baud, 56 ms of SBUS or DBUS -- until `rc_update()`; what does not fit is
dropped and counted in `link.dropped`. One writer: `rc_rx_feed()` and
`rc_rx_idle()` from one interrupt or one task. Neither is placed in IRAM:
on ESP-IDF, do not call them from an interrupt registered with
`ESP_INTR_FLAG_IRAM`, which runs while the flash cache is off.

**The idle mark.** `rc_rx_idle()` says the line went quiet after the bytes
fed so far. `rc_update()` hands the bytes before it to the protocol, then
the idle, then the rest. The last `RC_IDLE_MARKS` (8) marks between two
`rc_update()` calls are kept, so a loop may run several frames behind; past
that the oldest marks are lost, and the frames they ended run together and
are thrown away as one, never misread.

- The DR16 needs it: a burst between two idles is a frame when it is
  exactly 18 bytes.
- SBUS uses it when it is there: a partial frame at an idle is dropped,
  and the next byte may start a frame.
- CRSF ignores it: its CRC finds the frames.

## CRSF

```c
struct crsf_config config = {
    .port = { .uart = UART_NUM_1, .rx_gpio = 16 },
    .baud = 0,          /* ESP-IDF read: 420000 */
};
struct crsf *receiver;
struct crsf_link_stats stats;

crsf_init(&config, &receiver);
...
if (crsf_get_link_stats(receiver, &stats) == 0 && stats.link_quality < 20)
    slow_down();
```

`crsf_get_link_stats()` returns the uplink half of the last link
statistics frame -- the last one, kept when the link goes, so read
`rc_get_link()` first: link quality in percent, the RSSI of both antennas as the
positive number the wire carries (85 means -85 dBm), the active antenna and
the SNR. A powered receiver sends them with no radio bound, so a stream of
link frames with no channel frames is a receiver that is wired right and
not bound.

CRSF has no failsafe flag; a receiver in "cut" failsafe stops sending
channels (crsf.md:522). Gate on `RC_LINK_TIMEOUT`, and on link quality,
which sags first. `crsf_us(raw)` converts to servo microseconds, 988 to
2012.

## SBUS

```c
struct sbus_config config = {
    .port = { .uart = UART_NUM_1, .rx_gpio = 16 },
    .not_inverted = false,
};
struct sbus_flags flags;

sbus_get_flags(receiver, &flags);  /* channel_17, channel_18, frame_lost, failsafe */
```

`failsafe` drives `RC_LINK_FAILSAFE`. `frame_lost` is one missed radio
packet, not a lost link (AP AP_RCProtocol_SBUS.cpp:135-140): it is in the
flags only.

## DJI DR16

```c
struct dr16_config config = { .port = { .fed = true } };
struct dr16_input input;

dr16_init(&config, &receiver);         /* -EOPNOTSUPP unless fed */
...
dr16_get_input(receiver, &input);
if (input.switch_s1 == DR16_SWITCH_DOWN)
    disarm();
if (input.keys & DR16_KEY_W)
    forward();
```

The four sticks are channels 0-3 in wire order, 364..1684 around 1024.
`dr16_input` holds the two switches (`switch_s1` the right one,
`switch_s2` the left, `DR16_SWITCH_UP`, `_MIDDLE`, `_DOWN`), the mouse
(x, y, z, two buttons), the keyboard (`DR16_KEY_*` bits) and the left dial
`wheel`. A frame with a stick or a switch out of range is thrown away and
counted. See [hardware](hardware.md) and `examples/` for the receive path.

## Errors

| errno | From |
|---|---|
| `-EINVAL` | a NULL, a count over `channel_count`, a negative `rx_buffer_len`, a read config with no UART handle (HAL) |
| `-EAGAIN` | `rc_update()`: no new channel frame |
| `-ENODATA` | a getter before the first frame |
| `-EOPNOTSUPP` | `dr16_init()` not fed; a CRSF or SBUS read init on a HAL build without the UART module |
| `-ENOMEM` | an init's allocation, or the ESP-IDF driver's |
| `-EIO`, `-ETIMEDOUT`, ... | the UART, through the bus layer's errno map |

## Changes from esp-crsf-rc

| esp-crsf-rc | mcu-rc |
|---|---|
| `crsf_handle_t`, `crsf_config_t` | `struct crsf *`, `struct crsf_config` with a `port` |
| `esp_err_t` returns | 0 or a negative errno, on both platforms |
| `crsf_init()` waits up to 1 s for a frame; `ESP_ERR_NOT_FOUND` if none | returns at once; a silent receiver is `RC_LINK_NO_FRAME` with no frames and no errors, a wrong baud shows as errors |
| `crsf_read(dev, &ch, timeout)` blocks for a frame | `rc_update(rc, now_ms)` never blocks; `rc_get_raw()` / `rc_get_channels()` |
| `crsf_age_ms()` reads the FreeRTOS tick | `rc_get_link(rc, now_ms, &link)`: `age_ms` from the app's time, plus a state |
| `crsf_crc_errors()` | `link.errors`, which counts every frame thrown away: bad CRC, and channel or link frames too short |
| `crsf_link_stats()`, `crsf_link_t.lq` | `crsf_get_link_stats()`, `struct crsf_link_stats.link_quality` |
| `cfg.rx_buf_len` | `config.port.rx_buffer_len` |
| channel payload of 22 or 23 bytes | 22 or longer, as the specification asks (crsf.md:177) |
| link statistics payload of exactly 10 | 10 or longer |
| ESP-IDF only | ESP-IDF and the STM32 HAL; read or fed |

## No loops here

No task, no thread, no timer: the app calls `rc_update()` from its own
loop, and decides what a lost link means.

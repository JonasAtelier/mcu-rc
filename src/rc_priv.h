/*
 * Shared by the protocol drivers inside this library. Not public API.
 *
 * Two layers sit under the protocols, the way regmap sits under a kernel
 * driver:
 *
 *   - The bus kind is chosen at run time. A driver holds a struct rc_bus
 *     and reaches it only through bus->ops, so it can sit on I2C, SPI or a
 *     UART. Every category library offers all three kinds, used or not;
 *     every receiver here is on a UART, read through receive().
 *   - The platform is chosen at link time. rc_port_esp.c and rc_port_stm.c
 *     each define the ops tables, the attach functions and
 *     rc_input_open() for their platform; the other one compiles to
 *     nothing.
 *
 * Above the bus sits struct rc_input: the bus when the library reads the
 * UART, or the ring rc_rx_feed() fills when the app does.
 *
 * ESP_PLATFORM, which ESP-IDF defines for every component, and the HAL's own
 * module switches are defined or not, never 0 or 1, so they are the one place
 * #ifdef is right; this library's CONFIG_RC_* options are always tested
 * with #if.
 */
#ifndef RC_PRIV_H
#define RC_PRIV_H

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "rc.h"

/* The most bytes after cmd: tx on I2C and UART, tx and rx together on SPI. */
#define RC_BUS_MAX_LEN	31

struct rc_bus;

enum rc_bus_kind {
	RC_BUS_I2C,
	RC_BUS_SPI,
	RC_BUS_UART,
};

struct rc_bus_ops {
	enum rc_bus_kind kind;
	/*
	 * One transaction: cmd, then tx_len bytes of tx, then rx_len bytes
	 * into rx. cmd is the register address, with any SPI read bit already
	 * set by the chip, and the first byte of a datagram on a UART.
	 *
	 *   I2C:  rx_len ? cmd, repeated start, read : cmd and tx, one write;
	 *         a bare cmd is a one-byte write. Sending tx and then reading
	 *         is -EINVAL.
	 *   SPI:  CS low, cmd, tx, rx, CS high -- high again even when the
	 *         transfer failed.
	 *   UART: drop stale input, send cmd and tx, then wait for rx_len
	 *         bytes; fewer within the timeout is -ETIMEDOUT. For a line
	 *         where the MCU does not hear itself; see exchange().
	 *
	 * On SPI more than RC_BUS_MAX_LEN bytes after cmd is -EINVAL; on
	 * I2C and UART only tx is bounded so, and rx on the HAL by its 16-bit
	 * count. 0 or a negative errno.
	 */
	int (*transfer)(struct rc_bus *bus, uint8_t cmd,
			const uint8_t *tx, size_t tx_len, uint8_t *rx,
			size_t rx_len);
	/*
	 * len bytes of tx out while len bytes come back into rx, rx[i] the
	 * one that answered tx[i]. More than 1 + RC_BUS_MAX_LEN bytes, or
	 * none, is -EINVAL. NULL on I2C.
	 *
	 *   SPI:  one frame, full duplex: CS low, the bytes at the same clocks,
	 *         CS high -- high again even when the transfer failed. For a
	 *         part whose answer to one frame comes back in the next.
	 *   UART: drop stale input, send tx, and collect what the line gave
	 *         back meanwhile. On a single wire the MCU hears its own
	 *         transmission; this is how a chip takes that echo off the
	 *         line, so that the answer which follows starts clean. Fewer
	 *         than len bytes back within the timeout is -ETIMEDOUT.
	 */
	int (*exchange)(struct rc_bus *bus, const uint8_t *tx,
			uint8_t *rx, size_t len);
	/*
	 * rx_len bytes, with nothing sent and nothing dropped first: the
	 * answer that follows an exchange(). Fewer within the timeout is
	 * -ETIMEDOUT; rx is then undefined. NULL on I2C and SPI.
	 */
	int (*receive)(struct rc_bus *bus, uint8_t *rx,
		       size_t rx_len);
	/* Undo the attach. The bus itself is the app's and is left alone. */
	void (*detach)(struct rc_bus *bus);
};

#ifdef ESP_PLATFORM
#include "driver/i2c_master.h"
#include "driver/spi_master.h"
#include "driver/uart.h"
#include "esp_err.h"

/* The I2C argument of every I2C attach on this platform. */
typedef i2c_master_bus_handle_t rc_i2c_bus;

/*
 * One device on one bus, as its driver holds it. RC_HAVE_BUS
 * says the platform has at least one bus kind, so the struct exists.
 */
#define RC_HAVE_BUS
struct rc_bus {
	const struct rc_bus_ops *ops;
	uint32_t timeout_ms;
	union {
		i2c_master_dev_handle_t i2c;
		spi_device_handle_t spi;
		uart_port_t uart;
	};
};

/*
 * esp_err_t -> negative errno, at the bus boundary and nowhere else.
 * ESP_ERR_NOT_FOUND is -ENXIO (nothing at that address), not -ENODEV
 * (something answered but it is the wrong chip).
 */
static inline int rc_errno(esp_err_t err)
{
	switch (err) {
	case ESP_OK:
		return 0;
	case ESP_ERR_INVALID_ARG:
		return -EINVAL;
	case ESP_ERR_TIMEOUT:
		return -ETIMEDOUT;
	case ESP_ERR_NOT_FOUND:
		return -ENXIO;
	case ESP_ERR_INVALID_SIZE:
		return -EMSGSIZE;
	case ESP_ERR_NO_MEM:
		return -ENOMEM;
	case ESP_ERR_NOT_SUPPORTED:
		return -EOPNOTSUPP;
	default:
		return -EIO;
	}
}

#else /* STM32 HAL */
/*
 * rc.h, included above, stops the build with a clear #error when
 * CMSIS_device_header is undefined; the include is skipped then, so that
 * error is not followed by a bare `#include expects "FILENAME" or
 * <FILENAME>`.
 */
#ifdef CMSIS_device_header
#include CMSIS_device_header
#endif

#ifdef HAL_I2C_MODULE_ENABLED
typedef I2C_HandleTypeDef *rc_i2c_bus;
#endif

#if defined(HAL_I2C_MODULE_ENABLED) || defined(HAL_SPI_MODULE_ENABLED) || \
    defined(HAL_UART_MODULE_ENABLED)
#define RC_HAVE_BUS
struct rc_bus {
	const struct rc_bus_ops *ops;
	uint32_t timeout_ms;
	union {
#ifdef HAL_I2C_MODULE_ENABLED
		struct {
			I2C_HandleTypeDef *handle;
			uint8_t address;	/* 7-bit */
		} i2c;
#endif
#ifdef HAL_SPI_MODULE_ENABLED
		struct {
			SPI_HandleTypeDef *handle;
			GPIO_TypeDef *cs_port;	/* app's output, idle high */
			uint16_t cs_pin;
		} spi;
#endif
#ifdef HAL_UART_MODULE_ENABLED
		UART_HandleTypeDef *uart;
#endif
	};
};
#endif

/*
 * HAL_StatusTypeDef -> negative errno, at the bus boundary and nowhere else.
 * HAL_BUSY -- the bus or the handle in use, or a handle never initialised --
 * is -EAGAIN. A NACK in the middle of a transfer is HAL_ERROR like any other
 * fault, so -EIO.
 */
static inline int rc_hal_errno(HAL_StatusTypeDef status)
{
	switch (status) {
	case HAL_OK:
		return 0;
	case HAL_TIMEOUT:
		return -ETIMEDOUT;
	case HAL_BUSY:
		return -EAGAIN;
	default:
		return -EIO;
	}
}

#endif /* STM32 HAL */

#ifdef RC_HAVE_BUS
/* bus->ops->exchange(), or -EOPNOTSUPP on a kind that has none. */
static inline int rc_bus_exchange(struct rc_bus *bus,
				   const uint8_t *tx, uint8_t *rx,
				   size_t len)
{
	if (!bus->ops->exchange)
		return -EOPNOTSUPP;
	return bus->ops->exchange(bus, tx, rx, len);
}

/* bus->ops->receive(), or -EOPNOTSUPP on a kind that has none. */
static inline int rc_bus_receive(struct rc_bus *bus,
				  uint8_t *rx,
				  size_t rx_len)
{
	if (!bus->ops->receive)
		return -EOPNOTSUPP;
	return bus->ops->receive(bus, rx, rx_len);
}

static inline void rc_bus_detach(struct rc_bus *bus)
{
	bus->ops->detach(bus);
}
#endif

#if defined(ESP_PLATFORM) || defined(HAL_I2C_MODULE_ENABLED)
/*
 * Is anything at this 7-bit address? -ENXIO when nothing ACKs -- nothing at
 * that address -- not -ENODEV, which means something answered but it is the
 * wrong chip. A NACK in the middle of a transfer is a plain -EIO on both
 * platforms, which is why a driver decides presence here, never by its
 * first transfer.
 */
int rc_i2c_probe(rc_i2c_bus handle, uint8_t address,
		  uint32_t timeout_ms);

/*
 * Bind bus to the I2C device at address. On ESP-IDF the driver adds the
 * device at scl_hz, 0 for 400 kHz; on the HAL the clock is the handle's and
 * scl_hz is ignored. Undo with rc_bus_detach(). Every attach leaves
 * bus->ops NULL when it fails, so a bus with ops is one with something to
 * detach.
 */
int rc_i2c_attach(struct rc_bus *bus,
		   rc_i2c_bus handle,
		   uint8_t address, uint32_t scl_hz, uint32_t timeout_ms);
#endif

#ifdef ESP_PLATFORM
/*
 * Add the SPI device on host's cs_pin, in SPI mode 0-3 at clock_hz. The
 * host is the app's, already spi_bus_initialize()d.
 */
int rc_spi_attach(struct rc_bus *bus,
		   spi_host_device_t host,
		   int cs_pin, uint32_t clock_hz, uint8_t mode);

/* port's driver is the app's, already installed and configured. */
int rc_uart_attach(struct rc_bus *bus, uart_port_t port,
		    uint32_t timeout_ms);
#else
#ifdef HAL_SPI_MODULE_ENABLED
/*
 * Bind bus to the SPI device behind cs_port/cs_pin, and deselect it, so the
 * first transfer starts on a falling edge. Mode, clock and frame are the
 * handle's; the chip select is an output the app has set up.
 */
int rc_spi_attach(struct rc_bus *bus,
		   SPI_HandleTypeDef *handle,
		   GPIO_TypeDef *cs_port, uint16_t cs_pin,
		   uint32_t timeout_ms);
#endif
#ifdef HAL_UART_MODULE_ENABLED
/* handle is the app's, already HAL_UART_Init()ed. */
int rc_uart_attach(struct rc_bus *bus,
		    UART_HandleTypeDef *handle,
		    uint32_t timeout_ms);
#endif
#endif

/*
 * The bits of len bytes of data, least significant bit of data[0] first,
 * as count words of 11 bits: CRSF's and SBUS's 16 channels and the DR16's
 * four (each cited in its <protocol>_proto.h). Only word 0 starts on a
 * byte boundary. The window never holds more than 18 bits, as the inner
 * loop drains it below 11 before the next byte goes in.
 */
static inline void rc_unpack_11bit(const uint8_t *data, uint16_t *words,
				   size_t count)
{
	uint32_t window = 0;
	unsigned held = 0;
	size_t n = 0;

	while (n < count) {
		window |= (uint32_t)*data++ << held;
		held += 8;
		while (held >= 11 && n < count) {
			words[n++] = (uint16_t)(window & 0x7FFu);
			window >>= 11;
			held -= 11;
		}
	}
}

/* ------------------------------------------------------------ input -- */

/* Bytes the ring holds: about 12 ms of CRSF at 420000 baud, 56 ms of SBUS
 * or DBUS at 100000. A power of two. */
#define RC_RING_LEN	512

/*
 * The bytes rc_rx_feed() hands over: one writer, the app's interrupt,
 * through rc_ring_put() and rc_ring_mark_idle(); one reader, rc_update().
 * head and tail run free and wrap; head - tail is what is waiting. The
 * writer fills the bytes before it publishes head, the reader takes them
 * before it publishes tail: acquire and release order that on any core.
 *
 * The idle marks are a second, smaller ring on the same terms: mark n is
 * the head when the line went idle, in idle_at[n % RC_IDLE_MARKS], and
 * idle_count, published after the slot, says how many were ever made.
 * The writer never waits for the reader there: a ninth mark overwrites
 * the first. See rc_update_fed() for why that is safe.
 */
struct rc_ring {
	uint8_t buf[RC_RING_LEN];
	uint32_t head;		/* the writer's: bytes ever put */
	uint32_t tail;		/* the reader's: bytes ever taken */
	uint32_t dropped;	/* the writer's: bytes that found it full */
	uint32_t idle_at[RC_IDLE_MARKS];	/* the writer's: head at each */
	uint32_t idle_count;	/* the writer's: idle marks ever made */
};

size_t rc_ring_put(struct rc_ring *ring, const uint8_t *data, size_t len);
void rc_ring_mark_idle(struct rc_ring *ring);

/* What a protocol's line is; the ESP-IDF init sets the UART to it. */
struct rc_line {
	uint32_t baud;
	bool even_parity;	/* else none */
	bool two_stop_bits;	/* else one */
	bool inverted;		/* idle low: invert RX */
};

/* Where one receiver's bytes come from. */
struct rc_input {
	bool fed;
#ifdef RC_HAVE_BUS
	struct rc_bus bus;	/* !fed: the UART, attached with timeout 0 */
#endif
#ifdef ESP_PLATFORM
	bool installed;		/* !fed: the init installed the UART driver */
#endif
	struct rc_ring ring;	/* fed */
	uint32_t idle_seen;	/* the reader's: idle marks delivered */
};

/*
 * Set input up from the app's port config, for a protocol with the given
 * line. Fed: nothing to open. Read on ESP-IDF: install the UART driver,
 * set the line and the RX pin, attach the bus; on the HAL: attach the
 * app's handle -- its line is the app's. -EOPNOTSUPP for read on a HAL
 * build without the UART module; -EINVAL for no handle. input must be
 * zeroed.
 */
int rc_input_open(struct rc_input *input, const struct rc_port_config *port,
		  const struct rc_line *line);

#ifdef RC_HAVE_BUS
/*
 * The read path: up to len bytes the UART already holds, never waiting.
 * ESP-IDF: what uart_get_buffered_data_len() says is there, in one read.
 * HAL: a byte at a time, timeout 0, until the data register is empty.
 * The count, 0 for none, or a negative errno with nothing read.
 */
int rc_input_read(struct rc_input *input, uint8_t *buf, size_t len);
#endif

/* Undo rc_input_open(): detach, and on ESP-IDF delete the driver. */
int rc_input_close(struct rc_input *input);

/*
 * Fill in the struct rc_dev of a protocol whose handle is priv and whose
 * bytes come from input. timeout_ms 0 is RC_DEFAULT_TIMEOUT_MS.
 */
void rc_dev_init(struct rc_dev *rc, const char *name,
		 const struct rc_ops *ops, void *priv, struct rc_input *input,
		 uint8_t channel_count, uint16_t raw_min, uint16_t raw_mid,
		 uint16_t raw_max, uint32_t timeout_ms);

/* For a protocol's parse: raw[] holds a whole new channel frame. */
static inline void rc_frame_done(struct rc_dev *rc, bool failsafe)
{
	rc->failsafe = failsafe;
	rc->new_frame = true;
	rc->frames++;
}

/* For a protocol's parse: a frame was thrown away. */
static inline void rc_frame_error(struct rc_dev *rc)
{
	rc->errors++;
}

#endif /* RC_PRIV_H */

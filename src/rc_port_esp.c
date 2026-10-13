/*
 * rc_priv.h on ESP-IDF: the bus layer. The CMakeLists
 * lists this file; stm-forge compiles every .c in src/, so outside ESP-IDF
 * it is empty.
 */
#ifdef ESP_PLATFORM

#include <string.h>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "rc.h"
#include "rc_priv.h"

/* 400 kHz when a caller says 0: fast mode. */
#define DEFAULT_SCL_HZ		400000

/*
 * Ticks for at least ms milliseconds, for the bus timeouts. Two things make
 * a plain pdMS_TO_TICKS(ms) wait less than asked:
 *
 *   - pdMS_TO_TICKS() truncates. At IDF's default 100 Hz tick, 2 ms is 0
 *     ticks and 15 ms is 1, so round up instead.
 *   - A wait of n ticks counts tick interrupts, and the first one may be
 *     about to fire, so n ticks can be as little as n - 1 full periods.
 *     Add one.
 *
 * So 20 ms at 100 Hz is 3 ticks, 20-30 ms: long, never short.
 */
static TickType_t rc_ticks(uint32_t ms)
{
	return (TickType_t)(((uint64_t)ms * configTICK_RATE_HZ + 999) / 1000) +
	       1;
}

/* --------------------------------------------------------------- I2C -- */

int rc_i2c_probe(rc_i2c_bus handle, uint8_t address,
		  uint32_t timeout_ms)
{
	return rc_errno(i2c_master_probe(handle, address,
					  (int)timeout_ms));
}

static int rc_i2c_transfer(struct rc_bus *bus,
			    uint8_t cmd,
			    const uint8_t *tx, size_t tx_len, uint8_t *rx,
			    size_t rx_len)
{
	uint8_t out[1 + RC_BUS_MAX_LEN];

	if ((tx_len && rx_len) || tx_len > RC_BUS_MAX_LEN)
		return -EINVAL;
	out[0] = cmd;
	if (rx_len)
		return rc_errno(i2c_master_transmit_receive(
			bus->i2c, out, 1, rx, rx_len, (int)bus->timeout_ms));
	if (tx_len)
		memcpy(out + 1, tx, tx_len);
	return rc_errno(i2c_master_transmit(bus->i2c, out,
					     1 + tx_len,
					     (int)bus->timeout_ms));
}

static void rc_i2c_detach(struct rc_bus *bus)
{
	i2c_master_bus_rm_device(bus->i2c);
}

static const struct rc_bus_ops rc_i2c_ops = {
	.kind = RC_BUS_I2C,
	.transfer = rc_i2c_transfer,
	.detach = rc_i2c_detach,
};

int rc_i2c_attach(struct rc_bus *bus,
		   rc_i2c_bus handle,
		   uint8_t address, uint32_t scl_hz, uint32_t timeout_ms)
{
	i2c_device_config_t devcfg = {
		.dev_addr_length = I2C_ADDR_BIT_LEN_7,
		.device_address = address,
		.scl_speed_hz = scl_hz ? scl_hz : DEFAULT_SCL_HZ,
	};

	int ret;

	ret = rc_errno(i2c_master_bus_add_device(handle, &devcfg,
						  &bus->i2c));
	if (ret)
		return ret;
	/* Set only now: a bus with ops is one that has something to detach. */
	bus->ops = &rc_i2c_ops;
	bus->timeout_ms = timeout_ms;
	return 0;
}

/* --------------------------------------------------------------- SPI -- */

/*
 * One full-duplex transaction, so CS stays low across cmd, tx and rx: clock
 * out cmd and tx followed by zeros, and keep what came back after them. The
 * buffers live on the stack, word-aligned: IDF bounces non-DMA-capable or
 * unaligned buffers through a copy of its own.
 */
static int rc_spi_transfer(struct rc_bus *bus,
			    uint8_t cmd,
			    const uint8_t *tx, size_t tx_len, uint8_t *rx,
			    size_t rx_len)
{
	uint8_t out[1 + RC_BUS_MAX_LEN] __attribute__((aligned(4))) = {
		0
	};
	uint8_t in[1 + RC_BUS_MAX_LEN] __attribute__((aligned(4)));
	spi_transaction_t t = { 0 };
	int ret;

	if (tx_len + rx_len > RC_BUS_MAX_LEN)
		return -EINVAL;
	out[0] = cmd;
	if (tx_len)
		memcpy(out + 1, tx, tx_len);

	t.length = (1 + tx_len + rx_len) * 8;
	t.tx_buffer = out;
	t.rx_buffer = in;
	ret = rc_errno(spi_device_polling_transmit(bus->spi, &t));
	if (!ret && rx_len)
		memcpy(rx, in + 1 + tx_len, rx_len);
	return ret;
}

/*
 * One frame, full duplex: rx[i] is what came back while tx[i] went out.
 * Polled, not queued: for a few bytes the interrupt and task switch of a
 * queued transfer cost more than the transfer itself. A frame of up to four
 * bytes travels in the transaction itself (tx_data, rx_data); a longer one,
 * through word-aligned buffers on the stack.
 */
static int rc_spi_exchange(struct rc_bus *bus,
			    const uint8_t *tx,
			    uint8_t *rx, size_t len)
{
	uint8_t out[1 + RC_BUS_MAX_LEN] __attribute__((aligned(4)));
	uint8_t in[1 + RC_BUS_MAX_LEN] __attribute__((aligned(4)));
	spi_transaction_t t = { 0 };
	bool inline_data = len <= sizeof(t.tx_data);
	int ret;

	if (!len || len > sizeof(out))
		return -EINVAL;

	t.length = len * 8;
	if (inline_data) {
		t.flags = SPI_TRANS_USE_TXDATA | SPI_TRANS_USE_RXDATA;
		memcpy(t.tx_data, tx, len);
	} else {
		memcpy(out, tx, len);
		t.tx_buffer = out;
		t.rx_buffer = in;
	}
	ret = rc_errno(spi_device_polling_transmit(bus->spi, &t));
	if (!ret)
		memcpy(rx, inline_data ? t.rx_data : in, len);
	return ret;
}

static void rc_spi_detach(struct rc_bus *bus)
{
	spi_bus_remove_device(bus->spi);
}

static const struct rc_bus_ops rc_spi_ops = {
	.kind = RC_BUS_SPI,
	.transfer = rc_spi_transfer,
	.exchange = rc_spi_exchange,
	.detach = rc_spi_detach,
};

int rc_spi_attach(struct rc_bus *bus,
		   spi_host_device_t host,
		   int cs_pin, uint32_t clock_hz, uint8_t mode)
{
	spi_device_interface_config_t devcfg = {
		.mode = mode,
		.clock_speed_hz = (int)clock_hz,
		.spics_io_num = cs_pin,
		.queue_size = 1,
	};

	int ret;

	ret = rc_errno(spi_bus_add_device(host, &devcfg,
					   &bus->spi));
	if (ret)
		return ret;
	bus->ops = &rc_spi_ops;
	bus->timeout_ms = 0;	/* spi_master transfers take none */
	return 0;
}

/* -------------------------------------------------------------- UART -- */

/*
 * Drop whatever is waiting -- a late reply to an earlier command would read
 * as the start of this one -- then send, then wait for the whole reply.
 * uart_read_bytes() returns early only with everything; a short count means
 * the timeout ran out.
 */
static int rc_uart_transfer(struct rc_bus *bus,
			     uint8_t cmd,
			     const uint8_t *tx, size_t tx_len,
			     uint8_t *rx, size_t rx_len)
{
	uint8_t out[1 + RC_BUS_MAX_LEN];
	int n, ret;

	if (tx_len > RC_BUS_MAX_LEN || rx_len > INT32_MAX)
		return -EINVAL;
	out[0] = cmd;
	if (tx_len)
		memcpy(out + 1, tx, tx_len);

	ret = rc_errno(uart_flush_input(bus->uart));
	if (ret)
		return ret;
	n = uart_write_bytes(bus->uart, out, 1 + tx_len);
	if (n != (int)(1 + tx_len))
		return -EIO;
	if (!rx_len)
		return 0;
	n = uart_read_bytes(bus->uart, rx, (uint32_t)rx_len,
			    rc_ticks(bus->timeout_ms));
	if (n < 0)
		return -EIO;
	return (size_t)n < rx_len ? -ETIMEDOUT : 0;
}

/*
 * The UART driver keeps everything the line delivers in its RX ring buffer,
 * so the echo of a whole datagram waits there however fast it came: flush,
 * send, wait for the last bit to leave, then read back as many bytes as
 * went out.
 */
static int rc_uart_exchange(struct rc_bus *bus,
			     const uint8_t *tx,
			     uint8_t *rx, size_t len)
{
	int n, ret;

	if (!len || len > 1 + RC_BUS_MAX_LEN)
		return -EINVAL;

	ret = rc_errno(uart_flush_input(bus->uart));
	if (ret)
		return ret;
	n = uart_write_bytes(bus->uart, tx, len);
	if (n != (int)len)
		return -EIO;
	ret = rc_errno(uart_wait_tx_done(bus->uart,
					  rc_ticks(bus->timeout_ms)));
	if (ret)
		return ret;
	n = uart_read_bytes(bus->uart, rx, (uint32_t)len,
			    rc_ticks(bus->timeout_ms));
	if (n < 0)
		return -EIO;
	return (size_t)n < len ? -ETIMEDOUT : 0;
}

static int rc_uart_receive(struct rc_bus *bus,
			    uint8_t *rx,
			    size_t rx_len)
{
	int n;

	if (rx_len > INT32_MAX)
		return -EINVAL;
	n = uart_read_bytes(bus->uart, rx, (uint32_t)rx_len,
			    rc_ticks(bus->timeout_ms));
	if (n < 0)
		return -EIO;
	return (size_t)n < rx_len ? -ETIMEDOUT : 0;
}

/* The driver is the app's: nothing was added, nothing to remove. */
static void rc_uart_detach(struct rc_bus *bus)
{
	(void)bus;
}

static const struct rc_bus_ops rc_uart_ops = {
	.kind = RC_BUS_UART,
	.transfer = rc_uart_transfer,
	.exchange = rc_uart_exchange,
	.receive = rc_uart_receive,
	.detach = rc_uart_detach,
};

int rc_uart_attach(struct rc_bus *bus, uart_port_t port,
		    uint32_t timeout_ms)
{
	if (!uart_is_driver_installed(port))
		return -EINVAL;
	bus->ops = &rc_uart_ops;
	bus->timeout_ms = timeout_ms;
	bus->uart = port;
	return 0;
}
/* ------------------------------------------------------------- input -- */

#define RC_RX_BUFFER_DEFAULT	2048	/* 49 ms of CRSF at 420000 baud */

int rc_input_open(struct rc_input *input, const struct rc_port_config *port,
		  const struct rc_line *line)
{
	uart_config_t config = {
		.baud_rate = (int)line->baud,
		.data_bits = UART_DATA_8_BITS,
		.parity = line->even_parity ? UART_PARITY_EVEN
					    : UART_PARITY_DISABLE,
		.stop_bits = line->two_stop_bits ? UART_STOP_BITS_2
						 : UART_STOP_BITS_1,
		.flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
		.source_clk = UART_SCLK_DEFAULT,
	};
	int ret;

	if (port->fed) {
		input->fed = true;
		return 0;
	}
	if (port->rx_buffer_len < 0)
		return -EINVAL;

	/* No TX buffer: nothing is sent to the receiver. */
	ret = rc_errno(uart_driver_install(port->uart,
					   port->rx_buffer_len ?
					   port->rx_buffer_len :
					   RC_RX_BUFFER_DEFAULT,
					   0, 0, NULL, 0));
	if (ret)
		return ret;
	input->installed = true;
	ret = rc_errno(uart_param_config(port->uart, &config));
	if (!ret)
		ret = rc_errno(uart_set_pin(port->uart, UART_PIN_NO_CHANGE,
					    port->rx_gpio, UART_PIN_NO_CHANGE,
					    UART_PIN_NO_CHANGE));
	if (!ret && line->inverted)
		ret = rc_errno(uart_set_line_inverse(port->uart,
						     UART_SIGNAL_RXD_INV));
	if (!ret)
		ret = rc_uart_attach(&input->bus, port->uart, 0);
	if (ret) {
		uart_driver_delete(port->uart);
		input->installed = false;
	}
	return ret;
}

/*
 * The bus layer's receive() throws away a short read, so the read path asks
 * the driver what it holds and reads exactly that, with 0 ticks: none of it
 * is lost, and a frame cut in two stays with the protocol's parser.
 */
int rc_input_read(struct rc_input *input, uint8_t *buf, size_t len)
{
	size_t held = 0;
	int ret, n;

	if (input->fed || !input->bus.ops)
		return -EINVAL;
	ret = rc_errno(uart_get_buffered_data_len(input->bus.uart, &held));
	if (ret)
		return ret;
	if (held > len)
		held = len;
	if (!held)
		return 0;
	n = uart_read_bytes(input->bus.uart, buf, (uint32_t)held, 0);
	return n < 0 ? -EIO : n;
}

int rc_input_close(struct rc_input *input)
{
	uart_port_t port = input->bus.uart;

	if (input->fed || !input->installed)
		return 0;
	if (input->bus.ops)
		rc_bus_detach(&input->bus);
	input->installed = false;
	return rc_errno(uart_driver_delete(port));
}

#endif /* ESP_PLATFORM */

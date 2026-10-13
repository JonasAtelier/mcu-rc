/*
 * Host-side check of src/rc_port_esp.c against test/idf_stubs: the bus
 * ops -- I2C, presence probe included, SPI and UART -- which every category
 * library carries, and rc_input_open(), which installs and sets the UART a
 * receiver is read through.
 */
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "driver/i2c_master.h"
#include "driver/spi_master.h"
#include "driver/uart.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "rc.h"
#include "rc_priv.h"

#define UART_PORT 1

extern TickType_t stub_ticks, stub_tick_step;

static esp_err_t flush_err, done_err;
static bool installed;
static char calls[16];		/* F flush, W write, D wait done, R read,
				 * I install, P param, N pin, V inverse,
				 * X delete */
static uint8_t out[40];		/* what the last write sent */
static size_t out_len;
static uint8_t in[64];		/* what is waiting to be read */
static size_t in_len;
static uint8_t reply[32];	/* what the device sends after a write */
static size_t reply_len;
static bool echo;		/* the line gives every byte written back */
static TickType_t waited;	/* ticks_to_wait of the last read */
static TickType_t done_waited;	/* ticks_to_wait of the last wait done */
static int write_short, read_error;

static void reset(void)
{
	installed = true;
	calls[0] = '\0';
	out_len = in_len = reply_len = 0;
	echo = false;
	write_short = read_error = 0;
	flush_err = done_err = ESP_OK;
}

static void called(char c)
{
	size_t n = strlen(calls);

	assert(n + 1 < sizeof(calls));
	calls[n] = c;
	calls[n + 1] = '\0';
}

bool uart_is_driver_installed(uart_port_t port)
{
	return installed && port == UART_PORT;
}

esp_err_t uart_flush_input(uart_port_t port)
{
	assert(port == UART_PORT);
	called('F');
	if (flush_err != ESP_OK)
		return flush_err;
	in_len = 0;
	return ESP_OK;
}

int uart_write_bytes(uart_port_t port, const void *src, size_t size)
{
	assert(port == UART_PORT && size <= sizeof(out));
	called('W');
	if (write_short)
		return 0;
	memcpy(out, src, size);
	out_len = size;
	/* On a single wire the bytes come straight back, then the device
	 * answers after them: queue both. */
	if (echo) {
		memcpy(in + in_len, src, size);
		in_len += size;
	}
	memcpy(in + in_len, reply, reply_len);
	in_len += reply_len;
	return (int)size;
}

esp_err_t uart_wait_tx_done(uart_port_t port, TickType_t ticks_to_wait)
{
	assert(port == UART_PORT);
	called('D');
	done_waited = ticks_to_wait;
	return done_err;
}

/* The driver install and line setup rc_input_open() does. */
static int install_rx_len;
static esp_err_t install_err, param_err, inverse_err;
static uart_config_t param;
static int pin_rx = -2;
static uint32_t inverse_mask;

esp_err_t uart_driver_install(uart_port_t port, int rx_buffer_size,
			      int tx_buffer_size, int queue_size,
			      QueueHandle_t *uart_queue, int intr_alloc_flags)
{
	assert(port == UART_PORT && tx_buffer_size == 0 && queue_size == 0 &&
	       !uart_queue && intr_alloc_flags == 0);
	called('I');
	if (install_err != ESP_OK)
		return install_err;
	install_rx_len = rx_buffer_size;
	installed = true;
	return ESP_OK;
}

esp_err_t uart_driver_delete(uart_port_t port)
{
	assert(port == UART_PORT && installed);
	called('X');
	installed = false;
	return ESP_OK;
}

esp_err_t uart_param_config(uart_port_t port, const uart_config_t *config)
{
	assert(port == UART_PORT);
	called('P');
	param = *config;
	return param_err;
}

esp_err_t uart_set_pin(uart_port_t port, int tx, int rx, int rts, int cts)
{
	assert(port == UART_PORT && tx == UART_PIN_NO_CHANGE &&
	       rts == UART_PIN_NO_CHANGE && cts == UART_PIN_NO_CHANGE);
	called('N');
	pin_rx = rx;
	return ESP_OK;
}

esp_err_t uart_set_line_inverse(uart_port_t port, uint32_t mask)
{
	assert(port == UART_PORT);
	called('V');
	inverse_mask = mask;
	return inverse_err;
}

int uart_read_bytes(uart_port_t port, void *buf, uint32_t length,
		    TickType_t ticks_to_wait)
{
	size_t n = length < in_len ? length : in_len;

	assert(port == UART_PORT);
	called('R');
	waited = ticks_to_wait;
	if (read_error)
		return -1;
	memcpy(buf, in, n);
	memmove(in, in + n, in_len - n);
	in_len -= n;
	return (int)n;
}

/* The SPI device: what was added, the last transaction, the next answer. */
static spi_device_interface_config_t spi_cfg;
static int spi_added;
static esp_err_t spi_err;
static uint8_t spi_out[40];	/* what the last transaction clocked out */
static size_t spi_bits;
static bool spi_inline;		/* the last transaction used tx_data/rx_data */
static uint8_t spi_in[40];	/* what the device clocks back */

esp_err_t spi_bus_add_device(spi_host_device_t host,
			     const spi_device_interface_config_t *config,
			     spi_device_handle_t *out_handle)
{
	assert(host == SPI2_HOST);
	spi_cfg = *config;
	spi_added++;
	*out_handle = (spi_device_handle_t)(void *)&spi_cfg;
	return spi_err;
}

esp_err_t spi_bus_remove_device(spi_device_handle_t device)
{
	assert(device == (spi_device_handle_t)(void *)&spi_cfg);
	spi_added--;
	return ESP_OK;
}

esp_err_t spi_device_polling_transmit(spi_device_handle_t device,
				      spi_transaction_t *transfer)
{
	size_t n = transfer->length / 8;

	assert(device == (spi_device_handle_t)(void *)&spi_cfg);
	assert(transfer->length % 8 == 0 && n <= sizeof(spi_out));
	assert(transfer->rxlength == 0 && "rx as long as tx");
	if (spi_err != ESP_OK)
		return spi_err;
	spi_inline = transfer->flags != 0;
	if (spi_inline) {
		/* Up to four bytes in the transaction itself, both ways. */
		assert(transfer->flags ==
		       (SPI_TRANS_USE_TXDATA | SPI_TRANS_USE_RXDATA) && n <= 4);
		memcpy(spi_out, transfer->tx_data, n);
		memcpy(transfer->rx_data, spi_in, n);
	} else {
		memcpy(spi_out, transfer->tx_buffer, n);
		memcpy(transfer->rx_buffer, spi_in, n);
	}
	spi_bits = transfer->length;
	return ESP_OK;
}

/* The I2C device: what was added, the last write, the next answer. */
static i2c_device_config_t i2c_cfg;
static int i2c_added;
static esp_err_t i2c_err;
static uint8_t i2c_out[40];
static size_t i2c_out_len, i2c_rx_len;

esp_err_t i2c_master_bus_add_device(i2c_master_bus_handle_t bus,
				    const i2c_device_config_t *config,
				    i2c_master_dev_handle_t *out_handle)
{
	(void)bus;
	i2c_cfg = *config;
	i2c_added++;
	*out_handle = (i2c_master_dev_handle_t)(void *)&i2c_cfg;
	return i2c_err;
}

esp_err_t i2c_master_bus_rm_device(i2c_master_dev_handle_t device)
{
	assert(device == (i2c_master_dev_handle_t)(void *)&i2c_cfg);
	i2c_added--;
	return ESP_OK;
}

esp_err_t i2c_master_transmit(i2c_master_dev_handle_t device,
			      const uint8_t *data, size_t length,
			      int timeout_ms)
{
	(void)device;
	(void)timeout_ms;
	memcpy(i2c_out, data, length);
	i2c_out_len = length;
	i2c_rx_len = 0;
	return i2c_err;
}

esp_err_t i2c_master_transmit_receive(i2c_master_dev_handle_t device,
				      const uint8_t *tx, size_t tx_length,
				      uint8_t *rx, size_t rx_length,
				      int timeout_ms)
{
	(void)device;
	(void)timeout_ms;
	memcpy(i2c_out, tx, tx_length);
	i2c_out_len = tx_length;
	i2c_rx_len = rx_length;
	memcpy(rx, reply, rx_length);
	return i2c_err;
}

/* What i2c_master_probe() answers, and where it was asked. */
static esp_err_t probe_err;
static uint16_t probe_addr;
static int probe_timeout;

esp_err_t i2c_master_probe(i2c_master_bus_handle_t bus, uint16_t address,
			   int xfer_timeout_ms)
{
	(void)bus;
	probe_addr = address;
	probe_timeout = xfer_timeout_ms;
	return probe_err;
}

static void test_i2c(void)
{
	static const uint8_t args[2] = { 0x11, 0x22 };
	struct rc_bus bus = { 0 };
	uint8_t rx[2] = { 0 };

	/* A refused add leaves no ops: nothing to detach. */
	i2c_err = ESP_ERR_NO_MEM;
	assert(rc_i2c_attach(&bus, NULL, 0x2A, 0, 10) == -ENOMEM);
	assert(!bus.ops);
	i2c_added = 0;
	i2c_err = ESP_OK;

	assert(rc_i2c_attach(&bus, NULL, 0x2A, 0, 10) == 0);
	assert(bus.ops->kind == RC_BUS_I2C && i2c_added == 1);
	assert(i2c_cfg.device_address == 0x2A &&
	       i2c_cfg.scl_speed_hz == 400000);

	reset();
	memcpy(reply, "\x12\x34", 2);
	assert(bus.ops->transfer(&bus, 0x0F, NULL, 0, rx, 2) == 0);
	assert(i2c_out_len == 1 && i2c_out[0] == 0x0F && i2c_rx_len == 2);
	assert(rx[0] == 0x12 && rx[1] == 0x34);
	assert(bus.ops->transfer(&bus, 0x20, args, 2, NULL, 0) == 0);
	assert(i2c_out_len == 3 && i2c_out[0] == 0x20 && i2c_out[2] == 0x22);
	assert(bus.ops->transfer(&bus, 0x20, args, 2, rx, 1) == -EINVAL);

	/* Neither exchange() nor receive() on I2C. */
	assert(rc_bus_exchange(&bus, args, rx, 1) == -EOPNOTSUPP);
	assert(rc_bus_receive(&bus, rx, 1) == -EOPNOTSUPP);

	rc_bus_detach(&bus);
	assert(i2c_added == 0);
}

static void test_spi(void)
{
	static const uint8_t args[2] = { 0x11, 0x22 };
	uint8_t many[RC_BUS_MAX_LEN] = { 0 };
	struct rc_bus bus = { 0 };
	uint8_t rx[3] = { 0 };

	/* A refused add leaves no ops: nothing to detach. */
	spi_err = ESP_ERR_INVALID_ARG;
	assert(rc_spi_attach(&bus, SPI2_HOST, 5, 1000000, 3) == -EINVAL);
	assert(!bus.ops && spi_added == 1);
	spi_added = 0;
	spi_err = ESP_OK;

	assert(rc_spi_attach(&bus, SPI2_HOST, 5, 4000000, 3) == 0);
	assert(bus.ops->kind == RC_BUS_SPI && spi_added == 1);
	assert(spi_cfg.mode == 3 && spi_cfg.clock_speed_hz == 4000000 &&
	       spi_cfg.spics_io_num == 5);

	/* A read: cmd, then zeros, and what came back after cmd. */
	memcpy(spi_in, "\xFF\x12\x34\x56", 4);
	memset(spi_out, 0xAA, sizeof(spi_out));
	assert(bus.ops->transfer(&bus, 0x8D, NULL, 0, rx, 3) == 0);
	assert(spi_bits == 4 * 8);
	assert(spi_out[0] == 0x8D && spi_out[1] == 0 && spi_out[2] == 0 &&
	       spi_out[3] == 0);
	assert(rx[0] == 0x12 && rx[1] == 0x34 && rx[2] == 0x56);

	/* A write: cmd and tx, nothing kept. */
	assert(bus.ops->transfer(&bus, 0x0D, args, 2, NULL, 0) == 0);
	assert(spi_bits == 3 * 8);
	assert(spi_out[0] == 0x0D && spi_out[1] == 0x11 && spi_out[2] == 0x22);

	/* Too long, counting tx and rx together; a failure is mapped. */
	assert(bus.ops->transfer(&bus, 0x0D, many, sizeof(many), rx, 1) ==
	       -EINVAL);
	rx[0] = 0x99;
	spi_err = ESP_ERR_TIMEOUT;
	assert(bus.ops->transfer(&bus, 0x8D, NULL, 0, rx, 1) == -ETIMEDOUT);
	assert(rx[0] == 0x99 && "rx untouched on failure");
	spi_err = ESP_OK;

	/* exchange(): one frame, rx[i] clocked in while tx[i] goes out. */
	{
		static const uint8_t two[2] = { 0xFF, 0xFC };
		static const uint8_t five[5] = { 0xA1, 1, 2, 3, 4 };
		uint8_t back[5] = { 0 };
		uint8_t big[2 + RC_BUS_MAX_LEN] = { 0 };

		memcpy(spi_in, "\x9A\xBC", 2);
		assert(rc_bus_exchange(&bus, two, back, 2) == 0);
		assert(spi_inline && spi_bits == 16);
		assert(spi_out[0] == 0xFF && spi_out[1] == 0xFC);
		assert(back[0] == 0x9A && back[1] == 0xBC);

		/* Five bytes go through buffers. */
		memcpy(spi_in, "\x09\xDE\xAD\xBE\xEF", 5);
		assert(rc_bus_exchange(&bus, five, back, 5) == 0);
		assert(!spi_inline && spi_bits == 40);
		assert(memcmp(spi_out, five, 5) == 0);
		assert(memcmp(back, "\x09\xDE\xAD\xBE\xEF", 5) == 0);

		assert(rc_bus_exchange(&bus, two, back, 0) == -EINVAL);
		assert(rc_bus_exchange(&bus, big, big, sizeof(big)) ==
		       -EINVAL);
		back[0] = 0x55;
		spi_err = ESP_ERR_TIMEOUT;
		assert(rc_bus_exchange(&bus, five, back, 5) ==
		       -ETIMEDOUT);
		assert(back[0] == 0x55 && "rx untouched on failure");
		spi_err = ESP_OK;
	}

	/* No receive() on SPI. */
	assert(rc_bus_receive(&bus, rx, 1) == -EOPNOTSUPP);
	rc_bus_detach(&bus);
	assert(spi_added == 0);
}

static void test_uart(void)
{
	static const uint8_t args[2] = { 0x11, 0x22 };
	uint8_t many[RC_BUS_MAX_LEN + 1] = { 0 };
	struct rc_bus bus;
	uint8_t rx[2] = { 0 };

	/* The driver is the app's to install; without it, refuse. */
	reset();
	installed = false;
	assert(rc_uart_attach(&bus, UART_PORT, 100) == -EINVAL);
	installed = true;
	assert(rc_uart_attach(&bus, UART_PORT, 100) == 0);
	assert(bus.ops->kind == RC_BUS_UART);

	/* A stale byte is dropped, then cmd and args go out, then the reply. */
	in[0] = 0xEE;
	in_len = 1;
	memcpy(reply, "\x12\x34", 2);
	reply_len = 2;
	assert(bus.ops->transfer(&bus, 0x13, args, 2, rx, 2) == 0);
	assert(strcmp(calls, "FWR") == 0);
	assert(out_len == 3 && out[0] == 0x13 && out[1] == 0x11 &&
	       out[2] == 0x22);
	assert(rx[0] == 0x12 && rx[1] == 0x34);
	/* 100 ms at the stub's 100 Hz: 10 ticks, plus one for the first. */
	assert(waited == 11);

	/* A command with no reply does not read. */
	reset();
	assert(bus.ops->transfer(&bus, 0x80, NULL, 0, NULL, 0) == 0);
	assert(strcmp(calls, "FW") == 0);

	/* A reply that stops short is a timeout. */
	reset();
	reply[0] = 0x12;
	reply_len = 1;
	assert(bus.ops->transfer(&bus, 0x13, NULL, 0, rx, 2) == -ETIMEDOUT);

	/* Driver failures. */
	reset();
	read_error = 1;
	assert(bus.ops->transfer(&bus, 0x13, NULL, 0, rx, 2) == -EIO);
	reset();
	write_short = 1;
	assert(bus.ops->transfer(&bus, 0x13, NULL, 0, rx, 2) == -EIO);
	assert(strcmp(calls, "FW") == 0);
	reset();
	flush_err = ESP_ERR_INVALID_ARG;
	assert(bus.ops->transfer(&bus, 0x13, NULL, 0, rx, 2) == -EINVAL);
	assert(strcmp(calls, "F") == 0);
	reset();
	assert(bus.ops->transfer(&bus, 0x13, many, sizeof(many), NULL, 0) ==
	       -EINVAL);
	assert(calls[0] == '\0');

	rc_bus_detach(&bus);
}

/*
 * A single wire, where the MCU hears itself: exchange() flushes, sends,
 * waits for the last bit to leave and reads back as many bytes as went out;
 * receive() then takes the reply that follows, nothing flushed in between.
 */
static void test_uart_echo(void)
{
	static const uint8_t req[4] = { 0x05, 0x00, 0x02, 0x3F };
	uint8_t echo_back[4] = { 0 }, answer[8] = { 0 };
	uint8_t big[2 + RC_BUS_MAX_LEN] = { 0 };
	struct rc_bus bus;

	reset();
	assert(rc_uart_attach(&bus, UART_PORT, 20) == 0);

	in[0] = 0xEE;
	in_len = 1;
	echo = true;
	memcpy(reply, "\x05\xFF\x02\x00\x00\x00\x07\x99", 8);
	reply_len = 8;
	assert(rc_bus_exchange(&bus, req, echo_back, 4) == 0);
	assert(strcmp(calls, "FWDR") == 0);
	assert(memcmp(echo_back, req, 4) == 0);
	/* 20 ms at 100 Hz: 2 ticks rounded up, plus one. */
	assert(done_waited == 3 && waited == 3);
	assert(rc_bus_receive(&bus, answer, 8) == 0);
	assert(strcmp(calls, "FWDRR") == 0);
	assert(memcmp(answer, reply, 8) == 0 && waited == 3);

	/* A short echo, or a short answer, is a timeout. */
	reset();
	assert(rc_bus_exchange(&bus, req, echo_back, 4) == -ETIMEDOUT);
	reset();
	echo = true;
	assert(rc_bus_exchange(&bus, req, echo_back, 4) == 0);
	assert(rc_bus_receive(&bus, answer, 8) == -ETIMEDOUT);

	/* The last bit never leaving is a timeout, before any read. */
	reset();
	done_err = ESP_ERR_TIMEOUT;
	assert(rc_bus_exchange(&bus, req, echo_back, 4) == -ETIMEDOUT);
	assert(strcmp(calls, "FWD") == 0);

	/* Driver failures; nothing, or too much, refused first. */
	reset();
	write_short = 1;
	assert(rc_bus_exchange(&bus, req, echo_back, 4) == -EIO);
	reset();
	read_error = 1;
	echo = true;
	assert(rc_bus_exchange(&bus, req, echo_back, 4) == -EIO);
	assert(rc_bus_receive(&bus, answer, 8) == -EIO);
	reset();
	assert(rc_bus_exchange(&bus, req, echo_back, 0) == -EINVAL);
	assert(rc_bus_exchange(&bus, big, big, sizeof(big)) == -EINVAL);
	assert(calls[0] == '\0');

	rc_bus_detach(&bus);
}

static void test_input_open(void)
{
	static const struct rc_line sbus = {
		.baud = 100000, .even_parity = true, .two_stop_bits = true,
		.inverted = true,
	};
	static const struct rc_line crsf = { .baud = 420000 };
	struct rc_port_config port = { .uart = UART_PORT, .rx_gpio = 16 };
	struct rc_input input;

	/* Read: install, set the line, the pin, the inversion, attach. */
	reset();
	installed = false;
	memset(&input, 0, sizeof(input));
	assert(rc_input_open(&input, &port, &sbus) == 0);
	assert(strcmp(calls, "IPNV") == 0);
	assert(install_rx_len == 2048 && pin_rx == 16);
	assert(param.baud_rate == 100000 && param.data_bits == UART_DATA_8_BITS &&
	       param.parity == UART_PARITY_EVEN &&
	       param.stop_bits == UART_STOP_BITS_2 &&
	       param.flow_ctrl == UART_HW_FLOWCTRL_DISABLE);
	assert(inverse_mask == UART_SIGNAL_RXD_INV);
	assert(!input.fed && input.installed && input.bus.ops &&
	       input.bus.ops->kind == RC_BUS_UART && input.bus.timeout_ms == 0);
	assert(rc_input_close(&input) == 0);
	assert(strcmp(calls, "IPNVX") == 0 && !installed);
	assert(rc_input_close(&input) == 0);	/* twice is harmless */
	assert(strcmp(calls, "IPNVX") == 0);

	/* 8N1, not inverted: no inverse call; the buffer as asked. */
	reset();
	installed = false;
	memset(&input, 0, sizeof(input));
	port.rx_buffer_len = 512;
	assert(rc_input_open(&input, &port, &crsf) == 0);
	assert(strcmp(calls, "IPN") == 0 && install_rx_len == 512);
	assert(param.baud_rate == 420000 &&
	       param.parity == UART_PARITY_DISABLE &&
	       param.stop_bits == UART_STOP_BITS_1);
	assert(rc_input_close(&input) == 0 && !installed);

	/* A failure after the install deletes the driver again. */
	reset();
	installed = false;
	memset(&input, 0, sizeof(input));
	inverse_err = ESP_ERR_INVALID_ARG;
	assert(rc_input_open(&input, &port, &sbus) == -EINVAL);
	assert(strcmp(calls, "IPNVX") == 0 && !installed && !input.installed);
	inverse_err = ESP_OK;
	reset();
	installed = false;
	param_err = ESP_FAIL;
	assert(rc_input_open(&input, &port, &sbus) == -EIO);
	assert(strcmp(calls, "IPX") == 0 && !installed);
	param_err = ESP_OK;
	reset();
	installed = false;
	install_err = ESP_ERR_NO_MEM;
	assert(rc_input_open(&input, &port, &sbus) == -ENOMEM);
	assert(strcmp(calls, "I") == 0);
	install_err = ESP_OK;
	port.rx_buffer_len = -1;
	assert(rc_input_open(&input, &port, &sbus) == -EINVAL);
	port.rx_buffer_len = 0;

	/* Fed: the UART is never touched. */
	reset();
	memset(&input, 0, sizeof(input));
	port.fed = true;
	assert(rc_input_open(&input, &port, &sbus) == 0);
	assert(input.fed && calls[0] == '\0');
	assert(rc_input_close(&input) == 0 && calls[0] == '\0');
}

static void test_probe(void)
{
	/* Nothing ACKs: -ENXIO. */
	probe_err = ESP_ERR_NOT_FOUND;
	assert(rc_i2c_probe(NULL, 0x2A, 100) == -ENXIO);
	assert(probe_addr == 0x2A && probe_timeout == 100);
	probe_err = ESP_ERR_TIMEOUT;
	assert(rc_i2c_probe(NULL, 0x2A, 100) == -ETIMEDOUT);
	probe_err = ESP_OK;
	assert(rc_i2c_probe(NULL, 0x2A, 100) == 0);
}

int main(void)
{
	test_i2c();
	test_spi();
	test_uart();
	test_uart_echo();
	test_input_open();
	test_probe();
	printf("test_port_esp: all passed\n");
	return 0;
}

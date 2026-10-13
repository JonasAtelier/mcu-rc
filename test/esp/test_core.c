/*
 * Host-side check of the core on ESP-IDF: the cases are ../core_cases.h; this
 * file supplies the log prefix and a fake UART for the read path.
 */
#include <assert.h>
#include <errno.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "driver/uart.h"
#include "esp_log.h"

#include "rc.h"
#include "rc_log.h"
#include "rc_priv.h"

/* The ESP-IDF stub log puts the level letter first. */
#define LOG_PREFIX(level)	level " "

#define UART_PORT	2

static uint8_t line[512];
static size_t line_len;
static int line_fail;
static bool installed;
static int read_calls;

static void line_push(const uint8_t *data, size_t len)
{
	assert(line_len + len <= sizeof(line));
	memcpy(line + line_len, data, len);
	line_len += len;
}

static struct rc_port_config read_port(void)
{
	return (struct rc_port_config){ .uart = UART_PORT, .rx_gpio = 4 };
}

esp_err_t uart_driver_install(uart_port_t port, int rx_buffer_size,
			      int tx_buffer_size, int queue_size,
			      QueueHandle_t *uart_queue, int intr_alloc_flags)
{
	(void)rx_buffer_size;
	(void)tx_buffer_size;
	(void)queue_size;
	(void)uart_queue;
	(void)intr_alloc_flags;
	assert(port == UART_PORT && !installed);
	installed = true;
	return ESP_OK;
}

esp_err_t uart_driver_delete(uart_port_t port)
{
	assert(port == UART_PORT && installed);
	installed = false;
	return ESP_OK;
}

esp_err_t uart_param_config(uart_port_t port, const uart_config_t *config)
{
	assert(port == UART_PORT && config->baud_rate == 115200);
	return ESP_OK;
}

esp_err_t uart_set_pin(uart_port_t port, int tx, int rx, int rts, int cts)
{
	(void)tx;
	(void)rts;
	(void)cts;
	assert(port == UART_PORT && rx == 4);
	return ESP_OK;
}

bool uart_is_driver_installed(uart_port_t port)
{
	return port == UART_PORT && installed;
}

esp_err_t uart_get_buffered_data_len(uart_port_t port, size_t *size)
{
	assert(port == UART_PORT);
	*size = line_len;
	return ESP_OK;
}

/* The library polls: every read here must be a 0-tick one, and asks for
 * no more than the driver said it holds. */
int uart_read_bytes(uart_port_t port, void *buf, uint32_t length,
		    TickType_t ticks_to_wait)
{
	size_t n = length < line_len ? length : line_len;

	assert(port == UART_PORT && ticks_to_wait == 0);
	assert(length > 0 && length <= line_len);
	read_calls++;
	if (line_fail)
		return -1;
	memcpy(buf, line, n);
	memmove(line, line + n, line_len - n);
	line_len -= n;
	return (int)n;
}

#include "../core_cases.h"

/* ESP-IDF reads what the driver holds in chunks, not a byte at a time, and
 * not at all when it holds nothing. */
static void test_read_in_batches(void)
{
	struct rc_port_config port = read_port();
	static const struct rc_line line_settings = { .baud = 115200 };
	uint8_t many[150];

	memset(&fake, 0, sizeof(fake));
	assert(rc_input_open(&fake.input, &port, &line_settings) == 0);
	rc_dev_init(&fake.rc, "fake", &fake_ops, &fake, &fake.input, 4, 172,
		    992, 1811, 0);

	read_calls = 0;
	assert(rc_update(&fake.rc, 0) == -EAGAIN && read_calls == 0);
	memset(many, 'x', sizeof(many));
	line_push(many, sizeof(many));
	assert(rc_update(&fake.rc, 0) == -EAGAIN);
	assert(read_calls == 3 && fake.seen_len == 150);	/* 64, 64, 22 */
	assert(rc_input_close(&fake.input) == 0);
}

int main(void)
{
	run_core_cases();
	test_read_in_batches();
	assert(!installed);
	printf("test_core: all passed\n");
	return 0;
}

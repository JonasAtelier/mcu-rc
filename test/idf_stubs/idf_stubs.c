/* The stub definitions every test binary links. */
#include <assert.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>

#include "driver/i2c_master.h"
#include "driver/spi_master.h"
#include "driver/uart.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

char stub_log_last[256];
int stub_critical, stub_critical_entries;

/*
 * ESP-IDF's default CONFIG_FREERTOS_HZ. Weak, so a test can define its own.
 */
__attribute__((weak)) unsigned int stub_tick_hz = 100;

void stub_log(char level, const char *tag, const char *fmt, ...)
{
	va_list ap;
	int n;

	/* ESP-IDF logging may block; never under a spinlock. */
	assert(!stub_critical);
	n = snprintf(stub_log_last, sizeof(stub_log_last), "%c %s: ", level,
		     tag);
	va_start(ap, fmt);
	vsnprintf(stub_log_last + n, sizeof(stub_log_last) - n, fmt, ap);
	va_end(ap);
	fprintf(stderr, "%s\n", stub_log_last);
}

/*
 * A scheduler of one task: time passes in vTaskDelay(), and also by
 * stub_tick_step every time xTaskGetTickCount() looks, so that a deadline
 * loop still ends in a test whose own vTaskDelay() does nothing -- the
 * original drivers' tests define one. A test that wants exact time sets
 * stub_tick_step to 0. Weak, so a test that wants its own clock defines
 * them.
 */
TickType_t stub_ticks;
TickType_t stub_tick_step = 1;

__attribute__((weak)) void vTaskDelay(TickType_t ticks)
{
	stub_ticks += ticks;
}

__attribute__((weak)) TickType_t xTaskGetTickCount(void)
{
	TickType_t now = stub_ticks;

	stub_ticks += stub_tick_step;
	return now;
}

/*
 * src/rc_port_esp.c is in every binary and calls the bus and UART ones.
 * A test with a fake chip defines the ones it needs, which win over these
 * weak ones; a test that reaches one it did not define has a bug, so stop it
 * there.
 */
__attribute__((weak)) esp_err_t
i2c_master_bus_add_device(i2c_master_bus_handle_t bus,
			  const i2c_device_config_t *config,
			  i2c_master_dev_handle_t *out_handle)
{
	(void)bus;
	(void)config;
	(void)out_handle;
	abort();
}

__attribute__((weak)) esp_err_t
i2c_master_bus_rm_device(i2c_master_dev_handle_t device)
{
	(void)device;
	abort();
}

/* Something answers at every address, unless a test says otherwise. */
__attribute__((weak)) esp_err_t
i2c_master_probe(i2c_master_bus_handle_t bus, uint16_t address,
		 int xfer_timeout_ms)
{
	(void)bus;
	(void)address;
	(void)xfer_timeout_ms;
	return ESP_OK;
}

__attribute__((weak)) esp_err_t
i2c_master_transmit(i2c_master_dev_handle_t device, const uint8_t *data,
		    size_t length, int timeout_ms)
{
	(void)device;
	(void)data;
	(void)length;
	(void)timeout_ms;
	abort();
}

__attribute__((weak)) esp_err_t
i2c_master_transmit_receive(i2c_master_dev_handle_t device,
			    const uint8_t *tx, size_t tx_length, uint8_t *rx,
			    size_t rx_length, int timeout_ms)
{
	(void)device;
	(void)tx;
	(void)tx_length;
	(void)rx;
	(void)rx_length;
	(void)timeout_ms;
	abort();
}

__attribute__((weak)) esp_err_t
spi_bus_add_device(spi_host_device_t host,
		   const spi_device_interface_config_t *config,
		   spi_device_handle_t *out_handle)
{
	(void)host;
	(void)config;
	(void)out_handle;
	abort();
}

__attribute__((weak)) esp_err_t
spi_bus_remove_device(spi_device_handle_t device)
{
	(void)device;
	abort();
}

__attribute__((weak)) esp_err_t
spi_device_polling_transmit(spi_device_handle_t device,
			    spi_transaction_t *transfer)
{
	(void)device;
	(void)transfer;
	abort();
}

__attribute__((weak)) esp_err_t
uart_driver_install(uart_port_t uart_num, int rx_buffer_size,
		    int tx_buffer_size, int queue_size, QueueHandle_t *uart_queue,
		    int intr_alloc_flags)
{
	(void)uart_num;
	(void)rx_buffer_size;
	(void)tx_buffer_size;
	(void)queue_size;
	(void)uart_queue;
	(void)intr_alloc_flags;
	abort();
}

__attribute__((weak)) esp_err_t
uart_driver_delete(uart_port_t uart_num)
{
	(void)uart_num;
	abort();
}

__attribute__((weak)) esp_err_t
uart_param_config(uart_port_t uart_num, const uart_config_t *uart_config)
{
	(void)uart_num;
	(void)uart_config;
	abort();
}

__attribute__((weak)) esp_err_t
uart_set_pin(uart_port_t uart_num, int tx_io_num, int rx_io_num,
	     int rts_io_num, int cts_io_num)
{
	(void)uart_num;
	(void)tx_io_num;
	(void)rx_io_num;
	(void)rts_io_num;
	(void)cts_io_num;
	abort();
}

__attribute__((weak)) esp_err_t
uart_set_line_inverse(uart_port_t uart_num, uint32_t inverse_mask)
{
	(void)uart_num;
	(void)inverse_mask;
	abort();
}

__attribute__((weak)) bool
uart_is_driver_installed(uart_port_t uart_num)
{
	(void)uart_num;
	abort();
}

__attribute__((weak)) esp_err_t
uart_get_buffered_data_len(uart_port_t uart_num, size_t *size)
{
	(void)uart_num;
	(void)size;
	abort();
}

__attribute__((weak)) esp_err_t
uart_flush_input(uart_port_t uart_num)
{
	(void)uart_num;
	abort();
}

__attribute__((weak)) int
uart_write_bytes(uart_port_t uart_num, const void *src, size_t size)
{
	(void)uart_num;
	(void)src;
	(void)size;
	abort();
}

__attribute__((weak)) esp_err_t
uart_wait_tx_done(uart_port_t uart_num, TickType_t ticks_to_wait)
{
	(void)uart_num;
	(void)ticks_to_wait;
	abort();
}

__attribute__((weak)) int
uart_read_bytes(uart_port_t uart_num, void *buf, uint32_t length,
		TickType_t ticks_to_wait)
{
	(void)uart_num;
	(void)buf;
	(void)length;
	(void)ticks_to_wait;
	abort();
}

__attribute__((weak)) const char *esp_err_to_name(esp_err_t code)
{
	return code == ESP_OK ? "ESP_OK" : "ESP_ERR";
}

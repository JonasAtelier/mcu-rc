#ifndef UART_H
#define UART_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"

/*
 * Host stub for the slice of the ESP-IDF UART driver API the library uses.
 * Names and values as in IDF v5.5.5's hal/uart_types.h and driver/uart.h;
 * uart_port_t is an int here, an enum there.
 */

typedef int uart_port_t;

typedef enum {
	UART_DATA_8_BITS = 0x3,
} uart_word_length_t;

typedef enum {
	UART_PARITY_DISABLE = 0x0,
	UART_PARITY_EVEN = 0x2,
} uart_parity_t;

typedef enum {
	UART_STOP_BITS_1 = 0x1,
	UART_STOP_BITS_2 = 0x3,
} uart_stop_bits_t;

typedef enum {
	UART_HW_FLOWCTRL_DISABLE = 0x0,
} uart_hw_flowcontrol_t;

typedef enum {
	UART_SCLK_DEFAULT = 4,
} uart_sclk_t;

#define UART_SIGNAL_RXD_INV	(0x1 << 2)
#define UART_PIN_NO_CHANGE	(-1)

typedef struct {
	int baud_rate;
	uart_word_length_t data_bits;
	uart_parity_t parity;
	uart_stop_bits_t stop_bits;
	uart_hw_flowcontrol_t flow_ctrl;
	uint8_t rx_flow_ctrl_thresh;
	uart_sclk_t source_clk;
} uart_config_t;

typedef void *QueueHandle_t;

esp_err_t uart_driver_install(uart_port_t uart_num, int rx_buffer_size,
			      int tx_buffer_size, int queue_size,
			      QueueHandle_t *uart_queue, int intr_alloc_flags);
esp_err_t uart_driver_delete(uart_port_t uart_num);
esp_err_t uart_param_config(uart_port_t uart_num,
			    const uart_config_t *uart_config);
esp_err_t uart_set_pin(uart_port_t uart_num, int tx_io_num, int rx_io_num,
		       int rts_io_num, int cts_io_num);
esp_err_t uart_set_line_inverse(uart_port_t uart_num, uint32_t inverse_mask);
bool uart_is_driver_installed(uart_port_t uart_num);
esp_err_t uart_get_buffered_data_len(uart_port_t uart_num, size_t *size);
esp_err_t uart_flush_input(uart_port_t uart_num);
int uart_write_bytes(uart_port_t uart_num, const void *src, size_t size);
esp_err_t uart_wait_tx_done(uart_port_t uart_num, TickType_t ticks_to_wait);
int uart_read_bytes(uart_port_t uart_num, void *buf, uint32_t length,
		    TickType_t ticks_to_wait);

#endif

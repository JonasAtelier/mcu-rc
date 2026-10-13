/*
 * Print the sticks, switches and keys of a DJI DR16 once a second.
 *
 * The DR16 is fed: the app owns the UART. Here the main task waits on the
 * UART driver's event queue; every UART_DATA event's bytes go to
 * rc_rx_feed(), and an event with timeout_flag set -- the RX FIFO timed
 * out, the line went quiet (ESP-IDF v5.5.5 driver/uart.h:71-73, :93) -- is
 * the end of a frame: rc_rx_idle(), then rc_update() at once. One task
 * does it all, so nothing here needs a lock, and no idle mark waits long
 * (RC_IDLE_MARKS are kept between updates).
 *
 * Wire the DR16's DBUS signal to the GPIO below: the UART inverts it in
 * hardware. The DR16 takes 4-8.4 V (DT7 & DR16 manual v2.0, p.2), and
 * puts out a signal level no source here gives: measure it before it
 * meets a 3.3 V pin.
 */
#include <stdio.h>

#include "dr16.h"
#include "driver/uart.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "rc.h"
#include "rc_log.h"

#define RC_UART		UART_NUM_1
#define RC_RX_GPIO	16

static const char *TAG = "dr16-example";
static QueueHandle_t uart_events;

static uint32_t now_ms(void)
{
	return (uint32_t)(esp_timer_get_time() / 1000);
}

/* Hand the bytes of one UART_DATA event to the receiver. */
static void feed(struct rc_dev *rc, size_t size)
{
	uint8_t bytes[128];

	while (size) {
		size_t n = size < sizeof(bytes) ? size : sizeof(bytes);
		int got = uart_read_bytes(RC_UART, bytes, n, 0);

		if (got <= 0)
			return;
		rc_rx_feed(rc, bytes, (size_t)got);
		size -= (size_t)got;
	}
}

static void log_state(struct rc_dev *rc, struct dr16 *receiver)
{
	struct dr16_input input;
	struct rc_link link;
	float sticks[DR16_CHANNELS];

	rc_get_link(rc, now_ms(), &link);
	rc_log_link(rc, &link);
	if (rc_get_channels(rc, sticks, DR16_CHANNELS) == 0 &&
	    dr16_get_input(receiver, &input) == 0)
		ESP_LOGI(TAG, "sticks %+.2f %+.2f %+.2f %+.2f, S1 %u S2 %u, "
			 "keys 0x%04x",
			 (double)sticks[0], (double)sticks[1],
			 (double)sticks[2], (double)sticks[3],
			 input.switch_s1, input.switch_s2, input.keys);
}

void app_main(void)
{
	/* 100000 baud, 8 data bits, even parity, 1 stop bit, inverted
	 * (RoboMaster Development Board C tutorial, p.116). */
	uart_config_t line = {
		.baud_rate = 100000,
		.data_bits = UART_DATA_8_BITS,
		.parity = UART_PARITY_EVEN,
		.stop_bits = UART_STOP_BITS_1,
		.flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
		.source_clk = UART_SCLK_DEFAULT,
	};
	struct dr16_config config = { .port = { .fed = true } };
	struct dr16 *receiver;
	struct rc_dev *rc;
	int ret;
	uint32_t last_log = 0;

	ESP_ERROR_CHECK(uart_driver_install(RC_UART, 1024, 0, 16, &uart_events,
					    0));
	ESP_ERROR_CHECK(uart_param_config(RC_UART, &line));
	ESP_ERROR_CHECK(uart_set_pin(RC_UART, UART_PIN_NO_CHANGE, RC_RX_GPIO,
				     UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
	ESP_ERROR_CHECK(uart_set_line_inverse(RC_UART, UART_SIGNAL_RXD_INV));

	ret = dr16_init(&config, &receiver);
	if (ret) {
		ESP_LOGE(TAG, "dr16_init: %d", ret);
		return;
	}
	rc = dr16_rc(receiver);

	while (1) {
		uart_event_t event;

		if (!xQueueReceive(uart_events, &event, pdMS_TO_TICKS(1000)))
			event.type = UART_EVENT_MAX;	/* a quiet second */
		if (event.type == UART_DATA) {
			feed(rc, event.size);
			if (event.timeout_flag) {
				rc_rx_idle(rc);
				rc_update(rc, now_ms());
			}
		} else if (event.type != UART_EVENT_MAX) {
			/* Overflow and the like: drop what is there; the
			 * driver counts the broken frame. */
			uart_flush_input(RC_UART);
			xQueueReset(uart_events);
		}
		if (now_ms() - last_log >= 1000) {
			last_log = now_ms();
			log_state(rc, receiver);
		}
	}
}

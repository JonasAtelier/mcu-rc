/*
 * Print the first four channels, the link state and the SBUS flags once a
 * second, from an SBUS receiver read on UART1.
 *
 * Wire the receiver's SBUS output straight to the GPIO below: the ESP32
 * UART inverts in hardware, which sbus_init() turns on. Check that the
 * receiver's signal is 3.3 V; some put out 5 V.
 */
#include <stdio.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "rc.h"
#include "rc_log.h"
#include "sbus.h"

#define RC_UART		UART_NUM_1
#define RC_RX_GPIO	16

static const char *TAG = "sbus-example";

static uint32_t now_ms(void)
{
	return (uint32_t)(esp_timer_get_time() / 1000);
}

void app_main(void)
{
	struct sbus_config config = {
		.port = { .uart = RC_UART, .rx_gpio = RC_RX_GPIO },
	};
	struct sbus *receiver;
	struct rc_dev *rc;
	int ret;
	int ticks = 0;

	ret = sbus_init(&config, &receiver);
	if (ret) {
		ESP_LOGE(TAG, "sbus_init: %d", ret);
		return;
	}
	rc = sbus_rc(receiver);

	while (1) {
		struct sbus_flags flags;
		struct rc_link link;
		float sticks[4];

		rc_update(rc, now_ms());
		if (++ticks % 100 == 0) {
			rc_get_link(rc, now_ms(), &link);
			rc_log_link(rc, &link);
			if (rc_get_channels(rc, sticks, 4) == 0)
				ESP_LOGI(TAG, "sticks %+.2f %+.2f %+.2f %+.2f",
					 (double)sticks[0], (double)sticks[1],
					 (double)sticks[2], (double)sticks[3]);
			if (sbus_get_flags(receiver, &flags) == 0)
				ESP_LOGI(TAG, "ch17 %d ch18 %d lost %d failsafe %d",
					 flags.channel_17, flags.channel_18,
					 flags.frame_lost, flags.failsafe);
		}
		vTaskDelay(pdMS_TO_TICKS(10));
	}
}

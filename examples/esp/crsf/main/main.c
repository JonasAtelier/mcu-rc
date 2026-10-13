/*
 * Print the first four channels, the link state and the link quality once
 * a second, from a CRSF receiver read on UART1.
 *
 * Wire the receiver's TX to the GPIO below -- the names cross over -- and
 * its 5 V and GND. The signal is 3.3 V and not inverted: no inverter, no
 * level shifter.
 */
#include <stdio.h>

#include "crsf.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "rc.h"
#include "rc_log.h"

#define RC_UART		UART_NUM_1
#define RC_RX_GPIO	16

static const char *TAG = "crsf-example";

static uint32_t now_ms(void)
{
	return (uint32_t)(esp_timer_get_time() / 1000);
}

void app_main(void)
{
	struct crsf_config config = {
		.port = { .uart = RC_UART, .rx_gpio = RC_RX_GPIO },
	};
	struct crsf *receiver;
	struct rc_dev *rc;
	int ret;
	int ticks = 0;

	ret = crsf_init(&config, &receiver);
	if (ret) {
		ESP_LOGE(TAG, "crsf_init: %d", ret);
		return;
	}
	rc = crsf_rc(receiver);

	while (1) {
		struct crsf_link_stats stats;
		struct rc_link link;
		float sticks[4];

		rc_update(rc, now_ms());
		rc_get_link(rc, now_ms(), &link);

		/* The failsafe is the app's: here, just say so. */
		if (link.state != RC_LINK_UP && ticks % 100 == 0)
			ESP_LOGW(TAG, "no link");

		if (++ticks % 100 == 0) {
			rc_log_link(rc, &link);
			if (rc_get_channels(rc, sticks, 4) == 0)
				ESP_LOGI(TAG, "sticks %+.2f %+.2f %+.2f %+.2f",
					 (double)sticks[0], (double)sticks[1],
					 (double)sticks[2], (double)sticks[3]);
			if (crsf_get_link_stats(receiver, &stats) == 0)
				ESP_LOGI(TAG, "LQ %u%%, RSSI -%u/-%u dBm",
					 stats.link_quality, stats.rssi1,
					 stats.rssi2);
		}
		vTaskDelay(pdMS_TO_TICKS(10));
	}
}

/*
 * Kernel dev_err()-style logging for receiver drivers and their users.
 *
 * ESP-IDF: everything goes out under the one ESP-IDF tag "rc",
 * prefixed with the receiver's name, so one esp_log_level_set("rc", ...)
 * controls every receiver and a line still says which protocol it came
 * from.
 * There is deliberately no level machinery of our own; rc_dbg()
 * is also subject to CONFIG_LOG_MAXIMUM_LEVEL, and CONFIG_RC_DEBUG
 * is not used.
 *
 * STM32 HAL: every line goes out through printf() as
 * "rc: <name>: ...". Where printf() ends up is the app's
 * business: with newlib, retarget _write() to a UART. rc_dbg()
 * alone is compiled out unless CONFIG_RC_DEBUG is 1 (stm-forge:
 * RC_DEBUG = y).
 */
#ifndef RC_LOG_H
#define RC_LOG_H

#ifdef ESP_PLATFORM
#include "esp_log.h"
#else
#include <stdio.h>
#endif

#include "rc.h"

#ifdef __cplusplus
extern "C" {
#endif

#ifdef ESP_PLATFORM
#define RC_LOG_TAG	"rc"

#define rc_err(rc, fmt, ...) \
	ESP_LOGE(RC_LOG_TAG, "%s: " fmt, (rc)->name, ##__VA_ARGS__)
#define rc_warn(rc, fmt, ...) \
	ESP_LOGW(RC_LOG_TAG, "%s: " fmt, (rc)->name, ##__VA_ARGS__)
#define rc_info(rc, fmt, ...) \
	ESP_LOGI(RC_LOG_TAG, "%s: " fmt, (rc)->name, ##__VA_ARGS__)
#define rc_dbg(rc, fmt, ...) \
	ESP_LOGD(RC_LOG_TAG, "%s: " fmt, (rc)->name, ##__VA_ARGS__)

#else /* STM32 HAL */
#define rc_err(rc, fmt, ...) \
	printf("rc: %s: " fmt "\n", (rc)->name, ##__VA_ARGS__)
#define rc_warn(rc, fmt, ...) \
	printf("rc: %s: " fmt "\n", (rc)->name, ##__VA_ARGS__)
#define rc_info(rc, fmt, ...) \
	printf("rc: %s: " fmt "\n", (rc)->name, ##__VA_ARGS__)

#if CONFIG_RC_DEBUG
#define rc_dbg(rc, fmt, ...) \
	printf("rc: %s: " fmt "\n", (rc)->name, ##__VA_ARGS__)
#else
/* Still type-checked, so a debug line cannot rot while it is off. */
#define rc_dbg(rc, fmt, ...) do { \
	if (0) \
		printf("rc: %s: " fmt "\n", (rc)->name, \
		       ##__VA_ARGS__); \
} while (0)
#endif
#endif /* ESP_PLATFORM */

/* One info line: the link's state, age and counters. */
void rc_log_link(const struct rc_dev *rc, const struct rc_link *link);

#ifdef __cplusplus
}
#endif

#endif /* RC_LOG_H */

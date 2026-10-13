/*
 * Host-side check of the core on STM32 HAL: the cases are ../core_cases.h;
 * this file supplies the log prefix and a fake UART for the read path.
 */
#include <assert.h>
#include <errno.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "stm32_hal_stub.h"

#include "rc.h"
#include "rc_log.h"
#include "rc_priv.h"

/* The HAL's lines are bare printf(), no level letter. */
#define LOG_PREFIX(level)	""

static UART_HandleTypeDef uart_handle;
static uint8_t line[512];
static size_t line_len;
static int line_fail;

static void line_push(const uint8_t *data, size_t len)
{
	assert(line_len + len <= sizeof(line));
	memcpy(line + line_len, data, len);
	line_len += len;
}

static struct rc_port_config read_port(void)
{
	return (struct rc_port_config){ .uart = &uart_handle };
}

/* The library polls a byte at a time, with timeout 0. */
HAL_StatusTypeDef HAL_UART_Receive(UART_HandleTypeDef *huart, uint8_t *data,
				   uint16_t size, uint32_t timeout)
{
	assert(huart == &uart_handle && size == 1 && timeout == 0);
	if (line_fail)
		return HAL_ERROR;
	if (!line_len)
		return HAL_TIMEOUT;
	data[0] = line[0];
	memmove(line, line + 1, --line_len);
	return HAL_OK;
}

#include "../core_cases.h"

int main(void)
{
	run_core_cases();
	printf("test_core: all passed\n");
	return 0;
}

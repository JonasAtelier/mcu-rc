/*
 * SBUS on an STM32: the UART's DMA hands every chunk to the receiver, and
 * the main loop updates it and prints the link once a second.
 *
 * A snippet, not a project: the clock tree, the printf() retarget and the
 * UART are the app's -- here huart3 as CubeMX generates it, RX only, with
 * its RX on a DMA stream in circular mode, at SBUS's 100000 baud, even
 * parity, 2 stop bits. The HAL counts the parity bit in the word length
 * (stm32f4xx_hal_uart.h:63-66, F4 HAL v1.7.12), so 8 data bits with parity
 * is UART_WORDLENGTH_9B:
 *
 *   huart3.Init.BaudRate = 100000;
 *   huart3.Init.WordLength = UART_WORDLENGTH_9B;
 *   huart3.Init.StopBits = UART_STOPBITS_2;
 *   huart3.Init.Parity = UART_PARITY_EVEN;
 *   huart3.Init.Mode = UART_MODE_RX;
 *
 * SBUS idles low. The F4's USART cannot invert its RX (its HAL has no
 * AdvancedInit), so an inverting buffer goes in front of the pin; on a
 * family whose HAL has UART_ADVFEATURE_RXINVERT_INIT, set that instead.
 * Needs HAL_UART_MODULE_ENABLED, HAL_DMA_MODULE_ENABLED and RC_SBUS = y.
 *
 * Why DMA: a blocking HAL_UART_Receive() only listens while it runs, and
 * at 100000 baud 8E2 a byte lasts 120 us.
 */
#include <stdio.h>

#include CMSIS_device_header

#include "rc.h"
#include "rc_log.h"
#include "sbus.h"

extern UART_HandleTypeDef huart3;	/* from MX_USART3_UART_Init() */

static struct sbus *receiver;
static uint8_t dma_buf[128];
static uint16_t dma_old;		/* where the last event left off */

/*
 * ReceiveToIdle in circular mode: size is the position the DMA has written
 * up to; hand over everything since the last call, wrapping at the end.
 * SBUS frames itself by header and footer, so no idle mark is needed.
 */
void HAL_UARTEx_RxEventCallback(UART_HandleTypeDef *huart, uint16_t size)
{
	struct rc_dev *rc = sbus_rc(receiver);

	if (huart != &huart3)
		return;
	if (size > dma_old) {
		rc_rx_feed(rc, dma_buf + dma_old, size - dma_old);
	} else if (size < dma_old) {
		rc_rx_feed(rc, dma_buf + dma_old, sizeof(dma_buf) - dma_old);
		rc_rx_feed(rc, dma_buf, size);
	}
	dma_old = size == sizeof(dma_buf) ? 0 : size;
}

/*
 * In DMA reception the HAL treats every receive error -- parity, framing,
 * noise, overrun -- as blocking and aborts the transfer
 * (stm32f4xx_hal_uart.c:2409-2414). Without this, one glitch on the line
 * ends reception for good, and the link reads RC_LINK_TIMEOUT forever.
 * Restart from the top of the buffer; an idle mark drops the frame in
 * flight.
 */
void HAL_UART_ErrorCallback(UART_HandleTypeDef *huart)
{
	if (huart != &huart3)
		return;
	rc_rx_idle(sbus_rc(receiver));
	dma_old = 0;
	HAL_UARTEx_ReceiveToIdle_DMA(&huart3, dma_buf, sizeof(dma_buf));
}

void rc_example_task(void)
{
	struct sbus_config config = { .port = { .fed = true } };
	struct rc_dev *rc;
	uint32_t last_log = 0;

	if (sbus_init(&config, &receiver))
		return;
	rc = sbus_rc(receiver);
	HAL_UARTEx_ReceiveToIdle_DMA(&huart3, dma_buf, sizeof(dma_buf));

	for (;;) {
		struct rc_link link;
		float sticks[4];

		rc_update(rc, HAL_GetTick());
		if (HAL_GetTick() - last_log < 1000)
			continue;
		last_log = HAL_GetTick();
		rc_get_link(rc, last_log, &link);
		rc_log_link(rc, &link);
		if (rc_get_channels(rc, sticks, 4) == 0)
			printf("sticks %+.2f %+.2f %+.2f %+.2f\n",
			       (double)sticks[0], (double)sticks[1],
			       (double)sticks[2], (double)sticks[3]);
	}
}

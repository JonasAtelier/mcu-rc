/*
 * A DJI DR16 on an STM32, the way a RoboMaster board wires it: DBUS into
 * USART3 RX through the board's inverter (Development Board C tutorial
 * p.116; the board C examples use USART3, usart.c:35).
 *
 * A snippet, not a project: the clock tree, the printf() retarget and the
 * UART are the app's -- here huart3 as CubeMX generates it, RX only, with
 * its RX on a DMA stream in NORMAL mode, at 100000 baud, even parity, one
 * stop bit. The HAL counts the parity bit in the word length
 * (stm32f4xx_hal_uart.h:63-66, F4 HAL v1.7.12), so 8 data bits with parity
 * is UART_WORDLENGTH_9B -- DJI's own example sets 8B (usart.c:37-39); see
 * docs/hardware.md:
 *
 *   huart3.Init.BaudRate = 100000;
 *   huart3.Init.WordLength = UART_WORDLENGTH_9B;
 *   huart3.Init.StopBits = UART_STOPBITS_1;
 *   huart3.Init.Parity = UART_PARITY_EVEN;
 *   huart3.Init.Mode = UART_MODE_RX;
 *
 * Needs HAL_UART_MODULE_ENABLED, HAL_DMA_MODULE_ENABLED and RC_DR16 = y.
 *
 * Framing: HAL_UARTEx_ReceiveToIdle_DMA() in normal mode ends at an idle
 * line, and calls HAL_UARTEx_RxEventCallback() with the bytes so far
 * (stm32f4xx_hal_uart.c:2488-2526). It also calls it at half and at full
 * buffer (:3066, :3101), and the F4 HAL of that version does not say which
 * event it was. So: the half-transfer interrupt is switched off, and a
 * callback with less than a full buffer is the idle line. A full buffer is
 * a burst far longer than a frame; it is fed, and the next idle throws it
 * away as one broken frame.
 */
#include <stdio.h>

#include CMSIS_device_header

#include "dr16.h"
#include "rc.h"
#include "rc_log.h"

extern UART_HandleTypeDef huart3;	/* from MX_USART3_UART_Init() */
extern DMA_HandleTypeDef hdma_usart3_rx;

static struct dr16 *receiver;
static uint8_t dma_buf[64];

static void start_receive(void)
{
	HAL_UARTEx_ReceiveToIdle_DMA(&huart3, dma_buf, sizeof(dma_buf));
	__HAL_DMA_DISABLE_IT(&hdma_usart3_rx, DMA_IT_HT);
}

void HAL_UARTEx_RxEventCallback(UART_HandleTypeDef *huart, uint16_t size)
{
	struct rc_dev *rc = dr16_rc(receiver);

	if (huart != &huart3)
		return;
	rc_rx_feed(rc, dma_buf, size);
	if (size < sizeof(dma_buf))
		rc_rx_idle(rc);
	start_receive();
}

/*
 * In DMA reception the HAL treats every receive error -- parity, framing,
 * noise, overrun -- as blocking and aborts the transfer
 * (stm32f4xx_hal_uart.c:2409-2414). Without this, one glitch on the line
 * ends reception for good, and the link reads RC_LINK_TIMEOUT forever. The
 * bytes of the frame in flight are broken: an idle mark drops them.
 */
void HAL_UART_ErrorCallback(UART_HandleTypeDef *huart)
{
	if (huart != &huart3)
		return;
	rc_rx_idle(dr16_rc(receiver));
	start_receive();
}

void rc_example_task(void)
{
	struct dr16_config config = { .port = { .fed = true } };
	struct rc_dev *rc;
	uint32_t last_log = 0;

	if (dr16_init(&config, &receiver))
		return;
	rc = dr16_rc(receiver);
	start_receive();

	/* No delay: the ring keeps RC_IDLE_MARKS idle marks between two
	 * updates; a loop that falls further behind loses frames. */
	for (;;) {
		struct dr16_input input;
		struct rc_link link;
		float sticks[DR16_CHANNELS];

		rc_update(rc, HAL_GetTick());
		if (HAL_GetTick() - last_log < 1000)
			continue;
		last_log = HAL_GetTick();
		rc_get_link(rc, last_log, &link);
		rc_log_link(rc, &link);
		if (rc_get_channels(rc, sticks, DR16_CHANNELS) == 0 &&
		    dr16_get_input(receiver, &input) == 0)
			printf("sticks %+.2f %+.2f %+.2f %+.2f, S1 %u S2 %u, "
			       "keys 0x%04x\n",
			       (double)sticks[0], (double)sticks[1],
			       (double)sticks[2], (double)sticks[3],
			       input.switch_s1, input.switch_s2, input.keys);
	}
}

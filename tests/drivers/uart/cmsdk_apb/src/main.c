/* SPDX-License-Identifier: Apache-2.0 */
#include <zephyr/drivers/uart.h>
#include <zephyr/kernel.h>
#include <zephyr/ztest.h>

static const struct device *const uart = DEVICE_DT_GET(DT_ALIAS(test_uart));
static K_SEM_DEFINE(called, 0, 1);
static int before, after;

static void tx_ready(const struct device *dev, void *context)
{
	if (!uart_irq_update(dev) || !uart_irq_tx_ready(dev)) {
		return;
	}
	before = uart_irq_tx_complete(dev);
	uart_irq_tx_disable(dev);
	after = uart_irq_tx_complete(dev);
	k_sem_give(&called);
}

ZTEST(cmsdk_apb, test_idle_completion_with_masked_tx_interrupt)
{
	zassert_true(device_is_ready(uart));
	zassert_ok(uart_irq_callback_user_data_set(uart, tx_ready, NULL));
	/* No warm-up transmission: a cold, idle transmitter is already complete. */
	for (int i = 0; i < 2; i++) {
		uart_irq_tx_enable(uart);
		zassert_ok(k_sem_take(&called, K_SECONDS(1)));
		zassert_equal(before, 1);
		zassert_equal(after, 1, "Disabling TX IRQ changed completion of an idle UART");
	}
}

ZTEST_SUITE(cmsdk_apb, NULL, NULL, NULL, NULL, NULL);

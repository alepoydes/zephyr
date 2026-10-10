// SPDX-License-Identifier: Apache-2.0
#include <zephyr/bluetooth/buf.h>
#include <zephyr/bluetooth/hci_types.h>
#include <zephyr/drivers/bluetooth.h>
#include <zephyr/drivers/serial/uart_emul.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/ztest.h>

static const struct device *const hci = DEVICE_DT_GET(DT_CHOSEN(zephyr_bt_hci));
static const struct device *const uart = DEVICE_DT_GET(DT_NODELABEL(test_uart));
static K_SEM_DEFINE(received, 0, 1);
static K_SEM_DEFINE(transmitting, 0, 1);
NET_BUF_POOL_FIXED_DEFINE(tx_pool, 2, 16, 0, NULL);
static int setup_error, teardown_error;

/* Exercise the production H:4 driver, replacing only board power hooks. */
int bt_hci_transport_setup(const struct device *dev)
{
	return setup_error;
}

int bt_hci_transport_teardown(const struct device *dev)
{
	return teardown_error;
}

static int receive(const struct device *dev, struct net_buf *buf)
{
	static const uint8_t expected[] = {BT_HCI_H4_EVT, 0xff, 1, 0xbb};

	zassert_equal(buf->len, sizeof(expected));
	zassert_mem_equal(buf->data, expected, sizeof(expected));
	net_buf_unref(buf);
	k_sem_give(&received);
	return 0;
}

static void tx_ready(const struct device *dev, size_t size, void *user_data)
{
	k_sem_give(&transmitting);
}

static void *setup(void)
{
	uart_emul_callback_tx_data_ready_set(uart, tx_ready, NULL);
	return NULL;
}

ZTEST(h4, test_setup_error)
{
	setup_error = -ETIMEDOUT;
	zassert_equal(bt_hci_open(hci, receive), -ETIMEDOUT);
	setup_error = 0;
}

ZTEST(h4, test_close_and_reopen)
{
	uint8_t partial[] = {BT_HCI_H4_EVT, 0xff, 3, 0xaa};
	static const uint8_t event[] = {BT_HCI_H4_EVT, 0xff, 1, 0xbb};

	/* Repeat beyond pool capacity to catch retained TX buffers. */
	for (int cycle = 0; cycle < 4; cycle++) {
		zassert_ok(bt_hci_open(hci, receive));
		partial[2] = cycle < 2 ? 3 : 255;
		zassert_equal(uart_emul_put_rx_data(uart, partial,
			 sizeof(partial)), sizeof(partial));
		/* Let the emulated interrupt consume the partial event. */
		k_msleep(1);
		zassert_equal(uart_emul_flush_rx_data(uart), 0);
		for (int n = 0; n < 2; n++) {
			struct net_buf *buf =
				net_buf_alloc(&tx_pool, K_NO_WAIT);

			zassert_not_null(buf, "TX buffer retained on close");
			memset(net_buf_add(buf, 16), 0xa5, 16);
			zassert_ok(bt_hci_send(hci, buf));
		}
		zassert_ok(k_sem_take(&transmitting, K_MSEC(100)));
		teardown_error = cycle % 2 ? -EIO : 0;
		zassert_equal(bt_hci_close(hci), teardown_error);
		/* Model board reset clearing bytes already in the UART. */
		uart_emul_flush_tx_data(uart);
		teardown_error = 0;
		zassert_ok(bt_hci_open(hci, receive));
		zassert_equal(uart_emul_put_rx_data(uart, event, sizeof(event)),
			      sizeof(event));
		zassert_ok(k_sem_take(&received, K_MSEC(100)));
		zassert_ok(bt_hci_close(hci));
		zassert_equal(uart_emul_flush_tx_data(uart), 0,
			      "Stale transmission resumed after reopen");
	}
}

ZTEST_SUITE(h4, NULL, setup, NULL, NULL, NULL);

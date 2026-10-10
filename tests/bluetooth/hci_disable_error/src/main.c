// SPDX-License-Identifier: Apache-2.0
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/buf.h>
#include <zephyr/bluetooth/hci.h>
#include <zephyr/drivers/bluetooth.h>
#include <zephyr/ztest.h>

#define DT_DRV_COMPAT zephyr_bt_hci_test
static bt_hci_recv_t receive;
static unsigned int commands, closes;
static int close_error;

static int open_driver(const struct device *dev, bt_hci_recv_t recv)
{
	receive = recv;
	return 0;
}

static int setup_driver(const struct device *dev,
			const struct bt_hci_setup_params *params)
{
	/* Fail initialization after opening, without modeling a controller. */
	return -ECANCELED;
}

static int send_driver(const struct device *dev, struct net_buf *buf)
{
	struct net_buf *event;
	uint16_t opcode;

	zassert_equal(net_buf_pull_u8(buf), BT_HCI_H4_CMD);
	opcode = net_buf_pull_le16(buf);
	zassert_equal(opcode, BT_HCI_OP_RESET);
	commands++;
	event = bt_buf_get_evt(BT_HCI_EVT_CMD_COMPLETE, false, K_NO_WAIT);
	zassert_not_null(event);
	net_buf_add_u8(event, BT_HCI_EVT_CMD_COMPLETE);
	net_buf_add_u8(event, 4);
	net_buf_add_u8(event, 1);
	net_buf_add_le16(event, opcode);
	net_buf_add_u8(event, 0);
	zassert_ok(receive(dev, event));
	net_buf_unref(buf);
	return 0;
}

static int close_driver(const struct device *dev)
{
	closes++;
	return close_error;
}

static DEVICE_API(bt_hci, api) = {
	.open = open_driver,
	.setup = setup_driver,
	.send = send_driver,
	.close = IS_ENABLED(CONFIG_TEST_HCI_NO_CLOSE) ? NULL : close_driver,
};
DEVICE_DT_INST_DEFINE(0, NULL, NULL, NULL, NULL, POST_KERNEL,
	CONFIG_KERNEL_INIT_PRIORITY_DEVICE, &api);

ZTEST(disable, test_close_errors)
{
	zassert_equal(bt_enable(NULL), -ECANCELED);
	zassert_false(bt_is_ready());
	if (IS_ENABLED(CONFIG_TEST_HCI_NO_CLOSE)) {
		for (int i = 0; i < 2; i++) {
			zassert_equal(bt_disable(), -ENOTSUP);
			zassert_equal(commands, 0, "Unexpected HCI Reset");
			zassert_false(bt_is_ready());
		}
		zassert_equal(closes, 0);
	} else {
		close_error = -EIO;
		zassert_equal(bt_disable(), -EIO);
		zassert_equal(closes, 1);
		zassert_false(bt_is_ready(), "Failed close claimed ready");
		zassert_equal(bt_enable(NULL), -EALREADY);
		zassert_equal(bt_disable(), -EALREADY);
		zassert_equal(closes, 1);
		zassert_equal(commands, 1);
		zassert_false(bt_is_ready());
	}
}
ZTEST_SUITE(disable, NULL, NULL, NULL, NULL, NULL);

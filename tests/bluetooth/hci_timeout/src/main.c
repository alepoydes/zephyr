// SPDX-License-Identifier: Apache-2.0
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/buf.h>
#include <zephyr/bluetooth/hci.h>
#include <zephyr/drivers/bluetooth.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/ztest.h>

#include "hci_core.h"

#define DT_DRV_COMPAT zephyr_bt_hci_test
#define OPCODE BT_OP(BT_OGF_VS, 1)

static bt_hci_recv_t receive;
static const struct device *hci;
static bool silent;
static uint8_t reply_status;
static int send_error;
static unsigned int sent;

static void complete(uint16_t opcode)
{
	struct net_buf *event = bt_buf_get_evt(BT_HCI_EVT_CMD_COMPLETE,
					    false, K_NO_WAIT);

	zassert_not_null(event);
	net_buf_add_u8(event, BT_HCI_EVT_CMD_COMPLETE);
	net_buf_add_u8(event, 4);
	net_buf_add_u8(event, 1);
	net_buf_add_le16(event, opcode);
	net_buf_add_u8(event, reply_status);
	zassert_ok(receive(hci, event));
}

static int open_driver(const struct device *dev, bt_hci_recv_t recv)
{
	hci = dev;
	receive = recv;
	return 0;
}

static int setup_driver(const struct device *dev,
			const struct bt_hci_setup_params *params)
{
	/* Open the command path without modeling controller capabilities. */
	return -ECANCELED;
}

static int send_driver(const struct device *dev, struct net_buf *buf)
{
	zassert_equal(net_buf_pull_u8(buf), BT_HCI_H4_CMD);
	uint16_t opcode = net_buf_pull_le16(buf);

	sent++;
	if (send_error) {
		return send_error;
	}
	if (!silent) {
		complete(opcode);
	}
	net_buf_unref(buf);
	return 0;
}

static DEVICE_API(bt_hci, api) = {
	.open = open_driver,
	.setup = setup_driver,
	.send = send_driver,
};
DEVICE_DT_INST_DEFINE(0, NULL, NULL, NULL, NULL, POST_KERNEL,
	CONFIG_KERNEL_INIT_PRIORITY_DEVICE, &api);

static void *setup(void)
{
	zassert_equal(bt_enable(NULL), -ECANCELED);
	return NULL;
}

static void before(void *fixture)
{
	silent = false;
	reply_status = 0;
	send_error = 0;
	sent = 0;
}

ZTEST(hci_timeout, test_completion_and_error)
{
	struct net_buf *buf = bt_hci_cmd_alloc(K_NO_WAIT);
	struct net_buf *rsp = NULL;
	struct bt_hci_cmd_state_set state;
	atomic_t target = 0;

	zassert_not_null(buf);
	bt_hci_cmd_state_set_init(buf, &state, &target, 0, true);
	zassert_ok(bt_hci_cmd_send_sync(OPCODE, buf, &rsp));
	zassert_not_null(rsp);
	zassert_equal(rsp->len, 1);
	zassert_equal(rsp->data[0], 0);
	zassert_equal(atomic_get(&target), 1);
	net_buf_unref(rsp);
	reply_status = BT_HCI_ERR_CMD_DISALLOWED;
	zassert_equal(bt_hci_cmd_send_sync(OPCODE, NULL, NULL), -EACCES);
	send_error = -EIO;
	zassert_equal(bt_hci_cmd_send_sync(OPCODE, NULL, NULL), -EIO);
}

static void expire_command(void)
{
	struct net_buf *buf = bt_hci_cmd_alloc(K_NO_WAIT);
	struct bt_hci_cmd_state_set state;
	atomic_t target = 0;

	zassert_not_null(buf);
	bt_hci_cmd_state_set_init(buf, &state, &target, 0, true);
	zassert_equal(bt_hci_cmd_send_sync(OPCODE, buf, NULL), -ETIMEDOUT);
	zassert_equal(atomic_get(&target), 0);
	/* Late completion must not update even a still-live old stack. */
	complete(OPCODE);
	zassert_equal(atomic_get(&target), 0);
}

ZTEST(hci_timeout, test_late_completion)
{
	silent = true;
	expire_command();
	silent = false;
	zassert_ok(bt_hci_cmd_send_sync(OPCODE, NULL, NULL));
	zassert_equal(sent, 2);
}

static int work_result;
static K_SEM_DEFINE(work_done, 0, 1);

static void send_from_work(struct k_work *work)
{
	work_result = bt_hci_cmd_send_sync(OPCODE + 1, NULL, NULL);
	k_sem_give(&work_done);
}
static K_WORK_DEFINE(command_work, send_from_work);

ZTEST(hci_timeout, test_workqueue_completion)
{
	k_work_submit(&command_work);
	zassert_ok(k_sem_take(&work_done, K_SECONDS(1)));
	zassert_ok(work_result);
	zassert_equal(sent, 1);
}

ZTEST(hci_timeout, test_queued_expiry)
{
	silent = true;
	zassert_equal(bt_hci_cmd_send_sync(OPCODE, NULL, NULL), -ETIMEDOUT);
	/* Controller still owns the credit. Repeated queued expiry must neither
	 * leak buffers nor transmit a canceled command when the credit returns.
	 */
	for (int i = 0; i < 3; i++) {
		k_work_submit(&command_work);
		zassert_ok(k_sem_take(&work_done, K_SECONDS(11)));
		zassert_equal(work_result, -ETIMEDOUT);
		zassert_equal(sent, 1);
	}
	complete(OPCODE);
	silent = false;
	zassert_ok(bt_hci_cmd_send_sync(OPCODE + 2, NULL, NULL));
	zassert_equal(sent, 2);
}

ZTEST(hci_timeout, test_allocation_exhaustion)
{
	struct net_buf *held[2];

	for (size_t i = 0; i < ARRAY_SIZE(held); i++) {
		held[i] = bt_hci_cmd_alloc(K_NO_WAIT);
		zassert_not_null(held[i]);
	}
	zassert_equal(bt_hci_cmd_send_sync(OPCODE, NULL, NULL), -ENOBUFS);
	zassert_equal(sent, 0);
	for (size_t i = 0; i < ARRAY_SIZE(held); i++) {
		net_buf_unref(held[i]);
	}
	zassert_ok(bt_hci_cmd_send_sync(OPCODE, NULL, NULL));
}

ZTEST_SUITE(hci_timeout, NULL, setup, before, NULL, NULL);

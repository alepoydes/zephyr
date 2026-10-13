/*
 * SPDX-License-Identifier: Apache-2.0
 */

#include <string.h>

#include <zephyr/bluetooth/addr.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/l2cap.h>
#include <zephyr/net_buf.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/ztest.h>

#include "host/conn_internal.h"
#include "host/classic/l2cap_br_internal.h"

#define TEST_PSM 0x1001
#define PEER_CID 0x0040
#define REQUEST_IDENT 7

NET_BUF_POOL_FIXED_DEFINE(rx_pool, 1, 32, 0, NULL);

static struct bt_l2cap_br_chan accepted_chan;
static unsigned int security_requests;
static unsigned int released_count;
static unsigned int data_count;
static uint8_t local_conf_ident;

int __wrap_bt_conn_set_security(struct bt_conn *conn, bt_security_t level)
{
	zassert_not_null(conn);
	zassert_equal(level, BT_SECURITY_L2);
	security_requests++;
	return 0;
}

void __wrap_bt_conn_data_ready(struct bt_conn *conn)
{
	/* The test pulls real L2CAP PDUs; no HCI controller is present. */
	zassert_not_null(conn);
}

static int recv_data(struct bt_l2cap_chan *chan, struct net_buf *buf)
{
	ARG_UNUSED(chan);
	ARG_UNUSED(buf);
	data_count++;
	return 0;
}

static void released(struct bt_l2cap_chan *chan)
{
	released_count++;
	/* The owner may reclaim all channel storage from this callback. */
	memset(CONTAINER_OF(chan, struct bt_l2cap_br_chan, chan), 0xa5,
	       sizeof(struct bt_l2cap_br_chan));
}

static const struct bt_l2cap_chan_ops chan_ops = {
	.recv = recv_data,
	.released = released,
};

static int accept(struct bt_conn *conn, struct bt_l2cap_server *server,
		  struct bt_l2cap_chan **chan)
{
	ARG_UNUSED(conn);
	ARG_UNUSED(server);
	memset(&accepted_chan, 0, sizeof(accepted_chan));
	accepted_chan.chan.ops = &chan_ops;
	accepted_chan.rx.mtu = 64;
	*chan = &accepted_chan.chan;
	return 0;
}

static struct bt_l2cap_server server = {
	.psm = TEST_PSM,
	.sec_level = BT_SECURITY_L1,
	.accept = accept,
};

static struct bt_conn *new_connection(void)
{
	static const bt_addr_t address = { .val = { 1, 2, 3, 4, 5, 6 } };
	struct bt_conn *conn = bt_conn_add_br(&address);
	struct net_buf *buf;
	size_t length;

	zassert_not_null(conn);
	conn->state = BT_CONN_CONNECTED;
	conn->sec_level = BT_SECURITY_L1;
	conn->br.features[1][0] = BIT(0); /* SSP capable peer. */
	sys_slist_init(&conn->channels);
	sys_slist_init(&conn->l2cap_data_ready);
	bt_l2cap_br_connected(conn);
	/* The fixed signaling channel first sends its feature query. */
	while ((buf = l2cap_br_data_pull(conn, 256, &length)) != NULL) {
		net_buf_unref(buf);
	}
	return conn;
}

static void request_channel(struct bt_conn *conn)
{
	struct net_buf *buf = net_buf_alloc(&rx_pool, K_NO_WAIT);

	zassert_not_null(buf);
	net_buf_add_le16(buf, sizeof(struct bt_l2cap_sig_hdr) + sizeof(struct bt_l2cap_conn_req));
	net_buf_add_le16(buf, BT_L2CAP_CID_BR_SIG);
	net_buf_add_u8(buf, BT_L2CAP_CONN_REQ);
	net_buf_add_u8(buf, REQUEST_IDENT);
	net_buf_add_le16(buf, sizeof(struct bt_l2cap_conn_req));
	net_buf_add_le16(buf, TEST_PSM);
	net_buf_add_le16(buf, PEER_CID);
	bt_l2cap_br_recv(conn, buf); /* Consumes buf. */
}

static void send_data(struct bt_conn *conn, uint16_t cid)
{
	struct net_buf *buf = net_buf_alloc(&rx_pool, K_NO_WAIT);

	zassert_not_null(buf);
	net_buf_add_le16(buf, 1);
	net_buf_add_le16(buf, cid);
	net_buf_add_u8(buf, 0x5a);
	bt_l2cap_br_recv(conn, buf);
}

static void send_configuration(struct bt_conn *conn, uint8_t code, uint8_t ident, uint16_t cid)
{
	struct net_buf *buf = net_buf_alloc(&rx_pool, K_NO_WAIT);
	uint16_t payload_len = code == BT_L2CAP_CONF_REQ ? 4 : 6;

	zassert_not_null(buf);
	net_buf_add_le16(buf, sizeof(struct bt_l2cap_sig_hdr) + payload_len);
	net_buf_add_le16(buf, BT_L2CAP_CID_BR_SIG);
	net_buf_add_u8(buf, code);
	net_buf_add_u8(buf, ident);
	net_buf_add_le16(buf, payload_len);
	net_buf_add_le16(buf, cid);
	net_buf_add_le16(buf, 0); /* flags */
	if (code == BT_L2CAP_CONF_RSP) {
		net_buf_add_le16(buf, BT_L2CAP_CONF_SUCCESS);
	}
	bt_l2cap_br_recv(conn, buf);
}

static void expect_response(struct bt_conn *conn, uint16_t expected)
{
	struct net_buf *buf;
	size_t length;
	bool found = false;

	while ((buf = l2cap_br_data_pull(conn, 256, &length)) != NULL) {
		const uint8_t *data = buf->data;
		uint8_t code = data[sizeof(struct bt_l2cap_hdr)];

		zassert_equal(length, buf->len);
		if (code == BT_L2CAP_CONF_REQ) {
			local_conf_ident = data[sizeof(struct bt_l2cap_hdr) + 1];
		}
		if (buf->len >= sizeof(struct bt_l2cap_hdr) + sizeof(struct bt_l2cap_sig_hdr) +
			       sizeof(struct bt_l2cap_conn_rsp) &&
		    sys_get_le16(data + 2) == BT_L2CAP_CID_BR_SIG &&
		    code == BT_L2CAP_CONN_RSP) {
			const size_t result_offset = sizeof(struct bt_l2cap_hdr) +
						     sizeof(struct bt_l2cap_sig_hdr) + 4;

			zassert_equal(data[sizeof(struct bt_l2cap_hdr) + 1], REQUEST_IDENT);
			zassert_equal(sys_get_le16(data + result_offset), expected);
			found = true;
		}
		net_buf_unref(buf);
	}

	zassert_true(found, "no L2CAP connection response");
}

static void close_connection(struct bt_conn *conn)
{
	bt_l2cap_br_disconnected(conn);
	bt_conn_unref(conn);
}

ZTEST(l2cap_br_security, test_pending_security_completion_and_failure)
{
	struct bt_conn *conn;

	zassert_ok(bt_l2cap_br_server_register(&server));

	conn = new_connection();
	request_channel(conn);
	zassert_equal(security_requests, 1);
	zassert_equal(accepted_chan.state, BT_L2CAP_CONNECTING);
	expect_response(conn, BT_L2CAP_BR_PENDING);
	send_data(conn, accepted_chan.rx.cid);
	zassert_equal(data_count, 0);
	conn->encrypt = true;
	conn->sec_level = BT_SECURITY_L2;
	l2cap_br_encrypt_change(conn, 0);
	expect_response(conn, BT_L2CAP_BR_SUCCESS);
	zassert_equal(accepted_chan.state, BT_L2CAP_CONFIG);
	zassert_not_equal(local_conf_ident, 0);
	send_configuration(conn, BT_L2CAP_CONF_REQ, REQUEST_IDENT + 1, accepted_chan.rx.cid);
	send_configuration(conn, BT_L2CAP_CONF_RSP, local_conf_ident, accepted_chan.rx.cid);
	zassert_equal(accepted_chan.state, BT_L2CAP_CONNECTED);
	send_data(conn, accepted_chan.rx.cid);
	zassert_equal(data_count, 1);
	close_connection(conn);
	zassert_equal(released_count, 1);
	data_count = 0;

	conn = new_connection();
	request_channel(conn);
	expect_response(conn, BT_L2CAP_BR_PENDING);
	uint16_t rejected_cid = accepted_chan.rx.cid;

	l2cap_br_encrypt_change(conn, BT_HCI_ERR_AUTH_FAIL);
	expect_response(conn, BT_L2CAP_BR_ERR_SEC_BLOCK);
	zassert_equal(released_count, 2);
	zassert_is_null(bt_l2cap_br_lookup_tx_cid(conn, PEER_CID));
	send_data(conn, rejected_cid);
	zassert_equal(data_count, 0);
	close_connection(conn);

	conn = new_connection();
	request_channel(conn);
	expect_response(conn, BT_L2CAP_BR_PENDING);
	conn->encrypt = true;
	/* Encryption alone does not meet the channel's requested level. */
	l2cap_br_encrypt_change(conn, 0);
	expect_response(conn, BT_L2CAP_BR_ERR_SEC_BLOCK);
	zassert_equal(released_count, 3);
	zassert_is_null(bt_l2cap_br_lookup_tx_cid(conn, PEER_CID));
	zassert_equal(data_count, 0);
	close_connection(conn);

	zassert_equal(security_requests, 3);
	zassert_ok(bt_l2cap_br_server_unregister(&server));
}

ZTEST_SUITE(l2cap_br_security, NULL, NULL, NULL, NULL, NULL);

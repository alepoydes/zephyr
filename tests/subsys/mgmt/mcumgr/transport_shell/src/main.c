// SPDX-License-Identifier: Apache-2.0
#include <errno.h>
#include <zephyr/mgmt/mcumgr/smp/smp.h>
#include <zephyr/mgmt/mcumgr/transport/smp_shell.h>
#include <zephyr/ztest.h>

NET_BUF_POOL_DEFINE(lines, 1, SMP_SHELL_RX_BUF_SIZE, 0, NULL);
static struct smp_shell_data input = { .buf_pool = &lines };
static struct mcumgr_serial_rx_ctxt assembly;
/* smpclient 7.3.0 EchoWrite(d="brickphyr") + smp.packet.encode(). */
static const char echo[] = "\x06\tABcKAAANAAAAAKFhZGlicmlja3BoeXJYww==\n";

static void before(void *fixture)
{
	k_fifo_init(&input.buf_ready);
}

static void after(void *fixture)
{
	smp_shell_rx_reset(&input, &assembly);
}

static void feed(const void *bytes, size_t length)
{
	zassert_equal(smp_shell_rx_bytes(&input, bytes, length), length);
}

ZTEST(smp_shell, test_rejected_header_and_oversized_line)
{
	uint8_t byte = MCUMGR_SERIAL_HDR_PKT_1;

	feed(&byte, 1);
	byte = 'x';
	zassert_equal(smp_shell_rx_bytes(&input, &byte, 1), 0);
	byte = MCUMGR_SERIAL_HDR_PKT_2;
	zassert_equal(smp_shell_rx_bytes(&input, &byte, 1), 0,
		      "Rejected prefix stayed armed and consumed unrelated input");
	/* A valid prefix cannot make an overlong line acceptable. */
	feed(echo, sizeof(echo) - 2);
	byte = ' ';
	for (int i = 0; i < SMP_SHELL_RX_BUF_SIZE; i++) {
		feed(&byte, 1);
	}
	byte = '\n';
	feed(&byte, 1);
	zassert_is_null(smp_shell_rx_packet(&input, &assembly));
	feed(echo, sizeof(echo) - 1);
	struct net_buf *packet = smp_shell_rx_packet(&input, &assembly);

	zassert_not_null(packet);
	zassert_equal(packet->data[0], 0x0a);
	zassert_mem_equal(packet->data + packet->len - 9, "brickphyr", 9);
	smp_packet_free(packet);
}

ZTEST(smp_shell, test_partial_and_queued_line_reset)
{
	for (int i = 0; i < CONFIG_MCUMGR_TRANSPORT_NETBUF_COUNT + 1; i++) {
		feed(echo, 10);
		smp_shell_rx_reset(&input, &assembly);
		feed(echo, sizeof(echo) - 1);
		smp_shell_rx_reset(&input, &assembly);
		zassert_is_null(smp_shell_rx_packet(&input, &assembly));
	}
	feed(echo, sizeof(echo) - 1);
	struct net_buf *packet = smp_shell_rx_packet(&input, &assembly);

	zassert_not_null(packet, "Reset leaked receive buffers");
	smp_packet_free(packet);
}

static uint8_t encoded[512];
static size_t encoded_length;

static int collect(const void *bytes, int length)
{
	zassert_true(encoded_length + length <= sizeof(encoded));
	memcpy(encoded + encoded_length, bytes, length);
	encoded_length += length;
	return 0;
}

static int reject_after_prefix(const void *bytes, int length)
{
	if (encoded_length == 6) {
		return -EIO;
	}

	return collect(bytes, length);
}

ZTEST(smp_shell, test_multiframe_serial_encoding)
{
	/* Payload bytes 0..92; CRC-16/ITU-T is 0x8206. */
	static const char expected[] =
		"\x06\x09" "AF8AAQIDBAUGBwgJCgsMDQ4PEBESExQVFhcYGRobHB0eHyAhIiMkJSYnKCkqKywtLi8w"
		"MTIzNDU2Nzg5Ojs8PT4/QEFCQ0RFRkdISUpLTE1OT1BRUlNUVVZXWFla\n"
		"\x04\x14" "W1yCBg==\n";
	uint8_t payload[93];

	for (size_t i = 0; i < sizeof(payload); i++) {
		payload[i] = i;
	}

	encoded_length = 0;
	zassert_ok(mcumgr_serial_tx_pkt(payload, sizeof(payload), collect));
	zassert_equal(encoded_length, sizeof(expected) - 1);
	zassert_mem_equal(encoded, expected, sizeof(expected) - 1);

	encoded_length = 0;
	zassert_equal(mcumgr_serial_tx_pkt(payload, sizeof(payload), reject_after_prefix), -EIO);
	zassert_equal(encoded_length, 6, "Transmission continued after callback error");
	zassert_mem_equal(encoded, expected, encoded_length);
}

ZTEST(smp_shell, test_partial_packet_reset)
{
	const uint8_t payload[220] = { 0xa5 };

	encoded_length = 0;
	zassert_ok(mcumgr_serial_tx_pkt(payload, sizeof(payload), collect));
	uint8_t *newline = memchr(encoded, '\n', encoded_length);

	zassert_not_null(newline);
	size_t first_length = newline - encoded + 1;

	feed(encoded, first_length);
	zassert_is_null(smp_shell_rx_packet(&input, &assembly));
	zassert_not_null(assembly.nb, "No partial packet owned");
	smp_shell_rx_reset(&input, &assembly);
	zassert_is_null(assembly.nb);
	for (size_t i = first_length; i < encoded_length; i++) {
		feed(encoded + i, 1);
		if (encoded[i] == '\n') {
			zassert_is_null(smp_shell_rx_packet(&input, &assembly),
				       "Continuation completed a request from a closed session");
		}
	}
}

ZTEST_SUITE(smp_shell, NULL, NULL, before, after, NULL);

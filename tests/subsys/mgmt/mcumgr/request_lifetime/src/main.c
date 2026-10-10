// SPDX-License-Identifier: Apache-2.0
#include <zephyr/ztest.h>
#include <zephyr/net_buf.h>
#include <zephyr/mgmt/mcumgr/mgmt/mgmt.h>
#include <zephyr/mgmt/mcumgr/smp/smp.h>
#include <zephyr/mgmt/mcumgr/transport/smp.h>

static unsigned int began, ended, handled, sent, cleaned;
static int admission, output_error, final_result;
static bool active;
static unsigned int token;

static int begin(const struct net_buf *request, void **context)
{
	zassert_not_null(request);
	zassert_is_null(*context);
	zassert_false(active);
	began++;
	if (admission) {
		return admission;
	}
	active = true;
	token = cleaned;
	*context = &token;
	return 0;
}

static void end(void *context, int result)
{
	zassert_equal_ptr(context, &token);
	zassert_true(active);
	zassert_true(cleaned > token, "Completion preceded buffer cleanup");
	active = false;
	ended++;
	final_result = result;
}

static void cleanup(void *data)
{
	cleaned++;
}

static int output(struct net_buf *reply)
{
	zassert_true(active, "Response outlived admission");
	sent++;
	cleanup(net_buf_user_data(reply));
	smp_packet_free(reply);
	return output_error;
}

static int read_value(struct smp_streamer *streamer)
{
	zassert_true(active, "Handler ran without admission");
	handled++;
	return 0;
}

static const struct mgmt_handler handlers[] = { { .mh_read = read_value } };
static struct mgmt_group group = {
	.mg_handlers = handlers, .mg_handlers_count = ARRAY_SIZE(handlers),
	.mg_group_id = 64,
};
static struct smp_transport transport = {
	.functions = { .output = output, .ud_free = cleanup,
		.request_begin = begin, .request_end = end },
};

/* SMP v2 READ, empty CBOR map, application group 64, sequence 5, command 0. */
static const uint8_t read_packet[] = { 0x08, 0, 0, 1, 0, 64, 5, 0, 0xa0 };

static int process(const uint8_t *data, size_t size)
{
	struct cbor_nb_reader reader;
	struct cbor_nb_writer writer;
	struct smp_streamer streamer = {
		.smpt = &transport, .reader = &reader, .writer = &writer,
	};
	struct net_buf *request = smp_packet_alloc();

	zassert_not_null(request);
	if (size) {
		net_buf_add_mem(request, data, size);
	}
	return smp_process_request_packet(&streamer, request);
}

static void before(void *fixture)
{
	began = ended = handled = sent = cleaned = 0;
	admission = output_error = final_result = 0;
	active = false;
}

static void *setup(void)
{
	mgmt_register_group(&group);
	return NULL;
}

ZTEST(lifetime, test_rejected_packet_has_no_effect)
{
	admission = MGMT_ERR_EACCESSDENIED;
	zassert_equal(process(read_packet, sizeof(read_packet)), admission);
	zassert_equal(began, 1);
	zassert_equal(ended, 0);
	zassert_equal(handled, 0);
	zassert_equal(sent, 0);
	zassert_equal(cleaned, 1);
}

ZTEST(lifetime, test_success)
{
	zassert_ok(process(read_packet, sizeof(read_packet)));
	zassert_equal(began, 1);
	zassert_equal(ended, 1);
	zassert_equal(handled, 1);
	zassert_equal(sent, 1);
	zassert_ok(final_result);
	zassert_false(active);
}

ZTEST(lifetime, test_empty_and_malformed)
{
	zassert_ok(process(NULL, 0));
	zassert_equal(ended, 1);
	zassert_equal(process(read_packet, 3), MGMT_ERR_ECORRUPT);
	zassert_equal(ended, 2);
	zassert_equal(final_result, MGMT_ERR_ECORRUPT);
	zassert_equal(handled, 0);
	zassert_equal(sent, 0);
}

ZTEST(lifetime, test_handler_and_output_errors)
{
	uint8_t unknown[sizeof(read_packet)];

	memcpy(unknown, read_packet, sizeof(unknown));
	unknown[5] = 65; /* No registered handler. */
	zassert_equal(process(unknown, sizeof(unknown)), MGMT_ERR_ENOTSUP);
	zassert_equal(ended, 1);
	zassert_equal(handled, 0);
	zassert_equal(sent, 1);
	output_error = MGMT_ERR_EUNKNOWN;
	zassert_equal(process(read_packet, sizeof(read_packet)), output_error);
	zassert_equal(ended, 2);
	zassert_equal(handled, 1);
	zassert_equal(final_result, output_error);
}

ZTEST(lifetime, test_allocation_failure)
{
	struct net_buf *held[CONFIG_MCUMGR_TRANSPORT_NETBUF_COUNT - 1];

	for (size_t i = 0; i < ARRAY_SIZE(held); i++) {
		held[i] = smp_packet_alloc();
		zassert_not_null(held[i]);
	}
	int result = process(read_packet, sizeof(read_packet));

	for (size_t i = 0; i < ARRAY_SIZE(held); i++) {
		smp_packet_free(held[i]);
	}
	zassert_equal(result, MGMT_ERR_ENOMEM);
	zassert_equal(ended, 1);
	zassert_equal(final_result, MGMT_ERR_ENOMEM);
	zassert_equal(handled, 0);
}

ZTEST(lifetime, test_incomplete_callback_pair)
{
	transport.functions.request_end = NULL;
	int result = process(read_packet, sizeof(read_packet));
	int init_result = smp_transport_init(&transport);

	transport.functions.request_end = end;
	zassert_equal(result, MGMT_ERR_EINVAL);
	zassert_equal(init_result, -EINVAL);
	zassert_equal(began, 0);
	zassert_equal(handled, 0);
	zassert_equal(cleaned, 1);
}

ZTEST_SUITE(lifetime, NULL, setup, before, NULL, NULL);

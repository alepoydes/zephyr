/* SPDX-License-Identifier: Apache-2.0 */

#include <zephyr/drivers/uart.h>
#include <zephyr/kernel.h>
#include <zephyr/ztest.h>
#include <zephyr/ztest_error_hook.h>

#define STACK_SIZE (1024 + CONFIG_TEST_EXTRA_STACK_SIZE)

static const struct device *const uart = DEVICE_DT_GET(DT_ALIAS(test_uart));
static K_THREAD_STACK_DEFINE(user_stack, STACK_SIZE);
static struct k_thread user_thread;
static ZTEST_BMEM volatile bool user_returned;
static ZTEST_BMEM volatile unsigned int fault_count;
static ZTEST_BMEM volatile unsigned int fault_reason;

void ztest_post_fatal_error_hook(unsigned int reason, const struct arch_esf *esf)
{
	ARG_UNUSED(esf);
	fault_reason = reason;
	fault_count++;
}

static void user_uart_call(void *arg1, void *arg2, void *arg3)
{
	ARG_UNUSED(arg1);
	ARG_UNUSED(arg2);
	ARG_UNUSED(arg3);

	zassert_true(k_is_user_context());
	if (!IS_ENABLED(CONFIG_UART_USERSPACE)) {
		ztest_set_fault_valid(true);
	}
	uart_poll_out(uart, 'U');
	user_returned = true;
	if (!IS_ENABLED(CONFIG_UART_USERSPACE)) {
		ztest_set_fault_valid(false);
	}
}

ZTEST(uart_userspace_api, test_uart_user_permission_and_supervisor_access)
{
	zassert_equal(IS_ENABLED(CONFIG_UART_USERSPACE),
		      !IS_ENABLED(TEST_UART_USERSPACE_DISABLED));
	zassert_true(device_is_ready(uart));
	uart_poll_out(uart, 'S');

	user_returned = false;
	fault_count = 0;
	fault_reason = 0;
	k_tid_t tid = k_thread_create(&user_thread, user_stack, STACK_SIZE,
				      user_uart_call, NULL, NULL, NULL,
				      K_PRIO_PREEMPT(5), K_USER, K_FOREVER);
	k_object_access_grant(uart, tid);
	k_thread_start(tid);
	zassert_ok(k_thread_join(tid, K_SECONDS(1)));

	if (IS_ENABLED(CONFIG_UART_USERSPACE)) {
		zassert_true(user_returned);
		zassert_equal(fault_count, 0);
	} else {
		zassert_false(user_returned);
		zassert_equal(fault_count, 1);
		zassert_equal(fault_reason, K_ERR_KERNEL_OOPS);
	}
	uart_poll_out(uart, 'T');
}

ZTEST_SUITE(uart_userspace_api, NULL, NULL, NULL, NULL, NULL);

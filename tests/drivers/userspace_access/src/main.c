/* SPDX-License-Identifier: Apache-2.0 */

#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/kernel.h>
#include <zephyr/ztest.h>
#include <zephyr/ztest_error_hook.h>

#define STACK_SIZE (1024 + CONFIG_TEST_EXTRA_STACK_SIZE)

static const struct device *const uart = DEVICE_DT_GET(DT_ALIAS(test_uart));
static const struct device *const gpio = DEVICE_DT_GET(DT_ALIAS(test_gpio));
static K_THREAD_STACK_DEFINE(user_stack, STACK_SIZE);
static struct k_thread user_thread;
static ZTEST_BMEM volatile bool user_returned;
static ZTEST_BMEM volatile int user_gpio_set_result;
static ZTEST_BMEM volatile int user_gpio_get_result;
static ZTEST_BMEM volatile gpio_port_value_t user_gpio_value;
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

static void user_gpio_call(void *arg1, void *arg2, void *arg3)
{
	gpio_port_value_t value = 0;

	ARG_UNUSED(arg1);
	ARG_UNUSED(arg2);
	ARG_UNUSED(arg3);

	zassert_true(k_is_user_context());
	if (!IS_ENABLED(CONFIG_GPIO_USERSPACE)) {
		ztest_set_fault_valid(true);
	}
	user_gpio_set_result = gpio_port_set_bits_raw(gpio, BIT(0));
	user_gpio_get_result = gpio_port_get_raw(gpio, &value);
	user_gpio_value = value;
	user_returned = true;
	if (!IS_ENABLED(CONFIG_GPIO_USERSPACE)) {
		ztest_set_fault_valid(false);
	}
}

static void check_user_access(const struct device *dev, k_thread_entry_t entry, bool enabled)
{
	k_tid_t tid;

	user_returned = false;
	fault_count = 0;
	fault_reason = 0;
	tid = k_thread_create(&user_thread, user_stack, STACK_SIZE,
			      entry, NULL, NULL, NULL,
			      K_PRIO_PREEMPT(5), K_USER, K_FOREVER);
	k_object_access_grant(dev, tid);
	k_thread_start(tid);
	zassert_ok(k_thread_join(tid, K_SECONDS(1)));

	if (enabled) {
		zassert_true(user_returned);
		zassert_equal(fault_count, 0);
	} else {
		zassert_false(user_returned);
		zassert_equal(fault_count, 1);
		zassert_equal(fault_reason, K_ERR_KERNEL_OOPS);
	}
}

ZTEST(driver_userspace_access, test_uart_user_permission_and_supervisor_access)
{
	zassert_equal(IS_ENABLED(CONFIG_UART_USERSPACE),
		      !IS_ENABLED(TEST_DRIVER_USERSPACE_DISABLED));
	zassert_true(device_is_ready(uart));
	uart_poll_out(uart, 'S');
	check_user_access(uart, user_uart_call, IS_ENABLED(CONFIG_UART_USERSPACE));
	uart_poll_out(uart, 'T');
}

ZTEST(driver_userspace_access, test_gpio_user_permission_and_supervisor_access)
{
	gpio_port_value_t value = 0;

	zassert_equal(IS_ENABLED(CONFIG_GPIO_USERSPACE),
		      !IS_ENABLED(TEST_DRIVER_USERSPACE_DISABLED));
	zassert_true(device_is_ready(gpio));
	zassert_ok(gpio_pin_configure(gpio, 0, GPIO_INPUT | GPIO_OUTPUT_LOW));
	zassert_ok(gpio_port_get_raw(gpio, &value));
	zassert_equal(value & BIT(0), 0);

	user_gpio_set_result = -1;
	user_gpio_get_result = -1;
	user_gpio_value = 0;
	check_user_access(gpio, user_gpio_call, IS_ENABLED(CONFIG_GPIO_USERSPACE));
	if (IS_ENABLED(CONFIG_GPIO_USERSPACE)) {
		zassert_ok(user_gpio_set_result);
		zassert_ok(user_gpio_get_result);
		zassert_equal(user_gpio_value & BIT(0), BIT(0));
	}

	zassert_ok(gpio_port_clear_bits_raw(gpio, BIT(0)));
	zassert_ok(gpio_port_get_raw(gpio, &value));
	zassert_equal(value & BIT(0), 0);
}

ZTEST_SUITE(driver_userspace_access, NULL, NULL, NULL, NULL, NULL);

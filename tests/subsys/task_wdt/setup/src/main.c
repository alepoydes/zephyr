/* SPDX-License-Identifier: Apache-2.0 */
#include <zephyr/device.h>
#include <zephyr/drivers/watchdog.h>
#include <zephyr/task_wdt/task_wdt.h>
#include <zephyr/ztest.h>

static int setup_error = -EIO;
static unsigned int setup_calls;
static unsigned int feeds;

static int install(const struct device *dev, const struct wdt_timeout_cfg *cfg)
{
	return 0;
}

static int setup(const struct device *dev, uint8_t options)
{
	setup_calls++;
	return setup_error;
}

static int feed(const struct device *dev, int channel)
{
	zassert_ok(setup_error, "Fed a watchdog whose setup failed");
	feeds++;
	return 0;
}

static DEVICE_API(wdt, api) = {
	.install_timeout = install,
	.setup = setup,
	.feed = feed,
};

DEVICE_DEFINE(test_wdt, "test_wdt", NULL, NULL, NULL, NULL,
	      POST_KERNEL, CONFIG_KERNEL_INIT_PRIORITY_DEVICE, &api);

ZTEST(task_wdt_setup, test_failed_setup_preserves_error_and_channel_capacity)
{
	zassert_ok(task_wdt_init(DEVICE_GET(test_wdt)));
	for (int i = 0; i < 3; i++) {
		zassert_equal(task_wdt_add(1000, NULL, NULL), -EIO);
	}
	zassert_equal(feeds, 0);
	zassert_equal(setup_calls, 3);

	setup_error = 0;
	int first = task_wdt_add(1000, NULL, NULL);
	int second = task_wdt_add(1000, NULL, NULL);

	zassert_true(first >= 0);
	zassert_true(second >= 0);
	zassert_not_equal(first, second);
	zassert_equal(setup_calls, 4);
	zassert_true(feeds >= 2);
	zassert_equal(task_wdt_add(1000, NULL, NULL), -ENOMEM);
	zassert_ok(task_wdt_delete(first));
	zassert_ok(task_wdt_delete(second));
	task_wdt_suspend();
}

ZTEST_SUITE(task_wdt_setup, NULL, NULL, NULL, NULL, NULL);

/* SPDX-License-Identifier: Apache-2.0 */
#include <zephyr/drivers/adc.h>
#include <zephyr/kernel.h>
#include <zephyr/ztest.h>
#include <cmsis_core.h>

/* Physical F4 regression: mask only the ADC IRQ, leaving the kernel timer
 * running. Timeout must complete the read before a late IRQ can use its buffer.
 * No input voltage or external loopback is required for these lifecycle checks.
 */
static const struct adc_dt_spec adc = ADC_DT_SPEC_GET(DT_PATH(zephyr_user));
#define ADC_IRQ DT_IRQN(DT_NODELABEL(adc1))

ZTEST(adc_timeout, test_timeout_recovery_and_sequence)
{
	uint16_t samples[3] = {0};
	struct adc_sequence sequence = {.buffer = samples, .buffer_size = sizeof(samples)};

	zassert_true(adc_is_ready_dt(&adc));
	zassert_ok(adc_channel_setup_dt(&adc));
	zassert_ok(adc_sequence_init_dt(&adc, &sequence));
	/* The first operation must also work from the driver's cold state. */
	zassert_ok(adc_read_dt(&adc, &sequence));
	irq_disable(ADC_IRQ);
	int64_t start = k_uptime_get();
	int result = adc_read_dt(&adc, &sequence);

	/* Restore IRQ admission even on failure before reporting assertions. */
	irq_enable(ADC_IRQ);
	zassert_equal(result, -ETIMEDOUT);
	zassert_between_inclusive(k_uptime_get() - start, 2, 20);
	samples[0] = 0x5a5a;
	NVIC_SetPendingIRQ(ADC_IRQ);
	k_msleep(3);
	zassert_equal(samples[0], 0x5a5a, "Late ISR wrote into completed buffer");
	zassert_ok(adc_read_dt(&adc, &sequence));
	zassert_true(samples[0] < 4096);

	struct k_poll_signal signal = K_POLL_SIGNAL_INITIALIZER(signal);
	struct k_poll_event event = K_POLL_EVENT_INITIALIZER(
		K_POLL_TYPE_SIGNAL, K_POLL_MODE_NOTIFY_ONLY, &signal);

	irq_disable(ADC_IRQ);
	result = adc_read_async(adc.dev, &sequence, &signal);
	int waited = result ? result : k_poll(&event, 1, K_MSEC(20));

	irq_enable(ADC_IRQ);
	zassert_ok(result);
	zassert_ok(waited);
	unsigned int signaled;
	k_poll_signal_check(&signal, &signaled, &result);
	zassert_true(signaled);
	zassert_equal(result, -ETIMEDOUT);
	zassert_ok(adc_read_dt(&adc, &sequence));

	/* Intentional intervals exceed the per-conversion timeout. */
	struct adc_sequence_options options = {.interval_us = 5000, .extra_samplings = 2};
	sequence.options = &options;
	zassert_ok(adc_read_dt(&adc, &sequence));
	for (size_t i = 0; i < ARRAY_SIZE(samples); i++) {
		zassert_true(samples[i] < 4096);
	}
}
ZTEST_SUITE(adc_timeout, NULL, NULL, NULL, NULL, NULL);

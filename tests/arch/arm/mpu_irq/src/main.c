// SPDX-License-Identifier: Apache-2.0
#include <zephyr/ztest.h>
#include <zephyr/irq.h>
#include <cmsis_core.h>

/* Inject a real pending IRQ at the MPU driver boundary during an ordinary
 * context switch. Do not replace the region programming itself. The Cortex-M3
 * QEMU board has an unused, implemented NVIC line 31.
 */
struct z_arm_mpu_partition;
void __real_arm_core_mpu_configure_dynamic_mpu_regions(
	const struct z_arm_mpu_partition *, uint8_t);
static volatile bool armed, updating;
static volatile unsigned int interruptions, delivered, updates;

static void interrupt(const void *unused)
{
	interruptions += updating;
	delivered++;
}

void __wrap_arm_core_mpu_configure_dynamic_mpu_regions(
	const struct z_arm_mpu_partition *regions, uint8_t count)
{
	if (armed) {
		updating = true;
		updates++;
		NVIC_SetPendingIRQ(31);
		__DSB();
		__ISB();
	}
	__real_arm_core_mpu_configure_dynamic_mpu_regions(regions, count);
	updating = false;
}

ZTEST(mpu_irq, test_switch_defers_irq_until_mapping_is_complete)
{
	IRQ_CONNECT(31, 0, interrupt, NULL, 0);
	irq_enable(31);
	armed = true;
	k_sleep(K_MSEC(1));
	armed = false;
	irq_disable(31);
	zassert_true(updates > 0, "No context switch exercised");
	zassert_true(delivered > 0, "Pending IRQ was lost");
	zassert_equal(interruptions, 0, "IRQ observed an MPU update: %u/%u",
		      interruptions, updates);
}
ZTEST_SUITE(mpu_irq, NULL, NULL, NULL, NULL, NULL);

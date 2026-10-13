/* SPDX-License-Identifier: Apache-2.0 */

#include <cmsis_core.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/linker/section_tags.h>
#include <zephyr/platform/hooks.h>
#include <zephyr/ztest.h>

static volatile uint32_t __noinit early_primask;
static uint32_t pre_kernel_primask;

/* The early reset hook cannot rely on a usable stack or initialized RAM. */
__attribute__((naked)) void soc_early_reset_hook(void)
{
	__asm__ volatile("mrs r0, primask\n"
			 "ldr r1, =early_primask\n"
			 "str r0, [r1]\n"
			 "bx lr\n");
}

static int capture_pre_kernel_primask(void)
{
	pre_kernel_primask = __get_PRIMASK();
	return 0;
}
SYS_INIT(capture_pre_kernel_primask, PRE_KERNEL_1, 0);

K_SEM_DEFINE(timer_fired, 0, 1);

static void timer_expired(struct k_timer *timer)
{
	ARG_UNUSED(timer);
	k_sem_give(&timer_fired);
}
K_TIMER_DEFINE(test_timer, timer_expired, NULL);

ZTEST(arm_boot_handoff, test_interrupt_mask_handoff)
{
	zassert_equal(early_primask, 1, "interrupts were enabled at early reset hook");
	zassert_equal(pre_kernel_primask, 1, "interrupts were enabled at PRE_KERNEL_1");
	zassert_equal(__get_PRIMASK(), 0, "interrupts remained masked in thread context");

	k_timer_start(&test_timer, K_MSEC(2), K_NO_WAIT);
	zassert_ok(k_sem_take(&timer_fired, K_MSEC(250)), "kernel timer interrupt did not run");
}

ZTEST_SUITE(arm_boot_handoff, NULL, NULL, NULL, NULL, NULL);

// SPDX-License-Identifier: Apache-2.0
#include <zephyr/ztest.h>

#include "udc_stm32_bc12.h"

static void reach_primary(struct stm32_bc12_detection *state)
{
	stm32_bc12_start(state, 0);
	zassert_true(stm32_bc12_step(state, true, 10) > 0);
	zassert_equal(state->phase, STM32_BC12_SETTLE);
	zassert_true(stm32_bc12_step(state, true, 109) > 0);
	zassert_equal(state->phase, STM32_BC12_SETTLE);
	zassert_true(stm32_bc12_step(state, false, 110) > 0);
	zassert_equal(state->phase, STM32_BC12_PRIMARY);
}

ZTEST(stm32_bc12, test_source_classes)
{
	struct stm32_bc12_detection state;

	reach_primary(&state);
	zassert_equal(stm32_bc12_step(&state, false, 210), -1);
	zassert_equal(state.type, BC12_TYPE_SDP);

	reach_primary(&state);
	zassert_true(stm32_bc12_step(&state, true, 210) > 0);
	zassert_equal(state.phase, STM32_BC12_SECONDARY);
	zassert_equal(stm32_bc12_step(&state, false, 310), -1);
	zassert_equal(state.type, BC12_TYPE_CDP);

	reach_primary(&state);
	zassert_true(stm32_bc12_step(&state, true, 210) > 0);
	zassert_equal(stm32_bc12_step(&state, true, 310), -1);
	zassert_equal(state.type, BC12_TYPE_DCP);
}

ZTEST(stm32_bc12, test_timeout_and_voltage_settling)
{
	struct stm32_bc12_detection state;

	stm32_bc12_start(&state, 0);
	zassert_true(stm32_bc12_step(&state, false, 999) > 0);
	zassert_equal(state.phase, STM32_BC12_CONTACT);
	/* A contact indication arriving at the timeout is too late. */
	zassert_equal(stm32_bc12_step(&state, true, 1000), -1);
	zassert_equal(state.type, BC12_TYPE_UNKNOWN);

	reach_primary(&state);
	zassert_true(stm32_bc12_step(&state, true, 209) > 0);
	zassert_equal(state.phase, STM32_BC12_PRIMARY);
	zassert_true(stm32_bc12_step(&state, true, 210) > 0);
	zassert_true(stm32_bc12_step(&state, true, 309) > 0);
	zassert_equal(state.phase, STM32_BC12_SECONDARY);
	zassert_equal(stm32_bc12_step(&state, true, 310), -1);
	zassert_equal(state.type, BC12_TYPE_DCP);
}

ZTEST(stm32_bc12, test_restart_discards_partial_detection)
{
	struct stm32_bc12_detection state;

	reach_primary(&state);
	zassert_true(stm32_bc12_step(&state, true, 210) > 0);
	zassert_equal(state.phase, STM32_BC12_SECONDARY);
	stm32_bc12_start(&state, 250);
	zassert_equal(state.type, BC12_TYPE_NONE);
	zassert_equal(state.phase, STM32_BC12_CONTACT);
	zassert_true(stm32_bc12_step(&state, true, 310) > 0);
	zassert_equal(state.phase, STM32_BC12_SETTLE);
	zassert_true(stm32_bc12_step(&state, true, 409) > 0);
	zassert_equal(state.phase, STM32_BC12_SETTLE);
	zassert_true(stm32_bc12_step(&state, false, 410) > 0);
	zassert_equal(state.phase, STM32_BC12_PRIMARY);
	zassert_equal(stm32_bc12_step(&state, false, 510), -1);
	zassert_equal(state.type, BC12_TYPE_SDP);
}

ZTEST_SUITE(stm32_bc12, NULL, NULL, NULL, NULL, NULL);

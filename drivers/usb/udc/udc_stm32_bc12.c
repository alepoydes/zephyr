/* SPDX-License-Identifier: MIT
 * Copyright (c) 2022-2026 The Pybricks Authors
 * Detection sequence adapted from lib/pbio/drv/usb/usb_stm32.c at a7b1b04ae.
 * Explicit deadlines replace the cooperative firmware's await operations.
 */
#include "udc_stm32_bc12.h"

void stm32_bc12_start(struct stm32_bc12_detection *state, int64_t now)
{
	*state = (struct stm32_bc12_detection) {
		.phase = STM32_BC12_CONTACT, .deadline = now + 1000,
	};
}

int stm32_bc12_step(struct stm32_bc12_detection *state, bool detected, int64_t now)
{
	if (state->phase == STM32_BC12_CONTACT) {
		if (now >= state->deadline) {
			state->phase = STM32_BC12_COMPLETE;
			state->type = BC12_TYPE_UNKNOWN;
		} else if (detected) {
			state->phase = STM32_BC12_SETTLE;
			state->deadline = now + 100;
		}
	} else if (now >= state->deadline) {
		switch (state->phase) {
		case STM32_BC12_SETTLE:
			state->phase = STM32_BC12_PRIMARY;
			state->deadline = now + 100;
			break;
		case STM32_BC12_PRIMARY:
			if (detected) {
				state->phase = STM32_BC12_SECONDARY;
				state->deadline = now + 100;
			} else {
				state->phase = STM32_BC12_COMPLETE;
				state->type = BC12_TYPE_SDP;
			}
			break;
		case STM32_BC12_SECONDARY:
			state->phase = STM32_BC12_COMPLETE;
			state->type = detected ? BC12_TYPE_DCP : BC12_TYPE_CDP;
			break;
		default:
			break;
		}
	}
	if (state->phase == STM32_BC12_IDLE || state->phase == STM32_BC12_COMPLETE) {
		return -1;
	}
	/* Contact detection has no IRQ here. Bound observation latency to 20 ms;
	 * the voltage-settling phases need only their single deadline wakeup. */
	return MIN(state->deadline - now, state->phase == STM32_BC12_CONTACT ? 20 : 100);
}

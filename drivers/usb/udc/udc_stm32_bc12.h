/* SPDX-License-Identifier: MIT
 * Copyright (c) 2022-2026 The Pybricks Authors
 * Detection sequence adapted from lib/pbio/drv/usb/usb_stm32.c at a7b1b04ae.
 * Register ownership remains in the STM32 UDC driver.
 */
#ifndef ZEPHYR_DRIVERS_USB_UDC_STM32_BC12_H_
#define ZEPHYR_DRIVERS_USB_UDC_STM32_BC12_H_

#include <zephyr/drivers/usb/usb_bc12.h>

enum stm32_bc12_phase {
	STM32_BC12_IDLE, STM32_BC12_CONTACT, STM32_BC12_SETTLE,
	STM32_BC12_PRIMARY, STM32_BC12_SECONDARY, STM32_BC12_COMPLETE,
};

struct stm32_bc12_detection {
	enum stm32_bc12_phase phase;
	enum bc12_type type;
	int64_t deadline;
};

void stm32_bc12_start(struct stm32_bc12_detection *state, int64_t now);
/* The caller supplies the comparator selected by the current phase. Returns
 * the next sampling delay in ms, or -1 when no detection is pending. */
int stm32_bc12_step(struct stm32_bc12_detection *state, bool detected, int64_t now);

#endif

# SPDX-License-Identifier: Apache-2.0
cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(mpu_irq)
target_sources(app PRIVATE src/main.c)
zephyr_link_libraries(-Wl,--wrap=arm_core_mpu_configure_dynamic_mpu_regions)

# SPDX-License-Identifier: Apache-2.0
cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(cdc_acm_tx_complete)
target_include_directories(app PRIVATE ${ZEPHYR_BASE}/subsys/usb/host)
target_sources(app PRIVATE src/main.c)
target_link_options(app PUBLIC -Wl,--wrap=net_buf_alloc_fixed -Wl,--wrap=usbd_ep_enqueue)

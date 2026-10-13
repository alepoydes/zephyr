# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr COMPONENTS unittest REQUIRED HINTS $ENV{ZEPHYR_BASE})

project(bt_keys_br_eviction)

add_subdirectory(${ZEPHYR_BASE}/tests/bluetooth/host host_mocks)

target_link_libraries(testbinary PRIVATE host_mocks)

target_sources(testbinary PRIVATE
    src/main.c
    ${ZEPHYR_BASE}/subsys/bluetooth/host/classic/keys_br.c
    ${ZEPHYR_BASE}/subsys/bluetooth/common/addr.c
)

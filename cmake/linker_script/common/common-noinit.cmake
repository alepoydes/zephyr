# SPDX-License-Identifier: Apache-2.0
# The contents of this file is based on include/zephyr/linker/common-noinit.ld
# Please keep in sync

zephyr_linker_section(NAME .noinit GROUP NOINIT_REGION TYPE NOLOAD NOINIT NOINPUT)

if(CONFIG_USERSPACE)
  zephyr_linker_section_configure(
    SECTION .noinit
    PRIO 50
    INPUT ".user_stacks*"
    SYMBOLS z_user_stacks_start z_user_stacks_end)

endif()
# Keep MPU-aligned user stacks before the ordinary noinit objects.
zephyr_linker_section_configure(SECTION .noinit INPUT ".noinit" ".noinit.*")
if(CONFIG_LLEXT)
  zephyr_linker_section_configure(
    SECTION .noinit INPUT ".llext_heap" ".llext_ext_heap" ".llext_metadata_heap")
endif()
# TODO: #include <snippets-noinit.ld>
include(${COMMON_ZEPHYR_LINKER_DIR}/kobject-priv-stacks.cmake)

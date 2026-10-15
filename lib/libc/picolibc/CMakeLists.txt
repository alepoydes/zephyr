# SPDX-License-Identifier: Apache-2.0

zephyr_library()
zephyr_library_sources(
  assert.c
  cbprintf.c
  chk_fail.c
  errno_wrap.c
  exit.c
  locks.c
  stdio.c
  )
zephyr_include_directories(include)

zephyr_library_compile_options($<TARGET_PROPERTY:compiler,prohibit_lto>)

# define __LINUX_ERRNO_EXTENSIONS__ so we get errno defines like -ESHUTDOWN
# used by the network stack
zephyr_compile_definitions(__LINUX_ERRNO_EXTENSIONS__)

if(NOT CONFIG_PICOLIBC_USE_MODULE)

  if(CONFIG_PICOLIBC_TOOLCHAIN_SPEED_OPTIMIZATIONS)
    function(picolibc_select_toolchain_speed_archive)
      # GCC selects size multilibs if -Os occurs anywhere in the query.
      compiler_simple_options(simple_options)
      foreach(option IN LISTS TOOLCHAIN_C_FLAGS simple_options)
        if(option MATCHES "^-O")
          message(FATAL_ERROR
            "Picolibc speed archive selection requires no additional optimization flag: ${option}")
        endif()
      endforeach()

      # Keep the firmware's optimization (and LTO) flags unchanged. The
      # override is local to compiler_file_path's archive lookup.
      set(COMPILER_OPTIMIZATION_FLAG ${COMPILER_OPTIMIZE_FOR_SPEED_FLAG})
      compiler_file_path(libc.a picolibc_speed_archive)
      if(NOT IS_ABSOLUTE "${picolibc_speed_archive}" OR
         NOT EXISTS "${picolibc_speed_archive}")
        message(FATAL_ERROR "GNU toolchain Picolibc speed archive libc.a was not found")
      endif()
      set_linker_property(PROPERTY c_library "${picolibc_speed_archive}")
    endfunction()
    picolibc_select_toolchain_speed_archive()
  endif()

  # Use picolibc provided with the toolchain. This requires a new enough
  # toolchain so that the version of picolibc supports auto-detecting a
  # Zephyr build (via the __ZEPHYR__ macro) to expose the Zephyr C API
  zephyr_compile_options(PROPERTY specs picolibc.specs)
  zephyr_link_libraries(PROPERTY specs picolibc.specs)
  if(CONFIG_PICOLIBC_IO_FLOAT)
    zephyr_compile_definitions(PICOLIBC_DOUBLE_PRINTF_SCANF)
    zephyr_link_libraries(-DPICOLIBC_DOUBLE_PRINTF_SCANF)
  elseif(CONFIG_PICOLIBC_IO_MINIMAL)
    zephyr_compile_definitions(PICOLIBC_MINIMAL_PRINTF_SCANF)
    zephyr_link_libraries(-DPICOLIBC_MINIMAL_PRINTF_SCANF)
  elseif(CONFIG_PICOLIBC_IO_LONG_LONG)
    zephyr_compile_definitions(PICOLIBC_LONG_LONG_PRINTF_SCANF)
    zephyr_link_libraries(-DPICOLIBC_LONG_LONG_PRINTF_SCANF)
  else()
    zephyr_compile_definitions(PICOLIBC_INTEGER_PRINTF_SCANF)
    zephyr_link_libraries(-DPICOLIBC_INTEGER_PRINTF_SCANF)
  endif()

endif()

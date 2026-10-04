# SPDX-License-Identifier: MIT
if(CONFIG_ZMK_DISPLAY AND CONFIG_EYELASH_NV_WIDGET_STATUS)
  zephyr_library_include_directories(${CMAKE_SOURCE_DIR}/include)
  zephyr_library_sources(custom_status_screen.c)
  zephyr_library_sources(widgets/bolt.c)
  zephyr_library_sources(widgets/util.c)
  zephyr_library_sources(widgets/peripheral_status.c)
  zephyr_library_sources(widgets/custom_art.c)
endif()

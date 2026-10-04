if(CONFIG_EYELASH_DISPLAY_REFRESH)
  zephyr_library()
  zephyr_library_sources(src/display_refresh.c)
endif()

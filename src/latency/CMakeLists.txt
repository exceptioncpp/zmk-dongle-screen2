zephyr_library_named(dongle_screen_latency)

zephyr_library_include_directories(${ZEPHYR_CURRENT_MODULE_DIR}/include)

zephyr_library_sources(
  metrics_core.c
  metrics_wrappers.c
  metrics_transport.c
  metrics_display.c
)

if(TARGET app)
  target_link_options(app PRIVATE -Wl,--wrap=k_msgq_put -Wl,--wrap=k_msgq_get -Wl,--wrap=k_cpu_idle)
  if(CONFIG_DONGLE_SCREEN_LATENCY_SHOW_BLE)
    target_link_options(app PRIVATE -Wl,--wrap=bt_gatt_notify -Wl,--wrap=bt_gatt_notify_cb)
    target_link_options(app PRIVATE -Wl,--wrap=zmk_hog_send_keyboard_report)
    if(CONFIG_ZMK_SPLIT)
      target_link_options(app PRIVATE -Wl,--wrap=zmk_split_transport_central_peripheral_event_handler)
    endif()
  endif()
  if(CONFIG_DONGLE_SCREEN_LATENCY_SHOW_USB)
    target_link_options(app PRIVATE -Wl,--wrap=k_sem_take -Wl,--wrap=k_sem_give -Wl,--wrap=hid_int_ep_write)
  endif()
endif()

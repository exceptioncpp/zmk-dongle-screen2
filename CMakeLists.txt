zephyr_compile_definitions(LV_LVGL_H_INCLUDE_SIMPLE=1)

zephyr_include_directories(include)
zephyr_library()

if(CONFIG_SHIELD_DONGLE_SCREEN)

        add_subdirectory(${ZEPHYR_CURRENT_MODULE_DIR}/drivers/display)
        add_subdirectory(${ZEPHYR_CURRENT_MODULE_DIR}/modules/lvgl)

        if(CONFIG_DONGLE_SCREEN_LATENCY_METRICS)
                add_subdirectory(${ZEPHYR_CURRENT_MODULE_DIR}/src/latency)
        endif()


endif()
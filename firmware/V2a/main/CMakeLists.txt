set(T1E_SRCS
    "main.c"
    "hal/hal_sleep.c"
    "hal/hal_buttons.c"
    "hal/hal_power.c"
    "drivers/epd/epd_ssd1681.c"
    "drivers/epd/epd_gfx.c"
    "drivers/rtc/rtc_ds3231.c"
    "app/state.c"
    "app/modes/mode_clock.c"
    "app/modes/mode_life_clock.c"
    "app/modes/mode_dice.c"
    "app/modes/mode_art.c"
    "app/modes/mode_pomo.c"
    "app/modes/mode_life.c"
    "app/modes/mode_settings.c"
    "tests/epd_hello_test.c"
)

set(T1E_INCLUDE_DIRS
    "."
    "hal"
    "drivers/epd"
    "drivers/rtc"
    "app"
    "app/modes"
    "assets/fonts"
    "tests"
)

set(T1E_REQUIRES
    driver
    esp_adc
    esp_driver_gpio
    esp_driver_i2c
    esp_driver_spi
    esp_hw_support
    esp_event
    esp_netif
    esp_wifi
    lwip
    nvs_flash
)

# Keep hello-test builds minimal to avoid pulling full app state/modes into RAM.
# PlatformIO doesn't always propagate custom -D flags into ESP-IDF CMake compile
# commands, so detect hello builds from the actual build directory name.
set(T1E_HELLO_BUILD OFF)
if(CMAKE_BINARY_DIR MATCHES "hello")
    set(T1E_HELLO_BUILD ON)
endif()
if(CMAKE_CURRENT_BINARY_DIR MATCHES "hello")
    set(T1E_HELLO_BUILD ON)
endif()

if(T1E_HELLO_BUILD)
    set(T1E_SRCS
        "main.c"
        "drivers/epd/epd_ssd1681.c"
        "drivers/epd/epd_gfx.c"
        "tests/epd_hello_test.c"
    )

    set(T1E_INCLUDE_DIRS
        "."
        "hal"
        "drivers/epd"
        "assets/fonts"
        "tests"
    )

    set(T1E_REQUIRES
        esp_driver_gpio
        esp_driver_spi
    )
endif()

idf_component_register(
    SRCS ${T1E_SRCS}
    INCLUDE_DIRS ${T1E_INCLUDE_DIRS}
    REQUIRES ${T1E_REQUIRES}
)

# Ensure board/test defines are always present for this component.
if(IDF_TARGET STREQUAL "esp32c3")
    target_compile_definitions(${COMPONENT_LIB} PRIVATE BOARD_VARIANT_C3=1)
elseif(IDF_TARGET STREQUAL "esp32c6")
    target_compile_definitions(${COMPONENT_LIB} PRIVATE BOARD_VARIANT_C6=1)
endif()

if(T1E_HELLO_BUILD)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE EPD_HELLO_TEST=1)
endif()

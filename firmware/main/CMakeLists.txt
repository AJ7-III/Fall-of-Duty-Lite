set(core_sources)
if(NOT CONFIG_FOD_BOARD_DIAGNOSTIC)
    set(core_sources "../../app.cpp" "../../audio.cpp" "../../game.cpp" "../../assets.cpp" "../../light.cpp" "../../mission.cpp" "../../render.cpp" "../../hud.cpp" "../../trace.cpp" "../../host/briar_house.cpp")
endif()
idf_component_register(
    SRCS "board.cpp" "display.cpp" "gamepad.cpp" "audio.cpp" "main.cpp" ${core_sources}
    INCLUDE_DIRS "../.."
    REQUIRES esp_lcd esp_hw_support esp_timer esp_driver_gpio esp_driver_i2c
             esp_driver_i2s esp_driver_ppa fatfs esp_driver_sdmmc sdmmc
             usb nvs_flash esp_psram spi_flash esp_driver_uart esp_rom
)
target_compile_features(${COMPONENT_LIB} PRIVATE cxx_std_20)
target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-missing-field-initializers)

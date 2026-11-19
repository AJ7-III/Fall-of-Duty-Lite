# Compile a narrowly instrumented version of the pinned driver in the build
# directory. The ESP-IDF checkout is shared and remains untouched.
if(NOT IDF_VERSION_MAJOR EQUAL 5 OR NOT IDF_VERSION_MINOR EQUAL 5 OR NOT IDF_VERSION_PATCH EQUAL 3)
    message(FATAL_ERROR "Fall of Duty firmware requires ESP-IDF v5.5.3")
endif()
idf_component_get_property(lcd esp_lcd COMPONENT_LIB)
get_target_property(lcd_sources ${lcd} SOURCES)
set(stock_driver "$ENV{IDF_PATH}/components/esp_lcd/dsi/esp_lcd_panel_dpi.c")
file(SHA256 "${stock_driver}" driver_sha)
if(NOT driver_sha STREQUAL "6d4a59514c1401334f1a4a89933611b259ca20ef09c9670fabfc37f1dea630da")
    message(FATAL_ERROR "Unexpected DPI driver source for the v5.5.3 scanout patch")
endif()
set(patched_driver "${CMAKE_BINARY_DIR}/fod_esp_lcd_panel_dpi.c")
file(READ "${stock_driver}" driver)
string(REPLACE "typedef struct esp_lcd_dpi_panel_t"
    "extern bool fod_dpi_did_select(uint8_t index, bool presentation_slot);\nextern void fod_dpi_underrun(void);\n\ntypedef struct esp_lcd_dpi_panel_t"
    driver "${driver}")
string(REPLACE "    uint8_t num_fbs;" "    uint8_t scan_fb_index;\n    uint8_t scan_refresh_phase;\n    uint8_t num_fbs;" driver "${driver}")
string(REPLACE "    uint8_t fb_index = dpi_panel->cur_fb_index;"
    "    bool presentation_slot = (++dpi_panel->scan_refresh_phase & 1) == 0;\n    if (presentation_slot) {\n        dpi_panel->scan_fb_index = __atomic_load_n(&dpi_panel->cur_fb_index, __ATOMIC_ACQUIRE);\n    }\n    uint8_t fb_index = dpi_panel->scan_fb_index;"
    driver "${driver}")
string(REPLACE "        dpi_panel->cur_fb_index = draw_buf_fb_index;"
    "        __atomic_store_n(&dpi_panel->cur_fb_index, draw_buf_fb_index, __ATOMIC_RELEASE);"
    driver "${driver}")
string(REPLACE "    dw_gdma_channel_enable_ctrl(chan, true);"
    "    dw_gdma_channel_enable_ctrl(chan, true);\n    yield_needed = fod_dpi_did_select(fb_index, presentation_slot);"
    driver "${driver}")
string(REPLACE [=[        // when an underrun happens, the LCD display may already becomes blue
        // it's too late to recover the display, so we just print an error message
        // as a hint to the user that he should optimize the memory bandwidth (with AXI-ICM)
        ESP_DRAM_LOGE(TAG, "can't fetch data from external memory fast enough, underrun happens");]=]
    "        // Report the count from task context to keep UART traffic out of this ISR.\n        fod_dpi_underrun();"
    driver "${driver}")
if(EXISTS "${patched_driver}")
    file(READ "${patched_driver}" last_driver)
endif()
if(NOT driver STREQUAL last_driver)
    file(WRITE "${patched_driver}" "${driver}")
endif()
list(REMOVE_ITEM lcd_sources "${stock_driver}")
set_property(TARGET ${lcd} PROPERTY SOURCES "${lcd_sources}")
target_sources(${lcd} PRIVATE "${patched_driver}")
target_include_directories(${lcd} PRIVATE "$ENV{IDF_PATH}/components/esp_lcd/dsi")

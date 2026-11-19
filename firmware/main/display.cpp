#include "board.hpp"
#include "st7701_init.h"

#include <algorithm>
#include <cinttypes>

#include "driver/gpio.h"
#include "driver/ppa.h"
#include "esp_lcd_mipi_dsi.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_ldo_regulator.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace board {
namespace {
constexpr auto tag = "fod-display";
constexpr size_t frameBytes = nativeWidth * nativeHeight * sizeof(uint16_t);
uint16_t* buffers[3]{};
esp_lcd_panel_handle_t panel;
ppa_client_handle_t ppa;
TaskHandle_t presenter;
portMUX_TYPE displayLock = portMUX_INITIALIZER_UNLOCKED;
DisplayStatus status;
int drawing = -1;
int pending = -1;
int nextBuffer = 1;
uint32_t sequences[3]{};
SlotSample* capture;
size_t captureSize;
size_t captureCount;
uint32_t captureAudioStart, captureAudioEnd;
int64_t captureStartUs;

// Five columns per glyph, least significant bit at top.
constexpr uint8_t digit[][5]{
    {0x3e,0x51,0x49,0x45,0x3e}, {0,0x42,0x7f,0x40,0}, {0x62,0x51,0x49,0x49,0x46},
    {0x22,0x41,0x49,0x49,0x36}, {0x18,0x14,0x12,0x7f,0x10}, {0x2f,0x49,0x49,0x49,0x31},
    {0x3e,0x49,0x49,0x49,0x30}, {0x01,0x71,0x09,0x05,0x03}, {0x36,0x49,0x49,0x49,0x36},
    {0x06,0x49,0x49,0x49,0x3e}
};
constexpr uint8_t letter[][5]{
    {0x7e,0x09,0x09,0x09,0x7e}, {0x7f,0x49,0x49,0x49,0x36}, {0x3e,0x41,0x41,0x41,0x22},
    {0x7f,0x41,0x41,0x22,0x1c}, {0x7f,0x49,0x49,0x49,0x41}, {0x7f,0x09,0x09,0x09,0x01},
    {0x3e,0x41,0x49,0x49,0x7a}, {0x7f,0x08,0x08,0x08,0x7f}, {0x41,0x41,0x7f,0x41,0x41},
    {0x20,0x40,0x40,0x40,0x3f}, {0x7f,0x08,0x14,0x22,0x41}, {0x7f,0x40,0x40,0x40,0x40},
    {0x7f,0x02,0x0c,0x02,0x7f}, {0x7f,0x04,0x08,0x10,0x7f}, {0x3e,0x41,0x41,0x41,0x3e},
    {0x7f,0x09,0x09,0x09,0x06}, {0x3e,0x41,0x51,0x21,0x5e}, {0x7f,0x09,0x19,0x29,0x46},
    {0x26,0x49,0x49,0x49,0x32}, {0x01,0x01,0x7f,0x01,0x01}, {0x3f,0x40,0x40,0x40,0x3f},
    {0x1f,0x20,0x40,0x20,0x1f}, {0x7f,0x20,0x18,0x20,0x7f}, {0x63,0x14,0x08,0x14,0x63},
    {0x07,0x08,0x70,0x08,0x07}, {0x61,0x51,0x49,0x45,0x43}
};
}

esp_err_t initDisplay() {
    gpio_reset_pin(GPIO_NUM_23);
    gpio_set_direction(GPIO_NUM_23, GPIO_MODE_OUTPUT);
    gpio_set_level(GPIO_NUM_23, 0);

    esp_ldo_channel_handle_t phy;
    const esp_ldo_channel_config_t phyConfig{.chan_id = 3, .voltage_mv = 2500};
    if (const auto err = esp_ldo_acquire_channel(&phyConfig, &phy); err != ESP_OK) return err;
    esp_lcd_dsi_bus_handle_t bus;
    const esp_lcd_dsi_bus_config_t busConfig{
        .bus_id = 0, .num_data_lanes = 2, .phy_clk_src = MIPI_DSI_PHY_CLK_SRC_DEFAULT, .lane_bit_rate_mbps = 500
    };
    if (const auto err = esp_lcd_new_dsi_bus(&busConfig, &bus); err != ESP_OK) return err;
    esp_lcd_panel_io_handle_t io;
    const esp_lcd_dbi_io_config_t ioConfig{.virtual_channel = 0, .lcd_cmd_bits = 8, .lcd_param_bits = 8};
    if (const auto err = esp_lcd_new_panel_io_dbi(bus, &ioConfig, &io); err != ESP_OK) return err;
    const esp_lcd_dpi_panel_config_t panelConfig{
        .virtual_channel = 0, .dpi_clk_src = MIPI_DSI_DPI_CLK_SRC_DEFAULT, .dpi_clock_freq_mhz = 34,
        .pixel_format = LCD_COLOR_PIXEL_FORMAT_RGB565,
        .in_color_format = LCD_COLOR_FMT_RGB565, .out_color_format = LCD_COLOR_FMT_RGB565,
        .num_fbs = 3,
        .video_timing = {
            .h_size = nativeWidth, .v_size = nativeHeight,
            .hsync_pulse_width = 12, .hsync_back_porch = 42, .hsync_front_porch = 42,
            .vsync_pulse_width = 2, .vsync_back_porch = 8, .vsync_front_porch = 166
        },
    };
    if (const auto err = esp_lcd_new_panel_dpi(bus, &panelConfig, &panel); err != ESP_OK) return err;
    void* fb[3]{};
    if (const auto err = esp_lcd_dpi_panel_get_frame_buffer(panel, 3, &fb[0], &fb[1], &fb[2], (void**)nullptr);
        err != ESP_OK) return err;
    for (int i = 0; i < 3; ++i) buffers[i] = static_cast<uint16_t*>(fb[i]);

    gpio_reset_pin(GPIO_NUM_5);
    gpio_set_direction(GPIO_NUM_5, GPIO_MODE_OUTPUT);
    gpio_set_level(GPIO_NUM_5, 0);
    vTaskDelay(pdMS_TO_TICKS(20));
    gpio_set_level(GPIO_NUM_5, 1);
    vTaskDelay(pdMS_TO_TICKS(120));
    for (const auto& cmd : board_p4_panel_init_cmds) {
        if (const auto err = esp_lcd_panel_io_tx_param(io, cmd.cmd, cmd.data, cmd.len); err != ESP_OK) return err;
        if (cmd.delay_ms) vTaskDelay(pdMS_TO_TICKS(cmd.delay_ms));
    }
    if (const auto err = esp_lcd_panel_init(panel); err != ESP_OK) return err;
    const ppa_client_config_t ppaConfig{.oper_type = PPA_OPERATION_SRM, .max_pending_trans_num = 1};
    if (const auto err = ppa_register_client(&ppaConfig, &ppa); err != ESP_OK) return err;
    gpio_set_level(GPIO_NUM_23, 1);
    ESP_LOGI(tag, "480x800 RGB565, stride=960, requested DPI clock=34 MHz, framebuffers=%p %p %p",
             buffers[0], buffers[1], buffers[2]);
    return ESP_OK;
}

void setPresenter() {
    portENTER_CRITICAL(&displayLock);
    presenter = xTaskGetCurrentTaskHandle();
    portEXIT_CRITICAL(&displayLock);
}

bool waitForSlot(uint32_t waitMs) {
    return ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(waitMs)) != 0;
}

DisplayStatus displayStatus() {
    portENTER_CRITICAL(&displayLock);
    const auto current = status;
    portEXIT_CRITICAL(&displayLock);
    return current;
}

bool captureSlots(SlotSample* samples, size_t count, int64_t warmupUs) {
    portENTER_CRITICAL(&displayLock);
    const bool available = samples && count && !capture;
    if (available) {
        capture = samples;
        captureSize = count;
        captureCount = 0;
        captureStartUs = esp_timer_get_time() + warmupUs;
    }
    portEXIT_CRITICAL(&displayLock);
    return available;
}

size_t capturedSlots() {
    portENTER_CRITICAL(&displayLock);
    const auto count = captureCount;
    portEXIT_CRITICAL(&displayLock);
    return count;
}

uint32_t capturedAudioUnderruns() {
    portENTER_CRITICAL(&displayLock);
    const auto count = captureAudioEnd - captureAudioStart;
    portEXIT_CRITICAL(&displayLock);
    return count;
}

Frame acquireFrame() {
    Frame frame;
    portENTER_CRITICAL(&displayLock);
    // A queued image has to reach scanout before another can be submitted.
    // A late slot notification can arrive while that image is still pending.
    if (drawing < 0 && pending < 0) {
        for (int offset = 0; offset < 3; ++offset) {
            const int i = (nextBuffer + offset) % 3;
            if (i == status.index) continue;
            drawing = i;
            frame = {buffers[i], i};
            nextBuffer = (i + 1) % 3;
            break;
        }
    }
    portEXIT_CRITICAL(&displayLock);
    return frame;
}

esp_err_t scaleWorld(Frame frame, const uint16_t* source, int width, int height) {
    if (!frame || !source || !((width == 400 && height == 225) || (width == 320 && height == 180))) {
        return ESP_ERR_INVALID_ARG;
    }
    const float scale = static_cast<float>(logicalWidth) / width;
    const ppa_srm_oper_config_t op{
        .in = {
            .buffer = source,
            .pic_w = static_cast<uint32_t>(width), .pic_h = static_cast<uint32_t>(height),
            .block_w = static_cast<uint32_t>(width), .block_h = static_cast<uint32_t>(height),
            .srm_cm = PPA_SRM_COLOR_MODE_RGB565,
        },
        .out = {
            .buffer = frame.pixels, .buffer_size = frameBytes,
            .pic_w = nativeWidth, .pic_h = nativeHeight, .block_offset_x = 15,
            .srm_cm = PPA_SRM_COLOR_MODE_RGB565,
        },
        .rotation_angle = PPA_SRM_ROTATION_ANGLE_270,
        .scale_x = scale, .scale_y = scale,
        .mode = PPA_TRANS_MODE_BLOCKING,
    };
    return ppa_do_scale_rotate_mirror(ppa, &op);
}

void clearMargins(Frame frame) {
    rect(frame, 0, 0, logicalWidth, 15, 0);
    rect(frame, 0, logicalHeight - 15, logicalWidth, 15, 0);
}

esp_err_t present(Frame frame, uint32_t sequence) {
    portENTER_CRITICAL(&displayLock);
    const bool valid = frame && drawing == frame.index && pending < 0;
    if (valid) {
        pending = frame.index;
        sequences[frame.index] = sequence;
    }
    portEXIT_CRITICAL(&displayLock);
    if (!valid) return ESP_ERR_INVALID_STATE;

    // The stock branch writes the submitted buffer back before changing the
    // requested index. The actual selection is observed by fod_dpi_did_select.
    auto err = esp_lcd_panel_draw_bitmap(panel, 0, 0, nativeWidth, nativeHeight, frame.pixels);
    portENTER_CRITICAL(&displayLock);
    drawing = -1;
    if (err != ESP_OK) pending = -1;
    portEXIT_CRITICAL(&displayLock);
    return err;
}

void discard(Frame frame) {
    portENTER_CRITICAL(&displayLock);
    if (drawing == frame.index) drawing = -1;
    portEXIT_CRITICAL(&displayLock);
}

void rect(Frame frame, int x, int y, int w, int h, uint16_t color) {
    if (!frame) return;
    const int y0 = std::max(y, 0);
    const int y1 = std::min(y + h, logicalHeight);
    if (y0 >= y1) return;
    for (int px = std::max(x, 0); px < std::min(x + w, logicalWidth); ++px) {
        auto* row = frame.pixels + px * nativeWidth;
        std::fill(row + nativeWidth - y1, row + nativeWidth - y0, color);
    }
}

void text(Frame frame, int x, int y, const char* message, uint16_t color, int scale) {
    constexpr uint8_t dot[]{0,0x60,0x60,0,0};
    constexpr uint8_t dash[]{0,0x08,0x08,0x08,0};
    for (; *message; ++message, x += 6 * scale) {
        const char ch = *message;
        const uint8_t* glyph = nullptr;
        if ('0' <= ch && ch <= '9') glyph = digit[ch - '0'];
        if ('A' <= ch && ch <= 'Z') glyph = letter[ch - 'A'];
        if ('a' <= ch && ch <= 'z') glyph = letter[ch - 'a'];
        if (ch == '.') glyph = dot;
        if (ch == '-') glyph = dash;
        if (!glyph) continue;
        for (int col = 0; col < 5; ++col) {
            for (int row = 0; row < 7; ++row) {
                if ((glyph[col] & (1 << row)) != 0) rect(frame, x + col * scale, y + row * scale, scale, scale, color);
            }
        }
    }
}

extern "C" bool fod_dpi_did_select(uint8_t index, bool presentationSlot) {
    TaskHandle_t notify = nullptr;
    portENTER_CRITICAL_ISR(&displayLock);
    ++status.refreshes;
    status.index = index;
    status.sequence = sequences[index];
    if (pending == index) pending = -1;
    if (presentationSlot) {
        ++status.slots;
        if (capture) {
            const int64_t now = esp_timer_get_time();
            if (now >= captureStartUs) {
                if (captureCount == 0) captureAudioStart = audioUnderruns();
                capture[captureCount++] = SlotSample{now, status.sequence, index,
                                                     static_cast<uint16_t>(std::min<uint32_t>(status.underruns, 65535))};
                if (captureCount == captureSize) {
                    captureAudioEnd = audioUnderruns();
                    capture = nullptr;
                }
            }
        }
        notify = presenter;
    }
    portEXIT_CRITICAL_ISR(&displayLock);
    BaseType_t woken = pdFALSE;
    if (notify) vTaskNotifyGiveFromISR(notify, &woken);
    return woken == pdTRUE;
}

extern "C" void fod_dpi_underrun() {
    portENTER_CRITICAL_ISR(&displayLock);
    ++status.underruns;
    portEXIT_CRITICAL_ISR(&displayLock);
}

} // namespace board

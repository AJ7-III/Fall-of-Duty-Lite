#include "board.hpp"
#include "board_i2c.hpp"

#include <algorithm>
#include <cinttypes>
#include <cstdio>

#include "driver/gpio.h"
#include "driver/sdmmc_host.h"
#include "esp_chip_info.h"
#include "esp_clk_tree.h"
#include "esp_heap_caps.h"
#include "esp_ldo_regulator.h"
#include "esp_log.h"
#include "esp_psram.h"
#include "esp_flash.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_vfs_fat.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "sdmmc_cmd.h"

namespace board {
namespace {
constexpr auto tag = "fod-board";
i2c_master_bus_handle_t bus;
i2c_master_dev_handle_t touch;
esp_ldo_channel_handle_t sdPower;
sdmmc_card_t* card;
bool nvsReady;
}

int64_t timeUs() { return esp_timer_get_time(); }

void logProfile() {
    esp_chip_info_t chip{};
    esp_chip_info(&chip);
    uint32_t flash = 0;
    esp_flash_get_physical_size(nullptr, &flash);
    uint32_t cpu = 0;
    esp_clk_tree_src_get_freq_hz(SOC_MOD_CLK_CPU, ESP_CLK_TREE_SRC_FREQ_PRECISION_EXACT, &cpu);
    ESP_LOGI(tag, "chip model=%d revision=%u.%u cores=%u cpu_hz=%" PRIu32
              " flash_bytes=%" PRIu32 " psram_bytes=%u psram_config_mhz=%d idf=%s",
              chip.model, chip.revision / 100, chip.revision % 100, chip.cores, cpu,
              flash, static_cast<unsigned>(esp_psram_get_size()), CONFIG_SPIRAM_SPEED, esp_get_idf_version());
}

void logMemory(const char* stage) {
    for (uint32_t memory : {MALLOC_CAP_INTERNAL, MALLOC_CAP_SPIRAM}) {
        uint32_t caps = memory | MALLOC_CAP_8BIT;
        multi_heap_info_t info{};
        heap_caps_get_info(&info, caps);
        ESP_LOGI(tag, "memory stage=%s kind=%s free=%u allocated=%u largest=%u min_free=%u",
                 stage, (memory == MALLOC_CAP_INTERNAL) ? "internal" : "psram",
                 static_cast<unsigned>(info.total_free_bytes), static_cast<unsigned>(info.total_allocated_bytes),
                 static_cast<unsigned>(info.largest_free_block), static_cast<unsigned>(info.minimum_free_bytes));
    }
}

esp_err_t initI2c() {
    const i2c_master_bus_config_t config{
        .i2c_port = I2C_NUM_0,
        .sda_io_num = GPIO_NUM_7,
        .scl_io_num = GPIO_NUM_8,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags = {.enable_internal_pullup = true},
    };
    return i2c_new_master_bus(&config, &bus);
}

i2c_master_bus_handle_t i2cBus() { return bus; }

esp_err_t initTouch() {
    if (!bus) return ESP_ERR_INVALID_STATE;
    gpio_reset_pin(GPIO_NUM_3);
    gpio_set_direction(GPIO_NUM_3, GPIO_MODE_OUTPUT);
    gpio_set_level(GPIO_NUM_3, 0);
    vTaskDelay(pdMS_TO_TICKS(10));
    gpio_set_level(GPIO_NUM_3, 1);
    vTaskDelay(pdMS_TO_TICKS(50));
    for (uint16_t address : {0x5D, 0x14}) {
        if (i2c_master_probe(bus, address, 20) != ESP_OK) continue;
        i2c_device_config_t config{
            .dev_addr_length = I2C_ADDR_BIT_LEN_7,
            .device_address = address,
            .scl_speed_hz = 400000,
        };
        ESP_LOGI(tag, "GT911 address 0x%02x", address);
        return i2c_master_bus_add_device(bus, &config, &touch);
    }
    return ESP_ERR_NOT_FOUND;
}

void readTouch(fod::Input& input) {
    if (!touch) return;
    static bool wasDown = false;
    const uint8_t statusAddress[]{0x81, 0x4e};
    uint8_t status = 0;
    if (i2c_master_transmit_receive(touch, statusAddress, 2, &status, 1, 2) != ESP_OK || !(status & 0x80)) return;
    const bool down = (status & 0x0f) != 0;
    if (down && !wasDown) {
        const uint8_t coordsAddress[]{0x81, 0x50};
        uint8_t coords[4]{};
        if (i2c_master_transmit_receive(touch, coordsAddress, 2, coords, 4, 2) == ESP_OK) {
            const int x = coords[0] | (coords[1] << 8);
            const int y = coords[2] | (coords[3] << 8);
            input.touchX = std::clamp(y, 0, logicalWidth - 1);
            input.touchY = nativeWidth - 1 - std::clamp(x, 0, nativeWidth - 1);
            wasDown = true;
        }
    } else if (!down) {
        wasDown = false;
    }
    const uint8_t clear[]{0x81, 0x4e, 0};
    i2c_master_transmit(touch, clear, sizeof(clear), 2);
}

esp_err_t mountSd() {
    if (card) return ESP_OK;
    if (!sdPower) {
        const esp_ldo_channel_config_t power{.chan_id = 4, .voltage_mv = 3300};
        if (const auto err = esp_ldo_acquire_channel(&power, &sdPower); err != ESP_OK) return err;
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.slot = SDMMC_HOST_SLOT_0;
    host.max_freq_khz = SDMMC_FREQ_DEFAULT;
    sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
    slot.width = 4;
    slot.clk = GPIO_NUM_43;
    slot.cmd = GPIO_NUM_44;
    slot.d0 = GPIO_NUM_39;
    slot.d1 = GPIO_NUM_40;
    slot.d2 = GPIO_NUM_41;
    slot.d3 = GPIO_NUM_42;
    slot.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;
    const esp_vfs_fat_sdmmc_mount_config_t cfg{
        .format_if_mount_failed = false,
        .max_files = 4,
        .allocation_unit_size = 0,
    };
    const esp_err_t err = esp_vfs_fat_sdmmc_mount("/sd", &host, &slot, &cfg, &card);
    if (err == ESP_OK) sdmmc_card_print_info(stdout, card);
    return err;
}

void unmountSd() {
    if (!card) return;
    esp_vfs_fat_sdcard_unmount("/sd", card);
    card = nullptr;
}

void initializeSettings() {
    nvsReady = nvs_flash_init() == ESP_OK;
    if (!nvsReady) ESP_LOGW(tag, "settings storage unavailable");
}

SavedSettings readSettings() {
    SavedSettings settings{};
    nvs_handle_t handle;
    if (!nvsReady || nvs_open("fod", NVS_READONLY, &handle) != ESP_OK) return settings;
    nvs_get_i32(handle, "difficulty", &settings.difficulty);
    nvs_get_u8(handle, "fast", &settings.fast);
    nvs_get_u8(handle, "voice", &settings.voice);
    nvs_get_u8(handle, "volume", &settings.volume);
    if (nvs_get_u8(handle, "opponent", &settings.opponent) != ESP_OK ||
        settings.opponent > uint8_t(fod::Opponent::FreeForAll)) {
        uint8_t alternate = fod::defaultOpponent == fod::Opponent::Hanz;
        nvs_get_u8(handle, "alternate", &alternate);
        settings.opponent = uint8_t(alternate ? fod::Opponent::Hanz : fod::Opponent::IPwndUrMom);
    }
    size_t len = sizeof(settings.callsign);
    nvs_get_str(handle, "callsign", settings.callsign, &len);
    nvs_close(handle);
    settings.difficulty = std::clamp<int32_t>(settings.difficulty, 1, 10);
    settings.volume = std::min<uint8_t>(settings.volume, 100);
    return settings;
}

esp_err_t writeSettings(const SavedSettings& settings) {
    nvs_handle_t handle;
    if (!nvsReady) return ESP_ERR_INVALID_STATE;
    auto err = nvs_open("fod", NVS_READWRITE, &handle);
    if (err != ESP_OK) return err;
    err = nvs_set_i32(handle, "difficulty", settings.difficulty);
    if (err == ESP_OK) err = nvs_set_u8(handle, "fast", settings.fast);
    if (err == ESP_OK) err = nvs_set_u8(handle, "voice", settings.voice);
    if (err == ESP_OK) err = nvs_set_u8(handle, "volume", settings.volume);
    if (err == ESP_OK) err = nvs_set_u8(handle, "opponent", settings.opponent);
    if (err == ESP_OK) err = nvs_set_str(handle, "callsign", settings.callsign);
    if (err == ESP_OK) err = nvs_commit(handle);
    nvs_close(handle);
    return err;
}

esp_err_t requestLaunch(uint8_t launch) {
    nvs_handle_t handle;
    if (!nvsReady) return ESP_ERR_INVALID_STATE;
    auto err = nvs_open("fod", NVS_READWRITE, &handle);
    if (err != ESP_OK) return err;
    err = nvs_set_u8(handle, "launch", launch);
    if (err == ESP_OK) err = nvs_commit(handle);
    nvs_close(handle);
    return err;
}

uint8_t takeLaunch() {
    uint8_t launch = 0;
    nvs_handle_t handle;
    if (!nvsReady || nvs_open("fod", NVS_READWRITE, &handle) != ESP_OK) return 0;
    if (nvs_get_u8(handle, "launch", &launch) == ESP_OK) {
        nvs_erase_key(handle, "launch");
        nvs_commit(handle);
    }
    nvs_close(handle);
    return launch;
}

} // namespace board

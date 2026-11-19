#include "board.hpp"
#include "board_i2c.hpp"

#include <algorithm>
#include <array>
#include <atomic>

#include "driver/i2s_std.h"
#include "esp_codec_dev.h"
#include "esp_codec_dev_defaults.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace board {
namespace {
constexpr auto tag = "fod-audio";
constexpr int blockFrames = fod::audioHz / 200; // A 5 ms stereo DMA descriptor.
constexpr size_t blockBytes = blockFrames * 2 * sizeof(int16_t);

i2s_chan_handle_t tx;
bool ready;
std::atomic<uint32_t> underruns{0};
portMUX_TYPE mixerLock = portMUX_INITIALIZER_UNLOCKED;
Mix mixCallback;
void* mixContext;

bool onSendQueueOverflow(i2s_chan_handle_t, i2s_event_data_t*, void*) {
    underruns.fetch_add(1, std::memory_order_relaxed);
    return false;
}

void audioTask(void*) {
    std::array<int16_t, blockFrames * 2> pcm{};
    for (;;) {
        portENTER_CRITICAL(&mixerLock);
        const auto mix = mixCallback;
        void* context = mixContext;
        portEXIT_CRITICAL(&mixerLock);
        if (mix) {
            mix(context, pcm.data(), blockFrames);
        } else {
            pcm.fill(0);
        }
        size_t written = 0;
        if (i2s_channel_write(tx, pcm.data(), blockBytes, &written, 30) != ESP_OK || written != blockBytes) {
            underruns.fetch_add(1, std::memory_order_relaxed);
            vTaskDelay(pdMS_TO_TICKS(1));
        }
    }
}

esp_err_t initCodec() {
    audio_codec_i2c_cfg_t control{.port = I2C_NUM_0, .addr = ES8311_CODEC_DEFAULT_ADDR, .bus_handle = i2cBus()};
    const auto* ctrl = audio_codec_new_i2c_ctrl(&control);
    audio_codec_i2s_cfg_t data{.port = I2S_NUM_0, .tx_handle = tx};
    const auto* io = audio_codec_new_i2s_data(&data);
    const auto* gpio = audio_codec_new_gpio();
    if (!(ctrl && io && gpio)) return ESP_ERR_NO_MEM;
    es8311_codec_cfg_t config{
        .ctrl_if = ctrl,
        .gpio_if = gpio,
        .codec_mode = ESP_CODEC_DEV_WORK_MODE_DAC,
        .pa_pin = 11,
        .use_mclk = true,
        .hw_gain = {.pa_voltage = 5.0f, .codec_dac_voltage = 3.3f},
        .mclk_div = I2S_MCLK_MULTIPLE_256,
    };
    const auto* codec = es8311_codec_new(&config);
    if (!codec) return ESP_FAIL;
    esp_codec_dev_cfg_t dev{.dev_type = ESP_CODEC_DEV_TYPE_OUT, .codec_if = codec, .data_if = io};
    const auto output = esp_codec_dev_new(&dev);
    if (!output) return ESP_ERR_NO_MEM;
    esp_codec_dev_sample_info_t sample{
        .bits_per_sample = 16,
        .channel = 2,
        .channel_mask = 0x03,
        .sample_rate = fod::audioHz,
    };
    if (esp_codec_dev_open(output, &sample) != ESP_CODEC_DEV_OK) return ESP_FAIL;
    if (esp_codec_dev_set_out_vol(output, 85) != ESP_CODEC_DEV_OK) return ESP_FAIL;
    return ESP_OK;
}
} // namespace

esp_err_t startAudio() {
    if (!i2cBus()) return ESP_ERR_INVALID_STATE;
    i2s_chan_config_t channel = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    channel.auto_clear_after_cb = true;
    channel.dma_desc_num = 4;
    channel.dma_frame_num = blockFrames;
    if (const auto err = i2s_new_channel(&channel, &tx, nullptr); err != ESP_OK) return err;
    const i2s_std_config_t config{
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(fod::audioHz),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = GPIO_NUM_13, .bclk = GPIO_NUM_12, .ws = GPIO_NUM_10,
            .dout = GPIO_NUM_9, .din = I2S_GPIO_UNUSED,
        },
    };
    if (const auto err = i2s_channel_init_std_mode(tx, &config); err != ESP_OK) return err;
    const i2s_event_callbacks_t callbacks{.on_send_q_ovf = onSendQueueOverflow};
    if (const auto err = i2s_channel_register_event_callback(tx, &callbacks, nullptr); err != ESP_OK) return err;
    const std::array<uint8_t, blockBytes * 4> silence{};
    size_t preloaded = 0;
    if (const auto err = i2s_channel_preload_data(tx, silence.data(), silence.size(), &preloaded); err != ESP_OK) return err;
    if (const auto err = i2s_channel_enable(tx); err != ESP_OK) return err;
    if (const auto err = initCodec(); err != ESP_OK) return err;
    underruns.store(0, std::memory_order_relaxed);
    if (xTaskCreatePinnedToCore(audioTask, "fod-pcm", 4096, nullptr, 11, nullptr, 0) != pdPASS) return ESP_ERR_NO_MEM;
    ready = true;
    ESP_LOGI(tag, "ES8311 I2S %d Hz stereo 16-bit, %d frames per descriptor", fod::audioHz, blockFrames);
    return ESP_OK;
}

void setMixer(Mix mixer, void* context) {
    portENTER_CRITICAL(&mixerLock);
    mixCallback = mixer;
    mixContext = context;
    portEXIT_CRITICAL(&mixerLock);
}

bool audioReady() { return ready; }

uint32_t audioUnderruns() {
    return underruns.load(std::memory_order_relaxed);
}

} // namespace board

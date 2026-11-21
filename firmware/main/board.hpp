#pragma once

#include <cstddef>
#include <cstdint>
#include "esp_err.h"
#include "types.hpp"
#include "campaign_progress.hpp"

namespace board {

constexpr int nativeWidth = 480;
constexpr int nativeHeight = 800;
constexpr int logicalWidth = 800;
constexpr int logicalHeight = 480;
constexpr const char* packPath = "/sd/fall-of-duty.fodp";

int64_t timeUs();
void logProfile();
void logMemory(const char* stage);
esp_err_t initI2c();
esp_err_t initTouch();
void readTouch(fod::Input& input);
esp_err_t mountSd();
void unmountSd();
void initializeSettings();

struct SavedSettings {
    int32_t difficulty = fod::defaultDifficulty;
    uint8_t fast = 0;
    uint8_t voice = 1;
    uint8_t volume = 70;
    uint8_t opponent = uint8_t(fod::defaultOpponent);
    char callsign[20] = "PLAYER";
};
fod::CampaignProgress readCampaign();
bool writeCampaign(const fod::CampaignProgress& progress);
SavedSettings readSettings();
esp_err_t writeSettings(const SavedSettings& settings);
// One-shot arena request that survives the restart used to switch arenas; taking it clears it.
esp_err_t requestLaunch(uint8_t launch);
uint8_t takeLaunch();

struct Frame {
    uint16_t* pixels = nullptr;
    int index = -1;
    explicit operator bool() const { return pixels != nullptr; }
};
struct DisplayStatus {
    uint32_t refreshes = 0, slots = 0, sequence = 0, underruns = 0;
    int index = 0;
};
struct SlotSample {
    int64_t us = 0;
    uint32_t sequence = 0;
    uint16_t index = 0;
    uint16_t underruns = 0;
};

esp_err_t initDisplay();
void setPresenter();
bool waitForSlot(uint32_t waitMs);
DisplayStatus displayStatus();
// Arm exactly one capture. Samples are written by the DMA-restart ISR.
bool captureSlots(SlotSample* samples, size_t count, int64_t warmupUs = 10'000'000);
size_t capturedSlots();
uint32_t capturedAudioUnderruns();
Frame acquireFrame();
esp_err_t scaleWorld(Frame frame, const uint16_t* source, int width, int height);
void clearMargins(Frame frame);
esp_err_t present(Frame frame, uint32_t sequence);
void discard(Frame frame);
void rect(Frame frame, int x, int y, int w, int h, uint16_t color);
void text(Frame frame, int x, int y, const char* message, uint16_t color, int scale = 3);

esp_err_t startGamepad();
fod::Input controllerAt(int64_t deadline);
bool hasController();
uint32_t reportOverflows();

using Mix = void (*)(void*, int16_t*, size_t);
esp_err_t startAudio();
void setMixer(Mix mixer, void* context);
bool audioReady();
uint32_t audioUnderruns();

} // namespace board

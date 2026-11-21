#include "board.hpp"
#include "campaign_voice_timing.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <memory>
#include <optional>
#include <vector>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_memory_utils.h"
#include "esp_random.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#if !CONFIG_FOD_BOARD_DIAGNOSTIC
#include "app.hpp"
#include "assets.hpp"
#include "host/briar_house.hpp"
#include "render.hpp"
#include "trace.hpp"
#endif

namespace {
constexpr auto tag = "fod";
constexpr size_t benchmarkSlots = 3000;
constexpr size_t benchmarkSamples = benchmarkSlots + 1;

void reportError(const char* device, esp_err_t status) {
    if (status != ESP_OK) ESP_LOGW(tag, "%s: %s", device, esp_err_to_name(status));
}

void reportCapture(const board::SlotSample* samples, bool stationary) {
    std::vector<int64_t> intervals;
    intervals.reserve(benchmarkSlots);
    uint32_t misses = 0, longestRun = 0, run = 0;
    int64_t lastPresented = samples[0].us;
    int64_t maximumGap = 0;
    for (size_t i = 1; i < benchmarkSamples; ++i) {
        if (samples[i].sequence == samples[i-1].sequence) {
            ++misses;
            longestRun = std::max(longestRun, ++run);
            continue;
        }
        run = 0;
        const int64_t interval = samples[i].us - lastPresented;
        intervals.push_back(interval);
        maximumGap = std::max(maximumGap, interval);
        lastPresented = samples[i].us;
    }
    maximumGap = std::max(maximumGap, samples[benchmarkSlots].us - lastPresented);
    std::sort(intervals.begin(), intervals.end());
    const auto percentile = [&](size_t percent) {
        return intervals.empty() ? 0 : intervals[(intervals.size() * percent + 99) / 100 - 1];
    };
    const double panelHz = (2e6 * benchmarkSlots) / (samples[benchmarkSlots].us - samples[0].us);
    const uint32_t displayUnderruns = samples[benchmarkSlots].underruns - samples[0].underruns;
    if (stationary) {
        ESP_LOGI(tag, "scanout baseline panel_hz=%.3f slots=%u display_underruns=%" PRIu32
                      " capture_audio_underruns=%" PRIu32,
                 panelHz, static_cast<unsigned>(benchmarkSlots), displayUnderruns, board::capturedAudioUnderruns());
        return;
    }
    ESP_LOGI(tag, "scanout panel_hz=%.3f slots=%u distinct=%" PRIu32 " misses=%" PRIu32 " consecutive=%" PRIu32
                  " p50_us=%" PRId64 " p95_us=%" PRId64 " p99_us=%" PRId64 " worst_gap_us=%" PRId64
                  " display_underruns=%" PRIu32 " capture_audio_underruns=%" PRIu32,
             panelHz, static_cast<unsigned>(benchmarkSlots), static_cast<uint32_t>(benchmarkSlots) - misses,
             misses, longestRun, percentile(50), percentile(95), percentile(99), maximumGap,
             displayUnderruns, board::capturedAudioUnderruns());
    for (size_t i = 0; i < 12; ++i) {
        ESP_LOGI(tag, "slot sample=%u selected_fb=%u sequence=%" PRIu32 " us=%" PRId64,
                 static_cast<unsigned>(i), samples[i].index, samples[i].sequence, samples[i].us);
    }
}

uint16_t* allocateWorld() {
    constexpr size_t bytes = 400 * 225 * sizeof(uint16_t);
    constexpr size_t reserveInternal = 96 * 1024;
    if (heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA) >= bytes + reserveInternal) {
        if (auto* p = static_cast<uint16_t*>(heap_caps_aligned_alloc(64, bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA))) {
            return p;
        }
    }
    return static_cast<uint16_t*>(heap_caps_aligned_alloc(
        64, bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
}

#if CONFIG_FOD_BOARD_DIAGNOSTIC

void tone(void*, int16_t* stereo, size_t frames) {
    static uint32_t cursor = 0;
    for (size_t i = 0; i < frames; ++i) {
        const int32_t level = (cursor % 24000 < 1200) ? ((cursor % 60 < 30) ? 1800 : -1800) : 0;
        ++cursor;
        stereo[2*i] = stereo[2*i+1] = level;
    }
}

void diagnostics() {
    board::setMixer(tone, nullptr);
    board::setPresenter();
#if CONFIG_FOD_DIAGNOSTIC_SCANOUT_ONLY
    constexpr bool stationary = true;
#else
    constexpr bool stationary = false;
#endif
    auto* source = stationary ? nullptr : allocateWorld();
    if (!stationary && !source) {
        ESP_LOGE(tag, "diagnostic render allocation failed");
        return;
    }
    auto* capture = static_cast<board::SlotSample*>(
        heap_caps_malloc(benchmarkSamples * sizeof(board::SlotSample), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    if (capture) board::captureSlots(capture, benchmarkSamples);
    uint32_t sequence = 0, previousControls = 0;
    int width = 400;
    int touchX = -1, touchY = -1;
    while (true) {
        if (!board::waitForSlot(100)) continue;
        auto input = board::controllerAt(board::timeUs());
        board::readTouch(input);
        if (input.held != previousControls) {
            ESP_LOGI(tag, "diagnostic pad buttons=0x%" PRIx32 " move=(%.2f,%.2f) look=(%.3f,%.3f)", input.held,
                     input.moveX, input.moveY, input.lookX, input.lookY);
            previousControls = input.held;
        }
        if (input.touchX >= 0) {
            touchX = input.touchX;
            touchY = input.touchY;
            ESP_LOGI(tag, "diagnostic touch x=%d y=%d", touchX, touchY);
        }
        if (input.edge(fod::Swap)) width = (width == 400) ? 320 : 400;
        if (capture && board::capturedSlots() == benchmarkSamples) {
            reportCapture(capture, stationary);
            heap_caps_free(capture);
            capture = nullptr;
        }
        if (stationary && sequence > 0) continue;
        auto frame = board::acquireFrame();
        if (!frame) continue;
        ++sequence;
        if (source) {
            const int height = width * 9 / 16;
            for (int y = 0; y < height; ++y) {
                auto* row = source + y * width;
                for (int x = 0; x < width; ++x) {
                    row[x] = ((x < width/2) != (bool(sequence & 1))) ? ((y < height/2) ? 0x8400 : 0x001f)
                                                                 : ((y < height/2) ? 0x07e0 : 0x780f);
                }
            }
            const auto err = board::scaleWorld(frame, source, width, height);
            if (err != ESP_OK) {
                reportError("diagnostic PPA", err);
                board::discard(frame);
                continue;
            }
            board::clearMargins(frame);
        } else {
            board::rect(frame, 0, 0, 800, 480, 0x2947);
        }
        char label[64];
        std::snprintf(label, sizeof(label), "FRAME %lu  FB %d  WIDTH %d",
                      static_cast<unsigned long>(sequence), frame.index, width);
        board::text(frame, 195, 220, label, 0xffff);
        board::text(frame, 4, 2, "1", 0xffff, 2);
        board::text(frame, 783, 2, "2", 0xffff, 2);
        board::text(frame, 4, 463, "3", 0xffff, 2);
        board::text(frame, 783, 463, "4", 0xffff, 2);
        if (source) {
            const char moving[]{static_cast<char>('0' + (sequence % 10)), 0};
            board::text(frame, 111, static_cast<int>((sequence * 3) % 480) - 7, moving, 0xffff, 2);
        }
        if (touchX >= 0) board::rect(frame, touchX - 5, touchY - 5, 11, 11, 0xffff);
        const auto err = board::present(frame, sequence);
        if (err != ESP_OK) board::discard(frame);
    }
}

#else
struct Published {
    fod::Snapshot snapshot;
    fod::MenuState menu;
    bool marker = false;
    uint32_t matchRevision = 0;
};

#if CONFIG_FOD_REPLAY_FRAME_ONLY
constexpr bool frameOnly = true;
#else
constexpr bool frameOnly = false;
#endif

struct StageSample {
#if CONFIG_FOD_REPLAY_FRAME_ONLY
    uint32_t sequence, frame;
#else
    uint32_t sequence, slot, setup, raster, world, ppa, hud, submit;
#endif
};

// Arena switches restart the board and build the requested arena before any task touches it.
enum Launch : uint8_t { LaunchDefault = 0, LaunchCampaign = 1, LaunchBriarMatch = 2, LaunchShipMatch = 3, LaunchContinue = 4 };

const char* launchTitle(uint8_t launch) {
    if (launch == LaunchContinue) return "CONTINUING OPERATION BRIAR";
    return launch == LaunchCampaign ? "MISSION 1: BRIAR HOUSE" : launch == LaunchBriarMatch ? "BRIAR HOUSE" : "SHIP BOX";
}

struct Live {
    explicit Live(const fod::Assets& assets, fod::App& app) : assets(assets), app(app) {}
    const fod::Assets& assets;
    fod::App& app;
    std::atomic<uint8_t> loading{0}; // Set just before a restart; the render task shows the loading card
    Published latest{};
    portMUX_TYPE lock = portMUX_INITIALIZER_UNLOCKED;
    board::SlotSample* capture = nullptr;
    StageSample* stages = nullptr;
    bool benchmark = false;
    std::atomic<uint32_t> simOverruns{0}, simWorstUs{0};
};

board::SavedSettings fromMenu(const fod::MenuState& menu) {
    board::SavedSettings saved{
        .difficulty = menu.difficulty,
        .fast = menu.compact,
        .voice = menu.voice,
        .volume = static_cast<uint8_t>(menu.volume),
        .opponent = static_cast<uint8_t>(menu.opponent),
    };
    static_assert(sizeof(saved.callsign) >= std::tuple_size_v<decltype(menu.callsign)>);
    std::copy(menu.callsign.begin(), menu.callsign.end(), saved.callsign);
    return saved;
}

void toMenu(const board::SavedSettings& saved, fod::MenuState& menu) {
    menu.difficulty = saved.difficulty;
    menu.compact = saved.fast;
    menu.voice = saved.voice;
    menu.volume = saved.volume;
    menu.opponent = static_cast<fod::Opponent>(saved.opponent);
    std::copy_n(saved.callsign, menu.callsign.size() - 1, menu.callsign.begin());
    menu.callsign.back() = '\0';
}

bool sameSettings(const board::SavedSettings& x, const board::SavedSettings& y) {
    return x.difficulty == y.difficulty && x.fast == y.fast && x.voice == y.voice && x.volume == y.volume &&
           x.opponent == y.opponent && std::strcmp(x.callsign, y.callsign) == 0;
}

void publish(Live& live, bool marker) {
    Published current{live.app.state(), live.app.settings(), marker};
    portENTER_CRITICAL(&live.lock);
    current.matchRevision = live.latest.matchRevision + (current.snapshot.match != live.latest.snapshot.match);
    live.latest = current;
    portEXIT_CRITICAL(&live.lock);
}

void showLoading(const char* title) {
    auto frame = board::acquireFrame();
    if (!frame) return;
    static uint32_t sequence = 0x90000000U;
    fod::drawLoading(title, frame.pixels, true);
    if (board::present(frame, ++sequence) != ESP_OK) board::discard(frame);
}

void showRecovery(const char* message) {
    auto frame = board::acquireFrame();
    if (!frame) return;
    static uint32_t sequence = 0x80000000U;
    fod::drawRecovery(message, frame.pixels, true);
    if (board::present(frame, ++sequence) != ESP_OK) board::discard(frame);
}

void waitForRetry() {
    while (true) {
        auto input = board::controllerAt(board::timeUs());
        board::readTouch(input);
        if (input.edge(fod::Confirm) || input.touchY >= 300) return;
        vTaskDelay(pdMS_TO_TICKS(16));
    }
}

void loadPack(fod::Assets& assets) {
    while (true) {
        std::string error;
        const auto begin = board::timeUs();
        heap_caps_monitor_local_minimum_free_size_start();
        const auto card = board::mountSd();
        try {
            if (card != ESP_OK) error = esp_err_to_name(card);
            else if (assets.load(board::packPath, error)) {
                ESP_LOGI(tag, "pack=%s stored=%llu resident_payload=%llu reported_peak_payload=%llu load_us=%" PRId64,
                         board::packPath, static_cast<unsigned long long>(assets.storedBytes),
                         static_cast<unsigned long long>(assets.residentBytes), static_cast<unsigned long long>(assets.peakBytes),
                         board::timeUs() - begin);
                char revision[41]{};
                for (size_t i = 0; i < assets.sourceRevision.size(); ++i) {
                    std::snprintf(revision + 2*i, 3, "%02x", assets.sourceRevision[i]);
                }
                ESP_LOGI(tag, "pack_revision=%s art_seed=%" PRIu32, revision, assets.artSeed);
                board::logMemory("loaded");
                heap_caps_monitor_local_minimum_free_size_stop();
                return;
            }
        } catch (const std::bad_alloc&) {
            error = "ASSET MEMORY EXHAUSTED";
        }
        heap_caps_monitor_local_minimum_free_size_stop();
        board::unmountSd();
        ESP_LOGW(tag, "load %s failed (%s), waiting for retry", board::packPath, error.c_str());
        showRecovery(error.c_str());
        waitForRetry();
    }
}

void exportCapture(const Live& live) {
    FILE* file = std::fopen("/sd/slots.csv", "w");
    if (!file) {
        ESP_LOGW(tag, "could not write /sd/slots.csv");
        return;
    }
    std::fputs("sample,time_us,selected_sequence,selected_buffer,display_underruns\n", file);
    for (size_t i = 0; i < benchmarkSamples; ++i) {
        const auto& sample = live.capture[i];
        std::fprintf(file, "%u,%lld,%lu,%u,%u\n", static_cast<unsigned>(i),
                     static_cast<long long>(sample.us), static_cast<unsigned long>(sample.sequence),
                     sample.index, sample.underruns);
    }
    if (std::fclose(file) != 0) ESP_LOGW(tag, "incomplete slots.csv");
    file = std::fopen("/sd/stages.csv", "w");
    if (!file) {
        ESP_LOGW(tag, "could not write /sd/stages.csv");
        return;
    }
    std::fputs(frameOnly ? "sequence,frame_us\n"
                         : "sequence,request_slot,setup_us,raster_us,world_us,ppa_us,hud_us,submit_us\n", file);
    // Sequence zero has no submitted frame and is also the ring's initial value.
    const uint32_t first = std::max(live.capture[0].sequence, uint32_t{1});
    const uint32_t last = live.capture[benchmarkSlots].sequence;
    for (uint32_t sequence = first; sequence <= last; ++sequence) {
        const auto& stage = live.stages[sequence % benchmarkSamples];
        if (stage.sequence != sequence) continue;
#if CONFIG_FOD_REPLAY_FRAME_ONLY
        std::fprintf(file, "%" PRIu32 ",%" PRIu32 "\n", stage.sequence, stage.frame);
#else
        std::fprintf(file, "%" PRIu32 ",%" PRIu32 ",%" PRIu32 ",%" PRIu32 ",%" PRIu32
                     ",%" PRIu32 ",%" PRIu32 ",%" PRIu32 "\n",
                     stage.sequence, stage.slot, stage.setup, stage.raster, stage.world, stage.ppa,
                     stage.hud, stage.submit);
#endif
    }
    if (std::fclose(file) != 0) ESP_LOGW(tag, "incomplete stages.csv");
}

bool publishedCompact(Live& live) {
    portENTER_CRITICAL(&live.lock);
    const bool compact = live.latest.menu.compact;
    portEXIT_CRITICAL(&live.lock);
    return compact;
}

void renderTask(void* context) {
    auto& live = *static_cast<Live*>(context);

    std::unique_ptr<fod::Renderer> renderer;
#if CONFIG_FOD_CLASSIC_VISUALS
    constexpr auto look = fod::RenderLook::Classic;
#else
    constexpr auto look = fod::RenderLook::Enhanced;
#endif
    try {
        // Construct Renderer first so its heavily-accessed depth buffer gets priority for on-chip SRAM.
        renderer = std::make_unique<fod::Renderer>(live.assets, look);
    } catch (const std::bad_alloc&) {
        showRecovery("RENDER MEMORY");
        vTaskDelete(nullptr);
        return;
    }
    auto* source = allocateWorld();
    if (!source) {
        renderer.reset();
        showRecovery("RENDER MEMORY");
        vTaskDelete(nullptr);
        return;
    }
    ESP_LOGI(tag, "renderer working bytes=%llu color bytes=%u color_in_sram=%d depth_in_sram=%d",
             static_cast<unsigned long long>(renderer->workingBytes()),
             400 * 225 * static_cast<unsigned>(sizeof(uint16_t)),
             esp_ptr_internal(source) ? 1 : 0, renderer->depthInInternalRam() ? 1 : 0);
    fod::WorldFrame worldFrame(*renderer, source);
    std::optional<fod::MenuFrameKey> lastMenu;
    board::logMemory("renderer-ready");
    board::setPresenter();
    uint32_t sequence = 0;
    bool reportReady = live.benchmark && board::captureSlots(live.capture, benchmarkSamples);
    while (true) {
        if (!board::waitForSlot(100)) continue;
        if (const auto launch = live.loading.load()) {
            auto frame = board::acquireFrame();
            if (!frame) continue;
            fod::drawLoading(launchTitle(launch), frame.pixels, true);
            if (board::present(frame, ++sequence) != ESP_OK) board::discard(frame);
            continue;
        }
        if (reportReady && board::capturedSlots() >= benchmarkSamples) {
            reportReady = false;
            ESP_LOGI(tag, "replay scene=%s compact=%d sim_overruns=%" PRIu32 " worst_sim_us=%" PRIu32
                          " input_overflows=%" PRIu32 " audio_ready=%d"
                          " render_stack_min_bytes=%u",
                     CONFIG_FOD_REPLAY_SCENE, publishedCompact(live), live.simOverruns.load(), live.simWorstUs.load(),
                     board::reportOverflows(), board::audioReady(),
                     static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr)));
            reportCapture(live.capture, false);
            board::logMemory("replay-end");
            exportCapture(live);
        }
        portENTER_CRITICAL(&live.lock);
        const auto published = live.latest;
        portEXIT_CRITICAL(&live.lock);
        const bool menu = !live.benchmark && published.snapshot.match != fod::Match::Playing;
        const auto speaker = live.app.audio.speaking();
        const bool controllerPrompt = menu && !board::hasController();
        const bool marker = CONFIG_FOD_LATENCY_GPIO >= 0 && published.marker;
        std::optional<fod::MenuFrameKey> menuKey;
        if (menu) {
            menuKey.emplace(published.matchRevision, published.snapshot.match, published.menu, speaker, controllerPrompt, marker);
            if (menuKey == lastMenu) continue;
        }
        auto frame = board::acquireFrame();
        if (!frame) continue;
        const int width = published.menu.width();
        const int height = published.menu.height();
        const bool detail = reportReady && !frameOnly;
        const int64_t start = reportReady ? board::timeUs() : 0;
        if (live.benchmark) renderer->render(published.snapshot, width, height, source, detail);
        else worldFrame.render(published.snapshot, published.matchRevision, width, height);
        [[maybe_unused]] const int64_t world = detail ? board::timeUs() : 0;
        const auto err = board::scaleWorld(frame, source, width, height);
        [[maybe_unused]] const int64_t ppa = detail ? board::timeUs() : 0;
        if (err != ESP_OK) {
            reportError("PPA", err);
            board::discard(frame);
            continue;
        }
        fod::drawHud(live.assets, published.snapshot, published.menu, frame.pixels, true, speaker);
        if (controllerPrompt) {
            board::text(frame, 10, 460, "CONNECT DUALSHOCK 4 TO PLAY", 0xffff, 2);
        }
#if CONFIG_FOD_LATENCY_GPIO >= 0
        board::rect(frame, 390, 230, 20, 20, published.marker ? 0xffff : 0);
#endif
        [[maybe_unused]] const int64_t hud = detail ? board::timeUs() : 0;
        ++sequence;
        [[maybe_unused]] const auto slot = detail ? board::displayStatus().slots : 0;
        if (board::present(frame, sequence) != ESP_OK) {
            board::discard(frame);
            continue;
        }
        lastMenu = menuKey;
        if (reportReady) {
            const int64_t submitted = board::timeUs();
            auto& stage = live.stages[sequence % benchmarkSamples];
#if CONFIG_FOD_REPLAY_FRAME_ONLY
            stage = {sequence, static_cast<uint32_t>(submitted - start)};
#else
            const auto& timing = renderer->stats();
            stage = {sequence, slot, timing.setupUs, timing.rasterUs, static_cast<uint32_t>(world - start),
                     static_cast<uint32_t>(ppa - world), static_cast<uint32_t>(hud - ppa),
                     static_cast<uint32_t>(submitted - hud)};
#endif
        }
    }
}

void timerWake(void* handle) {
    xTaskNotifyGive(static_cast<TaskHandle_t>(handle));
}

void mixApp(void* context, int16_t* pcm, size_t frames) {
    static_cast<fod::App*>(context)->audio.mix(pcm, frames);
}

void runSimulation(Live& live, fod::ReplayTrace* replay) {
    esp_timer_handle_t timer;
    const esp_timer_create_args_t config{.callback = timerWake, .arg = xTaskGetCurrentTaskHandle(), .name = "fod-tick"};
    if (esp_timer_create(&config, &timer) != ESP_OK) {
        showRecovery("TICK TIMER FAILED");
        return;
    }
    vTaskPrioritySet(nullptr, 7);
    auto& app = live.app;
    int64_t origin = board::timeUs();
    uint64_t tick = 1;
    int64_t nextSave = 0;
    auto saved = fromMenu(app.settings());
    for (;;) {
        int64_t due = origin + tick * 1'000'000 / 60;
        const auto delay = due - board::timeUs();
        if (delay > 0) {
            if (esp_timer_start_once(timer, delay) == ESP_OK) ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
            else vTaskDelay(1);
        }
        unsigned catchUp = 0;
        int64_t now = board::timeUs();
        while (now >= due && catchUp < 4) {
            const int64_t start = now;
            auto input = board::controllerAt(due);
            const auto previous = app.state().match;
            if (replay) {
                if (!replay->step(app)) {
                    replay->begin(app);
                    replay->step(app);
                }
            } else {
                if (previous != fod::Match::Playing || (tick & 1) == 0) board::readTouch(input);
                app.tick(input);
            }
            publish(live, input.down(fod::Fire));
            if (!replay && app.settings().pendingLaunch != fod::LaunchAction::None) {
                // The menu picked an arena that is not loaded: remember it and restart into it.
                const auto action = app.settings().pendingLaunch;
                app.settings().pendingLaunch = fod::LaunchAction::None;
                uint8_t launch = LaunchDefault;
                if (action == fod::LaunchAction::Campaign) launch = LaunchCampaign;
                else if (action == fod::LaunchAction::ContinueCampaign) launch = LaunchContinue;
                else if (action == fod::LaunchAction::BotMatch) {
                    launch = app.settings().selectedArena == fod::Arena::BriarHouse ? LaunchBriarMatch : LaunchShipMatch;
                }
                if (launch != LaunchDefault) {
                    auto err = board::writeSettings(fromMenu(app.settings()));
                    if (err == ESP_OK && launch == LaunchContinue && app.settings().saveFailed &&
                        !board::writeCampaign(app.settings().progress)) err = ESP_FAIL;
                    if (err == ESP_OK) err = board::requestLaunch(launch);
                    if (err == ESP_OK) {
                        ESP_LOGI(tag, "restarting into %s", launchTitle(launch));
                        live.loading.store(launch);
                        vTaskDelay(pdMS_TO_TICKS(150));
                        esp_restart();
                    }
                    reportError("Launch request", err);
                }
            }
            now = board::timeUs();
            const auto tickUs = static_cast<uint32_t>(now - start);
            if (tickUs > live.simWorstUs.load()) live.simWorstUs.store(tickUs);
            ++catchUp;
            if (!replay && previous != app.state().match) {
                origin = now;
                tick = 1;
                if (app.state().match == fod::Match::Results) board::logMemory("match-results");
            } else {
                ++tick;
            }
            due = origin + tick * 1'000'000 / 60;
        }
        if (now >= due) {
            ++live.simOverruns;
            origin = now;
            tick = 1;
        }
        if (!live.benchmark && app.state().match != fod::Match::Playing && now >= nextSave) {
            nextSave = now + 1'000'000;
            const auto settings = fromMenu(app.settings());
            if (!sameSettings(saved, settings)) {
                const auto err = board::writeSettings(settings);
                if (err == ESP_OK) saved = settings;
                else reportError("Settings save", err);
            }
        }
    }
}

void gameplay() {
    fod::Assets assets;
    loadPack(assets);
    const char* name = CONFIG_FOD_REPLAY_SCENE;
    fod::ReplayTrace trace;
    fod::ReplayTrace* replay = nullptr;
    if (*name) {
        if (std::strlen(name) > 48 ||
            std::strspn(name, "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_-") != std::strlen(name)) {
            showRecovery("INVALID REPLAY NAME");
            return;
        }
        std::string error, path = std::string("/sd/") + name + ".fodtrace";
        if (!trace.load(path.c_str(), error, assets.extent)) {
            ESP_LOGE(tag, "cannot load %s (%s)", path.c_str(), error.c_str());
            showRecovery(error.c_str());
            return;
        }
        replay = &trace;
    }
    const uint8_t launch = replay ? uint8_t(replay->campaign() ? LaunchCampaign : LaunchDefault) : board::takeLaunch();
    const bool campaign = launch == LaunchCampaign || launch == LaunchContinue;
    if (campaign || launch == LaunchBriarMatch) {
        showLoading(launchTitle(launch));
        const auto begin = board::timeUs();
        try {
            fod::buildBriarHouse(assets, campaign ? fod::BriarHouseMode::Invasion
                                                                  : fod::BriarHouseMode::Standard);
        } catch (const std::bad_alloc&) {
            // The request was already cleared, so a retry boots straight back into Ship Box.
            showRecovery("ARENA MEMORY EXHAUSTED");
            waitForRetry();
            esp_restart();
        }
        ESP_LOGI(tag, "arena=%s build_us=%" PRId64 " resident_payload=%llu", launchTitle(launch), board::timeUs() - begin,
                 static_cast<unsigned long long>(assets.residentBytes));
        board::logMemory("arena-built");
        if (campaign) {
            std::string error;
            const auto voiceStart = board::timeUs();
            if (assets.loadCampaignVoices("/sd/campaign-voices.fodv", error)) {
                ESP_LOGI(tag, "%s campaign_voices_bytes=%" PRIu32 " load_us=%" PRId64,
                         fod::campaignVoiceBuildMarker, assets.campaignVoices->codeBytes, board::timeUs() - voiceStart);
                board::logMemory("campaign-voices-loaded");
            } else {
                ESP_LOGW(tag, "campaign voices unavailable (%s); using subtitles", error.c_str());
            }
        }
    } else {
        try {
            fod::optimizeShipBoxWorld(assets);
        } catch (const std::bad_alloc&) {
            ESP_LOGW(tag, "Ship Box mesh simplification ran out of memory");
        }
    }
    fod::App app(assets, replay ? 0u : esp_random());
    toMenu(board::readSettings(), app.settings());
    if (!replay) {
        app.settings().progress = board::readCampaign();
        app.setProgressWriter(board::writeCampaign);
        if (launch == LaunchContinue && app.settings().progress.available())
            app.queueLoadedLaunch(fod::LaunchAction::ContinueCampaign);
        else if (launch == LaunchCampaign) app.queueLoadedLaunch(fod::LaunchAction::Campaign);
        else if (launch == LaunchShipMatch || launch == LaunchBriarMatch) app.queueLoadedLaunch(fod::LaunchAction::BotMatch);
    }
    if (replay) {
#if CONFIG_FOD_REPLAY_COMPACT
        app.settings().compact = true;
#else
        app.settings().compact = false;
#endif
        replay->begin(app);
#if CONFIG_FOD_REPLAY_JOURNAL
        FILE* journal = std::fopen("/sd/native.jsonl", "w");
        if (!journal) {
            showRecovery("JOURNAL WRITE FAILED");
            return;
        }
        while (!std::ferror(journal) && replay->step(app)) fod::writeTraceTick(journal, app);
        const bool writeFailed = std::ferror(journal) != 0;
        const int closeResult = std::fclose(journal);
        const bool saved = !writeFailed && closeResult == 0;
        ESP_LOGI(tag, "replay journal input_ticks=%u unused_draws=%u missing_draws=%" PRIu32 " write_ok=%d close=%d",
                 static_cast<unsigned>(replay->length()), static_cast<unsigned>(replay->unusedDraws()),
                 replay->missingDraws(), !writeFailed, closeResult);
        showRecovery(saved ? "JOURNAL WRITTEN TO SD" : "JOURNAL WRITE FAILED");
        return;
#endif
    }
    Live live(assets, app);
    if (replay) {
        live.capture = static_cast<board::SlotSample*>(
            heap_caps_malloc(benchmarkSamples * sizeof(board::SlotSample), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
        live.stages = static_cast<StageSample*>(heap_caps_calloc(benchmarkSamples, sizeof(StageSample), MALLOC_CAP_SPIRAM));
        if (!live.capture || !live.stages) {
            heap_caps_free(live.capture);
            heap_caps_free(live.stages);
            showRecovery("CAPTURE MEMORY FAILED");
            return;
        }
        live.benchmark = true;
    }
    publish(live, false);
    if (xTaskCreatePinnedToCore(renderTask, "fod-raster", 16384, &live, 5, nullptr, 1) != pdPASS) {
        showRecovery("RENDER TASK FAILED");
        return;
    }
    board::setMixer(mixApp, &app);
    runSimulation(live, replay);
    // Keep the task-owned state alive if the simulation could not initialize.
    vTaskSuspend(nullptr);
}


#endif
} // namespace

extern "C" void app_main() {
    board::logProfile();
    board::logMemory("boot");
    const auto panel = board::initDisplay();
    if (panel != ESP_OK) {
        reportError("MIPI display", panel);
        return;
    }
    const auto i2c = board::initI2c();
    reportError("shared I2C", i2c);
    if (i2c == ESP_OK) {
        reportError("touch", board::initTouch());
        reportError("audio", board::startAudio());
    }
    reportError("USB host", board::startGamepad());
    board::initializeSettings();
    board::logMemory("drivers");
#if CONFIG_FOD_BOARD_DIAGNOSTIC
    reportError("SDMMC", board::mountSd());
    diagnostics();
#else
    gameplay();
#endif
}

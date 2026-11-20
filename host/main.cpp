#include "app.hpp"
#include "assets.hpp"
#include "briar_house.hpp"
#include "render.hpp"
#include "trace.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <optional>

namespace {
using namespace fod;
using Clock = std::chrono::steady_clock;

struct Options {
    const char *pack = "assets/fall-of-duty.fodp", *trace = nullptr, *journal = nullptr, *image = nullptr;
    int frames = 3000, warmup = 300, imageTick = 0;
    int width = 0;
    int duelWidth = 0;
    std::optional<float> stickSensitivity;
    Arena arena = Arena::ShipBox;
    BriarHouseMode briarMode = BriarHouseMode::Standard;
    RenderLook look = RenderLook::Enhanced;
    bool benchmark = false, inspect = false, both = false, localDuel = false;
};

void help() {
    std::puts("fod_host [--pack PATH] [--map ship-box|briar-house] [--mission briar-house] [--mode 400|320|both] [--look enhanced|classic]\n"
              "         [--trace PATH [--journal PATH] | --bench PATH [--frames 3000] [--warmup 300]]\n"
              "         [--image PATH [--image-tick TICK]] [--inspect]\n"
#ifdef FOD_DESKTOP
              "         [--mode 800|1280] for desktop solo\n"
              "         [--local-duel [--duel-resolution 400|640|800]]\n"
              "         [--stick-sensitivity PERCENT] for desktop gamepad aim (1-200)\n"
#endif
              "Without headless options, launch an offline duel. Ship Box is the default map.");
}

bool options(int argc, char** argv, Options& out) {
    for (int i = 1; i < argc; ++i) {
        const char* option = argv[i];
        if (!std::strcmp(option, "--inspect")) {
            out.inspect = true;
            continue;
        }
#ifdef FOD_DESKTOP
        if (!std::strcmp(option, "--local-duel")) {
            out.localDuel = true;
            continue;
        }
#endif
        if (i + 1 == argc) return false;
        const char* value = argv[++i];
        if (!std::strcmp(option, "--pack")) out.pack = value;
        else if (!std::strcmp(option, "--map")) {
            if (!std::strcmp(value, "ship-box")) { out.arena = Arena::ShipBox; out.briarMode = BriarHouseMode::Standard; }
            else if (!std::strcmp(value, "briar-house")) { out.arena = Arena::BriarHouse; out.briarMode = BriarHouseMode::Standard; }
            else return false;
        } else if (!std::strcmp(option, "--mission")) {
            if (!std::strcmp(value, "briar-house")) { out.arena = Arena::BriarHouse; out.briarMode = BriarHouseMode::Invasion; }
            else return false;
        }
        else if (!std::strcmp(option, "--trace")) out.trace = value;
        else if (!std::strcmp(option, "--bench")) { out.benchmark = true; out.trace = value; }
        else if (!std::strcmp(option, "--journal")) out.journal = value;
        else if (!std::strcmp(option, "--image")) out.image = value;
#ifdef FOD_DESKTOP
        else if (!std::strcmp(option, "--duel-resolution")) {
            if (!std::strcmp(value, "400")) out.duelWidth = 400;
            else if (!std::strcmp(value, "640")) out.duelWidth = 640;
            else if (!std::strcmp(value, "800")) out.duelWidth = 800;
            else return false;
        } else if (!std::strcmp(option, "--stick-sensitivity")) {
            char* end = nullptr;
            const long percent = std::strtol(value, &end, 10);
            if (*end || percent < 1 || percent > 200) return false;
            out.stickSensitivity = float(percent) / 100;
        }
#endif
        else if (!std::strcmp(option, "--look")) {
            if (!std::strcmp(value, "classic")) out.look = RenderLook::Classic;
            else if (!std::strcmp(value, "enhanced")) out.look = RenderLook::Enhanced;
            else return false;
        } else if (!std::strcmp(option, "--mode")) {
            if (!std::strcmp(value, "320")) out.width = 320;
            else if (!std::strcmp(value, "both")) { out.both = true; out.width = 400; }
            else if (!std::strcmp(value, "400")) out.width = 400;
#ifdef FOD_DESKTOP
            else if (!std::strcmp(value, "800")) out.width = 800;
            else if (!std::strcmp(value, "1280")) out.width = desktopHdWidth;
#endif
            else return false;
        } else if (!std::strcmp(option, "--frames") || !std::strcmp(option, "--warmup") || !std::strcmp(option, "--image-tick")) {
            char* end = nullptr;
            const long n = std::strtol(value, &end, 10);
            if (!end || *end || n < 0 || n > 200000) return false;
            if (!std::strcmp(option, "--frames")) out.frames = int(n);
            else if (!std::strcmp(option, "--warmup")) out.warmup = int(n);
            else out.imageTick = int(n);
        } else return false;
    }
    return (!out.duelWidth || out.localDuel) &&
           (!out.stickSensitivity || (!out.inspect && !out.trace && !out.image)) &&
           (!out.localDuel || (!out.inspect && !out.trace && !out.image && (out.width == 0 || out.width == 400))) &&
           (out.width <= 400 || (!out.inspect && !out.trace && !out.image)) &&
           (!out.benchmark || (out.frames && !out.journal)) &&
           (!out.journal || out.trace) && (!out.imageTick || (out.trace && out.image && !out.benchmark)) && (!out.both || (out.benchmark && !out.image));
}

void imageFile(const char* path, const std::vector<uint16_t>& colors) {
    FILE* out = std::fopen(path, "wb");
    if (!out) { std::perror(path); std::exit(1); }
    std::fputs("P6\n800 480\n255\n", out);
    std::array<uint8_t, 2400> row{};
    for (size_t y = 0; y < 480; ++y) {
        for (size_t x = 0; x < 800; ++x) {
            const uint16_t c = colors[y * 800 + x];
            row[x * 3] = uint8_t((c >> 11) * 255 / 31);
            row[x * 3 + 1] = uint8_t(((c >> 5) & 63) * 255 / 63);
            row[x * 3 + 2] = uint8_t((c & 31) * 255 / 31);
        }
        if (std::fwrite(row.data(), 1, row.size(), out) != row.size()) { std::perror(path); std::exit(1); }
    }
    if (std::fclose(out)) { std::perror(path); std::exit(1); }
}

void describeAssets(const Assets& assets) {
    size_t voiceClips = 0, voiceBytes = 0, voicePcmBytes = 0;
    for (const auto& bank : assets.voices) {
        voiceClips += bank.size();
        for (const auto& clip : bank) {
            voiceBytes += clip.codes.size();
            voicePcmBytes += size_t(clip.frames) * sizeof(int16_t);
        }
    }
    std::printf("{\"sdBytes\":%llu,\"residentAssetBytes\":%llu,\"peakAssetLoadBytes\":%llu,"
                "\"worldTriangles\":%zu,\"shotTriangles\":%zu,\"obstacles\":%zu,\"navNodes\":%zu,"
                "\"rivalFrames\":%zu,\"textures\":%zu,\"audioClips\":%zu,\"compressedVoiceClips\":%zu,"
                "\"residentVoiceCodesBytes\":%zu,\"voicePcmEquivalentBytes\":%zu,\"sourceRevision\":\"",
                static_cast<unsigned long long>(assets.storedBytes), static_cast<unsigned long long>(assets.residentBytes),
                static_cast<unsigned long long>(assets.peakBytes), assets.world.triangles.size(),
                assets.shotSurfaces.size(), assets.obstacles.size(), assets.navNodes.size(), assets.rivalFrames.size(),
                assets.textures.size(), assets.audio.size(), voiceClips, voiceBytes, voicePcmBytes);
    for (const auto byte : assets.sourceRevision) std::printf("%02x", unsigned(byte));
    std::printf("\",\"artSeed\":%u}\n", unsigned(assets.artSeed));
}

using Samples = std::array<std::vector<uint32_t>, 6>;

void percentiles(const char* name, std::vector<uint32_t>& samples, bool last) {
    std::sort(samples.begin(), samples.end());
    const auto percentile = [&](size_t p) { return samples[(samples.size() * p + 99) / 100 - 1]; };
    std::printf("\"%s\":{\"p50\":%u,\"p95\":%u,\"p99\":%u,\"max\":%u}%s",
                name, percentile(50), percentile(95), percentile(99), samples.back(), last ? "" : ",");
}

uint32_t duration(Clock::time_point start, Clock::time_point end) {
    return uint32_t(std::chrono::duration_cast<std::chrono::microseconds>(end - start).count());
}

int headless(const Assets& assets, ReplayTrace* trace, const Options& out, int width) {
    App app(assets);
    auto& menu = app.settings();
    menu.compact = width == 320;
    const int height = menu.height();
    const bool renderFrames = out.benchmark || out.image;
    std::optional<Renderer> render;
    if (renderFrames) render.emplace(assets, out.look);
    std::vector<uint16_t> color(renderFrames ? width * height : 0);
    std::vector<uint16_t> framebuffer(renderFrames ? 800 * 480 : 0);
    std::array<int16_t, 2 * audioHz / 30> pcm{};
    if (trace) trace->begin(app);
    const int frames = out.benchmark ? out.frames + out.warmup : trace ? int((trace->length() + 1) / 2) : 1;
    Samples timings;
    for (auto& channel : timings) channel.reserve(out.benchmark ? out.frames : 0);
    FILE* journal = out.journal ? std::fopen(out.journal, "w") : nullptr;
    if (out.journal && !journal) { std::perror(out.journal); return 1; }
    uint32_t maxTriangles = 0, maxChunks = 0, maxPixels = 0;
    Snapshot imageState{};
    Speaker imageSpeaker = Speaker::None;
    bool imageCaptured = false;

    for (int frame = 0; frame < frames; ++frame) {
        const auto start = Clock::now();
        bool capture = false;
        for (int i = 0; trace && i < 2; ++i) {
            if (!trace->step(app)) {
                if (!out.benchmark) break;
                trace->begin(app);
                trace->step(app);
            }
            if (journal) writeTraceTick(journal, app);
            if (!imageCaptured && out.imageTick && int(app.state().tick) == out.imageTick) {
                imageState = app.state();
                imageCaptured = capture = true;
            }
        }
        const auto simulated = Clock::now();
        app.audio.mix(pcm.data(), pcm.size() / 2);
        const Speaker speaker = app.audio.speaking();
        if (capture) imageSpeaker = speaker;
        const auto mixed = Clock::now();
        if (render && (out.benchmark || frame == frames - 1)) {
            const auto& state = imageCaptured ? imageState : app.state();
            render->render(state, width, height, color.data(), out.benchmark);
            const auto rendered = Clock::now();
            // Benchmarks keep the plain scaler. A capture shows the filtered picture the handheld presents.
            scaleWorld(color.data(), width, height, framebuffer.data(), false, !out.benchmark && out.look == RenderLook::Enhanced);
            drawHud(assets, state, menu, framebuffer.data(), false, imageCaptured ? imageSpeaker : speaker);
            const auto presented = Clock::now();
            if (out.benchmark && frame >= out.warmup) {
                const auto& stats = render->stats();
                const uint32_t values[]{duration(start, simulated), duration(simulated, mixed),
                                        stats.setupUs, stats.rasterUs, duration(rendered, presented), duration(start, presented)};
                for (size_t i = 0; i < timings.size(); ++i) timings[i].push_back(values[i]);
                maxTriangles = std::max(maxTriangles, stats.triangles);
                maxChunks = std::max(maxChunks, stats.visibleChunks);
                maxPixels = std::max(maxPixels, stats.depthPasses);
            }
        }
    }
    if (journal) {
        const bool writeFailed = std::ferror(journal) != 0;
        if (std::fclose(journal) != 0 || writeFailed) {
            std::fprintf(stderr, "%s: could not finish writing the replay journal\n", out.journal);
            return 1;
        }
    }
    if (out.imageTick && !imageCaptured) {
        std::fputs("The replay did not reach the requested image tick.\n", stderr);
        return 2;
    }
    if (out.image) imageFile(out.image, framebuffer);
    if (out.benchmark) {
        std::printf("{\"platform\":\"desktop\",\"width\":%d,\"height\":%d,\"frames\":%d,\"warmupFrames\":%d,"
                    "\"rendererWorkingBytes\":%llu,\"colorAndPresentationBytes\":%zu,\"audioQueueDrops\":%u,"
                    "\"maxTriangles\":%u,\"maxChunks\":%u,\"maxDepthPasses\":%u,\"microseconds\":{",
                    width, height, out.frames, out.warmup, static_cast<unsigned long long>(render->workingBytes()),
                    (color.capacity() + framebuffer.capacity()) * sizeof(uint16_t), app.audio.dropped(), maxTriangles, maxChunks, maxPixels);
        const char* names[]{"simulationTwoTicks", "audio800StereoSamples", "setup", "raster", "cpuScaleAndHud", "total"};
        for (size_t i = 0; i < timings.size(); ++i) percentiles(names[i], timings[i], i + 1 == timings.size());
        std::puts("}}");
    } else if (trace) {
        const auto& state = app.state();
        std::printf("{\"ticks\":%u,\"inputTicks\":%zu,\"clip\":%d,\"reserve\":%d,\"score\":[%d,%d],"
                    "\"missingDraws\":%u,\"unusedDraws\":%zu}\n",
                    unsigned(state.tick), trace->length(), state.clip, state.reserve, state.playerScore, state.rivalScore,
                    trace->missingDraws(), trace->unusedDraws());
    }
    return 0;
}
} // namespace

#ifdef FOD_DESKTOP
std::optional<fod::LaunchAction> runDesktop(const fod::Assets& assets, int width, fod::RenderLook look,
                                           std::optional<float> stickSensitivity, bool autoStart, fod::MenuState* carry);
int runLocalDuel(const fod::Assets& assets, fod::RenderLook look, int width, std::optional<float> stickSensitivity);
#endif

int main(int argc, char** argv) {
    if (argc == 2 && !std::strcmp(argv[1], "--help")) { help(); return 0; }
    Options out;
    if (!options(argc, argv, out)) { help(); return 2; }
    fod::Assets assets;
    std::string error;
    if (!assets.load(out.pack, error)) {
        std::fprintf(stderr, "%s: %s\n", out.pack, error.c_str());
        return 1;
    }
    if (out.arena == Arena::BriarHouse) buildBriarHouse(assets, out.briarMode);
    fod::ReplayTrace trace;
    if (out.trace && !trace.load(out.trace, error, assets.extent)) {
        std::fprintf(stderr, "%s: %s\n", out.trace, error.c_str());
        return 1;
    }
    if (trace.campaign()) buildBriarHouse(assets, BriarHouseMode::Invasion);
    if (out.inspect) describeAssets(assets);
    if (out.trace || out.image) {
        if (out.benchmark || out.image) fod::optimizeShipBoxWorld(assets);
        const auto run = headless(assets, out.trace ? &trace : nullptr, out, out.width ? out.width : 400);
        return run || !out.both ? run : headless(assets, &trace, out, 320);
    }
    if (out.inspect) return 0;
#ifdef FOD_DESKTOP
    fod::optimizeShipBoxWorld(assets);
    if (out.localDuel) return runLocalDuel(assets, out.look, out.duelWidth ? out.duelWidth : 640, out.stickSensitivity);
#ifdef __APPLE__
    constexpr int soloDefaultWidth = fod::desktopHdWidth;
#else
    constexpr int soloDefaultWidth = 400;
#endif
    fod::MenuState carry{};
    carry.desktopRenderWidth = out.width ? out.width : soloDefaultWidth;
    carry.selectedArena = out.arena;
    bool autoStart = false;
    for (;;) {
        const auto launch = runDesktop(assets, carry.desktopRenderWidth, out.look, out.stickSensitivity, autoStart, &carry);
        if (!launch) return 1;
        const auto action = *launch;
        if (action == fod::LaunchAction::None) return 0;
        const bool campaign = action == fod::LaunchAction::Campaign;
        const auto arena = campaign ? fod::Arena::BriarHouse : carry.selectedArena;
        if (assets.arena != arena) {
            assets = fod::Assets{};
            if (!assets.load(out.pack, error)) {
                std::fprintf(stderr, "%s: %s\n", out.pack, error.c_str());
                return 1;
            }
        }
        if (arena == fod::Arena::BriarHouse) {
            buildBriarHouse(assets, campaign ? fod::BriarHouseMode::Invasion : fod::BriarHouseMode::Standard);
        } else {
            fod::optimizeShipBoxWorld(assets);
        }
        if (action == fod::LaunchAction::LocalDuel) {
            return runLocalDuel(assets, out.look, out.duelWidth ? out.duelWidth : 640, out.stickSensitivity);
        }
        autoStart = true;
    }
#else
    std::fputs("Build with FOD_DESKTOP=ON for the interactive SDL2 host.\n", stderr);
    return 2;
#endif
}

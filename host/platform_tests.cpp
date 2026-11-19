#include "app.hpp"
#include "assets.hpp"
#include "render.hpp"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <vector>

namespace {
using namespace fod;

void check(bool truth, const char* message) {
    if (truth) return;
    std::fprintf(stderr, "%s\n", message);
    std::exit(1);
}

void checkOrientation(const Assets& assets) {
    for (const int width : {400, 320}) {
        const int height = width * 9 / 16;
        std::vector<uint16_t> image(width * height);
        for (size_t i = 0; i < image.size(); ++i) image[i] = uint16_t(i * 53);
        std::vector<uint16_t> landscape(800 * 480), native(480 * 800);
        scaleWorld(image.data(), width, height, landscape.data(), false);
        scaleWorld(image.data(), width, height, native.data(), true);
        App app(assets);
        drawHud(assets, app.state(), app.settings(), landscape.data(), false);
        drawHud(assets, app.state(), app.settings(), native.data(), true);
        for (size_t x = 0; x < 800; ++x) {
            for (size_t y = 0; y < 480; ++y) {
                check(landscape[y * 800 + x] == native[x * 480 + 479 - y], "Rotated HUD or viewport pixel mismatch.");
            }
        }
    }
}

void checkStartGradient(const Assets& assets) {
    App app(assets);
    std::vector<uint16_t> frame(800 * 480, 0xf800);
    drawHud(assets, app.state(), app.settings(), frame.data(), false);
    const auto row = frame.data() + 450 * 800;
    for (int x = 261; x <= 554; ++x) {
        check((row[x] >> 11) >= (row[x - 1] >> 11) && (row[x] & 31) <= (row[x - 1] & 31),
              "The title fade must blend each color channel independently.");
    }
}

void checkTouchAndReconnect(const Assets& assets) {
    App app(assets);
    Input input;
    input.touchX = startX + 16;
    input.touchY = setupY + 16;
    app.tick(input);
    check(app.settings().setup && app.state().match == Match::Start, "Touch Duel Setup should open settings.");
    input.touchX = 620;
    input.touchY = menuY + RenderRow * menuRowHeight + 2;
    app.tick(input);
    check(app.settings().compact, "Touch should select the compact display setting.");
    input.touchY = menuY + CallsignRow * menuRowHeight + 2;
    app.tick(input);
    check(app.settings().editing, "Touch should open the callsign editor.");
    input.touchX = keyboardX + 2;
    input.touchY = keyboardY + 2;
    app.tick(input);
    check(app.settings().callsign[0] == 'A', "Touch should update the callsign.");
    input.touchY = 385;
    app.tick(input);
    check(!app.settings().editing, "Touch Done should save the callsign.");
    input.touchY = menuY + 4;
    app.tick(input);
    check(!app.settings().setup && app.state().match == Match::Start, "Back to Start should close settings.");
    app.settings().voice = false;
    input.touchX = startX + 16;
    input.touchY = startY + 16;
    app.tick(input);
    check(app.state().match == Match::Playing, "Touch should start the live duel.");
    check(app.state().alternateRival, "A new installation should start a duel against Hanz.");
    std::array<int16_t, 64> silence{};
    app.audio.mix(silence.data(), silence.size() / 2);
    check(std::all_of(silence.begin(), silence.end(), [](int16_t sample) { return sample == 0; }),
          "The live duel must begin without a weather loop when rival voice is disabled.");
    input = {};
    app.tick(input);
    input.disconnected = true;
    app.tick(input);
    check(app.state().match == Match::Paused, "A controller disconnect should pause the duel.");
    input = {};
    input.held = Fire;
    app.tick(input);
    check(app.state().match == Match::Paused, "A reconnect should require explicit resume.");
    input.pressed = Confirm;
    app.tick(input);
    check(app.state().match == Match::Playing, "Confirm should resume.");
    input.pressed = Fire;
    app.tick(input);
    check(app.state().clip == 30, "A held trigger should stay blocked after reconnect.");
    app.tick({});
    app.tick(input);
    check(app.state().clip == 29, "A new trigger press should fire after release.");
}

void checkRadar() {
    Assets assets;
    MenuState menu;
    Snapshot snap;
    snap.match = Match::Playing;
    snap.playerYaw = 0;
    snap.rival.position = {0, 0, 10};
    std::vector<uint16_t> frame(800 * 480);
    const auto draw = [&] {
        std::fill(frame.begin(), frame.end(), 0);
        drawHud(assets, snap, menu, frame.data(), false);
    };
    constexpr auto red = uint16_t((243 >> 3) << 11 | (96 >> 2) << 5 | (68 >> 3));
    constexpr int midX = 68, midY = 81, offset = 24;
    draw();
    const auto idleRim = frame[28 * 800 + 15];
    check(frame[(midY - offset) * 800 + midX] != red, "Silent hostiles must be absent from passive radar.");
    snap.radarContact = true;
    draw();
    check(frame[(midY - offset) * 800 + midX] == red, "A forward contact must appear above the player.");
    snap.playerYaw = pi / 2;
    draw();
    check(frame[midY * 800 + midX - offset] == red, "A right turn must move the same contact to the left.");
    snap.uavSeconds = 30;
    draw();
    check(frame[28 * 800 + 15] != idleRim, "UAV recon must have a distinct radar rim.");
    check(frame[midY * 800 + midX - offset - 3] == red, "UAV contacts must be larger than passive contacts.");
    const auto scan = frame;
    snap.scaledTime = 0.7;
    draw();
    check(frame != scan, "UAV sweep and contact pulse must advance with game time.");
    snap.rival.position = {0, 0, 33};
    draw();
    check(std::find(frame.begin(), frame.end(), red) == frame.end(), "Out-of-range contacts must not stick to the radar edge.");
}

void checkResumeLatch(const Assets& assets) {
    App app(assets);
    const auto buttons = [&app](uint32_t held, uint32_t pressed) {
        Input input;
        input.held = held;
        input.pressed = pressed;
        app.tick(input);
    };
    buttons(Jump | Confirm, Jump | Confirm);
    buttons(Jump, Fire);
    check(app.state().clip == 29 && app.state().grounded, "Confirm must suppress the held jump and allow a fresh trigger tap.");
    buttons(0, Pause);
    buttons(0, Confirm);
    for (int i = 0; i < 8; ++i) app.tick({});
    buttons(0, Pause);
    buttons(Reload, Confirm);
    buttons(Reload, Fire);
    check(app.state().clip == 28, "A held reload button must allow a fresh trigger tap immediately after resume.");
}

void checkTranslucentPass() {
    Assets assets;
    assets.materials = {{0, -1, 255, true}, {65535, -1, 8, true}};
    assets.world.vertices = {
        {{-.05f, -.05f, 3}, 0, 0}, {{.05f, -.05f, 3}, 0, 0}, {{0, .05f, 3}, 0, 0},
        {{-.2f, -.2f, 4}, 0, 0}, {{.2f, -.2f, 4}, 0, 0}, {{0, .2f, 4}, 0, 0},
    };
    constexpr Triangle glass{0, 1, 2, 1};
    constexpr Triangle back{3, 4, 5, 0};
    Snapshot snap;
    snap.match = Match::Playing;
    snap.camera = {{}, 0, 0, pi / 3};
    snap.rival.visible = false;
    snap.device = Device::Laptop;
    std::vector<uint16_t> first(320 * 180), second(320 * 180);
    assets.world.triangles.assign(1025, glass);
    assets.world.triangles.push_back(back);
    Renderer glassFirst(assets);
    glassFirst.render(snap, 320, 180, first.data());
    std::rotate(assets.world.triangles.begin(), assets.world.triangles.end() - 1, assets.world.triangles.end());
    Renderer glassLast(assets);
    glassLast.render(snap, 320, 180, second.data());
    check(first == second, "Every translucent primitive must wait for opaque depth.");
}

void checkDepthPrecision() {
    Assets assets;
    assets.materials = {{0x07e0, -1, 255, true}, {0xf800, -1, 255, true}};
    assets.world.vertices = {
        {{-1, -1, 30.02f}, 0, 0}, {{1, -1, 30.02f}, 0, 0}, {{0, 1, 30.02f}, 0, 0},
        {{-1, -1, 30}, 0, 0}, {{1, -1, 30}, 0, 0}, {{0, 1, 30}, 0, 0},
    };
    assets.world.triangles = {{0, 1, 2, 0}, {3, 4, 5, 1}};
    Snapshot snap;
    snap.match = Match::Playing;
    snap.camera = {{}, 0, 0, pi / 3};
    snap.rival.visible = false;
    snap.device = Device::Laptop;
    std::vector<uint16_t> image(320 * 180);
    Renderer classic(assets, RenderLook::Classic);
    classic.render(snap, 320, 180, image.data());
    constexpr size_t center = 90 * 320 + 160;
    check(image[center] & 0x07e0, "The classic buffer should reproduce the distant depth tie.");
    Renderer enhanced(assets);
    enhanced.render(snap, 320, 180, image.data());
    check((image[center] & 0xf800) && !(image[center] & 0x07e0),
          "Enhanced depth should resolve two-centimeter separation at 30 meters.");
}

void checkProjectionCache() {
    Assets assets;
    assets.materials = {{0xf800}, {0x07e0}};
    // All three world indices share one cache slot. The first vertex crosses the near plane.
    assets.world.vertices.resize(257);
    assets.world.vertices[0] = {{-.05f, -.06f, .07f}, 0, 0};
    assets.world.vertices[128] = {{.4f, -.2f, 1}, 0, 0};
    assets.world.vertices[256] = {{0, .4f, 1}, 0, 0};
    assets.world.triangles = {{0, 128, 256, 0}};
    assets.apache.vertices.resize(257);
    assets.apache.vertices[256] = {{0, .2f, 0}, 0, 0};
    assets.apache.vertices[0] = {{-.2f, -.2f, 0}, 0, 0};
    assets.apache.vertices[1] = {{.2f, -.2f, 0}, 0, 0};
    assets.apache.triangles = {{256, 0, 1, 1}};

    Snapshot snap;
    snap.match = Match::Playing;
    snap.camera = {{}, 0, 0, pi / 3};
    snap.device = Device::Laptop;
    Renderer renderer(assets);
    std::vector<uint16_t> image(320 * 180);
    constexpr size_t center = 90 * 320 + 160;
    renderer.render(snap, 320, 180, image.data());
    check((image[center] & 0xf800) && !(image[center] & 0x07ff), "Cache collisions must preserve near-plane clipping.");
    snap.camera.position.x = 1;
    snap.apacheActive = true;
    snap.apachePosition = {1, 0, 2};
    renderer.render(snap, 320, 180, image.data());
    check((image[center] & 0x07e0) && !(image[center] & 0xf81f), "Projections must follow the current camera and mesh.");
}

void checkPlayerUniform() {
    Assets assets;
    assets.materials = {{0xf800, -1, 255, true}, {0x001f, -1, 255, true}};
    assets.playerUniform = {1, 1};
    auto& body = assets.rivalFrames.emplace_back();
    body.vertices = {{{-0.3f, 0.8f, 0}, 0, 0}, {{0.3f, 0.8f, 0}, 0, 0}, {{0, 1.4f, 0}, 0, 0}};
    body.triangles = {{0, 1, 2, 0}};
    Snapshot snap;
    snap.match = Match::Playing;
    snap.camera = {{0, 1, 0}, 0, 0, pi / 3};
    snap.rival.position = snap.player.position = {0, 0, 3};
    snap.rival.visible = true;
    snap.player.visible = true;
    snap.device = Device::Laptop;
    Renderer renderer(assets);
    std::vector<uint16_t> image(320 * 180);
    constexpr size_t center = 90 * 320 + 160;
    renderer.render(snap, 320, 180, image.data());
    check((image[center] & 0xf800) && !(image[center] & 0x001f), "A rival must use the rival uniform.");
    snap.rival.visible = false;
    snap.dead = true;
    renderer.render(snap, 320, 180, image.data());
    check((image[center] & 0x001f) && !(image[center] & 0xf800), "The death camera must use the player uniform.");

    assets.playerUniform.clear();
    assets.materials.push_back({0x07e0, -1, 255, true});
    assets.hanzFrames.push_back(body);
    assets.hanzFrames.front().triangles.front().material = 2;
    assets.playerBody = body;
    assets.playerBody.triangles.front().material = 1;
    Renderer variants(assets);
    snap.dead = false;
    snap.rival.visible = true;
    snap.alternateRival = true;
    variants.render(snap, 320, 180, image.data());
    check((image[center] & 0x07e0) && !(image[center] & 0xf81f), "Selecting Hanz must draw his own rig.");
    snap.rival.visible = false;
    snap.dead = true;
    variants.render(snap, 320, 180, image.data());
    check((image[center] & 0x001f) && !(image[center] & 0xffe0), "The death camera must draw the separate player rig.");
}

void checkMixer(const Assets& assets) {
    Mixer mixer(assets);
    std::array<int16_t, 1600> buffer{};
    mixer.loop(Sound::Rotor, 0.5f, -1.0f);
    mixer.mix(buffer.data(), buffer.size() / 2);
    bool leftAudible = false;
    for (size_t i = 0; i < buffer.size(); i += 2) {
        leftAudible |= buffer[i] != 0;
        check(buffer[i + 1] == 0, "Hard-left panning must leave the right channel silent.");
    }
    check(leftAudible, "The packed rotor loop should produce mixer output.");
    mixer.stop(Sound::Rotor);
    mixer.mix(buffer.data(), buffer.size() / 2);
    check(std::all_of(buffer.begin(), buffer.end(), [](int16_t sample) { return sample == 0; }),
          "A stopped rotor loop should leave both channels silent.");
    check(mixer.dropped() == 0, "Short mixer test overflowed the command queue.");

    Assets fixture;
    fixture.audio.resize(size_t(Sound::Count));
    fixture.audio[size_t(Sound::Rotor)].samples = {2000, 500};
    fixture.audio[size_t(Sound::Pistol)].samples = {32767, -32768};
    Mixer exact(fixture);
    exact.setVolume(100);
    exact.loop(Sound::Rotor, 1.0f, -1.0f);
    exact.play(Sound::Pistol);
    std::array<int16_t, 8> combined{};
    exact.mix(combined.data(), 1);
    exact.mix(combined.data() + 2, 3);
    const std::array<int16_t, 8> expected{32767, 32767, -32268, -32768, 2000, 0, 500, 0};
    check(combined == expected, "Mixer must preserve clipping, sample order, loop wrap and one-shot retirement.");
}

void checkSpeakerHud() {
    Assets assets;
    MenuState menu;
    Snapshot snap;
    snap.match = Match::Playing;
    std::vector<uint16_t> frame(800 * 480);
    const auto draw = [&](Speaker speaker) {
        std::fill(frame.begin(), frame.end(), 0);
        drawHud(assets, snap, menu, frame.data(), false, speaker);
    };
    draw(Speaker::None);
    check(!frame[145 * 800 + 22], "The speaker icon must be hidden during silence.");
    draw(Speaker::Hanz);
    check(frame[145 * 800 + 22] && frame[141 * 800 + 41] && !frame[141 * 800 + 100],
          "Show Hanz's speaker icon and short gamertag just below radar.");
    snap.uavSeconds = 18;
    draw(Speaker::IPwndUrMom);
    check(frame[162 * 800 + 22] && frame[138 * 800 + 15] != frame[155 * 800 + 15],
          "Show the speaker beneath the active UAV countdown.");
    check(frame[158 * 800 + 100], "The callout must fit IPwndUrMom's full gamertag.");
    menu.voice = false;
    draw(Speaker::Hanz);
    check(!frame[162 * 800 + 22], "Muted voice must hide the speaker icon.");
}

void checkSpeech() {
    Assets assets;
    assets.voices[0].push_back({{0x71}, 3, 1000, 0});
    assets.voices[4] = {{{}, 1, 4000, 0}, {{}, 1, 5000, 0}, {{}, 1, 6000, 0}};
    Mixer mixer(assets);
    mixer.setVolume(100);
    std::array<int16_t, 6> output{};
    mixer.play(Sound::VoiceStart);
    check(mixer.speaking() == Speaker::None, "Queued speech must not claim the speaker icon.");
    mixer.mix(output.data(), 1);
    check(output[0] == 898 && output[1] == 898, "Speech should play the stored predictor at 90% gain.");
    check(mixer.speaking() == Speaker::IPwndUrMom, "The first line should identify its actual voice bank.");
    mixer.pauseSpeech(true);
    mixer.mix(output.data(), 1);
    check(output[0] == 0 && output[1] == 0, "Paused speech should leave both channels silent.");
    check(mixer.speaking() == Speaker::None, "Paused speech must hide the speaker icon.");
    mixer.play(Sound::VoiceAlternateKill);
    mixer.play(Sound::VoiceStart);
    mixer.pauseSpeech(false);
    mixer.mix(output.data(), 3);
    check(output[0] == 899 && output[1] == 899 && output[2] == 909 && output[3] == 909,
          "Resumed speech must decode the next two nibbles in order.");
    const auto first = output[4];
    check(first >= 3593 && first <= 5390 && output[5] == first, "Queued speech should start after the active line.");
    check(mixer.speaking() == Speaker::Hanz, "Queued speech should claim the icon only when it plays.");
    mixer.mix(output.data(), 1);
    check(output[0] == 0 && output[1] == 0, "Only one additional line should queue.");
    check(mixer.speaking() == Speaker::None, "Silence after a line must clear the icon.");

    std::array<int16_t, 3> heard{first, 0, 0};
    for (size_t i = 1; i < heard.size(); ++i) {
        mixer.play(Sound::VoiceAlternateKill);
        mixer.mix(output.data(), 1);
        heard[i] = output[0];
    }
    check(heard[0] != heard[1] && heard[1] != heard[2] && heard[0] != heard[2],
          "Every variant should play once per shuffle.");
    mixer.play(Sound::VoiceAlternateKill);
    mixer.mix(output.data(), 1);
    check(output[0] != heard.back(), "A new shuffle should avoid repeating the last line.");
    mixer.play(Sound::VoiceStart);
    mixer.stopSpeech();
    mixer.mix(output.data(), 3);
    check(std::all_of(output.begin(), output.end(), [](int16_t sample) { return sample == 0; }),
          "Muting must clear decoded and queued speech.");
    mixer.setVolume(0);
    mixer.play(Sound::VoiceStart);
    mixer.mix(output.data(), 3);
    check(mixer.speaking() == Speaker::None, "Zero master volume must leave the speaker icon hidden.");
}
} // namespace

int main(int argc, char** argv) {
    check(argc == 2, "Usage: fod_platform_tests PACK");
    Assets assets;
    std::string error;
    if (!assets.load(argv[1], error)) {
        std::fprintf(stderr, "%s\n", error.c_str());
        return 1;
    }
    checkOrientation(assets);
    checkRadar();
    checkStartGradient(assets);
    checkTouchAndReconnect(assets);
    checkResumeLatch(assets);
    checkTranslucentPass();
    checkDepthPrecision();
    checkProjectionCache();
    checkPlayerUniform();
    checkMixer(assets);
    checkSpeakerHud();
    checkSpeech();
    std::puts("Viewport mapping, menus, radar, trigger latches, translucency, PCM mixing and ADPCM speech passed.");
}

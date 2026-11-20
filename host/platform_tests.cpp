#include "app.hpp"
#include "assets.hpp"
#include "light.hpp"
#include "render.hpp"
#include "trace.hpp"

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
        for (const bool smooth : {false, true}) {
            std::vector<uint16_t> landscape(800 * 480), native(480 * 800);
            scaleWorld(image.data(), width, height, landscape.data(), false, smooth);
            scaleWorld(image.data(), width, height, native.data(), true, smooth);
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
    // Smooth scaling must leave an even picture unchanged from edge to edge.
    std::vector<uint16_t> even(400 * 225, 0x8410), frame(800 * 480);
    scaleWorld(even.data(), 400, 225, frame.data(), false, true);
    for (size_t y = 15; y < 465; ++y) {
        for (size_t x = 0; x < 800; ++x) check(frame[y * 800 + x] == 0x8410, "Smooth scaling changed an even picture.");
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

void checkFrozenWorld(const Assets& assets) {
    Renderer renderer(assets), reference(assets);
    std::vector<uint16_t> pixels(400 * 225), expected(pixels.size());
    WorldFrame frame(renderer, pixels.data());
    Snapshot snapshot;
    uint32_t matchRevision = 0;
    int width = 400, height = 225;
    renderer.render(snapshot, width, height, pixels.data());
    check(renderer.stats().depthPasses, "Measured rendering should record pixel work before returning to live mode.");
    const auto draw = [&](bool rebuild) {
        check(frame.render(snapshot, matchRevision, width, height) == rebuild, "Wrong world cache invalidation.");
        reference.render(snapshot, width, height, expected.data());
        check(pixels == expected, "World cache changed the rendered pixels.");
        check(!renderer.stats().depthPasses && !renderer.stats().triangles && !renderer.stats().visibleChunks &&
              !renderer.stats().rasterUs && !renderer.stats().setupUs, "Live rendering collected frame statistics.");
        check(reference.stats().triangles && reference.stats().depthPasses, "Measured rendering lost its work counters.");
    };

    draw(true);
    draw(false);
    snapshot.match = Match::Playing;
    snapshot.camera = {{-13.8f, 1.7f, -13.8f}, 0.60f, 0, 59 * pi / 180};
    ++matchRevision;
    draw(true);
    draw(true);
    snapshot.match = Match::Paused;
    ++matchRevision;
    draw(true);
    draw(false);

    // Resume, move the camera, and pause again before the next presentation.
    matchRevision += 2;
    snapshot.camera.yaw += 0.25f;
    draw(true);
    draw(false);
    ++snapshot.difficulty;
    draw(true);
    width = 320;
    height = 180;
    draw(true);
    draw(false);
    snapshot.match = Match::Results;
    ++matchRevision;
    draw(true);
    draw(false);
    snapshot.match = Match::Start;
    ++matchRevision;
    draw(true);
}

void checkComposedMenu() {
    MenuState menu;
    const MenuFrameKey idle(1, Match::Start, menu, Speaker::None);
    check(idle == MenuFrameKey(1, Match::Start, menu, Speaker::Hanz), "Hidden speech should not redraw the menu.");
    check(!(idle == MenuFrameKey(2, Match::Paused, menu, Speaker::None)), "A new match state should redraw.");
    check(!(idle == MenuFrameKey(1, Match::Start, menu, Speaker::None, true)), "Controller prompt should redraw.");
    check(!(idle == MenuFrameKey(1, Match::Start, menu, Speaker::None, false, true)), "Latency marker should redraw.");
    menu.compact = true;
    check(!(idle == MenuFrameKey(1, Match::Start, menu, Speaker::None)), "Changing resolution should redraw.");
    const MenuFrameKey results(2, Match::Results, menu, Speaker::None);
    check(!(results == MenuFrameKey(2, Match::Results, menu, Speaker::Hanz)), "Visible speech should redraw.");
    menu.selection = RivalRow;
    check(!(results == MenuFrameKey(2, Match::Results, menu, Speaker::None)), "Moving menu selection should redraw.");
}

void checkTouchAndReconnect(const Assets& assets) {
    App app(assets);
    Input input;
    input.touchX = startX + 16;
    input.touchY = setupY + 16;
    app.tick(input);
    check(app.settings().setup && app.state().match == Match::Start, "Touch Match Setup should open settings.");
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
    check(app.state().difficulty == 5, "A new installation should start at difficulty level 5.");
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
    input.touchX = menuX + 5;
    input.touchY = menuY + 5;
    app.tick(input);
    check(app.state().match == Match::Playing, "A click should resume after the disconnect event clears.");
    input.touchX = input.touchY = -1;
    input.pressed = Fire;
    app.tick(input);
    check(app.state().clip == 30, "A held trigger should stay blocked after reconnect.");
    app.tick({});
    app.tick(input);
    check(app.state().clip == 29, "A new trigger press should fire after release.");
}

void checkFreeForAllMenu(const Assets& assets) {
    App app(assets);
    Input input;
    input.touchX = startX + 16;
    input.touchY = setupY + 16;
    app.tick(input);
    input.touchX = menuX + menuWidth - 8;
    input.touchY = menuY + RivalRow * menuRowHeight + 2;
    app.tick(input);
    check(app.settings().opponent == Opponent::FreeForAll, "Touch should select Free-for-all.");
    app.tick(input);
    check(app.settings().opponent == Opponent::IPwndUrMom, "Opponent selection should wrap after Free-for-all.");
    input.touchX = menuX + 8;
    app.tick(input);
    check(app.settings().opponent == Opponent::FreeForAll, "Opponent selection should wrap backwards.");
    input = {};
    input.pressed = Back;
    app.tick(input);
    input.pressed = Confirm;
    app.tick(input);
    check(app.state().match == Match::Playing && app.state().freeForAll, "Start should launch Free-for-all.");
    input.pressed = Pause;
    app.tick(input);
    input = {};
    input.touchX = menuX + menuWidth - 8;
    input.touchY = menuY + RivalRow * menuRowHeight + 2;
    app.tick(input);
    check(app.state().freeForAll && app.settings().opponent == Opponent::FreeForAll,
          "Pause should preserve the active lineup.");
    input.touchY = menuY + ExitRow * menuRowHeight + 2;
    app.tick(input);
    check(app.state().match == Match::Results, "End Match should show the Free-for-all results.");
    input.touchY = menuY + RivalRow * menuRowHeight + 2;
    app.tick(input);
    input.touchY = menuY + 2;
    app.tick(input);
    check(app.state().match == Match::Playing && !app.state().freeForAll && !app.state().alternateRival,
          "The results picker should allow a rematch in a different mode.");
}

void checkFreeForAllRender(const Assets& assets) {
    Renderer renderer(assets);
    Snapshot snap;
    snap.match = Match::Playing;
    snap.freeForAll = true;
    snap.camera = {{-14.3f, 1.7f, -12.5f}, 0, 0, 70 * pi / 180};
    snap.rival.position = {-14.65f, 0, -8.5f};
    snap.secondRival.position = {-13.6f, 0, -8.5f};
    snap.rival.yaw = snap.secondRival.yaw = pi;
    for (int width : {320, 400}) {
        const int height = width * 9 / 16;
        std::vector<uint16_t> two(width * height), one(width * height);
        snap.secondRival.visible = true;
        renderer.render(snap, width, height, two.data());
        snap.secondRival.visible = false;
        renderer.render(snap, width, height, one.data());
        check(two != one, "Free-for-all should render the second rival at both resolutions.");
    }

    std::vector<uint16_t> withoutContact(800 * 480), withContact(800 * 480);
    snap.player.position = {-14.3f, 0, -12.5f};
    snap.playerYaw = 0;
    snap.secondRival.visible = true;
    drawHud(assets, snap, {}, withoutContact.data(), false);
    snap.secondRadarContact = true;
    drawHud(assets, snap, {}, withContact.data(), false);
    check(withContact != withoutContact, "The second rival should have a distinct radar contact.");
}

void checkFreeForAllKillNotice(const Assets& assets) {
    Snapshot snap;
    snap.freeForAll = true;
    std::vector<uint16_t> without(800 * 480), with(without.size());
    const auto noticeDrawn = [&](Match match, int y) {
        snap.match = match;
        snap.killNotice = {};
        std::fill(without.begin(), without.end(), 0);
        std::fill(with.begin(), with.end(), 0);
        drawHud(assets, snap, {}, without.data(), false);
        snap.killNotice = {2.5f, 1, 0};
        drawHud(assets, snap, {}, with.data(), false);
        return !std::equal(without.begin() + y * 800, without.begin() + (y + 20) * 800, with.begin() + y * 800);
    };
    check(noticeDrawn(Match::Playing, 54), "Show the Free-for-all kill notice below the live scoreboard.");
    check(noticeDrawn(Match::Results, 412), "Keep the deciding kill visible on the results screen.");
    snap.device = Device::Laptop;
    check(!noticeDrawn(Match::Playing, 54), "Keep the laptop controls clear of the kill notice.");
}

void checkFreeForAllTrace(const Assets& assets, const char* path) {
    ReplayTrace trace;
    std::string error;
    check(trace.load(path, error), "The Free-for-all benchmark trace should load.");
    App app(assets);
    Renderer renderer(assets);
    for (int width : {320, 400}) {
        trace.begin(app);
        check(app.state().freeForAll, "The stress trace should launch Free-for-all.");
        const int height = width * 9 / 16;
        std::vector<uint16_t> full(width * height), hidden(full.size());
        bool overlap = false;
        while (trace.step(app)) {
            const auto& state = app.state();
            if (!state.apacheActive || state.apachePosition.x <= 18 || state.tick % 2 || state.dead ||
                state.rival.health <= 0 || state.secondRival.health <= 0) continue;
            renderer.render(state, width, height, full.data(), false);
            auto sample = state;
            sample.apacheActive = false;
            renderer.render(sample, width, height, hidden.data(), false);
            overlap = full != hidden;
            for (Actor Snapshot::*rival : {&Snapshot::rival, &Snapshot::secondRival}) {
                sample = state;
                (sample.*rival).visible = false;
                renderer.render(sample, width, height, hidden.data(), false);
                overlap &= full != hidden;
            }
            if (overlap) break;
        }
        check(overlap, "The stress trace should render both living rivals and the incoming Apache together.");
    }
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
    // Haze tints the surface at this distance, so compare the two channels.
    check((image[center] >> 11) > ((image[center] >> 5) & 63),
          "Enhanced depth should resolve two-centimeter separation at 30 meters.");
}

void checkBakedLight() {
    // A two-meter crate on a textured yard.
    Assets assets;
    assets.materials = {{0xffff, 0, 255, true}};
    assets.textures = {{2, 2, std::vector<uint16_t>(4, 0xffff)}};
    assets.navNodes = {{{-5, 0, -5}, 0, 0, false, true}, {{5, 0, 5}, 0, 0, false, true}};
    auto& world = assets.world;
    const auto quad = [&world](Vec3 a, Vec3 b, Vec3 c, Vec3 d) {
        const uint32_t first = uint32_t(world.vertices.size());
        world.vertices.insert(world.vertices.end(), {{a, 0, 0}, {b, 1, 0}, {c, 1, 1}, {d, 0, 1}});
        world.triangles.push_back({first, first + 1, first + 2, 0});
        world.triangles.push_back({first, first + 2, first + 3, 0});
    };
    quad({-10, 0, -10}, {10, 0, -10}, {10, 0, 10}, {-10, 0, 10});
    quad({1, 0, -1}, {1, 0, 1}, {1, 2, 1}, {1, 2, -1});
    quad({-1, 0, 1}, {-1, 0, -1}, {-1, 2, -1}, {-1, 2, 1});
    quad({1, 0, 1}, {-1, 0, 1}, {-1, 2, 1}, {1, 2, 1});
    quad({-1, 0, -1}, {1, 0, -1}, {1, 2, -1}, {-1, 2, -1});
    quad({-1, 2, -1}, {1, 2, -1}, {1, 2, 1}, {-1, 2, 1});

    SceneLight light;
    light.bake(assets);
    check(light.mapped() && light.patches.size() == 2 && !light.faces[0].back && !light.faces[1].back,
          "The yard should take its light from the ground map.");
    const unsigned open = light.at(6, -6), shaded = light.at(-1.7f, 0.8f), sunny = light.at(1.9f, -1.9f);
    check(open > 240, "Open ground should be fully lit.");
    check(shaded + 50 < open, "The crate should shade the ground on the side away from the sun.");
    check(sunny > shaded + 30, "Ground toward the sun should stay brighter than the crate's shadow.");
    const auto& wall = light.faces[2];
    check(wall.back && wall.front[0] < wall.front[2] && wall.front[1] < wall.front[2],
          "A wall should darken toward the ground.");

    // The same yard without navigation data has no map, and its faces still receive light.
    assets.navNodes.clear();
    SceneLight bare;
    bare.bake(assets);
    check(!bare.mapped() && bare.patches.empty() && bare.at(0, 0) == 255 && bare.faces[0].back,
          "A world without a yard should skip the ground map.");

    // Render the crate's shadow from above with both looks.
    assets.navNodes = {{{-5, 0, -5}, 0, 0, false, true}, {{5, 0, 5}, 0, 0, false, true}};
    Snapshot snap;
    snap.match = Match::Playing;
    snap.camera = {{0, 9, 0}, 0, pi / 2 - 0.001f, pi / 3};
    snap.rival.visible = false;
    snap.device = Device::Laptop;
    std::vector<uint16_t> image(320 * 180);
    const auto green = [&image](int x, int y) { return (image[y * 320 + x] >> 5) & 63; };
    Renderer enhanced(assets);
    enhanced.render(snap, 320, 180, image.data());
    // The view looks straight down with north at the top. The shadow falls to the upper left.
    check(green(130, 80) + 8 < green(260, 150), "The enhanced look should draw the crate's shadow.");
    Renderer classic(assets, RenderLook::Classic);
    classic.render(snap, 320, 180, image.data());
    check(green(130, 80) == green(260, 150), "The classic look should keep the yard evenly lit.");
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
    check(mixer.speaking() == Speaker::None, "A speech request must wait for playback to claim the speaker icon.");
    mixer.mix(output.data(), 1);
    check(output[0] == 898 && output[1] == 898, "Speech should play the stored predictor at 90% gain.");
    check(mixer.speaking() == Speaker::IPwndUrMom, "The first line should identify its actual voice bank.");
    mixer.pauseSpeech(true);
    mixer.mix(output.data(), 1);
    check(output[0] == 0 && output[1] == 0, "Paused speech should leave both channels silent.");
    check(mixer.speaking() == Speaker::None, "Paused speech must hide the speaker icon.");
    mixer.play(Sound::VoiceAlternateKill);
    mixer.mix(output.data(), 1);
    mixer.pauseSpeech(false);
    mixer.play(Sound::VoiceAlternateKill);
    mixer.mix(output.data(), 3);
    const std::array<int16_t, 6> resumed{899, 899, 909, 909, 0, 0};
    check(output == resumed, "Resumed speech should finish alone, with no other rival queued behind it.");
    mixer.play(Sound::VoiceAlternateKill);
    mixer.mix(output.data(), 1);
    check(output[0] == 0 && output[1] == 0 && mixer.speaking() == Speaker::None,
          "The quiet gap should suppress rival taunts and clear the speaker icon.");

    std::vector<int16_t> quiet(audioHz * 3);
    const auto clearGap = [&] {
        mixer.mix(quiet.data(), quiet.size() / 2);
        check(std::all_of(quiet.begin(), quiet.end(), [](int16_t sample) { return sample == 0; }),
              "Expired taunts must stay silent after the gap ends.");
    };
    clearGap();
    std::array<int16_t, 3> heard{};
    for (auto& line : heard) {
        mixer.play(Sound::VoiceAlternateKill);
        mixer.mix(output.data(), 1);
        line = output[0];
        check(line >= 3593 && line <= 5390 && mixer.speaking() == Speaker::Hanz,
              "A fresh taunt may play and claim the correct speaker after the gap.");
        clearGap();
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

void checkSpeechVariety() {
    Assets assets;
    assets.voices[0].push_back({{}, 1, 1000, 0});
    assets.voices[3].push_back({{}, 1, 5000, 0});
    bool heardFirst = false, heardSecond = false;
    size_t earliest = audioHz * 2, latest = 0;
    constexpr size_t block = audioHz / 200;
    std::array<int16_t, 2 * block> pcm{};
    for (uint32_t seed : {1u, 0x4323u, 0xbcd1u, 0x1e9f1c21u}) {
        Mixer mixer(assets, seed);
        mixer.setVolume(100);
        mixer.play(Sound::VoiceStart);
        mixer.play(Sound::VoiceAlternateStart);
        mixer.mix(pcm.data(), 1);
        check(pcm[0] == 898 || pcm[0] == 4492, "Simultaneous requests should play exactly one voice.");
        heardFirst |= mixer.speaking() == Speaker::IPwndUrMom;
        heardSecond |= mixer.speaking() == Speaker::Hanz;
        size_t elapsed = 0;
        while (elapsed < audioHz * 2) {
            mixer.play(Sound::VoiceStart);
            mixer.mix(pcm.data(), block);
            if (pcm[0]) break;
            elapsed += block;
        }
        earliest = std::min(earliest, elapsed);
        latest = std::max(latest, elapsed);
    }
    check(heardFirst && heardSecond, "Simultaneous requests should not always favor the first rival.");
    check(earliest >= audioHz * 3 / 4 && latest <= audioHz * 3 / 2 && earliest < latest,
          "Speech should leave a different quiet gap across seeds.");
}

void checkAudioControlsWithFullQueue() {
    Assets assets;
    assets.audio.resize(size_t(Sound::Count));
    assets.audio[size_t(Sound::Rotor)].samples = {1000, 1000};
    assets.audio[size_t(Sound::VoiceStart)].samples.assign(6, 4000);
    Mixer mixer(assets);
    std::array<int16_t, 2> pcm{};
    mixer.loop(Sound::Rotor, 1);
    mixer.play(Sound::VoiceStart);
    mixer.mix(pcm.data(), 1);
    check(pcm[0] != 0, "The rotor and voice should initially be audible.");
    const auto fillQueue = [&] {
        for (int i = 0; i < 140; ++i) mixer.loop(Sound::Rotor, 1);
    };
    fillQueue();
    mixer.stop(Sound::Rotor);
    mixer.pauseSpeech(true);
    mixer.mix(pcm.data(), 1);
    check(mixer.dropped() && pcm[0] == 0, "A full queue must still stop the rotor and pause speech.");
    mixer.pauseSpeech(false);
    mixer.mix(pcm.data(), 1);
    check(pcm[0] != 0 && mixer.speaking() == Speaker::IPwndUrMom, "Speech should resume with the rotor still stopped.");
    fillQueue();
    mixer.stopSpeech();
    mixer.stop(Sound::Rotor);
    mixer.mix(pcm.data(), 1);
    check(pcm[0] == 0 && mixer.speaking() == Speaker::None, "Muting must clear speech when the queue is full.");
}
} // namespace

int main(int argc, char** argv) {
    check(argc == 3, "Usage: fod_platform_tests PACK FFA_TRACE");
    Assets assets;
    std::string error;
    if (!assets.load(argv[1], error)) {
        std::fprintf(stderr, "%s\n", error.c_str());
        return 1;
    }
    checkOrientation(assets);
    checkRadar();
    checkStartGradient(assets);
    checkFrozenWorld(assets);
    checkComposedMenu();
    checkTouchAndReconnect(assets);
    checkFreeForAllMenu(assets);
    checkFreeForAllRender(assets);
    checkFreeForAllKillNotice(assets);
    checkFreeForAllTrace(assets, argv[2]);
    checkResumeLatch(assets);
    checkTranslucentPass();
    checkDepthPrecision();
    checkBakedLight();
    checkProjectionCache();
    checkPlayerUniform();
    checkMixer(assets);
    checkSpeakerHud();
    checkSpeech();
    checkSpeechVariety();
    checkAudioControlsWithFullQueue();
    std::puts("Viewport mapping, menus, radar, trigger latches, translucency, PCM mixing and ADPCM speech passed.");
}

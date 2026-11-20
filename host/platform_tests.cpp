#include "app.hpp"
#include "assets.hpp"
#include "briar_house.hpp"
#include "briar_mission_data.hpp"
#include "mission.hpp"
#include "mission_internal.hpp"
#include "game_bot.hpp"
#include "light.hpp"
#include "render.hpp"
#include "trace.hpp"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <initializer_list>
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

#ifdef FOD_LOCAL_DUEL
void checkDesktopResolution(const Assets& assets) {
    App app(assets);
    auto& menu = app.settings();
    menu.setup = true;
    menu.selection = RenderRow;
    menu.desktopRenderWidth = desktopHdWidth;
    const MenuFrameKey hd(1, Match::Start, menu, Speaker::None);

    Input input;
    input.pressed = Left;
    app.tick(input);
    check(menu.width() == 800 && menu.height() == 450, "Left should lower the desktop resolution.");
    check(!(hd == MenuFrameKey(1, Match::Start, menu, Speaker::None)), "Desktop resolution should invalidate the menu cache.");
    input.pressed = Right;
    app.tick(input);
    check(menu.width() == 1280 && menu.height() == 720, "Right should restore the HD desktop resolution.");
    app.tick(input);
    check(menu.width() == 320 && menu.height() == 180, "The desktop resolutions should wrap to the lowest setting.");

    constexpr uint16_t background = 0xffff;
    const int width = desktopHdWidth, canvasHeight = width * 3 / 5;
    std::vector<uint16_t> guarded(size_t(width) * canvasHeight + 2, background);
    auto* pixels = guarded.data() + 1;
    Snapshot playing;
    playing.match = Match::Playing;
    drawDesktopHud(assets, playing, menu, pixels, width);
    const int gutter = 15 * width / 800;
    const auto rightPixel = [&](int row) { return pixels[row * width + width - 1]; };
    check(rightPixel(gutter - 1) != background && rightPixel(canvasHeight - gutter) != background,
          "The HD HUD should fill both gutters to the right edge.");
    check(rightPixel(canvasHeight - gutter - 1) == background, "The lower gutter must preserve the last world row.");
    check(guarded.front() == background && guarded.back() == background, "The HD HUD must stay inside its canvas.");
}
#endif

void checkTouchAndReconnect(const Assets& assets) {
    App app(assets);
    Input input;
    input.touchX = startX + 16;
    input.touchY = startCard(StartItem::MatchSetup).y + 16;
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
    input.touchY = startCard(StartItem::BotMatch).y + 16;
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
    input.touchY = startCard(StartItem::MatchSetup).y + 16;
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

    // Verify Main Menu hub options: Campaign, Bot Match, Multiplayer LAN lobby, Arena toggle, and focus-loss preservation.
    App hub(assets);
    check(hub.settings().selection == int(StartItem::BotMatch), "Ship Box main menu must start focused on Bot Match.");
    Input lostFocus{};
    lostFocus.disconnected = true;
    hub.tick(lostFocus);
    hub.tick({});
    check(hub.settings().selection == int(StartItem::BotMatch),
          "Focus loss on the main menu must not reset the highlighted option.");
    hub.tick(Input{0, 0, 0, 0, Up, Up});
    check(hub.settings().selection == int(StartItem::Campaign), "Up from Bot Match must select Campaign.");
    hub.tick(Input{0, 0, 0, 0, Confirm, Confirm});
    check(hub.settings().pendingLaunch == LaunchAction::Campaign, "Selecting Campaign on Ship Box must request Campaign launch.");
    hub.settings().pendingLaunch = LaunchAction::None;
    hub.tick(Input{0, 0, 0, 0, Down, Down});
    hub.tick(Input{0, 0, 0, 0, Down, Down});
    check(hub.settings().selection == int(StartItem::MultiplayerLan), "Down twice from Campaign must select Multiplayer LAN.");
    hub.tick(Input{0, 0, 0, 0, Confirm, Confirm});
    check(hub.settings().lanMenu && hub.settings().selection == LanStartRow, "Multiplayer LAN must open the LAN lobby.");
    hub.tick(Input{0, 0, 0, 0, Down, Down});
    hub.tick(Input{0, 0, 0, 0, Right, Right});
    check(hub.settings().selectedArena == Arena::BriarHouse, "Right on LAN Arena row must toggle to Briar House.");
    hub.tick(Input{0, 0, 0, 0, Up, Up});
    hub.tick(Input{0, 0, 0, 0, Confirm, Confirm});
#ifdef FOD_LOCAL_DUEL
    check(hub.settings().pendingLaunch == LaunchAction::LocalDuel, "Start 2P Local Duel must request LocalDuel launch.");
#else
    check(hub.settings().pendingLaunch == LaunchAction::None && hub.settings().lanMenu,
          "Headless/handheld builds must not emit a desktop-only LocalDuel launch request.");
#endif
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

void checkBriarHouse(Assets& assets) {
    using namespace game_detail;
    buildBriarHouse(assets);
    const World world(assets);
    Navigation nav(assets);
    const auto point = [](float x, float y, float height = 0.f) {
        return Vec3{(x - 316) * .05f, height, (212.5f - y) * .05f};
    };
    const auto south = assets.spawns[0].position, north = assets.spawns[1].position;
    const int start = nav.nearest(south);
    std::vector<int> path;
    for (const auto& spawn : assets.spawns) {
        check(!world.walkBlocked(spawn.position, spawn.position), "Briar House spawn overlaps solid geometry.");
        check(nav.path(start, nav.nearest(spawn.position), path), "Briar House spawns must share a bot navigation network.");
    }
    for (const auto room : {point(188, 234), point(320, 248), point(548, 215), point(603, 281), point(20, 227)}) {
        check(nav.path(start, nav.nearest(room), path), "Bots must have access to each room and the west garden.");
        check(planar(nav.node(path.back()).position - room) < .6f, "The bot network must approach the room through its own doors.");
    }
    check(!world.walkBlocked(point(368, 406), point(368, 379)), "The front door must allow passage.");
    check(!world.walkBlocked(point(368, 14), point(368, 42)), "The rear door must allow passage.");
    check(world.walkBlocked(point(22, 250), point(86, 250)), "The kitchen side must connect outdoors without a side door.");
    check(world.walkBlocked(point(625, 495), point(625, 522)), "The driveway gate must keep the estate boundary closed.");
    check(!world.shot(point(625, 495, 1.7f), {0, 0, -1}, 0).hit, "The homes across the road must remain scenery.");
    for (const auto& [inside, outside] : {std::array{point(442, 495), point(442, 526)},
                                         std::array{point(329, -75), point(329, -111)},
                                         std::array{point(-27, 54), point(-76, 54)}}) {
        check(world.walkBlocked(inside, outside), "The iron railing must keep players inside the estate.");
        check(!world.blocked(atHeight(inside, 1.7f), atHeight(outside, 1.7f)),
              "The iron railing must leave an eye-level view of the street.");
    }
    for (size_t slot = 0; slot < 2; ++slot) {
        const auto& spawn = assets.spawns[slot];
        check(!world.walkBlocked(spawn.position, spawn.position + forward(spawn.yaw, 0) * 4.f),
              "Each lawn spawn must have a clear forward exit.");
        const float gate = slot == 0 ? 392.f : 28.f;
        check(world.blocked(point(310, gate, 1.7f), atHeight(spawn.position, 1.7f)) &&
              world.blocked(point(426, gate, 1.7f), atHeight(spawn.position, 1.7f)),
              "The spawn hedge must screen both sides of the nearby entrance.");
    }

    const auto traverse = [&](Vec3 position, std::initializer_list<Vec3> route) {
        for (const auto to : route) {
            if (world.walkBlocked(position, to)) {
                std::fprintf(stderr, "Blocked Briar House segment from %.2f, %.2f to %.2f, %.2f.\n",
                             position.x, position.z, to.x, to.z);
                check(false, "Each complete Briar House route must have player-width clearance.");
            }
            for (int i = 0; i < 200 && planar(position - to) > .04f; ++i) {
                const auto offset = to - position;
                Vec3 velocity = normalized(offset) * std::min(6.2f, planar(offset) / float(tickSeconds));
                world.move(position, velocity, float(tickSeconds), 1.85f, .4f);
            }
            check(planar(position - to) <= .04f, "Player collision must allow traversal of each complete route.");
        }
    };
    const auto front = point(368, 357), rear = point(368, 75);
    traverse(front, {point(188, 350), point(188, 75), rear});
    traverse(front, {point(549, 350), point(549, 75), rear});
    traverse(front, {point(322, 313), point(322, 112), rear});
    traverse(point(626, 460), {point(605, 358), point(603, 131), point(618, 108), point(618, 72)});
    traverse(point(626, 460), {point(603, 357), point(603, 280), point(547, 280), point(534, 252)});
    traverse(south, {point(207, 482), point(-26, 480), point(-26, 277), point(17, 270), point(17, 181),
                     point(-24, 162), point(-24, -14), point(228, -15), point(234, -67), point(367, -70), north});
    check(world.blocked(point(188, 157, 1.7f), point(329, 157, 1.7f)) &&
          world.blocked(point(419, 171, 1.7f), point(549, 171, 1.7f)),
          "The grand hall must not have a standing sightline into both wings.");
    check(!world.walkBlocked(point(207, 212), point(280, 212)) &&
          !world.walkBlocked(point(451, 252), point(535, 252)),
          "Each wing must have a crossover into the hall as well as the end-to-end route.");
    const auto northOfPlinth = point(368, 170, 1.7f), southOfPlinth = point(368, 254, 1.7f);
    check(world.blocked(point(368, 315, 1.7f), northOfPlinth) &&
          !world.blocked(point(279, 212, 1.7f), northOfPlinth) &&
          world.blocked(point(368, 107, 1.7f), southOfPlinth) &&
          !world.blocked(point(457, 252, 1.7f), southOfPlinth),
          "Each wing must open a side angle on a hall position shielded from the direct route.");

    const auto outside = point(25, 174, 1.7f);
    const Vec3 inward{1, 0, 0};
    const auto pane = world.shot(outside, inward, 0);
    check(pane.glass >= 0, "The garden window must catch the first shot.");
    check(!world.blocked(outside, point(85, 174, 1.7f)), "The window must allow visibility into the kitchen.");
    check(world.walkBlocked(point(25, 174), point(85, 174)), "Window openings must keep the walking routes at the doors.");

    Game game(assets);
    game.start(37, 5, Opponent::FreeForAll);
    check(game.state().freeForAll && planar(game.state().secondRival.position - assets.spawns[2].position) < .01f,
          "Briar House Free-for-all must use the map's third spawn.");
    game.start(37, 5);
    check(planar(game.state().player.position - south) < .01f && planar(game.state().rival.position - north) < .01f,
          "Briar House Solo must start at the sheltered lawn spawns.");
    Fixture fixture;
    fixture.player = point(447, 251);
    fixture.rival = north;
    fixture.yaw = 0;
    fixture.freezeRival = true;
    game.setupFixture(fixture);
    game.step(Input{0, 1, 0, 0, Sprint, Jump});
    for (int i = 0; i < 72; ++i) game.step(Input{0, 1, 0, 0, Sprint});
    check(game.state().player.position.z < point(447, 229).z && game.state().player.position.y < .1f,
          "The fallen bookcase must keep the stairs closed to a sprinting jump.");

    fixture.player = {outside.x, 0, outside.z};
    fixture.rival = north;
    fixture.yaw = pi / 2;
    fixture.freezeRival = true;
    game.setupFixture(fixture);
    game.step(Input{0, 0, 0, 0, Fire, Fire});
    const auto broken = game.state().brokenGlass;
    check((broken & (uint64_t(1) << pane.glass)) && world.shot(outside, inward, broken).glass != pane.glass,
          "Briar House panes must break in a match and let follow-up shots cross the opening.");

    game.start(21, 6);
    fixture.rival = point(153, 174);
    fixture.rivalYaw = -pi / 2;
    fixture.freezeRival = false;
    game.setupFixture(fixture);
    for (int i = 0; i < 360 && game.state().brokenGlass == 0; ++i) game.step({});
    check((game.state().brokenGlass & (uint64_t(1) << pane.glass)) && game.state().player.health == 100.f,
          "The first bot shot through the garden window must break glass before damaging the player.");
#ifdef FOD_LOCAL_DUEL
    game.startLocalDuel(37);
    check(planar(game.localView(0).player.position - south) < .01f && planar(game.localView(1).player.position - north) < .01f,
          "Briar House Local Duel must use both lawn spawns.");
#endif
}

// Plays the campaign the way a player would: walking the nav graph, aiming, firing and signalling.
struct CampaignDriver {
    App& app;
    const game_detail::World& world;
    game_detail::Navigation& nav;

    const Snapshot& s() const { return app.state(); }
    const MissionSnapshot& m() const { return *app.state().mission; }
    void step(const Input& in) { app.game.step(in); }

    void walkToward(Vec3 target) {
        using namespace game_detail;
        const Vec3 pos = s().player.position;
        std::vector<int> route;
        if (world.blocked(atHeight(pos, .65f), atHeight(target, .65f)) &&
            nav.path(nav.nearest(pos), nav.nearest(target), route) && route.size() > 1) {
            target = nav.node(route[1]).position;
        }
        const Vec3 dir = normalized({target.x - pos.x, 0, target.z - pos.z});
        const float sy = std::sin(s().playerYaw), cy = std::cos(s().playerYaw);
        step(Input{dir.x * cy - dir.z * sy, dir.x * sy + dir.z * cy, 0, 0, Sprint});
    }
    bool look(Vec3 target, Input& in, float tolerance) const {
        const Vec3 delta = target - s().camera.position;
        const float dy = std::remainder(std::atan2(delta.x, delta.z) - s().playerYaw, 2 * pi);
        const float dp = -std::atan2(delta.y, std::max(.01f, game_detail::planar(delta))) - s().playerPitch;
        in.lookX = std::clamp(dy, -.35f, .35f);
        in.lookY = std::clamp(dp, -.35f, .35f);
        return std::abs(dy) < tolerance && std::abs(dp) < tolerance;
    }
    Vec3 aimPoint(int slot) const {
        const auto& a = m().actors[size_t(slot)];
        return a.role == MissionRole::AlienDrone ? a.actor.position : game_detail::atHeight(a.actor.position, 1.35f);
    }
    // The nearest hostile, preferring whatever is holding the emitter or dish.
    int target(float range, bool needSight) const {
        int best = -1;
        float bestScore = range;
        for (int i = 4; i < int(MissionSnapshot::maxActors); ++i) {
            const auto& a = m().actors[size_t(i)];
            if (!a.active || a.dead || a.arrival > .25f) continue;
            if (needSight && world.blocked(s().camera.position, aimPoint(i))) continue;
            const float score = game_detail::planar(aimPoint(i) - s().player.position) - (i == m().operation.jammer ? 20.f : 0.f);
            if (score < bestScore) {
                bestScore = score;
                best = i;
            }
        }
        return best;
    }
    bool fight(bool advance = true) {
        int slot = target(60.f, true);
        if (slot >= 0) {
            Input in{};
            if (look(aimPoint(slot), in, .035f)) in.held = in.pressed = Fire;
            in.held |= Aim;
            step(in);
            return true;
        }
        slot = target(80.f, false);
        if (slot < 0) return false;
        if (advance) walkToward(m().actors[size_t(slot)].actor.position);
        else step({});
        return true;
    }
};

void playTimberlineThroughMenu(App& app, const game_detail::World& world, game_detail::Navigation& nav, float& previousTime) {
    using namespace game_detail;
    CampaignDriver d{app, world, nav};
    app.settings().difficulty = defaultDifficulty;
    app.tick(Input{0, 0, 0, 0, Confirm, Confirm});
    {
        const auto& m = d.m();
        check(d.s().match == Match::Playing && m.chapter == MissionChapter::Timberline &&
                  m.earlierMissionSeconds == previousTime && m.checkpointStage == MissionCheckpoint::Operation &&
                  m.actors[1].active && m.actors[1].civilian == CivilianOutcome::Sheltered &&
                  std::string_view(m.objective) == "TAKE NORA TO TIMBERLINE" &&
                  std::string_view(m.radioSpeaker) == "NORA MARSH",
              "Next Mission must open Timberline in the room under the dish, with Nora.");
    }
    const Vec3 kit = mission_detail::mapPoint(-245, 214);
    bool sawUnaware = false, sawShipMove = false, sawPainting = false, sawSecondDrone = false;
    int lastCalls = -1;
    Vec3 spot{};
    for (int t = 0; t < 10 * 60 * 60 && d.s().match == Match::Playing; ++t) {
        const auto& m = d.m();
        const auto& op = m.operation;
        sawShipMove |= length(m.fleetShift[2]) > 3.f;
        for (int i = 4; i < int(MissionSnapshot::maxActors); ++i) {
            const auto& a = m.actors[size_t(i)];
            if (!a.active || a.dead) continue;
            sawUnaware |= op.stage == OperationStage::Fight && !a.alerted && a.role == MissionRole::AlienTrooper;
            sawSecondDrone |= op.calls >= 2 && a.role == MissionRole::AlienDrone && a.alerted;
        }
        if (op.stage == OperationStage::Travel) {
            if (m.aliensRemaining > 0 && d.fight()) continue;
            d.walkToward(m.hasWaypoint ? m.waypoint : kit);
        } else if (op.stage == OperationStage::Call) {
            if (op.jammer >= 0 || (!op.targetSet && d.target(14.f, true) >= 0)) {
                if (!d.fight()) d.step({});
                continue;
            }
            if (op.calls != lastCalls) {
                // Open ground in plain sight, well clear of Nora and the kit.
                lastCalls = op.calls;
                float best = -1e9f;
                for (int n = 0; n < nav.size(); ++n) {
                    if (!nav.valid(n)) continue;
                    const Vec3 at = nav.node(n).position;
                    const float range = planar(at - d.s().player.position);
                    if (range < 14.f || range > 28.f || planar(at - kit) < 12.f ||
                        world.blocked(d.s().camera.position, atHeight(at, .05f))) continue;
                    if (-std::abs(range - 20.f) > best) {
                        best = -std::abs(range - 20.f);
                        spot = at;
                    }
                }
                check(best > -1e9f, "Timberline must offer open ground to call a ship onto.");
            }
            Input in{};
            d.look(spot, in, .02f);
            in.held = Signal;
            d.step(in);
            sawPainting |= d.m().operation.painting && d.m().operation.targetSet;
        } else if (op.stage == OperationStage::Fight) {
            if (!d.fight()) d.step({});
        } else {
            d.step({});
        }
    }
    const auto& done = d.m();
    check(d.s().match == Match::Results && done.phase == MissionPhase::Victory && done.operation.calls == 4 &&
              std::string_view(done.epilogue).find("NORA HAS HER NUMBERS") == 0,
          "Four answered calls must complete Timberline.");
    check(sawPainting && sawShipMove && sawUnaware && sawSecondDrone,
          "Holding Signal must mark open ground, move the saucer there, and land drops that have not seen the player.");
    previousTime = done.earlierMissionSeconds + done.missionSeconds;
    std::printf("Timberline complete in %.1fs, campaign %.1fs.\n", done.missionSeconds, previousTime);
}

void playArrayThroughMenu(App& app, const game_detail::World& world, game_detail::Navigation& nav, float& previousTime) {
    using namespace game_detail;
    CampaignDriver d{app, world, nav};
    app.tick(Input{0, 0, 0, 0, Confirm, Confirm});
    check(d.s().match == Match::Playing && d.m().chapter == MissionChapter::Array && d.m().earlierMissionSeconds == previousTime &&
              d.m().operation.stage == OperationStage::Garden && d.m().aliensRemaining == 3,
          "Next Mission must open The Array by the wreck, with the garden squad unaware.");
    const Vec3 rearDoor = mission_detail::mapPoint(368, 34);
    // The long way round: out over the north fields, then west to the saddle.
    const Vec3 route[]{mission_detail::mapPoint(700, -420), mission_detail::mapPoint(400, -560),
                       mission_detail::mapPoint(60, -560), mission_detail::mapPoint(-364, -268)};
    size_t leg = 0;
    bool sawDishLink = false, sawFinalJammer = false, sawCommit = false, sawRecall = false, signalled = false;
    for (int t = 0; t < 10 * 60 * 60 && d.s().match == Match::Playing; ++t) {
        const auto& m = d.m();
        const auto& op = m.operation;
        sawDishLink |= m.dishLink;
        if (op.stage < OperationStage::Steer) {
            if (m.aliensRemaining > 0 && d.fight()) continue;
            d.walkToward(op.stage == OperationStage::Door ? rearDoor : m.hasWaypoint ? m.waypoint : mission_detail::mapPoint(368, 300));
        } else if (op.stage == OperationStage::Steer) {
            for (int i = 4; i < int(MissionSnapshot::maxActors); ++i) {
                sawFinalJammer |= m.actors[size_t(i)].active && !m.actors[size_t(i)].dead && i == op.jammer && op.lock >= .75f;
            }
            if (planar(d.s().player.position - rearDoor) > 2.f) {
                d.walkToward(rearDoor);
                continue;
            }
            const int threat = d.target(op.jammer >= 0 ? 40.f : op.dishDistance > 16.f ? 9.f : 6.f, true);
            if (op.jammer >= 0 || threat >= 0) {
                const int slot = op.jammer >= 0 ? op.jammer : threat;
                if (world.blocked(d.s().camera.position, d.aimPoint(slot))) d.walkToward(rearDoor + Vec3{0, 0, 1.6f});
                else d.fight(false);
                continue;
            }
            if (leg + 1 < std::size(route) && planar(op.ship - Vec3{route[leg].x, 0, route[leg].z}) < 6.f) ++leg;
            // Look at the hills on the bearing of this leg to send the beam out along it.
            const Vec3 eye = d.s().camera.position;
            const Vec3 flat = normalized(Vec3{route[leg].x - eye.x, 0, route[leg].z - eye.z});
            const float reach = std::min(planar(route[leg] - eye), 41.f);
            Input in{};
            d.look(eye + flat * 10.f - Vec3{0, eye.y / reach * 10.f, 0}, in, .01f);
            in.held = Signal;
            d.step(in);
        } else if (op.stage == OperationStage::Commit) {
            sawCommit = true;
            sawRecall |= m.aliensRemaining == 0 && !m.flagshipBeam;
            d.step({});
        } else if (op.stage == OperationStage::Reunite && !signalled && m.signalReady) {
            d.step(Input{0, 0, 0, 0, Signal, Signal});
            signalled = true;
        } else {
            d.step({});
        }
    }
    const auto& done = d.m();
    check(d.s().match == Match::Results && done.phase == MissionPhase::Victory && !done.operation.clearingBurned &&
              std::string_view(done.epilogue) == "THE FLEET SET DOWN BEYOND THE RIDGE. DUSTOFF CAME BACK FOR US.",
          "Steering the beam around the clearing to the saddle must complete The Array.");
    check(sawDishLink && sawFinalJammer && sawCommit && sawRecall && signalled,
          "The finale must link the dish, send a last jammer, set the fleet down, and end on the signal to Nora.");
    previousTime = done.earlierMissionSeconds + done.missionSeconds;
    std::printf("The Array complete in %.1fs, campaign %.1fs.\n", done.missionSeconds, previousTime);
}

void playBriarHouse(const Assets& assets, const game_detail::World& world, game_detail::Navigation& nav, bool retries) {
    using namespace game_detail;
    const auto point = [](float x, float y) { return Vec3{(x - 316) * .05f, 0.f, (212.5f - y) * .05f}; };
    App app(assets, 23);
    auto& mission = app.game;
    if (retries) mission.startMission(42, 3, false);
    else app.tick(Input{0, 0, 0, 0, Confirm, Confirm});
    for (int i = 0; i < 65; ++i) mission.step({});
    check(mission.state().mission->titleCard > 0.f && mission.state().mission->phase == MissionPhase::Intro,
          "Mission 1 must open on the title card.");
    mission.step(Input{0, 0, 0, 0, Jump, Jump});
    check(mission.state().mission->titleCard == 0.f && mission.state().mission->phase == MissionPhase::Intro,
          "Jump must move the title card on to the wake-up.");
    for (int i = 0; i < 65; ++i) mission.step({});
    mission.step(Input{0, 0, 0, 0, Jump, Jump});
    check(mission.state().mission->phase == MissionPhase::Overwatch, "Jump must allow skipping the wake-up intro.");
    check(mission.canRetryCheckpoint() && mission.state().mission->checkpointStage == MissionCheckpoint::Overwatch,
          "Reaching the overwatch must save a checkpoint so a breach death skips the intro.");
    {
        // The road patrol stands well beyond the front wall: a real sniper shot from the portico.
        const auto& m = *mission.state().mission;
        float nearest = 1e9f;
        for (int i = 4; i < int(MissionSnapshot::maxActors); ++i) {
            if (m.actors[i].active) nearest = std::min(nearest, planar(m.actors[i].actor.position - point(368, 380)));
        }
        check(nearest > 12.f, "Overwatch targets must patrol the road beyond the front wall.");
    }
    // Reloading the overwatch checkpoint skips the intro and resets the patrol.
    if (retries) mission.startMission(41, 3, true);
    mission.step({});
    check(mission.state().mission->phase == MissionPhase::Overwatch && mission.state().mission->retries == int(retries) &&
              mission.state().mission->aliensRemaining == 3 && mission.state().eyeHeight > 1.6f,
          "The overwatch checkpoint must resume standing on the portico with the road patrol reset.");
    {
        // Campaign quick-swap: a tap returns to the previous weapon, holding Swap reaches the third.
        const auto tap = [&] {
            mission.step(Input{0, 0, 0, 0, Swap, Swap});
            for (int t = 0; t < 40; ++t) mission.step({});
        };
        check(mission.state().weapon == Weapon::Mp44, "The mission must start on the MP44.");
        tap();
        check(mission.state().weapon == Weapon::Sniper, "The first tap must bring up the M40A3 for the overwatch.");
        tap();
        check(mission.state().weapon == Weapon::Mp44, "A second tap must go straight back to the MP44.");
        mission.step(Input{0, 0, 0, 0, Swap, Swap});
        for (int t = 0; t < 40; ++t) mission.step(Input{0, 0, 0, 0, Swap, 0});
        check(mission.state().weapon == Weapon::Pistol, "Holding Swap must reach the USP .45.");
        tap();
        check(mission.state().weapon == Weapon::Mp44, "A tap from the USP must return to the MP44.");
        check(mission.state().mission->phase == MissionPhase::Overwatch, "Swapping must not trigger the breach.");
    }

    const auto walkToward = [&](Vec3 target) {
        const Vec3 pos = mission.state().player.position;
        std::vector<int> route;
        if (world.blocked(atHeight(pos, .65f), atHeight(target, .65f)) &&
            nav.path(nav.nearest(pos), nav.nearest(target), route) && route.size() > 1) {
            target = nav.node(route[1]).position;
        }
        const Vec3 dir = normalized({target.x - pos.x, 0, target.z - pos.z});
        const float sy = std::sin(mission.state().playerYaw), cy = std::cos(mission.state().playerYaw);
        mission.step(Input{dir.x * cy - dir.z * sy, dir.x * sy + dir.z * cy, 0, 0, Sprint});
    };
    const auto aimAt = [&](Vec3 target) {
        const Vec3 delta = target - mission.state().camera.position;
        const float dy = std::remainder(std::atan2(delta.x, delta.z) - mission.state().playerYaw, 2 * pi);
        const float dp = -std::atan2(delta.y, std::max(.01f, planar(delta))) - mission.state().playerPitch;
        Input in{};
        in.lookX = std::clamp(dy, -.35f, .35f);
        in.lookY = std::clamp(dp, -.35f, .35f);
        if (std::abs(dy) < .035f && std::abs(dp) < .035f) {
            const auto& state = mission.state();
            const Vec3 shot = normalized(delta);
            const Vec3 right{std::cos(state.playerYaw), 0, -std::sin(state.playerYaw)};
            for (int i = 1; i <= 3; ++i) {
                const auto& civ = state.mission->actors[i];
                if (!civ.active || civ.dead) continue;
                const Vec3 fromEye = atHeight(civ.actor.position, .8f) - state.camera.position;
                const float along = dot(fromEye, shot);
                const Vec3 crossing = fromEye - shot * along;
                if (along > 0 && along < length(delta) + 1.f &&
                    std::abs(dot(crossing, right)) < .5f + .10f * along &&
                    std::abs(crossing.y) < 1.f + .10f * along) {
                    in.moveX = dot(crossing, right) < 0.f ? 1.f : -1.f;
                    return in;
                }
            }
            in.held = in.pressed = Fire;
        }
        return in;
    };

    // Helper to aim directly at an active alien and fire until eliminated.
    const auto clearActiveAliens = [&](int maxTicks) {
        for (int t = 0; t < maxTicks && mission.state().match == Match::Playing; ++t) {
            const auto& m = *mission.state().mission;
            int target = -1;
            float best = 1e9f;
            for (int i = 4; i < int(MissionSnapshot::maxActors); ++i) {
                if (!m.actors[i].active || m.actors[i].dead) continue;
                const float d = planar(m.actors[i].actor.position - mission.state().player.position);
                if (d < best) { best = d; target = i; }
            }
            if (target < 0) return;
            const auto& ma = m.actors[target];
            const Vec3 aim = ma.role == MissionRole::AlienDrone ? ma.actor.position : atHeight(ma.actor.position, 1.35f);
            if (world.blocked(mission.state().camera.position, aim)) walkToward(ma.actor.position);
            else mission.step(aimAt(aim));
        }
    };

    // The lawn team and the rear-door flankers both have to fall before the Breach ends.
    const auto fighting = [&] {
        const auto phase = mission.state().mission->phase;
        return mission.state().match == Match::Playing && (phase == MissionPhase::Overwatch || phase == MissionPhase::Breach);
    };
    for (int round = 0; round < 12 && fighting(); ++round) {
        clearActiveAliens(600);
        for (int t = 0; t < 60 && fighting() && mission.state().mission->aliensRemaining == 0; ++t) mission.step({});
    }
    check(mission.state().mission->phase == MissionPhase::Regroup && mission.canRetryCheckpoint(),
          "Clearing Breach must enter the quiet Regroup lull and unlock the checkpoint.");
    check(mission.state().mission->aliensKilled >= 4,
          "A flanking team must hit the rear doors once the lawn push thins out.");
    for (int t = 0; t < 260 && mission.state().mission->phase == MissionPhase::Regroup; ++t) mission.step({});
    check(mission.state().mission->phase == MissionPhase::Rescue,
          "Regroup lull must transition cleanly into the Rescue phase.");

    // Verify retry from checkpoint starts directly in Rescue outside Briar House on South Boulevard,
    // without prematurely triggering the distant town districts.
    if (retries) {
        mission.startMission(43, 3, true);
        mission.step({});
        check(mission.state().mission->phase == MissionPhase::Rescue && mission.state().mission->introProgress == 1.f,
              "Retrying from checkpoint must resume directly in Rescue phase.");
        check(mission.state().mission->aliensRemaining == 0,
              "Starting on South Boulevard must not prematurely trigger distant town rescue districts.");
    }

    // Sweep all three surrounding town districts (East Ridge, North Valley, West Timberline),
    // verifying that the rescue objectives and Extraction LZ cover ground well outside Briar House.
    const Vec3 sectorTargets[3]{point(920, 445), point(535, -215), point(-298, 252)};
    auto outsideEstate = [](Vec3 p) { return std::abs(p.x) > 19.f || std::abs(p.z) > 15.5f; };
    bool signalledBen = false;
    for (int s = 0; s < 3; ++s) {
        check(outsideEstate(sectorTargets[s]), "Each town rescue district must lie outside Briar House.");
        const int civSlot = s + 1;
        for (int t = 0; t < 1800 && !civilianRescued(mission.state().mission->actors[civSlot].civilian); ++t) {
            const auto& m = *mission.state().mission;
            if (m.aliensRemaining > 0) {
                clearActiveAliens(180);
            } else if (s == 0 && !retries && m.signalReady && std::string_view(m.signalHint) == "SIGNAL BEN") {
                // Wave Ben over from where the player stands instead of walking up to him.
                const float range = planar(m.actors[civSlot].actor.position - mission.state().player.position);
                mission.step(Input{0, 0, 0, 0, Signal, Signal});
                signalledBen = civilianRescued(mission.state().mission->actors[civSlot].civilian) && range > 4.f;
            } else {
                walkToward(sectorTargets[s]);
            }
        }
    }
    check(retries || signalledBen, "Signal must send Ben across from a distance once nothing can see him.");
    check(mission.state().mission->civiliansSaved == 3, "Clearing and reaching all districts must rescue all 3 civilians.");
    check(mission.state().mission->phase == MissionPhase::Uplink && mission.state().mission->uplinkHealth > 0.f,
          "The completed sweep must reveal the jammer objective.");
    check(mission.state().mission->hasWaypoint && outsideEstate(uplinkPosition),
          "The North Avenue uplink must publish route guidance.");
    check(mission.canRetryCheckpoint() && mission.state().mission->checkpointStage == MissionCheckpoint::Uplink,
          "Finishing the sweep must save a checkpoint before the uplink.");
    if (retries) {
        const int killsAtCheckpoint = mission.state().mission->aliensKilled;
        mission.startMission(44, 3, true);
        mission.step({});
        {
            const auto& cp = *mission.state().mission;
            check(cp.phase == MissionPhase::Uplink && cp.civiliansSaved == 3 && cp.retries == 3 &&
                      cp.aliensKilled == killsAtCheckpoint && cp.missionSeconds > 20.f,
                  "The uplink checkpoint must carry all civilian rescues, kills, and mission time.");
            int sheltering = 0;
            for (int c = 1; c <= 3; ++c) sheltering += cp.actors[c].active && civilianRescued(cp.actors[c].civilian);
            check(sheltering == 3, "Civilian outcomes must survive a checkpoint reload.");
        }
    }

    const Vec3 core = uplinkPosition + uplinkEmitter;
    const float initialIntegrity = mission.state().mission->uplinkHealth;
    for (int t = 0; t < 1800 && mission.state().mission->phase == MissionPhase::Uplink; ++t) {
        if (planar(mission.state().player.position - uplinkPosition) > 16.f ||
            world.blocked(mission.state().camera.position, core)) walkToward(uplinkPosition);
        else mission.step(aimAt(core));
    }
    check(mission.state().mission->phase == MissionPhase::Incoming && initialIntegrity > 0.f &&
              mission.state().mission->uplinkHealth == 0.f && !mission.state().apacheReady,
          "Player rounds must destroy the uplink before the major counterattack.");
    check(mission.state().mission->checkpointStage == MissionCheckpoint::Extraction, "Destroying the uplink must save a separate extraction checkpoint.");

    if (retries) {
        mission.startMission(45, 3, true);
        check(mission.state().mission->phase == MissionPhase::Incoming && mission.state().mission->uplinkHealth == 0.f &&
                  mission.state().mission->uplinkVisible && mission.state().mission->retries == 4,
              "The extraction checkpoint must keep the uplink disabled.");
    }

    const Vec3 lz = point(210, -220);
    for (int t = 0; t < 650 && mission.state().mission->phase == MissionPhase::Incoming; ++t) {
        if (planar(mission.state().player.position - lz) > 5.f) walkToward(lz);
        else mission.step({});
    }
    check(mission.state().mission->phase == MissionPhase::Extraction && mission.state().apacheReady &&
              mission.state().mission->evacSeconds > 40.f,
          "Reaching the North LZ must sound the warhorn and start the timed hold with Apache support.");

    // Deploy US Apache support and hold until the evac birds arrive.
    mission.step(Input{0, 0, 0, 0, Streak, Streak});
    for (int t = 0; t < 60 && !mission.state().apacheActive; ++t) mission.step({});
    check(mission.state().apacheActive, "Pressing Streak during Extraction must deploy the US Apache gunship.");
    const int killsBeforeHold = mission.state().mission->aliensKilled;
    const Vec3 beyondPerimeter = point(525, -220);
    for (int t = 0; t < 240 && planar(mission.state().player.position - lz) < 15.1f; ++t) walkToward(beyondPerimeter);
    const float pausedHold = mission.state().mission->evacSeconds;
    for (int t = 0; t < 30; ++t) mission.step({});
    check(planar(mission.state().player.position - lz) > 15.f && mission.state().mission->evacSeconds == pausedHold &&
              std::string_view(mission.state().mission->objective) == "RETURN TO THE NORTH LZ",
          "The evacuation countdown must pause while the player is outside the landing zone.");
    // Dustoff comes down in the fight. Clear what can see the wall, then send the family across.
    bool sawBoarding = false, sawLift = false, skipped = false;
    for (int t = 0; t < 120 * 60 && mission.state().match == Match::Playing; ++t) {
        const auto& m = *mission.state().mission;
        if (m.cinematic) {
            sawLift = true;
            check(m.civiliansBoarded == 2 && m.actors[mission_detail::nora + 1].civilian == CivilianOutcome::Stayed && m.actors[0].active,
                  "Ben and Walt must be aboard, with Nora and Delta-1 still on the ground, as Dustoff lifts.");
            if (!retries && !skipped) {
                for (int wait = 0; wait < 65; ++wait) mission.step({});
                mission.step(Input{0, 0, 0, 0, Jump, Jump});
                skipped = true;
            } else {
                mission.step({});
            }
            continue;
        }
        if (m.phase == MissionPhase::Boarding && m.evacVisible) {
            sawBoarding = true;
            if (planar(mission.state().player.position - lz) >= 12.f) walkToward(lz);
            else if (m.aliensRemaining > 0) clearActiveAliens(60);
            else mission.step(m.signalReady ? Input{0, 0, 0, 0, Signal, Signal} : Input{});
            continue;
        }
        if (planar(mission.state().player.position - lz) >= 14.f) walkToward(lz);
        else if (m.aliensRemaining > 0) clearActiveAliens(60);
        else mission.step({});
    }
    check(sawBoarding && sawLift, "The hold must end with Dustoff on the ground and the player sending the family across.");
    check(mission.state().match == Match::Results && mission.state().mission->phase == MissionPhase::Victory,
          "Sending the family to Dustoff must complete Mission 1 with Victory.");
    const auto& result = *mission.state().mission;
    check(result.civiliansBoarded == 2 && result.civiliansSaved == 3 && result.civiliansLost == 0 &&
              result.retries == (retries ? 4 : 0) &&
              std::string_view(result.epilogue) == "BEN AND WALT FLEW OUT. NORA STAYED WITH HER DISH.",
          "Natural and skipped lifts must produce the same outcome: two aboard, Nora staying.");
    check(mission.state().mission->evacSeconds == 0.f, "Dustoff must wait for the evac countdown to finish.");
    check(mission.state().mission->aliensKilled - killsBeforeHold > 4,
          "Reinforcements must keep landing during the timed hold.");
    if (!retries) {
        float previousTime = result.missionSeconds;
        playTimberlineThroughMenu(app, world, nav, previousTime);
        playArrayThroughMenu(app, world, nav, previousTime);
        check(!mission.state().mission->nextMissionReady() && mission.state().mission->chapter == MissionChapter::Array,
              "The campaign must finish after The Array.");
    }
    // Verify returning to Main Menu clears stale mission state cleanly.
    mission.quit();
    check(mission.state().match == Match::Start && !mission.state().mission,
          "Returning to Main Menu from mission results must clear stale mission state.");
}

void checkBriarHouseMission(Assets& assets) {
    using namespace game_detail;
    buildBriarHouse(assets, BriarHouseMode::Invasion);
    {
        App app(assets);
        app.game.startFinaleReplay(1, defaultDifficulty);
        app.tick(Input{0, 0, 0, 0, Signal, Pause});
        app.tick(Input{0, 0, 0, 0, Signal, Confirm});
        app.tick(Input{0, 0, 0, 0, Signal});
        check(app.state().match == Match::Playing && !app.state().mission->dishLink,
              "A Signal held through resume must stay blocked.");
        app.tick({});
        app.tick(Input{0, 0, 0, 0, Signal, Signal});
        check(app.state().mission->dishLink, "A fresh Signal must steer the beam after resume.");
    }
    check(assets.briarHouseMode == BriarHouseMode::Invasion, "Invasion builder must set BriarHouseMode::Invasion.");
    check(assets.residentBytes < 12u * 1024u * 1024u, "Invasion Briar House must stay within the 12 MiB handheld budget.");
    check(!assets.alienTrooper.triangles.empty() && !assets.alienDrone.triangles.empty() &&
          !assets.alienFleet[0].triangles.empty() && !assets.alienFleet[0].chunks.empty(),
          "Invasion mode must generate welded procedural alien trooper, drone, and frustum-bounded fleet meshes.");
    check(!assets.alienUplink.chunks.empty() && !assets.alienUplinkCore.triangles.empty(),
          "The destructible uplink must have a visible base and emitter mesh.");
    check(!assets.relayTerminal.chunks.empty(), "Nora's emitter kit and the dish console must have their own mesh.");
    // The creature carries tendrils, fangs and claws but stays well under the 2,424-vertex infantry rig.
    check(assets.alienTrooper.vertices.size() < 900 && assets.alienDrone.vertices.size() < 80,
          "Procedural alien meshes must weld duplicate vertices to minimize per-frame transforms.");
    check(assets.alienTrooperPoses.size() == size_t(alienTotalPoses) &&
              assets.alienEnforcerPoses.size() == assets.alienTrooperPoses.size(),
          "The ground alien and the enforcer must ship idle, lope, ray-gun aim and recoil poses.");
    check(!assets.alienRayGun.triangles.empty() && !assets.alienRayRings.triangles.empty() &&
              !assets.alienHeavyRayGun.triangles.empty() && !assets.alienHeavyRayRings.triangles.empty(),
          "Invasion mode must build animated ray guns and spinning accelerator rings for troopers and enforcers.");
    check(assets.alienRayGun.vertices.size() < 260 && assets.alienHeavyRayGun.vertices.size() < 320,
          "Procedural alien ray guns must stay welded and compact for fast per-frame rendering.");
    check(!assets.mist.empty() && !assets.fires.empty() && !assets.smoke.empty() && assets.roadFlares.size() >= 4,
          "Invasion mode must place dusk mist, fires, smoke, and the extraction flares.");
    check(!assets.shipBoxTruck.triangles.empty() && !assets.shipBoxCar.triangles.empty() &&
          !assets.shipBoxCrate.triangles.empty() && !assets.shipBoxBarrel.triangles.empty() &&
          !assets.shipBoxLamp.triangles.empty() && !assets.shipBoxContainer.triangles.empty() &&
          assets.propInstances.size() >= 8,
          "Invasion mode must reuse extracted Ship Box vehicles, crates, barrels, lamps, and container across the town.");
    check(assets.alienEnforcer.vertices.size() > assets.alienTrooper.vertices.size() &&
              assets.alienEnforcer.vertices.size() < 900,
          "Enforcers must carry their bone armor within the creature budget.");
    for (const auto& pose : assets.alienEnforcerPoses) {
        check(pose.vertexCount() == assets.alienEnforcer.vertices.size(), "Enforcer poses must match the enforcer mesh.");
    }
    check(!assets.transportHelo.triangles.empty() && !assets.transportRotor.triangles.empty(),
          "Invasion mode must build the evac helicopter and its spinning rotor.");
    check(assets.civilianPoses.size() == civilianStandPose + civilianStandPoses &&
              std::all_of(assets.civilianPoses.begin(), assets.civilianPoses.end(), [&](const Mesh& pose) {
                  return pose.vertexCount() == assets.civilianMesh.vertexCount() && pose.triangles.empty();
              }),
          "Civilian cower, run, and breathing frames must share the same outfit topology.");
    for (const auto& pose : assets.alienTrooperPoses) {
        check(pose.vertexCount() == assets.alienTrooper.vertices.size() && pose.triangles.empty(),
              "Alien poses must be position-only frames on the base mesh's topology.");
    }
    {
        // Feet planted, roughly two metres tall, and the lope actually moves the limbs.
        const auto& first = assets.alienTrooperPoses[alienIdlePoses];
        const auto& later = assets.alienTrooperPoses[alienIdlePoses + alienStridePoses / 2];
        float low = 1e9f, high = -1e9f, moved = 0;
        for (size_t i = 0; i < first.vertexCount(); ++i) {
            low = std::min(low, first.position(i).y);
            high = std::max(high, first.position(i).y);
            moved = std::max(moved, length(first.position(i) - later.position(i)));
        }
        check(std::abs(low) < .05f && high > 1.9f && high < 2.7f && moved > .2f,
              "The ground alien must stand on its feet at creature height and stride between poses.");
    }
    check(!assets.commanderUniform.empty() && !assets.enforcerUniform.empty() &&
          !assets.civilianUniforms[0].empty() && !assets.civilianUniforms[1].empty() &&
          !assets.civilianUniforms[2].empty(),
          "Invasion mode must build Delta commander, enforcer, and civilian uniform remaps.");
    check(assets.audio.size() >= size_t(Sound::Count) &&
          !assets.audio[size_t(Sound::PlasmaFire)].samples.empty() &&
          !assets.audio[size_t(Sound::ConcussionRing)].samples.empty() &&
          !assets.audio[size_t(Sound::Heartbeat)].samples.empty() &&
          !assets.audio[size_t(Sound::AlienHorn)].samples.empty() &&
          !assets.audio[size_t(Sound::InvasionHum)].samples.empty() &&
          !assets.audio[size_t(Sound::RadioChirp)].samples.empty() &&
          !assets.audio[size_t(Sound::CivilianSaved)].samples.empty() &&
          !assets.audio[size_t(Sound::AlienRoar)].samples.empty() &&
          !assets.audio[size_t(Sound::RayCharge)].samples.empty() &&
          !assets.audio[size_t(Sound::ShieldBreak)].samples.empty(),
          "Invasion mode must synthesize all mission and cinematic audio clips.");

    // Verify Briar House connects seamlessly out through the breached gates into all four
    // surrounding Colorado mountain town districts (South Blvd, East Ridge, North Valley LZ, West Timberline).
    const World world(assets);
    Navigation nav(assets);
    // Compare the column search with the original full scan, including ties
    // between grid points and queries centered on blocked or isolated nodes.
    for (size_t i = 0; i < assets.navNodes.size(); i += 9) {
        const Vec3 node = assets.navNodes[i].position;
        for (const Vec3 query : {node, node + Vec3{.5f, 0, .5f}}) {
            int expected = -1;
            float best = infinity;
            for (int j = 0; j < nav.size(); ++j) {
                if (!nav.valid(j)) continue;
                const float distance = planarSquared(nav.node(j).position - query);
                if (distance < best) {
                    best = distance;
                    expected = j;
                }
            }
            check(nav.nearest(query) == expected, "The town navigation search must preserve the nearest node and tie order.");
        }
    }
    const auto point = [](float x, float y) { return Vec3{(x - 316) * .05f, 0.f, (212.5f - y) * .05f}; };
    const int start = nav.nearest(point(368, 305));
    std::vector<int> path;
    for (const auto townSpot : {point(368, 565), point(920, 445), point(535, -215), point(210, -220), point(-298, 252)}) {
        check(nav.path(start, nav.nearest(townSpot), path),
              "Invasion mode must connect Briar House out into every surrounding town district.");
        check(planar(nav.node(path.back()).position - townSpot) < 2.2f,
              "Each surrounding town objective must sit directly on the walkable town navigation graph.");
    }
    check(assets.extent.x >= 50.f && assets.extent.z >= 38.f,
          "Invasion mode must open the surrounding Colorado town well beyond Briar House's walls.");

    App app(assets, 19);
    check(app.settings().campaign, "App must auto-enable campaign mode for Invasion Briar House.");
    // Press Confirm on Start Screen to launch Mission 1.
    app.tick(Input{0, 0, 0, 0, Confirm, Confirm});
    check(app.state().match == Match::Playing && app.state().mission &&
              app.state().mission->active && app.state().mission->campaign,
          "Confirm on Invasion Start Screen must launch the full campaign at Briar House.");
    check(app.state().mission->phase == MissionPhase::Intro, "Mission 1 must begin with the concussed wake-up intro.");
    check(app.state().eyeHeight < 0.6f && app.state().mission->concussion > 0.9f,
          "Concussed intro must start prone with full disorientation.");
    check((app.state().brokenGlass & ((uint64_t(1) << 2) | (uint64_t(1) << 3) | (uint64_t(1) << 4))) != 0,
          "Invasion mission must start with blown-out front windows.");

    // Render intro frame and HUD at handheld size to verify zero renderer/HUD crashes,
    // and confirm the Colorado red-orange dusk sunset palette is active in Invasion mode.
    Renderer renderer(assets, RenderLook::Enhanced, 400 * 225);
    std::vector<uint16_t> color(400 * 225), fb(800 * 480);
    renderer.render(app.state(), 400, 225, color.data());
    scaleWorld(color.data(), 400, 225, fb.data(), false, true);
    drawHud(assets, app.state(), app.settings(), fb.data(), false);
    {
        Snapshot vista{};
        vista.match = Match::Start;
        renderer.render(vista, 400, 225, color.data());
        int warmSkyPixels = 0;
        for (int y = 8; y < 70; ++y) {
            for (int x = 160; x < 390; ++x) {
                const uint16_t c = color[size_t(y) * 400 + x];
                const int r = (c >> 11) * 8, b = (c & 31) * 8;
                if (r > b + 45) ++warmSkyPixels;
            }
        }
        check(warmSkyPixels > 2000, "Invasion Briar House must render the warm Colorado red-orange dusk sunset sky.");
    }

    // Verify copy-on-write zero-allocation publish: steady ticks reuse the same MissionSnapshot buffer,
    // while holding an external Snapshot copy triggers a safe copy-on-write allocation.
    const auto* firstPtr = app.state().mission.get();
    app.tick({});
    check(app.state().mission.get() == firstPtr, "Steady mission ticks must reuse the published MissionSnapshot without reallocating.");
    const Snapshot savedCopy = app.state();
    app.tick({});
    check(app.state().mission.get() != savedCopy.mission.get(),
          "Holding an external Snapshot copy must trigger copy-on-write so frozen snapshots stay immutable.");

    // Let the concussed wake-up intro run naturally through Delta-1's callouts into Overwatch.
    bool sawUnderAttack = false;
    bool sawConcussedFov = false;
    std::string shownLine = app.state().mission->radioLine;
    int shownAt = 0, quickestSwap = 1 << 20, radioLines = 0, tick = 0;
    for (int i = 0; i < 800 && app.state().mission->phase == MissionPhase::Intro; ++i) {
        app.tick({});
        ++tick;
        if (app.state().camera.fov > 71.f * pi / 180.f) sawConcussedFov = true;
        if (std::string_view(app.state().mission->radioLine).find("WARHORSE") != std::string_view::npos) {
            sawUnderAttack = true;
        }
        if (shownLine != app.state().mission->radioLine) {
            if (!shownLine.empty()) quickestSwap = std::min(quickestSwap, tick - shownAt);
            shownLine = app.state().mission->radioLine;
            shownAt = tick;
            ++radioLines;
        }
    }
    // The overwatch itself opens in silence: nothing replaces the last wake-up line.
    const int wakeLines = radioLines;
    for (int i = 0; i < 240; ++i) {
        app.tick({});
        ++tick;
        if (shownLine != app.state().mission->radioLine) {
            quickestSwap = std::min(quickestSwap, tick - shownAt);
            shownLine = app.state().mission->radioLine;
            shownAt = tick;
            ++radioLines;
        }
    }
    check(radioLines == wakeLines, "The overwatch must open without a new order over the radio.");
    check(sawConcussedFov, "Concussed intro must preserve FOV breathing modulation across weapon updates.");
    check(sawUnderAttack, "Delta-1 must tell the player their helicopter went down during the awakening.");
    check(radioLines >= 3 && quickestSwap >= 84,
          "Routine radio lines must stay up long enough to read before the next one replaces them.");
    check(app.state().mission->phase == MissionPhase::Overwatch,
          "Intro must transition into the unhurried Sniper Overwatch recon beat.");
    check(app.state().eyeHeight > 1.6f && app.state().mission->concussion == 0.f,
          "Player must stand up cleanly when Overwatch begins.");
    check(app.state().mission->aliensRemaining == 3 && !app.state().mission->actors[4].alerted,
          "Overwatch must spawn 3 unalerted patrolling lawn hostiles sweeping with scanner beams.");

    // Advance a few ticks so scanner beams emit and render the live Overwatch frame.
    for (int i = 0; i < 15; ++i) app.tick({});
    renderer.render(app.state(), 400, 225, color.data());
    scaleWorld(color.data(), 400, 225, fb.data(), false, false);
    drawHud(assets, app.state(), app.settings(), fb.data(), false);

    playBriarHouse(assets, world, nav, true);
    playBriarHouse(assets, world, nav, false);
}

namespace mission_rules {
using namespace game_detail;
using View = std::shared_ptr<const MissionSnapshot>;

void clearAll(mission_detail::MissionController& controller, Snapshot& snap, mission_detail::MissionHost& host, View& view,
              int except = -1, bool spareDrones = false) {
    controller.publish(view);
    for (int i = 4; i < int(view->actors.size()); ++i) {
        const auto& a = view->actors[size_t(i)];
        if (!a.active || a.dead || i == except || (spareDrones && a.role == MissionRole::AlienDrone)) continue;
        const Vec3 aim = a.actor.position + Vec3{0, a.role == MissionRole::AlienDrone ? 0.f : 1.25f, 0};
        Vec3 hit{};
        controller.apacheShotAlien(i, aim + Vec3{0, 0, 2}, {0, 0, -1}, 3.f, 1000.f, snap, host, hit);
    }
}

void place(Snapshot& snap, Vec3 at) {
    snap.player.position = at;
    snap.eyeHeight = 1.7f;
    snap.camera.position = atHeight(at, 1.7f);
}

// Point the camera at a spot on the ground.
void lookAt(Snapshot& snap, Vec3 at) {
    const Vec3 d = at - snap.camera.position;
    snap.camera.yaw = std::atan2(d.x, d.z);
    snap.camera.pitch = std::atan2(-d.y, planar(d));
}

// Open ground the emitter will accept: in plain sight, clear of the kit.
Vec3 callSpot(const Snapshot& snap, Vec3 kit, const World& world, Navigation& nav) {
    Vec3 best{};
    float bestScore = -1e9f;
    for (int n = 0; n < nav.size(); ++n) {
        if (!nav.valid(n)) continue;
        const Vec3 at = nav.node(n).position;
        const float range = planar(at - snap.player.position);
        if (range < 14.f || range > 26.f || planar(at - kit) < 12.f || world.blocked(snap.camera.position, atHeight(at, .05f))) continue;
        if (-std::abs(range - 18.f) > bestScore) {
            bestScore = -std::abs(range - 18.f);
            best = at;
        }
    }
    check(bestScore > -1e9f, "The Timberline rules fixture must find open ground to call onto.");
    return best;
}

} // namespace mission_rules

void checkTimberlineRules(mission_detail::MissionController& controller, Snapshot& snap, mission_detail::MissionHost& host,
                          mission_rules::View& view, const game_detail::World& world, game_detail::Navigation& nav) {
    using namespace mission_rules;
    using mission_detail::MissionStart;
    const Vec3 kit = mission_detail::mapPoint(-245, 214);
    place(snap, kit + Vec3{3.f, 0, -2.f});
    controller.step(snap, {}, tickSeconds, false, host);
    controller.publish(view);
    check(view->operation.stage == OperationStage::Call && view->operation.kitVisible &&
              view->checkpointStage == MissionCheckpoint::Operation,
          "Reaching Walt's stone wall must set up the emitter and save a checkpoint.");
    // Too close to the kit is refused; open ground well away from it is marked.
    Vec3 nearKit{};
    for (int n = 0; n < nav.size(); ++n) {
        if (!nav.valid(n)) continue;
        const Vec3 at = nav.node(n).position;
        if (planar(at - kit) > 3.f && planar(at - kit) < 7.f && planar(at - snap.player.position) > 6.5f &&
            !world.blocked(snap.camera.position, atHeight(at, .05f))) {
            nearKit = at;
            break;
        }
    }
    check(planar(nearKit - kit) > 3.f, "The Timberline rules fixture must find visible ground beside the kit.");
    lookAt(snap, nearKit);
    controller.step(snap, Input{0, 0, 0, 0, Signal, Signal}, tickSeconds, false, host);
    controller.publish(view);
    check(!view->operation.targetSet && std::string_view(view->signalHint) == "TOO CLOSE TO THE KIT",
          "The emitter must refuse a landing spot beside the kit.");
    const auto callAt = [&](Vec3 spot) {
        lookAt(snap, spot);
        controller.step(snap, Input{0, 0, 0, 0, Signal, Signal}, tickSeconds, false, host);
        controller.publish(view);
        check(view->operation.targetSet && view->operation.aimValid && planar(view->operation.target - spot) < .5f,
              "Holding Signal on open ground must mark it as the landing spot.");
    };
    const auto waitForDrop = [&](int calls) {
        for (int t = 0; t < 40 * 60 && view->operation.calls < calls; ++t) {
            controller.step(snap, {}, tickSeconds, false, host);
            controller.publish(view);
        }
    };
    Vec3 spot = callSpot(snap, kit, world, nav);
    callAt(spot);
    waitForDrop(1);
    check(view->operation.stage == OperationStage::Fight && view->operation.calls == 1 && view->aliensRemaining == 2,
          "The saucer must fly to the marked spot and land a drop there.");
    int unaware = 0;
    for (int i = 4; i < int(view->actors.size()); ++i) {
        const auto& a = view->actors[size_t(i)];
        if (!a.active || a.dead) continue;
        unaware += !a.alerted;
        check(planar(a.actor.position - spot) < 4.5f, "A called drop must land where it was called.");
    }
    check(unaware == 2, "A called drop must land without having seen the player.");
    clearAll(controller, snap, host, view);
    controller.step(snap, {}, tickSeconds, false, host);
    controller.publish(view);
    check(view->operation.stage == OperationStage::Call && view->operation.calls == 1 &&
              view->checkpointStage == MissionCheckpoint::Operation,
          "Clearing a landing must arm the next call.");
    // A death goes back to the last call with the calibration kept.
    const float earlier = view->earlierMissionSeconds;
    snap.dead = true;
    controller.step(snap, {}, tickSeconds, false, host);
    snap = {};
    snap.match = Match::Playing;
    Vec3 start{};
    float yaw = 0;
    controller.start(47, 3, MissionStart::Retry, snap, start, yaw, host);
    controller.publish(view);
    check(view->operation.stage == OperationStage::Call && view->operation.calls == 1 && view->retries >= 1 &&
              view->earlierMissionSeconds == earlier && view->missionSeconds > 0.f,
          "Retrying Timberline must keep the calls already answered and the campaign time.");
    place(snap, start);
    spot = callSpot(snap, kit, world, nav);
    callAt(spot);
    waitForDrop(2);
    clearAll(controller, snap, host, view);
    controller.step(snap, {}, tickSeconds, false, host);
    // Third call: a jammer takes the emitter and drags the saucer down onto the kit.
    spot = callSpot(snap, kit, world, nav);
    callAt(spot);
    bool pulled = false;
    for (int t = 0; t < 60 * 60 && view->operation.calls < 3; ++t) {
        // Shoot everything except the jammer; let it reach the emitter.
        int jammerSlot = -1;
        for (int i = 4; i < int(view->actors.size()); ++i) {
            const auto& a = view->actors[size_t(i)];
            if (a.active && !a.dead && a.role == MissionRole::AlienDrone && a.alerted) jammerSlot = i;
        }
        if (view->operation.jammer < 0) clearAll(controller, snap, host, view, jammerSlot);
        controller.step(snap, {}, tickSeconds, false, host);
        controller.publish(view);
        pulled |= view->operation.jammer >= 0 && view->operation.pulled;
    }
    check(pulled && view->operation.calls == 3 && std::string_view(view->subObjective) == "THEY CAME DOWN ON THE EMITTER",
          "A jammer on the emitter must drag the third drop down onto the kit.");
    clearAll(controller, snap, host, view);
    controller.step(snap, {}, tickSeconds, false, host);
    controller.publish(view);
    // Fourth call: the heavy one creeps toward the kit unless Signal stays held.
    spot = callSpot(snap, kit, world, nav);
    callAt(spot);
    const float marked = planar(view->operation.target - kit);
    for (int t = 0; t < 90; ++t) controller.step(snap, {}, tickSeconds, false, host);
    controller.publish(view);

    check(view->operation.pulled && planar(view->operation.target - kit) < marked - 1.5f,
          "Letting go of the heavy call must let its landing spot creep toward the emitter.");
}

void checkArrayRules(const Assets& assets, mission_detail::MissionHost& host, const game_detail::World& world,
                     game_detail::Navigation& nav) {
    using namespace mission_rules;
    using mission_detail::MissionStart;
    {
        // Killing Nora on the way back in ends the mission, as it does at Timberline.
        View view;
        mission_detail::MissionController controller(assets, world, nav);
        Snapshot snap{};
        snap.difficulty = 3;
        snap.match = Match::Playing;
        Vec3 start{};
        float yaw = 0;
        controller.seedChapterReplay(MissionChapter::Array);
        controller.start(50, 3, MissionStart::NextChapter, snap, start, yaw, host);
        place(snap, start);
        for (int round = 0; round < 6; ++round) {
            controller.publish(view);
            if (view->phase != MissionPhase::Operation) break;
            const Vec3 nora = view->actors[1].actor.position;
            Vec3 hit{};
            controller.resolvePlayerShot(atHeight(nora, 1.f) + Vec3{0, 0, -1.2f}, {0, 0, 1}, 40.f, 2.f, snap, host, hit);
            for (int t = 0; t < 60; ++t) controller.step(snap, {}, tickSeconds, false, host);
        }
        controller.publish(view);
        check(view->phase == MissionPhase::Failed && std::string_view(view->failReason) == "NORA WAS KILLED",
              "Killing Nora in The Array must fail the mission.");
    }
    View view;
    mission_detail::MissionController controller(assets, world, nav);
    Snapshot snap{};
    snap.difficulty = 3;
    snap.match = Match::Playing;
    Vec3 start{};
    float yaw = 0;
    controller.seedChapterReplay(MissionChapter::Array);
    controller.start(51, 3, MissionStart::NextChapter, snap, start, yaw, host);
    controller.publish(view);
    check(view->chapter == MissionChapter::Array && view->operation.stage == OperationStage::Garden && view->actors[1].active &&
              view->aliensRemaining == 3 && !view->actors[4].alerted,
          "The Array must open by the wreck with Nora, and the garden squad unaware.");
    place(snap, start);
    clearAll(controller, snap, host, view);
    controller.step(snap, {}, tickSeconds, false, host);
    controller.publish(view);
    check(view->operation.stage == OperationStage::Hall, "Clearing the garden must open the way into the hall.");
    place(snap, mission_detail::mapPoint(368, 404));
    for (int t = 0; t < 10 * 60 && view->operation.stage == OperationStage::Hall; ++t) {
        controller.step(snap, {}, tickSeconds, false, host);
        if (t > 30) clearAll(controller, snap, host, view);
        controller.publish(view);
    }
    check(view->operation.stage == OperationStage::Door, "All four of the hall squad must come and be cleared.");
    const Vec3 rearDoor = mission_detail::mapPoint(368, 34);
    place(snap, rearDoor);
    for (int t = 0; t < 25 * 60 && view->operation.stage == OperationStage::Door; ++t) {
        controller.step(snap, {}, tickSeconds, false, host);
        controller.publish(view);
    }
    check(view->operation.stage == OperationStage::Steer && view->checkpointStage == MissionCheckpoint::Operation &&
              planar(view->actors[1].actor.position - mission_detail::mapPoint(594, 80)) < 2.5f,
          "With Nora at the console and the player at the rear door, the dish must come under the pointer.");
    // Left alone, the mothership homes on the dish.
    float first = view->operation.dishDistance;
    for (int t = 0; t < 60 * 60 && snap.match == Match::Playing && view->phase == MissionPhase::Operation; ++t) {
        clearAll(controller, snap, host, view);
        controller.step(snap, {}, tickSeconds, false, host);
        controller.publish(view);
    }
    check(view->phase == MissionPhase::Failed && std::string_view(view->failReason) == "THE FLEET FOUND THE DISH" && first > 15.f,
          "If nobody steers it, the mothership must drift onto the dish and end the mission.");
    for (int t = 0; t < 3 * 60; ++t) controller.step(snap, {}, tickSeconds, false, host);
    snap = {};
    snap.match = Match::Playing;
    controller.start(52, 3, MissionStart::Retry, snap, start, yaw, host);
    controller.publish(view);
    check(view->operation.stage == OperationStage::Steer && view->operation.lock == 0.f && !view->operation.clearingBurned,
          "Retrying The Array must return to the rear door with the beam back where it started.");
    place(snap, start);
    // Straight across the clearing: it burns.
    for (int t = 0; t < 40 * 60 && !view->operation.clearingBurned; ++t) {
        lookAt(snap, view->operation.clearing);
        clearAll(controller, snap, host, view);
        controller.step(snap, Input{0, 0, 0, 0, Signal, Signal}, tickSeconds, false, host);
        controller.publish(view);
    }
    check(view->operation.clearingBurned && view->dishLink, "Steering the beam over the clearing must burn Dustoff's landing ground.");
    // On to the saddle: look at the hills on its bearing and hold.
    bool jammedHold = false;
    float lockAtJam = -1.f;
    for (int t = 0; t < 120 * 60 && view->operation.stage == OperationStage::Steer && snap.match == Match::Playing; ++t) {
        const Vec3 eye = snap.camera.position, saddle = view->operation.saddle;
        const Vec3 flat = normalized(Vec3{saddle.x - eye.x, 0, saddle.z - eye.z});
        lookAt(snap, eye + flat * 10.f - Vec3{0, eye.y / 41.f * 10.f, 0});
        if (view->operation.jammer >= 0) {
            if (lockAtJam < 0.f) lockAtJam = view->operation.lock;
            else if (std::abs(view->operation.lock - lockAtJam) < 1e-4f) jammedHold = true;
            if (jammedHold) clearAll(controller, snap, host, view);
        } else {
            // Late in the lock, leave the drones so the last jammer reaches the dish.
            clearAll(controller, snap, host, view, -1, view->operation.lock >= .7f);
        }
        controller.step(snap, Input{0, 0, 0, 0, Signal, Signal}, tickSeconds, false, host);
        controller.publish(view);
    }
    check(jammedHold && view->operation.stage == OperationStage::Commit,
          "The last jammer must hold the lock until it is shot, and then the fleet must commit.");
    for (int t = 0; t < 10 * 60 && view->operation.stage != OperationStage::Reunite; ++t) {
        controller.step(snap, {}, tickSeconds, false, host);
        controller.publish(view);
    }
    check(view->operation.stage == OperationStage::Reunite && !view->flagshipBeam && view->aliensRemaining == 0 &&
              std::string_view(view->signalHint) == "SIGNAL NORA",
          "Once the fleet sets down, its troops go with it and the last signal is for Nora.");
    controller.step(snap, Input{0, 0, 0, 0, Signal, Signal}, tickSeconds, false, host);
    float closest = 1e9f;
    for (int t = 0; t < 15 * 60 && snap.match == Match::Playing; ++t) {
        controller.step(snap, {}, tickSeconds, false, host);
        controller.publish(view);
        closest = std::min(closest, planar(view->actors[1].actor.position - snap.player.position));
    }
    check(snap.match == Match::Results && view->phase == MissionPhase::Victory && closest < 2.8f && !view->nextMissionReady() &&
              std::string_view(view->epilogue) == "THE FLEET SET DOWN BEYOND THE RIDGE. WE MET DUSTOFF ON THE ROAD.",
          "Nora must come when signalled, and a burned clearing must change how the campaign ends.");
}

// The Array without Nora: Delta-1 works the console from her notes and answers the last signal.
void checkArrayWithoutNora(const Assets& assets, mission_detail::MissionHost& host, const game_detail::World& world,
                           game_detail::Navigation& nav) {
    using namespace mission_rules;
    using mission_detail::MissionStart;
    View view;
    mission_detail::MissionController controller(assets, world, nav);
    Snapshot snap{};
    snap.difficulty = 3;
    snap.match = Match::Playing;
    Vec3 start{};
    float yaw = 0;
    controller.seedChapterReplay(MissionChapter::Array, false);
    controller.start(53, 3, MissionStart::NextChapter, snap, start, yaw, host);
    controller.publish(view);
    check(view->chapter == MissionChapter::Array && !view->actors[1].active && std::string_view(view->radioSpeaker) == "DELTA-1",
          "Without Nora, The Array must open on Delta-1's voice.");
    place(snap, start);
    clearAll(controller, snap, host, view);
    controller.step(snap, {}, tickSeconds, false, host);
    controller.publish(view);
    place(snap, mission_detail::mapPoint(368, 404));
    for (int t = 0; t < 10 * 60 && view->operation.stage == OperationStage::Hall; ++t) {
        controller.step(snap, {}, tickSeconds, false, host);
        if (t > 30) clearAll(controller, snap, host, view);
        controller.publish(view);
    }
    place(snap, mission_detail::mapPoint(368, 34));
    for (int t = 0; t < 4 * 60 && view->operation.stage == OperationStage::Door; ++t) {
        controller.step(snap, {}, tickSeconds, false, host);
        controller.publish(view);
    }
    check(view->operation.stage == OperationStage::Steer, "Without Nora, the dish must come under the pointer at the door.");
    for (int t = 0; t < 150 * 60 && view->operation.stage == OperationStage::Steer && snap.match == Match::Playing; ++t) {
        const Vec3 eye = snap.camera.position, saddle = view->operation.saddle;
        // Swing out north first so the beam passes behind the clearing.
        const Vec3 aim = view->operation.ship.z < 30.f && view->operation.ship.x > -12.f ? mission_detail::mapPoint(300, -600) : saddle;
        const Vec3 flat = normalized(Vec3{aim.x - eye.x, 0, aim.z - eye.z});
        lookAt(snap, eye + flat * 10.f - Vec3{0, eye.y / 41.f * 10.f, 0});
        clearAll(controller, snap, host, view);
        controller.step(snap, Input{0, 0, 0, 0, Signal, Signal}, tickSeconds, false, host);
        controller.publish(view);
    }
    check(view->operation.stage == OperationStage::Commit && !view->operation.clearingBurned,
          "Without Nora, the beam must still be steerable around the clearing to the saddle.");
    for (int t = 0; t < 10 * 60 && view->operation.stage != OperationStage::Reunite; ++t) {
        controller.step(snap, {}, tickSeconds, false, host);
        controller.publish(view);
    }
    controller.step(snap, {}, tickSeconds, false, host);
    controller.publish(view);
    check(std::string_view(view->signalHint) == "SIGNAL DELTA-1", "Without Nora, the last signal must be for Delta-1.");
    controller.step(snap, Input{0, 0, 0, 0, Signal, Signal}, tickSeconds, false, host);
    bool line = false;
    for (int t = 0; t < 15 * 60 && snap.match == Match::Playing; ++t) {
        controller.step(snap, {}, tickSeconds, false, host);
        controller.publish(view);
        line |= std::string_view(view->radioLine) == "THAT WAS HER IDEA.";
    }
    check(snap.match == Match::Results && view->phase == MissionPhase::Victory && line &&
              std::string_view(view->epilogue) == "THE FLEET SET DOWN BEYOND THE RIDGE. DUSTOFF CAME BACK FOR US.",
          "Without Nora, The Array must still end, on Delta-1's line about her.");
}

void checkBenAndDish(const Assets& assets, mission_detail::MissionHost& host, const game_detail::World& world,
                     game_detail::Navigation& nav) {
    using namespace mission_rules;
    using mission_detail::MissionStart;
    using mission_detail::mapPoint;
    // Ben comes across when waved over from a distance, running to the player first.
    {
        View view;
        mission_detail::MissionController controller(assets, world, nav);
        Snapshot snap{};
        snap.difficulty = 3;
        Vec3 start{};
        float yaw = 0;
        controller.start(61, 3, MissionStart::SingleMission, snap, start, yaw, host);
        place(snap, start);
        for (int t = 0; t < 60; ++t) controller.step(snap, {}, tickSeconds, false, host);
        controller.step(snap, Input{0, 0, 0, 0, Jump, Jump}, tickSeconds, false, host);
        for (int t = 0; t < 60; ++t) controller.step(snap, {}, tickSeconds, false, host);
        controller.step(snap, Input{0, 0, 0, 0, Jump, Jump}, tickSeconds, false, host);
        controller.step(snap, {}, tickSeconds, true, host);
        for (int t = 0; t < 360; ++t) {
            clearAll(controller, snap, host, view);
            controller.step(snap, {}, tickSeconds, false, host);
        }
        const Vec3 ben = mapPoint(920, 445);
        // Somewhere in sight of him, a long wave away.
        Vec3 spot{};
        for (int n = 0; n < nav.size(); ++n) {
            if (!nav.valid(n)) continue;
            const Vec3 at = nav.node(n).position;
            const float range = planar(at - ben);
            if (range > 12.f && range < 16.f && !world.blocked(atHeight(at, 1.7f), atHeight(ben, .9f))) {
                spot = at;
                break;
            }
        }
        check(planar(spot - ben) > 12.f, "East Ridge must have ground in sight of Ben at a distance.");
        place(snap, spot);
        controller.step(snap, {}, tickSeconds, false, host);
        clearAll(controller, snap, host, view);
        for (int t = 0; t < 120; ++t) controller.step(snap, {}, tickSeconds, false, host);
        controller.publish(view);
        check(view->signalReady && std::string_view(view->signalHint) == "SIGNAL BEN" &&
                  view->actors[1].civilian == CivilianOutcome::Pinned,
              "With Ben in sight, Signal must offer to wave him over and he must wait until it is used.");
        controller.step(snap, Input{0, 0, 0, 0, Signal, Signal}, tickSeconds, false, host);
        controller.publish(view);
        const float gap = planar(view->actors[1].actor.position - snap.player.position);
        for (int t = 0; t < 90; ++t) controller.step(snap, {}, tickSeconds, false, host);
        controller.publish(view);
        check(view->actors[1].civilian == CivilianOutcome::Evacuating && view->civiliansSaved == 1 &&
                  planar(view->actors[1].actor.position - snap.player.position) < gap - 3.f &&
                  std::string_view(view->radioLine) == "MY MOM WENT UP NORTH AVENUE. IS SHE OKAY?",
              "Ben must run toward the player who waved, asking after his mother.");
    }
    // On North Avenue, each pulse from Nora's dish breaks the drone's link to the Enforcer.
    {
        View view;
        mission_detail::MissionController controller(assets, world, nav);
        Snapshot snap{};
        snap.difficulty = 5;
        Vec3 start{};
        float yaw = 0;
        controller.start(62, 5, MissionStart::SingleMission, snap, start, yaw, host);
        place(snap, start);
        for (int t = 0; t < 60; ++t) controller.step(snap, {}, tickSeconds, false, host);
        controller.step(snap, Input{0, 0, 0, 0, Jump, Jump}, tickSeconds, false, host);
        for (int t = 0; t < 60; ++t) controller.step(snap, {}, tickSeconds, false, host);
        controller.step(snap, Input{0, 0, 0, 0, Jump, Jump}, tickSeconds, false, host);
        controller.step(snap, {}, tickSeconds, true, host);
        for (int t = 0; t < 360; ++t) {
            clearAll(controller, snap, host, view);
            controller.step(snap, {}, tickSeconds, false, host);
        }
        place(snap, mapPoint(400, -215));
        int pulses = 0, enforcer = -1;
        bool brokeShield = false, droneHeld = true, linkedBefore = false;
        float lastPulse = 0.f;
        for (int t = 0; t < 16 * 60; ++t) {
            controller.step(snap, {}, tickSeconds, false, host);
            controller.publish(view);
            for (int i = 4; i < int(view->actors.size()); ++i) {
                const auto& a = view->actors[size_t(i)];
                if (a.active && !a.dead && a.role == MissionRole::AlienTrooper && a.variant > 0) enforcer = i;
            }
            if (enforcer >= 0 && view->actors[size_t(enforcer)].linked) linkedBefore = true;
            if (view->dishPulse > lastPulse + .5f) {
                ++pulses;
                if (enforcer >= 0) brokeShield |= linkedBefore && view->actors[size_t(enforcer)].shield == 0.f;
                // The drone holds its fire for the moment it is turned toward the house.
                for (int w = 0; w < 60; ++w) {
                    controller.step(snap, {}, tickSeconds, false, host);
                    controller.publish(view);
                    for (int i = 4; i < int(view->actors.size()); ++i) {
                        const auto& a = view->actors[size_t(i)];
                        if (a.active && !a.dead && a.role == MissionRole::AlienDrone && a.gunRecoil > .9f) droneHeld = false;
                    }
                }
            }
            lastPulse = view->dishPulse;
        }
        check(pulses >= 2 && brokeShield && droneHeld,
              "Nora's dish must pulse on a timer, drop the Enforcer's linked shield, and hold the drone's fire.");
    }
}

// Rules checked directly on the mission controller, without the full game around it.
void checkMissionRules(const Assets& assets) {
    using namespace game_detail;
    using mission_detail::MissionStart;
    struct QuietHost final : mission_detail::MissionHost {
        int shocks = 0, playerHits = 0;
        void playSound(Sound sound, float, bool, Vec3) override { shocks += sound == Sound::PlasmaShock; }
        void damagePlayer(float, Vec3) override { ++playerHits; }
        void hitWorld(const ShotHit&, Vec3) override {}
        void updateActorFrame(Actor&) override {}
        void refillPlayer(bool) override {}
    } host;
    const World world(assets);
    Navigation nav(assets);
    const auto point = [](float x, float y) { return Vec3{(x - 316) * .05f, 0.f, (212.5f - y) * .05f}; };
    std::shared_ptr<const MissionSnapshot> view;
    // One press moves the title card on to the wake-up, a second skips the wake-up.
    const auto skipIntro = [&](mission_detail::MissionController& controller, Snapshot& snap) {
        for (int t = 0; t < 60; ++t) controller.step(snap, {}, tickSeconds, false, host);
        controller.step(snap, Input{0, 0, 0, 0, Jump, Jump}, tickSeconds, false, host);
        for (int t = 0; t < 60; ++t) controller.step(snap, {}, tickSeconds, false, host);
        controller.step(snap, Input{0, 0, 0, 0, Jump, Jump}, tickSeconds, false, host);
    };
    const auto clearAliens = [&](mission_detail::MissionController& controller, Snapshot& snap) {
        controller.publish(view);
        for (int i = 4; i < int(view->actors.size()); ++i) {
            const auto& a = view->actors[i];
            if (!a.active || a.dead) continue;
            const Vec3 aim = a.actor.position + Vec3{0, a.role == MissionRole::AlienDrone ? 0.f : 1.25f, 0};
            Vec3 hit{};
            check(controller.apacheShotAlien(i, aim + Vec3{0, 0, 2}, {0, 0, -1}, 3.f, 1000.f, snap, host, hit),
                  "The mission rules fixture must be able to clear visible hostiles.");
        }
    };

    // The West Timberline route can stay quiet, or resume after the player is spotted.
    for (bool detected : {false, true}) {
        mission_detail::MissionController controller(assets, world, nav);
        Snapshot snap{};
        snap.difficulty = 3;
        Vec3 start{};
        float yaw = 0;
        controller.start(42, 3, MissionStart::SingleMission, snap, start, yaw, host);
        snap.player.position = start;
        skipIntro(controller, snap);
        controller.step(snap, {}, tickSeconds, true, host);
        for (int t = 0; t < 360; ++t) {
            clearAliens(controller, snap);
            controller.step(snap, {}, tickSeconds, false, host);
        }
        controller.publish(view);
        check(view->phase == MissionPhase::Rescue, "The Walt fixture must reach the rescue sweep.");
        check(view->waypointIsRoute && !world.walkBlocked(snap.player.position, view->waypoint),
              "From Briar House, the waypoint must lead to a visible street approach.");
        using namespace mission_detail;
        const auto movePlayer = [&](Vec3 pos) {
            snap.player.position = pos;
            snap.camera.position = atHeight(pos, .95f);
        };
        snap.crouch = true;
        snap.eyeHeight = .95f;
        const Vec3 pickup = waltPickupApproach;
        check(world.canStand(pickup, snap.eyeHeight) && world.canStand(waltCoverRoute[std::size(waltCoverRoute) - 1], 1.7f),
              "Both ends of the Timberline escort must be usable.");
        Vec3 from = view->actors[3].actor.position;
        for (const Vec3 cover : waltCoverRoute) {
            check(!world.walkBlocked(from, cover), "The Timberline escort must have a clear route between cover points.");
            from = cover;
        }
        movePlayer(pickup);
        controller.step(snap, {}, tickSeconds, false, host);
        controller.publish(view);
        check(view->actors[3].civilian == CivilianOutcome::Escorting && view->civiliansSaved == 0,
              "Walt can start the low approach while the patrol is unalerted.");
        const Vec3 cover = view->actors[3].actor.position;
        movePlayer(mapPoint(-159, -82));
        for (int t = 0; t < 20; ++t) controller.step(snap, {}, tickSeconds, false, host);
        controller.publish(view);
        check(planar(view->actors[3].actor.position - cover) < .05f &&
                  std::string_view(view->subObjective).find("FOLLOW WALT") != std::string_view::npos,
              "Walt must wait for the player while guidance stays on the engaged district.");
        movePlayer(pickup);
        if (detected) {
            controller.step(snap, {}, tickSeconds, true, host);
            controller.publish(view);
            check(std::string_view(view->subObjective).find("CLEAR THE PATROL") != std::string_view::npos,
                  "Alerting the patrol during the escort must switch the objective to combat.");
            clearAliens(controller, snap);
        }
        bool waited = false;
        for (int t = 0; t < 1200; ++t) {
            controller.publish(view);
            waited |= std::string_view(view->radioLine) == "WAIT.";
            if (civilianRescued(view->actors[3].civilian)) break;
            movePlayer(view->actors[3].actor.position + Vec3{2.6f, 0, .7f});
            controller.step(snap, {}, tickSeconds, false, host);
        }
        controller.publish(view);
        check(civilianRescued(view->actors[3].civilian) && view->civiliansSaved == 1 &&
                  view->aliensRemaining == (detected ? 0 : 2),
              "Walt must reach the second shelter both quietly and after a fight.");
        check(detected || waited, "On the quiet route, Walt must wait while the scanner sweeps across him.");
    }

    // Dustoff in the clearing: a signal under watch draws fire, a clear one sends the family across.
    {
        using namespace mission_detail;
        MissionController controller(assets, world, nav);
        Snapshot snap{};
        snap.difficulty = 3;
        snap.match = Match::Playing;
        Vec3 start{};
        float yaw = 0;
        controller.start(44, 3, MissionStart::Campaign, snap, start, yaw, host);
        controller.seedExtractionReplay();
        controller.start(44, 3, MissionStart::Retry, snap, start, yaw, host);
        snap.player.position = mapPoint(210, -215);
        snap.camera.position = atHeight(snap.player.position, 1.7f);
        // Hold the clearing without fighting, so the vanguard is still out there when the bird lands.
        for (int t = 0; t < 60 * 70; ++t) {
            controller.step(snap, {}, tickSeconds, false, host);
            controller.publish(view);
            if (view->phase == MissionPhase::Boarding && view->signalReady) break;
        }
        check(view->phase == MissionPhase::Boarding && view->evacVisible && view->aliensRemaining > 0 &&
                  std::string_view(view->signalHint) == "SEND THEM TO DUSTOFF",
              "Dustoff must come down while the fight is still going and wait for the player's signal.");
        float before = 0;
        for (int c = 1; c <= 3; ++c) before += view->actors[size_t(c)].actor.health;
        controller.step(snap, Input{0, 0, 0, 0, Signal, Signal}, tickSeconds, false, host);
        for (int t = 0; t < 90; ++t) controller.step(snap, {}, tickSeconds, false, host);
        controller.publish(view);
        float after = 0;
        for (int c = 1; c <= 3; ++c) after += view->actors[size_t(c)].actor.health;
        check(view->civiliansBoarded == 0 && view->actors[nora + 1].civilian == CivilianOutcome::Sheltered && after < before,
              "Signalling while hostiles watch the wall must draw fire and keep everyone at the wall.");
        for (int t = 0; t < 60; ++t) {
            clearAliens(controller, snap);
            controller.step(snap, {}, tickSeconds, false, host);
        }
        controller.step(snap, Input{0, 0, 0, 0, Signal, Signal}, tickSeconds, false, host);
        controller.publish(view);
        check(view->actors[nora + 1].civilian == CivilianOutcome::Stayed &&
                  std::string_view(view->radioLine) == "TAKE MY FATHER AND MY SON. I'M STAYING WITH THE DISH.",
              "With the clearing quiet, Nora must refuse the bird and send her father and son.");
        for (int t = 0; t < 60 * 20 && snap.match == Match::Playing; ++t) controller.step(snap, {}, tickSeconds, false, host);
        controller.publish(view);
        check(snap.match == Match::Results && view->phase == MissionPhase::Victory && view->civiliansBoarded == 2 &&
                  view->civiliansSaved == 3 && view->nextMissionReady(),
              "Boarding Ben and Walt must lift Dustoff and complete Briar House.");
    }

    // A survivor lost after rescue, a boarding path the actors cannot cross, and Timberline without Nora.
    {
        using namespace mission_detail;
        Assets blocked = assets;
        blocked.obstacles.push_back({exfilHover.x, exfilHover.z - 1.5f, 3.f, .75f, 0.f, 4.f, 0.f, 1.f, 0.f});
        const World blockedWorld(blocked);
        Navigation blockedNav(blocked);
        MissionController controller(blocked, blockedWorld, blockedNav);
        Snapshot snap{};
        snap.difficulty = 3;
        snap.match = Match::Playing;
        Vec3 start{};
        float yaw = 0;
        controller.start(45, 3, MissionStart::Campaign, snap, start, yaw, host);
        controller.seedExtractionReplay();
        controller.start(45, 3, MissionStart::Retry, snap, start, yaw, host);
        snap.player.position = start;
        snap.camera.position = atHeight(start, 1.7f);
        const Vec3 noraPos = mapPoint(evacSpots[nora][0], evacSpots[nora][1]);
        for (int round = 0; round < 3; ++round) {
            const Vec3 eye = atHeight(noraPos, 1.f) + Vec3{0, 0, -1.2f};
            Vec3 hit{};
            check(controller.resolvePlayerShot(eye, {0, 0, 1}, 40.f, 2.f, snap, host, hit),
                  "The loss fixture must hit Nora at the shelter.");
            for (int t = 0; t < 60; ++t) controller.step(snap, {}, tickSeconds, false, host);
        }
        controller.publish(view);
        check(view->actors[nora + 1].civilian == CivilianOutcome::Lost &&
                  view->civiliansLost == 1 && view->civiliansSaved == 2,
              "A loss after rescue must immediately reduce the number saved.");
        for (int t = 0; t < 60 * 70 && !view->signalReady; ++t) {
            clearAliens(controller, snap);
            controller.step(snap, {}, tickSeconds, false, host);
            controller.publish(view);
        }
        check(view->phase == MissionPhase::Boarding && view->signalReady,
              "A loss in the family must still bring Dustoff for the other survivors.");
        controller.step(snap, Input{0, 0, 0, 0, Signal, Signal}, tickSeconds, false, host);
        for (int t = 0; t < 4 * 60; ++t) {
            clearAliens(controller, snap);
            controller.step(snap, {}, tickSeconds, false, host);
        }
        controller.publish(view);
        check(!view->cinematic && view->civiliansBoarded == 0,
              "The obstruction must keep Ben and Walt out of the cabin for a while.");
        for (int t = 0; t < 14 * 60 && snap.match == Match::Playing; ++t) controller.step(snap, {}, tickSeconds, false, host);
        controller.publish(view);
        check(snap.match == Match::Results && view->civiliansBoarded == 2 && view->civiliansLost == 1 &&
                  std::string_view(view->epilogue) == "BEN AND WALT FLEW OUT. WE LOST NORA.",
              "A blocked boarding path must still resolve, and the epilogue must say who went and who was lost.");
        auto next = controller;
        controller.start(45, 3, MissionStart::Retry, snap, start, yaw, host);
        controller.publish(view);
        check(view->phase == MissionPhase::Incoming && view->civiliansSaved == 3 &&
                  view->civiliansLost == 0 && view->civiliansBoarded == 0,
              "Retrying must restore the civilian outcomes saved before the hold.");

        // Without Nora, Delta-1 finishes her work from her notes.
        snap = {};
        snap.match = Match::Playing;
        next.start(46, 3, MissionStart::NextChapter, snap, start, yaw, host);
        next.publish(view);
        check(view->chapter == MissionChapter::Timberline && !view->actors[1].active &&
                  std::string_view(view->objective) == "GET TO TIMBERLINE" && std::string_view(view->radioSpeaker) == "DELTA-1",
              "Losing Nora must send Delta-1 to Timberline alone, working from her notes.");
        checkTimberlineRules(next, snap, host, view, blockedWorld, blockedNav);
    }

    // A stray MP44 burst through a civilian wounds them but cannot kill; sustained fire still does.
    {
        mission_detail::MissionController controller(assets, world, nav);
        Snapshot snap{};
        Vec3 start{};
        float yaw = 0;
        controller.start(7, 5, MissionStart::SingleMission, snap, start, yaw, host);
        const Vec3 civ = point(920, 445);
        const Vec3 eye = civ + Vec3{-2.5f, 1.2f, 0.f};
        const Vec3 dir = normalized(atHeight(civ, 1.f) - eye);
        Vec3 hit{};
        for (int round = 0; round < 4; ++round) {
            controller.resolvePlayerShot(eye, dir, 32.f, 100.f, snap, host, hit);
            for (int t = 0; t < 6; ++t) controller.step(snap, {}, tickSeconds, false, host);
        }
        controller.publish(view);
        check(!view->actors[1].dead && view->actors[1].actor.health < 85.f && view->friendlyFireWarn,
              "One burst through a civilian must wound and warn, not kill.");
        for (int round = 0; round < 6 && !view->actors[1].dead; ++round) {
            controller.resolvePlayerShot(eye, dir, 32.f, 100.f, snap, host, hit);
            for (int t = 0; t < 60; ++t) controller.step(snap, {}, tickSeconds, false, host);
            controller.publish(view);
        }
        check(view->actors[1].dead && view->civiliansLost == 1, "Deliberate repeated fire must still kill a civilian.");
    }

    // Once the shooting starts, the road patrol runs to the front wall and vaults into the grounds.
    {
        mission_detail::MissionController controller(assets, world, nav);
        Snapshot snap{};
        snap.difficulty = 5;
        Vec3 start{};
        float yaw = 0;
        controller.start(9, 5, MissionStart::SingleMission, snap, start, yaw, host);
        snap.player.position = point(368, 300);
        snap.eyeHeight = 1.7f;
        snap.camera.position = atHeight(snap.player.position, 1.7f);
        skipIntro(controller, snap);
        controller.step(snap, {}, tickSeconds, true, host); // Opening shot goes wide
        controller.publish(view);
        check(view->phase == MissionPhase::Breach, "Firing on the overwatch must start the breach.");
        float highest = 0;
        for (int t = 0; t < 8 * 60; ++t) {
            controller.step(snap, {}, tickSeconds, false, host);
            controller.publish(view);
            for (int i = 4; i < 7; ++i) {
                if (view->actors[i].role == MissionRole::AlienTrooper) highest = std::max(highest, view->actors[i].actor.position.y);
            }
        }
        int inside = 0;
        for (int i = 4; i < 7; ++i) inside += view->actors[i].actor.position.z > (212.5f - 515.f) * .05f;
        check(inside >= 2 && highest > 1.2f,
              "The road patrol must push through the breach and vault the flanking front wall into the grounds.");

        // Inside, they take firing spots around the player instead of queueing up in one lane.
        for (int t = 0; t < 4 * 60; ++t) controller.step(snap, {}, tickSeconds, false, host);
        controller.publish(view);
        std::vector<float> bearings;
        for (int i = 4; i < int(MissionSnapshot::maxActors); ++i) {
            const auto& a = view->actors[i];
            if (!a.active || a.dead) continue;
            bearings.push_back(std::atan2(a.actor.position.x - snap.player.position.x, a.actor.position.z - snap.player.position.z));
        }
        float widest = 0;
        for (float b0 : bearings)
            for (float b1 : bearings) {
                float d = std::fmod(std::abs(b0 - b1), 2 * pi);
                widest = std::max(widest, std::min(d, 2 * pi - d));
            }
        check(bearings.size() >= 2 && widest > 50 * pi / 180, "Aliens must spread out to flank the player.");


    }

    // A trooper knocked below half health breaks line of sight before coming back. The same
    // deterministic fight runs twice, with and without the wounding shot, and the wounded run must
    // stay hidden at least a second longer. Difficulty 10 gives it the health to survive Delta-1.
    {
        const auto hiddenStreak = [&](bool wound) {
            mission_detail::MissionController controller(assets, world, nav);
            Snapshot snap{};
            snap.difficulty = 10;
            Vec3 start{};
            float yaw = 0;
            controller.start(9, 10, MissionStart::SingleMission, snap, start, yaw, host);
            snap.player.position = point(368, 300);
            snap.eyeHeight = 1.7f;
            snap.camera.position = atHeight(snap.player.position, 1.7f);
            skipIntro(controller, snap);
            controller.step(snap, {}, tickSeconds, true, host);
            for (int t = 0; t < 12 * 60; ++t) controller.step(snap, {}, tickSeconds, false, host);
            controller.publish(view);
            const Vec3 playerEye = atHeight(snap.player.position, snap.eyeHeight);
            int hurt = -1;
            for (int i = 4; i < int(MissionSnapshot::maxActors) && hurt < 0; ++i) {
                const auto& a = view->actors[i];
                if (a.active && !a.dead && a.role == MissionRole::AlienTrooper && a.actor.health > 120.f &&
                    !world.blocked(atHeight(a.actor.position, 1.8f), playerEye)) {
                    hurt = i;
                }
            }
            check(hurt >= 0, "A healthy trooper must hold a firing spot with sight of the player.");
            if (wound) {
                Vec3 hit{};
                const Vec3 legs = atHeight(view->actors[hurt].actor.position, .6f);
                controller.resolvePlayerShot(playerEye, normalized(legs - playerEye), 80.f, 100.f, snap, host, hit);
            }
            int hiddenFor = 0, longest = 0;
            for (int t = 0; t < 5 * 60; ++t) {
                controller.step(snap, {}, tickSeconds, false, host);
                controller.publish(view);
                const auto& a = view->actors[hurt];
                const bool hidden = a.active && !a.dead && world.blocked(atHeight(a.actor.position, 1.8f), playerEye);
                hiddenFor = hidden ? hiddenFor + 1 : 0;
                longest = std::max(longest, hiddenFor);
            }
            return longest;
        };
        const int wounded = hiddenStreak(true), unhurt = hiddenStreak(false);
        check(wounded >= unhurt + 60, "A badly hurt trooper must fall back out of sight before re-engaging.");
    }

    // Delta-1 steps out of the player's firing lane and stays out while his post is in it.
    {
        mission_detail::MissionController controller(assets, world, nav);
        Snapshot snap{};
        snap.difficulty = 5;
        Vec3 start{};
        float yaw = 0;
        controller.start(8, 5, MissionStart::SingleMission, snap, start, yaw, host);
        skipIntro(controller, snap);
        controller.publish(view);
        check(view->phase == MissionPhase::Overwatch, "Jump must skip the wake-up intro on the controller.");
        // Stand east of his Grand Hall post and aim straight through it.
        const Vec3 post = point(322, 313);
        snap.player.position = point(440, 313);
        snap.eyeHeight = 1.7f;
        snap.camera.position = atHeight(snap.player.position, 1.7f);
        const Vec3 toPost = atHeight(post, 1.3f) - snap.camera.position;
        snap.camera.yaw = std::atan2(toPost.x, toPost.z);
        snap.camera.pitch = 0.f;
        const Vec3 aim = forward(snap.camera.yaw, 0.f);
        const auto laneDistance = [&](Vec3 at) {
            const Vec3 rel = atHeight(at, 1.3f) - snap.camera.position;
            const float along = dot(rel, aim);
            return planar(rel - aim * along);
        };
        float closest = 1e9f;
        for (int t = 0; t < 600; ++t) {
            controller.step(snap, {}, tickSeconds, false, host);
            controller.publish(view);
            if (t > 180) closest = std::min(closest, laneDistance(view->actors[0].actor.position));
        }
        check(closest > 0.9f, "Delta-1 must clear the player's firing lane instead of standing in it.");
        // Look away and he returns to his post.
        snap.camera.yaw += pi / 2;
        for (int t = 0; t < 360; ++t) controller.step(snap, {}, tickSeconds, false, host);
        controller.publish(view);
        check(planar(view->actors[0].actor.position - post) < 1.2f, "Delta-1 must return to his post once the lane is clear.");
    }

    // The objective marker sits over a visible target and rides the correct screen edge otherwise.
    {
        constexpr auto rgb = [](unsigned r, unsigned g, unsigned b) { return uint16_t((r >> 3) << 11 | (g >> 2) << 5 | (b >> 3)); };
        const uint16_t accent = rgb(212, 223, 110), muted = rgb(167, 177, 162);
        auto objective = std::make_shared<MissionSnapshot>();
        objective->active = true;
        objective->phase = MissionPhase::Rescue;
        objective->hasWaypoint = true;
        Snapshot snap{};
        snap.match = Match::Playing;
        snap.camera = {{0, 1.7f, 0}, 0.f, 0.f, 70 * pi / 180};
        snap.player.position = {0, 0, 0};
        snap.mission = objective;
        MenuState menu{};
        std::vector<uint16_t> fb(800 * 480);
        const auto count = [&](int x0, int x1, int y0, int y1, uint16_t color) {
            int hits = 0;
            for (int y = y0; y < y1; ++y)
                for (int x = x0; x < x1; ++x) hits += fb[size_t(y) * 800 + x] == color;
            return hits;
        };
        objective->waypoint = {3.f, 0.f, 10.f}; // Ahead and to the right: about x 496 on screen
        std::fill(fb.begin(), fb.end(), 0);
        drawHud(assets, snap, menu, fb.data(), false);
        check(count(480, 513, 228, 253, accent) > 8, "The objective marker must sit over a target in view.");
        // Subtitles stay on screen with radio sound switched off: they carry the instructions.
        objective->radioTimer = 2.f;
        std::snprintf(objective->radioLine, sizeof(objective->radioLine), "%s", "CLEAR THE KITCHEN");
        menu.voice = false;
        std::fill(fb.begin(), fb.end(), 0);
        drawHud(assets, snap, menu, fb.data(), false);
        const uint16_t speakerTeal = rgb(119, 211, 183);
        check(count(148, 152, 342, 390, speakerTeal) > 100, "Mission subtitles must show even with radio sound off.");
        objective->radioTimer = 0;
        menu.voice = true;
        objective->waypoint = {-5.f, 0.f, -5.f}; // Behind and to the left
        std::fill(fb.begin(), fb.end(), 0);
        drawHud(assets, snap, menu, fb.data(), false);
        check(count(20, 64, 150, 300, muted) > 8 && count(736, 780, 150, 300, muted) == 0,
              "An objective behind and to the left must pin the marker to the left edge.");
    }

    // The chest weak point, visible plasma shots and drone shield links share this encounter.
    {
        mission_detail::MissionController controller(assets, world, nav);
        Snapshot snap{};
        snap.difficulty = 5;
        Vec3 start{};
        float yaw = 0;
        controller.start(17, 5, MissionStart::SingleMission, snap, start, yaw, host);
        snap.player.position = point(368, 340);
        snap.eyeHeight = 1.7f;
        snap.camera.position = atHeight(snap.player.position, 1.7f);
        skipIntro(controller, snap);
        controller.publish(view);
        const float overwatchMist = view->mistDensity;
        const auto& trooper = view->actors[4].actor;
        const Vec3 front = forward(trooper.yaw, 0);
        const Vec3 chest = atHeight(trooper.position, alienWeakPoint.y);
        Vec3 hit{};
        const bool emptyAbove = controller.resolvePlayerShot(chest + front * 2.f + Vec3{0, .48f, 0},
                                                             front * -1.f, 70.f, 100.f, snap, host, hit);
        check(!emptyAbove, "Empty space above a trooper's shoulders must not register as a head hit.");
        const bool struck = controller.resolvePlayerShot(chest + front * 2.f, front * -1.f, 70.f, 100.f, snap, host, hit);
        controller.publish(view);
        check(struck && view->actors[4].dead && view->ambushKills == 1 && view->criticalKills == 1,
              "An unalerted chest weak-point hit in Overwatch must award an Ambush Kill.");

        bool sawCharge = false, sawPlasma = false, sawDroneLink = false, sawAlignedMuzzle = false;
        int linkedDrone = -1, linkedTrooper = -1;
        for (int t = 0; t < 12 * 60; ++t) {
            controller.step(snap, {}, tickSeconds, false, host);
            controller.publish(view);
            for (int i = 4; i < int(MissionSnapshot::maxActors); ++i) {
                const auto& a = view->actors[i];
                if (!a.active || a.dead) continue;
                if (a.gunCharge > 0.1f) sawCharge = true;
                if (a.role == MissionRole::AlienTrooper && a.linked && a.shield > 0.f) {
                    sawDroneLink = true;
                    linkedTrooper = i;
                }
                if (a.role == MissionRole::AlienDrone) linkedDrone = i;
            }
            for (const auto& cloud : view->plasma) {
                if (cloud.radius <= 0.f) continue;
                sawPlasma = true;
                for (size_t i = 4; i < view->actors.size(); ++i) {
                    const auto& ma = view->actors[i];
                    if (!ma.active || ma.dead || ma.role != MissionRole::AlienTrooper || ma.gunRecoil < .99f) continue;
                    const auto p = alienGunSocket(assets, ma.actor) * alienScale(ma.variant);
                    const Vec3 fwd = forward(ma.actor.yaw, 0), right{fwd.z, 0, -fwd.x};
                    const auto gun = alienGunFrame(ma.actor.position + right * p.x + Vec3{0, p.y, 0} + fwd * p.z,
                                                   ma.gunYaw, ma.gunPitch, ma.variant);
                    if (length(cloud.position - gun.muzzle(ma.variant)) < .002f &&
                        dot(normalized(gun.axis), cloud.direction) > .9999f) sawAlignedMuzzle = true;
                }
            }
            if (sawCharge && sawPlasma && sawDroneLink && sawAlignedMuzzle) break;
        }
        check(sawCharge && sawPlasma && sawAlignedMuzzle,
              "A charging alien must launch a visible cloud straight through the rendered muzzle.");
        check(sawDroneLink && linkedDrone >= 4 && linkedTrooper >= 4,
              "An alerted Scout Drone must project a Shield Link to a nearby ground alien.");
        check(view->mistDensity > overwatchMist, "Dusk mist must thicken as the mission progresses past Overwatch.");

        // Destroying the linking Drone must trigger EMP disruption, shattering the linked alien's shield and staggering it.
        const Vec3 dPos = view->actors[linkedDrone].actor.position;
        controller.resolvePlayerShot(dPos + Vec3{0, 0, -2.f}, {0, 0, 1}, 200.f, 100.f, snap, host, hit);
        controller.publish(view);
        check(view->actors[linkedDrone].dead && view->actors[linkedTrooper].shield == 0.f &&
                  view->actors[linkedTrooper].stagger > 0.5f,
              "Destroying a linking Scout Drone must EMP-disrupt its linked ally's shield and stagger it.");

        // Put the player in the path of a cloud in flight and verify the hit and shock.
        bool intercepted = false;
        for (int t = 0; t < 480 && !intercepted; ++t) {
            controller.step(snap, {}, tickSeconds, false, host);
            controller.publish(view);
            for (const auto& cloud : view->plasma) {
                if (cloud.radius <= 0.f || world.shot(cloud.position, cloud.direction, snap.brokenGlass).distance < .8f) continue;
                snap.player.position = cloud.position + cloud.direction * .82f - Vec3{0, .85f, 0};
                snap.camera.position = atHeight(snap.player.position, snap.eyeHeight);
                const int hitsBefore = host.playerHits;
                const int shocksBefore = host.shocks;
                controller.step(snap, {}, tickSeconds, false, host);
                controller.publish(view);
                intercepted = host.playerHits > hitsBefore && view->plasmaShock > .6f;
                if (intercepted && shocksBefore == 0) check(host.shocks == 1, "A first plasma hit must sound like an electrical discharge.");
                break;
            }
        }
        check(intercepted, "A flying plasma cloud must damage and electrically shock the player on contact.");
    }
    checkBenAndDish(assets, host, world, nav);
    checkArrayRules(assets, host, world, nav);
    checkArrayWithoutNora(assets, host, world, nav);
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
#ifdef FOD_LOCAL_DUEL
    checkDesktopResolution(assets);
#endif
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
    const auto stockSky = assets.sky;
    const auto stockAudio = assets.audio.size();
    checkBriarHouse(assets);
    const auto standardMaterials = assets.materials.size(), standardTextures = assets.textures.size();
    checkBriarHouseMission(assets);
    checkMissionRules(assets);
    const auto invasionMaterials = assets.materials.size(), invasionTextures = assets.textures.size();
    check(assets.sky == stockSky, "Building the invasion must leave the source sky available for daylight rendering.");
    buildBriarHouse(assets, BriarHouseMode::Standard);
    check(assets.materials.size() == standardMaterials && assets.textures.size() == standardTextures &&
              assets.audio.size() == stockAudio && assets.alienTrooper.triangles.empty(),
          "Returning to the standard arena must release invasion assets and retain the pack's audio.");
    buildBriarHouse(assets, BriarHouseMode::Invasion);
    check(assets.materials.size() == invasionMaterials && assets.textures.size() == invasionTextures &&
              assets.residentBytes < 12u * 1024u * 1024u && assets.sky == stockSky,
          "Rebuilding the campaign must reuse the pack's original palette and stay within its memory budget.");
    std::puts("Viewport mapping, menus, radar, trigger latches, translucency, PCM mixing, ADPCM speech and Operation Briar passed.");
}

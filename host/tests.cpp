#include "assets.hpp"
#include "game_bot.hpp"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace {

using namespace fod;
using namespace fod::game_detail;

void require(bool condition, const char* test, int line) {
    if (condition) return;
    std::fprintf(stderr, "FAIL line %d: %s\n", line, test);
    std::exit(1);
}

#define CHECK(condition) require(bool(condition), #condition, __LINE__)

void ticks(Game& g, int count, Input in = {}) {
    while (count--) g.step(in);
}

int count(const Game& g, EventKind kind) {
    return int(std::count_if(g.events(), g.events() + g.eventCount(), [kind](const Event& e) { return e.kind == kind; }));
}

const Event* event(const Game& g, EventKind kind) {
    const auto end = g.events() + g.eventCount();
    const auto found = std::find_if(g.events(), end, [kind](const Event& e) { return e.kind == kind; });
    return found == end ? nullptr : found;
}

Input buttons(uint32_t held, uint32_t pressed = 0) {
    Input input;
    input.held = held;
    input.pressed = pressed;
    return input;
}

Fixture quiet() {
    Fixture f;
    f.freezeRival = true;
    return f;
}

void testWeapons(const Assets& assets) {
    Game g(assets);
    g.start(17, 4);
    g.setupFixture(quiet());
    std::vector<uint32_t> fireTicks;
    for (int i = 0; i < 31; ++i) {
        g.step(buttons(Fire, i == 0 ? Fire : 0u));
        if (count(g, EventKind::Fire)) fireTicks.push_back(g.state().tick);
    }
    CHECK((fireTicks == std::vector<uint32_t>{1, 7, 13, 19, 24, 30}));
    CHECK(g.state().clip == 24);
    g.step(buttons(Reload, Reload));
    const auto start = g.state().tick;
    CHECK(count(g, EventKind::ReloadStart) == 1);
    uint32_t seated = 0, completed = 0;
    for (int i = 0; i < 90; ++i) {
        g.step({});
        if (count(g, EventKind::ReloadRound)) seated = g.state().tick;
        if (count(g, EventKind::ReloadEnd)) completed = g.state().tick;
    }
    CHECK(seated == start + 56 && completed == start + 76);
    CHECK(g.state().clip == 30 && g.state().reserve == 84);

    g.step(buttons(Swap, Swap));
    for (int i = 0; i < 12; ++i) g.step({});
    CHECK(g.state().weapon == Weapon::Mp44);
    g.step({});
    CHECK(g.state().weapon == Weapon::Sniper && count(g, EventKind::Swap) == 1);
    ticks(g, 50, buttons(Aim));
    CHECK(g.state().ads == 1 && g.state().scope > .99f);
    g.step(buttons(Aim | Fire, Fire | Reload));
    CHECK(g.state().weaponState == WeaponState::Bolt);
    CHECK(count(g, EventKind::Fire) == 1 && count(g, EventKind::ReloadStart) == 0);
    CHECK(g.state().clip == 4);
    ticks(g, 73, buttons(Fire));
    CHECK(g.state().clip == 4 && g.state().weaponState == WeaponState::Idle);
    g.step(buttons(Reload, Reload));
    CHECK(count(g, EventKind::ReloadStart) == 1);
    ticks(g, 90);
    CHECK(g.state().clip == 5 && g.state().reserve == 14);

    g.step(buttons(Swap, Swap));
    ticks(g, 40);
    CHECK(g.state().weapon == Weapon::Pistol);
    g.step(buttons(Fire, Fire));
    ticks(g, 60, buttons(Fire));
    CHECK(g.state().clip == 11);
    g.step(buttons(Reload, Reload));
    ticks(g, 69);
    CHECK(g.state().clip == 12 && g.state().reserve == 35);
    g.step({});
    CHECK(g.state().weaponState == WeaponState::Idle);

    g.start(33, 3);
    Fixture lastRound = quiet();
    lastRound.clip = 1;
    g.setupFixture(lastRound);
    g.step(buttons(Fire, Fire));
    CHECK(g.state().clip == 0 && g.state().weaponState == WeaponState::Reload);
    CHECK(count(g, EventKind::ReloadStart) == 1);
    ticks(g, 98);
    CHECK(g.state().clip == 30 && g.state().reserve == 60 && g.state().weaponState == WeaponState::Idle);
}

void testMovementAndPause(const Assets& assets) {
    Game g(assets);
    g.start(23, 3);
    auto f = quiet();
    f.yaw = 0;
    g.setupFixture(f);
    Input move;
    move.moveY = 1;
    g.step(move);
    const auto* end = g.events() + g.eventCount();
    const auto* step = std::find_if(g.events(), end, [](const Event& e) {
        return e.kind == EventKind::Sound && e.sound == Sound::Footstep;
    });
    CHECK(step != end && std::abs(step->volume - .119f) < .001f);
    ticks(g, 11, move);
    CHECK(std::abs(g.state().player.position.z - (f.player.z + .8f)) < .001f);
    g.step(buttons(Jump, Jump));
    ticks(g, 18);
    CHECK(g.state().player.position.y > .78f && !g.state().grounded);
    ticks(g, 45);
    CHECK(g.state().player.position.y == 0 && g.state().grounded);
    g.step(buttons(Stance, Stance));
    g.step({});
    CHECK(g.state().crouch && !g.state().prone);
    ticks(g, 19, buttons(Stance));
    CHECK(!g.state().prone);
    g.step(buttons(Stance));
    CHECK(g.state().prone);
    g.step({});
    CHECK(g.state().crouch && !g.state().prone);

    Input lost = buttons(Fire, Fire);
    lost.disconnected = true;
    const auto tick = g.state().tick;
    g.step(lost);
    CHECK(g.state().match == Match::Paused && g.state().tick == tick && g.eventCount() == 0);
    g.step(buttons(Fire, Fire));
    CHECK(g.state().match == Match::Paused);
    g.resume();
    ticks(g, 4, buttons(Fire));
    CHECK(g.state().clip == 30);
    g.step({});
    g.step(buttons(Fire, Fire));
    CHECK(g.state().clip == 29);
    g.pause();
    g.quit();
    CHECK(g.state().match == Match::Results && count(g, EventKind::MatchEnd) == 1);
}

bool centerDraw(void*, uint32_t, const char*, double& out) {
    out = .5;
    return true;
}

void testGlass(const Assets& assets) {
    const World world(assets);
    for (size_t i = 0; i < assets.shotSurfaces.size(); ++i) {
        const auto& tri = assets.shotSurfaces[i];
        if (tri.glass < 0 || assets.glassHits[tri.glass] != 3) continue;
        const auto [a, b, c] = assets.shotCorners(i);
        const Vec3 center = (a + b + c) / 3;
        const Vec3 normal = normalized(cross(b - a, c - a));
        for (float side : {-1.f, 1.f}) {
            Fixture f = quiet();
            f.weapon = Weapon::Sniper;
            f.player = center + normal * (side * 2.3f);
            f.player.y = 0;
            if (std::abs(f.player.x) > 15.6f || std::abs(f.player.z) > 15.6f) continue;
            Vec3 check = f.player, velocity;
            world.move(check, velocity, float(tickSeconds), 1.85f, .4f);
            if (length(check - f.player) > .0001f) continue;
            const Vec3 eye = atHeight(f.player, 1.704f);
            const Vec3 aim = normalized(center - eye);
            f.yaw = std::atan2(aim.x, aim.z);
            f.pitch = -std::atan2(aim.y, planar(aim));
            const auto hit = world.shot(eye, aim, 0);
            const auto through = world.shot(eye, aim, uint64_t(1) << tri.glass);
            if (hit.glass != tri.glass || !through.hit || through.surface == hit.surface || through.distance - hit.distance < .02f) continue;
            Game g(assets);
            g.setRandomHook(centerDraw, nullptr);
            g.start(28, 3);
            g.setupFixture(f);
            bool finished = true;
            for (int i = 1; i <= 3; ++i) {
                g.step(buttons(Fire, Fire));
                const auto* at = event(g, EventKind::GlassHit);
                if (!at || at->value != i || at->surface != hit.surface) {
                    finished = false;
                    break;
                }
                CHECK(count(g, EventKind::GlassBreak) == (i == 3));
                ticks(g, 80);
            }
            if (!finished) continue;
            g.step(buttons(Fire, Fire));
            const auto* other = event(g, EventKind::WorldHit);
            if (!other || other->surface != through.surface) continue;
            CHECK((g.state().brokenGlass & (uint64_t(1) << tri.glass)) != 0);
            // The pane is excluded from the next hitscan. Its wreck's OBB still
            // blocks an approach along the same ray.
            Vec3 approach = f.player;
            Vec3 along{aim.x * 4, 0, aim.z * 4};
            for (int i = 0; i < 90; ++i) {
                velocity = along;
                world.move(approach, velocity, float(tickSeconds), 1.85f, .4f);
            }
            CHECK(planar(approach - f.player) < 5.f);
            std::printf("Real laminated pane %d hits surface %u three times, then surface %u through the opening.\n",
                tri.glass, hit.surface, through.surface);
            return;
        }
    }
    CHECK(false && "Find and test a line through a real laminated wreck pane");
}

void testDeath(const Assets& assets) {
    Game g(assets);
    g.setRandomHook(centerDraw, nullptr);
    g.start(22, 3);
    auto f = quiet();
    f.killPlayerAtTick = 5;
    g.setupFixture(f);
    ticks(g, 4);
    CHECK(g.state().timeScale == 1 && !g.state().dead);
    g.step({});
    CHECK(count(g, EventKind::PlayerDeath) == 1 && g.state().rivalScore == 1);
    CHECK(g.state().timeScale == .5 && g.state().dead);
    g.step(buttons(Fire, Fire));
    CHECK(g.state().scaledDelta == tickSeconds * .5);
    int elapsed = 1;
    while (g.state().dead && elapsed < 360) {
        g.step(buttons(Fire));
        ++elapsed;
    }
    CHECK(!g.state().dead && count(g, EventKind::PlayerRespawn) == 1);
    CHECK(g.state().timeScale == 1 && g.state().realTime > g.state().scaledTime);
    // The browser accepts continuous automatic input on the respawn tick
    // after refilling the magazine. This is separate from pause and USB loss.
    CHECK(elapsed > 192 && elapsed < 360 && g.state().clip == 29 && count(g, EventKind::Fire) == 1);
    ticks(g, 7);
    g.step(buttons(Fire, Fire));
    CHECK(g.state().clip == 28);
    std::printf("Death camera refilled the loadout after %d wall-clock ticks.\n", elapsed);

    g.start(22, 3);
    f.weapon = Weapon::Pistol;
    g.setupFixture(f);
    ticks(g, 5);
    CHECK(g.state().dead);
    g.step(buttons(Fire, Fire));
    while (g.state().dead) g.step(buttons(Fire));
    CHECK(g.state().clip == 12 && count(g, EventKind::Fire) == 0);
    g.step({});
    g.step(buttons(Fire, Fire));
    CHECK(g.state().clip == 11);

    g.start(22, 3);
    f.rivalScore = 9;
    f.killPlayerAtTick = 1;
    g.setupFixture(f);
    g.step({});
    const auto* result = event(g, EventKind::MatchEnd);
    CHECK(g.state().rivalScore == 10 && g.state().match == Match::Results && result && result->value == -1);
}

void testStreakDevices(const Assets& assets) {
    Game g(assets);
    g.setRandomHook(centerDraw, nullptr);
    g.start(55, 3);
    auto f = quiet();
    f.airstrikeReady = true;
    g.setupFixture(f);
    g.step(buttons(Streak | Fire, Streak | Fire));
    CHECK(g.state().device == Device::Laptop && count(g, EventKind::LaptopOpen) == 1 && !count(g, EventKind::Fire));
    ticks(g, 20);
    Input corner;
    corner.lookX = 1;
    corner.lookY = -1;
    const float yaw = g.state().playerYaw;
    g.step(corner);
    CHECK(g.state().cursorX == 15.6f && g.state().cursorZ == 15.6f);
    CHECK(g.state().playerYaw == yaw);
    corner.lookX = -1.5f;
    corner.lookY = 1.5f;
    g.step(corner);
    CHECK(g.state().cursorX == -15.6f && g.state().cursorZ == -15.6f);
    g.step(buttons(Fire, Fire));
    CHECK(count(g, EventKind::StrikeMark) == 1 && !g.state().airstrikeReady && g.state().clip == 30);
    CHECK(count(g, EventKind::Fire) == 0);
    int blasts = 0;
    for (int i = 0; i < 410; ++i) {
        g.step({});
        blasts += count(g, EventKind::StrikeImpact);
        CHECK(count(g, EventKind::StrikeMark) == 0);
    }
    CHECK(blasts == 9 && g.state().device == Device::None);

    g.start(55, 3);
    f.apacheReady = true;
    f.rival = {0, 0, .3f};
    g.setupFixture(f);
    g.step(buttons(Streak, Streak));
    ticks(g, 27);
    CHECK(g.state().device == Device::Transmitter && !g.state().apacheActive);
    g.step({});
    CHECK(count(g, EventKind::ApacheLaunch) == 1 && g.state().tick == 29);
    CHECK(g.state().apacheActive && !g.state().apacheReady && g.state().airstrikeReady);
    int apacheShots = 0;
    for (int i = 0; i < 1600; ++i) {
        g.step({});
        CHECK(!g.state().dead && g.state().playerYaw == f.yaw);
        CHECK(length(g.state().camera.position - atHeight(f.player, 1.7f)) < .01f);
        apacheShots += count(g, EventKind::ApacheAttack);
    }
    CHECK(apacheShots > 0);
    std::printf("Airstrike delivered %d impacts and Apache launched at tick 29, retaining player view through %d rounds.\n",
        blasts, apacheShots);
}

void testStreakRewards(const Assets& assets) {
    Game g(assets);
    g.setRandomHook(centerDraw, nullptr);
    const int thresholds[]{3, 5, 7, 10};
    const EventKind awards[]{EventKind::Uav, EventKind::AirstrikeReady, EventKind::ApacheReady, EventKind::MatchEnd};
    for (int i = 0; i < 4; ++i) {
        g.start(28, 3);
        auto f = quiet();
        f.player = {-14.3f, 0, -12.5f};
        f.rival = {-14.3f, 0, -7.5f};
        f.yaw = 0;
        f.rivalYaw = pi;
        f.pitch = .08f;
        f.weapon = Weapon::Sniper;
        f.playerScore = f.streak = thresholds[i] - 1;
        g.setupFixture(f);
        ticks(g, 60, buttons(Aim));
        g.step(buttons(Fire | Aim, Fire));
        CHECK(count(g, EventKind::RivalHit) == 1 && count(g, EventKind::RivalKill) == 1);
        CHECK(count(g, awards[i]) == 1 && g.state().playerScore == thresholds[i]);
        CHECK(g.state().lethalMarker && g.state().killConfirm > 1);
        const Sound cue = g.state().headshot ? Sound::HeadshotConfirm : Sound::KillConfirm;
        CHECK(std::count_if(g.events(), g.events() + g.eventCount(), [cue](const Event& e) {
            return e.kind == EventKind::Sound && e.sound == cue;
        }) == 1);
        if (g.state().match == Match::Playing) {
            ticks(g, 86);
            CHECK(g.state().killConfirm == 0 && g.state().hitMarker == 0);
        }
        if (thresholds[i] == 3) {
            CHECK(g.state().uavSeconds > 28 && !g.state().radarContact);
            ticks(g, 255);
            CHECK(g.state().rival.health > 0 && g.state().radarContact);
            const float remaining = g.state().uavSeconds;
            g.pause();
            ticks(g, 60);
            CHECK(g.state().uavSeconds == remaining);
            g.resume(false);
            ticks(g, int(std::floor(remaining / tickSeconds)) + 1);
            CHECK(g.state().uavSeconds == 0 && !g.state().radarContact);
            g.start(28, 3);
            CHECK(g.state().uavSeconds == 0 && !g.state().radarContact);
        }
    }
}

void testDuel(const Assets& assets, bool hanz) {
    Game g(assets);
    g.start(449, 3, hanz);
    const World world(assets);
    Navigation nav(assets);
    std::vector<int> path;
    CHECK(nav.path(16, 138, path));
    CHECK((path == std::vector<int>{16, 15, 14, 13, 12, 11, 10, 36, 61, 87, 112, 138}));
    path.clear();
    size_t waypoint = 0;
    uint32_t planAt = 0;
    int shots = 0, hits = 0, rivalShots = 0, reloads = 0, exposureTicks = 0;
    for (int i = 0; i < 36000 && g.state().match == Match::Playing; ++i) {
        const auto& s = g.state();
        Input input;
        if (i == 1) input.pressed = Swap;
        if (!s.dead && s.rival.health == 0 && s.weapon == Weapon::Sniper && s.weaponState == WeaponState::Idle &&
            s.clip < 5 && s.reserve > 0) input.pressed = Reload;
        if (i > 40 && !s.dead && s.rival.health > 0) {
            const auto& target = s.rival;
            const auto& pose = assets.rivalHitboxes[target.frame];
            const Vec3 chest = [&] {
                for (const auto& b : pose) if (b.multiplier == 1.5f) return b.center;
                return Vec3{0, 1.35f, 0};
            }();
            const float c = std::cos(target.yaw), sn = std::sin(target.yaw);
            const Vec3 aim = target.position + Vec3{chest.x * c + chest.z * sn, chest.y, -chest.x * sn + chest.z * c};
            const Vec3 d = aim - s.camera.position;
            const auto nearest = world.shot(s.camera.position, normalized(d), s.brokenGlass);
            const bool canSee = !nearest.hit || nearest.distance >= length(d) - .2f;
            exposureTicks = canSee ? exposureTicks + 1 : 0;
            if (canSee || (nearest.hit && nearest.glass >= 0)) {
                const float y = std::atan2(d.x, d.z);
                const float p = -std::atan2(d.y, planar(d));
                const auto& frames = game_data::aim[int(Weapon::Sniper)];
                const int frame = std::clamp(int(std::lround(s.ads * game_data::aimLast[1])), 0, game_data::aimLast[1]);
                input.lookX = float(angleDelta(s.camera.yaw, y) / frames[frame].sensitivity);
                input.lookY = float((p - s.camera.pitch) / frames[frame].sensitivity);
                input.held |= Aim;
                // Hold the lane briefly so the rival gets to perceive and
                // return fire before the scripted sharpshooter takes the shot.
                if (exposureTicks >= 140 && s.ads >= 1 && s.weaponState == WeaponState::Idle &&
                    std::abs(input.lookX) + std::abs(input.lookY) < .09f) {
                    input.pressed = Fire;
                }
            } else {
                if (s.tick >= planAt || waypoint >= path.size()) {
                    planAt = s.tick + 24;
                    nav.path(nav.nearest(s.player.position), nav.nearest(target.position), path);
                    waypoint = 0;
                }
                Vec3 goal = waypoint < path.size() ? nav.node(path[waypoint]).position : target.position;
                while (waypoint + 1 < path.size() && !world.walkBlocked(s.player.position, nav.node(path[waypoint + 1]).position)) {
                    goal = nav.node(path[++waypoint]).position;
                }
                if (waypoint + 1 < path.size() && planar(goal - s.player.position) < .48f) {
                    goal = nav.node(path[++waypoint]).position;
                }
                const Vec3 move = normalized(Vec3{goal.x - s.player.position.x, 0, goal.z - s.player.position.z});
                const float sy = std::sin(s.playerYaw), cy = std::cos(s.playerYaw);
                input.moveX = move.x * cy - move.z * sy;
                input.moveY = move.x * sy + move.z * cy;
                input.lookX = float(angleDelta(s.playerYaw, std::atan2(move.x, move.z))) * .22f;
            }
        }
        g.step(input);
        shots += count(g, EventKind::Fire);
        hits += count(g, EventKind::RivalHit);
        if (count(g, EventKind::BotFire)) CHECK(g.state().radarContact);
        rivalShots += count(g, EventKind::BotFire);
        reloads += count(g, EventKind::ReloadEnd);
    }
    const auto& s = g.state();
    std::printf("Duel against %s on Ship Box ended %d-%d at tick %u (%d shots, %d hits, %d rival rounds, %d reloads).\n",
        hanz ? "Hanz" : "IPwndUrMom", s.playerScore, s.rivalScore, s.tick, shots, hits, rivalShots, reloads);
    CHECK(s.match == Match::Results && s.playerScore == 10 && count(g, EventKind::MatchEnd) == 1);
    CHECK(shots >= 10 && hits >= 10 && rivalShots > 0 && reloads >= 1);
    g.start(450, 10, true);
    CHECK(g.state().match == Match::Playing && g.state().playerScore == 0 && g.state().rivalScore == 0);
    CHECK(g.state().weapon == Weapon::Mp44 && g.state().clip == 30 && !g.state().brokenGlass && !g.state().apacheActive);
    CHECK(g.state().rival.health == 132 && g.state().alternateRival);
}

} // namespace

int main(int argc, char** argv) {
    CHECK(argc == 2);
    Assets assets;
    std::string error;
    if (!assets.load(argv[1], error)) {
        std::fprintf(stderr, "Asset load failed: %s\n", error.c_str());
        return 1;
    }
    CHECK(assets.navNodes.size() == 676);
    CHECK(!assets.world.triangles.empty() && !assets.shotSurfaces.empty() && !assets.obstacles.empty());
    CHECK(!assets.rivalHitboxes.empty() && !assets.rivalHitboxes[0].empty());
    CHECK(!assets.audio.empty());
    testWeapons(assets);
    testMovementAndPause(assets);
    testGlass(assets);
    testDeath(assets);
    testStreakRewards(assets);
    testStreakDevices(assets);
    testDuel(assets, false);
    testDuel(assets, true);
    return 0;
}

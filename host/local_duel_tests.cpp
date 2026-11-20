#include "assets.hpp"
#include "game.hpp"
#include "game_data.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>

using namespace fod;

void require(bool condition, const char* expression, int line) {
    if (condition) return;
    std::fprintf(stderr, "Duel test line %d: %s\n", line, expression);
    std::exit(1);
}

#define CHECK(condition) require(bool(condition), #condition, __LINE__)

void testLocalDuelDesktop(const Assets& assets, bool live, int width);

namespace {
Input buttons(uint32_t held, uint32_t pressed = 0) {
    Input input;
    input.held = held;
    input.pressed = pressed;
    return input;
}

std::array<Fixture, 2> duelLine() {
    std::array<Fixture, 2> fixtures{};
    fixtures[0].player = {0, 0, -2};
    fixtures[1].player = {0, 0, 2};
    fixtures[0].yaw = 0;
    fixtures[1].yaw = pi;
    for (auto& fixture : fixtures) fixture.weapon = Weapon::Sniper;
    return fixtures;
}

int events(const Game& game, EventKind kind) {
    return int(std::count_if(game.events(), game.events() + game.eventCount(),
                             [kind](const Event& event) { return event.kind == kind; }));
}

void aimBoth(Game& game) {
    for (int i = 0; i < 45; ++i) game.stepLocalDuel({buttons(Aim), buttons(Aim)});
    CHECK(game.localView(0).ads == 1.f && game.localView(1).ads == 1.f);
}

void movementAndWeapons(const Assets& assets) {
    Game game(assets);
    game.startLocalDuel(1);
    const auto position = game.localView(0).player.position;
    const auto yaw = game.localView(1).playerYaw;
    Input move, look;
    move.moveY = .7f;
    look.lookX = .2f;
    game.stepLocalDuel({move, look});
    CHECK(length(game.localView(0).player.position - position) > .02f);
    CHECK(game.localView(1).playerYaw > yaw + .19f);
    CHECK(length(game.localView(0).player.position - game.localView(1).rival.position) == 0.f);
    game.stepLocalDuel({buttons(Fire, Fire), buttons(Aim)});
    CHECK(game.localView(0).clip == 29 && game.localView(1).clip == 30);
    CHECK(game.localView(0).muzzle == 1.f && game.localView(1).muzzle == 0.f);
    game.stepLocalDuel({buttons(Reload, Reload), buttons(Swap, Swap)});
    CHECK(game.localView(0).weaponState == WeaponState::Reload);
    for (int i = 0; i < 90; ++i) game.stepLocalDuel({});
    CHECK(game.localView(0).clip == 30 && game.localView(0).reserve == 89);
    CHECK(game.localView(1).weapon == Weapon::Sniper && game.localView(1).clip == 5);
    game.stepLocalDuel({Input{}, buttons(Swap, Swap)});
    for (int i = 0; i < 40; ++i) game.stepLocalDuel({});
    CHECK(game.localView(1).weapon == Weapon::Pistol && game.localView(1).clip == 12);

    const auto tick = game.state().tick;
    game.stepLocalDuel({Input{}, buttons(0, Pause)});
    CHECK(game.state().match == Match::Paused && game.localView(1).match == Match::Paused);
    CHECK(game.state().tick == tick);
    game.resumeLocalDuel();
    game.stepLocalDuel({buttons(Fire, Fire), buttons(Fire, Fire)});
    CHECK(game.localView(0).clip == 30 && game.localView(1).clip == 12);
    game.stepLocalDuel({});
    game.stepLocalDuel({buttons(Fire, Fire), buttons(Fire, Fire)});
    CHECK(game.localView(0).clip == 29 && game.localView(1).clip == 11);
}

void scoringAndTime(const Assets& assets) {
    Game game(assets);
    const auto fire = buttons(Aim | Fire, Fire);
    for (int winner = 0; winner < 2; ++winner) {
        auto frames = duelLine();
        frames[winner].playerScore = 9;
        game.startLocalDuel(2);
        game.setupLocalDuelFixture(frames);
        aimBoth(game);
        std::array<Input, 2> inputs{buttons(Aim), buttons(Aim)};
        inputs[winner] = fire;
        game.stepLocalDuel(inputs);
        CHECK(game.state().match == Match::Results);
        CHECK(game.localView(winner).playerScore == 10 && game.localView(1 - winner).rivalScore == 10);
        CHECK(!game.localView(winner).dead && game.localView(1 - winner).dead);
    }

    for (int score : {0, 9}) {
        auto frames = duelLine();
        frames[0].playerScore = frames[1].playerScore = score;
        game.startLocalDuel(3);
        game.setupLocalDuelFixture(frames);
        aimBoth(game);
        game.stepLocalDuel({fire, fire});
        CHECK(game.localView(0).dead && game.localView(1).dead);
        CHECK(game.localView(0).playerScore == score + 1 && game.localView(1).playerScore == score + 1);
        CHECK(events(game, EventKind::PlayerDeath) == 2);
        CHECK(game.localView(0).timeScale == 1 && game.localView(1).timeScale == 1);
        CHECK(game.state().match == (score == 9 ? Match::Results : Match::Playing));
        if (score == 9) {
            CHECK(events(game, EventKind::MatchEnd) == 1);
            CHECK(game.events()[game.eventCount() - 1].value == 0);
        } else {
            for (int i = 0; i < 205; ++i) game.stepLocalDuel({});
            CHECK(!game.localView(0).dead && !game.localView(1).dead);
            CHECK(length(game.localView(0).player.position + game.localView(1).player.position) < .01f);
            CHECK(length(game.localView(0).player.position - game.localView(1).player.position) > 30.f);
        }
    }

    game.startLocalDuel(4);
    game.setupLocalDuelFixture(duelLine());
    aimBoth(game);
    game.stepLocalDuel({fire, buttons(Aim)});
    CHECK(!game.localView(0).dead && game.localView(1).dead);
    const auto before = game.localView(0).player.position;
    Input move;
    move.moveY = 1;
    for (int i = 0; i < 205; ++i) game.stepLocalDuel({move, {}});
    CHECK(!game.localView(1).dead);
    CHECK(game.localView(0).player.position.z > before.z + 10);
    CHECK(length(game.localView(0).player.position - game.localView(1).player.position) > 3);
    CHECK(game.localView(1).player.health == 100);
    game.startLocalDuel(5);
    CHECK(game.localView(0).playerScore == 0 && game.localView(1).playerScore == 0);
    CHECK(game.localView(0).tick == 0 && game.localView(1).weapon == Weapon::Mp44);
}

void sharedGlass() {
    Assets assets;
    assets.glassHits = {1};
    assets.shotSurfaces = {{1, 0}, {2, 0}};
    assets.shotVertices = {{-2, 0, 0}, {2, 0, 0}, {2, 3, 0}, {-2, 0, 0}, {2, 3, 0}, {-2, 3, 0}};
    Game game(assets);
    game.startLocalDuel(6);
    game.setupLocalDuelFixture(duelLine());
    aimBoth(game);
    const auto fire = buttons(Aim | Fire, Fire);
    game.stepLocalDuel({fire, fire});
    CHECK(game.localView(0).brokenGlass == 1 && game.localView(1).brokenGlass == 1);
    CHECK(events(game, EventKind::GlassBreak) == 1);
    CHECK(events(game, EventKind::PlayerHit) == 0);
    for (int i = 0; i < 130; ++i) game.stepLocalDuel({buttons(Aim), buttons(Aim)});
    game.stepLocalDuel({fire, fire});
    CHECK(game.localView(0).playerScore == 1 && game.localView(1).playerScore == 1);
    game.startLocalDuel(7);
    CHECK(game.localView(0).brokenGlass == 0 && game.localView(1).brokenGlass == 0);
}

void completeMatch(const Assets& assets) {
    Game game(assets);
    game.startLocalDuel(8);
    game.setupLocalDuelFixture(duelLine());
    constexpr int sniper = int(Weapon::Sniper);
    const auto sensitivity = game_data::aim[sniper][game_data::aimLast[sniper]].sensitivity;
    aimBoth(game);
    for (int point = 1; point <= 10; ++point) {
        const auto& shooter = game.localView(0);
        const auto& target = game.localView(1);
        const Vec3 difference = target.player.position + Vec3{0, 1.5f, 0} - shooter.camera.position;
        const float yaw = std::atan2(difference.x, difference.z);
        const float pitch = -std::atan2(difference.y, std::hypot(difference.x, difference.z));
        Input shot = buttons(Aim | Fire, Fire);
        shot.lookX = float(std::remainder(yaw - shooter.playerYaw, 2 * pi) / sensitivity);
        shot.lookY = float((pitch - shooter.playerPitch) / sensitivity);
        CHECK(shooter.ads == 1.f && !target.dead);
        game.stepLocalDuel({shot, Input{}});
        CHECK(game.localView(0).playerScore == point && game.localView(1).dead);
        CHECK(game.state().match == (point == 10 ? Match::Results : Match::Playing));
        for (int tick = 0; point < 10 && tick < 275; ++tick) {
            game.stepLocalDuel({buttons(Aim), Input{}});
        }
    }
}
} // namespace

int main(int argc, char** argv) {
    CHECK(argc == 2 || ((argc == 3 || argc == 4) && std::strcmp(argv[2], "--live") == 0));
    const int width = argc == 4 ? std::atoi(argv[3]) : 640;
    CHECK(width == 400 || width == 640 || width == 800);
    Assets assets;
    std::string error;
    if (!assets.load(argv[1], error)) {
        std::fprintf(stderr, "%s\n", error.c_str());
        return 1;
    }
    const Assets emptyMap;
    movementAndWeapons(emptyMap);
    scoringAndTime(emptyMap);
    completeMatch(emptyMap);
    sharedGlass();
    testLocalDuelDesktop(assets, argc >= 3, width);
}

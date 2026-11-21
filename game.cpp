#include "game.hpp"

#include "game_bot.hpp"
#include "mission_internal.hpp"

#include <iterator>
#include <optional>

namespace fod {
namespace {

using namespace game_detail;

constexpr Sound weaponShots[]{Sound::Mp44, Sound::Sniper, Sound::Pistol};
constexpr double fallAt[]{1.32, 1.62, 1.30};
constexpr double fallStart[]{.50, .98, .62};
constexpr double gaspAt[]{.30, .32, .26};
constexpr float standEye = 1.7f, crouchEye = .95f, proneEye = .42f;
constexpr float footstepGain = .35f;
constexpr int playerSlot = -1;
constexpr int matchTarget = 10;
constexpr double campaignRejoinSeconds = 5.0;
constexpr int maxDifficulty = std::size(game_data::levels[0]);

constexpr auto freeForAllHanzLevels = [] {
    std::array<game_data::Difficulty, maxDifficulty> levels{};
    for (int i = 0; i < maxDifficulty; ++i) {
        const auto& base = game_data::levels[0][i];
        auto& hanz = levels[i];
        hanz = game_data::levels[1][i];
        // Let his reactions and tactics distinguish Hanz in Free-for-all.
        hanz.health = base.health;
        hanz.damage = base.damage;
        hanz.aimError = base.aimError;
        hanz.aimSpeed = base.aimSpeed;
        hanz.spread = base.spread;
    }
    return levels;
}();

struct ReloadCue {
    double time = 0;
    enum class Kind { Handle, MagOut, MagIn, Round } kind = Kind::MagOut;
};
struct Gun {
    int clip = 0, reserve = 0, nextCue = 0, cueCount = 0;
    WeaponState mode = WeaponState::Idle;
    double timer = 0, duration = 0, cooldown = 0, adsFrame = 0, kick = 0, bolt = 0;
    bool burst = false, boltSound = false, slideLocked = false, aiming = false;
    bool emptyReload = false;
    std::array<ReloadCue, 7> cues{};
};
enum class Phase { Away, Raising, Out, Lowering };
enum class Flight { Off, Enter, Hunt, Leave };
enum class Cause { Weapon, Airstrike, Apache };

struct Bomb { double at = 0; Vec3 pos{}; };
struct Airstrike {
    std::array<Bomb, 9> bombs{};
    std::array<double, 3> launches{}, expires{};
    std::array<Vec3, 3> origins{};
    Vec3 direction{};
    double latestBomb = 0;
};
struct Apache {
    Flight phase = Flight::Off;
    Vec3 pos{}, vel{};
    double until = 0, angle = 0, yaw = 0, pitch = 0, roll = 0, timer = 0, burstAt = 0, retargetAt = 0;
    int rounds = 0;
    int target = playerSlot;
};
struct Death {
    double elapsed = 0;
    float yaw = 0, distance = 0, height = 0, liveDistance = 0;
    Camera origin{};
    uint8_t variant = 0;
};

struct Human {
    Snapshot view{};
    std::array<Gun, 3> guns{};
    double switchTimer = 0;
    Phase swap = Phase::Away;
    int previousWeapon = int(Weapon::Sniper); // Campaign quick-swap returns here
    bool lookLocked = false, stanceWasDown = false, stanceHeld = false, crouchToggle = false;
    bool playerFired = false, blockFire = false;
    double stanceTime = 0, invulnerableUntil = 0, hurtAt = -forever, respawn = 0;
    Vec3 bob{}, sway{};
    double swayTime = 0, bobTime = 0, scopeTime = 0, footstep = 0;
    double recoilPitch = 0, recoilYaw = 0, recoilKick = 0;
    Death death{};

    int slot = playerSlot;
    int weapon() const { return int(view.weapon); }
    Gun& gun() { return guns[weapon()]; }
};

struct Shot {
    Vec3 origin{}, direction{};
    float damage = 0;
};

struct Rival {
    Rival(Navigation& navigation, const World& world, Random& random) : bot(navigation, world, random) {}

    Bot bot;
    Death death{};
    double respawn = 0, footstep = 0, lastShot = -forever, lastHit = -forever, retargetAt = 0;
    int target = playerSlot, attacker = playerSlot;
    Cause killCause = Cause::Weapon;
    float killZone = 1;
    bool counted = false;
};

game_data::AimFrame interpolateAim(int id, double frame) {
    const int i = int(frame);
    const int j = std::min(i + 1, game_data::aimLast[id]);
    const auto& a = game_data::aim[id][i];
    const auto& b = game_data::aim[id][j];
    const double u = frame - i;
    const auto lerp = [u](double start, double end) { return start + (end - start) * u; };
    game_data::AimFrame out{};
    for (int k = 0; k < 3; ++k) {
        out.position[k] = lerp(a.position[k], b.position[k]);
        out.rotation[k] = lerp(a.rotation[k], b.rotation[k]);
    }
    out.fov = lerp(a.fov, b.fov);
    out.scope = lerp(a.scope, b.scope);
    out.sensitivity = lerp(a.sensitivity, b.sensitivity);
    return out;
}

Vec3 intoActor(Vec3 point, float yaw) {
    const float c = std::cos(yaw), s = std::sin(yaw);
    return {c * point.x - s * point.z, point.y, s * point.x + c * point.z};
}

} // namespace

struct Game::Impl {
    Impl(const Assets& a, uint32_t voiceSeed) : assets(a), world(a), navigation(a), first(navigation, world, random) {
        voiceRandom.state = voiceSeed ? voiceSeed : 0x516ad3cdu;
        const auto& spawn = assets.spawns[0];
        snapshot.rival.position = assets.spawns[1].position;
        snapshot.player.position = spawn.position;
        snapshot.camera.position = atHeight(spawn.position, standEye);
        snapshot.camera.yaw = snapshot.playerYaw = spawn.yaw;
        refill(player, true);
        viewWeapon(player, handheld());
    }

    const Assets& assets;
    World world;
    // The simulation steps rivals sequentially. Each keeps its own completed route.
    Navigation navigation;
    Random random, voiceRandom, secondRandom;
    Rival first;
    std::optional<Rival> second;
    std::unique_ptr<mission_detail::MissionController> mission;
    float signalLower = 0; // How far the gun has dipped while the player holds Signal
    Human player;
    Snapshot& snapshot = player.view;
#ifdef FOD_LOCAL_DUEL
    std::unique_ptr<Human> guest;
    std::array<Random, 2> localRandom{};
    std::array<uint32_t, 2> campaignBlockedActions{};
    float scoutLookYaw = 0, scoutLookPitch = 0;
    bool partnerCameraShared = false;
#endif
    std::array<Event, 96> eventBuffer{};
    size_t eventCount = 0;
    std::optional<CampaignVoiceCue> campaignVoice;

    std::array<uint8_t, 64> paneHits{};
    Phase laptop = Phase::Away, transmitter = Phase::Away;
    double laptopTime = 0, transmitterTime = 0, uavUntil = -1;
    bool signalSent = false;
    Airstrike strike{};
    Apache apache{};
    uint32_t fixtureDeathTick = 0;

    int rivalCount() const { return second ? 2 : 1; }
    const Rival& rival(int slot) const { return slot == 0 ? first : *second; }
    Rival& rival(int slot) { return slot == 0 ? first : *second; }
    Random& rivalRandom(int slot) { return slot == 0 ? random : secondRandom; }
    int& rivalScore(int slot) { return slot == 0 ? snapshot.rivalScore : snapshot.secondRivalScore; }

    bool isHanz(int slot) const { return slot == 1 || snapshot.alternateRival; }
    const game_data::Difficulty& difficulty(int slot) const {
        const int level = snapshot.difficulty - 1;
        if (second && slot == 1) return freeForAllHanzLevels[level];
        return game_data::levels[isHanz(slot) ? 1 : 0][level];
    }

    void emit(EventKind kind, int value = 0, Vec3 position = {}, uint32_t surface = 0,
              int slot = playerSlot, int attacker = playerSlot) {
        if (second && (kind == EventKind::RivalKill || kind == EventKind::BotKill ||
                       (kind == EventKind::PlayerDeath && attacker != playerSlot))) {
            snapshot.killNotice = {2.5f, int8_t(attacker), int8_t(slot)};
        }
        if (eventCount >= eventBuffer.size()) return;
        auto& event = eventBuffer[eventCount++];
        event = {kind, snapshot.tick, surface, value, position};
        event.rival = int8_t(slot);
        event.attacker = int8_t(attacker);
    }

    void sound(Sound id, float volume = 1, bool positional = false, Vec3 position = {}) {
        if (eventCount >= eventBuffer.size()) return;
        eventBuffer[eventCount++] = {EventKind::Sound, snapshot.tick, 0, 0, position, id, volume, positional};
    }

    void taunt(int slot, bool rivalWon) {
        // Each taunt is independently eligible, from the cosmetic RNG only.
        if (voiceRandom.draw("voice") >= .5) return;
        const bool alternate = isHanz(slot);
        const Sound line = rivalWon
            ? (alternate ? Sound::VoiceAlternateVictory : Sound::VoiceVictory)
            : (alternate ? Sound::VoiceAlternateKill : Sound::VoiceKill);
        sound(line, .602f);
    }

    void refill(Human& human, bool newMatch) {
        for (int i = 0; i < 3; ++i) {
            auto& state = human.guns[i];
            state = {};
            state.clip = game_data::weapons[i].mag;
            state.reserve = game_data::weapons[i].reserve;
        }
        if (newMatch) {
            human.view.weapon = Weapon::Mp44;
            human.previousWeapon = int(Weapon::Sniper);
        }
        human.swap = Phase::Away;
        human.switchTimer = 0;
        human.playerFired = false;
    }

    void cancelWeapon(Human& human) {
        auto& state = human.gun();
        if (state.mode == WeaponState::Reload) {
            state.mode = WeaponState::Idle;
            state.timer = 0;
            state.cueCount = 0;
            if (state.clip) state.slideLocked = false;
        }
        state.adsFrame = 0;
        state.aiming = false;
    }

    void applyRecoil(Human& human, Random& rng, double pitch, double yaw, double kick) {
        human.recoilPitch += pitch * deg;
        human.recoilYaw += (rng.draw("camera.recoil.yaw") - .5) * yaw * deg;
        human.recoilKick += kick;
    }

    void resetPlayer(Human& human, Vec3 position, float yaw) {
        auto& s = human.view;
        s.player = {};
        s.player.position = position;
        s.player.yaw = yaw;
        s.player.visible = false;
        s.playerVelocity = {};
        s.viewmodelSway = {};
        s.landing = s.sprintPose = s.sprintPhase = 0;
        s.weaponYawLag = s.weaponPitchLag = 0;
        s.dead = s.sprint = s.crouch = s.prone = false;
        s.grounded = true;
        s.eyeHeight = standEye;
        s.playerYaw = yaw;
        s.playerPitch = s.damage = 0;
        s.camera.position = atHeight(position, standEye);
        s.camera.yaw = yaw;
        s.camera.pitch = 0;
        human.crouchToggle = human.lookLocked = false;
        resetInput(human);
        human.bob = human.sway = {};
        human.swayTime = human.bobTime = human.scopeTime = human.footstep = 0;
        human.recoilPitch = human.recoilYaw = human.recoilKick = 0;
        human.invulnerableUntil = s.scaledTime + 1;
        human.hurtAt = -forever;
    }

    void resetInput(Human& human) {
        human.stanceWasDown = human.stanceHeld = false;
        human.stanceTime = 0;
    }

    BotTarget target() const {
        const auto& s = snapshot;
        return {s.player.position, s.playerVelocity, s.eyeHeight, s.dead, s.sprint, s.prone, s.crouch, player.playerFired};
    }

    BotTarget combatTarget(int slot, double dt = 0) const {
        if (slot == playerSlot) return target();
        const auto& r = rival(slot);
        return {r.bot.actor.position, r.bot.velocity, botEye, r.bot.dead, r.bot.actor.speed > 4.8f,
                false, false, r.lastShot >= snapshot.scaledTime - dt};
    }

    void setDifficulty(int level) { snapshot.difficulty = std::clamp(level, 1, maxDifficulty); }

    void useGeometry(ArenaScene scene) {
        const auto& geometry = assets.geometry(scene);
        world.useGeometry(geometry);
        navigation.useGeometry(geometry);
    }

    void start(uint32_t seed, int level, Opponent opponent) {
        useGeometry(ArenaScene::Estate);
#ifdef FOD_LOCAL_DUEL
        guest.reset();
        player.slot = playerSlot;
#endif
        mission.reset();
        snapshot = {};
        snapshot.match = Match::Playing;
        snapshot.alternateRival = opponent == Opponent::Hanz;
        snapshot.freeForAll = opponent == Opponent::FreeForAll;
        if (snapshot.freeForAll) {
            if (!second) second.emplace(navigation, world, secondRandom);
        } else {
            second.reset();
        }
        setDifficulty(level);
        random.state = seed ? seed : 1;
        secondRandom.state = random.state ^ 0x82f63b78u;
        if (!secondRandom.state) secondRandom.state = 1;
        random.tick = 0;
        eventCount = 0;
        campaignVoice.reset();
        paneHits.fill(0);
        refill(player, true);
        resetPlayer(player, assets.spawns[0].position, assets.spawns[0].yaw);
        for (int slot = 0; slot < rivalCount(); ++slot) {
            auto& r = rival(slot);
            r.bot.freeze = false;
            const auto& spawn = assets.spawns[slot + 1];
            resetRival(slot, spawn.position, spawn.yaw, true);
        }
        laptop = transmitter = Phase::Away;
        laptopTime = transmitterTime = 0;
        uavUntil = -1;
        strike = {};
        apache = {};
        player.death = {};
        player.respawn = 0;
        fixtureDeathTick = 0;
        player.blockFire = false;
        const int speaker = second && voiceRandom.draw("voice.start") >= .5 ? 1 : 0;
        sound(isHanz(speaker) ? Sound::VoiceAlternateStart : Sound::VoiceStart, .602f);
        viewWeapon(player, handheld());
        viewWorld();
    }

    void resetRival(int slot, Vec3 pos, float yaw, bool newMatch) {
        auto& r = rival(slot);
        r.bot.reset(pos, yaw, difficulty(slot), snapshot.scaledTime, newMatch);
        r.death = {};
        r.counted = false;
        r.footstep = r.respawn = r.retargetAt = 0;
        r.lastShot = r.lastHit = -forever;
        r.target = r.attacker = playerSlot;
        r.killCause = Cause::Weapon;
        r.killZone = 1;
    }

    float respawnYaw(const SpawnPoint& spawn) const {
        return assets.arena == Arena::ShipBox ? std::atan2(-spawn.position.x, -spawn.position.z) : spawn.yaw;
    }

    const SpawnPoint& chooseDuelSpawn(bool forRival) {
        const auto& s = snapshot;
        const auto& bot = first.bot;
        const Vec3 other = forRival ? s.player.position : bot.actor.position;
        const Vec3 eye = atHeight(other, forRival ? s.eyeHeight : botEye);
        double best = -forever;
        size_t chosen = 0;
        for (size_t i = forRival ? 1 : 0; i < assets.spawns.size(); ++i) {
            const Vec3 spawn = assets.spawns[i].position;
            const bool hidden = (!forRival && bot.dead) || world.blocked(atHeight(spawn, botEye), eye);
            double value = planar(spawn - other);
            if (forRival) {
                value += (hidden ? 30 : 0) + random.draw("bot.spawn.corner") * 11;
            } else {
                value = (bot.dead ? 30 : std::min(30., value)) + (hidden ? 24 : 0) +
                    random.draw("player.spawn.corner") * 2;
            }
            if (value > best) {
                best = value;
                chosen = i;
            }
        }
        return assets.spawns[chosen];
    }

    const SpawnPoint& chooseSpawn(int slot) {
        if (!second) return chooseDuelSpawn(slot != playerSlot);
        double best = -forever;
        const auto* chosen = &assets.spawns.front();
        for (const auto& point : assets.spawns) {
            const auto spawn = point.position;
            float nearest = 30;
            bool seen = false;
            for (int other = playerSlot; other < rivalCount(); ++other) {
                if (other == slot) continue;
                const auto enemy = combatTarget(other);
                if (enemy.dead) continue;
                nearest = std::min(nearest, planar(spawn - enemy.position));
                seen |= !world.blocked(atHeight(spawn, botEye), atHeight(enemy.position, enemy.eye));
            }
            auto& rng = slot == playerSlot ? random : rivalRandom(slot);
            const double jitter = slot == playerSlot ? rng.draw("player.spawn.corner") * 2 : rng.draw("bot.spawn.corner") * 11;
            const double value = nearest + (seen ? 0 : 24) + jitter - (nearest < 1 ? 100 : 0);
            if (value > best) {
                chosen = &point;
                best = value;
            }
        }
        return *chosen;
    }

    void playerStep(const Input& input, double dt) {
        auto& s = snapshot;
        s.damage *= float(std::exp(-1.6 * dt));
        if (s.dead) {
            player.respawn -= dt;
            if (player.respawn > 0) return;
            const auto& spawn = chooseSpawn(playerSlot);
            const Vec3 pos = spawn.position;
            resetPlayer(player, pos, respawnYaw(spawn));
            refill(player, false);
            s.timeScale = 1;
            emit(EventKind::PlayerRespawn, 0, pos);
        } else if (s.player.health < 100 && s.scaledTime - player.hurtAt > 4) {
            s.player.health = std::min(100.f, s.player.health + float(40 * dt));
        }

        movePlayer(player, input, dt);
    }

    void movePlayer(Human& human, const Input& input, double dt) {
        auto& s = human.view;
        const auto& state = human.gun();
        const double progress = state.adsFrame / game_data::aimLast[human.weapon()];
        const float previousYaw = s.playerYaw, previousPitch = s.playerPitch;
        if (!human.lookLocked) {
            const auto aim = interpolateAim(human.weapon(), state.adsFrame);
            s.playerYaw += float(input.lookX * aim.sensitivity);
            s.playerPitch = std::clamp(s.playerPitch + float(input.lookY * aim.sensitivity), float(-85 * deg), float(85 * deg));
        }
        const float turnDecay = 1.f - std::min(1.f, float(dt * 13));
        s.weaponYawLag = std::clamp(s.weaponYawLag * turnDecay + (previousYaw - s.playerYaw), -.14f, .14f);
        s.weaponPitchLag = std::clamp(s.weaponPitchLag * turnDecay + (previousPitch - s.playerPitch), -.10f, .10f);
        const bool stanceDown = input.down(Stance);
        const bool stancePress = !human.stanceWasDown && (stanceDown || input.edge(Stance));
        const bool stanceReleased = !stanceDown && (human.stanceWasDown || stancePress);
        if (stancePress) {
            human.stanceTime = 0;
            human.stanceHeld = false;
        }
        if (stanceDown) {
            human.stanceTime += dt;
            if (!human.stanceHeld && human.stanceTime >= .32) {
                s.prone = true;
                human.stanceHeld = true;
            }
        } else if (stanceReleased) {
            if (human.stanceHeld || s.prone) {
                const float desired = human.stanceHeld && human.crouchToggle ? crouchEye : standEye;
                if (world.canStand(s.player.position, desired)) {
                    s.prone = false;
                    human.crouchToggle = desired == crouchEye;
                } else if ((!human.stanceHeld || !human.crouchToggle) && world.canStand(s.player.position, crouchEye)) {
                    s.prone = false;
                    human.crouchToggle = true;
                }
            } else if (!human.crouchToggle || world.canStand(s.player.position, standEye)) {
                human.crouchToggle = !human.crouchToggle;
            }
        }
        human.stanceWasDown = stanceDown;
        s.crouch = human.crouchToggle && !s.prone;
        const float desired = s.prone ? proneEye : s.crouch ? crouchEye : standEye;
        s.eyeHeight += (desired - s.eyeHeight) * float(1 - std::exp(-15 * dt));
        s.sprint = input.down(Sprint) && input.moveY > 0 && !s.crouch && !s.prone && progress < .1;
        const float gait = s.prone ? .9f : s.crouch ? 2.f : s.sprint ? 6.2f : 4.f;
        const float shock = playerPlasmaShock(s) / plasmaShockDuration;
        const float speed = gait * (1.f - .27f * shock);
        const float sy = std::sin(s.playerYaw), cy = std::cos(s.playerYaw);
        const Vec3 desiredVel{sy * input.moveY + cy * input.moveX, 0, cy * input.moveY - sy * input.moveX};
        const float magnitude = planar(desiredVel);
        const Vec3 applied = desiredVel * (speed / std::max(1.f, magnitude));
        s.playerVelocity.x = applied.x;
        s.playerVelocity.z = applied.z;
        if (input.edge(Jump) && s.grounded && !s.crouch && !s.prone) {
            s.playerVelocity.y = 5 + gravity / (2 * 60);
        }
        const bool wasGrounded = s.grounded;
        const float descent = s.playerVelocity.y;
        const Vec3 previousPosition = s.player.position;
        s.grounded = world.move(s.player.position, s.playerVelocity, float(dt), s.eyeHeight + .15f, .4f);
        const float motionBlend = std::min(1.f, float(dt * 10));
        s.player.trackMotion(s.playerVelocity, motionBlend);
        s.landing *= 1.f - motionBlend;
        if (!wasGrounded && s.grounded) s.landing = std::clamp(-descent * .15f, 0.f, 1.f);
        s.player.yaw = s.playerYaw;
        s.player.pitch = s.playerPitch;
        s.player.speed = planar(s.playerVelocity);
        float running = 0;
        if (s.sprint && s.grounded && wasGrounded) {
            // A full stride spans two footfalls. Measure travel so walls and edges stop the cycle.
            constexpr float strideLength = 2.f * 6.2f * .34f;
            const float seconds = float(dt);
            const float distance = std::min(planar(s.player.position - previousPosition), s.player.speed * seconds);
            const float travelSpeed = distance / std::max(1e-6f, seconds);
            running = std::clamp((travelSpeed - 2.f) / 3.8f, 0.f, 1.f);
            const float phase = s.sprintPhase + distance * (1.f / strideLength);
            s.sprintPhase = phase - std::floor(phase);
        }
        s.sprintPose += (running - s.sprintPose) * motionBlend;
        const bool moving = magnitude > .001f;
        if (moving && s.grounded && s.player.speed > .2f && !s.prone) {
            human.footstep -= dt;
            if (human.footstep <= 0) {
                sound(Sound::Footstep, footstepGain * (s.sprint ? .55f : s.crouch ? .18f : .34f),
                      human.slot != playerSlot, human.slot != playerSlot ? s.player.position : Vec3{});
                human.footstep += s.sprint ? .34 : s.crouch ? .60 : .45;
            }
        } else {
            human.footstep = 0;
        }
        playerCamera(human, dt, moving, speed / 4, progress);
    }

    void playerCamera(Human& human, double dt, bool moving, float speedRatio, double progress) {
        auto& s = human.view;
        const double decay = std::exp(-10 * dt);
        human.recoilPitch *= decay;
        human.recoilYaw *= decay;
        human.recoilKick *= std::exp(-15 * dt);
        human.scopeTime += dt * .85;
        const double scopeAmplitude = .0026 * progress * progress * (3 - 2 * progress);
        const double scopedYaw = std::sin(human.scopeTime) * scopeAmplitude;
        const double scopedPitch = std::sin(human.scopeTime * 2) * scopeAmplitude * .55;
        const bool sight = progress > .6;
        const double swayScale = sight ? .05 : .2;
        human.swayTime += dt * (sight ? 1.0 : 1.5);
        human.sway.x = float(std::sin(human.swayTime * 1.2) * .015 * swayScale);
        human.sway.y = float(std::cos(human.swayTime * .8) * .02 * swayScale);
        if (moving && !sight) {
            human.bobTime += dt * 12 * speedRatio;
            const double amount = .5 + .5 * speedRatio;
            human.bob.x = float(std::sin(human.bobTime * .5) * .025 * amount);
            human.bob.y = float(std::abs(std::sin(human.bobTime)) * .035 * amount);
        } else {
            human.bobTime = 0;
            human.bob *= float(decay);
        }
        s.viewmodelSway = human.bob + human.sway;
        s.camera.position = atHeight(s.player.position, s.eyeHeight) + human.bob + human.sway -
            forward(s.playerYaw, s.playerPitch) * float(human.recoilKick);
        s.camera.yaw = s.playerYaw + float(human.recoilYaw + scopedYaw);
        s.camera.pitch = s.playerPitch + float(-human.recoilPitch + scopedPitch);
    }

    void startReload(Human& human, Gun& state) {
        const int id = human.weapon();
        const auto& config = game_data::weapons[id];
        if (state.reserve <= 0 || state.clip >= config.mag) return;
        const bool empty = state.clip == 0;
        state.emptyReload = empty;
        using K = ReloadCue::Kind;
        state.nextCue = state.cueCount = 0;
        const auto cue = [&state](double at, K type) { state.cues[state.cueCount++] = {at, type}; };
        if (id == int(Weapon::Sniper)) {
            constexpr double open = .41, feed = .38, close = .45;
            const int rounds = std::min(state.reserve, config.mag - state.clip);
            state.duration = open + rounds * feed + close;
            cue(open * .55, K::Handle);
            for (int i = 0; i < rounds; ++i) cue(open + (i + .85) * feed, K::Round);
            cue(state.duration - close * .45, K::Handle);
        } else if (id == int(Weapon::Pistol)) {
            state.duration = 1.16;
            cue(.15, K::MagOut);
            cue(.75, K::MagIn);
            if (state.slideLocked) cue(.94, K::Handle);
        } else {
            state.duration = empty ? 1.62 : 1.26;
            cue(.2, K::MagOut);
            cue(.92, K::MagIn);
            if (empty) cue(1.1, K::Handle);
        }
        state.timer = state.duration;
        state.mode = WeaponState::Reload;
        emit(EventKind::ReloadStart, id, {}, 0, human.slot);
    }

    void updateCues(Human& human, Gun& state, double dt) {
        state.timer -= dt;
        const double elapsed = state.duration - state.timer;
        while (state.nextCue < state.cueCount && elapsed >= state.cues[state.nextCue].time) {
            const auto& config = game_data::weapons[human.weapon()];
            const auto& cue = state.cues[state.nextCue++];
            using K = ReloadCue::Kind;
            if (cue.kind == K::Round || cue.kind == K::MagIn) {
                const int added = std::min(state.reserve, cue.kind == K::Round ? 1 : config.mag - state.clip);
                state.clip += added;
                state.reserve -= added;
                emit(EventKind::ReloadRound, added, {}, 0, human.slot);
            }
            if (cue.kind == K::Handle && human.weapon() == int(Weapon::Pistol)) state.slideLocked = false;
            sound(cue.kind == K::Handle ? Sound::Bolt : Sound::Reload);
        }
        if (state.timer <= 0) {
            state.mode = WeaponState::Idle;
            state.cueCount = 0;
            emit(EventKind::ReloadEnd, human.weapon(), {}, 0, human.slot);
        }
    }

    void hitRival(int slot, float amount, Cause cause, float zone, Vec3 point, int attacker = playerSlot) {
        auto& r = rival(slot);
        auto& bot = r.bot;
        if (bot.dead) return;
        bot.actor.health -= amount;
        bot.pendingAlert = true;
        r.killCause = cause;
        r.killZone = zone;
        r.attacker = attacker;
        r.lastHit = snapshot.scaledTime;
        r.retargetAt = 0;
        emit(EventKind::RivalHit, int(std::lround(zone * 100)), point, 0, slot, attacker);
        if (cause == Cause::Weapon && attacker == playerSlot) {
            snapshot.lethalMarker = bot.actor.health <= 0;
            snapshot.hitMarker = snapshot.lethalMarker ? .33f : .15f;
            sound(Sound::Hit, .65f);
        }
        if (bot.actor.health <= 0) {
            bot.dead = true;
            bot.radar = 0;
            bot.actor.health = 0;
            bot.actor.speed = 0;
            bot.velocity = {};
            r.death = {};
            r.death.variant = uint8_t(rivalRandom(slot).draw("rival.death.variant") * 3);
            bot.actor.fallVariant = r.death.variant;
        }
    }

    float rivalSurface(const Bot& bot, Vec3 origin, Vec3 dir, float before, float& zone) const {
        if (bot.dead) return infinity;
        return actorSurface(bot.actor, origin, dir, before, zone);
    }

    float actorSurface(const Actor& a, Vec3 origin, Vec3 dir, float before, float& zone,
                       float heightScale = 1.f) const {
        Vec3 p = intoActor(origin - a.position, a.yaw);
        Vec3 d = intoActor(dir, a.yaw);
        p.y /= heightScale;
        d.y /= heightScale;
        float near = 0;
        if (!intersects(p, d, {{-1, -.1f, -1}, {1, 2.2f, 1}}, near, before)) return infinity;
        float best = before;
        if (a.frame < assets.rivalHitboxes.size() && !assets.rivalHitboxes[a.frame].empty()) {
            const auto& boxes = assets.rivalHitboxes[a.frame];
            for (size_t i = 0; i < boxes.size(); ++i) {
                auto b = boxes[i];
                if (a.frameBlend > 0 && a.nextFrame < assets.rivalHitboxes.size() &&
                    i < assets.rivalHitboxes[a.nextFrame].size()) {
                    const auto& next = assets.rivalHitboxes[a.nextFrame][i];
                    const float mix = a.frameBlend;
                    b.center += (next.center - b.center) * mix;
                    b.half += (next.half - b.half) * mix;
                    b.right = normalized(b.right + (next.right - b.right) * mix);
                    b.forward = normalized(cross(b.right, b.up + (next.up - b.up) * mix));
                    if (dot(b.forward, next.forward) < 0) b.forward *= -1;
                    b.up = normalized(b.up + (next.up - b.up) * mix);
                    b.up = normalized(b.up - b.right * dot(b.up, b.right));
                }
                const Vec3 delta = p - b.center;
                const Vec3 localOrigin{dot(delta, b.right), dot(delta, b.up), dot(delta, b.forward)};
                const Vec3 localDir{dot(d, b.right), dot(d, b.up), dot(d, b.forward)};
                near = 0;
                if (intersects(localOrigin, localDir, {b.half * -1.f, b.half}, near, best) && near < best) {
                    best = near;
                    zone = b.multiplier;
                }
            }
        } else {
            const float body = cylinder(origin, dir, a.position, .31f, 1.92f * heightScale);
            if (body < best) {
                best = body;
                const float hitHeight = p.y + d.y * body;
                zone = hitHeight > 1.67f ? 2.f : hitHeight > .92f ? 1.5f : 1.f;
            }
        }
        return best < before ? best : infinity;
    }

    void fire(Human& human, Random& rng, Gun& state, Shot* pending) {
        auto& s = human.view;
        const int id = human.weapon();
        const auto& config = game_data::weapons[id];
        const bool automatic = id == int(Weapon::Mp44), sniper = id == int(Weapon::Sniper);
        const auto& kick = state.aiming ? config.ads : config.hip;
        --state.clip;
        human.playerFired = true;
        state.kick = kick.kick;
        state.bolt = 1;
        if (sniper) {
            state.mode = WeaponState::Bolt;
            state.timer = config.cycle;
            state.boltSound = false;
        } else {
            state.cooldown = config.interval;
            state.slideLocked = !automatic && state.clip == 0;
        }

        const Vec3 f = forward(s.camera.yaw, s.camera.pitch);
        const Vec3 n{std::cos(s.camera.yaw), 0, -std::sin(s.camera.yaw)};
        const Vec3 up = cross(f, n);
        const double ads = state.adsFrame / game_data::aimLast[id];
        const double hip = (20. / 225.) * std::tan(interpolateAim(id, state.adsFrame).fov * deg / 2);
        const double spread = hip + (config.spread - hip) * ads;
        const Vec3 p = s.camera.position;
        Vec3 d = f;
        if (spread > .0001) {
            d += n * float(rng.between("shot.spread.x", -spread, spread));
            d += up * float(rng.between("shot.spread.y", -spread, spread));
            d = normalized(d);
        }
        emit(EventKind::Fire, state.clip, p, 0, human.slot);
        if (sniper) emit(EventKind::BoltStart, id, {}, 0, human.slot);
        sound(weaponShots[id], 1.f, pending != nullptr, pending ? p : Vec3{});
        s.muzzle = 1;

        const Shot shot{p, d, float(config.damage)};
        if (pending) *pending = shot;
        else resolveSoloShot(shot);
        applyRecoil(human, rng, kick.pitch, kick.yaw, kick.kick);
        if (!sniper && state.clip == 0 && state.reserve > 0) startReload(human, state);
    }

    struct HostAdapter final : mission_detail::MissionHost {
        Impl& g;
        explicit HostAdapter(Impl& impl) : g(impl) {}
        void playSound(Sound id, float vol, bool pos, Vec3 at) override { g.sound(id, vol, pos, at); }
        void setCampaignVoice(CampaignVoiceCue cue) override { g.campaignVoice = cue; }
        void damagePlayer(float dmg, Vec3 from, [[maybe_unused]] bool partner) override {
#ifdef FOD_LOCAL_DUEL
            if (partner) {
                auto& human = *g.guest;
                if (g.hurtPlayer(human, g.localRandom[1], dmg, from) && human.view.dead)
                    g.emit(EventKind::PlayerDeath, int(Cause::Weapon), human.view.player.position, 0, human.slot, 0);
                return;
            }
#endif
            if (g.hurtPlayer(g.player, g.random, dmg, from) && g.snapshot.dead) {
                if (g.laptop == Phase::Raising || g.laptop == Phase::Out) g.closeLaptop();
                if (g.transmitter != Phase::Away) g.transmitter = Phase::Away;
                g.emit(EventKind::PlayerDeath, int(Cause::Weapon), g.snapshot.player.position, 0, playerSlot, 0);
            }
        }
        void hitWorld(const ShotHit& hit, Vec3 point) override { g.hitWorld(hit, point); }
        void updateActorFrame(Actor& a) override { g.updateActorFrame(a); }
        void equipSniper(Human& human) {
            if (human.view.weapon == Weapon::Sniper) return;
            g.cancelWeapon(human);
            human.previousWeapon = human.weapon();
            human.view.weapon = Weapon::Sniper;
            human.swap = Phase::Raising;
            human.switchTimer = 0;
            g.emit(EventKind::Swap, human.weapon(), {}, 0, human.slot);
        }
        void equipSniper() override {
            equipSniper(g.player);
#ifdef FOD_LOCAL_DUEL
            if (g.snapshot.campaignSeat == 0) equipSniper(*g.guest);
#endif
        }
        void refillPlayer(bool newMatch) override {
            g.refill(g.player, newMatch);
#ifdef FOD_LOCAL_DUEL
            if (g.snapshot.campaignSeat == 0) g.refill(*g.guest, newMatch);
#endif
        }
        void useMissionScene(ArenaScene scene) override {
            g.useGeometry(scene);
            g.paneHits.fill(0);
        }
        void transferPlayer(Vec3 position, float yaw) override {
            auto& s = g.snapshot;
            s.player.position = position;
            s.playerVelocity = {};
            s.player.yaw = yaw;
            s.playerYaw = yaw;
            s.playerPitch = 0;
            s.camera.position = atHeight(position, s.eyeHeight);
            s.camera.yaw = yaw;
            s.camera.pitch = 0;
            s.grounded = true;
            g.player.recoilPitch = g.player.recoilYaw = g.player.recoilKick = 0;
#ifdef FOD_LOCAL_DUEL
            if (s.campaignSeat == 0) g.placePartner(position, yaw);
#endif
        }
    };

    void startMission(uint32_t seed, int level, mission_detail::MissionStart mode,
                      MissionChapter replayChapter = MissionChapter::BriarHouse, bool finale = false, bool noraAlive = true,
                      [[maybe_unused]] bool coOp = false) {
#ifdef FOD_LOCAL_DUEL
        player.slot = playerSlot;
        if (!coOp) guest.reset();
        else {
            if (!guest) guest = std::make_unique<Human>();
            else *guest = {};
            guest->slot = 1;
            guest->view.campaignSeat = 1;
            localRandom[1] = {};
            const uint32_t stream = seed ^ 0x82f63b78u;
            localRandom[1].state = stream ? stream : 1u;
            refill(*guest, true);
            campaignBlockedActions = {};
            partnerCameraShared = false;
        }
#endif
        second.reset();
        if (!mission) mission = std::make_unique<mission_detail::MissionController>(assets, world, navigation);
        const bool remote = replayChapter == MissionChapter::Timberline ||
            (!finale && replayChapter == MissionChapter::BriarHouse && snapshot.mission &&
                ((mode == mission_detail::MissionStart::NextChapter && snapshot.mission->chapter == MissionChapter::BriarHouse) ||
                 (mode == mission_detail::MissionStart::Retry && snapshot.mission->chapter == MissionChapter::Timberline)));
        useGeometry(remote ? ArenaScene::Timberline : ArenaScene::Estate);
        if (finale) mission->seedFinaleReplay();
        else if (replayChapter != MissionChapter::BriarHouse) mission->seedChapterReplay(replayChapter, noraAlive);
        signalLower = 0;
        snapshot = {};
#ifdef FOD_LOCAL_DUEL
        if (coOp) snapshot.campaignSeat = 0;
#endif
        snapshot.match = Match::Playing;
        setDifficulty(level);
        random.state = seed ? seed : 1;
        random.tick = 0;
        eventCount = 0;
        campaignVoice.reset();
        paneHits.fill(0);
        refill(player, true);
        laptop = transmitter = Phase::Away;
        laptopTime = transmitterTime = 0;
        uavUntil = -1;
        strike = {};
        apache = {};
        player.death = {};
        player.respawn = 0;
        fixtureDeathTick = 0;
        player.blockFire = false;
        first.bot.dead = true;
        first.bot.actor.visible = false;

        Vec3 startPos = assets.spawns[0].position;
        float startYaw = assets.spawns[0].yaw;
        HostAdapter host(*this);
        mission->start(seed, snapshot.difficulty, mode, snapshot, startPos, startYaw, host);
        resetPlayer(player, startPos, startYaw);
        player.invulnerableUntil = 0;
        mission->publish(snapshot.mission);
        if (snapshot.mission && snapshot.mission->phase == MissionPhase::Intro) {
            snapshot.eyeHeight = proneEye;
            snapshot.prone = true;
            snapshot.camera.position = atHeight(startPos, proneEye);
        }
        viewWeapon(player, handheld());
        viewWorld();
#ifdef FOD_LOCAL_DUEL
        if (coOp) {
            placePartner(startPos, startYaw);
            player.blockFire = true;
            viewWeapon(*guest, false);
            publishPartner();
        }
#endif
    }

    void resolveCampaignShot(const Shot& shot, Snapshot& shooter) {
        const auto hit = world.missionShot(shot.origin, shot.direction, snapshot.brokenGlass);
        Vec3 hitPoint{};
        HostAdapter host(*this);
        if (!mission->resolvePlayerShot(shot.origin, shot.direction, shot.damage, hit.distance, shooter, host, hitPoint) &&
            hit.hit) {
            hitWorld(hit, shot.origin + shot.direction * hit.distance);
        }
    }

    void resolveSoloShot(const Shot& shot) {
        if (mission && mission->active()) {
            resolveCampaignShot(shot, snapshot);
            return;
        }
        const Vec3 p = shot.origin, d = shot.direction;
        const auto hit = world.shot(p, d, snapshot.brokenGlass);
        float zone = 1, distance = hit.distance;
        int victim = playerSlot;
        for (int slot = 0; slot < rivalCount(); ++slot) {
            const float body = rivalSurface(rival(slot).bot, p, d, distance, zone);
            if (body < distance) {
                distance = body;
                victim = slot;
            }
        }
        const Vec3 point = p + d * (victim != playerSlot || hit.hit ? distance : 100.f);
        if (victim != playerSlot) {
            hitRival(victim, shot.damage * zone, Cause::Weapon, zone, point);
        } else if (hit.hit) {
            hitWorld(hit, point);
        }
    }

    void hitWorld(const ShotHit& hit, Vec3 point) {
        if (hit.glass >= 0 && hit.glass < int(paneHits.size())) {
            const uint64_t pane = uint64_t(1) << hit.glass;
            if (snapshot.brokenGlass & pane) return;
            const int count = ++paneHits[hit.glass];
            const int needed = hit.glass < int(world.geometry().glassHits.size()) ? world.geometry().glassHits[hit.glass] : 1;
            emit(EventKind::GlassHit, count, point, hit.surface);
            sound(Sound::Glass, count < needed ? .55f : 1.f, true, point);
            if (count >= needed) {
                snapshot.brokenGlass |= pane;
                emit(EventKind::GlassBreak, hit.glass, point, hit.surface);
            }
        } else {
            emit(EventKind::WorldHit, 0, point, hit.surface);
            sound(Sound::Impact, .6f, true, point);
        }
    }

    bool handheld() const { return laptop != Phase::Away || transmitter != Phase::Away; }

    void weapons(Human& human, Random& rng, const Input& input, double dt, bool deviceOut, Shot* pending = nullptr) {
        auto& s = human.view;
        human.playerFired = false;
        const bool allowed = !s.dead && !deviceOut;
        if (allowed && human.swap == Phase::Away && input.edge(Swap)) {
            human.swap = Phase::Lowering;
            human.switchTimer = 0;
            cancelWeapon(human);
            sound(Sound::Reload, .45f);
        }
        if (human.swap == Phase::Lowering) {
            human.switchTimer += dt;
            if (human.switchTimer >= .22) {
                const int current = human.weapon();
                int next = (current + 1) % 3;
                if (mission && mission->active()) {
                    // Campaign: a tap returns to the previous weapon; holding Swap through the lower
                    // reaches the third. Duels and replays keep the plain cycle.
                    next = input.down(Swap) ? 3 - current - human.previousWeapon : human.previousWeapon;
                    if (next == current || next < 0 || next > 2) next = (current + 1) % 3;
                }
                human.previousWeapon = current;
                s.weapon = Weapon(next);
                human.swap = Phase::Raising;
                human.switchTimer = 0;
                emit(EventKind::Swap, human.weapon(), {}, 0, human.slot);
            }
        } else if (human.swap == Phase::Raising) {
            human.switchTimer += dt;
            if (human.switchTimer >= .3) human.swap = Phase::Away;
        }

        const bool enabled = allowed && human.swap == Phase::Away;
        const int id = human.weapon();
        auto& state = human.gun();
        const auto& config = game_data::weapons[id];
        const bool ready = state.mode == WeaponState::Idle;
        state.aiming = enabled && input.down(Aim) && ready;
        state.adsFrame = std::clamp(state.adsFrame + (state.aiming ? dt : -dt) * 60, 0., double(game_data::aimLast[id]));
        const double decay = id == int(Weapon::Mp44) ? 18 : id == int(Weapon::Pistol) ? 14 : 12;
        state.kick *= std::exp(-decay * dt);
        state.bolt *= std::exp(-(id == int(Weapon::Mp44) ? 34 : 26) * dt);
        state.cooldown -= dt;
        if (state.mode == WeaponState::Bolt) {
            state.timer -= dt;
            if (config.cycle - state.timer >= .4 && !state.boltSound) {
                sound(Sound::Bolt);
                state.boltSound = true;
            }
            if (state.timer <= 0) {
                state.mode = WeaponState::Idle;
                emit(EventKind::BoltEnd, id, {}, 0, human.slot);
            }
        } else if (state.mode == WeaponState::Reload) {
            updateCues(human, state, dt);
        }

        const bool trigger = state.mode == WeaponState::Idle && enabled;
        const bool pressed = trigger && input.edge(Fire);
        const bool held = trigger && id == int(Weapon::Mp44) && input.down(Fire);
        if (!ready || !held || pressed) state.burst = false;
        if (!trigger) return;
        if (!state.clip && state.reserve > 0) {
            startReload(human, state);
            return;
        }
        if (pressed || held) {
            if (state.clip > 0 && state.cooldown <= 0) {
                const double overshoot = state.burst ? state.cooldown : 0;
                fire(human, rng, state, pending);
                if (held) {
                    state.cooldown = std::max(0., state.cooldown + overshoot);
                    state.burst = true;
                }
            } else if (!state.clip && pressed) {
                emit(EventKind::Dry, human.weapon(), {}, 0, human.slot);
                sound(Sound::Dry);
                if (id == int(Weapon::Mp44)) state.cooldown = .2;
            }
        }
        if (state.mode == WeaponState::Idle && input.edge(Reload)) startReload(human, state);
    }

    bool hurtPlayer(Human& human, Random& rng, float amount, Vec3 from, int attacker = playerSlot) {
        auto& s = human.view;
        if (s.dead || s.scaledTime < human.invulnerableUntil) return false;
        s.player.health = std::max(0.f, s.player.health - amount);
        human.hurtAt = s.scaledTime;
        s.damage = std::min(1.f, s.damage + amount / 45);
        s.damageFromYaw = std::atan2(from.x - s.player.position.x, from.z - s.player.position.z);
        applyRecoil(human, rng, .55, 1.4, .012);
        emit(EventKind::PlayerHit, int(std::lround(amount)), from, 0, human.slot, attacker);
        sound(Sound::Hit, .6f);
        if (s.player.health > 0) return true;
        s.dead = true;
        s.streak = 0;
        s.player.visible = true;
        s.player.speed = 0;
        human.respawn = 3.2;
        return true;
    }

    void damagePlayer(float amount, Vec3 from, Cause cause, int attacker = 0) {
        auto& s = snapshot;
        if (!hurtPlayer(player, random, amount, from) || !s.dead) return;
        if (!second || cause != Cause::Airstrike) ++rivalScore(attacker);
        if (laptop == Phase::Raising || laptop == Phase::Out) closeLaptop();
        if (transmitter != Phase::Away) {
            if (apache.phase == Flight::Off) s.apacheReady = true;
            transmitter = Phase::Away;
        }
        emit(EventKind::PlayerDeath, int(cause), s.player.position, 0, playerSlot,
             cause == Cause::Airstrike ? playerSlot : attacker);
        if (cause != Cause::Airstrike) taunt(attacker, true);
    }

    float seatClear(const Snapshot& s, float yaw, float distance, float height) const {
        const auto p = s.player.position;
        const Vec3 dir{std::sin(yaw) * distance, height - 1.f, std::cos(yaw) * distance};
        const float len = length(dir);
        if (len < .001f) return 0;
        const float reach = len + .45f;
        return std::min(reach, world.ray(atHeight(p, 1), dir / len, reach));
    }

    void beginDeath(Human& human, Random& rng, bool slowMotion) {
        auto& s = human.view;
        constexpr float seats[][3] = {
            {.6f, 2.4f, 1.25f}, {-.6f, 2.4f, 1.25f}, {1.35f, 2.3f, 1.2f}, {-1.35f, 2.3f, 1.2f},
            {.15f, 2.7f, 1.5f}, {2.4f, 2.4f, 1.3f}, {-2.4f, 2.4f, 1.3f}, {pi, 2.6f, 1.45f}
        };
        human.death = {};
        human.death.origin = s.camera;
        human.death.variant = uint8_t(rng.draw("player.death.variant") * 3);
        s.player.fallVariant = human.death.variant;
        float clearance = -1;
        const float* selected = seats[0];
        for (const auto& seat : seats) {
            const float clear = seatClear(s, s.playerYaw + seat[0], seat[1], seat[2]);
            if (clear > clearance) {
                selected = seat;
                clearance = clear;
            }
            if (clear >= std::hypot(seat[1], seat[2] - 1) + .45f) {
                selected = seat;
                clearance = clear;
                break;
            }
        }
        human.death.yaw = s.playerYaw + selected[0];
        human.death.distance = std::clamp(clearance - .45f, .9f, selected[1]);
        human.death.height = std::max(.5f, selected[2] * human.death.distance / selected[1]);
        human.death.liveDistance = human.death.distance;
        if (slowMotion) s.timeScale = .5;
    }

    void playFall(Death& d, Actor& actor, double dt, bool player) {
        const double t = d.elapsed += dt;
        const int variant = d.variant;
        if (t >= gaspAt[variant] && t - dt < gaspAt[variant]) sound(Sound::Death, 1, !player, actor.position);
        if (t >= fallAt[variant] && t - dt < fallAt[variant]) sound(Sound::Footstep, .8f, !player, actor.position);
        const double k = std::clamp((t - fallStart[variant]) / (fallAt[variant] - fallStart[variant]), 0., 1.);
        actor.fall = float(k * k * (1.6 - .6 * k));
    }

    void deathCamera(Human& human, double dt, bool slowMotion) {
        auto& s = human.view;
        playFall(human.death, s.player, dt, slowMotion);
        const double t = human.death.elapsed, impact = fallAt[human.death.variant];
        if (slowMotion) s.timeScale = .5 + .5 * std::clamp((t - impact) / .5, 0., 1.);
        const float ease = smooth(t / 1.1);
        const double drift = std::max(0., t - 1.1);
        const float yaw = human.death.yaw + float(drift * .16);
        const float wanted = human.death.distance - float(std::min(.5, drift * .12));
        const float height = std::max(.6f, human.death.height - float(std::min(.35, drift * .1)));
        const float clear = seatClear(s, yaw, wanted, height);
        const float allowed = std::max(.8f, std::min(wanted, clear - .45f));
        human.death.liveDistance += (allowed - human.death.liveDistance) * float(1 - std::exp(-8 * dt));
        human.death.liveDistance = std::min(human.death.liveDistance, allowed);
        const Vec3 seat = s.player.position + Vec3{
            std::sin(yaw) * human.death.liveDistance + float(std::sin(t * 1.7) * .02 * ease),
            height + float(std::sin(t * 1.1) * .015 * ease), std::cos(yaw) * human.death.liveDistance
        };
        s.camera.position = human.death.origin.position + (seat - human.death.origin.position) * ease;
        const float blend = std::clamp(float((t - .6) / .9), 0.f, 1.f);
        const float head = 1.62f - 1.35f * s.player.fall;
        const Vec3 look = atHeight(s.player.position, head + (.45f - head) * blend) - s.camera.position;
        const float facing = std::atan2(look.x, look.z);
        const float pitch = -std::atan2(look.y, std::max(.001f, planar(look)));
        const float steering = smooth(t / .45);
        s.camera.pitch = human.death.origin.pitch + (pitch - human.death.origin.pitch) * steering;
        s.camera.yaw = human.death.origin.yaw + float(angleDelta(human.death.origin.yaw, facing)) * steering;
    }

    void closeLaptop() {
        if (laptop == Phase::Away || laptop == Phase::Lowering) return;
        laptop = Phase::Lowering;
        laptopTime = 0;
        emit(EventKind::LaptopClose);
        sound(Sound::Laptop, .75f);
    }

    void markStrike() {
        auto& s = snapshot;
        const double now = s.scaledTime;
        const Vec3 point{s.cursorX, 0, s.cursorZ};
        s.airstrikeReady = false;
        closeLaptop();
        emit(EventKind::StrikeMark, 0, point);
        sound(Sound::Airstrike, .45f);
        const double angle = random.draw("airstrike.heading") * (360 * deg);
        strike.direction = {float(std::sin(angle)), 0, float(std::cos(angle))};
        const Vec3 perpendicular{strike.direction.z, 0, -strike.direction.x};
        // Each delivery has a leading flyby and three bombs in its own lane.
        for (int i = 0; i < 3; ++i) {
            const double pass = now + 2.5 + i * .8;
            const Vec3 lane = point + perpendicular * float((i - 1) * 2.7);
            strike.launches[i] = pass - 1;
            strike.expires[i] = pass + 1;
            strike.origins[i] = atHeight(lane - strike.direction * 60, 26 + 2.5f * i);
            for (int j = 0; j < 3; ++j) {
                Vec3 drop = lane + strike.direction * float((j - 1) * 3.4);
                drop.x += float(random.between("airstrike.bomb.x", -.8, .8));
                drop.z += float(random.between("airstrike.bomb.z", -.8, .8));
                const double at = pass + j * .14;
                strike.bombs[i * 3 + j] = {at, drop};
                strike.latestBomb = at;
            }
        }
    }

    void explode(Vec3 point) {
        auto& s = snapshot;
        emit(EventKind::StrikeImpact, 0, point);
        sound(Sound::Airstrike, 1, true, point);
        const float playerDistance = planar(s.player.position - point);
        applyRecoil(player, random, std::min(2.6, 7 / (1 + playerDistance * .7)), 4, std::min(.055, .16 / (1 + playerDistance * .5)));
        const Vec3 eye = atHeight(point, 1.1f);
        if (mission && mission->active()) {
            HostAdapter host(*this);
            mission->explodeAliens(point, 6.2f, snapshot, host);
        } else {
            for (int slot = 0; slot < rivalCount(); ++slot) {
                const auto& bot = rival(slot).bot;
                if (bot.dead) continue;
                const float d = planar(bot.actor.position - point);
                if (d <= 5.6f && !world.blocked(eye, atHeight(bot.actor.position, 1.3f))) {
                    hitRival(slot, 165 * (1 - d / 6.8f), Cause::Airstrike, 1, bot.actor.position);
                }
            }
        }
        if (!s.dead && playerDistance <= 5.6f && !world.blocked(eye, atHeight(s.player.position, s.eyeHeight))) {
            damagePlayer(165 * (1 - playerDistance / 6.8f), point, Cause::Airstrike);
        }
    }

    void deployApache() {
        auto& s = snapshot;
        s.apacheReady = false;
        apache = {};
        apache.phase = Flight::Enter;
        apache.until = s.scaledTime + 40;
        apache.pos = {38, 19, 38};
        apache.yaw = -135 * deg;
        apache.angle = random.draw("apache.orbit") * 360 * deg;
        emit(EventKind::ApacheLaunch, 0, apache.pos);
        sound(Sound::Airstrike, .4f);
    }

    void apacheRound(Vec3 gunPos, int slot) {
        if (mission && mission->active() && snapshot.mission) {
            if (slot < 4 || slot >= int(MissionSnapshot::maxActors)) return;
            const auto& ma = snapshot.mission->actors[slot];
            if (!ma.active || ma.dead) return;
            const Vec3 target = ma.role == MissionRole::AlienDrone ? ma.actor.position : atHeight(ma.actor.position, 1.15f);
            Vec3 dir = target - gunPos;
            dir.x += float(random.between("apache.shot.x", -.4, .4));
            dir.y += float(random.between("apache.shot.y", -.28, .28));
            dir.z += float(random.between("apache.shot.z", -.4, .4));
            dir = normalized(dir);
            const float worldHit = world.ray(gunPos, dir, 80);
            Vec3 point{};
            HostAdapter host(*this);
            const bool hit = mission->apacheShotAlien(slot, gunPos, dir, worldHit, 16.f, snapshot, host, point);
            emit(EventKind::ApacheAttack, hit ? 1 : 0, point);
            sound(Sound::Mp44, .7f, true, gunPos);
            return;
        }
        const auto& bot = rival(slot).bot;
        const Vec3 target = atHeight(bot.actor.position, 1.1f);
        Vec3 dir = target - gunPos;
        dir.x += float(random.between("apache.shot.x", -.4, .4));
        dir.y += float(random.between("apache.shot.y", -.28, .28));
        dir.z += float(random.between("apache.shot.z", -.4, .4));
        dir = normalized(dir);
        const float worldHit = world.ray(gunPos, dir, 80);
        const float bodyHit = cylinder(gunPos, dir, bot.actor.position, .45f, 1.95f);
        const bool hit = bodyHit < worldHit;
        const Vec3 point = gunPos + dir * (hit ? bodyHit : std::min(80.f, worldHit));
        emit(EventKind::ApacheAttack, hit ? 1 : 0, point);
        sound(Sound::Mp44, .7f, true, gunPos);
        tracer(gunPos, point);
        if (hit) hitRival(slot, 9, Cause::Apache, 1, point);
    }

    Vec3 apacheGun() const {
        constexpr float y = -.52f, z = 1.35f;
        const float c = float(std::cos(apache.pitch)), s = float(std::sin(apache.pitch));
        const float rolledX = -y * float(std::sin(apache.roll));
        const float rolledY = y * float(std::cos(apache.roll));
        const Vec3 localGun{rolledX, c * rolledY - s * z, s * rolledY + c * z};
        return apache.pos + intoActor(localGun, float(-apache.yaw));
    }

    int apacheTarget() const {
        if (mission && mission->active()) return mission->nearestAlienForApache(apacheGun());
        if (!second) return first.bot.dead ? playerSlot : 0;
        const Vec3 gun = apacheGun();
        float best = infinity;
        int target = playerSlot;
        for (int slot = 0; slot < rivalCount(); ++slot) {
            const auto& bot = rival(slot).bot;
            if (bot.dead) continue;
            const Vec3 aim = atHeight(bot.actor.position, 1.2f);
            const float score = length(aim - gun) * (world.blocked(gun, aim) ? 2.5f : 1.f);
            if (score < best) {
                best = score;
                target = slot;
            }
        }
        return target;
    }

    void updateApache(double now, double dt) {
        auto& a = apache;
        if (a.phase == Flight::Off) return;
        const bool inMission = mission && mission->active() && snapshot.mission;
        const auto targetAlive = [&](int slot) -> bool {
            if (slot == playerSlot) return false;
            if (inMission) return slot >= 4 && slot < int(MissionSnapshot::maxActors) &&
                                  snapshot.mission->actors[slot].active && !snapshot.mission->actors[slot].dead;
            return !rival(slot).bot.dead;
        };
        const auto targetPos = [&](int slot) -> Vec3 {
            if (inMission) return snapshot.mission->actors[slot].actor.position;
            return rival(slot).bot.actor.position;
        };
        if (a.phase == Flight::Hunt && (now >= a.retargetAt || !targetAlive(a.target))) {
            const int target = apacheTarget();
            if ((second || inMission) && a.target != target) a.rounds = 0;
            a.target = target;
            a.retargetAt = now + .7;
        }
        const bool tracking = targetAlive(a.target);
        const Vec3 trackPos = tracking ? targetPos(a.target) : Vec3{};
        Vec3 desired{0, 12.5f + float(std::sin(now * .8) * .5), 0};
        if (a.phase == Flight::Leave) {
            desired = {46, 24, -46};
        } else if (tracking) {
            a.angle += .32 * dt;
            desired.x = trackPos.x + float(std::sin(a.angle) * 8.5);
            desired.z = trackPos.z + float(std::cos(a.angle) * 8.5);
        } else {
            a.angle += .22 * dt;
            desired.x = float(std::sin(a.angle) * 11);
            desired.z = float(std::cos(a.angle) * 11);
        }
        Vec3 velocity = desired - a.pos;
        const float dist = length(velocity);
        if (dist > .01f) velocity *= (a.phase == Flight::Leave ? 17.1f : std::min(9.5f, dist * .9f)) / dist;
        a.vel += (velocity - a.vel) * float(1 - std::exp(-1.7 * dt));
        a.pos += a.vel * float(dt);
        if (a.phase == Flight::Enter &&
            ((inMission && planar(a.pos - snapshot.player.position) < 34.f) ||
             (std::abs(a.pos.x) < 18 && std::abs(a.pos.z) < 18))) {
            a.phase = Flight::Hunt;
            a.retargetAt = 0;
        }
        double wantedYaw = a.yaw;
        if (a.phase == Flight::Hunt && tracking) {
            const Vec3 target = trackPos - a.pos;
            wantedYaw = std::atan2(target.x, target.z);
        } else if (dot(a.vel, a.vel) > .5f) {
            wantedYaw = std::atan2(a.vel.x, a.vel.z);
        }
        const double yawDelta = angleDelta(a.yaw, wantedYaw);
        a.yaw += yawDelta * (1 - std::exp(-2.2 * dt));
        const double fwdSpeed = a.vel.x * std::sin(a.yaw) + a.vel.z * std::cos(a.yaw);
        const double tilt = 1 - std::exp(-3 * dt);
        a.pitch += (std::clamp(fwdSpeed * .022, -.22, .22) - a.pitch) * tilt;
        a.roll += (std::clamp(-yawDelta * .5, -.3, .3) - a.roll) * tilt;
        a.timer -= dt;
        if (a.phase == Flight::Hunt && tracking) {
            const Vec3 gun = apacheGun();
            const Vec3 aim = inMission && snapshot.mission->actors[a.target].role == MissionRole::AlienDrone
                ? trackPos : atHeight(trackPos, 1.2f);
            if (a.rounds > 0) {
                if (a.timer <= 0) {
                    apacheRound(gun, a.target);
                    a.timer = .07;
                    if (--a.rounds == 0) a.burstAt = now + .9 + random.draw("apache.burst.cooldown") * .6;
                }
            } else if (now >= a.burstAt && !world.blocked(gun, aim)) {
                a.rounds = 10;
            }
        } else {
            a.rounds = 0;
        }
        if (a.phase == Flight::Hunt && now >= a.until) {
            a.phase = Flight::Leave;
            a.target = playerSlot;
        }
        if (a.phase == Flight::Leave && (std::abs(a.pos.x) > 44 || std::abs(a.pos.z) > 44)) a.phase = Flight::Off;
    }

    void devices(const Input& input, double dt) {
        auto& s = snapshot;
        const bool wasHandheld = handheld();
        const bool laptopOpen = laptop == Phase::Raising || laptop == Phase::Out;
        if (input.edge(Streak)) {
            if (s.apacheReady && apache.phase == Flight::Off && !s.dead && !laptopOpen && transmitter == Phase::Away) {
                transmitter = Phase::Raising;
                transmitterTime = 0;
                signalSent = false;
                sound(Sound::Laptop, .8f);
            } else if (laptopOpen) {
                closeLaptop();
            } else if (s.airstrikeReady && !s.dead && transmitter == Phase::Away) {
                laptop = Phase::Raising;
                laptopTime = 0;
                s.cursorX = s.cursorZ = 0;
                emit(EventKind::LaptopOpen);
                sound(Sound::Laptop, .85f);
            }
        }
        if (laptop == Phase::Raising || laptop == Phase::Lowering) {
            laptopTime += dt;
            if (laptopTime >= (laptop == Phase::Raising ? .3 : .22)) {
                laptop = laptop == Phase::Raising ? Phase::Out : Phase::Away;
            }
        }
        if (laptop == Phase::Out) {
            constexpr float cursorGain = 31.2f * .62f / (272.f * .0023f);
            s.cursorX = world.clampX(s.cursorX + input.lookX * cursorGain);
            s.cursorZ = world.clampZ(s.cursorZ - input.lookY * cursorGain);
            if (input.edge(Fire)) markStrike();
        }
        if (transmitter != Phase::Away) {
            transmitterTime += dt;
            if (transmitter == Phase::Raising && transmitterTime >= .22) {
                transmitter = Phase::Out;
                transmitterTime = 0;
            } else if (transmitter == Phase::Out) {
                if (!signalSent && transmitterTime >= .52 * .48) {
                    signalSent = true;
                    deployApache();
                }
                if (transmitterTime >= .52) {
                    transmitter = Phase::Lowering;
                    transmitterTime = 0;
                }
            } else if (transmitter == Phase::Lowering && transmitterTime >= .24) {
                transmitter = Phase::Away;
            }
        }
        player.lookLocked = laptop == Phase::Raising || laptop == Phase::Out;
        if (!wasHandheld && handheld()) cancelWeapon(player);

        const double now = s.scaledTime;
        for (int i = 2; i >= 0; --i) {
            const double at = strike.launches[i];
            if (at > 0 && now >= at && now - dt < at) sound(Sound::Airstrike, .55f, true, strike.origins[i]);
        }
        for (int i = 8; i >= 0; --i) {
            Bomb& bomb = strike.bombs[i];
            if (bomb.at > 0 && now >= bomb.at) {
                bomb.at = 0;
                explode(bomb.pos);
            }
        }
        updateApache(now, dt);
    }

    void registerKill(int slot) {
        auto& s = snapshot;
        auto& r = rival(slot);
        r.counted = true;
        r.respawn = 5;
        if (r.attacker != playerSlot) {
            ++rivalScore(r.attacker);
            emit(EventKind::BotKill, 0, r.bot.actor.position, 0, slot, r.attacker);
            taunt(r.attacker, true);
            return;
        }
        ++s.playerScore;
        ++s.streak;
        s.killConfirm = 1.4f;
        s.headshot = r.killZone >= 2;
        emit(EventKind::RivalKill, int(r.killCause) * 10 + (s.headshot ? 1 : 0), r.bot.actor.position, 0, slot);
        sound(s.headshot ? Sound::HeadshotConfirm : Sound::KillConfirm, 1.05f);
        taunt(slot, false);
        if (s.streak == 3) {
            uavUntil = s.scaledTime + 30;
            emit(EventKind::Uav);
            sound(Sound::Uav);
        } else if (s.streak == 5) {
            s.airstrikeReady = true;
            emit(EventKind::AirstrikeReady);
        } else if (s.streak == 7) {
            s.apacheReady = true;
            emit(EventKind::ApacheReady);
        }
    }

    void tracer(Vec3 from, Vec3 to) {
        snapshot.tracerStart = from;
        snapshot.tracerEnd = to;
        snapshot.tracer = 1;
    }

    void retargetRival(int slot) {
        auto& r = rival(slot);
        const auto now = snapshot.scaledTime;
        if (now < r.retargetAt && !combatTarget(r.target).dead) return;
        r.retargetAt = now + .5;
        const int other = 1 - slot;
        int next = r.target;
        if (now - r.lastHit < 1.5 && !combatTarget(r.attacker).dead) {
            next = r.attacker;
        } else {
            const Vec3 eye = atHeight(r.bot.actor.position, botEye);
            float best = infinity;
            for (int candidate : {playerSlot, other}) {
                const auto enemy = combatTarget(candidate);
                if (enemy.dead) continue;
                const bool blocked = world.blocked(eye, atHeight(enemy.position, enemy.eye));
                const float score = planar(enemy.position - eye) * (blocked ? 2.5f : 1.f) *
                    (candidate == r.target ? .75f : 1.f);
                if (score < best) {
                    best = score;
                    next = candidate;
                }
            }
        }
        if (next != r.target) {
            r.target = next;
            r.bot.retarget();
        }
    }

    Vec3 rivalSeparation(double dt) const {
        if (!second || first.bot.dead || second->bot.dead || dt < .0001) return {};
        Vec3 offset = second->bot.actor.position - first.bot.actor.position;
        if (std::abs(offset.y) >= 1.7f) return {};
        offset.y = 0;
        const float distance = planar(offset);
        if (distance >= .9f) return {};
        const Vec3 direction = distance > .001f ? offset / distance : Vec3{1, 0, 0};
        return direction * float((.9f - distance) / (2 * dt));
    }

    void resolveBotShot(int slot, const BotRound& round) {
        auto& r = rival(slot);
        r.lastShot = snapshot.scaledTime;
        emit(EventKind::BotFire, int(round.sound), round.muzzle, 0, slot);
        sound(round.sound, .88f, true, round.muzzle);

        constexpr float range = 60;
        ShotHit worldHit;
        if (assets.arena == Arena::BriarHouse) worldHit = world.shot(round.muzzle, round.direction, snapshot.brokenGlass);
        else worldHit.distance = world.ray(round.muzzle, round.direction, range);
        float distance = std::min(range, worldHit.distance);
        int victim = slot;
        for (int other = playerSlot; other < rivalCount(); ++other) {
            if (other == slot) continue;
            const auto fighter = combatTarget(other);
            if (fighter.dead) continue;
            const float body = cylinder(round.muzzle, round.direction, fighter.position, .42f, fighter.eye + .15f);
            if (body < distance) {
                distance = body;
                victim = other;
            }
        }
        const Vec3 end = round.muzzle + round.direction * distance;
        if (second) snapshot.rivalTracers[slot] = {round.muzzle, end, 1};
        else tracer(round.muzzle, end);
        if (victim == playerSlot) {
            damagePlayer(round.damage, r.bot.actor.position, Cause::Weapon, slot);
        } else if (victim != slot) {
            hitRival(victim, round.damage, Cause::Weapon, 1, end, slot);
            if (rival(victim).bot.dead) registerKill(victim);
        } else if (worldHit.glass >= 0 && worldHit.distance <= range) {
            hitWorld(worldHit, end);
        }
    }

    void botStep(int slot, double dt, Vec3 separation) {
        auto& r = rival(slot);
        auto& bot = r.bot;
        const auto now = snapshot.scaledTime;
        if (second && !bot.dead && !bot.freeze) retargetRival(slot);
        const BotRound round = bot.update(combatTarget(r.target, dt), difficulty(slot), now, dt, separation);
        if (round.fired) resolveBotShot(slot, round);
        if (bot.dead) {
            playFall(r.death, bot.actor, dt, false);
            r.respawn -= dt;
            if (r.respawn <= 0) {
                const auto& spawn = chooseSpawn(slot);
                const Vec3 pos = spawn.position;
                resetRival(slot, pos, respawnYaw(spawn), false);
                const double skill = difficulty(slot).aggression;
                if (!snapshot.dead && rivalRandom(slot).draw("bot.spawn.remember") < .45 + skill * .55) {
                    bot.spawnIntuition(target(), 2.5 + (1 - skill) * 9, now);
                }
                emit(EventKind::RivalRespawn, 0, pos, 0, slot);
            }
        } else if (bot.actor.speed > .35f) {
            const float speed = bot.actor.speed;
            r.footstep -= dt;
            if (r.footstep <= 0) {
                sound(Sound::Footstep, footstepGain * .4f, true, bot.actor.position);
                r.footstep += speed > 4.8f ? .34 : .45;
            }
        } else {
            r.footstep = 0;
        }
        updateActorFrame(bot.actor);
    }

    void updateActorFrame(Actor& actor) const {
        actor.frame = actor.nextFrame = 0;
        actor.frameBlend = 0;
        if (actor.fall > 0 || actor.speed < .4f) return;
        const bool run = actor.speed >= 3.1f && assets.runFrameCount > 0;
        const unsigned count = run ? assets.runFrameCount : assets.walkFrameCount;
        if (!count) return;
        const float sample = actor.phase * count;
        const unsigned i = std::min(unsigned(sample), count - 1);
        const uint16_t first = uint16_t(1 + (run ? assets.walkFrameCount : 0));
        actor.frame = uint16_t(first + i);
        actor.nextFrame = uint16_t(first + (i + 1) % count);
        actor.frameBlend = sample - i;
    }

    void viewWorld() {
        auto& s = snapshot;
        const auto& bot = first.bot;
        s.rival = bot.actor;
        s.apachePosition = apache.pos;
        s.apacheYaw = float(apache.yaw);
        s.apachePitch = float(apache.pitch);
        s.apacheRoll = float(apache.roll);
        s.apacheActive = apache.phase != Flight::Off;
        s.uavSeconds = float(std::max(0., uavUntil - s.scaledTime));
        s.radarContact = !bot.dead && (bot.radar > 0 || s.uavSeconds > 0);
        if (second) {
            s.secondRival = second->bot.actor;
            s.secondRadarContact = !second->bot.dead && (second->bot.radar > 0 || s.uavSeconds > 0);
        }
        s.strikeInbound = s.scaledTime < strike.latestBomb;
        for (int i = 0; i < 3; ++i) {
            const double age = s.scaledTime - strike.launches[i];
            s.jets[i] = {strike.origins[i] + strike.direction * float(age * 72),
                std::atan2(strike.direction.x, strike.direction.z), age >= 0 && s.scaledTime < strike.expires[i]};
        }
        if (transmitter != Phase::Away) {
            s.device = Device::Transmitter;
            s.deviceAmount = transmitter == Phase::Raising ? smooth(transmitterTime / .22) :
                transmitter == Phase::Lowering ? 1 - smooth(transmitterTime / .24) : 1.f;
        } else if (laptop != Phase::Away) {
            s.device = Device::Laptop;
            s.deviceAmount = laptop == Phase::Raising ? smooth(laptopTime / .3) :
                laptop == Phase::Lowering ? 1 - smooth(laptopTime / .22) : 1.f;
        } else {
            s.device = Device::None;
            s.deviceAmount = 0;
        }
    }

    void viewWeapon(Human& human, bool deviceOut) {
        auto& s = human.view;
        const int id = human.weapon();
        const auto& state = human.gun();
        const auto aim = interpolateAim(id, state.adsFrame);
        s.ads = float(state.adsFrame / game_data::aimLast[id]);
        s.camera.fov = float(aim.fov * deg);
        s.scope = s.dead || deviceOut ? 0 : float(aim.scope);
        s.weaponPosition = {float(aim.position[0]), float(aim.position[1]), float(aim.position[2])};
        s.weaponRotation = {float(aim.rotation[0]), float(aim.rotation[1]), float(aim.rotation[2])};
        s.recoil = float(state.kick);
        s.clip = state.clip;
        s.reserve = state.reserve;
        s.weaponState = state.mode;
        s.emptyReload = state.mode == WeaponState::Reload && state.emptyReload;
        s.reloadDuration = state.mode == WeaponState::Reload ? float(state.duration) : 0.f;
        s.weaponAnimationTime = state.mode == WeaponState::Reload ? float(state.duration - state.timer)
            : state.mode == WeaponState::Bolt ? float(game_data::weapons[id].cycle - state.timer) : 0.f;
        s.reload = state.mode == WeaponState::Reload ? float(1 - state.timer / state.duration) : 0;
        if (state.mode == WeaponState::Bolt) {
            const float cycle = float(1 - state.timer / game_data::weapons[id].cycle);
            s.bolt = smooth((cycle - .2) / .27) * (1 - smooth((cycle - .63) / .27));
        } else {
            s.bolt = state.slideLocked ? 1.f : float(state.bolt);
        }
        s.lower = human.swap == Phase::Lowering ? float(std::min(1., human.switchTimer / .22)) :
            human.swap == Phase::Raising ? float(std::max(0., 1 - human.switchTimer / .3)) : 0.f;
    }

    bool matchPointReached() const {
        return snapshot.playerScore >= matchTarget || snapshot.leadingRivalScore() >= matchTarget;
    }

    void finish() {
        auto& s = snapshot;
        s.match = Match::Results;
        s.timeScale = 1;
        laptop = transmitter = Phase::Away;
        apache.phase = Flight::Off;
        s.scope = 0;
        player.lookLocked = false;
        const int leadingRival = s.leadingRivalScore();
        const int result = s.playerScore >= matchTarget ? 1 : leadingRival >= matchTarget ? -1 :
            (s.playerScore > leadingRival) - (s.playerScore < leadingRival);
        emit(EventKind::MatchEnd, result);
    }

    void pause() {
        if (snapshot.match != Match::Playing) return;
        snapshot.match = Match::Paused;
        resetInput(player);
        eventCount = 0;
        player.blockFire = true;
#ifdef FOD_LOCAL_DUEL
        if (snapshot.campaignSeat == 0) {
            guest->view.match = Match::Paused;
            resetInput(*guest);
            guest->blockFire = true;
        }
#endif
    }

    void releaseFire(Human& human, Input& input) {
        if (!human.blockFire) return;
        if (!input.down(Fire) && !input.edge(Fire)) human.blockFire = false;
        input.held &= ~uint32_t(Fire);
        input.pressed &= ~uint32_t(Fire);
    }

#ifdef FOD_LOCAL_DUEL
    Human& localPlayer(int slot) { return slot == 0 ? player : *guest; }

    Vec3 partnerSpawn(Vec3 position, float yaw) const {
        const Vec3 ahead = forward(yaw, 0.f), right{ahead.z, 0.f, -ahead.x};
        for (const auto offset : {right, right * -1.f, ahead * -1.f, ahead}) {
            Vec3 at = position + offset * 1.3f;
            at.x = world.clampX(at.x);
            at.z = world.clampZ(at.z);
            at.y = world.groundHeight(at);
            if (std::abs(at.y - position.y) < .8f && world.canStand(at, standEye) &&
                !world.blocked(atHeight(position, 1.f), atHeight(at, 1.f))) return at;
        }
        return position;
    }

    void placePartner(Vec3 position, float yaw) {
        const float health = guest->view.dead ? 100.f : guest->view.player.health;
        resetPlayer(*guest, partnerSpawn(position, yaw), yaw);
        guest->view.player.health = health;
        guest->blockFire = true;
        guest->death = {};
        scoutLookYaw = scoutLookPitch = 0.f;
    }

    void publishPartner() {
        auto& s = snapshot;
        auto& p = guest->view;
        p.mission = s.mission;
        p.match = s.match;
        p.difficulty = s.difficulty;
        p.brokenGlass = s.brokenGlass;
        p.apachePosition = s.apachePosition;
        p.apacheYaw = s.apacheYaw;
        p.apachePitch = s.apachePitch;
        p.apacheRoll = s.apacheRoll;
        p.apacheActive = s.apacheActive;
        p.jets = s.jets;
        const bool flying = s.mission->operation.stage == OperationStage::ScoutFlight;
        const bool sharedCamera = flying || s.mission->cinematic || s.mission->phase == MissionPhase::Intro;
        if (sharedCamera) {
            p.camera = s.camera;
            if (flying) {
                p.camera.yaw += scoutLookYaw;
                p.camera.pitch = std::clamp(p.camera.pitch + scoutLookPitch, -1.4f, 1.4f);
            }
            p.lower = s.lower;
            p.scope = 0.f;
        } else {
            // The intro or a scene transfer can release both views between simulation ticks.
            if (partnerCameraShared && !p.dead) {
                p.camera.position = atHeight(p.player.position, p.eyeHeight);
                p.camera.yaw = p.playerYaw;
                p.camera.pitch = p.playerPitch;
            }
            viewWeapon(*guest, false);
        }
        partnerCameraShared = sharedCamera;
        s.rival = p.player;
        p.rival = s.player;
        s.rival.visible = p.rival.visible = !sharedCamera;
        s.rivalHeightScale = p.dead ? 1.f : p.eyeHeight / standEye;
        p.rivalHeightScale = s.dead ? 1.f : s.eyeHeight / standEye;
    }

    void rejoinPartner(double dt) {
        if (snapshot.mission->phase == MissionPhase::Failed) return;
        for (int slot = 0; slot < 2; ++slot) {
            auto& downed = localPlayer(slot);
            const auto& survivor = localPlayer(1 - slot).view;
            if (!downed.view.dead || survivor.dead) continue;
            downed.respawn -= dt;
            if (downed.respawn > 0 || !survivor.grounded) continue;
            const Vec3 at = partnerSpawn(survivor.player.position, survivor.playerYaw);
            if (!world.canStand(at, standEye)) continue;
            resetPlayer(downed, at, survivor.playerYaw);
            cancelWeapon(downed);
            downed.death = {};
            downed.blockFire = true;
            emit(EventKind::PlayerRespawn, 0, at, 0, downed.slot);
        }
    }

    void advancePartnerClock(double dt, double wallDelta) {
        auto& p = guest->view;
        p.tick = localRandom[1].tick = snapshot.tick;
        p.realTime = snapshot.realTime;
        p.scaledTime = snapshot.scaledTime;
        p.scaledDelta = dt;
        p.muzzle = std::max(0.f, p.muzzle - float(dt * 17));
        p.hitMarker = std::max(0.f, p.hitMarker - float(wallDelta));
        p.killConfirm = std::max(0.f, p.killConfirm - float(wallDelta));
    }

    bool stepCampaignPartner(Input input, double dt) {
        auto& human = *guest;
        auto& p = human.view;
        releaseFire(human, input);
        moveCampaignPlayer(human, input, dt, false);
        Shot shot{};
        weapons(human, localRandom[1], input, dt, false, &shot);
        if (shot.damage > 0.f) resolveCampaignShot(shot, p);
        return human.playerFired;
    }

    void publishDuel() {
        for (int slot = 0; slot < 2; ++slot) {
            auto& human = localPlayer(slot);
            auto& s = human.view;
            const auto& opponent = localPlayer(1 - slot).view;
            s.rival = opponent.player;
            s.rival.visible = true;
            s.rivalHeightScale = opponent.dead ? 1.f : opponent.eyeHeight / standEye;
            s.rivalScore = opponent.playerScore;
            s.rivalTracers[0] = {opponent.tracerStart, opponent.tracerEnd, opponent.tracer};
            s.brokenGlass = snapshot.brokenGlass;
            viewWeapon(human, false);
        }
    }

    void startDuel(uint32_t seed) {
        useGeometry(ArenaScene::Estate);
        player = {};
        if (guest) *guest = {};
        else guest = std::make_unique<Human>();
        mission.reset();
        second.reset();
        eventCount = 0;
        paneHits.fill(0);
        for (int slot = 0; slot < 2; ++slot) {
            auto& human = localPlayer(slot);
            human.slot = slot;
            human.view.localDuel = true;
            human.view.alternateRival = slot == 0;
            human.view.match = Match::Playing;
            localRandom[slot] = {};
            const uint32_t stream = seed ^ (slot == 0 ? 0x6d2b79f5u : 0x82f63b78u);
            localRandom[slot].state = stream ? stream : 1u;
            refill(human, true);
            const auto& spawn = assets.spawns[slot];
            resetPlayer(human, spawn.position, spawn.yaw);
            human.blockFire = true;
        }
        publishDuel();
    }

    const SpawnPoint& localSpawn(const Snapshot& opponent) const {
        const auto* best = &assets.spawns.front();
        float bestScore = -infinity;
        for (const auto& point : assets.spawns) {
            const auto spawn = point.position;
            const float distance = planar(spawn - opponent.player.position);
            const bool hidden = world.blocked(atHeight(spawn, standEye), atHeight(opponent.player.position, opponent.eyeHeight));
            const float value = distance + (hidden ? 24.f : 0.f) - (distance < 2.f ? 100.f : 0.f);
            if (value > bestScore) {
                bestScore = value;
                best = &point;
            }
        }
        return *best;
    }

    void respawnDuelPlayers() {
        std::array<bool, 2> due{};
        for (int slot = 0; slot < 2; ++slot) {
            auto& human = localPlayer(slot);
            if (!human.view.dead) continue;
            human.respawn -= tickSeconds;
            due[slot] = human.respawn <= 0;
        }
        for (int slot = 0; slot < 2; ++slot) {
            if (!due[slot]) continue;
            auto& human = localPlayer(slot);
            // A trade sends both players to opposing corners before either can move.
            const auto& spawn = due[1 - slot] ? assets.spawns[slot] : localSpawn(localPlayer(1 - slot).view);
            const Vec3 pos = spawn.position;
            resetPlayer(human, pos, respawnYaw(spawn));
            refill(human, false);
            emit(EventKind::PlayerRespawn, 0, pos, 0, human.slot);
        }
    }

    void moveDuelPlayer(Human& human, const Input& input) {
        auto& s = human.view;
        s.damage *= float(std::exp(-1.6 * tickSeconds));
        if (s.dead) return;
        if (s.player.health < 100 && s.scaledTime - human.hurtAt > 4) {
            s.player.health = std::min(100.f, s.player.health + float(40 * tickSeconds));
        }
        movePlayer(human, input, tickSeconds);
        const float speed = s.player.speed;
        const float cycle = speed >= 3.1f ? .62f : .83f;
        s.player.phase = std::fmod(s.player.phase + speed * float(tickSeconds) / (4.f * cycle), 1.f);
        updateActorFrame(s.player);
    }

    void resolveDuelShots(const std::array<Shot, 2>& shots) {
        const uint64_t glassBeforeShots = snapshot.brokenGlass;
        for (int slot = 0; slot < 2; ++slot) {
            const auto& shot = shots[slot];
            if (shot.damage <= 0) continue;
            const auto hit = world.shot(shot.origin, shot.direction, glassBeforeShots);
            auto& shooter = localPlayer(slot).view;
            auto& victim = localPlayer(1 - slot);
            float zone = 1.f;
            // The other shot only damages this shooter, so this target is still in its pre-impact pose.
            const float body = victim.view.dead ? infinity :
                actorSurface(victim.view.player, shot.origin, shot.direction, hit.distance, zone,
                             victim.view.eyeHeight / standEye);
            const Vec3 point = shot.origin + shot.direction * (body < hit.distance ? body : hit.hit ? hit.distance : 100.f);
            shooter.tracerStart = shot.origin;
            shooter.tracerEnd = point;
            shooter.tracer = 1.f;
            if (body < hit.distance) {
                if (!hurtPlayer(victim, localRandom[1 - slot], shot.damage * zone, shot.origin, slot)) continue;
                shooter.lethalMarker = victim.view.dead;
                shooter.hitMarker = victim.view.dead ? .33f : .15f;
                if (!victim.view.dead) continue;
                ++shooter.playerScore;
                shooter.headshot = zone >= 2.f;
                shooter.killConfirm = 1.4f;
                beginDeath(victim, localRandom[1 - slot], false);
                emit(EventKind::PlayerDeath, int(Cause::Weapon), victim.view.player.position, 0, 1 - slot, slot);
                sound(shooter.headshot ? Sound::HeadshotConfirm : Sound::KillConfirm, .9f);
            } else if (hit.hit) {
                hitWorld(hit, point);
            }
        }
    }

    void stepDuel(std::array<Input, 2> inputs) {
        eventCount = 0;
        if (!guest || snapshot.match != Match::Playing) return;
        for (const auto& input : inputs) {
            if (!input.disconnected && !input.edge(Pause)) continue;
            snapshot.match = guest->view.match = Match::Paused;
            return;
        }
        std::array<Shot, 2> shots{};
        for (int slot = 0; slot < 2; ++slot) {
            auto& human = localPlayer(slot);
            auto& s = human.view;
            localRandom[slot].tick = ++s.tick;
            s.realTime += tickSeconds;
            s.scaledTime = s.realTime;
            s.scaledDelta = tickSeconds;
            s.muzzle = std::max(0.f, s.muzzle - float(tickSeconds * 17));
            s.tracer = std::max(0.f, s.tracer - float(tickSeconds * 18));
            s.hitMarker = std::max(0.f, s.hitMarker - float(tickSeconds));
            s.killConfirm = std::max(0.f, s.killConfirm - float(tickSeconds));
        }
        respawnDuelPlayers();
        for (int slot = 0; slot < 2; ++slot) {
            auto& human = localPlayer(slot);
            releaseFire(human, inputs[slot]);
            moveDuelPlayer(human, inputs[slot]);
            weapons(human, localRandom[slot], inputs[slot], tickSeconds, false, &shots[slot]);
        }
        resolveDuelShots(shots);
        for (int slot = 0; slot < 2; ++slot) {
            auto& human = localPlayer(slot);
            if (human.view.dead) deathCamera(human, tickSeconds, false);
        }
        const auto p1 = snapshot.playerScore, p2 = guest->view.playerScore;
        if (p1 >= matchTarget || p2 >= matchTarget) {
            snapshot.match = guest->view.match = Match::Results;
            emit(EventKind::MatchEnd, (p1 > p2) - (p2 > p1));
        }
        publishDuel();
    }

    void resumeDuel() {
        if (!guest || snapshot.match != Match::Paused) return;
        for (int slot = 0; slot < 2; ++slot) {
            auto& human = localPlayer(slot);
            resetInput(human);
            human.blockFire = true;
            human.view.match = Match::Playing;
        }
    }
#endif

    void moveCampaignPlayer(Human& human, Input& input, double dt, bool deviceOut) {
        auto& s = human.view;
        s.damage *= float(std::exp(-1.6 * dt));
        if (s.dead) return;
        if (s.player.health < 100 && s.scaledTime - human.hurtAt > 4.5) {
            s.player.health = std::min(100.f, s.player.health + float(28 * dt));
        }
        const bool precisionAim = s.mission->chapter == MissionChapter::BriarHouse || timberlineRetreat(*s.mission);
        if (!deviceOut && !input.down(Signal) && (input.stickX != 0.f || input.stickY != 0.f)) {
            const float gain = mission->stickGain(s, !precisionAim);
            input.lookX += input.stickX * (gain - 1.f);
            input.lookY += input.stickY * (gain - 1.f);
        }
        movePlayer(human, input, dt);
    }

    void campaignDeath(Human& human, Random& rng, double dt, bool slowMotion) {
        if (!human.view.dead) return;
        if (human.death.elapsed == 0) {
            beginDeath(human, rng, slowMotion);
            if (!slowMotion) human.respawn = campaignRejoinSeconds;
        }
        deathCamera(human, dt, slowMotion);
    }

    void publishMission() {
#ifdef FOD_LOCAL_DUEL
        // There is one mission snapshot shared by both cameras.
        if (snapshot.campaignSeat == 0) guest->view.mission.reset();
#endif
        mission->publish(snapshot.mission);
#ifdef FOD_LOCAL_DUEL
        if (snapshot.campaignSeat == 0) publishPartner();
#endif
    }

    void step(Input input, double wallDelta, [[maybe_unused]] Input partnerInput = {}) {
        eventCount = 0;
        campaignVoice.reset();
        if (input.disconnected) pause();
        auto& s = snapshot;
        if (s.match != Match::Playing || wallDelta <= 0 || !std::isfinite(wallDelta)) return;
        ++s.tick;
        random.tick = s.tick;
        const double dt = wallDelta * s.timeScale;
        s.realTime += wallDelta;
        s.scaledTime += dt;
        s.scaledDelta = dt;
        s.muzzle = std::max(0.f, s.muzzle - float(dt * 17));
        s.tracer = std::max(0.f, s.tracer - float(dt * 18));
        if (second) {
            for (auto& shot : s.rivalTracers) shot.time = std::max(0.f, shot.time - float(dt * 18));
            s.killNotice.remaining = std::max(0.f, s.killNotice.remaining - float(wallDelta));
        }
        s.hitMarker = std::max(0.f, s.hitMarker - float(wallDelta));
        s.killConfirm = std::max(0.f, s.killConfirm - float(wallDelta));
        releaseFire(player, input);
        if (mission && mission->active()) {
            HostAdapter host(*this);
            mission_detail::MissionPartner partner{};
            const bool inIntro = s.mission && s.mission->phase == MissionPhase::Intro;
            const bool flying = s.mission && s.mission->operation.stage == OperationStage::ScoutFlight;
            const bool cinematic = s.mission && (s.mission->cinematic || flying);
#ifdef FOD_LOCAL_DUEL
            if (s.campaignSeat == 0) {
                advancePartnerClock(dt, wallDelta);
                partner.view = &guest->view;
                if (inIntro || cinematic) releaseFire(*guest, partnerInput);
                if (flying) {
                    scoutLookYaw = std::clamp(scoutLookYaw + partnerInput.lookX, -.85f, .85f);
                    scoutLookPitch = std::clamp(scoutLookPitch + partnerInput.lookY, -.5f, .5f);
                }
                partner.stabilizing = flying && partnerInput.down(Signal);
                guest->view.stabilizingScout = partner.stabilizing;
            }
#endif
            if (cinematic) {
                // Cinematics and flying give the operation control of the camera and movement.
                devices(Input{}, dt);
                mission->step(s, input, dt, false, host, partner);
                const float cutFov = s.camera.fov;
                viewWeapon(player, true);
                s.camera.fov = cutFov;
                viewWorld();
                s.rival.visible = false;
                s.lower = 1.f;
                publishMission();
                return;
            }
            if (inIntro) {
                s.damage *= float(std::exp(-1.6 * dt));
                // Allow looking around as vision clears while waking up from concussion.
                if (s.mission->introProgress > 0.18f) {
                    s.playerYaw += float(input.lookX * 0.65);
                    s.playerPitch = std::clamp(s.playerPitch + float(input.lookY * 0.65), float(-75 * deg), float(75 * deg));
                }
                mission->step(s, input, dt, false, host, partner);
                const float introFov = s.camera.fov;
                viewWeapon(player, true);
                s.camera.fov = introFov;
                viewWorld();
                s.rival.visible = false;
                publishMission();
                if (s.mission) {
                    s.lower = std::clamp(1.f - (s.mission->introProgress - 0.35f) * 1.65f, 0.f, 1.f);
#ifdef FOD_LOCAL_DUEL
                    if (partner.view && partnerCameraShared) guest->view.lower = s.lower;
#endif
                }
                return;
            }
#ifdef FOD_LOCAL_DUEL
            if (partner.view) {
                rejoinPartner(dt);
                if (s.mission->phase != MissionPhase::Failed) partner.fired = stepCampaignPartner(partnerInput, dt);
                else input = {};
            }
#endif
            moveCampaignPlayer(player, input, dt, handheld());
            devices(input, dt);
            // Signalling takes the trigger hand: hold Signal and the gun dips and will not fire.
            const bool signalling = input.down(Signal) && !s.dead;
            Input combat = input;
            if (signalling) {
                combat.held &= ~uint32_t(Fire);
                combat.pressed &= ~uint32_t(Fire);
            }
            weapons(player, random, combat, dt, handheld());
            mission->step(s, input, dt, player.playerFired, host, partner);
            campaignDeath(player, random, dt, !partner.view);
            if (!s.dead) player.death = {};
#ifdef FOD_LOCAL_DUEL
            if (partner.view) {
                campaignDeath(*guest, localRandom[1], dt, false);
                for (int slot = 0; slot < 2; ++slot) {
                    auto& actor = localPlayer(slot).view.player;
                    const float cycle = actor.speed >= 3.1f ? .62f : .83f;
                    actor.phase = std::fmod(actor.phase + actor.speed * float(dt) / (4.f * cycle), 1.f);
                    updateActorFrame(actor);
                }
            }
#endif
            viewWeapon(player, handheld());
            const float dip = signalling && s.ads < .5f ? .55f : 0.f;
            signalLower += (dip - signalLower) * float(1.0 - std::exp(-10.0 * dt));
            s.lower = std::max(s.lower, signalLower);
            viewWorld();
            s.rival.visible = false;
            publishMission();
            return;
        }
        playerStep(input, dt);
        devices(input, dt);
        weapons(player, random, input, dt, handheld());
        // Credit player shots and streaks before any rival gets to fire this tick.
        for (int slot = 0; slot < rivalCount() && (!second || !matchPointReached()); ++slot) {
            if (rival(slot).bot.dead && !rival(slot).counted) registerKill(slot);
        }
        const Vec3 separation = rivalSeparation(dt);
        for (int slot = 0; slot < rivalCount() && (!second || !matchPointReached()); ++slot) {
            botStep(slot, dt, slot == 0 ? separation * -1.f : separation);
        }
        if (s.tick == fixtureDeathTick && (!second || !matchPointReached())) {
            damagePlayer(1000, first.bot.actor.position, Cause::Weapon);
        }
        if (s.dead) {
            if (player.death.elapsed == 0) beginDeath(player, random, true);
            deathCamera(player, dt, true);
        } else {
            player.death = {};
        }
        if (matchPointReached()) finish();
        viewWeapon(player, handheld());
        viewWorld();
    }
};

Game::Game(const Assets& assets, uint32_t voiceSeed) : impl_(new Impl(assets, voiceSeed)) {}
Game::~Game() = default;
const Snapshot& Game::state() const { return impl_->snapshot; }
const Event* Game::events() const { return impl_->eventBuffer.data(); }
size_t Game::eventCount() const { return impl_->eventCount; }
const CampaignVoiceCue* Game::campaignVoiceCue() const {
    return impl_->campaignVoice ? &*impl_->campaignVoice : nullptr;
}
void Game::start(uint32_t seed, int difficulty, bool alternateRival) {
    start(seed, difficulty, alternateRival ? Opponent::Hanz : Opponent::IPwndUrMom);
}
void Game::start(uint32_t seed, int difficulty, Opponent opponent) { impl_->start(seed, difficulty, opponent); }
void Game::startMission(uint32_t seed, int difficulty, bool fromCheckpoint) {
    using mission_detail::MissionStart;
    impl_->startMission(seed, difficulty, fromCheckpoint ? MissionStart::Retry : MissionStart::SingleMission);
}
void Game::startCampaign(uint32_t seed, int difficulty) {
    impl_->startMission(seed, difficulty, mission_detail::MissionStart::Campaign);
}
bool Game::advanceCampaign(uint32_t seed, int difficulty) {
    const auto& snap = state();
    if (snap.match != Match::Results || !snap.mission || !snap.mission->nextMissionReady()) return false;
    impl_->startMission(seed, difficulty, mission_detail::MissionStart::NextChapter);
    return true;
}
void Game::startExtractionReplay(uint32_t seed, int difficulty) {
    impl_->startMission(seed, difficulty, mission_detail::MissionStart::ExtractionReplay);
}
void Game::startChapterReplay(uint32_t seed, int difficulty, MissionChapter chapter, bool noraAlive) {
    impl_->startMission(seed, difficulty, mission_detail::MissionStart::NextChapter, chapter, false, noraAlive);
}
void Game::startArrayGroundReplay(uint32_t seed, int difficulty) {
    impl_->startMission(seed, difficulty, mission_detail::MissionStart::ArrayGroundReplay);
}
void Game::startScoutReplay(uint32_t seed, int difficulty) {
    impl_->startMission(seed, difficulty, mission_detail::MissionStart::ScoutReplay);
}
void Game::startFinaleReplay(uint32_t seed, int difficulty) {
    impl_->startMission(seed, difficulty, mission_detail::MissionStart::Retry, MissionChapter::Array, true);
}
bool Game::isMission() const { return impl_->mission && impl_->mission->active(); }
bool Game::canRetryCheckpoint() const { return impl_->mission && impl_->mission->canRetryCheckpoint(); }
void Game::pause() { impl_->pause(); }
void Game::resume(bool triggerHeld) {
    if (impl_->snapshot.match != Match::Paused) return;
    impl_->player.blockFire = triggerHeld;
    impl_->snapshot.match = Match::Playing;
}
void Game::quit() {
    impl_->campaignVoice.reset();
    if (impl_->snapshot.match == Match::Paused || impl_->snapshot.match == Match::Playing) {
        impl_->eventCount = 0;
        impl_->finish();
        impl_->viewWorld();
#ifdef FOD_LOCAL_DUEL
        if (impl_->snapshot.campaignSeat == 0) impl_->publishPartner();
#endif
    } else {
        impl_->snapshot.match = Match::Start;
        impl_->snapshot.mission.reset();
        impl_->mission.reset();
#ifdef FOD_LOCAL_DUEL
        if (impl_->snapshot.campaignSeat == 0) {
            impl_->snapshot.campaignSeat = -1;
            impl_->snapshot.rival.visible = false;
            impl_->guest.reset();
        }
#endif
    }
}
void Game::setDifficulty(int level) { impl_->setDifficulty(level); }
void Game::setRandomHook(RandomHook hook, void* data) {
    impl_->random.hook = hook;
    impl_->random.data = data;
}
void Game::setupFixture(const Fixture& fixture) {
    auto& g = *impl_;
    g.resetPlayer(g.player, fixture.player, fixture.yaw);
    g.resetRival(0, fixture.rival, fixture.rivalYaw, true);
    g.first.bot.freeze = fixture.freezeRival;
    if (g.second) {
        g.resetRival(1, fixture.secondRival, fixture.secondRivalYaw, true);
        g.second->bot.freeze = fixture.freezeSecondRival;
        g.snapshot.secondRivalScore = fixture.secondRivalScore;
    }
    g.fixtureDeathTick = fixture.killPlayerAtTick;
    if (g.fixtureDeathTick) g.player.invulnerableUntil = 0;
    auto& s = g.snapshot;
    s.weapon = fixture.weapon;
    s.playerPitch = s.camera.pitch = fixture.pitch;
    s.playerScore = fixture.playerScore;
    s.rivalScore = fixture.rivalScore;
    s.streak = fixture.streak;
    s.airstrikeReady = fixture.airstrikeReady;
    s.apacheReady = fixture.apacheReady;
    if (fixture.clip >= 0) g.player.gun().clip = std::clamp(fixture.clip, 0, game_data::weapons[g.player.weapon()].mag);
    g.viewWeapon(g.player, g.handheld());
    g.viewWorld();
}
void Game::step(const Input& input, double realDelta) { impl_->step(input, realDelta); }

#ifdef FOD_LOCAL_DUEL
void Game::startLocalDuel(uint32_t seed) { impl_->startDuel(seed); }
void Game::stepLocalDuel(std::array<Input, 2> inputs) { impl_->stepDuel(inputs); }
void Game::resumeLocalDuel() { impl_->resumeDuel(); }
void Game::startLocalCampaign(uint32_t seed, int difficulty, MissionChapter chapter) {
    using mission_detail::MissionStart;
    const auto mode = chapter == MissionChapter::BriarHouse ? MissionStart::Campaign : MissionStart::NextChapter;
    impl_->startMission(seed, difficulty, mode, chapter, false, true, true);
}
void Game::continueLocalCampaign(uint32_t seed, int difficulty) {
    using mission_detail::MissionStart;
    const auto& s = state();
    if (s.campaignSeat != 0 || s.match != Match::Results) return;
    auto mode = MissionStart::Campaign;
    if (s.mission->nextMissionReady()) mode = MissionStart::NextChapter;
    else if (s.mission->phase == MissionPhase::Failed && canRetryCheckpoint()) mode = MissionStart::Retry;
    impl_->startMission(seed, difficulty, mode, MissionChapter::BriarHouse, false, true, true);
}
void Game::stepLocalCampaign(std::array<Input, 2> inputs) {
    if (state().campaignSeat != 0 || state().match != Match::Playing) return;
    for (const auto& input : inputs) {
        if (!input.disconnected && !input.edge(Pause)) continue;
        impl_->pause();
        return;
    }
    for (size_t seat = 0; seat < inputs.size(); ++seat) {
        auto& input = inputs[seat];
        auto& blocked = impl_->campaignBlockedActions[seat];
        const uint32_t previous = blocked;
        blocked &= input.held;
        input.held &= ~previous;
        input.pressed &= ~previous;
    }
    impl_->step(inputs[0], tickSeconds, inputs[1]);
}
void Game::resumeLocalCampaign(const std::array<Input, 2>& inputs) {
    if (state().campaignSeat != 0 || state().match != Match::Paused) return;
    resume();
    impl_->guest->view.match = Match::Playing;
    impl_->guest->blockFire = true;
    constexpr uint32_t actions = Fire | Reload | Stance | Swap | Streak | Jump | Signal;
    for (size_t seat = 0; seat < inputs.size(); ++seat)
        impl_->campaignBlockedActions[seat] = inputs[seat].held & actions;
}
const Snapshot& Game::localView(int slot) const { return impl_->localPlayer(slot).view; }
void Game::setupLocalDuelFixture(const std::array<Fixture, 2>& fixtures) {
    auto& g = *impl_;
    for (int slot = 0; slot < 2; ++slot) {
        auto& human = g.localPlayer(slot);
        const auto& f = fixtures[slot];
        human.view.weapon = f.weapon;
        g.resetPlayer(human, f.player, f.yaw);
        human.view.playerPitch = human.view.camera.pitch = f.pitch;
        human.view.playerScore = f.playerScore;
        human.invulnerableUntil = 0;
        if (f.clip >= 0) human.gun().clip = std::clamp(f.clip, 0, game_data::weapons[human.weapon()].mag);
    }
    g.publishDuel();
}
#endif

} // namespace fod

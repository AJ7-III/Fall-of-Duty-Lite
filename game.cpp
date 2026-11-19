#include "game.hpp"

#include "game_bot.hpp"

namespace fod {
namespace {

using namespace game_detail;

constexpr Vec3 playerSpawns[]{
    {-13.5f, 0, -13.5f}, {13.5f, 0, 13.5f}, {-13.5f, 0, 13.5f}, {13.5f, 0, -13.5f},
};
constexpr Sound weaponShots[]{Sound::Mp44, Sound::Sniper, Sound::Pistol};
constexpr double fallAt[]{1.32, 1.62, 1.30};
constexpr double fallStart[]{.50, .98, .62};
constexpr double gaspAt[]{.30, .32, .26};
constexpr float standEye = 1.7f, crouchEye = .95f, proneEye = .42f;
constexpr float footstepGain = .35f;

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
    bool target = false;
};
struct Death {
    double elapsed = 0;
    float yaw = 0, distance = 0, height = 0, liveDistance = 0;
    Camera origin{};
    uint8_t variant = 0;
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
    explicit Impl(const Assets& a) : assets(a), world(a), bot(a, world, random) {
        snapshot.rival.position = playerSpawns[1];
        snapshot.player.position = playerSpawns[0];
        refill(true);
        viewWeapon();
    }

    const Assets& assets;
    World world;
    Random random, voiceRandom;
    Bot bot;
    Snapshot snapshot{};
    std::array<Event, 96> eventBuffer{};
    size_t eventCount = 0;

    std::array<Gun, 3> guns{};
    std::array<uint8_t, 64> paneHits{};
    double switchTimer = 0;
    Phase swap = Phase::Away, laptop = Phase::Away, transmitter = Phase::Away;
    double laptopTime = 0, transmitterTime = 0, uavUntil = -1;
    bool signalSent = false;
    Airstrike strike{};
    Apache apache{};
    bool lookLocked = false, stanceWasDown = false, stanceHeld = false, crouchToggle = false;
    bool playerFired = false, blockFire = false, rivalCounted = false;
    uint32_t fixtureDeathTick = 0;
    unsigned voiceGates[2]{};
    double stanceTime = 0, invulnerableUntil = 0, hurtAt = -forever, respawn = 0, rivalRespawn = 0;
    Vec3 bob{}, sway{};
    double swayTime = 0, bobTime = 0, scopeTime = 0, footstep = 0, rivalFootstep = 0;
    double recoilPitch = 0, recoilYaw = 0, recoilKick = 0;
    Death death{}, rivalDeath{};
    Cause killCause = Cause::Weapon;
    float killZone = 1;

    const game_data::Difficulty& difficulty() const {
        return game_data::levels[snapshot.alternateRival ? 1 : 0][snapshot.difficulty - 1];
    }

    int weapon() const { return int(snapshot.weapon); }
    Gun& gun() { return guns[weapon()]; }

    void emit(EventKind kind, int value = 0, Vec3 position = {}, uint32_t surface = 0) {
        if (eventCount >= eventBuffer.size()) return;
        eventBuffer[eventCount++] = {kind, snapshot.tick, surface, value, position};
    }

    void sound(Sound id, float volume = 1, bool positional = false, Vec3 position = {}) {
        if (eventCount >= eventBuffer.size()) return;
        eventBuffer[eventCount++] = {EventKind::Sound, snapshot.tick, 0, 0, position, id, volume, positional};
    }

    void taunt(bool rivalWon) {
        const int channel = rivalWon ? 1 : 0;
        // Browser voice gates play once in each pair of events per channel.
        // Use a cosmetic random stream so playback never changes gameplay.
        if (!voiceGates[channel]) {
            voiceGates[channel] = voiceRandom.draw("voice") < .5 ? 0b101 : 0b110;
        }
        const bool play = voiceGates[channel] & 1;
        voiceGates[channel] >>= 1;
        if (voiceGates[channel] == 1) voiceGates[channel] = 0;
        if (!play) return;
        const bool alternate = snapshot.alternateRival;
        const Sound line = rivalWon
            ? (alternate ? Sound::VoiceAlternateVictory : Sound::VoiceVictory)
            : (alternate ? Sound::VoiceAlternateKill : Sound::VoiceKill);
        sound(line, .602f);
    }

    void refill(bool newMatch) {
        for (int i = 0; i < 3; ++i) {
            auto& state = guns[i];
            state = {};
            state.clip = game_data::weapons[i].mag;
            state.reserve = game_data::weapons[i].reserve;
        }
        if (newMatch) snapshot.weapon = Weapon::Mp44;
        swap = Phase::Away;
        switchTimer = 0;
        playerFired = false;
    }

    void cancelWeapon() {
        auto& state = gun();
        if (state.mode == WeaponState::Reload) {
            state.mode = WeaponState::Idle;
            state.timer = 0;
            state.cueCount = 0;
            if (state.clip) state.slideLocked = false;
        }
        state.adsFrame = 0;
        state.aiming = false;
    }

    void applyRecoil(double pitch, double yaw, double kick) {
        recoilPitch += pitch * deg;
        recoilYaw += (random.draw("camera.recoil.yaw") - .5) * yaw * deg;
        recoilKick += kick;
    }

    void resetPlayer(Vec3 position, float yaw) {
        auto& s = snapshot;
        s.player = {};
        s.player.position = position;
        s.player.yaw = yaw;
        s.player.visible = false;
        s.playerVelocity = {};
        s.dead = s.sprint = s.crouch = s.prone = false;
        s.grounded = true;
        s.eyeHeight = standEye;
        s.playerYaw = yaw;
        s.playerPitch = s.damage = 0;
        s.camera.position = atHeight(position, standEye);
        s.camera.yaw = yaw;
        s.camera.pitch = 0;
        crouchToggle = lookLocked = false;
        resetInput();
        bob = sway = {};
        swayTime = bobTime = scopeTime = footstep = 0;
        recoilPitch = recoilYaw = recoilKick = 0;
        invulnerableUntil = s.scaledTime + 1;
        hurtAt = -forever;
    }

    void resetInput() {
        stanceWasDown = stanceHeld = false;
        stanceTime = 0;
    }

    BotTarget target() const {
        const auto& s = snapshot;
        return {s.player.position, s.playerVelocity, s.eyeHeight, s.dead, s.sprint, s.prone, s.crouch, playerFired};
    }

    void setDifficulty(int level) { snapshot.difficulty = std::clamp(level, 1, 10); }

    void start(uint32_t seed, int level, bool alternate) {
        snapshot = {};
        snapshot.match = Match::Playing;
        snapshot.alternateRival = alternate;
        setDifficulty(level);
        random.state = seed ? seed : 1;
        voiceRandom.state = random.state ^ 0x516ad3cdu;
        if (!voiceRandom.state) voiceRandom.state = 1;
        voiceGates[0] = voiceGates[1] = 0;
        random.tick = 0;
        eventCount = 0;
        paneHits.fill(0);
        refill(true);
        resetPlayer(playerSpawns[0], pi / 4);
        bot.freeze = false;
        resetRival(playerSpawns[1], -3 * pi / 4, true);
        laptop = transmitter = Phase::Away;
        laptopTime = transmitterTime = 0;
        uavUntil = -1;
        strike = {};
        apache = {};
        death = rivalDeath = {};
        respawn = 0;
        fixtureDeathTick = 0;
        blockFire = false;
        sound(alternate ? Sound::VoiceAlternateStart : Sound::VoiceStart, .602f);
        viewWeapon();
        viewWorld();
    }

    void resetRival(Vec3 pos, float yaw, bool newMatch) {
        bot.reset(pos, yaw, difficulty(), snapshot.scaledTime, newMatch);
        rivalCounted = false;
        rivalFootstep = rivalRespawn = 0;
        killCause = Cause::Weapon;
        killZone = 1;
    }

    Vec3 chooseSpawn(bool forRival) {
        const auto& s = snapshot;
        const Vec3 other = forRival ? s.player.position : bot.actor.position;
        const Vec3 eye = atHeight(other, forRival ? s.eyeHeight : botEye);
        double best = -forever;
        Vec3 chosen{};
        for (int i = forRival ? 1 : 0; i < 4; ++i) {
            const Vec3 spawn = playerSpawns[i];
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
                chosen = spawn;
            }
        }
        return chosen;
    }

    void playerStep(const Input& input, double dt) {
        auto& s = snapshot;
        s.damage *= float(std::exp(-1.6 * dt));
        if (s.dead) {
            respawn -= dt;
            if (respawn > 0) return;
            const Vec3 pos = chooseSpawn(false);
            resetPlayer(pos, std::atan2(-pos.x, -pos.z));
            refill(false);
            s.timeScale = 1;
            emit(EventKind::PlayerRespawn, 0, pos);
        } else if (s.player.health < 100 && s.scaledTime - hurtAt > 4) {
            s.player.health = std::min(100.f, s.player.health + float(40 * dt));
        }

        const auto& state = gun();
        const double progress = state.adsFrame / game_data::aimLast[weapon()];
        if (!lookLocked) {
            const auto aim = interpolateAim(weapon(), state.adsFrame);
            s.playerYaw += float(input.lookX * aim.sensitivity);
            s.playerPitch = std::clamp(s.playerPitch + float(input.lookY * aim.sensitivity), float(-85 * deg), float(85 * deg));
        }
        const bool stanceDown = input.down(Stance);
        const bool stancePress = !stanceWasDown && (stanceDown || input.edge(Stance));
        const bool stanceReleased = !stanceDown && (stanceWasDown || stancePress);
        if (stancePress) {
            stanceTime = 0;
            stanceHeld = false;
        }
        if (stanceDown) {
            stanceTime += dt;
            if (!stanceHeld && stanceTime >= .32) {
                s.prone = true;
                stanceHeld = true;
            }
        } else if (stanceReleased) {
            if (stanceHeld || s.prone) {
                const float desired = stanceHeld && crouchToggle ? crouchEye : standEye;
                if (world.canStand(s.player.position, desired)) {
                    s.prone = false;
                    crouchToggle = desired == crouchEye;
                } else if ((!stanceHeld || !crouchToggle) && world.canStand(s.player.position, crouchEye)) {
                    s.prone = false;
                    crouchToggle = true;
                }
            } else if (!crouchToggle || world.canStand(s.player.position, standEye)) {
                crouchToggle = !crouchToggle;
            }
        }
        stanceWasDown = stanceDown;
        s.crouch = crouchToggle && !s.prone;
        const float desired = s.prone ? proneEye : s.crouch ? crouchEye : standEye;
        s.eyeHeight += (desired - s.eyeHeight) * float(1 - std::exp(-15 * dt));
        s.sprint = input.down(Sprint) && input.moveY > 0 && !s.crouch && !s.prone && progress < .1;
        const float speed = s.prone ? .9f : s.crouch ? 2.f : s.sprint ? 6.2f : 4.f;
        const float sy = std::sin(s.playerYaw), cy = std::cos(s.playerYaw);
        const Vec3 desiredVel{sy * input.moveY + cy * input.moveX, 0, cy * input.moveY - sy * input.moveX};
        const float magnitude = planar(desiredVel);
        const Vec3 applied = desiredVel * (speed / std::max(1.f, magnitude));
        s.playerVelocity.x = applied.x;
        s.playerVelocity.z = applied.z;
        if (input.edge(Jump) && s.grounded && !s.crouch && !s.prone) {
            s.playerVelocity.y = 5 + gravity / (2 * 60);
        }
        s.grounded = world.move(s.player.position, s.playerVelocity, float(dt), s.eyeHeight + .15f, .4f);
        s.player.yaw = s.playerYaw;
        s.player.pitch = s.playerPitch;
        s.player.speed = planar(s.playerVelocity);
        const bool moving = magnitude > .001f;
        if (moving && s.grounded && s.player.speed > .2f && !s.prone) {
            footstep -= dt;
            if (footstep <= 0) {
                sound(Sound::Footstep, footstepGain * (s.sprint ? .55f : s.crouch ? .18f : .34f));
                footstep += s.sprint ? .34 : s.crouch ? .60 : .45;
            }
        } else {
            footstep = 0;
        }
        playerCamera(dt, moving, speed / 4, progress);
    }

    void playerCamera(double dt, bool moving, float speedRatio, double progress) {
        auto& s = snapshot;
        const double decay = std::exp(-10 * dt);
        recoilPitch *= decay;
        recoilYaw *= decay;
        recoilKick *= std::exp(-15 * dt);
        scopeTime += dt * .85;
        const double scopeAmplitude = .0026 * progress * progress * (3 - 2 * progress);
        const double scopedYaw = std::sin(scopeTime) * scopeAmplitude;
        const double scopedPitch = std::sin(scopeTime * 2) * scopeAmplitude * .55;
        const bool sight = progress > .6;
        const double swayScale = sight ? .05 : .2;
        swayTime += dt * (sight ? 1.0 : 1.5);
        sway.x = float(std::sin(swayTime * 1.2) * .015 * swayScale);
        sway.y = float(std::cos(swayTime * .8) * .02 * swayScale);
        if (moving && !sight) {
            bobTime += dt * 12 * speedRatio;
            const double amount = .5 + .5 * speedRatio;
            bob.x = float(std::sin(bobTime * .5) * .025 * amount);
            bob.y = float(std::abs(std::sin(bobTime)) * .035 * amount);
        } else {
            bobTime = 0;
            bob *= float(decay);
        }
        s.camera.position = atHeight(s.player.position, s.eyeHeight) + bob + sway -
            forward(s.playerYaw, s.playerPitch) * float(recoilKick);
        s.camera.yaw = s.playerYaw + float(recoilYaw + scopedYaw);
        s.camera.pitch = s.playerPitch + float(-recoilPitch + scopedPitch);
    }

    void startReload(Gun& state) {
        const int id = weapon();
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
        emit(EventKind::ReloadStart, id);
    }

    void updateCues(Gun& state, double dt) {
        state.timer -= dt;
        const double elapsed = state.duration - state.timer;
        while (state.nextCue < state.cueCount && elapsed >= state.cues[state.nextCue].time) {
            const auto& config = game_data::weapons[weapon()];
            const auto& cue = state.cues[state.nextCue++];
            using K = ReloadCue::Kind;
            if (cue.kind == K::Round || cue.kind == K::MagIn) {
                const int added = std::min(state.reserve, cue.kind == K::Round ? 1 : config.mag - state.clip);
                state.clip += added;
                state.reserve -= added;
                emit(EventKind::ReloadRound, added);
            }
            if (cue.kind == K::Handle && weapon() == int(Weapon::Pistol)) state.slideLocked = false;
            sound(cue.kind == K::Handle ? Sound::Bolt : Sound::Reload);
        }
        if (state.timer <= 0) {
            state.mode = WeaponState::Idle;
            state.cueCount = 0;
            emit(EventKind::ReloadEnd, weapon());
        }
    }

    void hitRival(float amount, Cause cause, float zone, Vec3 point) {
        if (bot.dead) return;
        bot.actor.health -= amount;
        bot.pendingAlert = true;
        killCause = cause;
        killZone = zone;
        emit(EventKind::RivalHit, int(std::lround(zone * 100)), point);
        if (cause == Cause::Weapon) {
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
            rivalDeath = {};
            rivalDeath.variant = uint8_t(random.draw("rival.death.variant") * 3);
            bot.actor.fallVariant = rivalDeath.variant;
        }
    }

    float rivalSurface(Vec3 origin, Vec3 dir, float before, float& zone) const {
        if (bot.dead) return infinity;
        const auto& a = bot.actor;
        const Vec3 p = intoActor(origin - a.position, a.yaw);
        const Vec3 d = intoActor(dir, a.yaw);
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
            const float body = cylinder(origin, dir, a.position, .31f, 1.92f);
            if (body < best) {
                best = body;
                const float hitHeight = p.y + d.y * body;
                zone = hitHeight > 1.67f ? 2.f : hitHeight > .92f ? 1.5f : 1.f;
            }
        }
        return best < before ? best : infinity;
    }

    void fire(Gun& state) {
        auto& s = snapshot;
        const int id = weapon();
        const auto& config = game_data::weapons[id];
        const bool automatic = id == int(Weapon::Mp44), sniper = id == int(Weapon::Sniper);
        const auto& kick = state.aiming ? config.ads : config.hip;
        --state.clip;
        playerFired = true;
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
            d += n * float(random.between("shot.spread.x", -spread, spread));
            d += up * float(random.between("shot.spread.y", -spread, spread));
            d = normalized(d);
        }
        emit(EventKind::Fire, state.clip, p);
        if (sniper) emit(EventKind::BoltStart, id);
        sound(weaponShots[id]);
        s.muzzle = 1;

        const auto hit = world.shot(p, d, s.brokenGlass);
        float zone = 1;
        const float rival = rivalSurface(p, d, hit.distance, zone);
        const Vec3 point = p + d * (rival < infinity ? rival : hit.hit ? hit.distance : 100.f);
        if (rival < infinity) {
            hitRival(config.damage * zone, Cause::Weapon, zone, point);
        } else if (hit.hit && hit.glass >= 0 && hit.glass < int(paneHits.size())) {
            const int count = ++paneHits[hit.glass];
            const int needed = hit.glass < int(assets.glassHits.size()) ? assets.glassHits[hit.glass] : 1;
            emit(EventKind::GlassHit, count, point, hit.surface);
            sound(Sound::Glass, count < needed ? .55f : 1.f, true, point);
            if (count >= needed) {
                s.brokenGlass |= uint64_t(1) << hit.glass;
                emit(EventKind::GlassBreak, hit.glass, point, hit.surface);
            }
        } else if (hit.hit) {
            emit(EventKind::WorldHit, 0, point, hit.surface);
            sound(Sound::Impact, .6f, true, point);
        }
        applyRecoil(kick.pitch, kick.yaw, kick.kick);
        if (!sniper && state.clip == 0 && state.reserve > 0) startReload(state);
    }

    bool handheld() const { return laptop != Phase::Away || transmitter != Phase::Away; }

    void weapons(const Input& input, double dt) {
        auto& s = snapshot;
        playerFired = false;
        const bool allowed = !s.dead && !handheld();
        if (allowed && swap == Phase::Away && input.edge(Swap)) {
            swap = Phase::Lowering;
            switchTimer = 0;
            cancelWeapon();
            sound(Sound::Reload, .45f);
        }
        if (swap == Phase::Lowering) {
            switchTimer += dt;
            if (switchTimer >= .22) {
                s.weapon = Weapon((weapon() + 1) % 3);
                swap = Phase::Raising;
                switchTimer = 0;
                emit(EventKind::Swap, weapon());
            }
        } else if (swap == Phase::Raising) {
            switchTimer += dt;
            if (switchTimer >= .3) swap = Phase::Away;
        }

        const bool enabled = allowed && swap == Phase::Away;
        const int id = weapon();
        auto& state = gun();
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
                emit(EventKind::BoltEnd, id);
            }
        } else if (state.mode == WeaponState::Reload) {
            updateCues(state, dt);
        }

        const bool trigger = state.mode == WeaponState::Idle && enabled;
        const bool pressed = trigger && input.edge(Fire);
        const bool held = trigger && id == int(Weapon::Mp44) && input.down(Fire);
        if (!ready || !held || pressed) state.burst = false;
        if (!trigger) return;
        if (!state.clip && state.reserve > 0) {
            startReload(state);
            return;
        }
        if (pressed || held) {
            if (state.clip > 0 && state.cooldown <= 0) {
                const double overshoot = state.burst ? state.cooldown : 0;
                fire(state);
                if (held) {
                    state.cooldown = std::max(0., state.cooldown + overshoot);
                    state.burst = true;
                }
            } else if (!state.clip && pressed) {
                emit(EventKind::Dry, weapon());
                sound(Sound::Dry);
                if (id == int(Weapon::Mp44)) state.cooldown = .2;
            }
        }
        if (state.mode == WeaponState::Idle && input.edge(Reload)) startReload(state);
    }

    void damagePlayer(float amount, Vec3 from, Cause cause) {
        auto& s = snapshot;
        if (s.dead || s.scaledTime < invulnerableUntil) return;
        s.player.health = std::max(0.f, s.player.health - amount);
        hurtAt = s.scaledTime;
        s.damage = std::min(1.f, s.damage + amount / 45);
        s.damageFromYaw = std::atan2(from.x - s.player.position.x, from.z - s.player.position.z);
        applyRecoil(.55, 1.4, .012);
        emit(EventKind::PlayerHit, int(std::lround(amount)), from);
        sound(Sound::Hit, .6f);
        if (s.player.health > 0) return;
        s.dead = true;
        s.streak = 0;
        s.player.visible = true;
        s.player.speed = 0;
        ++s.rivalScore;
        respawn = 3.2;
        if (laptop == Phase::Raising || laptop == Phase::Out) closeLaptop();
        if (transmitter != Phase::Away) {
            if (apache.phase == Flight::Off) s.apacheReady = true;
            transmitter = Phase::Away;
        }
        emit(EventKind::PlayerDeath, int(cause), s.player.position);
        if (cause != Cause::Airstrike) taunt(true);
    }

    float seatClear(float yaw, float distance, float height) const {
        const auto p = snapshot.player.position;
        const Vec3 dir{std::sin(yaw) * distance, height - 1.f, std::cos(yaw) * distance};
        const float len = length(dir);
        if (len < .001f) return 0;
        const float reach = len + .45f;
        return std::min(reach, world.ray(atHeight(p, 1), dir / len, reach));
    }

    void beginDeath() {
        auto& s = snapshot;
        constexpr float seats[][3] = {
            {.6f, 2.4f, 1.25f}, {-.6f, 2.4f, 1.25f}, {1.35f, 2.3f, 1.2f}, {-1.35f, 2.3f, 1.2f},
            {.15f, 2.7f, 1.5f}, {2.4f, 2.4f, 1.3f}, {-2.4f, 2.4f, 1.3f}, {pi, 2.6f, 1.45f}
        };
        death = {};
        death.origin = s.camera;
        death.variant = uint8_t(random.draw("player.death.variant") * 3);
        s.player.fallVariant = death.variant;
        float clearance = -1;
        const float* selected = seats[0];
        for (const auto& seat : seats) {
            const float clear = seatClear(s.playerYaw + seat[0], seat[1], seat[2]);
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
        death.yaw = s.playerYaw + selected[0];
        death.distance = std::clamp(clearance - .45f, .9f, selected[1]);
        death.height = std::max(.5f, selected[2] * death.distance / selected[1]);
        death.liveDistance = death.distance;
        s.timeScale = .5;
    }

    void playFall(Death& d, Actor& actor, double dt, bool player) {
        const double t = d.elapsed += dt;
        const int variant = d.variant;
        if (t >= gaspAt[variant] && t - dt < gaspAt[variant]) sound(Sound::Death, 1, !player, actor.position);
        if (t >= fallAt[variant] && t - dt < fallAt[variant]) sound(Sound::Footstep, .8f, !player, actor.position);
        const double k = std::clamp((t - fallStart[variant]) / (fallAt[variant] - fallStart[variant]), 0., 1.);
        actor.fall = float(k * k * (1.6 - .6 * k));
    }

    void deathCamera(double dt) {
        auto& s = snapshot;
        playFall(death, s.player, dt, true);
        const double t = death.elapsed, impact = fallAt[death.variant];
        s.timeScale = .5 + .5 * std::clamp((t - impact) / .5, 0., 1.);
        const float ease = smooth(t / 1.1);
        const double drift = std::max(0., t - 1.1);
        const float yaw = death.yaw + float(drift * .16);
        const float wanted = death.distance - float(std::min(.5, drift * .12));
        const float height = std::max(.6f, death.height - float(std::min(.35, drift * .1)));
        const float clear = seatClear(yaw, wanted, height);
        const float allowed = std::max(.8f, std::min(wanted, clear - .45f));
        death.liveDistance += (allowed - death.liveDistance) * float(1 - std::exp(-8 * dt));
        death.liveDistance = std::min(death.liveDistance, allowed);
        const Vec3 seat = s.player.position + Vec3{
            std::sin(yaw) * death.liveDistance + float(std::sin(t * 1.7) * .02 * ease),
            height + float(std::sin(t * 1.1) * .015 * ease), std::cos(yaw) * death.liveDistance
        };
        s.camera.position = death.origin.position + (seat - death.origin.position) * ease;
        const float blend = std::clamp(float((t - .6) / .9), 0.f, 1.f);
        const float head = 1.62f - 1.35f * s.player.fall;
        const Vec3 look = atHeight(s.player.position, head + (.45f - head) * blend) - s.camera.position;
        const float facing = std::atan2(look.x, look.z);
        const float pitch = -std::atan2(look.y, std::max(.001f, planar(look)));
        const float steering = smooth(t / .45);
        s.camera.pitch = death.origin.pitch + (pitch - death.origin.pitch) * steering;
        s.camera.yaw = death.origin.yaw + float(angleDelta(death.origin.yaw, facing)) * steering;
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
        applyRecoil(std::min(2.6, 7 / (1 + playerDistance * .7)), 4, std::min(.055, .16 / (1 + playerDistance * .5)));
        const Vec3 eye = atHeight(point, 1.1f);
        if (!bot.dead) {
            const float d = planar(bot.actor.position - point);
            if (d <= 5.6f && !world.blocked(eye, atHeight(bot.actor.position, 1.3f))) {
                hitRival(165 * (1 - d / 6.8f), Cause::Airstrike, 1, bot.actor.position);
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

    void apacheRound(Vec3 gunPos) {
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
        if (hit) hitRival(9, Cause::Apache, 1, point);
    }

    Vec3 apacheGun() const {
        constexpr float y = -.52f, z = 1.35f;
        const float c = float(std::cos(apache.pitch)), s = float(std::sin(apache.pitch));
        const float rolledX = -y * float(std::sin(apache.roll));
        const float rolledY = y * float(std::cos(apache.roll));
        const Vec3 localGun{rolledX, c * rolledY - s * z, s * rolledY + c * z};
        return apache.pos + intoActor(localGun, float(-apache.yaw));
    }

    void updateApache(double now, double dt) {
        auto& a = apache;
        if (a.phase == Flight::Off) return;
        if (a.phase == Flight::Hunt && (now >= a.retargetAt || (a.target && bot.dead))) {
            a.target = !bot.dead;
            a.retargetAt = now + .7;
        }
        Vec3 desired{0, 12.5f + float(std::sin(now * .8) * .5), 0};
        if (a.phase == Flight::Leave) {
            desired = {46, 24, -46};
        } else if (a.target && !bot.dead) {
            a.angle += .32 * dt;
            desired.x = bot.actor.position.x + float(std::sin(a.angle) * 8.5);
            desired.z = bot.actor.position.z + float(std::cos(a.angle) * 8.5);
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
        if (a.phase == Flight::Enter && std::abs(a.pos.x) < 18 && std::abs(a.pos.z) < 18) {
            a.phase = Flight::Hunt;
            a.retargetAt = 0;
        }
        double wantedYaw = a.yaw;
        if (a.phase == Flight::Hunt && a.target && !bot.dead) {
            const Vec3 target = bot.actor.position - a.pos;
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
        if (a.phase == Flight::Hunt && a.target && !bot.dead) {
            const Vec3 gun = apacheGun();
            if (a.rounds > 0) {
                if (a.timer <= 0) {
                    apacheRound(gun);
                    a.timer = .07;
                    if (--a.rounds == 0) a.burstAt = now + .9 + random.draw("apache.burst.cooldown") * .6;
                }
            } else if (now >= a.burstAt && !world.blocked(gun, atHeight(bot.actor.position, 1.2f))) {
                a.rounds = 10;
            }
        } else {
            a.rounds = 0;
        }
        if (a.phase == Flight::Hunt && now >= a.until) {
            a.phase = Flight::Leave;
            a.target = false;
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
            s.cursorX = limit(s.cursorX + input.lookX * cursorGain);
            s.cursorZ = limit(s.cursorZ - input.lookY * cursorGain);
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
        lookLocked = laptop == Phase::Raising || laptop == Phase::Out;
        if (!wasHandheld && handheld()) cancelWeapon();

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

    void registerKill() {
        auto& s = snapshot;
        rivalCounted = true;
        rivalRespawn = 5;
        ++s.playerScore;
        ++s.streak;
        s.killConfirm = 1.4f;
        s.headshot = killZone >= 2;
        emit(EventKind::RivalKill, int(killCause) * 10 + (s.headshot ? 1 : 0), bot.actor.position);
        sound(s.headshot ? Sound::HeadshotConfirm : Sound::KillConfirm, 1.05f);
        taunt(false);
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

    void botStep(double dt) {
        const auto now = snapshot.scaledTime;
        if (bot.dead && !rivalCounted) registerKill();
        const BotRound round = bot.update(target(), difficulty(), now, dt);
        if (round.fired) {
            emit(EventKind::BotFire, int(round.sound), round.muzzle);
            sound(round.sound, .88f, true, round.muzzle);
            tracer(round.muzzle, round.end);
            if (round.damage > 0) damagePlayer(round.damage, bot.actor.position, Cause::Weapon);
        }
        if (bot.dead) {
            playFall(rivalDeath, bot.actor, dt, false);
            rivalRespawn -= dt;
            if (rivalRespawn <= 0) {
                const Vec3 pos = chooseSpawn(true);
                resetRival(pos, std::atan2(-pos.x, -pos.z), false);
                const double skill = difficulty().aggression;
                if (!snapshot.dead && random.draw("bot.spawn.remember") < .45 + skill * .55) {
                    bot.spawnIntuition(target(), 2.5 + (1 - skill) * 9, now);
                }
                emit(EventKind::RivalRespawn, 0, pos);
            }
        } else if (bot.actor.speed > .35f) {
            const float speed = bot.actor.speed;
            rivalFootstep -= dt;
            if (rivalFootstep <= 0) {
                sound(Sound::Footstep, footstepGain * .4f, true, bot.actor.position);
                rivalFootstep += speed > 4.8f ? .34 : .45;
            }
        } else {
            rivalFootstep = 0;
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
        s.rival = bot.actor;
        s.apachePosition = apache.pos;
        s.apacheYaw = float(apache.yaw);
        s.apacheActive = apache.phase != Flight::Off;
        s.uavSeconds = float(std::max(0., uavUntil - s.scaledTime));
        s.radarContact = !bot.dead && (bot.radar > 0 || s.uavSeconds > 0);
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

    void viewWeapon() {
        auto& s = snapshot;
        const int id = weapon();
        const auto& state = gun();
        const auto aim = interpolateAim(id, state.adsFrame);
        s.ads = float(state.adsFrame / game_data::aimLast[id]);
        s.camera.fov = float(aim.fov * deg);
        s.scope = s.dead || handheld() ? 0 : float(aim.scope);
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
        s.lower = swap == Phase::Lowering ? float(std::min(1., switchTimer / .22)) :
            swap == Phase::Raising ? float(std::max(0., 1 - switchTimer / .3)) : 0.f;
    }

    void finish() {
        auto& s = snapshot;
        s.match = Match::Results;
        s.timeScale = 1;
        laptop = transmitter = Phase::Away;
        apache.phase = Flight::Off;
        s.scope = 0;
        lookLocked = false;
        const int result = s.playerScore >= 10 ? 1 : s.rivalScore >= 10 ? -1 :
            (s.playerScore > s.rivalScore) - (s.playerScore < s.rivalScore);
        emit(EventKind::MatchEnd, result);
    }

    void pause() {
        if (snapshot.match != Match::Playing) return;
        snapshot.match = Match::Paused;
        resetInput();
        eventCount = 0;
        blockFire = true;
    }

    void step(Input input, double wallDelta) {
        eventCount = 0;
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
        s.hitMarker = std::max(0.f, s.hitMarker - float(wallDelta));
        s.killConfirm = std::max(0.f, s.killConfirm - float(wallDelta));
        if (blockFire) {
            if (!input.down(Fire) && !input.edge(Fire)) blockFire = false;
            input.held &= ~uint32_t(Fire);
            input.pressed &= ~uint32_t(Fire);
        }
        playerStep(input, dt);
        devices(input, dt);
        weapons(input, dt);
        botStep(dt);
        if (s.tick == fixtureDeathTick) damagePlayer(1000, bot.actor.position, Cause::Weapon);
        if (s.dead) {
            if (death.elapsed == 0) beginDeath();
            deathCamera(dt);
        } else {
            death = {};
        }
        if (s.playerScore >= 10 || s.rivalScore >= 10) finish();
        viewWeapon();
        viewWorld();
    }
};

Game::Game(const Assets& assets) : impl_(new Impl(assets)) {}
Game::~Game() = default;
const Snapshot& Game::state() const { return impl_->snapshot; }
const Event* Game::events() const { return impl_->eventBuffer.data(); }
size_t Game::eventCount() const { return impl_->eventCount; }
void Game::start(uint32_t seed, int difficulty, bool alternateRival) { impl_->start(seed, difficulty, alternateRival); }
void Game::pause() { impl_->pause(); }
void Game::resume(bool triggerHeld) {
    if (impl_->snapshot.match != Match::Paused) return;
    impl_->blockFire = triggerHeld;
    impl_->snapshot.match = Match::Playing;
}
void Game::quit() {
    if (impl_->snapshot.match == Match::Paused || impl_->snapshot.match == Match::Playing) {
        impl_->eventCount = 0;
        impl_->finish();
        impl_->viewWorld();
    } else {
        impl_->snapshot.match = Match::Start;
    }
}
void Game::setDifficulty(int level) { impl_->setDifficulty(level); }
void Game::setRandomHook(RandomHook hook, void* data) {
    impl_->random.hook = hook;
    impl_->random.data = data;
}
void Game::setupFixture(const Fixture& fixture) {
    auto& g = *impl_;
    g.resetPlayer(fixture.player, fixture.yaw);
    g.resetRival(fixture.rival, fixture.rivalYaw, true);
    g.bot.freeze = fixture.freezeRival;
    g.fixtureDeathTick = fixture.killPlayerAtTick;
    if (g.fixtureDeathTick) g.invulnerableUntil = 0;
    auto& s = g.snapshot;
    s.weapon = fixture.weapon;
    s.playerPitch = s.camera.pitch = fixture.pitch;
    s.playerScore = fixture.playerScore;
    s.rivalScore = fixture.rivalScore;
    s.streak = fixture.streak;
    s.airstrikeReady = fixture.airstrikeReady;
    s.apacheReady = fixture.apacheReady;
    if (fixture.clip >= 0) g.gun().clip = std::clamp(fixture.clip, 0, game_data::weapons[g.weapon()].mag);
    g.viewWeapon();
    g.viewWorld();
}
void Game::step(const Input& input, double realDelta) { impl_->step(input, realDelta); }

} // namespace fod

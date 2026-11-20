#include "mission_internal.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace fod::mission_detail {
namespace {

using namespace game_detail;

constexpr uint16_t rgb565(unsigned r, unsigned g, unsigned b) {
    return uint16_t((r >> 3) << 11 | (g >> 2) << 5 | (b >> 3));
}

constexpr uint16_t amberTracer = rgb565(255, 195, 107);
constexpr uint16_t cyanPlasma = rgb565(72, 248, 255);
constexpr uint16_t scanCyan = rgb565(42, 168, 198);
constexpr uint16_t crimsonPlasma = rgb565(255, 86, 112);

constexpr float standEye = 1.7f, proneEye = 0.42f;
constexpr double introDuration = 6.8;
constexpr double regroupDuration = 3.6;
constexpr double incomingLeadIn = 2.6;   // Warhorn to first drop
constexpr double incomingTimeout = 30.0; // The counter-attack lands even if the player never reaches the patio
constexpr double overwatchNudge = 22.0, overwatchSpotted = 45.0;
constexpr float lastStandHealth = 20.f;

// The ground alien stands about two metres tall, hunched with its head thrust forward.
// Enforcers are the same body scaled up by eight percent.
constexpr float alienHeight = 2.05f, alienHeadLine = 1.74f, alienMouth = 1.8f, alienChest = 1.45f;
constexpr float alienRadius = .4f;

// The front garden wall runs along map y = 515. Overwatch patrols sweep the road beyond it,
// then vault it when the shooting starts.
constexpr float frontWallZ = (212.5f - 515.f) * .05f;
constexpr double leapDuration = .85;

// The road lies outside the arena's movement clamp but is open ground, so aliens out
// there move freely until they vault in.
bool onRoad(Vec3 p) { return p.z < frontWallZ - .1f; }

// Rescued survivors shelter just inside the rear doors, behind the patio defence.
constexpr float evacSpots[3][2]{{334, 58}, {368, 70}, {402, 58}};
constexpr float patio[2]{368, 10};
constexpr float commanderPatio[2]{306, -6}; // West edge of the patio, off the main firing lanes
// The Marsh family, one to a room. Dr. Nora Marsh's radio array tracked the fleet before it
// landed; Delta Squad was sent to bring her and her family out of Briar House.
struct Survivor {
    const char* name;      // Subtitle speaker
    const char* callout;   // Delta-1 as you reach the room
    const char* underFire; // Delta-1 when the aliens turn on them
    const char* rescued;   // Their own words once the room is clear
    const char* worried;   // Said instead while Nora is still pinned
    const char* lost;
};
constexpr Survivor family[3]{
    {"BEN MARSH", "KITCHEN AHEAD - THAT'S BEN MARSH BEHIND THE ISLAND. WATCH YOUR FIRE.",
     "THEY'RE FIRING ON BEN - GET IN THERE!", "THEY CAME THROUGH THE WINDOWS... THANK YOU. THANK YOU.",
     "THEY CAME THROUGH THE WINDOWS... IS MY MOM OKAY? SHE WAS IN THE GARAGE!", "WE LOST BEN! LOCK IT DOWN, DELTA!"},
    {"DR. NORA MARSH", "GARAGE BAY - DR. MARSH IS BEHIND THE WAGON. DRONE OVERHEAD.",
     "THEY'RE FIRING ON DR. MARSH - MOVE!", "MY ARRAY ON THE ROOF - THEY'RE HOMING ON IT. GET MY FAMILY OUT!",
     "MY ARRAY ON THE ROOF - THEY'RE HOMING ON IT. GET MY FAMILY OUT!", "DR. MARSH IS DOWN! WE LOST THE PACKAGE!"},
    {"WALT MARSH", "SOMEONE'S IN THE GARDEN SHED - THE GRANDFATHER. LINE UP YOUR SHOTS.",
     "THEY'RE HITTING THE SHED - GET EYES ON WALT!", "TOOK YOU LONG ENOUGH, SON.",
     "TOOK YOU LONG ENOUGH, SON. WHERE'S MY DAUGHTER?", "WE LOST THE OLD MAN! LOCK IT DOWN!"},
};
constexpr int nora = 1;
constexpr double titleCardDuration = 4.6;

float wrapDelta(float from, float to) {
    float d = std::fmod(to - from, 2 * pi);
    if (d > pi) d -= 2 * pi;
    if (d < -pi) d += 2 * pi;
    return d;
}

void copyText(char* dst, size_t cap, const char* src) {
    if (!cap) return;
    std::snprintf(dst, cap, "%s", src ? src : "");
}

float alienScale(uint8_t variant) { return variant > 0 ? 1.08f : 1.f; }

float rayHitBox(Vec3 origin, Vec3 dir, Vec3 center, Vec3 half, float maxDist) {
    float near = 0;
    if (intersects(origin - center, dir, {half * -1.f, half}, near, maxDist)) return near;
    return infinity;
}

// Seconds a line must stay up before a queued line may replace it.
double readSeconds(const char* line) {
    return std::clamp(0.5 + 0.042 * double(std::strlen(line)), 1.4, 3.4);
}

} // namespace

Vec3 MissionController::mapPoint(float x, float y, float height) {
    return {(x - 316.f) * 0.05f, height, (212.5f - y) * 0.05f};
}

MissionController::MissionController(const World& world, Navigation& nav)
    : world_(world), nav_(nav) {}

void MissionController::resetAll() {
    state_ = {};
    for (auto& a : agents_) a = {};
    for (auto& s : sectors_) s = {};
    radioQueue_ = {};
    radioQueued_ = 0;
    radioShownAt_ = 0;
    radioPriority_ = RadioChatter;
    introCue_ = 0;
    introSkipped_ = false;
    wakeSounds_ = false;
    overwatchHintSent_ = false;
    overwatchNudgeSent_ = false;
    shedSniperHintSent_ = false;
    regroupBriefSent_ = false;
    victoryAnnounced_ = false;
    incomingArrived_ = false;
    lzCallSent_ = false;
    pendingFlankers_ = 0;
    flankersCalled_ = false;
    wallCalled_ = false;
    roarReadyAt_ = 0;
    exfil_ = false;
    exfilCue_ = 0;
    exfilTime_ = 0;
    liftAt_ = 0;
    evacBeat_ = 0;
    reinforcements_ = 0;
    introTime_ = 0;
    phaseTime_ = 0;
    elapsed_ = 0;
    heartbeatAt_ = 0;
    failDelay_ = 0;
    incomingArrivedAt_ = 0;
    evacTimer_ = 0;
    evacDuration_ = 1;
    nextReinforceAt_ = 0;
    graceUntil_ = 0;
    graceReadyAt_ = 0;
}

void MissionController::showRadio(const RadioLine& line, MissionHost& host, bool chirp) {
    copyText(state_.radioSpeaker, sizeof(state_.radioSpeaker), line.speaker);
    copyText(state_.radioLine, sizeof(state_.radioLine), line.text);
    state_.radioTimer = line.duration;
    radioShownAt_ = elapsed_;
    radioPriority_ = line.priority;
    if (chirp) host.playSound(Sound::RadioChirp, 0.52f, false, {});
}

void MissionController::setRadio(const char* speaker, const char* line, float duration,
                                 MissionHost& host, bool chirp, uint8_t priority, int sector) {
    if (state_.radioTimer > 0 && !std::strcmp(state_.radioLine, line)) return;
    for (int i = 0; i < radioQueued_; ++i) {
        if (!std::strcmp(radioQueue_[i].text, line)) return;
    }
    RadioLine next{};
    copyText(next.speaker, sizeof(next.speaker), speaker);
    copyText(next.text, sizeof(next.text), line);
    next.duration = duration;
    next.priority = priority;
    next.sector = int8_t(sector);
    next.queuedAt = elapsed_;

    const bool idle = state_.radioTimer <= 0;
    const bool read = elapsed_ - radioShownAt_ >= readSeconds(state_.radioLine);
    const bool interrupt = priority == RadioUrgent && radioPriority_ < RadioUrgent;
    if (idle || interrupt || (read && radioQueued_ == 0)) {
        showRadio(next, host, chirp);
        return;
    }
    // Keep the queue ordered by priority, oldest first within a priority.
    int at = radioQueued_;
    while (at > 0 && radioQueue_[at - 1].priority < priority) --at;
    if (radioQueued_ == int(radioQueue_.size())) {
        if (at == radioQueued_) return; // Lower priority than everything waiting
        --radioQueued_;
    }
    for (int i = radioQueued_; i > at; --i) radioQueue_[i] = radioQueue_[i - 1];
    radioQueue_[at] = next;
    ++radioQueued_;
}

void MissionController::pumpRadio(MissionHost& host) {
    // Chatter that waited too long no longer matches what is on screen.
    int kept = 0;
    for (int i = 0; i < radioQueued_; ++i) {
        const double waited = elapsed_ - radioQueue_[i].queuedAt;
        const int sector = radioQueue_[i].sector;
        const bool stale = (radioQueue_[i].priority == RadioChatter ? waited > 4.0 : waited > 9.0) ||
                           (sector >= 0 && sector < 3 && sectors_[sector].resolved);
        if (!stale) radioQueue_[kept++] = radioQueue_[i];
    }
    radioQueued_ = kept;
    if (!radioQueued_) return;
    if (state_.radioTimer > 0 && elapsed_ - radioShownAt_ < readSeconds(state_.radioLine)) return;
    const RadioLine next = radioQueue_[0];
    for (int i = 1; i < radioQueued_; ++i) radioQueue_[i - 1] = radioQueue_[i];
    --radioQueued_;
    showRadio(next, host, true);
}

void MissionController::setObjective(const char* mainText, const char* subText) {
    copyText(state_.objective, sizeof(state_.objective), mainText);
    copyText(state_.subObjective, sizeof(state_.subObjective), subText);
}

void MissionController::addTracer(Vec3 from, Vec3 to, uint16_t color) {
    size_t chosen = 0;
    for (size_t i = 0; i < state_.tracers.size(); ++i) {
        if (state_.tracers[i].time <= 0) { chosen = i; break; }
        if (state_.tracers[i].time < state_.tracers[chosen].time) chosen = i;
    }
    state_.tracers[chosen] = {from, to, 1.f, color};
}

void MissionController::emitScannerBeam(const Agent& a) {
    const bool drone = a.role == MissionRole::AlienDrone;
    const Vec3 eye = drone ? a.actor.position : atHeight(a.actor.position, (alienMouth + .06f) * alienScale(a.variant));
    const float sweepYaw = a.actor.yaw + std::sin(float(elapsed_) * (drone ? 1.15f : 0.85f) + a.hoverPhase) * 0.45f;
    const float pitch = drone ? 0.36f : 0.14f;
    const Vec3 dir = forward(sweepYaw, pitch);
    // OBB world_.ray is ~18x faster than full shot-triangle mesh intersection.
    const float dist = std::min(14.f, world_.ray(eye, dir, 14.f));
    if (dist > 0.6f) {
        size_t chosen = 0;
        for (size_t i = 0; i < state_.tracers.size(); ++i) {
            if (state_.tracers[i].time <= 0) { chosen = i; break; }
            if (state_.tracers[i].time < state_.tracers[chosen].time) chosen = i;
        }
        state_.tracers[chosen] = {eye + dir * 0.35f, eye + dir * dist, 0.28f, a.variant > 0 ? crimsonPlasma : scanCyan};
    }
}

void MissionController::spawnCommander(Vec3 pos, float yaw) {
    auto& cmd = agents_[0];
    cmd = {};
    cmd.used = true;
    cmd.role = MissionRole::Commander;
    cmd.sector = -1;
    cmd.actor.position = pos;
    cmd.actor.yaw = yaw;
    cmd.actor.health = 250.f;
    cmd.actor.visible = true;
    cmd.maxHealth = 250.f;
    cmd.home = pos;
    cmd.goal = pos;
    cmd.radar = 1.f;
}

void MissionController::spawnCivilian(int slot, int sector, Vec3 pos, float yaw, uint8_t outfit) {
    auto& civ = agents_[slot];
    civ = {};
    civ.used = true;
    civ.role = MissionRole::Civilian;
    civ.variant = outfit;
    civ.sector = int8_t(sector);
    civ.actor.position = pos;
    civ.actor.yaw = yaw;
    civ.actor.health = 85.f;
    civ.actor.visible = true;
    civ.maxHealth = 85.f;
    civ.home = pos;
    civ.goal = pos;
    civ.radar = 1.f;
}

int MissionController::spawnAlien(MissionRole role, int sector, Vec3 pos, float yaw, uint8_t variant, int difficulty) {
    int chosen = -1;
    for (int i = 4; i < int(agents_.size()); ++i) {
        if (!agents_[i].used || (agents_[i].dead && agents_[i].deathElapsed > 1.8)) {
            chosen = i;
            break;
        }
    }
    if (chosen < 0) return -1;
    auto& a = agents_[chosen];
    a = {};
    a.used = true;
    a.role = role;
    a.variant = variant;
    a.sector = int8_t(sector);
    a.actor.position = pos;
    a.actor.yaw = yaw;
    a.actor.visible = true;
    const float baseHp = role == MissionRole::AlienDrone ? (42.f + difficulty * 5.f) : (62.f + difficulty * 8.f);
    a.maxHealth = variant > 0 ? baseHp * 1.45f : baseHp;
    a.actor.health = a.maxHealth;
    a.home = pos;
    a.goal = pos;
    a.hoverBase = pos.y > 0.5f ? pos.y : 2.0f;
    a.hoverPhase = float(rng_.draw("drone.phase") * 2 * pi);
    a.fireCooldown = 0.65 + rng_.draw("alien.fire") * 0.6;
    a.radar = 0.f;
    return chosen;
}

// Reinforcements arrive on a visible drop beam so the player can read where they land.
int MissionController::dropAlien(MissionRole role, Vec3 pos, uint8_t variant, const Snapshot& snap, MissionHost& host) {
    const Vec3 to = snap.player.position - pos;
    const int slot = spawnAlien(role, -1, pos, std::atan2(to.x, to.z), variant, difficulty_);
    if (slot < 0) return -1;
    auto& a = agents_[slot];
    a.alerted = true;
    a.spawnFx = 0.75;
    a.radar = 2.f;
    a.burstPause = 0.9 + rng_.draw("drop.settle") * 0.5;
    if (role == MissionRole::AlienTrooper) roar(pos, host);
    else host.playSound(Sound::PlasmaFire, 0.7f, true, pos);
    state_.aliensRemaining = livingAliens();
    return slot;
}

void MissionController::spawnOverwatchWave(int difficulty) {
    // Fifteen to eighteen metres out on the road, visible through the railing from the portico.
    // Picking through the Warhorse wreck either side of it, with a drone scanning overhead.
    spawnAlien(MissionRole::AlienTrooper, -1, mapPoint(458, 690), -0.2f, 0, difficulty);
    spawnAlien(MissionRole::AlienTrooper, -1, mapPoint(296, 700), 0.35f, 0, difficulty);
    spawnAlien(MissionRole::AlienDrone, -1, mapPoint(380, 640, 3.6f), 0.f, 0, difficulty);
    for (int i = 4; i < int(agents_.size()); ++i) {
        if (agents_[i].used && !agents_[i].dead) {
            agents_[i].alerted = false;
            agents_[i].radar = 1.f;
        }
    }
    state_.aliensRemaining = livingAliens();
}

void MissionController::spawnSectorWave(int sector, int difficulty) {
    if (sector < 0 || sector >= 3 || sectors_[sector].spawned) return;
    sectors_[sector].spawned = true;
    if (sector == 0) {
        spawnAlien(MissionRole::AlienTrooper, 0, mapPoint(188, 128), pi, 0, difficulty);
        spawnAlien(MissionRole::AlienTrooper, 0, mapPoint(188, 344), 0.f, 0, difficulty);
    } else if (sector == 1) {
        spawnAlien(MissionRole::AlienTrooper, 1, mapPoint(605, 364), 0.f, 0, difficulty);
        spawnAlien(MissionRole::AlienDrone, 1, mapPoint(548, 215, 1.98f), pi / 2, 0, difficulty);
    } else {
        spawnAlien(MissionRole::AlienTrooper, 2, mapPoint(-24, 152), pi, 0, difficulty);
        spawnAlien(MissionRole::AlienDrone, 2, mapPoint(-20, 316, 2.02f), 0.f, 0, difficulty);
    }
    state_.aliensRemaining = livingAliens();
}

void MissionController::spawnExtractionWave(const Snapshot& snap, MissionHost& host) {
    // The enforcer squad lands across the rear lawn, well clear of the patio.
    dropAlien(MissionRole::AlienTrooper, mapPoint(120, -70), 1, snap, host);
    dropAlien(MissionRole::AlienTrooper, mapPoint(560, -70), 0, snap, host);
    dropAlien(MissionRole::AlienDrone, mapPoint(250, -80, 2.2f), 0, snap, host);
    if (difficulty_ >= 3) dropAlien(MissionRole::AlienTrooper, mapPoint(-30, -60), 0, snap, host);
    state_.aliensRemaining = livingAliens();
}

bool MissionController::spawnReinforcement(const Snapshot& snap, MissionHost& host) {
    struct Drop { float x, y, h; };
    static constexpr Drop ground[]{{-30, -60, 0}, {370, -78, 0}, {640, -66, 0}, {-40, 180, 0}, {120, -70, 0}, {560, -70, 0}};
    static constexpr Drop air[]{{-40, 460, 2.1f}, {640, 450, 2.1f}, {250, -80, 2.2f}, {520, -80, 2.2f}};
    const bool drone = reinforcements_ % 3 == 2;
    const int enforcerEvery = difficulty_ >= 7 ? 2 : difficulty_ >= 4 ? 3 : 4;
    const uint8_t variant = !drone && reinforcements_ % enforcerEvery == enforcerEvery - 1 ? 1 : 0;

    const Drop* options = drone ? air : ground;
    const int count = drone ? int(std::size(air)) : int(std::size(ground));
    int best = -1;
    float bestScore = -infinity;
    for (int i = 0; i < count; ++i) {
        const Vec3 at = mapPoint(options[i].x, options[i].y, options[i].h);
        const float dist = planar(at - snap.player.position);
        // Never drop on top of the player; prefer far points with some variety.
        const float score = (dist < 9.f ? -100.f : 0.f) + std::min(dist, 22.f) + float(rng_.draw("evac.drop") * 7.0);
        if (score > bestScore) {
            bestScore = score;
            best = i;
        }
    }
    if (best < 0) return false;
    const Drop& d = options[best];
    if (dropAlien(drone ? MissionRole::AlienDrone : MissionRole::AlienTrooper, mapPoint(d.x, d.y, d.h), variant, snap, host) < 0) {
        return false;
    }
    ++reinforcements_;
    return true;
}

void MissionController::saveCheckpoint(int stage) {
    checkpointStage_ = std::max(checkpointStage_, stage);
    state_.checkpointStage = uint8_t(checkpointStage_);
    state_.checkpointReached = true;
    checkpoint_.saved = state_.civiliansSaved;
    checkpoint_.lost = state_.civiliansLost;
    checkpoint_.kills = state_.aliensKilled;
    checkpoint_.headshots = state_.headshots;
    checkpoint_.seconds = state_.missionSeconds;
    for (int c = 0; c < 3; ++c) {
        const auto& civ = agents_[c + 1];
        checkpoint_.civilians[c] = civ.dead ? 2 : civ.rescued ? 1 : 0;
    }
}

void MissionController::start(uint32_t seed, int difficulty, bool fromCheckpoint, Snapshot& snap,
                              Vec3& playerPosOut, float& playerYawOut, MissionHost& host) {
    resetAll();
    difficulty_ = std::clamp(difficulty, 1, 10);
    rng_.state = seed ? seed : 0x44454c54u;
    rng_.tick = 0;

    state_.active = true;
    state_.civiliansTotal = 3;
    snap.brokenGlass |= (uint64_t(1) << 2) | (uint64_t(1) << 3) | (uint64_t(1) << 4);

    sectors_[0] = {false, false, false, false, 0.0, 1, mapPoint(188, 234)};
    sectors_[1] = {false, false, false, false, 0.0, 2, mapPoint(603, 281)};
    sectors_[2] = {false, false, false, false, 0.0, 3, mapPoint(20, 227)};
    spawnCivilian(1, 0, mapPoint(188, 234), pi / 2, 0);
    spawnCivilian(2, 1, mapPoint(603, 281), -pi / 2, 1);
    spawnCivilian(3, 2, mapPoint(20, 227), -pi / 2, 2);

    const Vec3 hallWake = mapPoint(368, 305);
    baseWakeYaw_ = pi - 0.14f;

    if (fromCheckpoint && checkpointStage_ > 0) {
        ++retries_;
        state_.retries = retries_;
        state_.checkpointReached = true;
        state_.checkpointStage = uint8_t(checkpointStage_);
        state_.civiliansSaved = checkpoint_.saved;
        state_.civiliansLost = checkpoint_.lost;
        state_.aliensKilled = checkpoint_.kills;
        state_.headshots = checkpoint_.headshots;
        state_.missionSeconds = checkpoint_.seconds;
        state_.introProgress = 1.f;
        state_.concussion = 0.f;
        state_.letterbox = 0.f;
        if (checkpointStage_ >= 3) {
            for (int s = 0; s < 3; ++s) sectors_[s].spawned = sectors_[s].triggered = sectors_[s].resolved = true;
            for (int c = 0; c < 3; ++c) {
                auto& civ = agents_[c + 1];
                if (checkpoint_.civilians[c] == 1) {
                    civ.rescued = true;
                    civ.actor.position = mapPoint(evacSpots[c][0], evacSpots[c][1]);
                    civ.actor.yaw = 0.f;
                } else {
                    civ.dead = true;
                    civ.deathElapsed = 10.0;
                    civ.actor.visible = false;
                }
            }
            playerPosOut = mapPoint(368, 48);
            playerYawOut = 0.f;
            spawnCommander(mapPoint(commanderPatio[0], commanderPatio[1]), 0.f);
            state_.phase = MissionPhase::Incoming;
            setObjective("FALL BACK TO THE REAR PATIO", "REGROUP WITH DELTA-1 AT THE EVAC POINT");
            setRadio("DELTA-1", "CHECKPOINT LOADED. ON ME AT THE PATIO - THEY'RE COMING BACK.", 4.0f, host);
        } else if (checkpointStage_ == 1) {
            // Back on the portico line with the road patrol reset.
            playerPosOut = mapPoint(368, 330);
            playerYawOut = pi;
            spawnCommander(mapPoint(332, 352), pi);
            enterOverwatch(snap, host, true);
            setRadio("DELTA-1", "CHECKPOINT LOADED. THEY'RE STILL ON THE ROAD - TAKE THE FIRST SHOT.", 4.2f, host, true,
                     RadioUrgent);
        } else {
            state_.phase = MissionPhase::Rescue;
            playerPosOut = mapPoint(368, 290);
            playerYawOut = -pi / 2;
            spawnCommander(mapPoint(322, 313), 0.f);
            setObjective("RESCUE THE MARSHES 0/3", "FIND BEN, DR. MARSH AND WALT");
            setRadio("DELTA-1", "CHECKPOINT LOADED. MOVE SLOW - THE MARSHES ARE COUNTING ON US.", 4.6f, host);
        }
        updateWaypoint(snap);
        return;
    }

    checkpointStage_ = 0;
    retries_ = 0;
    checkpoint_ = {};
    state_.checkpointReached = false;
    state_.phase = MissionPhase::Intro;
    state_.introProgress = 0.f;
    state_.concussion = 1.f;
    state_.letterbox = 1.f;
    playerPosOut = hallWake;
    playerYawOut = baseWakeYaw_;
    spawnCommander(mapPoint(388, 336), -0.58f);
    setObjective("OPERATION BRIAR", "BRIAR HOUSE, COLORADO");
    // The title card plays over the closed eyes first; the wake-up starts at introTime_ zero.
    introTime_ = -titleCardDuration;
    state_.titleCard = 0.001f;
    heartbeatAt_ = 1.35;
    updateWaypoint(snap);
}

void MissionController::updateIntro(Snapshot& snap, const Input& input, double dt, MissionHost& host) {
    introTime_ += dt;
    const bool press = input.edge(Jump) || input.edge(Confirm);
    if (introTime_ < 0) {
        // Title card: one press moves on to the wake-up, a second skips that too.
        if (press && introTime_ > .6 - titleCardDuration) introTime_ = 0;
        else state_.titleCard = float(titleCardDuration + introTime_);
    }
    if (introTime_ >= 0) state_.titleCard = 0.f;
    if (introTime_ >= 0 && !wakeSounds_) {
        wakeSounds_ = true;
        host.playSound(Sound::ConcussionRing, 0.68f, false, {});
        host.playSound(Sound::Heartbeat, 0.82f, false, {});
        host.playSound(Sound::Airstrike, 0.28f, true, mapPoint(372, 622));
        state_.heartbeatPulse = 0.9f;
    }
    if (introTime_ > 0.9 && press) {
        introSkipped_ = true;
    }
    const double progress = introSkipped_ ? 1.0 : std::clamp(introTime_ / introDuration, 0.0, 1.0);
    state_.introProgress = float(progress);
    state_.concussion = float(std::max(0.0, 1.0 - progress * 1.08));
    state_.letterbox = float(std::clamp((1.0 - progress) * 2.2, 0.0, 1.0));

    state_.heartbeatPulse = std::max(0.f, state_.heartbeatPulse - float(dt * 1.7));
    if (!introSkipped_ && introTime_ >= heartbeatAt_ && state_.concussion > 0.25f) {
        heartbeatAt_ += 1.32;
        state_.heartbeatPulse = std::min(0.85f, state_.concussion);
        host.playSound(Sound::Heartbeat, 0.65f * state_.concussion, false, {});
    }

    const float rise = smooth(std::clamp((progress - 0.34) / 0.60, 0.0, 1.0));
    snap.eyeHeight = proneEye + (standEye - proneEye) * rise;
    snap.prone = rise < 0.35f;
    snap.crouch = rise >= 0.35f && rise < 0.8f;

    const float dizzy = (1.f - rise);
    const float swayYaw = std::sin(float(introTime_) * 1.9f) * 0.14f * dizzy;
    const float swayPitch = (-0.26f * dizzy) + std::cos(float(introTime_) * 2.5f) * 0.07f * dizzy;
    snap.camera.position = atHeight(snap.player.position, snap.eyeHeight);
    snap.camera.yaw = snap.playerYaw + swayYaw;
    snap.camera.pitch = std::clamp(snap.playerPitch + swayPitch, -1.2f, 1.2f);
    snap.camera.fov = (70.f + 10.f * dizzy * (0.5f + 0.5f * std::sin(float(introTime_) * 3.8f))) * pi / 180.f;

    if (introCue_ == 0 && introTime_ >= 0.55) {
        introCue_ = 1;
        setRadio("DELTA-1", "HEY! STAY WITH ME - EYES ON ME!", 2.2f, host, true);
    } else if (introCue_ == 1 && introTime_ >= 2.55) {
        introCue_ = 2;
        addTracer(mapPoint(430, 680, 2.2f), mapPoint(315, 395, 1.6f), cyanPlasma);
        host.playSound(Sound::PlasmaFire, 0.58f, true, mapPoint(400, 620));
        setRadio("DELTA-1", "WARHORSE IS DOWN. IT'S JUST US NOW - GET UP!", 2.5f, host, true);
    } else if (introCue_ == 2 && introTime_ >= 4.85) {
        introCue_ = 3;
        agents_[0].actor.yaw = pi;
        addTracer(mapPoint(320, 700, 1.8f), mapPoint(412, 385, 2.1f), cyanPlasma);
        host.playSound(Sound::PlasmaFire, 0.55f, true, mapPoint(330, 640));
        setRadio("DELTA-1", "EASY... STAY LOW. EYES ON THE FRONT ROAD.", 2.6f, host, true);
    }

    if (progress >= 1.0) {
        snap.prone = snap.crouch = false;
        snap.eyeHeight = standEye;
        enterOverwatch(snap, host, introSkipped_);
    }
}

void MissionController::enterOverwatch(Snapshot& snap, MissionHost& host, bool urgent) {
    state_.phase = MissionPhase::Overwatch;
    phaseTime_ = 0;
    state_.introProgress = 1.f;
    state_.concussion = 0.f;
    state_.letterbox = 0.f;
    state_.heartbeatPulse = 0.f;
    spawnOverwatchWave(difficulty_);
    saveCheckpoint(1);
    updateWaypoint(snap);
    setObjective("OVERWATCH - FRONT ROAD", "SWAP TO THE M40A3 AND ENGAGE ON YOUR MARK");
    // A skipped intro or a reload replaces whatever line was still showing.
    setRadio("DELTA-1", "SWAP TO YOUR M40A3. THEY'RE PICKING THROUGH THE WRECK ON THE ROAD...", 4.2f, host, true,
             urgent ? RadioUrgent : RadioOrder);
}

void MissionController::updateOverwatch(Snapshot& snap, double dt, bool playerFired, MissionHost& host) {
    phaseTime_ += dt;
    if (!overwatchHintSent_ && phaseTime_ >= 4.2) {
        overwatchHintSent_ = true;
        setRadio("DELTA-1", "FIRST ONES I'VE SEEN UP CLOSE. HOLD STEADY - WE ENGAGE ON YOUR MARK.", 4.2f, host, true,
                 RadioChatter);
    }
    if (!overwatchNudgeSent_ && phaseTime_ >= overwatchNudge) {
        overwatchNudgeSent_ = true;
        setRadio("DELTA-1", "THEY'RE SWEEPING CLOSER. TAKE THE SHOT WHEN YOU HAVE IT.", 3.6f, host, true);
    }

    const bool steppedOutside = snap.player.position.z < -10.5f;
    const bool spotted = phaseTime_ >= overwatchSpotted;
    bool anyAlerted = false;
    for (int i = 4; i < int(agents_.size()); ++i) {
        if (agents_[i].used && (agents_[i].alerted || agents_[i].dead)) anyAlerted = true;
    }
    if (playerFired || anyAlerted || steppedOutside || spotted) {
        clearRadioQueue();
        state_.phase = MissionPhase::Breach;
        phaseTime_ = 0;
        pendingFlankers_ = difficulty_ <= 2 ? 1 : 2;
        for (int i = 4; i < int(agents_.size()); ++i) {
            if (agents_[i].used && !agents_[i].dead) agents_[i].alerted = true;
        }
        const bool cleanOpener = state_.aliensKilled > 0;
        for (int i = 4; i < int(agents_.size()); ++i) {
            if (agents_[i].used && !agents_[i].dead && agents_[i].role == MissionRole::AlienTrooper) {
                roar(agents_[i].actor.position, host);
                break;
            }
        }
        setObjective("REPEL THE ALIEN BREACH", "HOLD THE HALL - TAP SWAP BACK TO THE MP44");
        setRadio("DELTA-1",
                 cleanOpener ? "CLEAN SHOT! THE REST ARE GOING FOR THE WALL - HOLD THE HALL!"
                 : spotted   ? "THEY'VE SPOTTED US! WEAPONS FREE - HOLD THE HALL!"
                             : "CONTACT ON THE ROAD! WEAPONS FREE - HOLD THE HALL!",
                 3.8f, host, true, RadioUrgent);
    }
}

void MissionController::steerAlongPath(Agent& agent, Vec3 target, float speed, double dt, bool flying) {
    const Vec3 delta = target - agent.actor.position;
    if (planarSquared(delta) < 0.42f) {
        agent.velocity = {};
        agent.actor.speed = 0;
        if (flying) {
            agent.hoverPhase += float(dt * 2.4);
            agent.actor.position.y = agent.hoverBase + std::sin(agent.hoverPhase) * 0.14f;
        }
        return;
    }

    Vec3 waypoint = target;
    const bool obstructed = flying
        ? world_.blocked(agent.actor.position, atHeight(target, agent.hoverBase))
        : world_.walkBlocked(agent.actor.position, target);
    if (obstructed) {
        if (elapsed_ >= agent.repathAt || agent.pathIndex >= agent.path.size()) {
            const Vec3 fromPos = flying ? Vec3{agent.actor.position.x, 0, agent.actor.position.z} : agent.actor.position;
            const Vec3 toPos = flying ? Vec3{target.x, 0, target.z} : target;
            if (nav_.path(nav_.nearest(fromPos), nav_.nearest(toPos), agent.path)) {
                agent.pathIndex = agent.path.size() > 1 ? 1 : 0;
            } else {
                agent.path.clear();
                agent.pathIndex = 0;
            }
            agent.repathAt = elapsed_ + 0.85;
        }
        const float reachSq = flying ? 0.64f : 0.42f;
        while (agent.pathIndex < agent.path.size() &&
               planarSquared(nav_.node(agent.path[agent.pathIndex]).position - agent.actor.position) < reachSq) {
            ++agent.pathIndex;
        }
        if (agent.pathIndex < agent.path.size()) {
            waypoint = nav_.node(agent.path[agent.pathIndex]).position;
        }
    }

    Vec3 dir{waypoint.x - agent.actor.position.x, 0, waypoint.z - agent.actor.position.z};
    const float len = planar(dir);
    if (len > 0.01f) dir *= (speed / len);
    agent.velocity = dir;
    if (onRoad(agent.actor.position)) {
        agent.actor.position.x += dir.x * float(dt);
        agent.actor.position.z += dir.z * float(dt);
        if (flying) {
            agent.hoverPhase += float(dt * 2.8);
            agent.actor.position.y = agent.hoverBase + std::sin(agent.hoverPhase) * 0.14f;
        }
        agent.actor.speed = planar(dir);
    } else if (flying) {
        Vec3 groundPos{agent.actor.position.x, 0, agent.actor.position.z};
        Vec3 groundVel{dir.x, 0, dir.z};
        world_.move(groundPos, groundVel, float(dt), 1.8f, 0.42f);
        agent.hoverPhase += float(dt * 2.8);
        agent.actor.position = {groundPos.x, agent.hoverBase + std::sin(agent.hoverPhase) * 0.14f, groundPos.z};
        agent.actor.speed = planar(groundVel);
    } else {
        world_.move(agent.actor.position, agent.velocity, float(dt), 1.85f, 0.4f);
        agent.actor.speed = planar(agent.velocity);
    }
    if (agent.actor.speed > 0.25f) {
        const float cycle = agent.actor.speed >= 3.1f ? 0.62f : 0.83f;
        agent.actor.phase = std::fmod(agent.actor.phase + agent.actor.speed * float(dt) / (4.f * cycle), 1.f);
    }
}

void MissionController::updateCommander(Snapshot& snap, double dt, MissionHost& host) {
    auto& cmd = agents_[0];
    if (!cmd.used) return;
    cmd.radar = 1.f;
    if (state_.phase == MissionPhase::Intro) {
        cmd.actor.speed = 0;
        host.updateActorFrame(cmd.actor);
        return;
    }

    Vec3 anchor = mapPoint(322, 313);
    float speed = 3.8f;
    if (state_.phase == MissionPhase::Incoming || state_.phase == MissionPhase::Extraction) {
        anchor = mapPoint(commanderPatio[0], commanderPatio[1]);
        speed = 4.6f;
    } else if (state_.phase == MissionPhase::Regroup || state_.phase == MissionPhase::Rescue) {
        // Delta-1 shadows the player through the house, a few paces behind.
        const Vec3 toPlayer{snap.player.position.x - cmd.actor.position.x, 0, snap.player.position.z - cmd.actor.position.z};
        const float dist = planar(toPlayer);
        if (dist > 6.f) cmd.following = true;
        else if (dist < 3.4f) cmd.following = false;
        if (cmd.following) {
            anchor = snap.player.position - toPlayer * (2.8f / std::max(dist, 0.01f));
            speed = dist > 10.f ? 5.4f : 4.2f;
        } else {
            anchor = cmd.actor.position;
        }
    }
    // Step out of the player's line of fire rather than soaking up their rounds, and stay
    // out while his usual post is still in that line.
    const Vec3 aimDir = forward(snap.camera.yaw, snap.camera.pitch);
    const auto laneOffset = [&](Vec3 at, Vec3& off) {
        const Vec3 rel = atHeight(at, 1.3f) - snap.camera.position;
        const float along = dot(rel, aimDir);
        if (snap.dead || along <= 0.4f || along >= 24.f) return infinity;
        off = {rel.x - aimDir.x * along, 0, rel.z - aimDir.z * along};
        return planar(off);
    };
    Vec3 off{};
    const float lateral = laneOffset(cmd.actor.position, off);
    if (lateral < 1.1f && elapsed_ >= cmd.dodgeUntil) {
        const Vec3 side = lateral > 0.05f ? off * (1.f / lateral) : normalized(Vec3{aimDir.z, 0, -aimDir.x});
        cmd.goal = cmd.actor.position + side * 2.6f;
        cmd.dodgeUntil = elapsed_ + 0.7;
    }
    Vec3 anchorOff{};
    if (elapsed_ < cmd.dodgeUntil || (cmd.dodgeUntil > 0 && laneOffset(anchor, anchorOff) < 1.4f)) {
        anchor = cmd.goal;
        speed = std::max(speed, 4.8f);
    }
    steerAlongPath(cmd, anchor, speed, dt, false);

    if (state_.phase == MissionPhase::Overwatch) {
        const Vec3 lookSouth = mapPoint(368, 456) - cmd.actor.position;
        const float wantedYaw = std::atan2(lookSouth.x, lookSouth.z);
        cmd.actor.yaw += wrapDelta(cmd.actor.yaw, wantedYaw) * float(1.0 - std::exp(-6.0 * dt));
        host.updateActorFrame(cmd.actor);
        return;
    }

    int bestTarget = -1;
    float bestDist = 26.f;
    const Vec3 eye = atHeight(cmd.actor.position, botEye);
    for (int i = 4; i < int(agents_.size()); ++i) {
        const auto& alien = agents_[i];
        if (!alien.used || alien.dead || !alien.alerted || alien.spawnFx > 0) continue;
        const Vec3 aim = alien.role == MissionRole::AlienDrone ? alien.actor.position
                                                               : atHeight(alien.actor.position, alienChest * alienScale(alien.variant));
        const float d = length(aim - eye);
        if (d < bestDist && !world_.blocked(eye, aim)) {
            bestDist = d;
            bestTarget = i;
        }
    }

    if (bestTarget >= 0) {
        const auto& target = agents_[bestTarget];
        const Vec3 aim = target.role == MissionRole::AlienDrone ? target.actor.position
                                                                : atHeight(target.actor.position, alienChest * alienScale(target.variant));
        const float wantedYaw = std::atan2(aim.x - eye.x, aim.z - eye.z);
        cmd.actor.yaw += wrapDelta(cmd.actor.yaw, wantedYaw) * float(1.0 - std::exp(-10.0 * dt));
        cmd.fireCooldown -= dt;
        if (cmd.fireCooldown <= 0 && std::abs(wrapDelta(cmd.actor.yaw, wantedYaw)) < 0.28f) {
            cmd.fireCooldown = 0.24 + rng_.draw("cmd.fire") * 0.16;
            const Vec3 muzzle = atHeight(cmd.actor.position, 1.42f) + forward(cmd.actor.yaw, 0) * 0.4f;
            Vec3 dir = aim - muzzle;
            dir.x += float(rng_.between("cmd.spread.x", -0.45, 0.45));
            dir.y += float(rng_.between("cmd.spread.y", -0.35, 0.35));
            dir.z += float(rng_.between("cmd.spread.z", -0.45, 0.45));
            dir = normalized(dir);
            const float maxD = std::min(40.f, world_.ray(muzzle, dir, 40.f));
            const Vec3 end = muzzle + dir * std::min(maxD, length(aim - muzzle));
            addTracer(muzzle, end, amberTracer);
            host.playSound(Sound::Mp44, 0.62f, true, muzzle);
            if (rng_.draw("cmd.hit") < 0.5) {
                damageAgent(bestTarget, 13.f, false, false, snap, host);
            }
        }
    } else if (cmd.actor.speed > 0.3f) {
        const float moveYaw = std::atan2(cmd.velocity.x, cmd.velocity.z);
        cmd.actor.yaw += wrapDelta(cmd.actor.yaw, moveYaw) * float(1.0 - std::exp(-8.0 * dt));
    }

    host.updateActorFrame(cmd.actor);
}

void MissionController::updateCivilian(int slot, Snapshot& snap, double dt) {
    auto& civ = agents_[slot];
    if (!civ.used) return;
    if (civ.dead) {
        civ.deathElapsed += dt;
        const float k = float(std::clamp(civ.deathElapsed / 0.9, 0.0, 1.0));
        civ.actor.fall = k * k * (1.6f - 0.6f * k);
        return;
    }

    civ.radar = 1.f;
    if (civ.rescued) {
        const int spot = std::clamp(int(civ.sector), 0, 2);
        const Vec3 dest = mapPoint(evacSpots[spot][0], evacSpots[spot][1]);
        steerAlongPath(civ, dest, 4.4f, dt, false);
        if (civ.actor.speed > 0.3f) {
            const float moveYaw = std::atan2(civ.velocity.x, civ.velocity.z);
            civ.actor.yaw += wrapDelta(civ.actor.yaw, moveYaw) * float(1.0 - std::exp(-9.0 * dt));
        }
    } else {
        civ.actor.speed = 0;
        const Vec3 toPlayer = snap.player.position - civ.actor.position;
        if (planarSquared(toPlayer) < 100.f) {
            const float lookYaw = std::atan2(toPlayer.x, toPlayer.z);
            civ.actor.yaw += wrapDelta(civ.actor.yaw, lookYaw) * float(1.0 - std::exp(-5.0 * dt));
        }
    }
    poseCivilian(civ);
}

// Pinned civilians cower and shiver; rescued ones run for the back door, then stand.
void MissionController::poseCivilian(Agent& c) const {
    if (!c.rescued) {
        c.actor.frame = 0;
        c.actor.nextFrame = 1;
        c.actor.frameBlend = 0.5f + 0.5f * std::sin(float(elapsed_) * 9.f + float(c.sector) * 2.f);
    } else if (c.actor.speed > 0.4f) {
        const float sample = c.actor.phase * civilianRunPoses;
        const int i = std::min(int(sample), int(civilianRunPoses) - 1);
        c.actor.frame = uint16_t(civilianCowerPoses + i);
        c.actor.nextFrame = uint16_t(civilianCowerPoses + (i + 1) % civilianRunPoses);
        c.actor.frameBlend = sample - float(i);
    } else {
        c.actor.frame = c.actor.nextFrame = civilianStandPose;
        c.actor.frameBlend = 0;
    }
}

// Ground aliens breathe and writhe in place, and lope through the stride poses on the move.
void MissionController::poseAlien(Agent& a) const {
    if (a.role != MissionRole::AlienTrooper) return;
    if (a.actor.speed > 0.4f) {
        const float sample = a.actor.phase * alienStridePoses;
        const int i = std::min(int(sample), int(alienStridePoses) - 1);
        a.actor.frame = uint16_t(alienIdlePoses + i);
        a.actor.nextFrame = uint16_t(alienIdlePoses + (i + 1) % alienStridePoses);
        a.actor.frameBlend = sample - float(i);
    } else {
        a.actor.frame = 0;
        a.actor.nextFrame = 1;
        a.actor.frameBlend = 0.5f - 0.5f * std::cos(float(elapsed_) * 2.6f + a.hoverPhase);
    }
}

// Only a few aliens may aim at the player at once; the rest pin Delta-1 or lay down
// loose suppressing fire. The cap rises with difficulty, nearer aliens with sight win.
void MissionController::assignAttackTokens(const Snapshot& snap) {
    const int tokens = difficulty_ <= 5 ? 1 : 2;
    const Vec3 playerEye = atHeight(snap.player.position, snap.eyeHeight);
    std::array<float, MissionSnapshot::maxActors> score{};
    score.fill(infinity);
    for (int i = 4; i < int(agents_.size()); ++i) {
        auto& a = agents_[i];
        const bool held = a.token;
        a.token = false;
        a.seesPlayer = false;
        if (!a.used || a.dead) continue;
        const Vec3 eye = a.role == MissionRole::AlienDrone ? a.actor.position
                                                           : atHeight(a.actor.position, alienMouth * alienScale(a.variant));
        const float dist = planar(snap.player.position - a.actor.position);
        a.seesPlayer = !snap.dead && dist < 26.f && !world_.blocked(eye, playerEye);
        if (!a.alerted || a.spawnFx > 0) continue;
        score[i] = dist - (a.seesPlayer ? 40.f : 0.f) - (held ? 4.f : 0.f);
    }
    // Hand out the slots to the lowest scores; there are only ever one or two.
    for (int k = 0; k < tokens; ++k) {
        int best = -1;
        for (int i = 4; i < int(agents_.size()); ++i) {
            if (score[i] < infinity && !agents_[i].token && (best < 0 || score[i] < score[best])) best = i;
        }
        if (best < 0) break;
        agents_[best].token = true;
    }
}

// A hit that would cross into last-stand health never kills outright and buys a short
// window to reach cover. The window shrinks with difficulty and recharges slowly.
void MissionController::hurtPlayer(float amount, Vec3 from, const Snapshot& snap, MissionHost& host) {
    if (elapsed_ < graceUntil_) return;
    const float hp = snap.player.health;
    if (hp > lastStandHealth && hp - amount <= lastStandHealth && elapsed_ >= graceReadyAt_) {
        amount = std::min(amount, hp - 5.f);
        graceUntil_ = elapsed_ + std::max(0.45, 1.2 - difficulty_ * 0.075);
        graceReadyAt_ = elapsed_ + 7.0;
    }
    host.damagePlayer(amount, from);
}

void MissionController::updateAlien(int slot, Snapshot& snap, double dt, bool playerFired, MissionHost& host) {
    auto& a = agents_[slot];
    if (!a.used) return;
    const bool drone = a.role == MissionRole::AlienDrone;
    if (a.dead) {
        a.deathElapsed += dt;
        const float k = float(std::clamp(a.deathElapsed / 0.85, 0.0, 1.0));
        a.actor.fall = k * k * (1.6f - 0.6f * k);
        if (drone) {
            a.actor.position.y = std::max(0.22f, a.actor.position.y - float(3.8 * dt));
            a.actor.yaw += float(4.2 * dt);
        }
        a.radar = 0;
        if (a.deathElapsed > 4.5) a.actor.visible = false;
        return;
    }

    poseAlien(a);

    if (a.spawnFx > 0) {
        a.spawnFx -= dt;
        // A pulsing column from the mothership marks the landing for a moment.
        if ((int(elapsed_ * 60.0) & 3) == 0) {
            const Vec3 base = drone ? a.actor.position : atHeight(a.actor.position, 0.1f);
            addTracer(base + Vec3{0, 26.f, 0}, base, a.variant > 0 ? crimsonPlasma : cyanPlasma);
        }
    }

    a.radar = std::max(0.f, a.radar - float(dt));
    const Vec3 eye = drone ? a.actor.position : atHeight(a.actor.position, alienMouth * alienScale(a.variant));
    const Vec3 playerEye = atHeight(snap.player.position, snap.eyeHeight);
    const float distPlayer = planar(snap.player.position - a.actor.position);
    const bool seesPlayer = a.seesPlayer;
    if (seesPlayer) a.sightTime += dt;
    else a.sightTime = std::max(0.0, a.sightTime - dt * 2.0);

    if (!a.alerted) {
        if (state_.phase != MissionPhase::Overwatch) {
            const bool closeSight = seesPlayer && distPlayer < 9.5f;
            if (closeSight || (playerFired && distPlayer < 16.f)) a.alerted = true;
        }
        if (!a.alerted) {
            const float patrolOffset = std::sin(float(elapsed_) * 0.45f + a.hoverPhase) * 1.9f;
            const Vec3 patrolTarget = a.home + Vec3{patrolOffset, 0, std::cos(float(elapsed_) * 0.35f + a.hoverPhase) * 1.1f};
            steerAlongPath(a, patrolTarget, drone ? 1.5f : 1.15f, dt, drone);
            emitScannerBeam(a);
            return;
        }
    }

    int targetCiv = -1;
    bool atCommander = false;
    Vec3 targetPos = snap.player.position;
    Vec3 targetAim = playerEye;
    bool hasTarget = !snap.dead && a.alerted;

    if (a.sector >= 0 && a.sector < 3 && sectors_[a.sector].triggered) {
        const int cSlot = sectors_[a.sector].civSlot;
        const auto& civ = agents_[cSlot];
        if (civ.used && !civ.dead && !civ.rescued) {
            if (seesPlayer) {
                a.threatTimer = std::max(0.0, a.threatTimer - dt * 1.5);
            } else {
                a.threatTimer += dt;
                if (a.threatTimer > 7.5) {
                    targetCiv = cSlot;
                    targetPos = civ.actor.position;
                    targetAim = atHeight(civ.actor.position, 0.65f);
                    hasTarget = true;
                }
            }
        }
    }

    // Without an aimed-fire slot, keep Delta-1 pinned when he is in the open.
    const auto& cmd = agents_[0];
    if (hasTarget && targetCiv < 0 && !a.token && cmd.used && state_.phase != MissionPhase::Overwatch) {
        const Vec3 cmdAim = atHeight(cmd.actor.position, 1.3f);
        if (planar(cmd.actor.position - a.actor.position) < 24.f && !world_.blocked(eye, cmdAim)) {
            atCommander = true;
            targetPos = cmd.actor.position;
            targetAim = cmdAim;
        }
    }

    if (!hasTarget) return;

    const float distTarget = planar(targetPos - a.actor.position);
    const bool seesTarget = atCommander || (targetCiv < 0 ? seesPlayer : !world_.blocked(eye, targetAim));
    const float preferredRange = drone ? 9.0f : 7.5f;

    if (vaultWall(a, dt, host)) {
        // The vault owns movement until the alien is inside the grounds.
    } else if (a.spawnFx > 0) {
        a.velocity = {};
        a.actor.speed = 0;
    } else if (targetCiv < 0) {
        tacticalMove(a, slot, snap, dt);
    } else if (!seesTarget || distTarget > preferredRange + 2.5f) {
        const float moveSpeed = drone ? 3.8f : (2.9f + difficulty_ * 0.11f);
        steerAlongPath(a, targetPos, moveSpeed, dt, drone);
    } else {
        const float strafeSign = (int(elapsed_ * 0.65 + slot) & 1) ? 1.f : -1.f;
        const Vec3 toT = normalized({targetPos.x - a.actor.position.x, 0, targetPos.z - a.actor.position.z});
        const Vec3 side{toT.z * strafeSign, 0, -toT.x * strafeSign};
        steerAlongPath(a, a.actor.position + side * 2.2f, drone ? 2.4f : 1.9f, dt, drone);
    }

    const float wantedYaw = std::atan2(targetAim.x - eye.x, targetAim.z - eye.z);
    a.actor.yaw += wrapDelta(a.actor.yaw, wantedYaw) * float(1.0 - std::exp(-8.0 * dt));

    a.fireCooldown -= dt;
    a.burstPause -= dt;
    const bool suppressing = targetCiv < 0 && !atCommander && !a.token;
    if (seesTarget && a.spawnFx <= 0 && a.leapAt < 0 && std::abs(wrapDelta(a.actor.yaw, wantedYaw)) < 0.35f) {
        if (a.burstLeft <= 0 && a.burstPause <= 0) {
            a.burstLeft = drone ? 2 : (a.variant > 0 ? 4 : 3);
            a.fireCooldown = 0.09;
        }
        if (a.burstLeft > 0 && a.fireCooldown <= 0) {
            --a.burstLeft;
            a.fireCooldown = drone ? 0.17 : 0.14;
            if (a.burstLeft == 0) {
                a.burstPause = (std::max(0.8, 1.6 - difficulty_ * 0.06) + rng_.draw("alien.burst") * 0.45) *
                               (suppressing ? 1.6 : 1.0);
            }
            a.radar = 2.6f;
            const Vec3 muzzle = drone ? (a.actor.position + forward(a.actor.yaw, 0) * 0.45f)
                                      : (atHeight(a.actor.position, alienMouth * alienScale(a.variant)) +
                                         forward(a.actor.yaw, 0) * 0.55f); // Spat from the open maw
            Vec3 dir = targetAim - muzzle;
            float spread = std::max(0.05f, 0.13f - difficulty_ * 0.007f);
            if (targetCiv >= 0) {
                spread *= 1.45f;
            } else if (!atCommander) {
                // Fresh sight lines walk their fire in, and a moving target is harder to track.
                const double acquire = 1.4 - 0.06 * difficulty_;
                spread *= 1.f + 1.9f * float(std::clamp(1.0 - a.sightTime / acquire, 0.0, 1.0));
                spread *= 1.f + std::clamp((planar(snap.playerVelocity) - 1.5f) / 4.f, 0.f, 0.6f);
                if (suppressing) spread *= 2.4f;
            }
            dir = normalized(dir);
            dir.x += float(rng_.between("alien.sx", -spread, spread));
            dir.y += float(rng_.between("alien.sy", -spread * 0.8, spread * 0.8));
            dir.z += float(rng_.between("alien.sz", -spread, spread));
            dir = normalized(dir);

            const auto wh = world_.shot(muzzle, dir, snap.brokenGlass);
            float dist = std::min(55.f, wh.distance);
            bool hitPlayer = false;
            bool hitCiv = false;

            if (!snap.dead) {
                const float pHit = cylinder(muzzle, dir, snap.player.position, 0.40f, snap.eyeHeight + 0.15f);
                if (pHit < dist) {
                    dist = pHit;
                    hitPlayer = true;
                }
            }
            if (targetCiv >= 0 && !agents_[targetCiv].dead) {
                const float cHit = cylinder(muzzle, dir, agents_[targetCiv].actor.position, 0.38f, 1.12f);
                if (cHit < dist) {
                    dist = cHit;
                    hitPlayer = false;
                    hitCiv = true;
                }
            }
            if (atCommander) {
                const float cmdHit = cylinder(muzzle, dir, cmd.actor.position, 0.40f, 1.85f);
                if (cmdHit < dist) {
                    dist = cmdHit;
                    hitPlayer = false;
                }
            }

            const Vec3 end = muzzle + dir * dist;
            addTracer(muzzle, end, a.variant > 0 ? crimsonPlasma : cyanPlasma);
            host.playSound(drone ? Sound::DroneFire : Sound::PlasmaFire, 0.78f, true, muzzle);

            float boltDmg = (drone ? 6.5f + difficulty_ * 0.5f : 8.f + difficulty_ * 0.6f) + (a.variant > 0 ? 3.f : 0.f);
            if (suppressing) boltDmg *= 0.5f;
            if (hitPlayer) {
                hurtPlayer(boltDmg, a.actor.position, snap, host);
            } else if (hitCiv) {
                damageAgent(targetCiv, boltDmg * 0.7f, false, false, snap, host);
            } else if (wh.glass >= 0 && wh.distance <= 55.f) {
                host.hitWorld(wh, end);
            }
            if (targetCiv >= 0 && a.sector >= 0 && a.sector < 3 && !sectors_[a.sector].warned) {
                sectors_[a.sector].warned = true;
                setRadio("DELTA-1", family[a.sector].underFire, 3.4f, host, true, RadioUrgent, a.sector);
            }
        }
    }
}

// Exfil: the evac bird drops onto the rear lawn, survivors and Delta-1 run out and board,
// and it lifts away while the view closes like the eyelids that opened the mission.
namespace {
constexpr double exfilArrive = 4.4, exfilClimb = 3.2;
Vec3 exfilPoint(float x, float y, float h) { return {(x - 316.f) * .05f, h, (212.5f - y) * .05f}; }
// The bird settles low over the open rear lawn; the camera watches from just inside the rear
// doors so the survivors run out past it.
const Vec3 exfilHover = exfilPoint(350, -80, 2.4f), exfilApproach = exfilPoint(430, -470, 17.f);
const Vec3 exfilAway = exfilPoint(250, -780, 34.f), exfilEye = exfilPoint(412, 56, 1.6f);
} // namespace

bool MissionController::survivorAlive(int sector) const {
    const auto& civ = agents_[sectors_[sector].civSlot];
    return civ.used && !civ.dead;
}

// One line for the results screen, written from how the morning went.
void MissionController::writeEpilogue() {
    const int clock = 5 * 60 + 47 + int(state_.missionSeconds / 60.f);
    char line[72];
    if (state_.phase == MissionPhase::Failed && state_.civiliansLost >= state_.civiliansTotal) {
        std::snprintf(line, sizeof(line), "NO ONE LEFT BRIAR HOUSE ALIVE.");
    } else if (state_.phase == MissionPhase::Failed) {
        std::snprintf(line, sizeof(line), "BRIAR HOUSE WENT SILENT AT %02d:%02d.", clock / 60, clock % 60);
    } else if (state_.civiliansLost == 0) {
        std::snprintf(line, sizeof(line), "ALL THREE MARSHES MADE IT OUT. THE FLEET DATA REACHED COMMAND.");
    } else if (survivorAlive(nora)) {
        std::snprintf(line, sizeof(line), "DR. MARSH GOT OUT WITH HER DATA. NOT EVERYONE DID.");
    } else {
        std::snprintf(line, sizeof(line), "THE FAMILY GOT OUT. DR. MARSH AND HER DATA DID NOT.");
    }
    copyText(state_.epilogue, sizeof(state_.epilogue), line);
}

void MissionController::beginExfil(MissionHost& host) {
    clearRadioQueue();
    exfil_ = true;
    exfilTime_ = 0;
    exfilCue_ = 0;
    liftAt_ = 10.0;
    state_.cinematic = true;
    state_.evacVisible = true;
    state_.evacPos = exfilApproach;
    const Vec3 heading = exfilHover - exfilApproach;
    state_.evacYaw = std::atan2(heading.x, heading.z);
    state_.hasWaypoint = false;
    setObjective("BRIAR HOUSE SECURED", "EXFIL WITH THE MARSH FAMILY");
    setRadio("DELTA-1", "LZ IS CLEAR - DUSTOFF INBOUND. GET THE FAMILY MOVING!", 3.6f, host, true, RadioUrgent);
    writeEpilogue();
}

void MissionController::updateExfil(Snapshot& snap, const Input& input, double dt, MissionHost& host) {
    const bool first = exfilTime_ == 0;
    exfilTime_ += dt;
    const double t = exfilTime_;

    Vec3 bird, heading;
    if (t < exfilArrive) {
        const float k = float(t / exfilArrive);
        bird = exfilApproach + (exfilHover - exfilApproach) * (1.f - (1.f - k) * (1.f - k));
        heading = exfilHover - exfilApproach;
    } else if (t < liftAt_) {
        bird = exfilHover + Vec3{0, std::sin(float(t) * 1.7f) * .12f, 0};
        heading = {-1, 0, 0}; // Nose west along the lawn, cabin door toward the house
    } else {
        const float k = float(std::clamp((t - liftAt_) / exfilClimb, 0.0, 1.0));
        bird = exfilHover + (exfilAway - exfilHover) * (k * k);
        heading = exfilAway - exfilHover;
    }
    state_.evacPos = bird;
    state_.evacYaw += wrapDelta(state_.evacYaw, std::atan2(heading.x, heading.z)) * float(1.0 - std::exp(-2.0 * dt));
    state_.evacPitch = t < exfilArrive ? .14f * float(1.0 - t / exfilArrive) : t > liftAt_ ? .2f : 0.f;

    // Survivors and Delta-1 run under the bird and climb aboard.
    int waiting = 0;
    for (int c = 1; c <= 3; ++c) {
        auto& civ = agents_[c];
        if (!civ.used || civ.dead || !civ.rescued || civ.boarded) continue;
        ++waiting;
        if (t > exfilArrive - .6) {
            const Vec3 door = exfilHover + Vec3{float(c - 2) * .8f, -exfilHover.y, -1.5f};
            steerAlongPath(civ, door, 4.6f, dt, false);
            if (civ.actor.speed > .3f) {
                civ.actor.yaw += wrapDelta(civ.actor.yaw, std::atan2(civ.velocity.x, civ.velocity.z)) *
                                 float(1.0 - std::exp(-9.0 * dt));
            }
            if (planarSquared(door - civ.actor.position) < .8f) {
                civ.boarded = true;
                civ.actor.visible = false;
            }
        }
        poseCivilian(civ);
    }
    auto& cmd = agents_[0];
    if (cmd.used && !cmd.boarded) {
        ++waiting;
        if (t > exfilArrive + .5) {
            const Vec3 door = exfilHover + Vec3{-1.7f, -exfilHover.y, -1.5f};
            steerAlongPath(cmd, door, 4.6f, dt, false);
            if (cmd.actor.speed > .3f) {
                cmd.actor.yaw += wrapDelta(cmd.actor.yaw, std::atan2(cmd.velocity.x, cmd.velocity.z)) *
                                 float(1.0 - std::exp(-9.0 * dt));
            }
            if (planarSquared(door - cmd.actor.position) < .8f) {
                cmd.boarded = true;
                cmd.actor.visible = false;
            }
        }
        host.updateActorFrame(cmd.actor);
    }
    if (waiting == 0 && liftAt_ > t + 1.0) liftAt_ = t + 1.0;

    if (exfilCue_ == 0 && t > exfilArrive) {
        exfilCue_ = 1;
        setRadio("DELTA-1", "DUSTOFF'S DOWN - GO, GO, GO!", 2.6f, host, true, RadioUrgent);
    } else if (exfilCue_ == 1 && t > liftAt_) {
        exfilCue_ = 2;
        setRadio("DELTA-1", survivorAlive(nora) ? "WHEELS UP. TELL COMMAND WE HAVE DR. MARSH."
                                                : "WHEELS UP. TELL COMMAND... WE LOST THE DOC.",
                 3.2f, host, true, RadioUrgent);
    }

    // Cut to the rear doorway and follow the bird, framing the lawn beneath it once it settles.
    const float settle = float(std::clamp((t - 2.5) / 2.0, 0.0, 1.0)) * (t < liftAt_ ? 1.f : 0.f);
    const Vec3 d = bird - Vec3{0, 1.7f * settle, 0} - exfilEye;
    const float yaw = std::atan2(d.x, d.z);
    const float pitch = std::clamp(-std::atan2(d.y, std::max(.01f, planar(d))), -1.1f, .4f);
    if (first) {
        snap.camera.yaw = yaw;
        snap.camera.pitch = pitch;
    }
    const float follow = float(1.0 - std::exp(-3.0 * dt));
    snap.camera.position = exfilEye;
    snap.camera.yaw += wrapDelta(snap.camera.yaw, yaw) * follow;
    snap.camera.pitch += (pitch - snap.camera.pitch) * follow;
    snap.camera.fov = 66.f * pi / 180.f;
    snap.playerYaw = snap.camera.yaw;
    snap.playerPitch = snap.camera.pitch;

    const double end = liftAt_ + exfilClimb;
    state_.letterbox = float(std::min(.85, t * .85));
    state_.fade = float(std::clamp((t - (end - 1.1)) / 1.1, 0.0, 1.0));
    const bool skip = t > 1.0 && (input.edge(Jump) || input.edge(Confirm));
    if (t >= end || skip) {
        exfil_ = false;
        state_.cinematic = false;
        state_.fade = 1.f;
        state_.phase = MissionPhase::Victory;
        snap.match = Match::Results;
    }
}

// Aliens hold firing spots around the player instead of walking straight in: they keep a good
// range, prefer spots beside cover, and spread out to flank. A badly hurt trooper breaks
// contact once, then comes back from a new angle.
void MissionController::tacticalMove(Agent& a, int slot, const Snapshot& snap, double dt) {
    const bool drone = a.role == MissionRole::AlienDrone;
    const float speed = drone ? 3.8f : 2.9f + difficulty_ * .11f;
    const Vec3 player = snap.player.position;
    if (a.wantsCover && !plannedThisTick_) {
        a.wantsCover = false;
        plannedThisTick_ = true;
        const int node = pickPosition(a, slot, snap, true);
        if (node >= 0) {
            a.tacGoal = nav_.node(node).position;
            a.hasGoal = a.hiding = true;
            a.hideUntil = elapsed_ + 1.6 + rng_.draw("alien.hide") * 1.2;
            a.replanAt = a.hideUntil;
        }
    }
    if (a.hiding && elapsed_ >= a.hideUntil) {
        a.hiding = false;
        a.replanAt = 0;
    }
    const bool stale = planarSquared(player - a.planFrom) > 16.f;
    if (!a.hiding && !plannedThisTick_ && (elapsed_ >= a.replanAt || stale)) {
        plannedThisTick_ = true;
        const int node = pickPosition(a, slot, snap, false);
        a.hasGoal = node >= 0;
        if (a.hasGoal) a.tacGoal = nav_.node(node).position;
        a.planFrom = player;
        a.replanAt = elapsed_ + (a.hasGoal ? 2.4 + rng_.draw("alien.replan") * 1.6 : 1.0);
    }
    if (!a.hasGoal) {
        // No firing spot nearby, usually because the player is deep inside a room: push in.
        steerAlongPath(a, player, speed, dt, drone);
        return;
    }
    Vec3 goal = a.tacGoal;
    if (!a.hiding && planarSquared(goal - a.actor.position) < 1.2f) {
        // At the spot: shift side to side so it is not a static target.
        const Vec3 toPlayer = normalized(Vec3{player.x - goal.x, 0, player.z - goal.z});
        goal += Vec3{toPlayer.z, 0, -toPlayer.x} * (std::sin(float(elapsed_) * 1.3f + float(slot) * 1.7f) * .6f);
    }
    steerAlongPath(a, goal, a.hiding ? speed * 1.15f : speed, dt, drone);
}

int MissionController::pickPosition(const Agent& a, int slot, const Snapshot& snap, bool hide) const {
    const bool drone = a.role == MissionRole::AlienDrone;
    const Vec3 player = snap.player.position;
    const Vec3 playerEye = atHeight(player, snap.eyeHeight);
    const float eyeHeight = drone ? a.hoverBase : alienMouth * alienScale(a.variant);
    const float preferred = drone ? 9.f : a.token ? 7.f : 9.f;
    // Bearings around the player that other aliens already hold.
    std::array<float, MissionSnapshot::maxActors> claimed{};
    int claims = 0;
    for (int i = 4; i < int(agents_.size()); ++i) {
        const auto& o = agents_[i];
        if (i == slot || !o.used || o.dead || !o.alerted) continue;
        const Vec3 at = o.hasGoal ? o.tacGoal : o.actor.position;
        claimed[size_t(claims++)] = std::atan2(at.x - player.x, at.z - player.z);
    }
    // Keep the cheapest candidates, then test their sight lines in order.
    constexpr int keep = 24;
    std::array<float, keep> bestScore{};
    std::array<int, keep> bestNode{};
    int kept = 0;
    for (int n = 0; n < nav_.size(); ++n) {
        if (!nav_.valid(n)) continue;
        const Vec3 p = nav_.node(n).position;
        const float travel = planar(p - a.actor.position);
        if (travel < .8f || travel > (hide ? 7.f : 10.f)) continue;
        const float range = planar(p - player);
        float score;
        if (hide) {
            // Nodes beside obstacles are where a sight line can actually be broken.
            score = travel + (range < 4.f ? 6.f : 0.f) - std::min(range, 10.f) * .2f - (nav_.node(n).cover ? 3.f : 0.f);
        } else {
            if (range < 3.5f) continue;
            const float bearing = std::atan2(p.x - player.x, p.z - player.z);
            float spread = pi;
            for (int k = 0; k < claims; ++k) spread = std::min(spread, std::abs(wrapDelta(bearing, claimed[size_t(k)])));
            score = std::abs(range - preferred) * .6f + (1.f - spread / pi) * (a.token ? 3.f : 5.f) + travel * .25f -
                    (nav_.node(n).cover ? 1.2f : 0.f);
        }
        int at = kept < keep ? kept++ : keep;
        if (at == keep) {
            if (score >= bestScore[keep - 1]) continue;
            at = keep - 1;
        }
        while (at > 0 && bestScore[size_t(at - 1)] > score) {
            bestScore[size_t(at)] = bestScore[size_t(at - 1)];
            bestNode[size_t(at)] = bestNode[size_t(at - 1)];
            --at;
        }
        bestScore[size_t(at)] = score;
        bestNode[size_t(at)] = n;
    }
    for (int k = 0; k < kept; ++k) {
        const Vec3 eye = atHeight(nav_.node(bestNode[size_t(k)]).position, eyeHeight);
        const bool sees = !world_.blocked(eye, playerEye);
        if (hide ? !sees : sees) return bestNode[size_t(k)];
    }
    return -1;
}

void MissionController::roar(Vec3 at, MissionHost& host) {
    if (elapsed_ < roarReadyAt_) return;
    roarReadyAt_ = elapsed_ + 1.4;
    host.playSound(Sound::AlienRoar, 0.95f, true, atHeight(at, alienMouth));
}

// Alerted aliens still out on the road run to the front wall and vault it in one bound.
// Returns true while the vault owns the alien's movement.
bool MissionController::vaultWall(Agent& a, double dt, MissionHost& host) {
    const bool drone = a.role == MissionRole::AlienDrone;
    const float base = drone ? a.hoverBase : 0.f;
    if (a.leapAt >= 0) {
        a.leapAt += dt;
        const float t = float(std::clamp(a.leapAt / leapDuration, 0.0, 1.0));
        const Vec3 at = a.leapFrom + (a.leapTo - a.leapFrom) * t;
        a.actor.position = {at.x, base + std::sin(t * pi) * (drone ? .9f : 1.8f), at.z};
        a.actor.speed = 4.f;
        a.actor.phase = std::fmod(a.actor.phase + float(dt) * 1.4f, 1.f);
        if (t >= 1.f) {
            a.leapAt = -1;
            a.actor.position.y = base;
            a.path.clear();
            a.repathAt = 0;
        }
        return true;
    }
    if (!a.alerted || a.actor.position.z >= frontWallZ - .2f) return false;
    const Vec3 takeoff{std::clamp(a.actor.position.x, -9.5f, 14.5f), 0, frontWallZ - 1.1f};
    Vec3 to{takeoff.x - a.actor.position.x, 0, takeoff.z - a.actor.position.z};
    const float dist = planar(to);
    if (dist < .35f) {
        a.leapAt = 0;
        a.leapFrom = {a.actor.position.x, 0, a.actor.position.z};
        a.leapTo = {a.actor.position.x, 0, frontWallZ + 1.4f};
        if (!drone) roar(a.actor.position, host);
        if (!wallCalled_) {
            wallCalled_ = true;
            setRadio("DELTA-1", "THEY'RE COMING OVER THE WALL!", 2.2f, host, true, RadioChatter);
        }
        return true;
    }
    const float speed = drone ? 3.8f : 2.9f + difficulty_ * .11f;
    a.velocity = to * (std::min(speed, dist / float(dt)) / dist);
    a.actor.position = {a.actor.position.x + a.velocity.x * float(dt), base, a.actor.position.z + a.velocity.z * float(dt)};
    a.actor.speed = speed;
    a.actor.phase = std::fmod(a.actor.phase + speed * float(dt) / (4.f * .62f), 1.f);
    return true;
}

void MissionController::damageAgent(int slot, float amount, bool headshot, bool fromPlayer,
                                    Snapshot& snap, MissionHost& host) {
    auto& a = agents_[slot];
    if (!a.used || a.dead) return;

    if (a.role == MissionRole::Commander) {
        if (fromPlayer) {
            state_.friendlyFireWarn = true;
            state_.friendlyFireTimer = 2.0f;
            setRadio("DELTA-1", "CHECK YOUR FIRE! I'M ON YOUR SIX!", 2.4f, host, true, RadioUrgent);
        }
        return;
    }

    if (a.role == MissionRole::Civilian) {
        if (fromPlayer) {
            // One stray burst wounds but cannot kill: after a hit the player has a moment to
            // see the warning and lift off before the next round lands.
            if (elapsed_ < a.flinchUntil) return;
            a.flinchUntil = elapsed_ + 0.9;
            amount = std::min(amount, 30.f);
            state_.friendlyFireWarn = true;
            state_.friendlyFireTimer = 2.5f;
            setRadio("DELTA-1", "HOLD FIRE! THAT'S A CIVILIAN!", 2.6f, host, true, RadioUrgent);
        }
        a.actor.health -= amount;
        if (a.actor.health <= 0) {
            a.actor.health = 0;
            a.dead = true;
            a.deathElapsed = 0;
            ++state_.civiliansLost;
            if (a.sector >= 0 && a.sector < 3) sectors_[a.sector].resolved = true;
            host.playSound(Sound::Death, 0.9f, true, a.actor.position);
            setRadio("DELTA-1", a.sector >= 0 && a.sector < 3 ? family[a.sector].lost : "WE LOST A CIVILIAN!", 3.5f, host,
                     true, RadioUrgent);
            if (state_.civiliansLost >= state_.civiliansTotal) {
                state_.phase = MissionPhase::Failed;
                copyText(state_.failReason, sizeof(state_.failReason), "THE MARSH FAMILY WAS LOST");
                writeEpilogue();
                failDelay_ = 1.8;
            }
        }
        return;
    }

    a.alerted = true;
    a.radar = 3.0f;
    if (a.sector >= 0 && a.sector < 3) {
        sectors_[a.sector].triggered = true;
        for (int i = 4; i < int(agents_.size()); ++i) {
            if (agents_[i].used && !agents_[i].dead && agents_[i].sector == a.sector) agents_[i].alerted = true;
        }
    }
    a.actor.health -= amount;
    if (a.role == MissionRole::AlienTrooper && !a.hidOnce && a.actor.health > 0 && a.actor.health < a.maxHealth * .45f) {
        a.hidOnce = true;
        a.wantsCover = true;
    }
    if (fromPlayer) {
        snap.lethalMarker = a.actor.health <= 0;
        snap.hitMarker = snap.lethalMarker ? 0.33f : 0.15f;
        snap.headshot = headshot;
        host.playSound(Sound::Hit, 0.65f, false, {});
    }
    if (a.actor.health <= 0) {
        a.actor.health = 0;
        a.dead = true;
        a.deathElapsed = 0;
        a.actor.speed = 0;
        a.token = false;
        ++state_.aliensKilled;
        state_.aliensRemaining = livingAliens();
        if (fromPlayer) {
            snap.killConfirm = 1.25f;
            if (headshot) ++state_.headshots;
            host.playSound(headshot ? Sound::HeadshotConfirm : Sound::KillConfirm, 0.9f, false, {});
        }
        host.playSound(Sound::AlienDown, 0.72f, true, a.actor.position);
    }
}

bool MissionController::resolvePlayerShot(Vec3 origin, Vec3 dir, float damage, float worldDist, Snapshot& snap,
                                          MissionHost& host, Vec3& hitPointOut) {
    float best = worldDist;
    int victim = -1;
    float zone = 1.f;
    bool headshot = false;

    for (int i = 0; i < int(agents_.size()); ++i) {
        const auto& a = agents_[i];
        if (!a.used || a.dead) continue;
        float hit = infinity;
        float hitZone = 1.f;
        bool isHead = false;
        if (a.role == MissionRole::AlienDrone) {
            hit = rayHitBox(origin, dir, a.actor.position, {0.58f, 0.28f, 0.58f}, best);
            if (hit < best) {
                const Vec3 p = origin + dir * hit;
                isHead = length(p - a.actor.position) < 0.26f;
                hitZone = isHead ? 2.1f : 1.25f;
            }
        } else if (a.role == MissionRole::AlienTrooper) {
            const float scale = alienScale(a.variant);
            hit = cylinder(origin, dir, a.actor.position, alienRadius * scale, alienHeight * scale);
            if (hit < best) {
                const float h = (origin.y + dir.y * hit) - a.actor.position.y;
                isHead = h > alienHeadLine * scale;
                hitZone = isHead ? 2.2f : h > 1.1f * scale ? 1.4f : 1.0f;
            }
        } else {
            // Pinned civilians cower about 1.1 m tall; the hitbox follows what is drawn.
            const bool cowering = a.role == MissionRole::Civilian && !a.rescued;
            hit = cylinder(origin, dir, a.actor.position, 0.36f, cowering ? 1.12f : 1.92f);
            if (hit < best) {
                const float h = (origin.y + dir.y * hit) - a.actor.position.y;
                isHead = h > (cowering ? .82f : 1.52f);
                hitZone = isHead ? 2.2f : h > (cowering ? .5f : .9f) ? 1.4f : 1.0f;
            }
        }
        if (hit < best) {
            best = hit;
            victim = i;
            zone = hitZone;
            headshot = isHead;
        }
    }

    if (victim < 0) return false;
    hitPointOut = origin + dir * best;
    addTracer(origin + forward(snap.camera.yaw, snap.camera.pitch) * 0.5f, hitPointOut, amberTracer);
    damageAgent(victim, damage * zone, headshot, true, snap, host);
    return true;
}

int MissionController::nearestAlienForApache(Vec3 gunPos) const {
    int best = -1;
    float bestScore = infinity;
    for (int i = 4; i < int(agents_.size()); ++i) {
        const auto& a = agents_[i];
        if (!a.used || a.dead) continue;
        const Vec3 aim = a.role == MissionRole::AlienDrone ? a.actor.position : atHeight(a.actor.position, alienChest);
        const float score = length(aim - gunPos) * (world_.blocked(gunPos, aim) ? 2.5f : 1.f);
        if (score < bestScore) {
            bestScore = score;
            best = i;
        }
    }
    return best;
}

bool MissionController::apacheShotAlien(int index, Vec3 gunPos, Vec3 dir, float worldDist, float damage,
                                        Snapshot& snap, MissionHost& host, Vec3& hitPointOut) {
    if (index < 4 || index >= int(agents_.size())) return false;
    const auto& a = agents_[index];
    if (!a.used || a.dead) return false;
    const float hit = a.role == MissionRole::AlienDrone
        ? rayHitBox(gunPos, dir, a.actor.position, {0.65f, 0.35f, 0.65f}, worldDist)
        : cylinder(gunPos, dir, a.actor.position, alienRadius + .08f, alienHeight * alienScale(a.variant));
    const bool struck = hit < worldDist;
    hitPointOut = gunPos + dir * (struck ? hit : std::min(80.f, worldDist));
    addTracer(gunPos, hitPointOut, amberTracer);
    if (struck) damageAgent(index, damage, false, false, snap, host);
    return struck;
}

void MissionController::explodeAliens(Vec3 center, float radius, Snapshot& snap, MissionHost& host) {
    const Vec3 eye = atHeight(center, 1.1f);
    for (int i = 4; i < int(agents_.size()); ++i) {
        const auto& a = agents_[i];
        if (!a.used || a.dead) continue;
        const float d = planar(a.actor.position - center);
        const Vec3 aim = a.role == MissionRole::AlienDrone ? a.actor.position : atHeight(a.actor.position, alienChest);
        if (d <= radius && !world_.blocked(eye, aim)) {
            damageAgent(i, 175.f * (1.f - d / (radius + 1.2f)), false, true, snap, host);
        }
    }
}

int MissionController::livingAliens(int sectorFilter) const {
    int count = 0;
    for (int i = 4; i < int(agents_.size()); ++i) {
        if (!agents_[i].used || agents_[i].dead) continue;
        if (sectorFilter == -2 || agents_[i].sector == sectorFilter) ++count;
    }
    return count;
}

void MissionController::updateWaypoint(const Snapshot& snap) {
    state_.hasWaypoint = false;
    if (state_.phase == MissionPhase::Overwatch || state_.phase == MissionPhase::Breach ||
        state_.phase == MissionPhase::Extraction) {
        float best = infinity;
        for (int i = 4; i < int(agents_.size()); ++i) {
            if (!agents_[i].used || agents_[i].dead) continue;
            const float d = planarSquared(agents_[i].actor.position - snap.player.position);
            if (d < best) {
                best = d;
                state_.waypoint = agents_[i].actor.position;
                state_.hasWaypoint = true;
            }
        }
        return;
    }
    if (state_.phase == MissionPhase::Incoming) {
        state_.waypoint = mapPoint(patio[0], patio[1]);
        state_.hasWaypoint = true;
        return;
    }
    if (state_.phase == MissionPhase::Rescue || state_.phase == MissionPhase::Regroup) {
        float best = infinity;
        for (int s = 0; s < 3; ++s) {
            if (sectors_[s].resolved) continue;
            const float d = planarSquared(sectors_[s].center - snap.player.position);
            if (d < best) {
                best = d;
                state_.waypoint = sectors_[s].center;
                state_.hasWaypoint = true;
            }
        }
    }
}

void MissionController::enterIncoming(MissionHost& host) {
    clearRadioQueue();
    state_.phase = MissionPhase::Incoming;
    phaseTime_ = 0;
    incomingArrived_ = false;
    saveCheckpoint(3);
    setObjective("FALL BACK TO THE REAR PATIO", "CHECKPOINT SAVED / REGROUP WITH DELTA-1");
    setRadio("DELTA-1", "THE FAMILY'S AT THE BACK DOOR. ON ME AT THE PATIO - SOMETHING'S COMING.", 4.4f,
             host, true, RadioOrder);
}

void MissionController::enterExtraction(Snapshot& snap, MissionHost& host) {
    state_.phase = MissionPhase::Extraction;
    phaseTime_ = 0;
    evacDuration_ = evacTimer_ = 52.0 + 1.8 * difficulty_;
    state_.evacTotal = float(evacDuration_);
    state_.evacSeconds = float(evacTimer_);
    nextReinforceAt_ = 6.0;
    reinforcements_ = 0;
    evacBeat_ = 0;
    lzCallSent_ = false;
    spawnExtractionWave(snap, host);
    snap.apacheReady = true;
    setObjective("HOLD THE REAR PATIO", "");
}

void MissionController::checkPhaseTransitions(Snapshot& snap, double dt, MissionHost& host) {
    if (state_.phase == MissionPhase::Breach) {
        phaseTime_ += dt;
        // Once the lawn push thins out, a second team hits the rear doors.
        if (pendingFlankers_ > 0 && (phaseTime_ > 7.0 || livingAliens() <= 1)) {
            static constexpr float flank[2][2]{{120, -70}, {560, -70}};
            while (pendingFlankers_ > 0) {
                const auto* at = flank[pendingFlankers_ - 1];
                if (dropAlien(MissionRole::AlienTrooper, mapPoint(at[0], at[1]), 0, snap, host) < 0) break;
                --pendingFlankers_;
                if (!flankersCalled_) {
                    flankersCalled_ = true;
                    setRadio("DELTA-1", "MOVEMENT AT THE BACK DOOR - THEY'RE FLANKING US!", 3.4f, host, true, RadioUrgent);
                }
            }
        }
        state_.aliensRemaining = livingAliens();
        if (state_.aliensRemaining == 0 && pendingFlankers_ == 0) {
            clearRadioQueue();
            state_.phase = MissionPhase::Regroup;
            phaseTime_ = 0;
            regroupBriefSent_ = false;
            snap.player.health = 100.f;
            host.refillPlayer(false);
            saveCheckpoint(2);
            setObjective("BREACH CLEAR - REGROUP", "CHECKPOINT SAVED / AMMO RESUPPLIED");
            setRadio("DELTA-1", "CLEAR... CATCH YOUR BREATH. CHECK YOUR MAG.", 2.4f, host, true, RadioUrgent);
        }
        return;
    }

    if (state_.phase == MissionPhase::Regroup) {
        phaseTime_ += dt;
        if (!regroupBriefSent_ && phaseTime_ >= 2.0) {
            regroupBriefSent_ = true;
            setRadio("DELTA-1", "THE MARSHES ARE PINNED IN THE KITCHEN, GARAGE AND SHED. WE TAKE ALL THREE.", 4.4f, host, true);
        }
        if (phaseTime_ >= regroupDuration) {
            state_.phase = MissionPhase::Rescue;
            phaseTime_ = 0;
            setObjective("RESCUE THE MARSHES 0/3", "FIND BEN, DR. MARSH AND WALT");
        }
        return;
    }

    if (state_.phase == MissionPhase::Rescue) {
        const Vec3 p = snap.player.position;
        const int activeCount = livingAliens();
        if (!shedSniperHintSent_ && sectors_[0].resolved && !sectors_[2].resolved && p.x < -3.0f) {
            shedSniperHintSent_ = true;
            if (!sectors_[2].spawned && activeCount <= 2) {
                spawnSectorWave(2, difficulty_);
            }
            setRadio("DELTA-1", "EYES ON THE WEST WINDOWS - CLEAN SNIPER ANGLE ON THE GARDEN SHED.", 4.4f, host, true,
                     RadioOrder, 2);
        }

        const bool nearSector[3]{
            p.x < -3.0f || planarSquared(p - sectors_[0].center) < 7.5f * 7.5f,
            p.x > 8.2f || planarSquared(p - sectors_[1].center) < 8.0f * 8.0f,
            p.x < -12.8f || planarSquared(p - sectors_[2].center) < 8.2f * 8.2f,
        };
        const char* sectorCallouts[3]{family[0].callout, family[1].callout, family[2].callout};
        for (int s = 0; s < 3; ++s) {
            auto& sec = sectors_[s];
            if (sec.resolved) continue;
            if (!sec.spawned && nearSector[s] && activeCount <= 2) {
                sec.triggered = true;
                spawnSectorWave(s, difficulty_);
                setRadio("DELTA-1", sectorCallouts[s], 4.0f, host, true, RadioOrder, s);
            } else if (sec.spawned && !sec.triggered && nearSector[s]) {
                sec.triggered = true;
                setRadio("DELTA-1", sectorCallouts[s], 4.0f, host, true, RadioOrder, s);
            }
            if (sec.spawned && livingAliens(s) == 0) {
                sec.clearTimer += dt;
                auto& civ = agents_[sec.civSlot];
                const bool close = planarSquared(p - civ.actor.position) < 81.f;
                if (!civ.dead && !civ.rescued && (close || sec.clearTimer > 1.2)) {
                    civ.rescued = true;
                    sec.resolved = true;
                    ++state_.civiliansSaved;
                    host.playSound(Sound::CivilianSaved, 0.85f, false, {});
                    char obj[64];
                    std::snprintf(obj, sizeof(obj), "RESCUE THE MARSHES %d/%d", state_.civiliansSaved, state_.civiliansTotal);
                    setObjective(obj, state_.civiliansSaved + state_.civiliansLost >= 3
                        ? "ALL ROOMS SWEPT" : "MOVE SLOW - SWEEP THE REMAINING ROOMS");
                    // The survivor speaks for themselves, in person, so there is no radio squelch.
                    const bool noraPinned = s != nora && survivorAlive(nora) && !agents_[sectors_[nora].civSlot].rescued;
                    setRadio(family[s].name, noraPinned ? family[s].worried : family[s].rescued, 3.6f, host, false);
                } else if (civ.dead) {
                    sec.resolved = true;
                }
            }
        }
        state_.aliensRemaining = livingAliens();

        const bool allResolved = sectors_[0].resolved && sectors_[1].resolved && sectors_[2].resolved;
        if (allResolved && state_.civiliansSaved > 0) enterIncoming(host);
        return;
    }

    if (state_.phase == MissionPhase::Incoming) {
        phaseTime_ += dt;
        const bool atPatio = planarSquared(snap.player.position - mapPoint(patio[0], patio[1])) < 6.5f * 6.5f;
        if (!incomingArrived_ && (atPatio || phaseTime_ >= incomingTimeout)) {
            incomingArrived_ = true;
            incomingArrivedAt_ = phaseTime_;
            clearRadioQueue();
            host.playSound(Sound::AlienHorn, 0.88f, false, {});
            setRadio("DELTA-1", "HEAR THAT? THEY'RE DROPPING ON THE REAR LAWN! APACHE IS CLEARED HOT!", 4.2f, host,
                     true, RadioUrgent);
        }
        if (incomingArrived_ && phaseTime_ - incomingArrivedAt_ >= incomingLeadIn) enterExtraction(snap, host);
        return;
    }

    if (state_.phase == MissionPhase::Extraction) {
        phaseTime_ += dt;
        if (evacTimer_ > 0) {
            evacTimer_ = std::max(0.0, evacTimer_ - dt);
            if (evacTimer_ > 8.0 && phaseTime_ >= nextReinforceAt_) {
                nextReinforceAt_ = spawnReinforcement(snap, host)
                    ? phaseTime_ + std::max(2.8, 6.2 - 0.36 * difficulty_) : phaseTime_ + 0.5;
            }
            if (evacBeat_ == 0 && evacTimer_ <= evacDuration_ * 0.8) {
                evacBeat_ = 1;
                if (survivorAlive(nora)) {
                    setRadio(family[nora].name, "THEY'RE HOMING ON MY ARRAY - KEEP THEM OFF THE HOUSE!", 3.4f, host, false,
                             RadioChatter);
                }
            } else if (evacBeat_ == 1 && evacTimer_ <= evacDuration_ * 0.55) {
                evacBeat_ = 2;
                setRadio("DELTA-1", "KEEP THEM OFF THE PATIO! DUSTOFF IS ON THE WAY!", 3.4f, host, true, RadioChatter);
            } else if (evacBeat_ == 2 && evacTimer_ <= 20.0) {
                evacBeat_ = 3;
                setRadio("DELTA-1", "TWENTY SECONDS! HOLD THIS LINE, DELTA!", 3.0f, host, true);
            }
            const int secs = int(std::ceil(evacTimer_));
            char sub[64];
            std::snprintf(sub, sizeof(sub), snap.apacheReady ? "EVAC IN %d:%02d  /  UP: CALL APACHE" : "EVAC IN %d:%02d",
                          secs / 60, secs % 60);
            copyText(state_.subObjective, sizeof(state_.subObjective), sub);
        }
        state_.evacSeconds = float(evacTimer_);
        state_.aliensRemaining = livingAliens();
        if (evacTimer_ > 0) return;
        if (state_.aliensRemaining > 0) {
            if (!lzCallSent_) {
                lzCallSent_ = true;
                setRadio("DELTA-1", "DUSTOFF ON STATION! CLEAR THE LZ - FINISH THEM!", 3.4f, host, true, RadioUrgent);
            }
            char sub[64];
            std::snprintf(sub, sizeof(sub), "%d %s ON THE LZ", state_.aliensRemaining,
                          state_.aliensRemaining == 1 ? "HOSTILE" : "HOSTILES");
            setObjective("CLEAR THE LZ", sub);
            return;
        }
        if (!victoryAnnounced_) {
            victoryAnnounced_ = true;
            beginExfil(host);
        }
    }
}

void MissionController::step(Snapshot& snap, const Input& input, double dt, bool playerFired,
                             MissionHost& host) {
    if (!state_.active) return;
    elapsed_ += dt;
    rng_.tick = snap.tick;
    // The pause menu can change difficulty mid-mission; apply it to the aliens too.
    difficulty_ = std::clamp(snap.difficulty, 1, 10);

    for (auto& tr : state_.tracers) tr.time = std::max(0.f, tr.time - float(dt * 16.0));
    state_.radioTimer = std::max(0.f, state_.radioTimer - float(dt));
    state_.friendlyFireTimer = std::max(0.f, state_.friendlyFireTimer - float(dt));
    if (state_.friendlyFireTimer <= 0) state_.friendlyFireWarn = false;

    if (state_.phase == MissionPhase::Failed) {
        failDelay_ -= dt;
        if (failDelay_ <= 0) snap.match = Match::Results;
        return;
    }
    if (state_.phase == MissionPhase::Victory) return;
    if (exfil_) {
        // The clock stopped when the LZ went quiet; the cutscene plays on its own time.
        updateExfil(snap, input, dt, host);
        pumpRadio(host);
        return;
    }

    state_.missionSeconds += float(dt);

    if (snap.dead) {
        clearRadioQueue();
        state_.phase = MissionPhase::Failed;
        copyText(state_.failReason, sizeof(state_.failReason), "DELTA OPERATIVE KIA");
        writeEpilogue();
        failDelay_ = 2.0;
        setRadio("DELTA-1", "MAN DOWN! DELTA OPERATIVE IS DOWN!", 3.0f, host, true, RadioUrgent);
        return;
    }

    if (state_.phase == MissionPhase::Intro) {
        updateIntro(snap, input, dt, host);
        updateCommander(snap, dt, host);
        for (int c = 1; c <= 3; ++c) updateCivilian(c, snap, dt);
        pumpRadio(host);
        return;
    }

    if (state_.phase == MissionPhase::Overwatch) {
        updateOverwatch(snap, dt, playerFired, host);
    }

    plannedThisTick_ = false;
    assignAttackTokens(snap);
    updateCommander(snap, dt, host);
    for (int c = 1; c <= 3; ++c) updateCivilian(c, snap, dt);
    for (int i = 4; i < int(agents_.size()); ++i) updateAlien(i, snap, dt, playerFired, host);
    checkPhaseTransitions(snap, dt, host);
    pumpRadio(host);
    updateWaypoint(snap);
}

void MissionController::publish(std::shared_ptr<const MissionSnapshot>& target) {
    // Reuse the existing published buffer in-place (zero per-tick heap allocations) unless
    // external callers still hold an older Snapshot copy (copy-on-write safety).
    const long expectedRefs = (target && target.get() == published_.get()) ? 2 : 1;
    if (!published_ || published_.use_count() > expectedRefs) {
        published_ = std::make_shared<MissionSnapshot>();
    }
    *published_ = state_;
    for (size_t i = 0; i < agents_.size(); ++i) {
        const auto& a = agents_[i];
        published_->actors[i] = {
            a.actor, a.role, a.variant, a.used && a.actor.visible,
            a.dead, a.rescued, a.alerted, a.radar
        };
    }
    target = published_;
}

} // namespace fod::mission_detail

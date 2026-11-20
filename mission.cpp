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
constexpr uint16_t overchargeOrange = rgb565(255, 116, 62);
constexpr uint16_t shieldCyan = rgb565(96, 236, 255);
constexpr uint16_t calloutGold = rgb565(234, 186, 78);

// Tactical squad roles assigned dynamically to active aliens.
constexpr uint8_t rolePatrol = 0, roleSuppressor = 1, roleFlanker = 2, roleVanguard = 3, roleSupport = 4;

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

// The front garden wall runs along map y = 515. In the opening Overwatch/Breach beat, aliens
// flanking the closed iron railing sections vault over into the yard while center-road units
// push straight through the breached portico gap.
constexpr float frontWallZ = (212.5f - 515.f) * .05f;
constexpr float breachGapLeftX = (304.f - 316.f) * .05f;
constexpr float breachGapRightX = (432.f - 316.f) * .05f;
constexpr double leapDuration = .85;

// Rescued survivors shelter behind the stone walls bordering the flare-lit North Town Clearing LZ.
constexpr float evacSpots[3][2]{{175, -180}, {210, -172}, {245, -180}};
constexpr float townLz[2]{210, -220};
constexpr float commanderLz[2]{152, -195};
// The Marsh family scattered into three surrounding Colorado mountain town districts when the
// alien fleet struck Briar House. Delta Squad breaks out of Briar House to sweep the valley town.
struct Survivor {
    const char* name;      // Subtitle speaker
    const char* callout;   // Delta-1 as you reach the district
    const char* underFire; // Delta-1 when the aliens turn on them
    const char* rescued;   // Their own words once the block is clear
    const char* worried;   // Said instead while Nora is still pinned
    const char* lost;
};
constexpr Survivor family[3]{
    {"BEN MARSH", "EAST RIDGE AVENUE - BEN'S PINNED BY THE PICKUP! WATCH YOUR FIRE.",
     "THEY'RE FIRING ON BEN - PUSH UP EAST RIDGE!", "WE RAN WHEN THE SKY LIT UP... THANK YOU. THANK YOU.",
     "WE RAN WHEN THE SKY LIT UP... IS MY MOM OKAY? SHE HEADED UP NORTH AVENUE!", "WE LOST BEN! LOCK IT DOWN, DELTA!"},
    {"DR. NORA MARSH", "NORTH VALLEY AVENUE - DR. MARSH BY THE STONE WALL. HEAVY ENFORCER!",
     "THEY'RE FIRING ON DR. MARSH - MOVE!", "MY ARRAY AT BRIAR HOUSE LOCKED ONTO THEM - GET MY FAMILY TO THE NORTH CLEARING!",
     "MY ARRAY AT BRIAR HOUSE LOCKED ONTO THEM - GET MY FAMILY TO THE NORTH CLEARING!", "DR. MARSH IS DOWN! WE LOST THE PACKAGE!"},
    {"WALT MARSH", "WEST TIMBERLINE LANE - WALT'S HOLED UP BY THE CEDAR CABIN! CLEAR THE STREET!",
     "THEY'RE HITTING WEST TIMBERLINE - GET EYES ON WALT!", "TOOK YOU LONG ENOUGH, SON. HELL OF A SUNSET FOR AN INVASION.",
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

// World position of the alien's held ray gun emitter tip (matches render.cpp gun socket).
Vec3 MissionController::rayGunMuzzle(const Agent& a) {
    if (a.role == MissionRole::AlienDrone) {
        return a.actor.position + forward(a.actor.yaw, a.gunPitch) * 0.52f;
    }
    const float s = alienScale(a.variant);
    const Vec3 bodyFwd = forward(a.actor.yaw, 0.f);
    const Vec3 gunFwd = forward(a.actor.yaw, a.gunPitch);
    const Vec3 right{std::cos(a.actor.yaw), 0.f, -std::sin(a.actor.yaw)};
    const Vec3 grip = atHeight(a.actor.position, 1.54f * s) + right * (0.34f * s) + bodyFwd * (0.62f * s);
    return grip + gunFwd * ((a.variant > 0 ? 1.02f : 0.88f) * s);
}

MissionController::MissionController(const World& world, Navigation& nav)
    : world_(world), nav_(nav) {}

void MissionController::resetAll() {
    state_ = {};
    state_.mistDensity = 0.50f;
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
    overchargeWarned_ = false;
    droneLinkWarned_ = false;
    interruptPraised_ = false;
    mistRemarked_ = false;
    resupplyReadyAt_ = 0;
    roarReadyAt_ = 0;
    exfil_ = false;
    exfilCue_ = 0;
    exfilTime_ = 0;
    liftAt_ = 0;
    evacBeat_ = 0;
    reinforcements_ = 0;
    streetPatrols_ = 0;
    nextPatrolAt_ = 0;
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

void MissionController::setTacticalCallout(const char* text, uint16_t color, float duration) {
    copyText(state_.tacticalCallout, sizeof(state_.tacticalCallout), text);
    state_.tacticalCalloutColor = color;
    state_.tacticalCalloutTimer = duration;
}

void MissionController::addTracer(Vec3 from, Vec3 to, uint16_t color, TracerStyle style, float life) {
    size_t chosen = 0;
    for (size_t i = 0; i < state_.tracers.size(); ++i) {
        if (state_.tracers[i].time <= 0) { chosen = i; break; }
        if (state_.tracers[i].time < state_.tracers[chosen].time) chosen = i;
    }
    state_.tracers[chosen] = {from, to, life, color, style};
}

void MissionController::emitScannerBeam(const Agent& a) {
    const bool drone = a.role == MissionRole::AlienDrone;
    const Vec3 eye = drone ? a.actor.position : rayGunMuzzle(a);
    const float sweepYaw = a.actor.yaw + std::sin(float(elapsed_) * (drone ? 1.15f : 0.85f) + a.hoverPhase) * 0.45f;
    const float pitch = drone ? 0.36f : 0.14f;
    const Vec3 dir = forward(sweepYaw, pitch);
    // OBB world_.ray is ~18x faster than full shot-triangle mesh intersection.
    const float dist = std::min(14.f, world_.ray(eye, dir, 14.f));
    if (dist > 0.6f) {
        addTracer(eye + dir * 0.15f, eye + dir * dist, a.variant > 0 ? crimsonPlasma : scanCyan, TracerStyle::Scanner, 0.32f);
    }
}

void MissionController::updateMistDensity(double dt) {
    // Dusk mist steadily descends on the Colorado valley as the mission advances,
    // staying clear enough for long sniper lines in Overwatch and rolling thickest during Extraction.
    float target = 0.56f;
    switch (state_.phase) {
    case MissionPhase::Intro:      target = 0.48f + 0.08f * state_.introProgress; break;
    case MissionPhase::Overwatch:  target = 0.56f; break;
    case MissionPhase::Breach:     target = 0.65f; break;
    case MissionPhase::Regroup:    target = 0.70f; break;
    case MissionPhase::Rescue:     target = 0.72f + 0.04f * float(state_.civiliansSaved + state_.civiliansLost); break;
    case MissionPhase::Incoming:   target = 0.84f; break;
    case MissionPhase::Extraction: target = 0.86f + 0.03f * float(state_.waveIntensity); break;
    default:                       target = 0.88f; break;
    }
    state_.mistDensity += (target - state_.mistDensity) * float(1.0 - std::exp(-0.45 * dt));
}

// Alerted Scout Drones project an energy shield-link to the nearest ground alien within 12.5m,
// rewarding players who spot the glowing tether through the mist and drop the drone first.
void MissionController::updateDroneLinks(double dt) {
    for (int i = 4; i < int(agents_.size()); ++i) {
        agents_[i].linked = false;
        if (agents_[i].role == MissionRole::AlienDrone) agents_[i].linkTarget = -1;
    }
    for (int i = 4; i < int(agents_.size()); ++i) {
        auto& drone = agents_[i];
        if (!drone.used || drone.dead || !drone.alerted || drone.spawnFx > 0 || drone.role != MissionRole::AlienDrone) continue;
        int best = -1;
        float bestDistSq = 12.5f * 12.5f;
        for (int j = 4; j < int(agents_.size()); ++j) {
            const auto& mate = agents_[j];
            if (!mate.used || mate.dead || !mate.alerted || mate.role != MissionRole::AlienTrooper) continue;
            const float d2 = planarSquared(mate.actor.position - drone.actor.position);
            if (d2 < bestDistSq && !world_.blocked(drone.actor.position, atHeight(mate.actor.position, 1.4f))) {
                bestDistSq = d2;
                best = j;
            }
        }
        if (best >= 0) {
            drone.linkTarget = int8_t(best);
            auto& mate = agents_[best];
            mate.linked = true;
            const float linkCap = mate.variant > 0 ? mate.maxShield : 26.f + difficulty_ * 2.f;
            mate.maxShield = std::max(mate.maxShield, linkCap);
            if (elapsed_ >= mate.shieldRegenAt) {
                mate.shield = std::min(mate.maxShield, mate.shield + float(18.0 * dt));
            }
            if ((int(elapsed_ * 60.0) & 3) == 0) {
                addTracer(drone.actor.position + Vec3{0, -0.18f, 0},
                          atHeight(mate.actor.position, 1.35f * alienScale(mate.variant)),
                          shieldCyan, TracerStyle::ShieldLink, 0.45f);
            }
        }
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
    // Enforcers deploy with a crimson phase-shield that regenerates if left unpressured.
    if (role == MissionRole::AlienTrooper && variant > 0) {
        a.maxShield = 34.f + difficulty * 4.f;
        a.shield = a.maxShield;
        a.overchargeCooldown = 3.2 + rng_.draw("enforcer.oc") * 1.8;
    }
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
    // Fifteen to eighteen metres out on South Boulevard beyond the breached front gate,
    // picking through the burning Warhorse 2-1 wreck with a Scout Drone sweeping overhead.
    spawnAlien(MissionRole::AlienTrooper, -1, mapPoint(460, 668), -0.2f, 0, difficulty);
    spawnAlien(MissionRole::AlienTrooper, -1, mapPoint(272, 672), 0.35f, 0, difficulty);
    spawnAlien(MissionRole::AlienDrone, -1, mapPoint(378, 638, 3.4f), 0.f, 0, difficulty);
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
        // East Ridge Avenue (Ben Marsh): ray troopers holding the eastern street + Shield-Linking Drone.
        spawnAlien(MissionRole::AlienTrooper, 0, mapPoint(975, 305), pi, 0, difficulty);
        spawnAlien(MissionRole::AlienTrooper, 0, mapPoint(845, 555), 0.4f, 0, difficulty);
        spawnAlien(MissionRole::AlienDrone, 0, mapPoint(920, 365, 2.3f), pi * .7f, 0, difficulty);
    } else if (sector == 1) {
        // North Valley Avenue (Dr. Nora Marsh): Armored Enforcer + Trooper + Shield-Linking Drone beneath the Mothership!
        spawnAlien(MissionRole::AlienTrooper, 1, mapPoint(655, -225), -pi / 2, 1, difficulty);
        spawnAlien(MissionRole::AlienTrooper, 1, mapPoint(395, -250), pi / 2, 0, difficulty);
        spawnAlien(MissionRole::AlienDrone, 1, mapPoint(565, -248, 2.4f), -pi / 2, 0, difficulty);
    } else {
        // West Timberline Lane (Walt Marsh): silhouetted against the blazing Colorado red-orange sunset!
        spawnAlien(MissionRole::AlienTrooper, 2, mapPoint(-350, 125), pi, 0, difficulty);
        spawnAlien(MissionRole::AlienTrooper, 2, mapPoint(-318, 395), 0.f, 0, difficulty);
        spawnAlien(MissionRole::AlienDrone, 2, mapPoint(-338, 260, 2.3f), pi / 2, 0, difficulty);
    }
    state_.aliensRemaining = livingAliens();
}

void MissionController::spawnExtractionWave(const Snapshot& snap, MissionHost& host) {
    // The enforcer vanguard drops across the misty North Valley Avenue surrounding the town LZ.
    dropAlien(MissionRole::AlienTrooper, mapPoint(40, -225), 1, snap, host);
    dropAlien(MissionRole::AlienTrooper, mapPoint(395, -245), 0, snap, host);
    dropAlien(MissionRole::AlienDrone, mapPoint(210, -268, 2.5f), 0, snap, host);
    if (difficulty_ >= 3) dropAlien(MissionRole::AlienTrooper, mapPoint(-135, -192), 0, snap, host);
    state_.aliensRemaining = livingAliens();
}

bool MissionController::spawnReinforcement(const Snapshot& snap, MissionHost& host) {
    struct Drop { float x, y, h; };
    static constexpr Drop ground[]{
        {-150, -195, 0}, {38, -242, 0}, {388, -258, 0}, {565, -240, 0}, {215, -112, 0}, {-255, -75, 0},
    };
    static constexpr Drop air[]{
        {-140, -180, 2.3f}, {420, -235, 2.3f}, {225, -265, 2.4f}, {560, -210, 2.3f},
    };
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
        // Never drop on top of the player; prefer mid-far town avenues with variety.
        const float score = (dist < 10.f ? -100.f : 0.f) + std::min(dist, 26.f) + float(rng_.draw("evac.drop") * 7.0);
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
    checkpoint_.interrupts = state_.chargeInterrupts;
    checkpoint_.shieldsBroken = state_.shieldsBroken;
    checkpoint_.ambushKills = state_.ambushKills;
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

    // Three surrounding Colorado mountain town districts where the Marsh family took shelter.
    sectors_[0] = {false, false, false, false, 0.0, 1, mapPoint(920, 445)};   // East Ridge Avenue (Ben)
    sectors_[1] = {false, false, false, false, 0.0, 2, mapPoint(535, -215)};  // North Valley Avenue (Dr. Nora Marsh)
    sectors_[2] = {false, false, false, false, 0.0, 3, mapPoint(-298, 252)};  // West Timberline Lane (Walt)
    spawnCivilian(1, 0, mapPoint(920, 445), -pi / 2, 0);
    spawnCivilian(2, 1, mapPoint(535, -215), pi, 1);
    spawnCivilian(3, 2, mapPoint(-298, 252), pi / 2, 2);

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
        state_.chargeInterrupts = checkpoint_.interrupts;
        state_.shieldsBroken = checkpoint_.shieldsBroken;
        state_.ambushKills = checkpoint_.ambushKills;
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
            playerPosOut = mapPoint(210, -210);
            playerYawOut = 0.f;
            spawnCommander(mapPoint(commanderLz[0], commanderLz[1]), 0.f);
            state_.phase = MissionPhase::Incoming;
            setObjective("HOLD NORTH TOWN LZ", "REGROUP WITH DELTA-1 AT THE FLARE CLEARING");
            setRadio("DELTA-1", "CHECKPOINT LOADED. ON ME AT THE NORTH CLEARING - THEY'RE INBOUND.", 4.0f, host);
        } else if (checkpointStage_ == 1) {
            // Back on the portico line with the South Boulevard patrol reset.
            playerPosOut = mapPoint(368, 330);
            playerYawOut = pi;
            spawnCommander(mapPoint(332, 352), pi);
            enterOverwatch(snap, host, true);
            setRadio("DELTA-1", "CHECKPOINT LOADED. THEY'RE STILL ON SOUTH BOULEVARD - TAKE THE FIRST SHOT.", 4.2f, host, true,
                     RadioUrgent);
        } else {
            state_.phase = MissionPhase::Rescue;
            // Checkpoint 2 resumes outside Briar House on South Boulevard ready to sweep the town.
            playerPosOut = mapPoint(368, 565);
            playerYawOut = pi / 2;
            spawnCommander(mapPoint(335, 545), pi / 2);
            setObjective("SWEEP THE TOWN 0/3", "RESCUE BEN, DR. MARSH AND WALT ACROSS THE VALLEY");
            setRadio("DELTA-1", "CHECKPOINT LOADED. MOVE THROUGH TOWN - THE MARSHES ARE PINNED IN THREE DISTRICTS.", 4.6f, host);
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
        addTracer(mapPoint(430, 680, 2.2f), mapPoint(315, 395, 1.6f), cyanPlasma, TracerStyle::RayBolt);
        host.playSound(Sound::PlasmaFire, 0.58f, true, mapPoint(400, 620));
        setRadio("DELTA-1", "WARHORSE IS DOWN. IT'S JUST US NOW - GET UP!", 2.5f, host, true);
    } else if (introCue_ == 2 && introTime_ >= 4.85) {
        introCue_ = 3;
        agents_[0].actor.yaw = pi;
        addTracer(mapPoint(320, 700, 1.8f), mapPoint(412, 385, 2.1f), cyanPlasma, TracerStyle::RayBolt);
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
        setObjective("BREAK OUT ONTO SOUTH BOULEVARD", "CLEAR THE CRASH SITE - TAP SWAP TO MP44");
        setRadio("DELTA-1",
                 cleanOpener ? "CLEAN SHOT! PUSH THROUGH THE BREACH ONTO SOUTH BOULEVARD!"
                 : spotted   ? "THEY'VE SPOTTED US! WEAPONS FREE - BREAK OUT ONTO THE ROAD!"
                             : "CONTACT ON SOUTH BOULEVARD! WEAPONS FREE - BREAK OUT!",
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
    if (flying) {
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
        anchor = mapPoint(commanderLz[0], commanderLz[1]);
        speed = 5.2f;
    } else if (state_.phase == MissionPhase::Regroup || state_.phase == MissionPhase::Rescue) {
        // Delta-1 shadows the player across the mountain town streets, a few paces behind.
        const Vec3 toPlayer{snap.player.position.x - cmd.actor.position.x, 0, snap.player.position.z - cmd.actor.position.z};
        const float dist = planar(toPlayer);
        if (dist > 6.f) cmd.following = true;
        else if (dist < 3.4f) cmd.following = false;
        if (cmd.following) {
            anchor = snap.player.position - toPlayer * (2.8f / std::max(dist, 0.01f));
            speed = dist > 12.f ? 6.0f : 4.6f;
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

    // Field resupply: if the player is running dry during Rescue or Extraction and closes on Delta-1,
    // he tosses fresh magazines so sustained firefights stay tactical instead of stalling out.
    if (!snap.dead && (state_.phase == MissionPhase::Rescue || state_.phase == MissionPhase::Extraction) &&
        elapsed_ >= resupplyReadyAt_ && snap.reserve <= 15 && snap.clip <= 8 &&
        planarSquared(snap.player.position - cmd.actor.position) < 4.2f * 4.2f) {
        resupplyReadyAt_ = elapsed_ + 26.0;
        host.refillPlayer(false);
        setTacticalCallout("DELTA-1 RESUPPLY", calloutGold, 1.8f);
        setRadio("DELTA-1", "MAGS OUT - CATCH! STAY IN THE FIGHT!", 2.6f, host, true, RadioChatter);
    }

    int bestTarget = -1;
    float bestScore = infinity;
    const Vec3 eye = atHeight(cmd.actor.position, botEye);
    for (int i = 4; i < int(agents_.size()); ++i) {
        const auto& alien = agents_[i];
        if (!alien.used || alien.dead || !alien.alerted || alien.spawnFx > 0) continue;
        const Vec3 aim = alien.role == MissionRole::AlienDrone ? alien.actor.position
                                                               : atHeight(alien.actor.position, alienChest * alienScale(alien.variant));
        const float d = length(aim - eye);
        if (d < 26.f && !world_.blocked(eye, aim)) {
            // Prioritize threats to civilians, overcharging Enforcers, and close flankers.
            float score = d;
            if (alien.overcharging) score -= 10.f;
            if (alien.threatTimer > 5.0) score -= 8.f;
            if (alien.tacRole == roleFlanker) score -= 3.f;
            if (score < bestScore) {
                bestScore = score;
                bestTarget = i;
            }
        }
    }

    if (bestTarget >= 0) {
        auto& target = agents_[bestTarget];
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
            addTracer(muzzle, end, amberTracer, TracerStyle::Ballistic);
            host.playSound(Sound::Mp44, 0.62f, true, muzzle);
            target.suppression = std::min(1.f, target.suppression + 0.16f);
            if (target.overcharging) target.chargeTime = std::max(0.0, target.chargeTime - 0.12);
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

// Ground aliens breathe and writhe in place, lope on the move, brace two-handed while
// aiming/charging their ray gun, and kick back sharply on recoil or hit-stagger.
void MissionController::poseAlien(Agent& a) const {
    if (a.role != MissionRole::AlienTrooper) return;
    const uint16_t aimStart = alienIdlePoses + alienStridePoses;
    const uint16_t recoilStart = aimStart + alienAimPoses;
    if (a.gunRecoil > 0.18f || elapsed_ < a.staggerUntil) {
        a.actor.frame = recoilStart;
        a.actor.nextFrame = uint16_t(recoilStart + 1);
        a.actor.frameBlend = std::clamp(1.f - a.gunRecoil, 0.f, 1.f);
    } else if (a.overcharging || a.gunCharge > 0.08f || (a.alerted && a.seesPlayer && a.actor.speed < 1.0f)) {
        a.actor.frame = aimStart;
        a.actor.nextFrame = uint16_t(aimStart + 1);
        a.actor.frameBlend = a.gunCharge > 0.05f ? std::clamp(a.gunCharge, 0.f, 1.f)
                                                 : (0.5f - 0.5f * std::cos(float(elapsed_) * 3.8f + a.hoverPhase));
    } else if (a.actor.speed > 0.4f) {
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

// Only a few aliens may aim at the player at once; the rest flank, pin Delta-1 or lay down
// loose suppressing fire. Also coordinates live squad roles (Vanguard, Suppressor, Flanker, Support).
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
        a.tacRole = rolePatrol;
        if (!a.used || a.dead) continue;
        const Vec3 eye = a.role == MissionRole::AlienDrone ? a.actor.position
                                                           : atHeight(a.actor.position, alienMouth * alienScale(a.variant));
        const float dist = planar(snap.player.position - a.actor.position);
        a.seesPlayer = !snap.dead && dist < 26.f && !world_.blocked(eye, playerEye);
        if (!a.alerted || a.spawnFx > 0) continue;
        score[i] = dist - (a.seesPlayer ? 40.f : 0.f) - (held ? 4.f : 0.f) - (a.variant > 0 ? 5.f : 0.f);
    }
    // Hand out aimed-fire slots to the lowest scores; there are only ever one or two.
    for (int k = 0; k < tokens; ++k) {
        int best = -1;
        for (int i = 4; i < int(agents_.size()); ++i) {
            if (score[i] < infinity && !agents_[i].token && (best < 0 || score[i] < score[best])) best = i;
        }
        if (best < 0) break;
        agents_[best].token = true;
    }
    // Assign coordinated squad roles so alerted aliens pinch instead of clustering.
    int flankers = 0;
    for (int i = 4; i < int(agents_.size()); ++i) {
        auto& a = agents_[i];
        if (!a.used || a.dead || !a.alerted) continue;
        if (a.role == MissionRole::AlienDrone) {
            a.tacRole = roleSupport;
        } else if (a.variant > 0) {
            a.tacRole = roleVanguard;
        } else if (a.token) {
            a.tacRole = roleSuppressor;
        } else {
            a.tacRole = (flankers++ % 2 == 0) ? roleFlanker : roleSuppressor;
        }
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
        a.gunCharge = 0;
        a.gunRecoil = std::max(0.f, a.gunRecoil - float(dt * 4.0));
        a.overcharging = false;
        a.shield = 0;
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

    a.gunRecoil = std::max(0.f, a.gunRecoil - float(dt * 4.5));
    a.suppression = std::max(0.f, a.suppression - float(dt * 0.45));
    a.overchargeCooldown = std::max(0.0, a.overchargeCooldown - dt);
    // Enforcer natural phase-shield recharge after staying out of fire.
    if (!drone && a.variant > 0 && a.maxShield > 0 && elapsed_ >= a.shieldRegenAt && a.shield < a.maxShield) {
        a.shield = std::min(a.maxShield, a.shield + float(9.5 * dt));
    }
    // Unlinked standard troopers lose temporary drone-granted shield quickly.
    if (!drone && a.variant == 0 && !a.linked && a.shield > 0) {
        a.shield = std::max(0.f, a.shield - float(14.0 * dt));
    }
    if (a.linked && !droneLinkWarned_ && state_.phase != MissionPhase::Overwatch) {
        droneLinkWarned_ = true;
        setRadio("DELTA-1", "THAT DRONE'S SHIELDING THEM - DROP THE DRONE FIRST!", 3.4f, host, true, RadioOrder);
    }

    poseAlien(a);

    if (a.spawnFx > 0) {
        a.spawnFx -= dt;
        // A pulsing column from the mothership marks the landing for a moment.
        if ((int(elapsed_ * 60.0) & 3) == 0) {
            const Vec3 base = drone ? a.actor.position : atHeight(a.actor.position, 0.1f);
            addTracer(base + Vec3{0, 26.f, 0}, base, a.variant > 0 ? crimsonPlasma : cyanPlasma, TracerStyle::DropBeam, 0.85f);
        }
    }

    if (elapsed_ < a.staggerUntil) {
        a.overcharging = false;
        a.gunCharge = std::max(0.f, a.gunCharge - float(dt * 5.0));
        a.chargeTime = 0;
        a.burstLeft = 0;
        a.actor.speed = 0;
        return;
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
    const float wantedPitch = std::clamp(-std::atan2(targetAim.y - eye.y, std::max(0.1f, planar(targetAim - eye))), -0.65f, 0.65f);
    a.actor.yaw += wrapDelta(a.actor.yaw, wantedYaw) * float(1.0 - std::exp(-8.0 * dt));
    a.gunPitch += (wantedPitch - a.gunPitch) * float(1.0 - std::exp(-10.0 * dt));

    a.fireCooldown -= dt;
    a.burstPause -= dt;
    const bool suppressing = targetCiv < 0 && !atCommander && !a.token;
    const bool aligned = seesTarget && a.spawnFx <= 0 && a.leapAt < 0 && std::abs(wrapDelta(a.actor.yaw, wantedYaw)) < 0.36f;

    if (!aligned) {
        if (a.overcharging || a.chargeTime > 0) {
            a.chargeTime = std::max(0.0, a.chargeTime - dt * 2.2);
            if (a.chargeTime <= 0.01) {
                a.overcharging = false;
                a.chargeSoundPlayed = false;
            }
        }
        a.gunCharge = a.chargeDuration > 0 ? float(std::clamp(a.chargeTime / a.chargeDuration, 0.0, 1.0)) : 0.f;
        return;
    }

    // Telegraphed ray-gun charging: Troopers briefly spin up their ray rifles before bursts,
    // while Enforcers periodically charge a devastating interruptible Heavy Overcharge Beam!
    if (a.burstLeft <= 0 && a.burstPause <= 0 && a.chargeTime <= 0) {
        const bool startOvercharge = !drone && a.variant > 0 && targetCiv < 0 && !atCommander && a.token &&
                                     a.overchargeCooldown <= 0 && a.sightTime > 0.7 && distTarget < 22.f;
        a.overcharging = startOvercharge;
        a.chargeDuration = startOvercharge ? std::max(0.95, 1.32 - 0.03 * difficulty_)
                                           : (drone ? 0.24 : 0.34);
        a.chargeTime = 0.001;
        a.chargeSoundPlayed = false;
        if (startOvercharge && !overchargeWarned_) {
            overchargeWarned_ = true;
            setRadio("DELTA-1", "ENFORCER CHARGING HEAVY BEAM - HIT ITS HEAD OR TAKE COVER!", 3.6f, host, true, RadioUrgent);
        }
    }

    if (a.chargeTime > 0 && a.burstLeft <= 0) {
        a.chargeTime += dt;
        a.gunCharge = float(std::clamp(a.chargeTime / std::max(0.05, a.chargeDuration), 0.0, 1.0));
        if (!a.chargeSoundPlayed && (a.overcharging || a.gunCharge > 0.15f)) {
            a.chargeSoundPlayed = true;
            host.playSound(Sound::RayCharge, a.overcharging ? 0.88f : 0.42f, true, rayGunMuzzle(a));
        }
        if (a.overcharging) {
            state_.warningLock = true;
            // Crimson targeting laser lock beam telegraphs the impending heavy ray through the mist!
            if ((int(elapsed_ * 60.0) & 1) == 0) {
                const Vec3 m = rayGunMuzzle(a);
                const Vec3 d = normalized(targetAim - m);
                const float lockDist = std::min(length(targetAim - m), world_.ray(m, d, 30.f));
                addTracer(m, m + d * lockDist, crimsonPlasma, TracerStyle::Scanner, 0.25f);
            }
        }
        if (a.chargeTime >= a.chargeDuration) {
            a.chargeTime = 0;
            a.burstLeft = a.overcharging ? 1 : (drone ? 2 : (a.variant > 0 ? 4 : 3));
            a.fireCooldown = 0.02;
        }
        return;
    }

    if (a.burstLeft > 0 && a.fireCooldown <= 0) {
        const bool heavyBeam = a.overcharging;
        --a.burstLeft;
        a.fireCooldown = drone ? 0.17 : 0.14;
        a.gunRecoil = 1.f;
        a.gunCharge = a.burstLeft > 0 ? 0.65f : 0.f;
        if (a.burstLeft == 0) {
            a.overcharging = false;
            a.chargeSoundPlayed = false;
            if (heavyBeam) a.overchargeCooldown = std::max(4.5, 8.2 - 0.35 * difficulty_);
            a.burstPause = (std::max(0.8, 1.6 - difficulty_ * 0.06) + rng_.draw("alien.burst") * 0.45) *
                           (suppressing ? 1.55 : heavyBeam ? 1.35 : 1.0);
        }
        a.radar = 2.8f;
        const Vec3 muzzle = rayGunMuzzle(a);
        Vec3 dir = targetAim - muzzle;
        float spread = std::max(0.045f, 0.125f - difficulty_ * 0.007f);
        if (heavyBeam) spread *= 0.45f;
        if (targetCiv >= 0) {
            spread *= 1.45f;
        } else if (!atCommander) {
            // Fresh sight lines walk their fire in; movement, crouch/prone cover & player suppression reduce accuracy.
            const double acquire = 1.4 - 0.06 * difficulty_;
            spread *= 1.f + 1.85f * float(std::clamp(1.0 - a.sightTime / acquire, 0.0, 1.0));
            spread *= 1.f + std::clamp((planar(snap.playerVelocity) - 1.5f) / 4.f, 0.f, 0.6f);
            if (snap.prone) spread *= 1.40f;
            else if (snap.crouch) spread *= 1.22f;
            spread *= 1.f + 0.38f * a.suppression;
            if (suppressing) spread *= 2.35f;
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
        const uint16_t rayColor = heavyBeam ? overchargeOrange : (a.variant > 0 ? crimsonPlasma : cyanPlasma);
        const TracerStyle rayStyle = heavyBeam ? TracerStyle::HeavyRay : TracerStyle::RayBolt;
        addTracer(muzzle, end, rayColor, rayStyle, heavyBeam ? 1.35f : 1.05f);
        host.playSound(drone ? Sound::DroneFire : Sound::PlasmaFire, heavyBeam ? 1.0f : 0.78f, true, muzzle);

        float boltDmg = (drone ? 6.5f + difficulty_ * 0.5f : 8.f + difficulty_ * 0.6f) + (a.variant > 0 ? 3.f : 0.f);
        if (heavyBeam) boltDmg *= 2.15f;
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

// Exfil: the evac bird sweeps down over the misty northern ridges into the North Town Clearing LZ,
// survivors and Delta-1 sprint aboard across the flare square, and it climbs into the sunset.
namespace {
constexpr double exfilArrive = 4.4, exfilClimb = 3.2;
Vec3 exfilPoint(float x, float y, float h) { return {(x - 316.f) * .05f, h, (212.5f - y) * .05f}; }
const Vec3 exfilHover = exfilPoint(210, -235, 2.4f), exfilApproach = exfilPoint(60, -560, 19.f);
const Vec3 exfilAway = exfilPoint(420, -680, 34.f), exfilEye = exfilPoint(268, -162, 1.75f);
} // namespace

bool MissionController::survivorAlive(int sector) const {
    const auto& civ = agents_[sectors_[sector].civSlot];
    return civ.used && !civ.dead;
}

// One line for the results screen, written from how the dusk operation went.
void MissionController::writeEpilogue() {
    const int clock = 19 * 60 + 47 + int(state_.missionSeconds / 60.f);
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
    const float preferred = drone ? 9.f
                          : a.tacRole == roleVanguard ? 5.8f
                          : a.tacRole == roleFlanker ? 7.6f
                          : a.token ? 7.2f : 9.2f;
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
    constexpr int keep = 36;
    std::array<float, keep> bestScore{};
    std::array<int, keep> bestNode{};
    int kept = 0;
    for (int n = 0; n < nav_.size(); ++n) {
        if (!nav_.valid(n)) continue;
        const Vec3 p = nav_.node(n).position;
        const float travel = planar(p - a.actor.position);
        if (travel < .8f || travel > 11.5f) continue;
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
            const float flankWeight = a.tacRole == roleFlanker ? 7.2f : a.token ? 3.2f : 5.0f;
            const float coverBonus = (nav_.node(n).cover ? (1.4f + 2.0f * a.suppression) : 0.f);
            score = std::abs(range - preferred) * .6f + (1.f - spread / pi) * flankWeight + travel * .24f - coverBonus;
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

// During the opening Breach, alerted aliens flanking the closed front railing sections vault
// over into the estate grounds; once the mission pushes out into town, full nav pathing takes over.
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
    if (state_.phase != MissionPhase::Breach || !a.alerted || a.actor.position.z >= frontWallZ - .2f) return false;
    // Only aliens directly behind the intact iron railing spans vault; center-breach units charge through.
    const float ax = a.actor.position.x;
    const bool behindLeftRailing = ax > -10.2f && ax < breachGapLeftX - .4f;
    const bool behindRightRailing = ax > breachGapRightX + .4f && ax < 11.8f;
    if (!behindLeftRailing && !behindRightRailing) return false;
    const Vec3 takeoff{ax, 0, frontWallZ - 1.1f};
    Vec3 to{takeoff.x - a.actor.position.x, 0, takeoff.z - a.actor.position.z};
    const float dist = planar(to);
    if (dist < .45f) {
        a.leapAt = 0;
        a.leapFrom = {a.actor.position.x, 0, a.actor.position.z};
        a.leapTo = {a.actor.position.x, 0, frontWallZ + 1.4f};
        if (!drone) roar(a.actor.position, host);
        if (!wallCalled_) {
            wallCalled_ = true;
            setRadio("DELTA-1", "THEY'RE VAULTING THE FRONT WALL!", 2.2f, host, true, RadioChatter);
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

    const bool wasUnalerted = !a.alerted;
    a.alerted = true;
    a.radar = 3.0f;
    a.suppression = std::min(1.f, a.suppression + (fromPlayer ? 0.34f : 0.14f));
    if (a.sector >= 0 && a.sector < 3) {
        sectors_[a.sector].triggered = true;
        for (int i = 4; i < int(agents_.size()); ++i) {
            if (agents_[i].used && !agents_[i].dead && agents_[i].sector == a.sector) agents_[i].alerted = true;
        }
    }

    // Ambush Precision: a clean headshot on an unalerted alien rewards careful overwatch & room-clearing.
    const bool ambush = fromPlayer && headshot && wasUnalerted;
    if (ambush) amount *= 1.5f;

    // Charge Interrupt: headshotting (or landing a heavy hit on) a charging Enforcer causes a
    // plasma backfire, staggering it and cancelling the heavy beam!
    bool interrupted = false;
    if (fromPlayer && a.overcharging && (headshot || amount >= 48.f)) {
        interrupted = true;
        a.overcharging = false;
        a.chargeTime = 0;
        a.gunCharge = 0;
        a.chargeSoundPlayed = false;
        a.staggerUntil = elapsed_ + 0.78;
        a.overchargeCooldown = 5.5;
        amount += 28.f;
        ++state_.chargeInterrupts;
        setTacticalCallout("BEAM INTERRUPTED", calloutGold, 1.8f);
        host.playSound(Sound::ShieldBreak, 0.85f, true, a.actor.position);
        if (!interruptPraised_) {
            interruptPraised_ = true;
            setRadio("DELTA-1", "NICE SHOT - DISRUPTED ITS RAY CANNON!", 2.8f, host, true, RadioChatter);
        }
    } else if (fromPlayer && headshot && a.role == MissionRole::AlienTrooper && a.shield <= 0.f) {
        a.staggerUntil = std::max(a.staggerUntil, elapsed_ + 0.34);
    }

    // Phase-shield absorption (Enforcers & Drone-linked Troopers).
    bool hitShield = false;
    bool brokeShield = false;
    if (a.shield > 0.f) {
        hitShield = true;
        a.shieldRegenAt = elapsed_ + 5.5;
        const float absorbed = std::min(a.shield, amount);
        a.shield -= absorbed;
        amount -= absorbed;
        if (a.shield <= 0.01f) {
            a.shield = 0.f;
            brokeShield = true;
            a.staggerUntil = std::max(a.staggerUntil, elapsed_ + 0.45);
            host.playSound(Sound::ShieldBreak, 0.90f, true, a.actor.position);
            if (fromPlayer) {
                ++state_.shieldsBroken;
                if (!interrupted) setTacticalCallout("SHIELD BROKEN", shieldCyan, 1.5f);
            }
        }
    }

    a.actor.health -= amount;
    if (a.role == MissionRole::AlienTrooper && !a.hidOnce && a.actor.health > 0 &&
        (a.actor.health < a.maxHealth * .45f || (fromPlayer && brokeShield))) {
        a.hidOnce = true;
        a.wantsCover = true;
    }
    if (fromPlayer) {
        snap.lethalMarker = a.actor.health <= 0;
        snap.hitMarker = snap.lethalMarker ? 0.33f : 0.15f;
        snap.headshot = headshot;
        state_.shieldHit = hitShield && !brokeShield && !snap.lethalMarker;
        host.playSound(Sound::Hit, 0.65f, false, {});
    }
    if (a.actor.health <= 0) {
        a.actor.health = 0;
        a.dead = true;
        a.deathElapsed = 0;
        a.actor.speed = 0;
        a.token = false;
        a.overcharging = false;
        a.gunCharge = 0;
        // Drone EMP Feedback: destroying a linking Drone shatters its ally's shield and staggers them!
        if (a.role == MissionRole::AlienDrone && a.linkTarget >= 4 && a.linkTarget < int(agents_.size())) {
            auto& mate = agents_[a.linkTarget];
            if (mate.used && !mate.dead) {
                mate.shield = 0.f;
                mate.linked = false;
                mate.overcharging = false;
                mate.chargeTime = 0;
                mate.gunCharge = 0;
                mate.staggerUntil = std::max(mate.staggerUntil, elapsed_ + 0.70);
                host.playSound(Sound::ShieldBreak, 0.85f, true, mate.actor.position);
                if (fromPlayer) setTacticalCallout("LINK DISRUPTED", shieldCyan, 1.7f);
            }
            a.linkTarget = -1;
        }
        ++state_.aliensKilled;
        state_.aliensRemaining = livingAliens();
        if (fromPlayer) {
            snap.killConfirm = 1.25f;
            if (headshot) ++state_.headshots;
            if (ambush) {
                ++state_.ambushKills;
                setTacticalCallout("AMBUSH KILL", calloutGold, 1.7f);
            }
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

    if (victim < 0) {
        // Near-miss suppression: shots cracking past an alerted alien push them toward cover.
        for (int i = 4; i < int(agents_.size()); ++i) {
            auto& a = agents_[i];
            if (!a.used || a.dead || !a.alerted) continue;
            const Vec3 rel = atHeight(a.actor.position, 1.3f) - origin;
            const float along = dot(rel, dir);
            if (along > 1.f && along < std::min(worldDist, 32.f)) {
                const Vec3 perp = rel - dir * along;
                if (dot(perp, perp) < 2.2f * 2.2f) {
                    a.suppression = std::min(1.f, a.suppression + 0.24f);
                }
            }
        }
        return false;
    }
    hitPointOut = origin + dir * best;
    addTracer(origin + forward(snap.camera.yaw, snap.camera.pitch) * 0.5f, hitPointOut, amberTracer, TracerStyle::Ballistic);
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
        state_.waypoint = mapPoint(townLz[0], townLz[1]);
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
    setObjective("PUSH TO NORTH TOWN LZ", "CHECKPOINT SAVED / REGROUP AT THE FLARE CLEARING");
    setRadio("DELTA-1", "THE FAMILY'S MOVING TO THE NORTH CLEARING. ON ME AT THE TOWN LZ - SOMETHING BIG IS COMING.", 4.4f,
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
    setObjective("HOLD THE NORTH TOWN LZ", "");
}

void MissionController::checkPhaseTransitions(Snapshot& snap, double dt, MissionHost& host) {
    if (state_.phase == MissionPhase::Breach) {
        phaseTime_ += dt;
        // Once the Warhorse crash patrol thins out, flankers drop onto South Boulevard East & West.
        if (pendingFlankers_ > 0 && (phaseTime_ > 7.0 || livingAliens() <= 1)) {
            static constexpr float flank[2][2]{{625, 645}, {145, 645}};
            while (pendingFlankers_ > 0) {
                const auto* at = flank[pendingFlankers_ - 1];
                if (dropAlien(MissionRole::AlienTrooper, mapPoint(at[0], at[1]), 0, snap, host) < 0) break;
                --pendingFlankers_;
                if (!flankersCalled_) {
                    flankersCalled_ = true;
                    setRadio("DELTA-1", "DROP BEAMS ON SOUTH BOULEVARD - PUSH OUT THROUGH THE BREACH!", 3.4f, host, true, RadioUrgent);
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
            setObjective("BREAKOUT CLEAR - MOVE INTO TOWN", "CHECKPOINT SAVED / AMMO RESUPPLIED");
            setRadio("DELTA-1", "CRASH SITE CLEAR... CATCH YOUR BREATH. WE'RE PUSHING OUT INTO TOWN.", 2.6f, host, true, RadioUrgent);
        }
        return;
    }

    if (state_.phase == MissionPhase::Regroup) {
        phaseTime_ += dt;
        if (!regroupBriefSent_ && phaseTime_ >= 2.0) {
            regroupBriefSent_ = true;
            setRadio("DELTA-1", "THE MARSHES SCATTERED - BEN'S ON EAST RIDGE, NORA ON NORTH AVE, WALT ON WEST TIMBERLINE.", 4.6f, host, true);
        }
        if (phaseTime_ >= regroupDuration) {
            state_.phase = MissionPhase::Rescue;
            phaseTime_ = 0;
            nextPatrolAt_ = elapsed_ + 10.0;
            setObjective("SWEEP THE TOWN 0/3", "FIND BEN (EAST), DR. MARSH (NORTH) AND WALT (WEST)");
        }
        return;
    }

    if (state_.phase == MissionPhase::Rescue) {
        const Vec3 p = snap.player.position;
        const int activeCount = livingAliens();

        // Trigger each surrounding town district as the player advances down its avenue.
        const bool nearSector[3]{
            p.x > 21.f || planarSquared(p - sectors_[0].center) < 17.5f * 17.5f,
            (p.z > 14.5f && p.x > 1.f) || planarSquared(p - sectors_[1].center) < 17.5f * 17.5f,
            p.x < -21.f || planarSquared(p - sectors_[2].center) < 17.5f * 17.5f,
        };
        const char* sectorCallouts[3]{family[0].callout, family[1].callout, family[2].callout};
        for (int s = 0; s < 3; ++s) {
            auto& sec = sectors_[s];
            if (sec.resolved) continue;
            if (!sec.spawned && nearSector[s] && activeCount <= 1) {
                sec.triggered = true;
                spawnSectorWave(s, difficulty_);
                setRadio("DELTA-1", sectorCallouts[s], 4.2f, host, true, RadioOrder, s);
            } else if (sec.spawned && !sec.triggered && nearSector[s]) {
                sec.triggered = true;
                setRadio("DELTA-1", sectorCallouts[s], 4.2f, host, true, RadioOrder, s);
            }
            if (sec.spawned && livingAliens(s) == 0) {
                sec.clearTimer += dt;
                auto& civ = agents_[sec.civSlot];
                const bool close = planarSquared(p - civ.actor.position) < 12.f * 12.f;
                if (!civ.dead && !civ.rescued && (close || sec.clearTimer > 1.2)) {
                    civ.rescued = true;
                    sec.resolved = true;
                    ++state_.civiliansSaved;
                    host.playSound(Sound::CivilianSaved, 0.85f, false, {});
                    char obj[64];
                    std::snprintf(obj, sizeof(obj), "SWEEP THE TOWN %d/%d", state_.civiliansSaved, state_.civiliansTotal);
                    setObjective(obj, state_.civiliansSaved + state_.civiliansLost >= 3
                        ? "ALL DISTRICTS SWEPT" : "PUSH THROUGH THE MIST TO THE NEXT DISTRICT");
                    // The survivor speaks for themselves, in person, so there is no radio squelch.
                    const bool noraPinned = s != nora && survivorAlive(nora) && !agents_[sectors_[nora].civSlot].rescued;
                    setRadio(family[s].name, noraPinned ? family[s].worried : family[s].rescued, 3.8f, host, false);
                } else if (civ.dead) {
                    sec.resolved = true;
                }
            }
        }

        // Dynamic street patrol ambushes along the misty avenues while moving between town districts!
        const bool outsideEstate = std::abs(p.x) > 19.f || std::abs(p.z) > 15.5f;
        if (outsideEstate && activeCount == 0 && streetPatrols_ < 2 && elapsed_ >= nextPatrolAt_ &&
            (state_.civiliansSaved + state_.civiliansLost) > 0 &&
            (state_.civiliansSaved + state_.civiliansLost) < 3) {
            // Drop a roving patrol along the road ahead of the player toward the next district.
            int nextSec = -1;
            for (int s = 0; s < 3; ++s) {
                if (!sectors_[s].resolved) { nextSec = s; break; }
            }
            if (nextSec >= 0 && planar(sectors_[nextSec].center - p) > 22.f) {
                const Vec3 mid = p + normalized(sectors_[nextSec].center - p) * 14.f;
                const int node = nav_.nearest(mid);
                if (nav_.valid(node)) {
                    const Vec3 spot = nav_.node(node).position;
                    if (planar(spot - p) > 9.f && dropAlien(MissionRole::AlienTrooper, spot, 0, snap, host) >= 0) {
                        dropAlien(MissionRole::AlienDrone, atHeight(spot, 2.4f) + Vec3{1.5f, 0, 1.5f}, 0, snap, host);
                        ++streetPatrols_;
                        nextPatrolAt_ = elapsed_ + 22.0;
                        setRadio("DELTA-1", "ROVING PATROL IN THE MIST AHEAD - LIGHT 'EM UP!", 3.0f, host, true, RadioChatter);
                    }
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
        const bool atLz = planarSquared(snap.player.position - mapPoint(townLz[0], townLz[1])) < 9.5f * 9.5f;
        if (!incomingArrived_ && (atLz || phaseTime_ >= incomingTimeout)) {
            incomingArrived_ = true;
            incomingArrivedAt_ = phaseTime_;
            clearRadioQueue();
            host.playSound(Sound::AlienHorn, 0.88f, false, {});
            setRadio("DELTA-1", "HEAR THAT? THEY'RE DROPPING ACROSS NORTH AVENUE! APACHE IS CLEARED HOT!", 4.2f, host,
                     true, RadioUrgent);
        }
        if (incomingArrived_ && phaseTime_ - incomingArrivedAt_ >= incomingLeadIn) enterExtraction(snap, host);
        return;
    }

    if (state_.phase == MissionPhase::Extraction) {
        phaseTime_ += dt;
        if (evacTimer_ > 0) {
            evacTimer_ = std::max(0.0, evacTimer_ - dt);
            state_.waveIntensity = evacTimer_ <= 20.0 ? 3 : evacTimer_ <= evacDuration_ * 0.58 ? 2 : 1;
            if (evacTimer_ > 8.0 && phaseTime_ >= nextReinforceAt_) {
                nextReinforceAt_ = spawnReinforcement(snap, host)
                    ? phaseTime_ + std::max(2.6, 6.0 - 0.34 * difficulty_ - 0.35 * state_.waveIntensity) : phaseTime_ + 0.5;
            }
            if (evacBeat_ == 0 && evacTimer_ <= evacDuration_ * 0.8) {
                evacBeat_ = 1;
                if (survivorAlive(nora)) {
                    setRadio(family[nora].name, "THEY'RE HOMING ON MY ARRAY - KEEP THEM OFF THE HOUSE!", 3.4f, host, false,
                             RadioChatter);
                }
            } else if (evacBeat_ == 1 && evacTimer_ <= evacDuration_ * 0.55) {
                evacBeat_ = 2;
                if (!mistRemarked_) {
                    mistRemarked_ = true;
                    setRadio("DELTA-1", "MIST IS ROLLING IN OFF THE RIDGES - WATCH FOR THEIR RAY GLOW!", 3.6f, host, true, RadioChatter);
                } else {
                    setRadio("DELTA-1", "KEEP THEM OFF THE LZ! DUSTOFF IS ON THE WAY!", 3.4f, host, true, RadioChatter);
                }
            } else if (evacBeat_ == 2 && evacTimer_ <= 20.0) {
                evacBeat_ = 3;
                host.playSound(Sound::AlienHorn, 0.72f, false, {});
                setRadio("DELTA-1", "TWENTY SECONDS! HEAVY PUSH INCOMING - HOLD THIS LINE!", 3.2f, host, true, RadioOrder);
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

    for (auto& tr : state_.tracers) {
        const float decay = tr.style == TracerStyle::HeavyRay ? 4.5f
                          : tr.style == TracerStyle::RayBolt ? 7.5f
                          : tr.style == TracerStyle::DropBeam ? 6.0f
                          : tr.style == TracerStyle::ShieldLink ? 8.0f
                          : 16.0f;
        tr.time = std::max(0.f, tr.time - float(dt * decay));
    }
    state_.radioTimer = std::max(0.f, state_.radioTimer - float(dt));
    state_.tacticalCalloutTimer = std::max(0.f, state_.tacticalCalloutTimer - float(dt));
    state_.friendlyFireTimer = std::max(0.f, state_.friendlyFireTimer - float(dt));
    if (state_.friendlyFireTimer <= 0) state_.friendlyFireWarn = false;
    state_.warningLock = false;
    updateMistDensity(dt);

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
    updateDroneLinks(dt);
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
        const float shieldRatio = a.maxShield > 0.f ? std::clamp(a.shield / a.maxShield, 0.f, 1.f) : 0.f;
        const float staggerRatio = elapsed_ < a.staggerUntil ? float(std::clamp((a.staggerUntil - elapsed_) / 0.65, 0.0, 1.0)) : 0.f;
        published_->actors[i] = {
            a.actor, a.role, a.variant, a.tacRole, a.used && a.actor.visible,
            a.dead, a.rescued, a.alerted, a.radar,
            a.gunPitch, a.gunCharge, a.gunRecoil, shieldRatio, staggerRatio,
            a.overcharging, a.linked
        };
    }
    target = published_;
}

} // namespace fod::mission_detail

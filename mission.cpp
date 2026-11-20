#include "mission_internal.hpp"

#include "briar_mission_data.hpp"

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
constexpr uint16_t scanCyan = rgb565(42, 168, 198);
constexpr uint16_t crimsonPlasma = rgb565(255, 86, 112);
constexpr uint16_t overchargeOrange = rgb565(255, 116, 62);
constexpr uint16_t shieldCyan = rgb565(96, 236, 255);
constexpr uint16_t calloutGold = rgb565(234, 186, 78);
constexpr uint16_t pulseWhite = rgb565(255, 240, 214);
// A survivor who breaks cover in front of a watcher draws a shot and ducks back.
constexpr float flinchDamage = 16.f;

// Tactical squad roles assigned dynamically to active aliens.
constexpr uint8_t rolePatrol = 0, roleSuppressor = 1, roleFlanker = 2, roleVanguard = 3, roleSupport = 4;

constexpr float standEye = 1.7f, proneEye = 0.42f;
constexpr double introDuration = 6.8;
constexpr double regroupDuration = 3.6;
constexpr double incomingLeadIn = 2.6;   // Warhorn to first drop
constexpr double overwatchNudge = 22.0, overwatchSpotted = 45.0;
constexpr float lastStandHealth = 20.f;
constexpr double reuseDelay = 1.8;

constexpr float alienHeight = 1.88f, alienChest = 1.43f;
constexpr Vec3 alienWeakHalf{.30f, .27f, .18f};
constexpr float alienRadius = .4f;

// The front garden wall runs along map y = 515. In the opening Overwatch/Breach beat, aliens
// flanking the closed iron railing sections vault over into the yard while center-road units
// push straight through the breached portico gap.
constexpr float frontWallZ = (212.5f - 515.f) * .05f;
constexpr float breachGapLeftX = (304.f - 316.f) * .05f;
constexpr float breachGapRightX = (432.f - 316.f) * .05f;
constexpr double leapDuration = .85;

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

Vec3 MissionController::gunGrip(const Agent& a) const {
    if (a.role == MissionRole::AlienDrone) return a.actor.position;
    const auto p = alienGunSocket(assets_, a.actor) * alienScale(a.variant);
    const Vec3 right{std::cos(a.actor.yaw), 0, -std::sin(a.actor.yaw)};
    return a.actor.position + right * p.x + Vec3{0, p.y, 0} + forward(a.actor.yaw, 0.f) * p.z;
}

Vec3 MissionController::rayGunMuzzle(const Agent& a) const {
    const auto grip = gunGrip(a);
    if (a.role == MissionRole::AlienDrone) return grip + forward(a.gunYaw, a.gunPitch) * .52f;
    return alienGunFrame(grip, a.gunYaw, a.gunPitch, a.variant).muzzle(a.variant);
}

MissionController::MissionController(const Assets& assets, const World& world, Navigation& nav)
    : assets_(assets), world_(world), nav_(nav) {}

void MissionController::resetAll() {
    state_ = {};
    state_.mistDensity = 0.50f;
    operation_ = {};
    for (auto& a : agents_) a = {};
    for (auto& s : sectors_) s = {};
    radioQueue_ = {};
    radioQueued_ = 0;
    radioShownAt_ = 0;
    radioPriority_ = RadioChatter;
    introCue_ = 0;
    introSkipped_ = false;
    wakeSounds_ = false;
    overwatchNudgeSent_ = false;
    regroupBriefSent_ = false;
    lzEnforcerLanded_ = false;
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
    evacBeat_ = 0;
    reinforcements_ = 0;
    streetPatrols_ = 0;
    nextPatrolAt_ = 0;
    introTime_ = 0;
    phaseTime_ = 0;
    elapsed_ = 0;
    heartbeatAt_ = 0;
    failDelay_ = 0;
    uplinkGuards_ = 0;
    rescueObjective_ = -1;
    activeSector_ = -1;
    routeReadyAt_ = 0;
    route_.clear();
    evacTimer_ = 0;
    evacDuration_ = 1;
    nextReinforceAt_ = 0;
    graceUntil_ = 0;
    graceReadyAt_ = 0;
    signalEdge_ = signalHeld_ = false;
    signalReadyAt_ = 0;
    dishPulseAt_ = 0;
    dishTipSent_ = false;
    boardingCalled_ = false;
    boardingCue_ = 0;
    commanderHolds_ = false;
    operationHasGoal_ = operationGoalDirect_ = false;
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
        const auto priority = radioQueue_[i].priority;
        const bool aged = priority == RadioChatter ? waited > 4.0 : priority == RadioOrder && waited > 9.0;
        const bool stale = aged || (sector >= 0 && sector < 3 && sectors_[sector].resolved);
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

void MissionController::setSignalHint(const char* hint) {
    state_.signalReady = hint != nullptr;
    copyText(state_.signalHint, sizeof(state_.signalHint), hint);
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
    case MissionPhase::Uplink:
    case MissionPhase::Incoming:   target = 0.84f; break;
    case MissionPhase::Extraction: target = 0.86f + 0.03f * float(state_.waveIntensity); break;
    case MissionPhase::Boarding:   target = 0.88f; break;
    case MissionPhase::Operation:  target = state_.chapter == MissionChapter::Array ? 0.74f : 0.60f; break;
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
        if (!drone.used || drone.dead || !drone.alerted || drone.spawnFx > 0 || drone.role != MissionRole::AlienDrone ||
            drone.operationJammer || elapsed_ < drone.distractedUntil) continue;
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
    civ.actor.health = civilianHealth;
    civ.actor.visible = true;
    civ.maxHealth = civilianHealth;
    civ.home = pos;
    civ.goal = pos;
    civ.radar = 1.f;
}

int MissionController::spawnAlien(MissionRole role, int sector, Vec3 pos, float yaw, uint8_t variant, int difficulty) {
    int chosen = -1;
    for (int i = 4; i < int(agents_.size()); ++i) {
        if (!agents_[i].used || (agents_[i].dead && agents_[i].deathElapsed > reuseDelay)) {
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
    a.gunYaw = yaw - .14f;
    a.gunPitch = .25f;
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
    a.spawnFx = arrivalDuration;
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
    for (const auto& p : roadPatrol) spawnAlien(p.role, -1, p.position, p.yaw, p.tier, difficulty);
    for (int i = 4; i < int(agents_.size()); ++i) {
        if (agents_[i].used && !agents_[i].dead) {
            agents_[i].alerted = false;
            agents_[i].radar = 1.f;
        }
    }
    state_.aliensRemaining = livingAliens();
}

void MissionController::spawnSectorWave(int sector) {
    auto& sec = sectors_[sector];
    size_t free = 0;
    for (size_t i = 4; i < agents_.size(); ++i) {
        const auto& a = agents_[i];
        free += !a.used || (a.dead && a.deathElapsed > reuseDelay);
    }
    if (free < std::size(districtPatrol[sector])) return;
    for (const auto& p : districtPatrol[sector]) {
        const int slot = spawnAlien(p.role, sector, p.position, p.yaw, p.tier, difficulty_);
        agents_[slot].alerted = sector == nora;
    }
    sec.spawned = true;
}

void MissionController::spawnExtractionWave(const Snapshot& snap, MissionHost& host) {
    // The enforcer vanguard drops across the misty North Valley Avenue surrounding the town LZ.
    const size_t count = std::size(lzVanguard) - size_t(difficulty_ < 3);
    for (size_t i = 0; i < count; ++i) {
        const auto& p = lzVanguard[i];
        dropAlien(p.role, p.position, p.tier, snap, host);
    }
    state_.aliensRemaining = livingAliens();
}

bool MissionController::spawnReinforcement(const Snapshot& snap, MissionHost& host) {
    const bool heavyPush = state_.campaign && state_.waveIntensity == 3 && !lzEnforcerLanded_;
    const bool drone = !heavyPush && reinforcements_ % 3 == 2;
    const int enforcerEvery = difficulty_ >= 7 ? 2 : difficulty_ >= 4 ? 3 : 4;
    const uint8_t variant = heavyPush || (!drone && reinforcements_ % enforcerEvery == enforcerEvery - 1) ? 1 : 0;

    const Vec3* options = drone ? lzDroneDrops : lzGroundDrops;
    const int count = drone ? int(std::size(lzDroneDrops)) : int(std::size(lzGroundDrops));
    int best = -1;
    float bestScore = -infinity;
    for (int i = 0; i < count; ++i) {
        const float dist = planar(options[i] - snap.player.position);
        // Never drop on top of the player; prefer mid-far town avenues with variety.
        const float score = (dist < 10.f ? -100.f : 0.f) + std::min(dist, 26.f) + float(rng_.draw("evac.drop") * 7.0);
        if (score > bestScore) {
            bestScore = score;
            best = i;
        }
    }
    if (best < 0) return false;
    if (dropAlien(drone ? MissionRole::AlienDrone : MissionRole::AlienTrooper, options[best], variant, snap, host) < 0) {
        return false;
    }
    ++reinforcements_;
    if (heavyPush) {
        lzEnforcerLanded_ = true;
        host.playSound(Sound::AlienHorn, .72f, true, options[best]);
        setRadio("DELTA-1", "HEAVY COMING DOWN THE BEAM - WATCH THE FRONT!",
                 2.8f, host, true, RadioUrgent);
    }
    return true;
}

void MissionController::saveCheckpoint(MissionCheckpoint stage) {
    checkpoint_.stage = stage;
    state_.checkpointStage = stage;
    checkpoint_.kills = state_.aliensKilled;
    checkpoint_.criticalKills = state_.criticalKills;
    checkpoint_.interrupts = state_.chargeInterrupts;
    checkpoint_.shieldsBroken = state_.shieldsBroken;
    checkpoint_.ambushKills = state_.ambushKills;
    checkpoint_.seconds = state_.missionSeconds;
    for (int c = 0; c < 3; ++c) {
        const auto& civ = agents_[c + 1];
        checkpoint_.civilians[c] = {civ.civilian, civ.actor.position, civ.actor.health};
    }
}

void MissionController::seedExtractionReplay() {
    checkpoint_ = {};
    retries_ = 0;
    checkpoint_.stage = MissionCheckpoint::Extraction;
    for (size_t c = 0; c < checkpoint_.civilians.size(); ++c) {
        checkpoint_.civilians[c] = {
            CivilianOutcome::Sheltered, mapPoint(evacSpots[c][0], evacSpots[c][1]), civilianHealth
        };
    }
}

void MissionController::seedChapterReplay(MissionChapter chapter, bool noraAlive) {
    checkpoint_ = {};
    retries_ = 0;
    state_ = {};
    state_.campaign = true;
    state_.phase = MissionPhase::Victory;
    state_.chapter = chapter == MissionChapter::Array ? MissionChapter::Timberline : MissionChapter::BriarHouse;
    state_.civiliansSaved = noraAlive ? 3 : 2;
    state_.civiliansBoarded = 2;
    noraAlive_ = noraAlive;
    for (auto& sec : sectors_) sec = {};
    sectors_[nora].civSlot = nora + 1;
    agents_[size_t(nora + 1)].civilian = noraAlive ? CivilianOutcome::Stayed : CivilianOutcome::Lost;
}

void MissionController::seedFinaleReplay() {
    seedChapterReplay(MissionChapter::Array);
    state_.chapter = MissionChapter::Array;
    state_.phase = MissionPhase::Operation;
    checkpoint_.stage = MissionCheckpoint::Operation;
    checkpoint_.operationStage = OperationStage::Steer;
    checkpoint_.operationSpawn = mapPoint(368, 34);
    retries_ = -1; // The replay is not a retry; restoring the checkpoint counts it back to zero.
}

void MissionController::restoreCheckpointStats() {
    state_.retries = ++retries_;
    state_.checkpointStage = checkpoint_.stage;
    state_.aliensKilled = checkpoint_.kills;
    state_.criticalKills = checkpoint_.criticalKills;
    state_.chargeInterrupts = checkpoint_.interrupts;
    state_.shieldsBroken = checkpoint_.shieldsBroken;
    state_.ambushKills = checkpoint_.ambushKills;
    state_.missionSeconds = checkpoint_.seconds;
}

void MissionController::start(uint32_t seed, int difficulty, MissionStart mode, Snapshot& snap,
                              Vec3& playerPosOut, float& playerYawOut, MissionHost& host) {
    const bool replay = mode == MissionStart::ExtractionReplay;
    if (replay) seedExtractionReplay();
    const bool retry = mode == MissionStart::Retry && canRetryCheckpoint();
    const bool advance = mode == MissionStart::NextChapter && state_.nextMissionReady();
    const auto next = state_.chapter == MissionChapter::BriarHouse ? MissionChapter::Timberline : MissionChapter::Array;
    const auto chapter = advance ? next : retry ? state_.chapter : MissionChapter::BriarHouse;
    const bool campaign = mode == MissionStart::Campaign || ((retry || advance) && state_.campaign);
    const float earlierSeconds = advance ? state_.earlierMissionSeconds + state_.missionSeconds
                                        : retry ? state_.earlierMissionSeconds : 0.f;
    const int saved = state_.civiliansSaved, boarded = state_.civiliansBoarded;
    if (advance && state_.chapter == MissionChapter::BriarHouse) {
        noraAlive_ = agents_[sectors_[nora].civSlot].civilian == CivilianOutcome::Stayed;
    } else if (!retry && !advance) {
        noraAlive_ = true;
    }
    resetAll();
    difficulty_ = std::clamp(difficulty, 1, 10);
    rng_.state = seed ? seed : 0x44454c54u;
    rng_.tick = 0;

    state_.active = true;
    state_.campaign = campaign;
    state_.chapter = chapter;
    state_.earlierMissionSeconds = earlierSeconds;
    state_.civiliansTotal = 3;
    snap.brokenGlass |= (uint64_t(1) << 2) | (uint64_t(1) << 3) | (uint64_t(1) << 4);
    if (chapter != MissionChapter::BriarHouse) {
        state_.civiliansSaved = saved;
        state_.civiliansBoarded = boarded;
        state_.civiliansLost = state_.civiliansTotal - saved;
        startOperation(snap, playerPosOut, playerYawOut, host, retry);
        return;
    }

    for (int s = 0; s < int(sectors_.size()); ++s) {
        const auto& person = family[s];
        auto& sec = sectors_[s];
        sec.civSlot = s + 1;
        sec.center = mapPoint(person.x, person.y);
        spawnCivilian(sec.civSlot, s, sec.center, person.yaw, uint8_t(s));
    }

    const Vec3 hallWake = mapPoint(368, 305);
    baseWakeYaw_ = pi - 0.14f;

    if (retry || replay) {
        restoreCheckpointStats();
        state_.introProgress = 1.f;
        state_.concussion = 0.f;
        state_.letterbox = 0.f;
        for (size_t c = 0; c < sectors_.size(); ++c) {
            const auto& saved = checkpoint_.civilians[c];
            auto& civ = agents_[c + 1];
            civ.civilian = saved.outcome;
            civ.actor.position = saved.position;
            civ.actor.health = saved.health;
            if (civ.isDead()) {
                civ.deathElapsed = 10.0;
                civ.actor.visible = false;
            } else if (checkpoint_.stage >= MissionCheckpoint::Uplink && civ.civilian == CivilianOutcome::Evacuating) {
                civ.civilian = CivilianOutcome::Sheltered;
                civ.actor.position = mapPoint(evacSpots[c][0], evacSpots[c][1]);
            }
            sectors_[c].resolved = civ.isDead() || civilianRescued(civ.civilian);
            if (!civ.isDead()) poseCivilian(civ);
        }
        refreshCivilianTotals();
        if (checkpoint_.stage >= MissionCheckpoint::Uplink) {
            spawnCommander(mapPoint(commanderLz[0], commanderLz[1]), 0.f);
            if (checkpoint_.stage == MissionCheckpoint::Uplink) {
                playerPosOut = mapPoint(295, -216);
                playerYawOut = pi / 2;
                enterUplink(host);
            } else {
                playerPosOut = mapPoint(210, -210);
                playerYawOut = 0.f;
                state_.uplinkVisible = true;
                enterIncoming(host);
            }
        } else if (checkpoint_.stage == MissionCheckpoint::Overwatch) {
            // Back on the portico line with the South Boulevard patrol reset.
            playerPosOut = mapPoint(368, 330);
            playerYawOut = pi;
            spawnCommander(mapPoint(332, 352), pi);
            enterOverwatch(snap, host, true);
            setObjective("OVERWATCH - FRONT ROAD", "CHECKPOINT LOADED / TAKE THE FIRST SHOT");
        } else {
            state_.phase = MissionPhase::Rescue;
            nextPatrolAt_ = 10.0;
            // Checkpoint 2 resumes outside Briar House on South Boulevard ready to sweep the town.
            playerPosOut = mapPoint(368, 565);
            playerYawOut = pi / 2;
            spawnCommander(mapPoint(335, 545), pi / 2);
            setObjective("SWEEP THE TOWN 0/3", "CHECKPOINT LOADED / FIND BEN, NORA AND WALT");
        }
        auto atCheckpoint = snap;
        atCheckpoint.player.position = playerPosOut;
        updateWaypoint(atCheckpoint);
        return;
    }

    retries_ = 0;
    checkpoint_ = {};
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
        setRadio("DELTA-1", "HEY, BREATHE. EYES ON ME.", 2.2f, host, true);
    } else if (introCue_ == 1 && introTime_ >= 2.55) {
        introCue_ = 2;
        const Vec3 origin = mapPoint(430, 680, 2.2f), path = mapPoint(315, 395, 1.6f) - origin;
        launchPlasma(origin, normalized(path), false, 0.f, -1, length(path), snap);
        host.playSound(Sound::PlasmaFire, 0.58f, true, mapPoint(400, 620));
        setRadio("DELTA-1", "WARHORSE WENT DOWN OUT FRONT. I PULLED YOU IN.", 2.6f, host, true);
    } else if (introCue_ == 2 && introTime_ >= 4.85) {
        introCue_ = 3;
        agents_[0].actor.yaw = pi;
        const Vec3 origin = mapPoint(320, 700, 1.8f), path = mapPoint(412, 385, 2.1f) - origin;
        launchPlasma(origin, normalized(path), false, 0.f, -1, length(path), snap);
        host.playSound(Sound::PlasmaFire, 0.55f, true, mapPoint(330, 640));
        setRadio("DELTA-1", "NOBODY ELSE MADE IT. HOSTILES ARE PICKING THROUGH THE WRECK.", 3.2f, host, true);
    }

    if (progress >= 1.0) {
        snap.prone = snap.crouch = false;
        snap.eyeHeight = standEye;
        enterOverwatch(snap, host, introSkipped_);
    }
}

void MissionController::enterOverwatch(Snapshot& snap, MissionHost&, bool urgent) {
    state_.phase = MissionPhase::Overwatch;
    phaseTime_ = 0;
    state_.introProgress = 1.f;
    state_.concussion = 0.f;
    state_.letterbox = 0.f;
    state_.heartbeatPulse = 0.f;
    spawnOverwatchWave(difficulty_);
    saveCheckpoint(MissionCheckpoint::Overwatch);
    updateWaypoint(snap);
    // No order here. The wreck is framed in the doorway; the scope and the silence do the rest.
    setObjective("OVERWATCH - FRONT ROAD", "SWAP TO THE M40A3 AND TAKE THE FIRST SHOT");
    if (urgent) clearRadioQueue();
}

void MissionController::updateOverwatch(Snapshot& snap, double dt, bool playerFired, MissionHost& host) {
    phaseTime_ += dt;
    if (!overwatchNudgeSent_ && phaseTime_ >= overwatchNudge) {
        overwatchNudgeSent_ = true;
        setRadio("DELTA-1", "THEY'RE CLOSING ON THE FRONT GATE. TAKE YOUR SHOT.", 3.2f, host, true);
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
                 cleanOpener ? "NICE SHOT. MOVE OUT!"
                             : spotted ? "THEY MADE US! PUSH OUT!"
                                       : "WE'RE UP! PUSH OUT!",
                 2.4f, host, true, RadioUrgent);
    }
}

void MissionController::steerAlongPath(Agent& agent, Vec3 target, float speed, double dt, bool flying) {
    const Vec3 delta = target - agent.actor.position;
    const float motionBlend = std::min(1.f, float(dt * 10));
    if (planarSquared(delta) < 0.42f) {
        agent.velocity = {};
        agent.actor.speed = 0;
        agent.actor.trackMotion({}, motionBlend);
        if (flying) {
            agent.hoverPhase += float(dt * 2.4);
            agent.actor.position.y = agent.hoverBase + std::sin(agent.hoverPhase) * 0.14f;
        }
        return;
    }

    Vec3 waypoint = target;
    // Jammers move with the same ground footprint as other drones. Low cover can
    // block that footprint even when the emitter has a clear airborne sightline.
    const bool obstructed = flying && !agent.operationJammer
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
        agent.actor.trackMotion(groundVel, motionBlend);
    } else {
        world_.move(agent.actor.position, agent.velocity, float(dt), 1.85f, 0.4f);
        agent.actor.speed = planar(agent.velocity);
        agent.actor.trackMotion(agent.velocity, motionBlend);
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
    if (state_.phase == MissionPhase::Uplink || state_.phase == MissionPhase::Incoming ||
        state_.phase == MissionPhase::Extraction || state_.phase == MissionPhase::Boarding) {
        anchor = mapPoint(commanderLz[0], commanderLz[1]);
        speed = 5.2f;
    } else if (state_.phase == MissionPhase::Operation && commanderHolds_) {
        // Delta-1 takes a post the operation gives him: the emitter, the hall, the rear door.
        anchor = commanderPost_;
        speed = 5.0f;
    } else if (state_.phase == MissionPhase::Regroup || state_.phase == MissionPhase::Rescue ||
               state_.phase == MissionPhase::Operation) {
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
    if (!snap.dead && (state_.phase == MissionPhase::Rescue || state_.phase == MissionPhase::Uplink ||
                       state_.phase == MissionPhase::Extraction || state_.phase == MissionPhase::Boarding ||
                       state_.phase == MissionPhase::Operation) &&
        elapsed_ >= resupplyReadyAt_ && snap.reserve <= 15 && snap.clip <= 8 &&
        planarSquared(snap.player.position - cmd.actor.position) < 4.2f * 4.2f) {
        resupplyReadyAt_ = elapsed_ + 26.0;
        host.refillPlayer(false);
        setTacticalCallout("DELTA-1 RESUPPLY", calloutGold, 1.8f);
        setRadio("DELTA-1", "HERE - FRESH MAGS.", 1.8f, host, true, RadioChatter);
    }

    int bestTarget = -1;
    float bestScore = infinity;
    const Vec3 eye = atHeight(cmd.actor.position, botEye);
    for (int i = 4; i < int(agents_.size()); ++i) {
        const auto& alien = agents_[i];
        // Jammers are the player's problem: Delta-1 keeps his rifle on the ground fight.
        if (!alien.used || alien.dead || !alien.alerted || alien.spawnFx > 0 || alien.operationJammer) continue;
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

void MissionController::updateCivilian(int slot, Snapshot& snap, double dt, MissionHost& host) {
    auto& civ = agents_[slot];
    if (!civ.used) return;
    if (civ.isDead()) {
        civ.deathElapsed += dt;
        const float k = float(std::clamp(civ.deathElapsed / 0.9, 0.0, 1.0));
        civ.actor.fall = k * k * (1.6f - 0.6f * k);
        return;
    }

    civ.radar = 1.f;
    const auto faceTravel = [&] {
        if (civ.actor.speed <= 0.3f) return;
        const float moveYaw = std::atan2(civ.velocity.x, civ.velocity.z);
        civ.actor.yaw += wrapDelta(civ.actor.yaw, moveYaw) * float(1.0 - std::exp(-9.0 * dt));
    };
    if (state_.phase == MissionPhase::Operation) {
        // Nora in the later missions walks wherever the operation sends her.
        if (planarSquared(civ.goal - civ.actor.position) > .36f) steerAlongPath(civ, civ.goal, 4.2f, dt, false);
        else civ.actor.speed = 0;
        faceTravel();
        if (civ.actor.speed <= .3f) {
            const Vec3 toPlayer = snap.player.position - civ.actor.position;
            civ.actor.yaw += wrapDelta(civ.actor.yaw, std::atan2(toPlayer.x, toPlayer.z)) * float(1.0 - std::exp(-3.0 * dt));
        }
        poseCivilian(civ);
        return;
    }
    if (civ.running) {
        // Across the clearing to Dustoff's open cabin.
        const Vec3 door = exfilHover + Vec3{float(civ.sector - 1) * .8f, -exfilHover.y, -1.5f};
        steerAlongPath(civ, door, 4.0f, dt, false);
        faceTravel();
        if (planarSquared(door - civ.actor.position) < .8f) {
            civ.running = false;
            civ.civilian = CivilianOutcome::Boarded;
            civ.actor.visible = false;
            refreshCivilianTotals();
        }
    } else if (civ.civilian == CivilianOutcome::Evacuating || civ.civilian == CivilianOutcome::Escorting) {
        const bool escort = civ.civilian == CivilianOutcome::Escorting;
        auto& sec = sectors_[size_t(civ.sector)];
        auto& leg = sec.coverLeg;
        const auto& spot = evacSpots[civ.sector];
        const bool nextCover = escort && leg < std::size(waltCoverRoute);
        Vec3 dest = escort ? waltCoverRoute[std::min<size_t>(leg, std::size(waltCoverRoute) - 1)]
                           : mapPoint(spot[0], spot[1]);
        // Ben runs to the player who waved him over, then on toward the clearing.
        if (civ.rallying) {
            if (planarSquared(snap.player.position - civ.actor.position) < 3.f * 3.f) civ.rallying = false;
            else dest = snap.player.position;
        }
        const bool close = planarSquared(snap.player.position - civ.actor.position) < 6.f * 6.f;
        // Walt reads his own yard: he waits while a scanner sweeps his way, then goes.
        if (escort && nextCover && elapsed_ >= civ.holdUntil && sec.held < 4.f) {
            for (size_t i = 4; i < agents_.size(); ++i) {
                const auto& a = agents_[i];
                if (!a.used || a.dead || a.alerted || a.sector != civ.sector) continue;
                const Vec3 to = civ.actor.position - a.actor.position;
                if (planarSquared(to) > 12.f * 12.f) continue;
                // The same sweep the scanner beam draws: Walt moves when it has passed him.
                const bool drone = a.role == MissionRole::AlienDrone;
                const float sweep = a.actor.yaw + std::sin(float(elapsed_) * (drone ? 1.15f : .85f) + a.hoverPhase) * .45f;
                if (std::abs(wrapDelta(sweep, std::atan2(to.x, to.z))) > .32f) continue;
                if (world_.blocked(a.role == MissionRole::AlienDrone ? a.actor.position : atHeight(a.actor.position, 1.6f),
                                   atHeight(civ.actor.position, .9f))) continue;
                civ.holdUntil = elapsed_ + .8;
                if (sec.held == 0.f) setRadio("WALT MARSH", "HOLD UP - SCANNER.", 1.6f, host, false, RadioChatter, civ.sector);
                break;
            }
        }
        const bool holding = escort && elapsed_ < civ.holdUntil;
        if (holding) sec.held += float(dt);
        if (!escort || (nextCover && close && !districtAlerted(civ.sector) && !holding)) {
            steerAlongPath(civ, dest, escort ? 3.1f : 4.4f, dt, false);
        } else {
            civ.actor.speed = 0;
        }
        faceTravel();
        if (!civ.rallying && planarSquared(dest - civ.actor.position) < .5f) {
            if (nextCover && close) {
                ++leg;
                sec.held = 0.f;
            } else if (!escort) {
                civ.civilian = CivilianOutcome::Sheltered;
            }
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

void MissionController::poseCivilian(Agent& c) const {
    if (c.civilian == CivilianOutcome::Pinned) {
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
        c.actor.frame = civilianStandPose;
        c.actor.nextFrame = civilianStandPose + 1;
        c.actor.frameBlend = .5f - .5f * std::cos(float(elapsed_) * 3.3f + c.sector * 1.7f);
    }
    if (c.civilian != CivilianOutcome::Pinned && c.flinchUntil - elapsed_ > .56) {
        c.actor.nextFrame = 1;
        c.actor.frameBlend = float(std::clamp((c.flinchUntil - elapsed_ - .56) * 2.4, 0., .7));
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
                                                           : atHeight(a.actor.position, alienWeakPoint.y * alienScale(a.variant));
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
    if (state_.plasmaShock < plasmaShockDuration * .55f) host.playSound(Sound::PlasmaShock, .86f, false, {});
    state_.plasmaShock = plasmaShockDuration;
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
        setRadio("DELTA-1", "THAT DRONE'S FEEDING ITS SHIELD - DROP THE DRONE FIRST.", 3.2f, host, true, RadioOrder);
    }

    poseAlien(a);

    if (a.spawnFx > 0) a.spawnFx -= dt;

    if (elapsed_ < a.staggerUntil) {
        a.overcharging = false;
        a.gunCharge = std::max(0.f, a.gunCharge - float(dt * 5.0));
        a.chargeTime = 0;
        a.burstLeft = 0;
        a.actor.speed = 0;
        return;
    }
    if (elapsed_ < a.distractedUntil) {
        // A pulse from Nora's dish: the drone swings its emitter toward the house and holds still.
        const Vec3 to = dishFocus() - a.actor.position;
        const float yaw = std::atan2(to.x, to.z);
        a.actor.yaw += wrapDelta(a.actor.yaw, yaw) * float(1.0 - std::exp(-9.0 * dt));
        a.gunYaw = a.actor.yaw;
        a.gunPitch = -std::atan2(to.y, planar(to));
        a.gunCharge = std::max(0.f, a.gunCharge - float(dt * 5.0));
        a.chargeTime = 0;
        a.burstLeft = 0;
        steerAlongPath(a, a.actor.position, 0.f, dt, drone);
        return;
    }

    a.radar = std::max(0.f, a.radar - float(dt));
    const Vec3 eye = drone ? a.actor.position : atHeight(a.actor.position, alienWeakPoint.y * alienScale(a.variant));
    const Vec3 playerEye = atHeight(snap.player.position, snap.eyeHeight);
    const float distPlayer = planar(snap.player.position - a.actor.position);
    if (a.sector == walt && sectors_[walt].resolved && !a.alerted && distPlayer > 34.f) {
        a.used = false;
        return;
    }
    const bool seesPlayer = a.seesPlayer;
    if (seesPlayer) a.sightTime += dt;
    else a.sightTime = std::max(0.0, a.sightTime - dt * 2.0);

    if (!a.alerted) {
        if (state_.phase != MissionPhase::Overwatch) {
            const bool lowInTimberline = a.sector == walt && (snap.crouch || snap.prone);
            const bool closeSight = seesPlayer && distPlayer < (lowInTimberline ? 3.8f : 9.5f);
            // A called drop that nobody engages goes looking for whatever called it.
            const bool hunting = a.huntAt > 0 && elapsed_ >= a.huntAt;
            if (closeSight || (playerFired && distPlayer < 16.f) || hunting) {
                a.alerted = true;
                if (hunting) a.threatTimer = 8.0;
                if (a.squad >= 0) {
                    for (auto& mate : agents_) if (mate.used && !mate.dead && mate.squad == a.squad) mate.alerted = true;
                }
            }
        }
        if (!a.alerted) {
            const float patrolOffset = std::sin(float(elapsed_) * 0.45f + a.hoverPhase) * 1.9f;
            const Vec3 patrolTarget = a.home + Vec3{patrolOffset, 0, std::cos(float(elapsed_) * 0.35f + a.hoverPhase) * 1.1f};
            steerAlongPath(a, patrolTarget, drone ? 1.5f : 1.15f, dt, drone);
            a.gunYaw = a.actor.yaw - .14f;
            a.gunPitch = .25f;
            emitScannerBeam(a);
            return;
        }
    }

    int targetCiv = -1;
    bool atCommander = false;
    Vec3 targetPos = snap.player.position;
    Vec3 targetAim = playerEye;
    bool hasTarget = !snap.dead && a.alerted;

    // Who this alien turns on when it loses the player: its district's survivor, or Nora at the emitter.
    int protectedSlot = -1;
    if (a.sector >= 0 && a.sector < 3 && sectors_[a.sector].spawned) {
        const int cSlot = sectors_[a.sector].civSlot;
        const auto outcome = agents_[size_t(cSlot)].civilian;
        if (outcome == CivilianOutcome::Pinned || outcome == CivilianOutcome::Escorting) protectedSlot = cSlot;
    } else if (state_.phase == MissionPhase::Operation && state_.chapter == MissionChapter::Timberline && a.huntAt > 0 &&
               agents_[1].used && !agents_[1].isDead()) {
        protectedSlot = 1;
    }
    if (state_.phase == MissionPhase::Boarding && !drone) {
        for (int c = 1; c <= 3; ++c) {
            const auto& runner = agents_[size_t(c)];
            if (!runner.running || runner.isDead()) continue;
            const Vec3 body = atHeight(runner.actor.position, .9f);
            if (planarSquared(runner.actor.position - a.actor.position) > 26.f * 26.f || world_.blocked(eye, body)) continue;
            targetCiv = c;
            targetPos = runner.actor.position;
            targetAim = body;
            break;
        }
    }
    if (protectedSlot >= 0 && targetCiv < 0) {
        const int cSlot = protectedSlot;
        const auto& civ = agents_[size_t(cSlot)];
        if (civ.used && !civ.isDead()) {
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
    } else if (a.farJammer) {
        steerAlongPath(a, a.home, 1.2f, dt, true);
    } else if (a.operationJammer) {
        steerAlongPath(a, jammerPost_, 3.8f, dt, true);
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
    const auto aimGun = [&](Vec3 dir) {
        a.gunYaw = std::atan2(dir.x, dir.z);
        a.gunPitch = -std::atan2(dir.y, planar(dir));
    };
    if (a.gunRecoil < .81f) aimGun(targetAim - gunGrip(a) - Vec3{0, drone ? 0.f : rayBoreHeight * alienScale(a.variant), 0});

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
    // while Enforcers periodically charge a a stronger, interruptible plasma charge.
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
            setRadio("DELTA-1", "HEAVY'S CHARGING UP - HIT ITS CHEST OR GET DOWN!", 2.6f, host, true, RadioUrgent);
        }
    }

    if (a.chargeTime > 0 && a.burstLeft <= 0) {
        a.chargeTime += dt;
        a.gunCharge = float(std::clamp(a.chargeTime / std::max(0.05, a.chargeDuration), 0.0, 1.0));
        if (!a.chargeSoundPlayed && (a.overcharging || a.gunCharge > 0.15f)) {
            a.chargeSoundPlayed = true;
            host.playSound(Sound::RayCharge, a.overcharging ? 0.88f : 0.42f, true, rayGunMuzzle(a));
        }
        if (a.overcharging) state_.warningLock = true;
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
        a.chargedShot = heavyBeam;
        a.gunCharge = a.burstLeft > 0 ? 0.65f : 0.f;
        if (a.burstLeft == 0) {
            a.overcharging = false;
            a.chargeSoundPlayed = false;
            if (heavyBeam) a.overchargeCooldown = std::max(4.5, 8.2 - 0.35 * difficulty_);
            a.burstPause = (std::max(0.8, 1.6 - difficulty_ * 0.06) + rng_.draw("alien.burst") * 0.45) *
                           (suppressing ? 1.55 : heavyBeam ? 1.35 : 1.0);
        }
        a.radar = 2.8f;
        poseAlien(a);
        const float scale = alienScale(a.variant);
        Vec3 dir = targetAim - gunGrip(a) - Vec3{0, drone ? 0.f : rayBoreHeight * scale, 0};
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
        aimGun(dir);
        const Vec3 muzzle = rayGunMuzzle(a);
        const float barrel = drone ? .52f : rayMuzzle * rayLength(a.variant) * scale;
        const float damage = (drone ? 6.5f + difficulty_ * .5f : 8.f + difficulty_ * .6f) + (a.variant > 0 ? 3.f : 0.f);
        launchPlasma(muzzle, forward(a.gunYaw, a.gunPitch), heavyBeam,
                     damage * (heavyBeam ? 2.f : 1.f) * (suppressing ? .5f : 1.f),
                     atCommander ? 0 : targetCiv, 52.f, snap, barrel);
        host.playSound(drone ? Sound::DroneFire : Sound::PlasmaFire, heavyBeam ? 1.f : .78f, true, muzzle);

        if (targetCiv >= 0 && a.sector >= 0 && a.sector < 3 && !sectors_[a.sector].warned) {
            sectors_[a.sector].warned = true;
            setRadio("DELTA-1", family[a.sector].underFire, 3.4f, host, true, RadioUrgent, a.sector);
        }
    }
}

// Exfil: the evac bird sweeps down over the misty northern ridges into the North Town Clearing LZ,
// survivors and Delta-1 sprint aboard across the flare square, and it climbs into the sunset.
bool MissionController::survivorAlive(int sector) const {
    const auto& civ = agents_[sectors_[sector].civSlot];
    return civ.used && !civ.isDead();
}

void MissionController::refreshCivilianTotals() {
    state_.civiliansSaved = state_.civiliansLost = state_.civiliansBoarded = 0;
    for (const auto& sec : sectors_) {
        const auto outcome = agents_[sec.civSlot].civilian;
        state_.civiliansSaved += civilianRescued(outcome);
        state_.civiliansLost += outcome == CivilianOutcome::Lost;
        state_.civiliansBoarded += outcome == CivilianOutcome::Boarded;
    }
}

// Anyone still crossing the clearing makes it aboard before the bird lifts.
void MissionController::finishBoarding() {
    for (const auto& sec : sectors_) {
        auto& civ = agents_[size_t(sec.civSlot)];
        if (!civ.running || civ.isDead()) continue;
        civ.running = false;
        civ.civilian = CivilianOutcome::Boarded;
        civ.actor.visible = false;
    }
    refreshCivilianTotals();
}

// One line for the results screen, written from how the evening went.
void MissionController::writeEpilogue() {
    if (state_.chapter != MissionChapter::BriarHouse) {
        writeOperationEpilogue();
        return;
    }
    const int clock = 19 * 60 + 47 + int(state_.missionSeconds / 60.f);
    char line[72];
    const auto outcome = [&](int sector) { return agents_[size_t(sectors_[sector].civSlot)].civilian; };
    const bool benOut = outcome(ben) == CivilianOutcome::Boarded, waltOut = outcome(walt) == CivilianOutcome::Boarded;
    const bool noraStayed = outcome(nora) == CivilianOutcome::Stayed;
    const char* flew = benOut && waltOut ? "BEN AND WALT MADE IT OUT." : benOut ? "BEN MADE IT OUT." : waltOut ? "WALT MADE IT OUT." : "";
    if (state_.phase == MissionPhase::Failed && state_.civiliansLost >= state_.civiliansTotal) {
        std::snprintf(line, sizeof(line), "NO ONE LEFT BRIAR HOUSE ALIVE.");
    } else if (state_.phase == MissionPhase::Failed) {
        std::snprintf(line, sizeof(line), "BRIAR HOUSE WENT SILENT AT %02d:%02d.", clock / 60, clock % 60);
    } else if (noraStayed && benOut && waltOut) {
        std::snprintf(line, sizeof(line), "BEN AND WALT MADE IT OUT. NORA STAYED WITH HER DISH.");
    } else if (noraStayed && (benOut || waltOut)) {
        std::snprintf(line, sizeof(line), "%s NORA STAYED BEHIND. WE LOST %s.", flew, benOut ? "WALT" : "BEN");
    } else if (noraStayed) {
        std::snprintf(line, sizeof(line), "NORA STAYED WITH HER DISH. HER FAMILY DID NOT MAKE IT.");
    } else {
        std::snprintf(line, sizeof(line), "%s WE LOST DR. MARSH.", flew);
    }
    copyText(state_.epilogue, sizeof(state_.epilogue), line);
}

// The hold is over. Dustoff comes down in the clearing while the fight is still going, and the
// family waits at the stone wall for the player to send them across.
void MissionController::beginDustoff(MissionHost& host) {
    state_.phase = MissionPhase::Boarding;
    phaseTime_ = 0;
    exfilTime_ = 0;
    boardingCalled_ = false;
    boardingCue_ = 0;
    state_.evacVisible = true;
    state_.evacPos = exfilApproach;
    const Vec3 heading = exfilHover - exfilApproach;
    state_.evacYaw = std::atan2(heading.x, heading.z);
    // Anyone still on the long town route reaches the wall as the bird comes in.
    for (size_t i = 0; i < sectors_.size(); ++i) {
        auto& civ = agents_[size_t(sectors_[i].civSlot)];
        if (civ.civilian != CivilianOutcome::Evacuating) continue;
        civ.actor.position = mapPoint(evacSpots[i][0], evacSpots[i][1]);
        civ.actor.speed = 0;
        civ.path.clear();
        civ.rallying = false;
        civ.civilian = CivilianOutcome::Sheltered;
        poseCivilian(civ);
    }
    clearRadioQueue();
    setObjective("GET THE FAMILY TO DUSTOFF", "DUSTOFF ON FINAL");
    setRadio("DUSTOFF", "DUSTOFF-1 ON SHORT FINAL! KEEP THEIR HEADS DOWN, DELTA!", 3.2f, host, true, RadioUrgent);
}

void MissionController::updateBoarding(Snapshot& snap, double dt, MissionHost& host) {
    exfilTime_ += dt;
    const double t = exfilTime_;
    Vec3 heading{-1, 0, 0}; // Nose west along the clearing, cabin door toward the wall
    if (t < exfilArrive) {
        const float k = float(t / exfilArrive);
        state_.evacPos = exfilApproach + (exfilHover - exfilApproach) * (1.f - (1.f - k) * (1.f - k));
        heading = exfilHover - exfilApproach;
        state_.evacPitch = .14f * (1.f - k);
    } else {
        state_.evacPos = exfilHover + Vec3{0, std::sin(float(t) * 1.7f) * .12f, 0};
        state_.evacPitch = 0.f;
    }
    state_.evacYaw += wrapDelta(state_.evacYaw, std::atan2(heading.x, heading.z)) * float(1.0 - std::exp(-2.0 * dt));
    state_.aliensRemaining = livingAliens();
    if (t < exfilArrive) return;

    int waiting = 0, crossing = 0;
    bool goingBen = false, goingWalt = false, noraWaiting = false;
    for (int s = 0; s < 3; ++s) {
        const auto& civ = agents_[size_t(sectors_[s].civSlot)];
        if (civ.running && !civ.isDead()) ++crossing;
        if (civ.civilian != CivilianOutcome::Sheltered) continue;
        if (s == nora) {
            noraWaiting = !civ.running;
        } else if (!civ.running) {
            ++waiting;
            goingBen |= s == ben;
            goingWalt |= s == walt;
        }
    }
    if (boardingCue_ == 0) {
        boardingCue_ = 1;
        setRadio("DUSTOFF", "SKIDS DOWN! SIGNAL THE CIVILIANS ACROSS AS SOON AS THE LANE'S CLEAR.", 3.6f, host, true, RadioUrgent);
    } else if (boardingCue_ == 1 && !boardingCalled_ && t > exfilArrive + 28.0) {
        boardingCue_ = 2;
        setRadio("DUSTOFF", "BURNING FUEL ON A HOT LZ, DELTA - GET 'EM ON BOARD!", 3.0f, host, true, RadioChatter);
    }

    if (boardingCalled_) {
        setSignalHint(nullptr);
        // Anyone the clearing will not let through is hauled aboard before the bird has to go.
        if (crossing > 0 && t - boardingCalledAt_ > 6.0) {
            finishBoarding();
            crossing = 0;
        }
        if (crossing == 0) beginLift(snap, host);
        return;
    }
    setSignalHint(waiting ? "SEND THEM TO DUSTOFF" : "SEND DUSTOFF");
    setObjective("GET THE FAMILY TO DUSTOFF",
                 waiting ? "SIGNAL WHEN NOTHING CAN SEE THE WALL" : "SIGNAL DUSTOFF TO LEAVE");
    if (!signalEdge_ || elapsed_ < signalReadyAt_) return;
    signalReadyAt_ = elapsed_ + signalCooldown;

    // Anyone watching the wall sees them break cover, and they duck back down.
    for (int s = 0; s < 3; ++s) {
        auto& civ = agents_[size_t(sectors_[s].civSlot)];
        int shooter = -1;
        const Vec3 door = exfilHover + Vec3{float(civ.sector - 1) * .8f, -exfilHover.y, -1.5f};
        if (s == nora || civ.civilian != CivilianOutcome::Sheltered || !signalThreat(civ, door, shooter)) continue;
        const auto& a = agents_[size_t(shooter)];
        const Vec3 muzzle = rayGunMuzzle(a);
        launchPlasma(muzzle, normalized(atHeight(civ.actor.position, .9f) - muzzle), false, flinchDamage,
                     sectors_[s].civSlot, 48.f, snap);
        host.playSound(a.role == MissionRole::AlienDrone ? Sound::DroneFire : Sound::PlasmaFire, .8f, true, muzzle);
        setRadio(family[s].name, "THEY'VE GOT EYES ON THE WALL - WE'LL GET CUT DOWN!", 2.8f, host, false, RadioUrgent);
        setTacticalCallout("THEY'RE WATCHING THE WALL", crimsonPlasma, 2.0f);
        return;
    }
    boardingCalled_ = true;
    boardingCalledAt_ = t;
    for (int s = 0; s < 3; ++s) {
        auto& civ = agents_[size_t(sectors_[s].civSlot)];
        if (s != nora && civ.civilian == CivilianOutcome::Sheltered) civ.running = true;
    }
    if (noraWaiting) {
        // Nora stays. She knows the dish brought them, and she thinks it can send them away.
        agents_[size_t(sectors_[nora].civSlot)].civilian = CivilianOutcome::Stayed;
        refreshCivilianTotals();
        clearRadioQueue();
        state_.radioTimer = 0.f;
        setRadio("NORA MARSH", goingBen && goingWalt ? "GET MY DAD AND BEN OUT OF HERE. MY DISH BROUGHT THEM - I'M STAYING."
                               : goingWalt ? "GET MY DAD ON THAT BIRD. MY DISH BROUGHT THEM - I'M STAYING."
                               : goingBen  ? "GET BEN ON THAT BIRD. MY DISH BROUGHT THEM - I'M STAYING."
                                           : "LEAVE WITHOUT ME. MY DISH BROUGHT THEM HERE - I'M NOT RUNNING.",
                 4.0f, host, false, RadioStory);
    }
}

// Everyone who is going is aboard. The bird lifts with the player and Delta-1 still on the ground.
void MissionController::beginLift(const Snapshot& snap, MissionHost& host) {
    exfil_ = true;
    exfilTime_ = 0;
    state_.cinematic = true;
    state_.plasma.fill({});
    state_.plasmaShock = 0.f;
    state_.hasWaypoint = false;
    state_.holdRadius = 0.f;
    setSignalHint(nullptr);
    liftCamera_ = snap.camera;
    const bool noraStayed = agents_[size_t(sectors_[nora].civSlot)].civilian == CivilianOutcome::Stayed;
    setObjective("DUSTOFF IS UP", noraStayed ? "NORA STAYS WITH DELTA" : "DELTA STAYS IN THE VALLEY");
    setRadio("DELTA-1",
             noraStayed ? "DUSTOFF, LIFT OFF! WE'RE NOT LEAVING NORA DOWN HERE ALONE."
                        : "DUSTOFF, GET 'EM OUT! HER NOTES ARE STILL IN THE HOUSE - WE'LL FINISH THIS.",
             3.8f, host, true, RadioStory);
}

void MissionController::updateExfil(Snapshot& snap, const Input& input, double dt, MissionHost&) {
    exfilTime_ += dt;
    const double t = exfilTime_;
    const bool skip = t > 1.0 && (input.edge(Jump) || input.edge(Confirm));
    // The bird is gone by liftDuration, but the shot holds until the last line has been read.
    const bool talking = radioQueued_ > 0 || (state_.radioTimer > 0 && elapsed_ - radioShownAt_ < readSeconds(state_.radioLine) + .6);
    if ((t >= liftDuration && (!talking || t >= liftDuration + 6.0)) || skip) {
        finishBoarding();
        exfil_ = false;
        state_.cinematic = false;
        state_.fade = 1.f;
        state_.phase = MissionPhase::Victory;
        state_.evacVisible = false;
        writeEpilogue();
        snap.match = Match::Results;
        return;
    }
    const float k = float(std::clamp(t / exfilClimb, 0.0, 1.0));
    const Vec3 bird = exfilHover + (exfilAway - exfilHover) * (k * k);
    const Vec3 heading = exfilAway - exfilHover;
    state_.evacPos = bird;
    state_.evacYaw += wrapDelta(state_.evacYaw, std::atan2(heading.x, heading.z)) * float(1.0 - std::exp(-2.0 * dt));
    state_.evacPitch = .2f * k;

    // Stay where the player is standing and turn to watch the bird go.
    const Vec3 d = bird - liftCamera_.position;
    const float yaw = std::atan2(d.x, d.z);
    const float pitch = std::clamp(-std::atan2(d.y, std::max(.01f, planar(d))), -1.1f, .4f);
    const float follow = float(1.0 - std::exp(-2.4 * dt));
    snap.camera.position = liftCamera_.position;
    snap.camera.yaw += wrapDelta(snap.camera.yaw, yaw) * follow;
    snap.camera.pitch += (pitch - snap.camera.pitch) * follow;
    snap.camera.fov = 66.f * pi / 180.f;
    snap.playerYaw = snap.camera.yaw;
    snap.playerPitch = snap.camera.pitch;
    state_.letterbox = float(std::min(.85, t * .85));
    state_.fade = talking ? std::min(state_.fade, .35f) : std::max(state_.fade, float(std::clamp((t - (liftDuration - 1.2)) / 1.2, 0.0, 1.0)));
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
    const float eyeHeight = drone ? a.hoverBase : alienWeakPoint.y * alienScale(a.variant);
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
    const float flankWeight = a.tacRole == roleFlanker ? 7.2f : a.token ? 3.2f : 5.0f;
    for (int n = 0; n < nav_.size(); ++n) {
        if (!nav_.valid(n)) continue;
        const auto& node = nav_.node(n);
        const Vec3 p = node.position;
        const float travelSquared = planarSquared(p - a.actor.position);
        if (travelSquared < .8f * .8f || travelSquared > 11.5f * 11.5f) continue;
        const float travel = std::sqrt(travelSquared);
        const float range = planar(p - player);
        float score;
        if (hide) {
            // Nodes beside obstacles are where a sight line can actually be broken.
            score = travel + (range < 4.f ? 6.f : 0.f) - std::min(range, 10.f) * .2f - (node.cover ? 3.f : 0.f);
        } else {
            if (range < 3.5f) continue;
            float spread = pi;
            if (claims) {
                const float bearing = std::atan2(p.x - player.x, p.z - player.z);
                for (int k = 0; k < claims; ++k) {
                    const float gap = std::abs(bearing - claimed[size_t(k)]);
                    spread = std::min(spread, std::min(gap, 2 * pi - gap));
                }
            }
            const float coverBonus = node.cover ? 1.4f + 2.0f * a.suppression : 0.f;
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
    host.playSound(Sound::AlienRoar, 0.95f, true, atHeight(at, alienWeakPoint.y));
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
            setRadio("DELTA-1", "FLANKERS COMING OVER THE GARDEN WALL!", 2.4f, host, true, RadioChatter);
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

void MissionController::damageAgent(int slot, float amount, bool critical, bool fromPlayer,
                                    Snapshot& snap, MissionHost& host) {
    auto& a = agents_[slot];
    if (!a.used || a.isDead()) return;

    if (a.role == MissionRole::Commander) {
        if (fromPlayer) {
            state_.friendlyFireWarn = true;
            state_.friendlyFireTimer = 2.0f;
            setRadio("DELTA-1", "CHECK FIRE! THAT'S ME, WATCH YOUR LINE!", 2.4f, host, true, RadioUrgent);
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
            setRadio("DELTA-1", "CEASE FIRE! THAT'S A CIVILIAN!", 2.4f, host, true, RadioUrgent);
        }
        a.actor.health -= amount;
        if (a.actor.health <= 0) a.running = a.rallying = false;
        if (a.actor.health <= 0 && state_.phase == MissionPhase::Operation) {
            // Nora in the later missions; the operation fails the mission on its next step.
            a.actor.health = 0;
            a.civilian = CivilianOutcome::Lost;
            a.deathElapsed = 0;
            host.playSound(Sound::Death, 0.9f, true, a.actor.position);
            setRadio("DELTA-1", "NORA'S HIT! SHE'S DOWN!", 2.4f, host, true, RadioUrgent);
            return;
        }
        if (a.actor.health <= 0) {
            a.actor.health = 0;
            a.civilian = CivilianOutcome::Lost;
            a.deathElapsed = 0;
            refreshCivilianTotals();
            if (a.sector >= 0 && a.sector < 3) sectors_[a.sector].resolved = true;
            host.playSound(Sound::Death, 0.9f, true, a.actor.position);
            setRadio("DELTA-1", a.sector >= 0 && a.sector < 3 ? family[a.sector].lost : "DAMN IT - WE LOST A CIVILIAN!", 3.5f, host,
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
    for (int i = 4; i < int(agents_.size()); ++i) {
        auto& mate = agents_[i];
        if (!mate.used || mate.dead) continue;
        if ((a.sector >= 0 && a.sector < 3 && mate.sector == a.sector) || (a.squad >= 0 && mate.squad == a.squad)) {
            mate.alerted = true;
        }
    }

    // Ambush Precision: a critical hit on an unalerted alien rewards careful overwatch & room-clearing.
    const bool ambush = fromPlayer && critical && wasUnalerted;
    if (ambush) amount *= 1.5f;

    // A critical hit or a heavy hit backfires a charging Enforcer's cannon.
    bool interrupted = false;
    if (fromPlayer && a.overcharging && (critical || amount >= 48.f)) {
        interrupted = true;
        a.overcharging = false;
        a.chargeTime = 0;
        a.gunCharge = 0;
        a.chargeSoundPlayed = false;
        a.staggerUntil = elapsed_ + 0.78;
        a.overchargeCooldown = 5.5;
        amount += 28.f;
        ++state_.chargeInterrupts;
        setTacticalCallout("CHARGE INTERRUPTED", calloutGold, 1.8f);
        host.playSound(Sound::ShieldBreak, 0.85f, true, a.actor.position);
        if (!interruptPraised_) {
            interruptPraised_ = true;
            setRadio("DELTA-1", "GOOD HIT - BLEW ITS CHARGE RIGHT BACK AT IT.", 2.6f, host, true, RadioChatter);
        }
    } else if (fromPlayer && critical && a.role == MissionRole::AlienTrooper && a.shield <= 0.f) {
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
        snap.headshot = critical;
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
            if (critical) ++state_.criticalKills;
            if (ambush) {
                ++state_.ambushKills;
                setTacticalCallout("AMBUSH KILL", calloutGold, 1.7f);
            }
            host.playSound(critical ? Sound::HeadshotConfirm : Sound::KillConfirm, 0.9f, false, {});
        }
        host.playSound(Sound::AlienDown, 0.72f, true, a.actor.position);
    }
}

bool MissionController::resolvePlayerShot(Vec3 origin, Vec3 dir, float damage, float worldDist, Snapshot& snap,
                                          MissionHost& host, Vec3& hitPointOut) {
    float best = worldDist;
    int victim = -1;
    float zone = 1.f;
    bool critical = false;

    for (int i = 0; i < int(agents_.size()); ++i) {
        const auto& a = agents_[i];
        if (!a.used || !a.actor.visible || a.isDead()) continue;
        float hit = infinity;
        float hitZone = 1.f;
        bool isCritical = false;
        if (a.role == MissionRole::AlienDrone) {
            hit = rayHitBox(origin, dir, a.actor.position, {0.58f, 0.28f, 0.58f}, best);
            if (hit < best) {
                const Vec3 toCenter = a.actor.position - origin;
                const Vec3 perpendicular = toCenter - dir * dot(toCenter, dir);
                isCritical = dot(perpendicular, perpendicular) < .20f * .20f;
                hitZone = isCritical ? 2.1f : 1.25f;
            }
        } else if (a.role == MissionRole::AlienTrooper) {
            const float scale = alienScale(a.variant);
            const Vec3 ahead = forward(a.actor.yaw, 0), right{ahead.z, 0, -ahead.x};
            const Vec3 offset = origin - a.actor.position;
            const Vec3 localOrigin{dot(offset, right), offset.y, dot(offset, ahead)};
            const Vec3 localDir{dot(dir, right), dir.y, dot(dir, ahead)};
            hit = cylinder(origin, dir, a.actor.position, alienRadius * scale, alienHeight * scale);
            const float weak = rayHitBox(localOrigin, localDir, alienWeakPoint * scale, alienWeakHalf * scale, best);
            isCritical = weak < best && weak <= hit + .06f;
            if (isCritical) hit = weak;
            if (hit < best) {
                const float h = localOrigin.y + dir.y * hit;
                hitZone = isCritical ? 2.2f : h > 1.1f * scale ? 1.4f : 1.f;
            }
        } else {
            // Pinned civilians cower about 1.1 m tall; the hitbox follows what is drawn.
            const bool cowering = a.role == MissionRole::Civilian && a.civilian == CivilianOutcome::Pinned;
            hit = cylinder(origin, dir, a.actor.position, 0.36f, cowering ? 1.12f : 1.92f);
            if (hit < best) {
                const float h = (origin.y + dir.y * hit) - a.actor.position.y;
                isCritical = h > (cowering ? .82f : 1.52f);
                hitZone = isCritical ? 2.2f : h > (cowering ? .5f : .9f) ? 1.4f : 1.0f;
            }
        }
        if (hit < best) {
            best = hit;
            victim = i;
            zone = hitZone;
            critical = isCritical;
        }
    }

    if (state_.phase == MissionPhase::Uplink && state_.uplinkHealth > 0.f) {
        const Vec3 center = uplinkPosition + uplinkEmitter;
        const float core = rayHitBox(origin, dir, center, {.42f, .31f, .42f}, best);
        if (core < best) {
            hitPointOut = origin + dir * core;
            addTracer(origin + dir * .5f, hitPointOut, amberTracer);
            state_.uplinkHealth = std::max(0.f, state_.uplinkHealth - damage);
            snap.lethalMarker = state_.uplinkHealth == 0.f;
            snap.headshot = false;
            snap.hitMarker = snap.lethalMarker ? .33f : .15f;
            host.playSound(snap.lethalMarker ? Sound::ShieldBreak : Sound::Hit, .85f, true, center);
            if (snap.lethalMarker) {
                addTracer(center, center + Vec3{0, 3.f, 0}, overchargeOrange, TracerStyle::DropBeam, 3.f);
                enterIncoming(host);
            }
            return true;
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
    damageAgent(victim, damage * zone, critical, true, snap, host);
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

bool MissionController::districtAlerted(int sector) const {
    return std::any_of(agents_.begin() + 4, agents_.end(), [sector](const Agent& a) {
        return a.used && !a.dead && a.sector == sector && a.alerted;
    });
}

int MissionController::nextSector(Vec3 position) const {
    if (activeSector_ >= 0 && !sectors_[activeSector_].resolved) return activeSector_;
    int best = -1;
    float bestDist = infinity;
    for (int s = 0; s < int(sectors_.size()); ++s) {
        if (sectors_[s].resolved) continue;
        const float dist = planarSquared(sectors_[s].center - position);
        if (dist < bestDist) {
            best = s;
            bestDist = dist;
        }
    }
    return best;
}

void MissionController::updateWaypoint(const Snapshot& snap) {
    if (state_.phase != MissionPhase::Operation) state_.holdRadius = 0.f;
    if (exfil_) return;
    bool stageApproach = false;
    Vec3 goal;
    const Vec3 player{snap.player.position.x, 0.f, snap.player.position.z};
    switch (state_.phase) {
    case MissionPhase::Operation:
        if (!operationHasGoal_) {
            state_.hasWaypoint = false;
            return;
        }
        if (operationGoalDirect_) {
            // A place to look at, not walk to: the saddle beyond the lots, a landing across the lane.
            state_.hasWaypoint = true;
            state_.waypoint = operationGoal_;
            state_.waypointIsRoute = false;
            return;
        }
        goal = operationGoal_;
        break;
    case MissionPhase::Uplink:
        goal = uplinkPosition;
        break;
    case MissionPhase::Incoming:
    case MissionPhase::Extraction:
        goal = state_.holdCenter = mapPoint(townLz[0], townLz[1]);
        state_.holdRadius = state_.phase == MissionPhase::Incoming ? assemblyRadius : holdRadius;
        break;
    case MissionPhase::Boarding:
        state_.hasWaypoint = false;
        return;
    case MissionPhase::Rescue: {
        const int s = nextSector(player);
        if (s < 0) {
            state_.hasWaypoint = false;
            return;
        }
        const auto& sec = sectors_[s];
        const auto& civ = agents_[sec.civSlot];
        goal = sec.center;
        if (civ.civilian == CivilianOutcome::Escorting) {
            goal = civ.actor.position;
            if (sec.coverLeg < std::size(waltCoverRoute) && planarSquared(goal - player) <= 6.f * 6.f) {
                goal = waltCoverRoute[sec.coverLeg];
            }
        } else if (s == walt && sec.spawned && (snap.crouch || snap.prone) && !districtAlerted(s) &&
                   livingAliens(s) != 0) {
            goal = waltPickupApproach;
            stageApproach = true;
        } else if (!sec.spawned && planarSquared(sec.center - player) > 12.f * 12.f) {
            goal = districtApproach[s];
            stageApproach = true;
        }
        break;
    }
    default:
        state_.hasWaypoint = false;
        return;
    }
    const bool changedGoal = !state_.hasWaypoint || planarSquared(routeGoal_ - goal) > .25f;
    const bool moved = planarSquared(routeFrom_ - player) > .8f * .8f;
    if (changedGoal || (elapsed_ >= routeReadyAt_ && moved)) {
        const float clearance = state_.phase == MissionPhase::Operation ? operationPlayerClearance : .42f;
        routeReadyAt_ = elapsed_ + .35;
        routeFrom_ = player;
        routeGoal_ = routeWaypoint_ = goal;
        if (world_.walkBlocked(player, goal, clearance) &&
            nav_.path(nav_.nearest(player), nav_.nearest(goal), route_)) {
            for (const int node : route_) {
                const Vec3 pos = nav_.node(node).position;
                if (planarSquared(pos - player) < .8f * .8f) continue;
                if (world_.walkBlocked(player, pos, clearance)) {
                    if (planarSquared(routeWaypoint_ - goal) > .25f) break;
                    continue;
                }
                routeWaypoint_ = pos;
                // Advance through visible nodes without sending the player into blind corners.
                if (planarSquared(pos - player) >= 5.f * 5.f) break;
            }
        }
    }
    state_.hasWaypoint = true;
    state_.waypoint = routeWaypoint_;
    state_.waypointIsRoute = stageApproach || planarSquared(routeWaypoint_ - routeGoal_) > .25f;
}

void MissionController::enterUplink(MissionHost& host) {
    clearRadioQueue();
    state_.phase = MissionPhase::Uplink;
    state_.uplinkVisible = true;
    state_.uplinkHealth = uplinkMaxHealth;
    saveCheckpoint(MissionCheckpoint::Uplink);
    setObjective("DESTROY THE ALIEN RECEIVER", "NORTH AVENUE / SHOOT THE GLOWING CORE");
    // The tower is how they hear Nora's dish. Blind it and they have to come and look.
    if (survivorAlive(nora)) {
        setRadio("NORA MARSH", "THAT RELAY'S LOCKED ONTO MY DISH. KNOCK IT OUT AND THEY GO BLIND.", 4.0f, host, false,
                 RadioUrgent);
    } else {
        setRadio("DELTA-1", "THAT RELAY'S TRACKING HER DISH. BLOW IT SO DUSTOFF CAN GET IN.", 3.6f, host, true, RadioUrgent);
    }
}

void MissionController::enterIncoming(MissionHost& host) {
    clearRadioQueue();
    state_.phase = MissionPhase::Incoming;
    phaseTime_ = 0;
    saveCheckpoint(MissionCheckpoint::Extraction);
    setObjective("REGROUP AT THE NORTH LZ", "CHECKPOINT SAVED / MOVE TO THE FOUR FLARES");
    setRadio("DUSTOFF", "DUSTOFF-1 INBOUND. RALLY ON THE NORTH FLARES AND SECURE THE PAD.", 3.4f, host, true, RadioUrgent);
}

// Nora's dish on the east wing still transmits on a timer. Each pulse swings the shield drone on
// North Avenue round toward the house for a moment and drops its link to the Enforcer.
void MissionController::updateDishPulse(double dt, MissionHost& host) {
    state_.dishPulse = std::max(0.f, state_.dishPulse - float(dt * 1.25));
    const auto& sec = sectors_[nora];
    if (state_.phase != MissionPhase::Rescue || !sec.spawned || sec.resolved) return;
    if (dishPulseAt_ <= 0) dishPulseAt_ = elapsed_ + 3.0;
    if (elapsed_ < dishPulseAt_) return;
    dishPulseAt_ = elapsed_ + dishPulseInterval;
    state_.dishPulse = 1.f;
    const Vec3 focus = dishFocus();
    host.playSound(Sound::RadioChirp, .75f, true, focus);
    for (size_t i = 4; i < agents_.size(); ++i) {
        auto& drone = agents_[i];
        if (!drone.used || drone.dead || drone.role != MissionRole::AlienDrone || drone.sector != nora) continue;
        drone.distractedUntil = elapsed_ + dishDistraction;
        addTracer(focus, drone.actor.position, pulseWhite, TracerStyle::ShieldLink, 1.3f);
        if (drone.linkTarget < 4) continue;
        auto& mate = agents_[size_t(drone.linkTarget)];
        drone.linkTarget = -1;
        if (!mate.used || mate.dead) continue;
        mate.shield = 0.f;
        mate.linked = false;
        mate.shieldRegenAt = elapsed_ + dishDistraction;
        mate.overcharging = false;
        mate.chargeTime = 0;
        mate.gunCharge = 0;
        mate.staggerUntil = std::max(mate.staggerUntil, elapsed_ + .6);
        host.playSound(Sound::ShieldBreak, .8f, true, mate.actor.position);
        setTacticalCallout("LINK DISRUPTED", shieldCyan, 1.7f);
    }
}

// The nearest alien that would see a survivor break cover: one with sight of them, of where they
// are running to, or of the ground in between.
bool MissionController::signalThreat(const Agent& civ, Vec3 destination, int& shooter) const {
    const Vec3 route[]{atHeight(civ.actor.position, 1.f), atHeight((civ.actor.position + destination) * .5f, 1.f),
                       atHeight(destination, 1.f)};
    float best = watcherRange * watcherRange;
    shooter = -1;
    for (size_t i = 4; i < agents_.size(); ++i) {
        const auto& a = agents_[i];
        if (!a.used || a.dead || a.spawnFx > 0 || elapsed_ < a.distractedUntil) continue;
        const float range = planarSquared(a.actor.position - civ.actor.position);
        if (range >= best) continue;
        const Vec3 eye = a.role == MissionRole::AlienDrone ? a.actor.position
                                                           : atHeight(a.actor.position, alienWeakPoint.y * alienScale(a.variant));
        if (std::any_of(std::begin(route), std::end(route), [&](Vec3 at) { return !world_.blocked(eye, at); })) {
            best = range;
            shooter = int(i);
        }
    }
    return shooter >= 0;
}

void MissionController::updateBenSignal(const Snapshot& snap, MissionHost& host) {
    auto& sec = sectors_[ben];
    auto& civ = agents_[size_t(sec.civSlot)];
    if (!sec.spawned || sec.resolved || civ.civilian != CivilianOutcome::Pinned) return;
    const float range = planar(civ.actor.position - snap.player.position);
    if (range > benSignalRange ||
        world_.blocked(atHeight(snap.player.position, snap.eyeHeight), atHeight(civ.actor.position, .9f))) return;
    setSignalHint("SIGNAL BEN");
    const bool beside = range < 3.f && livingAliens(ben) == 0;
    if (elapsed_ < signalReadyAt_ || (!beside && !signalEdge_)) return;
    int shooter = -1;
    const bool watched = signalThreat(civ, snap.player.position, shooter);
    // Standing beside him only brings him when nothing is watching; only a wave draws fire.
    if (watched && !signalEdge_) return;
    signalReadyAt_ = elapsed_ + signalCooldown;
    if (watched) {
        const auto& a = agents_[size_t(shooter)];
        const Vec3 muzzle = rayGunMuzzle(a);
        launchPlasma(muzzle, normalized(atHeight(civ.actor.position, .6f) - muzzle), false, flinchDamage, sec.civSlot,
                     48.f, snap);
        host.playSound(a.role == MissionRole::AlienDrone ? Sound::DroneFire : Sound::PlasmaFire, .8f, true, muzzle);
        setRadio("BEN MARSH", "I CAN'T MOVE - THEY'VE GOT EYES RIGHT ON THIS TRUCK!", 2.8f, host, false, RadioUrgent, ben);
        return;
    }
    civ.civilian = CivilianOutcome::Evacuating;
    civ.rallying = true;
    sec.resolved = true;
    refreshCivilianTotals();
    host.playSound(Sound::CivilianSaved, .85f, false, {});
    setRadio(family[ben].name, survivorLine(ben), 3.8f, host, false, RadioStory);
}

const char* MissionController::survivorLine(int sector) const {
    const auto outcome = agents_[size_t(sectors_[nora].civSlot)].civilian;
    if (sector == nora || outcome == CivilianOutcome::Pinned) return family[sector].noraPinned;
    return outcome == CivilianOutcome::Lost ? family[sector].noraLost : family[sector].noraSafe;
}

void MissionController::updateRescue(Snapshot& snap, MissionHost& host) {
    const Vec3 player = snap.player.position;
    for (int s = 0; s < int(sectors_.size()); ++s) {
        auto& sec = sectors_[s];
        if (sec.resolved) continue;
        const float rangeSq = planarSquared(player - sec.center);
        if (!sec.spawned && rangeSq < 17.5f * 17.5f) {
            spawnSectorWave(s);
            if (sec.spawned) {
                setRadio("DELTA-1", family[s].callout, 3.4f, host, true, RadioOrder, s);
                if (s == nora) {
                    setRadio("NORA MARSH", "MY DISH IS STILL CYCLING - EVERY PULSE PULLS THAT DRONE OFF HIM!", 3.8f, host, false, RadioStory, s);
                }
            }
        }
        if (!sec.spawned) continue;
        if (rangeSq < 17.5f * 17.5f &&
            (activeSector_ < 0 || sectors_[activeSector_].resolved ||
             (rangeSq < 10.f * 10.f && planarSquared(sectors_[activeSector_].center - player) > 22.f * 22.f))) {
            activeSector_ = s;
        }
        auto& civ = agents_[sec.civSlot];
        if (s == ben) {
            updateBenSignal(snap, host);
            continue;
        }
        if (s == walt && civ.civilian == CivilianOutcome::Pinned && livingAliens(s) != 0 &&
            rangeSq < 3.4f * 3.4f && (snap.crouch || snap.prone) && !districtAlerted(s) &&
            !world_.blocked(atHeight(player, snap.eyeHeight), atHeight(civ.actor.position, .75f))) {
            activeSector_ = s;
            civ.civilian = CivilianOutcome::Escorting;
            setRadio("WALT MARSH", "STAY LOW AND STICK ON MY HEELS. I KNOW EVERY FENCE IN THIS YARD.", 3.6f, host, false, RadioUrgent, s);
        }
        if (civ.civilian == CivilianOutcome::Escorting) {
            if (districtAlerted(s) || sec.coverLeg < std::size(waltCoverRoute) ||
                planarSquared(civ.actor.position - player) >= 6.f * 6.f) continue;
        } else if (rangeSq >= 8.5f * 8.5f || livingAliens(s) != 0) {
            continue;
        }
        civ.civilian = CivilianOutcome::Evacuating;
        sec.resolved = true;
        refreshCivilianTotals();
        host.playSound(Sound::CivilianSaved, .85f, false, {});
        setRadio(family[s].name, survivorLine(s), 4.0f, host, false, RadioStory);
        if (s == nora) setRadio("DELTA-1", "GET BEHIND THE NORTH WALL - WE'LL TALK AT THE LZ, DOC.", 3.4f, host, true);
    }

    state_.aliensRemaining = livingAliens();
    const int next = nextSector(player);
    if (next < 0) {
        if (state_.civiliansSaved > 0) {
            if (snap.clip <= 8 && snap.reserve <= 15) {
                host.refillPlayer(false);
                setTacticalCallout("DELTA-1 RESUPPLY", calloutGold, 1.8f);
            }
            enterUplink(host);
        }
        return;
    }
    const auto& sec = sectors_[next];
    const bool escort = agents_[sec.civSlot].civilian == CivilianOutcome::Escorting;
    const int step = escort ? (districtAlerted(next) ? 5 : 4)
                   : !sec.spawned ? 0
                   : next == ben ? 6
                   : livingAliens(next) == 0 ? 2 : next == walt && !districtAlerted(next) ? 3
                   : next == nora && std::any_of(agents_.begin() + 4, agents_.end(), [](const Agent& a) {
                         return a.used && !a.dead && a.sector == nora && a.role == MissionRole::AlienDrone;
                     }) ? 7 : 1;
    const int objective = (state_.civiliansSaved + state_.civiliansLost) * 18 + next * 8 + step;
    if (objective != rescueObjective_) {
        rescueObjective_ = objective;
        char title[40], instruction[64];
        std::snprintf(title, sizeof(title), "SWEEP THE TOWN %d/3", state_.civiliansSaved + state_.civiliansLost);
        constexpr const char* actions[]{"FIND", "CLEAR HOSTILES NEAR", "REACH"};
        if (step < 3) std::snprintf(instruction, sizeof(instruction), "%s %s", actions[step], family[next].objective);
        else if (step == 3) copyText(instruction, sizeof(instruction), "CROUCH TO REACH WALT, OR CLEAR THE PATROL");
        else if (step == 4) copyText(instruction, sizeof(instruction), "STAY LOW AND FOLLOW WALT TO THE STONE WALL");
        else if (step == 5) copyText(instruction, sizeof(instruction), "WALT IS IN COVER. CLEAR THE PATROL TO CONTINUE");
        else if (step == 6) copyText(instruction, sizeof(instruction), "SIGNAL BEN ACROSS WHEN NOTHING CAN SEE HIM");
        else copyText(instruction, sizeof(instruction), "HIT THE BIG ONE WHILE THE DISH HAS THE DRONE");
        setObjective(title, instruction);
    }

    // A patrol can intercept a player between districts. It takes the same route as the waypoint.
    const bool outsideEstate = std::abs(player.x) > 19.f || std::abs(player.z) > 15.5f;
    if (!outsideEstate || state_.aliensRemaining > 0 || streetPatrols_ >= 2 || elapsed_ < nextPatrolAt_ ||
        state_.civiliansSaved + state_.civiliansLost == 0 || planarSquared(sec.center - player) <= 22.f * 22.f) return;
    const int node = nav_.nearest(player + normalized(sec.center - player) * 14.f);
    if (!nav_.valid(node)) return;
    const Vec3 spot = nav_.node(node).position;
    if (planarSquared(spot - player) <= 9.f * 9.f ||
        dropAlien(MissionRole::AlienTrooper, spot, 0, snap, host) < 0) return;
    ++streetPatrols_;
    nextPatrolAt_ = elapsed_ + 22.0;
    setRadio("DELTA-1", "EYES UP - PATROL DROPPING IN AHEAD.", 2.4f, host, true, RadioChatter);
}

void MissionController::enterExtraction(Snapshot& snap, MissionHost& host) {
    state_.phase = MissionPhase::Extraction;
    phaseTime_ = 0;
    evacDuration_ = evacTimer_ = 39.0 + 1.8 * difficulty_;
    state_.evacTotal = float(evacDuration_);
    state_.evacSeconds = float(evacTimer_);
    nextReinforceAt_ = 6.0;
    reinforcements_ = 0;
    evacBeat_ = 0;
    spawnExtractionWave(snap, host);
    snap.apacheReady = true;
    setObjective("HOLD THE NORTH TOWN LZ", "");
}

void MissionController::checkPhaseTransitions(Snapshot& snap, double dt, MissionHost& host) {
    if (state_.phase == MissionPhase::Breach) {
        phaseTime_ += dt;
        // Once the Warhorse crash patrol thins out, flankers drop onto South Boulevard East & West.
        if (pendingFlankers_ > 0 && (phaseTime_ > 7.0 || livingAliens() <= 1)) {
            while (pendingFlankers_ > 0) {
                if (dropAlien(MissionRole::AlienTrooper, breachFlank[pendingFlankers_ - 1], 0, snap, host) < 0) break;
                --pendingFlankers_;
                if (!flankersCalled_) {
                    flankersCalled_ = true;
                    setRadio("DELTA-1", "DROP BEAMS ON THE BOULEVARD - MORE INBOUND!", 2.8f, host, true, RadioUrgent);
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
            saveCheckpoint(MissionCheckpoint::Rescue);
            setObjective("BREAKOUT CLEAR - MOVE INTO TOWN", "CHECKPOINT SAVED / AMMO RESUPPLIED");
            setRadio("DELTA-1", "ROAD'S CLEAR. TOP OFF MAGS.", 2.2f, host, true, RadioUrgent);
        }
        return;
    }

    if (state_.phase == MissionPhase::Regroup) {
        phaseTime_ += dt;
        if (!regroupBriefSent_ && phaseTime_ >= 2.0) {
            regroupBriefSent_ = true;
            setRadio("DELTA-1", "MISSION STILL STANDS - WE GET DR. MARSH OUT. SAW HER SON HEAD EAST, HER DAD WEST.", 4.6f, host, true, RadioStory);
        }
        if (phaseTime_ >= regroupDuration) {
            state_.phase = MissionPhase::Rescue;
            phaseTime_ = 0;
            nextPatrolAt_ = elapsed_ + 10.0;
            setObjective("SWEEP THE TOWN 0/3", "FIND BEN (EAST), NORA (NORTH) AND WALT (WEST)");
        }
        return;
    }

    if (state_.phase == MissionPhase::Rescue) {
        updateRescue(snap, host);
        return;
    }

    if (state_.phase == MissionPhase::Uplink) {
        const float rangeSq = planarSquared(snap.player.position - uplinkPosition);
        if (rangeSq < 19.f * 19.f) {
            while (uplinkGuards_ < std::size(uplinkGuard)) {
                const bool drone = uplinkGuards_ == 0;
                if (dropAlien(drone ? MissionRole::AlienDrone : MissionRole::AlienTrooper,
                              uplinkGuard[uplinkGuards_], drone ? 0 : 1, snap, host) < 0) break;
                ++uplinkGuards_;
            }
        }
        return;
    }

    if (state_.phase == MissionPhase::Incoming) {
        const Vec3 lz = mapPoint(townLz[0], townLz[1]);
        if (planarSquared(snap.player.position - lz) >= assemblyRadius * assemblyRadius) return;
        if (phaseTime_ == 0) {
            clearRadioQueue();
            host.playSound(Sound::AlienHorn, .88f, false, {});
            setRadio("DELTA-1", "RELAY'S DEAD - NOW THEY'RE DROPPING TROOPS TO SEARCH THE VALLEY. HOLD HERE!", 3.8f, host, true, RadioUrgent);
        }
        phaseTime_ += dt;
        if (phaseTime_ >= incomingLeadIn) enterExtraction(snap, host);
        return;
    }

    if (state_.phase == MissionPhase::Extraction) {
        state_.aliensRemaining = livingAliens();
        const Vec3 lz = mapPoint(townLz[0], townLz[1]);
        if (planarSquared(snap.player.position - lz) >= holdRadius * holdRadius) {
            setObjective("RETURN TO THE NORTH LZ", "MOVE TO THE FLARES TO CONTINUE EXTRACTION");
            return;
        }
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
                // The idea the rest of the campaign is built on. It waits for a gap; it is never dropped.
                if (survivorAlive(nora)) {
                    setRadio("NORA MARSH", "WAIT - IF MY SIGNAL PULLED THEM HERE, I CAN SPOOF IT AND LEAD THEM OUT.", 4.2f, host,
                             false, RadioStory);
                }
            } else if (evacBeat_ == 1 && evacTimer_ <= evacDuration_ * 0.55) {
                evacBeat_ = 2;
                setRadio("DELTA-1", "HEAR THOSE ROTORS? DUSTOFF'S ALMOST ON US - HOLD THE LINE!", 3.0f, host, true, RadioChatter);
            } else if (evacBeat_ == 2 && evacTimer_ <= 20.0) {
                evacBeat_ = 3;
                if (!state_.campaign) {
                    host.playSound(Sound::AlienHorn, 0.72f, false, {});
                    setRadio("DELTA-1", "TWENTY SECONDS OUT - DON'T LET 'EM OVERRUN THE WALL!", 2.8f, host, true, RadioOrder);
                }
            }
            const int secs = int(std::ceil(evacTimer_));
            char sub[64];
            std::snprintf(sub, sizeof(sub), snap.apacheReady ? "EVAC IN %d:%02d  /  UP: CALL APACHE" : "EVAC IN %d:%02d",
                          secs / 60, secs % 60);
            setObjective("HOLD THE NORTH TOWN LZ", sub);
        }
        state_.evacSeconds = float(evacTimer_);
        if (evacTimer_ > 0) return;
        beginDustoff(host);
        return;
    }

    if (state_.phase == MissionPhase::Boarding) updateBoarding(snap, dt, host);
}

void MissionController::step(Snapshot& snap, const Input& input, double dt, bool playerFired,
                             MissionHost& host) {
    if (!state_.active) return;
    elapsed_ += dt;
    rng_.tick = snap.tick;
    // The pause menu can change difficulty mid-mission; apply it to the aliens too.
    difficulty_ = std::clamp(snap.difficulty, 1, 10);

    for (auto& tr : state_.tracers) {
        const float decay = tr.style == TracerStyle::PlasmaImpact ? 4.6f
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
    signalEdge_ = input.edge(Signal);
    signalHeld_ = input.down(Signal);
    setSignalHint(nullptr);

    if (state_.phase == MissionPhase::Failed) {
        failDelay_ -= dt;
        if (failDelay_ <= 0) snap.match = Match::Results;
        return;
    }
    if (state_.phase == MissionPhase::Victory) return;
    if (exfil_) {
        // The clock stopped when the last survivor boarded; the lift plays on its own time.
        updateExfil(snap, input, dt, host);
        pumpRadio(host);
        return;
    }

    state_.missionSeconds += float(dt);
    state_.plasmaShock = std::max(0.f, state_.plasmaShock - float(dt));
    if (!snap.dead) updatePlasma(snap, float(dt), host);

    if (snap.dead) {
        clearRadioQueue();
        state_.plasma.fill({});
        state_.phase = MissionPhase::Failed;
        copyText(state_.failReason, sizeof(state_.failReason), "DELTA OPERATIVE KIA");
        writeEpilogue();
        failDelay_ = 2.0;
        setRadio("DELTA-1", "STAY WITH ME! HANG ON, I'VE GOT YOU...", 2.2f, host, true, RadioUrgent);
        return;
    }

    if (state_.phase == MissionPhase::Intro) {
        updateIntro(snap, input, dt, host);
        updateCommander(snap, dt, host);
        for (int c = 1; c <= 3; ++c) updateCivilian(c, snap, dt, host);
        pumpRadio(host);
        return;
    }

    if (state_.phase == MissionPhase::Overwatch) {
        updateOverwatch(snap, dt, playerFired, host);
    }

    plannedThisTick_ = false;
    assignAttackTokens(snap);
    updateDroneLinks(dt);
    updateDishPulse(dt, host);
    updateCommander(snap, dt, host);
    for (int c = 1; c <= 3; ++c) updateCivilian(c, snap, dt, host);
    for (int i = 4; i < int(agents_.size()); ++i) updateAlien(i, snap, dt, playerFired, host);
    if (state_.phase == MissionPhase::Operation) stepOperation(snap, dt, host);
    else checkPhaseTransitions(snap, dt, host);
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
            a.isDead(), a.civilian, a.alerted, a.radar,
            a.gunYaw, a.gunPitch, a.gunCharge, a.gunRecoil, shieldRatio, staggerRatio,
            a.overcharging, a.linked, std::max(0.f, float(a.spawnFx) / arrivalDuration), a.chargedShot
        };
    }
    target = published_;
}

} // namespace fod::mission_detail

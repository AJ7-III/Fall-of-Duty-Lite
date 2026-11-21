#include "mission_internal.hpp"

#include "briar_mission_data.hpp"
#include "scout_data.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace fod::mission_detail {
namespace {

using namespace game_detail;

constexpr Vec3 road = mapPoint(300, 648);
constexpr Vec3 approach = mapPoint(368, 34);
constexpr Vec3 fieldKit = mapPoint(627, 298);
constexpr Vec3 retrieval = mapPoint(393, -102);
constexpr Vec3 overlook = mapPoint(0, -292), ridgeRetrieval = mapPoint(-12, -343);
constexpr Vec3 northPad = mapPoint(210, -220);
constexpr Vec3 southPad = mapPoint(432, 610);
constexpr Vec3 dishOperator = mapPoint(609, 79);
constexpr Vec3 flagshipVista{8, 10, 97};
constexpr Vec3 groundedVista{-22, -3.5f, 78};
constexpr Vec3 groundedValley = mapPoint(-535, -350, 4.f);
constexpr Vec3 flightStart{43, 43, 62};
constexpr Vec3 northGlide[]{{8, 33, 55}, {-25, 20, 37}, {-5.3f, 12, 21.625f}};
constexpr Vec3 southGlide[]{{31, 34, 28}, {20, 23, -7}, {5.8f, 12, -19.875f}};
constexpr Vec3 northValleyGlide[]{
    {28, 55, 77}, {-6, 62, 83}, {-31, 57, 67}, {-41, 45, 44}, {-24, 30, 34}, {-5.3f, 12, 21.625f}
};
constexpr Vec3 southValleyGlide[]{
    {28, 55, 77}, {-6, 62, 83}, {-31, 57, 67}, {-41, 46, 34},
    {-31, 36, 6}, {-18, 24, -16}, {5.8f, 12, -19.875f}
};
constexpr uint16_t cyan = 0x5f7f, warning = 0xfb92;
constexpr unsigned noraSlot = 1;

float ease(double t) {
    const auto u = float(std::clamp(t, 0., 1.));
    return u * u * (3.f - 2.f * u);
}

float bearing(Vec3 from, Vec3 to) { return std::atan2(to.x - from.x, to.z - from.z); }

Vec3 pad(bool burned) { return burned ? southPad : northPad; }

Vec3 approachBeacon(bool legacy, bool burned, size_t index) {
    if (legacy) return (burned ? southGlide : northGlide)[index];
    return burned ? southValleyGlide[index] : northValleyGlide[index];
}

Vec3 disembark(Vec3 landing) { return landing + Vec3{5.25f, 0, -4.7f}; }

void lookAt(Snapshot& snap, Vec3 eye, Vec3 focus, float fov) {
    const Vec3 line = focus - eye;
    snap.camera = {eye, bearing(eye, focus), -std::atan2(line.y, planar(line)), fov * pi / 180.f};
}

} // namespace

void MissionController::startScoutOperation(Snapshot& snap, Vec3& position, float& yaw, MissionHost& host, bool retry) {
    state_.phase = MissionPhase::Operation;
    state_.introProgress = 1.f;
    const auto stage = retry ? checkpoint_.operationStage : OperationStage::ScoutApproach;
    if (retry) restoreCheckpointStats();
    state_.operation.clearingBurned = retry && checkpoint_.clearingBurned;
    scout_.port = retry && checkpoint_.scoutPort;
    host.useMissionScene(missionScene(state_.chapter, stage));
    enterScoutStage(stage, host);
    position = retry ? checkpoint_.operationSpawn : road;
    yaw = bearing(position, operationGoal_);
    if (stage == OperationStage::ScoutReclaim) yaw = pi;
    if (scoutDeck(stage)) snap.brokenGlass = 0;
    saveOperationCheckpoint(position);
    auto atSpawn = snap;
    atSpawn.player.position = position;
    updateWaypoint(atSpawn);
    publishScoutFleet();
}

void MissionController::enterScoutStage(OperationStage stage, MissionHost& host) {
    using namespace scout;
    const auto squad = [&](std::initializer_list<Patrol> patrol, int group) {
        for (const auto& p : patrol) {
            const auto crew = spawnAlien(p.role, -1, p.position, p.yaw, p.tier, difficulty_);
            configureAliens(crew, [group](Agent& a) { a.squad = int8_t(group); });
        }
    };
    auto& op = state_.operation;
    auto& view = state_.scout;
    op.stage = stage;
    operation_.stageTime = 0;
    operation_.cue = 0;
    if (!legacyScout_) {
        operation_.settled = 0;
        view.autoReturn = false;
        view.groundSecured = true;
        if (view.retrievalCrew) view.cargoVisible = false;
        view.retrievalCrew = false;
    }
    scout_.progress = 0;
    scout_.landingAt = -1;
#ifndef ESP_PLATFORM
    view.landingGear = 0.f;
#endif
    operationHasGoal_ = true;
    operationGoalDirect_ = false;
    op.painting = op.targetSet = op.kitVisible = false;
    state_.cinematic = false;
    state_.letterbox = 0;
    state_.flagshipBeam = false;
    state_.dishLink = false;
    switch (stage) {
    case OperationStage::ScoutApproach:
        if (!legacyScout_) {
            if (!agents_[0].used) {
                spawnCommander(approach + Vec3{0, 0, -2.f}, 0);
                if (noraAlive_) {
                    spawnCivilian(noraSlot, -1, dishOperator, 0, uint8_t(nora));
                    agents_[noraSlot].civilian = CivilianOutcome::Sheltered;
                    agents_[noraSlot].goal = dishOperator;
                }
            }
            commanderHolds_ = true;
            commanderPost_ = approach + Vec3{0, 0, -2.f};
            operationGoal_ = overlook;
            view.capture = 0;
            op.lock = 0;
            op.pulled = false;
            setObjective("INVESTIGATE THE LAST SCOUT", "FOLLOW THE NORTH STONE WALL TO THE OVERLOOK");
            setRadio(Line::ArrayScoutSighted, 3.8f, host, false, RadioStory);
            break;
        }
        if (noraAlive_) {
            spawnCivilian(noraSlot, -1, mapPoint(325, 628), 0, uint8_t(nora));
            agents_[noraSlot].civilian = CivilianOutcome::Sheltered;
            agents_[noraSlot].goal = dishOperator;
        }
        spawnCommander(mapPoint(263, 653), 0);
        squad({{MissionRole::AlienTrooper, mapPoint(338, 451), -.65f},
               {MissionRole::AlienTrooper, mapPoint(490, 233), pi},
               {MissionRole::AlienDrone, mapPoint(363, 186, 2.8f), pi}}, 11);
        operationGoal_ = op.kit = fieldKit;
        op.kitVisible = true;
        setObjective("RECOVER THE TIMBERLINE EMITTER", "EAST GARAGE / TAKE THE FIELD KIT");
        setRadio(noraAlive_ ? Line::NoraReturnToHouse : Line::DeltaReturnToBriar, 4.5f, host, !noraAlive_, RadioStory);
        break;
    case OperationStage::ScoutBoard:
        state_.cinematic = true;
        state_.letterbox = .75f;
        operationHasGoal_ = false;
        state_.plasma.fill({});
        plasmaFlights_.fill({});
        setObjective("RIDING THE RETRIEVAL BEAM", "SCOUT / CARGO BAY");
        dismissRadio(host);
        setRadio(legacyScout_ ? (noraAlive_ ? Line::ScoutNoraBoard : Line::ScoutDeltaBoard)
                             : (noraAlive_ ? Line::ArrayNoraBoard : Line::ArrayDeltaBoard),
                 3.6f, host, false, RadioStory);
        host.playSound(Sound::AlienHorn, .66f, false, {});
        break;
    case OperationStage::ScoutCargo:
        squad({{MissionRole::AlienTrooper, {-5, 0, -14.4f}, 0},
               {MissionRole::AlienTrooper, {4, 0, -12.4f}, -.2f}}, 12);
        if (!legacyScout_) squad({{MissionRole::AlienDrone, {2.5f, 2.7f, -11.5f}, pi}}, 12);
        operationGoal_ = cargoExit;
        view.cargoVisible = true;
        view.cargo = cargoOrigin;
        scout_.cargoFrom = scout_.cargoTo = cargoOrigin;
        setObjective("REACH THE TRANSFER RING", "LOOK AT THE LEFT CHUTE AND SIGNAL TO ROUTE THE CRATE");
        setRadio(Line::ScoutCargoCue, 3.6f, host, false, RadioStory);
        break;
    case OperationStage::ScoutTransfer:
        squad({{MissionRole::AlienTrooper, {-8.3f, 0, 4.8f}, pi},
               {MissionRole::AlienTrooper, {8.9f, 0, 2.2f}, pi},
               {MissionRole::AlienDrone, {1.9f, 2.55f, 10.5f}, pi}}, 13);
        if (!legacyScout_) squad({{MissionRole::AlienTrooper, {-3.8f, 0, 8.5f}, pi}}, 13);
        view.cargoVisible = true;
        view.cargo = {0, .2f, -7.5f};
        scout_.cargoFrom = scout_.cargoTo = view.cargo;
        view.chuteActive = false;
        operationGoal_ = transponder;
        op.kit = transponder;
        op.kitVisible = true;
        setObjective("ROUTE THE SCOUT COUPLER", "SIGNAL THE PORT OR STARBOARD CHUTE");
        break;
    case OperationStage::ScoutBridge:
        squad({{MissionRole::AlienTrooper, {-4.8f, 0, 21.1f}, pi, 1},
               {MissionRole::AlienDrone, {6.2f, 2.7f, 25.f}, pi}}, 14);
        if (!legacyScout_) squad({{MissionRole::AlienTrooper, {5.9f, 0, 17.4f}, pi}}, 14);
        view.cargoVisible = view.chuteActive = false;
        operationGoal_ = helm;
        op.kit = helm;
        op.kitVisible = true;
        op.ship = beamStart;
        op.clearing = scout::clearing;
        op.saddle = saddle;
        op.clearingRadius = scout::clearingRadius;
        op.saddleRadius = 5.f;
        state_.flagshipBeam = legacyScout_;
        setObjective("TAKE THE BRIDGE", "REACH THE HELM / LOOK THROUGH THE GLASS");
        setRadio(legacyScout_ ? Line::ScoutBridgeCue : Line::ArrayBridge, 3.6f, host, false, RadioStory);
        break;
    case OperationStage::ScoutReclaim:
        operationGoal_ = releaseWinch(scout_.port);
        op.kit = operationGoal_;
        op.kitVisible = true;
        if (!legacyScout_) {
            operation_.drops = 0;
            op.lock = 1.f;
            view.response = 1.f;
            view.autoReturn = true;
            setObjective("AUTO-RETURN ENGAGED", scout_.port ? "CUT THE LINK AT THE PORT WINCH" :
                                                             "CUT THE LINK AT THE STARBOARD WINCH");
            dismissRadio(host);
            setRadio(Line::ArrayAutoReturn, 4.3f, host, false, RadioUrgent);
            host.playSound(Sound::AlienHorn, .85f, false, {});
            break;
        }
        op.ship = saddle;
        op.clearing = scout::clearing;
        op.saddle = saddle;
        op.clearingRadius = scout::clearingRadius;
        op.saddleRadius = 5.f;
        state_.flagshipBeam = true;
        op.lock = .44f;
        view.response = 1.f;
        setObjective("THE FLAGSHIP IS BOARDING", scout_.port ? "RELEASE THE PORT WINCH / THE BEAM IS DRIFTING" :
                                                           "RELEASE THE STARBOARD WINCH / THE BEAM IS DRIFTING");
        dismissRadio(host);
        setRadio(Line::ScoutReclaimCue, 3.8f, host, false, RadioUrgent);
        host.playSound(Sound::AlienHorn, .85f, false, {});
        break;
    case OperationStage::ScoutDescent:
        operationGoal_ = helm;
        op.kit = helm;
        op.kitVisible = true;
        setObjective("BRING THE SCOUT HOME", op.clearingBurned ? "LAND AT SOUTH ROAD / HOLD SIGNAL" :
                                                                   "LAND AT THE NORTH CLEARING / HOLD SIGNAL");
        dismissRadio(host);
        if (legacyScout_)
            setRadio(noraAlive_ ? Line::NoraFleetLocked : Line::DeltaFleetCommitted, 4.6f, host, !noraAlive_, RadioStory);
        setRadio(op.clearingBurned ? Line::ScoutRoadLanding : Line::ScoutClearLanding, 3.8f, host, false, RadioStory);
        break;
    case OperationStage::ScoutFlight:
        scout_.flightFrom = scout_.flightPosition = flightStart;
        scout_.chaseHeading = {};
        scout_.flightLateral = 0;
        view.beaconIndex = 0;
        view.beaconCount = legacyScout_ ? std::size(northGlide) :
            op.clearingBurned ? std::size(southValleyGlide) : std::size(northValleyGlide);
        view.beacon = approachBeacon(legacyScout_, op.clearingBurned, 0);
        operationHasGoal_ = false;
        setObjective("FLY THE SCOUT TO DUSTOFF", "STEER LEFT OR RIGHT THROUGH THE APPROACH BEACONS");
        break;
    case OperationStage::ScoutRecovery: {
        const Vec3 landing = pad(op.clearingBurned);
        const Vec3 ally = landing + Vec3{4.6f, 0, 2.8f};
        if (noraAlive_) {
            spawnCivilian(noraSlot, -1, ally, pi, uint8_t(nora));
            agents_[noraSlot].civilian = CivilianOutcome::Sheltered;
            agents_[noraSlot].goal = landing + Vec3{0, 0, 5.7f};
        }
        spawnCommander(ally + Vec3{2.2f, 0, 0}, pi);
        commanderHolds_ = true;
        commanderPost_ = landing + Vec3{1.2f, 0, 6.6f};
        operationGoal_ = landing + Vec3{0, 0, 5.7f};
        state_.fade = 1.f;
        state_.cinematic = true;
        state_.letterbox = .55f;
        setObjective("SCOUT SECURED", noraAlive_ ? "MEET NORA BY THE SCOUT" : "MEET DELTA-1 BY THE SCOUT");
        break;
    }
    default:
        break;
    }
    state_.aliensRemaining = livingAliens();
}

bool MissionController::routeScoutCargo(const Snapshot& snap, Vec3 chute, MissionHost& host) {
    const Vec3 direction = atHeight(chute, .8f) - snap.camera.position;
    if (length(direction) > 25.f || dot(normalized(direction), forward(snap.playerYaw, snap.playerPitch)) < .945f ||
        world_.blocked(snap.camera.position, atHeight(chute, .8f))) return false;
    setSignalHint("ROUTE CARGO TO THIS CHUTE");
    if (!signalEdge_ || elapsed_ < scout_.cargoAt + 1.7) return true;
    auto& view = state_.scout;
    if (operation_.cue != 0 && planarSquared(scout_.cargoTo - chute) < .01f) return true;
    scout_.cargoFrom = view.cargo;
    scout_.cargoTo = chute;
    scout_.cargoAt = elapsed_;
    scout_.diversionUntil = elapsed_ + 9.5;
    view.chuteActive = true;
    view.activeChute = chute;
    operation_.cue = 1;
    for (auto& alien : agents_) {
        if (!alien.used || alien.isDead() || alien.role != MissionRole::AlienTrooper || alien.variant != 0) continue;
        const Vec3 gun = atHeight(alien.actor.position, 1.4f);
        if (alien.alerted && !world_.blocked(gun, snap.camera.position)) continue;
        if (planar(alien.actor.position - chute) > 25.f) continue;
        alien.home = chute + Vec3{alien.actor.position.x < chute.x ? -1.1f : 1.1f, 0, 1.7f};
        alien.alerted = false;
        alien.huntAt = scout_.diversionUntil;
        alien.cargoDiversionUntil = std::min(scout_.diversionUntil, elapsed_ + 5.7);
        alien.path.clear();
        alien.repathAt = 0;
    }
    host.playSound(Sound::RayCharge, .88f, true, chute);
    setTacticalCallout("CARGO REROUTED", cyan, 2.5f);
    return true;
}

void MissionController::stepScout(Snapshot& snap, const Input& input, double dt, MissionHost& host, bool stabilizing) {
    using namespace scout;
    auto& view = state_.scout;
    auto& op = state_.operation;
    const Vec3 player = snap.player.position;
    const auto near = [&](Vec3 point, float range) { return planarSquared(player - point) <= range * range; };
    const auto looking = [&](Vec3 point, float alignment) {
        return dot(normalized(point - snap.camera.position), forward(snap.playerYaw, snap.playerPitch)) > alignment;
    };
    const auto steer = [&](Vec3 target, float speed) {
        op.ship.y = 0;
        moveShip(target, speed, dt);
        op.ship.y = beamStart.y;
    };
    const auto aimBeam = [&] {
        const Vec3 direction = forward(snap.playerYaw, snap.playerPitch);
        if (direction.y >= -.025f || direction.z <= .24f) return false;
        const float along = (beamStart.y - snap.camera.position.y) / direction.y;
        op.aim = snap.camera.position + direction * std::clamp(along, 25.f, 110.f);
        op.aim.y = beamStart.y;
        return true;
    };
    const auto checkClearing = [&] {
        if (op.clearingBurned || planar(op.ship - scout::clearing) > scout::clearingRadius) return;
        op.clearingBurned = true;
        setTacticalCallout("CLEARING LOST / SOUTH ROAD", warning, 3.4f);
        setRadio(Line::DustoffClearingBurned, 4.2f, host, true, RadioUrgent);
    };
    const auto clearCrew = [&] {
        for (auto& agent : agents_) agent = {};
        state_.plasma.fill({});
        plasmaFlights_.fill({});
        state_.tracers.fill({});
    };
    const auto releaseDistantGuards = [&](float behind) {
        for (size_t i = 4; i < agents_.size(); ++i) {
            auto& a = agents_[i];
            if (a.used && a.actor.position.z < behind &&
                world_.blocked(snap.camera.position, atHeight(a.actor.position, 1.35f))) a = {};
        }
    };
    const auto transfer = [&](OperationStage next, Vec3 at, float yaw) {
        const auto targetScene = missionScene(state_.chapter, next);
        clearCrew();
        host.useMissionScene(targetScene);
        host.transferPlayer(at, yaw);
        snap.brokenGlass = targetScene == ArenaScene::Estate ? uint64_t(28) : 0;
        enterScoutStage(next, host);
        saveOperationCheckpoint(at);
    };
    const auto& noraActor = agents_[noraSlot];
    if (noraActor.used && noraActor.isDead()) {
        failMission("NORA WAS KILLED", host);
        return;
    }
    operation_.stageTime += dt;
    const auto t = operation_.stageTime;
    state_.aliensRemaining = livingAliens();
    state_.dishPulse = std::max(0.f, state_.dishPulse - float(dt) * .6f);
    view.response = std::max(0.f, view.response - float(dt) * .45f);
    if (view.cargoVisible && scoutDeck(op.stage)) {
        const float k = ease((elapsed_ - scout_.cargoAt) / 1.7);
        if (op.stage == OperationStage::ScoutTransfer && k < 1.f) {
            constexpr float rail = -8.2f;
            const auto from = scout_.cargoFrom;
            const auto to = scout_.cargoTo;
            if (k < .22f) view.cargo = from + (Vec3{from.x, 0, rail} - from) * (k / .22f);
            else if (k < .7f) view.cargo = {from.x + (to.x - from.x) * ((k - .22f) / .48f), 0, rail};
            else view.cargo = {to.x, 0, rail + (to.z - rail) * ((k - .7f) / .3f)};
        } else {
            view.cargo = scout_.cargoFrom + (scout_.cargoTo - scout_.cargoFrom) * k;
        }
        view.cargo.y += 2.25f * std::sin(k * pi);
        if (elapsed_ >= scout_.diversionUntil) view.chuteActive = false;
    }

    switch (op.stage) {
    case OperationStage::ScoutApproach: {
        if (!legacyScout_) {
            if (operation_.cue == 0 && near(overlook, 5.f)) {
                operation_.cue = 1;
                scout_.progress = elapsed_;
                operationGoal_ = ridgeRetrieval;
                state_.holdCenter = ridgeRetrieval;
                state_.holdRadius = 2.25f;
                setObjective("BOARD THE SCOUT", "STEP INTO THE RETRIEVAL BEAM / HOLD SIGNAL");
                setRadio(Line::ArrayScoutTag, 4.2f, host, false, RadioStory);
                host.playSound(Sound::RayCharge, .62f, true, ridgeRetrieval);
            }
            if (operation_.cue == 0) break;
            const double since = elapsed_ - scout_.progress;
            const double lift = std::fmod(since, 8.);
            view.retrievalCrew = true;
            view.cargoVisible = lift < 4.2;
            view.cargo = ridgeRetrieval + Vec3{-1.2f, .7f + 19.f * ease(lift / 4.2), .15f};
            if (near(ridgeRetrieval, 2.25f) && since >= 2.2) {
                setSignalHint("HOLD SIGNAL TO RIDE THE BEAM");
                if (signalHeld_) view.capture = std::min(1.f, view.capture + float(dt));
                else view.capture = 0.f;
            } else view.capture = 0.f;
            if (view.capture >= 1.f) {
                scout_.cargoFrom = player;
                state_.holdRadius = 0;
                enterScoutStage(OperationStage::ScoutBoard, host);
            }
            break;
        }
        if (operation_.cue == 0 && near(fieldKit, 2.4f)) {
            setSignalHint("TAKE THE EMITTER");
            if (signalEdge_) {
                operation_.cue = 1;
                op.kitVisible = false;
                operationGoal_ = approach;
                setObjective("REACH THE NORTH PATIO", "BRIAR HOUSE / GET A VIEW OF THE SCOUT");
                setTacticalCallout("FIELD EMITTER SECURED", cyan, 2.3f);
            }
        }
        if (operation_.cue == 1 && near(approach, 8.f)) {
            operation_.cue = 2;
            scout_.progress = elapsed_;
            state_.dishPulse = 1.f;
            view.cargoVisible = true;
            operationGoal_ = retrieval;
            commanderHolds_ = true;
            commanderPost_ = approach;
            setObjective("BOARD THE SCOUT", "THE FLAGSHIP NEEDS THE SCOUT'S CONFIRMATION");
            setRadio(noraAlive_ ? Line::ScoutNoraRefused : Line::ScoutDeltaRefused, 4.f, host, false, RadioStory);
            setRadio(Line::ScoutDeltaCrate, 3.8f, host, false, RadioStory);
            host.playSound(Sound::RayCharge, .48f, true, retrieval);
        }
        if (operation_.cue < 2) break;
        const double since = elapsed_ - scout_.progress;
        view.cargo = retrieval + Vec3{-1.7f, (3.5f + 11.5f * ease((since - .65) / 2.4)) * ease(since / 1.3), 0};
        view.cargoVisible = since < 4.5;
        state_.holdCenter = retrieval;
        state_.holdRadius = 2.25f;
        const bool canBoard = near(retrieval, 2.25f) && since >= 3.4;
        if (canBoard) setSignalHint("HOLD TO RIDE THE BEAM");
        view.capture = canBoard && signalHeld_ ? std::min(1.f, view.capture + float(dt / .8)) : 0.f;
        if (view.capture >= 1) {
            scout_.cargoFrom = player;
            state_.holdRadius = 0;
            enterScoutStage(OperationStage::ScoutBoard, host);
        }
        break;
    }
    case OperationStage::ScoutBoard: {
        const Vec3 eye = scout_.cargoFrom + Vec3{0, 1.65f + 14.8f * ease(t / 3.8), 0};
        const Vec3 target = (legacyScout_ ? retrieval : ridgeRetrieval) + Vec3{0, 22.f, .9f};
        lookAt(snap, eye, target, 74);
        state_.fade = ease((t - 2.7) / 1.05);
        if (t >= 4.0 || (t > 1.1 && (input.edge(Jump) || input.edge(Confirm)))) {
            transfer(OperationStage::ScoutCargo, entry, 0);
            state_.fade = 1.f;
        }
        break;
    }
    case OperationStage::ScoutCargo:
        state_.fade = 1.f - ease(t / .8);
        routeScoutCargo(snap, cargoChute, host);
        if (player.z > cargoExit.z + .35f && (operation_.cue != 0 || livingAliens() == 0)) {
            enterScoutStage(OperationStage::ScoutTransfer, host);
            saveOperationCheckpoint({0, 0, -8.5f});
        }
        break;
    case OperationStage::ScoutTransfer: {
        if (operation_.cue < 2) {
            if (!routeScoutCargo(snap, portChute, host)) routeScoutCargo(snap, starboardChute, host);
            if (operation_.cue == 1) {
                operationGoal_ = view.activeChute;
                setObjective("RECOVER THE SCOUT COUPLER", "REACH THE ROUTED CRATE / SIGNAL TO TAKE IT");
                if (near(view.activeChute, 2.8f) && elapsed_ >= scout_.cargoAt + 1.7) {
                    setSignalHint("TAKE THE SCOUT COUPLER");
                    if (signalEdge_) {
                        operation_.cue = 2;
                        scout_.port = view.activeChute.x < 0;
                        operationGoal_ = transponder;
                        setObjective("PAIR WITH THE TRANSPONDER", "REACH THE FAR CONSOLE / HOLD SIGNAL");
                        setTacticalCallout("COUPLER SECURED", cyan, 2.2f);
                    }
                }
            }
            break;
        }
        const bool atTransponder = near(transponder, 2.8f);
        if (atTransponder) setSignalHint("PAIR EMITTER WITH SCOUT");
        scout_.progress = atTransponder && signalHeld_ && !signalEdge_ ? scout_.progress + dt : 0.;
        op.lock = float(std::min(1., scout_.progress / 1.8));
        if (scout_.progress >= 1.8) {
            releaseDistantGuards(5.f);
            op.lock = 0;
            enterScoutStage(OperationStage::ScoutBridge, host);
            saveOperationCheckpoint({0, 0, 13.6f});
            setTacticalCallout("TRANSPONDER PAIRED", cyan, 2.5f);
            host.playSound(Sound::RadioChirp, .8f, true, transponder);
        }
        break;
    }
    case OperationStage::ScoutBridge:
        if (legacyScout_) releaseDistantGuards(12.f);
        if (!legacyScout_) {
            if (near(helm, 3.f) && livingAliens() == 0) {
                enterScoutStage(OperationStage::ScoutReclaim, host);
                saveOperationCheckpoint(helm);
            }
            break;
        }
        op.aimValid = near(helm, 3.35f) && aimBeam();
        op.painting = op.aimValid && signalHeld_;
        if (near(helm, 3.35f)) {
            setSignalHint(op.aimValid ? "HOLD TO STEER THE LANDING BEAM" : "LOOK THROUGH THE VIEWPORT");
            if (operation_.cue == 0) {
                operation_.cue = 1;
                setObjective("ECHO THE DISH'S ORDER", "STEER ABOVE THE CLEARING AND INTO THE WEST SADDLE");
            }
        }
        if (op.painting) {
            steer(op.aim, 12.f);
            const float remaining = planar(op.ship - saddle);
            op.lock = .44f * std::clamp(1.f - remaining / 56.f, 0.f, 1.f);
            checkClearing();
        }
        if (op.painting && planar(op.ship - saddle) <= 3.4f) {
            enterScoutStage(OperationStage::ScoutReclaim, host);
            saveOperationCheckpoint(helm);
        }
        break;
    case OperationStage::ScoutReclaim: {
        if (legacyScout_) releaseDistantGuards(12.f);
        constexpr Patrol security[]{
            {MissionRole::AlienTrooper, {-3.2f, 0, 19.5f}, 0, 1},
            {MissionRole::AlienTrooper, {5.8f, 0, 17.f}, 0},
            {MissionRole::AlienDrone, {0, 2.8f, 15.7f}, 0},
        };
        if (!legacyScout_ && t > .65) {
            while (operation_.drops < int(std::size(security))) {
                const auto& p = security[operation_.drops];
                const auto crew = dropAlien(p.role, p.position, p.tier, snap, host);
                if (!crew) break;
                configureAliens(crew, [](Agent& a) {
                    a.squad = 15;
                    a.pursuer = true;
                    a.tacGoal = helm;
                });
                operation_.pursuitTrail = helm;
                ++operation_.drops;
            }
        } else if (legacyScout_ && operation_.drops == 0 && t > .65) {
            for (const auto& p : security) {
                const auto crew = dropAlien(p.role, p.position, p.tier, snap, host);
                configureAliens(crew, [](Agent& a) { a.squad = 15; });
            }
            operation_.drops = 1;
        }
        const bool blocked = operation_.drops == 0 || livingAliens() > 0;
        const bool atHelm = near(helm, 3.35f);
        if (!legacyScout_) {
            if (view.autoReturn) {
                constexpr float returnSeconds = 68.f;
                op.lock = std::max(0.f, 1.f - float(t) / returnSeconds);
                view.response = .8f + .2f * std::sin(float(t) * (op.lock < .23f ? 9.f : 4.f));
                if (op.lock < .23f && operation_.cue == 0) {
                    operation_.cue = 1;
                    setTacticalCallout("RETURN IMMINENT", warning, 2.6f);
                    host.playSound(Sound::AlienHorn, .56f, false, {});
                }
                if (op.lock <= 0.f) {
                    failMission("THE SCOUT RETURNED TO THE FLAGSHIP", host);
                } else if (near(releaseWinch(scout_.port), 2.75f)) {
                    setSignalHint("SIGNAL TO CUT RETURN LINK");
                    if (signalEdge_) {
                        view.autoReturn = false;
                        view.response = op.lock = 0;
                        op.kit = operationGoal_ = helm;
                        setObjective("CLAIM MANUAL FLIGHT", "RETURN TO THE HELM / HOLD SIGNAL");
                        setTacticalCallout("AUTO-RETURN DISABLED", cyan, 2.4f);
                        host.playSound(Sound::ShieldBreak, .7f, true, player);
                        dismissRadio(host);
                        setRadio(Line::ArrayManual, 3.7f, host, false, RadioStory);
                    }
                }
                break;
            }
            const bool claimable = !blocked && operation_.drops == int(std::size(security));
            if (atHelm) setSignalHint(claimable ? "HOLD FOR MANUAL FLIGHT" : "CLEAR THE SECURITY TEAM");
            scout_.progress = atHelm && claimable && signalHeld_ ? scout_.progress + dt : 0.;
            op.lock = std::min(1.f, float(scout_.progress / 1.5));
            if (op.lock >= 1.f) {
                enterScoutStage(OperationStage::ScoutDescent, host);
                saveOperationCheckpoint(helm);
            }
            break;
        }
        if (operation_.cue == 0 && near(releaseWinch(scout_.port), 2.75f)) {
            setSignalHint("RELEASE THE RECLAIM TETHER");
            if (signalEdge_) {
                operation_.cue = 1;
                op.kit = operationGoal_ = helm;
                view.response = 0;
                setTacticalCallout("TETHER RELEASED", cyan, 2.4f);
                host.playSound(Sound::ShieldBreak, .7f, true, player);
            }
        }
        op.aimValid = atHelm && aimBeam();
        op.painting = operation_.cue != 0 && !blocked && op.aimValid && signalHeld_;
        op.pulled = !op.painting;
        steer(op.painting ? op.aim : scout::clearing, op.painting ? 10.5f : noraAlive_ ? 1.1f : 1.3f);
        checkClearing();
        if (operation_.cue != 0 && !blocked) {
            setObjective("SEND THE FINAL CONFIRMATION", "TAKE THE HELM AND HOLD SIGNAL AT THE SADDLE");
            if (atHelm) setSignalHint(op.aimValid ? "HOLD TO FINISH THE SPOOF" : "LOOK THROUGH THE VIEWPORT");
            if (op.painting && planar(op.ship - saddle) < op.saddleRadius)
                op.lock = std::min(1.f, op.lock + float(dt / 7.2));
        }
        if (op.lock >= 1) {
            enterScoutStage(OperationStage::ScoutDescent, host);
            saveOperationCheckpoint(helm);
        }
        break;
    }
    case OperationStage::ScoutDescent:
        if (near(helm, 4.2f) && scout_.landingAt < 0) {
            const bool aligned = looking(op.clearingBurned ? roadApproach : scout::clearing, .93f);
            setSignalHint(aligned ? "SIGNAL TO BEGIN DESCENT" : "LOOK AT THE RECOVERY SITE");
            if (aligned && signalEdge_) {
                scout_.landingAt = t;
                state_.cinematic = true;
                state_.letterbox = .65f;
                host.playSound(Sound::AlienHorn, .46f, false, {});
            }
        }
        if (scout_.landingAt >= 0) {
            const double descent = t - scout_.landingAt;
            lookAt(snap, {0, 1.8f + .2f * std::sin(float(descent) * 3.4f), 26.3f}, {0, -9.3f, 69}, 72);
            state_.fade = ease((descent - 2.7) / .9);
            if (descent >= 4.1 || (descent > 1.1 && (input.edge(Jump) || input.edge(Confirm)))) {
                transfer(OperationStage::ScoutFlight, flightStart, pi);
            }
        }
        break;
    case OperationStage::ScoutFlight: {
        state_.fade = 1.f - ease(t / .65);
        const Vec3 course = view.beacon - scout_.flightFrom;
        const float range = length(course);
        const Vec3 heading = normalized(course);
        const Vec3 right = normalized(Vec3{heading.z, 0, -heading.x});
        const float wind = (stabilizing ? .65f : 2.1f) * std::sin(float(t * .83 + view.beaconIndex) + .42f);
        scout_.flightLateral = std::clamp(scout_.flightLateral + (input.moveX * 5.5f + wind) * float(dt), -9.f, 9.f);
        const bool aligned = std::abs(scout_.flightLateral) < 2.7f;
        view.flightBank += (-input.moveX * .29f - view.flightBank) * float(1. - std::exp(-4. * dt));
        const float speed = 4.8f + std::clamp(input.moveY, 0.f, 1.f) * 2.2f;
        const double limit = aligned ? range : range * .92f;
        scout_.progress = std::min(scout_.progress + dt * speed, std::max(scout_.progress, limit));
        scout_.flightPosition = scout_.flightFrom + heading * float(scout_.progress) + right * scout_.flightLateral;
        snap.player.position = scout_.flightPosition;
        view.flightError = -scout_.flightLateral;
        if (legacyScout_ || t <= dt) scout_.chaseHeading = heading;
        else scout_.chaseHeading += (heading - scout_.chaseHeading) * float(1. - std::exp(-5.4 * dt));
        const Vec3 chase = legacyScout_ ? heading : normalized(scout_.chaseHeading);
#ifndef ESP_PLATFORM
        view.flightYaw = std::atan2(chase.x, chase.z);
#endif
        const Vec3 eye = scout_.flightPosition - chase * 12.4f + Vec3{0, 7.8f, 0};
        lookAt(snap, eye, scout_.flightPosition + chase * 7.8f - Vec3{0, 2.3f, 0}, 70);
        if (scout_.progress >= range) {
            host.playSound(Sound::RadioChirp, .56f, false, {});
            if (++view.beaconIndex == view.beaconCount) {
                const Vec3 at = disembark(pad(op.clearingBurned));
                transfer(OperationStage::ScoutRecovery, at, bearing(at, pad(op.clearingBurned)));
            } else {
                scout_.flightFrom = scout_.flightPosition;
                scout_.progress = 0;
                scout_.flightLateral = 0;
                view.beacon = approachBeacon(legacyScout_, op.clearingBurned, view.beaconIndex);
            }
        } else if (scout_.progress >= range * .89f && !aligned) {
            setObjective("CENTER ON THE APPROACH BEACON", view.flightError < 0 ? "STEER LEFT" : "STEER RIGHT");
        } else {
            setObjective("FLY THE SCOUT TO DUSTOFF", "STEER THROUGH THE BEACON / FORWARD TO ACCELERATE");
        }
        break;
    }
    case OperationStage::ScoutRecovery: {
#ifndef ESP_PLATFORM
        view.landingGear = ease(t / 1.1);
#endif
        if (state_.cinematic) {
            const Vec3 landing = pad(op.clearingBurned);
            const Vec3 eye = landing + Vec3{-12.5f + 3.f * ease(t / 2.8), 3.8f, -9.6f};
            lookAt(snap, eye, landing + Vec3{0, 8.f - 5.8f * ease(t / 2.55), 0}, 65);
            state_.fade = 1.f - ease(t / .65);
            if (t >= 2.85 || (t > 1.05 && (input.edge(Jump) || input.edge(Confirm)))) {
                state_.cinematic = false;
                state_.letterbox = 0;
                const auto at = disembark(landing);
                host.transferPlayer(at, bearing(at, landing));
            }
            break;
        }
        const bool hasMet = near(operationGoal_, 3.3f);
        if (operation_.cue == 0 && hasMet) {
            operation_.cue = 1;
            operation_.lineAt = elapsed_;
            setObjective("SCOUT SECURED", "THE VALLEY IS CLEAR");
            dismissRadio(host);
            const Line reunion = !noraAlive_ ? Line::DeltaRememberNora :
                                 state_.civiliansBoarded >= 2 ? Line::NoraReuniteBoth :
                                 state_.civiliansBoarded == 1 ? Line::NoraReuniteOne : Line::NoraReuniteAlone;
            setRadio(reunion, 3.4f, host, !noraAlive_, RadioUrgent);
        }
        if (operation_.cue != 0 && !radioSpeaking()) {
            state_.fade = ease((elapsed_ - operation_.lineAt - radioReadSeconds_) / 1.3);
            if (state_.fade >= 1) {
                state_.phase = MissionPhase::Victory;
                std::snprintf(state_.epilogue, sizeof(state_.epilogue),
                              legacyScout_ ? "SCOUT RECOVERED AT %s. FEDERAL TEAM INBOUND."
                                           : "SCOUT SAFE AT %s. FLIGHT LOGS IN FEDERAL HANDS.",
                              op.clearingBurned ? "SOUTH ROAD" : "NORTH CLEARING");
                snap.match = Match::Results;
            }
        }
        break;
    }
    default:
        break;
    }
    state_.aliensRemaining = livingAliens();
    state_.dishLink = op.painting;
    publishScoutFleet();
}

void MissionController::publishScoutFleet() {
    const auto& op = state_.operation;
    state_.fleetShift = {};
    if (scoutDeck(op.stage)) {
        const float retreat = op.stage == OperationStage::ScoutDescent ? ease(operation_.stageTime / 7.) : 0.f;
        const Vec3 ship = flagshipVista + Vec3{op.ship.x * .72f - retreat * 19, -retreat * 14, retreat * 18};
        state_.fleetShift[0] = (legacyScout_ ? ship : groundedVista) - flagshipPosition;
    } else {
        if (op.stage == OperationStage::ScoutFlight) {
            state_.fleetShift[2] = scout_.flightPosition - scoutShipPosition;
            state_.fleetShift[0] = legacyScout_ ? Vec3{-110, -24, 15} : groundedValley - flagshipPosition;
            return;
        }
        const auto landed = op.stage == OperationStage::ScoutRecovery;
        const float k = landed ? ease(operation_.stageTime / 2.55) : 1.f;
        const Vec3 touchdown = pad(op.clearingBurned) +
            Vec3{legacyScout_ ? (1.f - k) * 2.8f : 0.f, 1.9f + (1.f - k) * (legacyScout_ ? 12.9f : 10.1f), 0};
        const Vec3 hover = legacyScout_ ? retrieval : ridgeRetrieval;
        state_.fleetShift[2] = (landed ? touchdown : hover + Vec3{0, 23.f, 0}) - scoutShipPosition;
        if (!legacyScout_) state_.fleetShift[0] = groundedValley - flagshipPosition;
        else if (landed) state_.fleetShift[0] = {-110, -24, 15};
    }
}

} // namespace fod::mission_detail

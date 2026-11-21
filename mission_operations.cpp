#include "mission_internal.hpp"

#include "briar_mission_data.hpp"
#include "timberline_data.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>

// Mission 2, Timberline: Nora's emitter calls the small saucer down wherever the player points.
// Mission 3, The Array: back through Briar House to the dish, and the mothership is steered off the valley.
namespace fod::mission_detail {
namespace {

using namespace game_detail;

constexpr uint16_t rgb565(unsigned r, unsigned g, unsigned b) {
    return uint16_t((r >> 3) << 11 | (g >> 2) << 5 | (b >> 3));
}
constexpr uint16_t green = rgb565(110, 234, 146), jammerPink = rgb565(255, 76, 186);
constexpr uint16_t alertRed = rgb565(255, 96, 86);

// Nora is always in the first civilian slot after Briar House.
constexpr int noraSlot = 1;

using namespace timberline;

constexpr Vec3 landingFields[]{
    {6, 0, -15}, {-12, 0, -28}, {-12, 0, 7}, {22, 0, 0}, {19, 0, 25}, {-7, 0, 28}
};
constexpr float kitArrival = 6.f;
constexpr Patrol timberSentries[]{
    {MissionRole::AlienTrooper, {-21, 0, -10}, .8f},
    {MissionRole::AlienDrone, {-19, 2.8f, -8}, .7f},
};
constexpr Patrol turnoutPatrol[]{
    {MissionRole::AlienTrooper, {1, 0, 8}, -2.4f},
    {MissionRole::AlienTrooper, {12, 0, 14}, pi},
    {MissionRole::AlienDrone, {9, 2.8f, 15}, pi},
};
constexpr Vec3 pursuitLandings[]{
    {-17, 0, -24}, {6, 0, -22}, {5, 0, -5}, {-9, 0, 4}, {11, 0, 7},
    {4, 0, 15}, {15, 0, 19}, {20, 0, 26}, {16, 0, 34}, {18, 0, 43},
    {23, 0, 48}, {13, 0, 53}, {24, 0, 57}, {15, 0, 60},
    {17, 0, 68}, {24, 0, 75}, {18, 0, 80}, {24, 0, 85},
};
constexpr int retreatLegCount = int(std::size(fallbacks));
constexpr Vec3 turnoutCruiser{3, 36, 18}, pursuitCruiser{-12, 37, 27};
constexpr double arrivalTime = 6.4, pickupTime = 7.4, departureTime = 4.8;
constexpr float baselineDistance = 9.f;
constexpr float callRange = 42.f, callClearOfKit = 9.f, callClearOfPlayer = 6.f;
constexpr float shipCruise = 30.f, shipLanding = 21.f;
constexpr double settleTime = 1.4, heavySettle = 2.6, huntDelay = 12.0;

// The Array. Start by the Warhorse wreck; the garden squad has not seen you yet.
constexpr Vec3 roadStart = mapPoint(300, 648), roadNora = mapPoint(262, 660), roadDelta = mapPoint(326, 640);
constexpr Patrol gardenSquad[]{
    {MissionRole::AlienTrooper, mapPoint(312, 420), .3f},
    {MissionRole::AlienTrooper, mapPoint(430, 426), -.4f},
    {MissionRole::AlienDrone, mapPoint(372, 434, 3.f), 0.f},
};
constexpr Patrol hallSquad[]{
    {MissionRole::AlienTrooper, mapPoint(368, 176), pi, 1},
    {MissionRole::AlienDrone, mapPoint(340, 250, 2.6f), pi},
    {MissionRole::AlienTrooper, mapPoint(302, 112), pi},
    {MissionRole::AlienTrooper, mapPoint(440, 330), pi},
};
constexpr Vec3 frontDoor = mapPoint(368, 404), rearDoor = mapPoint(368, 34);
constexpr Vec3 garageMouth = mapPoint(627, 400), eastYard = mapPoint(718, -22);
constexpr Vec3 frontEntry = mapPoint(368, 357), garageEntry = mapPoint(627, 353);
constexpr float doorZone = 4.2f;
constexpr Vec3 console = mapPoint(612, 74), consoleNora = mapPoint(594, 80), doorPost = mapPoint(368, 78);
constexpr Vec3 dishJamPost = mapPoint(612, -6, 4.6f), lastJammerPost = mapPoint(150, -300, 7.f);
constexpr Vec3 roofJammer = mapPoint(745, -15, 5.4f), testMark = mapPoint(585, -375);
constexpr Vec3 fleetRelay = mapPoint(914, -140), westRepeater = mapPoint(-222, -147);
// The beam starts over the north-east avenue. The saddle is beyond the north-west lots, past the
// clearing, so the short way there runs straight across Dustoff's landing ground.
constexpr Vec3 beamStart = mapPoint(816, -307), clearingCenter = mapPoint(210, -220), saddleCenter = mapPoint(-364, -268);
constexpr Vec3 northTurn = mapPoint(80, -520);
constexpr float clearingRadius = 7.f, saddleRadius = 9.f, dishReach = 7.f, pointerRim = 42.f;
constexpr double lockSeconds = 5.5;
constexpr const char* steerRoute = "HOLD SIGNAL / AIM NORTH OF THE CLEARING, THEN WEST";
enum ArrayDishCue { TestDish, WatchResponse, FightJammer, CopyCode, SetRepeater, DefendConsole, RelatchDish, ReachPatio };

constexpr Vec3 ground(Vec3 p) { return {p.x, 0.f, p.z}; }

} // namespace

void MissionController::startOperation(Snapshot& snap, Vec3& position, float& yaw, MissionHost& host, bool retry) {
    const bool timber = state_.chapter == MissionChapter::Timberline;
    state_.phase = MissionPhase::Operation;
    state_.introProgress = 1.f;
    operation_ = {};
    auto& op = state_.operation;
    op = {};
    op.clearing = clearingCenter;
    op.clearingRadius = clearingRadius;
    op.saddle = saddleCenter;
    op.saddleRadius = saddleRadius;
    op.ship = timber ? ground(scoutPost) : beamStart;
    op.target = op.ship;
    operation_.shipHeight = shipCruise;
    OperationStage stage = timber ? OperationStage::Arrival : OperationStage::Garden;
    if (retry) {
        restoreCheckpointStats();
        stage = checkpoint_.operationStage;
        op.calls = checkpoint_.calls;
        op.retreatLeg = checkpoint_.retreatLeg;
        operation_.firstMark = checkpoint_.firstMark;
        position = checkpoint_.operationSpawn;
    } else {
        state_.retries = retries_;
        position = timber ? dismount : roadStart;
    }

    // Who is where when this stage begins.
    Vec3 noraAt = dismount + Vec3{-1, 0, 1.5f}, deltaAt = dismount + Vec3{.4f, 0, -2};
    switch (stage) {
    case OperationStage::Call:
        noraAt = op.calls < 2 ? yardNora : quarryNora;
        deltaAt = op.calls < 2 ? yardDelta : quarryDelta;
        break;
    case OperationStage::Relocate: noraAt = yardNora; deltaAt = yardDelta; break;
    case OperationStage::Retreat:
    case OperationStage::Pickup: {
        const auto beside = [&](Vec3 offset) {
            Vec3 at = position + offset;
            if (world_.walkBlocked(at, at)) at = position;
            at.y = world_.groundHeight(at);
            return at;
        };
        noraAt = beside({-.95f, 0, -1.2f});
        deltaAt = beside({1.1f, 0, -1.4f});
        break;
    }
    case OperationStage::Garden: noraAt = roadNora; deltaAt = roadDelta; break;
    case OperationStage::Door:
    case OperationStage::Steer: noraAt = consoleNora; deltaAt = doorPost; break;
    default: break;
    }
    const Vec3 ahead = stage == OperationStage::Arrival || stage == OperationStage::Travel ? yardKit
                     : stage == OperationStage::Garden ? frontDoor
                     : stage == OperationStage::Door ? testMark
                     : stage == OperationStage::Steer ? mapPoint(368, -60)
                     : stage == OperationStage::Retreat ? fallbacks[op.retreatLeg].cover
                     : stage == OperationStage::Pickup ? pickupCover
                     : stage == OperationStage::Relocate ? quarryKit : landingFields[op.calls % std::size(landingFields)];
    yaw = std::atan2(ahead.x - position.x, ahead.z - position.z);
    if (noraAlive_) {
        spawnCivilian(noraSlot, -1, noraAt, yaw, uint8_t(nora));
        agents_[noraSlot].civilian = CivilianOutcome::Sheltered;
        agents_[noraSlot].goal = noraAt;
    }
    spawnCommander(deltaAt, yaw);
    enterStage(stage, host);
    if (retry && stage == OperationStage::Door && checkpoint_.arrayRepeater) {
        operation_.cue = DefendConsole;
        operationGoal_ = console;
        op.target = op.ship = testMark;
        op.targetSet = state_.flagshipBeam = true;
        setObjective("BREAK THE COUNTERATTACK", "RETURN TO BRIAR HOUSE / PROTECT THE CONSOLE");
    }
    if (timberlineRetreat(state_)) {
        operation_.pursuitTrail = position;
        if (stage == OperationStage::Pickup) host.equipSniper();
    }
    saveOperationCheckpoint(position);
    if (stage == OperationStage::Arrival) checkpoint_.operationStage = OperationStage::Travel;
    state_.aliensRemaining = livingAliens();
    publishFleet();
    auto atSpawn = snap;
    atSpawn.player.position = position;
    updateWaypoint(atSpawn);
}

void MissionController::saveOperationCheckpoint(Vec3 position) {
    saveCheckpoint(MissionCheckpoint::Operation);
    checkpoint_.operationSpawn = position;
    checkpoint_.operationStage = state_.operation.stage;
    checkpoint_.calls = state_.operation.calls;
    checkpoint_.retreatLeg = state_.operation.retreatLeg;
    checkpoint_.firstMark = operation_.firstMark;
    checkpoint_.clearingBurned = state_.operation.clearingBurned;
    checkpoint_.scoutPort = scout_.port;
    checkpoint_.arrayRepeater = state_.operation.stage == OperationStage::Door && operation_.cue >= DefendConsole;
}

void MissionController::enterStage(OperationStage stage, MissionHost& host) {
    auto& op = state_.operation;
    op.stage = stage;
    operation_.stageTime = 0;
    operation_.cue = 0;
    operation_.drops = 0;
    operation_.settled = 0;
    operationHasGoal_ = false;
    operationGoalDirect_ = false;
    commanderHolds_ = false;
    auto& noraA = agents_[noraSlot];
    const bool noraHere = noraA.used && !noraA.isDead();
    char title[40];
    switch (stage) {
    case OperationStage::Arrival:
        state_.flagshipBeam = false;
        state_.cinematic = state_.evacVisible = true;
        state_.evacPos = arrivalStart;
        state_.evacYaw = 0;
        state_.letterbox = .82f;
        state_.fade = 1.f;
        setObjective("TIMBERLINE LOGGING BASIN", noraHere ? "DUSTOFF PICKED NORA UP AT BRIAR HOUSE"
                                                        : "DELTA RECOVERED NORA'S NOTES AT BRIAR HOUSE");
        if (noraHere) {
            setRadio(Line::NoraBuildEmitter, 4.2f, host, false, RadioStory);
        } else {
            setRadio(Line::DeltaEmitterFound, 4.2f, host, true, RadioStory);
        }
        break;
    case OperationStage::Travel:
        state_.flagshipBeam = false;
        for (const auto& p : timberSentries) {
            const auto group = spawnAlien(p.role, -1, p.position, p.yaw, p.tier, difficulty_);
            configureAliens(group, [](Agent& a) { a.squad = 4; });
        }
        operationGoal_ = yardKit;
        operationHasGoal_ = true;
        setObjective(noraHere ? "TAKE NORA TO THE LOG YARD" : "GET TO THE LOG YARD", "CLEAR THE PATROL AT THE TIMBER SHED");
        break;
    case OperationStage::Call: {
        clearRadioQueue();
        op.kit = op.calls < 2 ? yardKit : quarryKit;
        op.kitVisible = true;
        op.targetSet = false;
        op.lock = 0.f;
        operation_.jammerSent = operation_.harasserSent = operation_.hijacked = false;
        commanderHolds_ = true;
        commanderPost_ = op.calls < 2 ? yardDelta : quarryDelta;
        if (noraHere) noraA.goal = op.calls < 2 ? yardNora : quarryNora;
        std::snprintf(title, sizeof(title), "SCOUT CALIBRATION %d/%d", op.calls + 1, OperationView::callsNeeded);
        const char* callHint = op.calls == 3 ? "HOLD SIGNAL UNTIL THE DROP COMMITS"
                             : op.calls == 1 ? "MARK OPEN GROUND AWAY FROM THE FIRST LANDING"
                             : "HOLD SIGNAL TO MARK OPEN GROUND";
        setObjective(title, callHint);
        if (op.calls == 0) {
            setRadio(noraHere ? Line::NoraEmitterHot : Line::DeltaEmitterReady,
                     4.0f, host, !noraHere, RadioStory);
        } else if (op.calls == 1) {
            setRadio(noraHere ? Line::NoraGoodMark : Line::DeltaFirstMark,
                     3.4f, host, !noraHere, RadioStory);
        } else if (op.calls == 3) {
            setRadio(noraHere ? Line::NoraFinalPass : Line::DeltaLastMark,
                     4.0f, host, !noraHere, RadioStory);
        }
        break;
    }
    case OperationStage::Relocate:
        clearRadioQueue();
        op.kitVisible = false;
        operationGoal_ = quarryKit;
        operationHasGoal_ = true;
        if (noraHere) noraA.goal = quarrySpawn;
        setObjective("MOVE THE EMITTER TO THE TURNOUT", "TWO READINGS LOGGED / MEASURE DRIFT AT THE SECOND SITE");
        setRadio(noraHere ? Line::NoraHalfway : Line::DeltaSecondMark,
                 3.2f, host, !noraHere, RadioStory);
        break;
    case OperationStage::Fight:
        clearRadioQueue();
        commanderHolds_ = true;
        commanderPost_ = op.calls <= 2 ? yardDelta : quarryDelta;
        if (op.calls == OperationView::callsNeeded) {
            setObjective("LAST READING LOGGED", operation_.hijacked ? "DRIFT MEASURED / DROP ON THE EMITTER"
                                                                    : "ENFORCER INBOUND / GET READY TO FALL BACK");
            break;
        }
        std::snprintf(title, sizeof(title), "CLEAR THE LANDING %d/%d", op.calls, OperationView::callsNeeded);
        setObjective(title, operation_.hijacked ? "DRIFT MEASURED / THEY'RE ON THE EMITTER"
                          : op.calls == 1 ? "THEY HAVEN'T SEEN YOU YET"
                          : "KEEP THE LANDING PARTY AWAY FROM THE EMITTER");
        break;
    case OperationStage::Retreat:
        op.kitVisible = op.targetSet = op.painting = false;
        state_.flagshipBeam = false;
        operationGoal_ = fallbacks[op.retreatLeg].cover;
        operationGoal_.y = world_.groundHeight(operationGoal_);
        operationHasGoal_ = true;
        operation_.nextDropAt = elapsed_;
        setObjective("FALL BACK INTO THE HILLS", "TAKE A SHOT FROM COVER / KEEP MOVING NORTH");
        host.equipSniper();
        if (op.retreatLeg == 0) {
            dismissRadio(host);
            setRadio(noraHere ? Line::NoraFullCalibration : Line::DeltaCalibrationComplete,
                     4.0f, host, !noraHere, RadioStory);
            setRadio(Line::DeltaMothershipTurning, 3.4f, host, true, RadioStory);
        }
        setTacticalCallout("M40A3 / THERMAL SCOPE", green, 3.f);
        break;
    case OperationStage::Pickup:
        operationGoal_ = pickupCover;
        operationHasGoal_ = true;
        setObjective("BOARD DUSTOFF", "DUSTOFF ON FINAL / CROUCH AT THE WALL");
        setRadio(Line::DustoffInbound, 3.3f, host, true, RadioOrder);
        break;
    case OperationStage::Departure:
        state_.cinematic = true;
        state_.letterbox = .82f;
        state_.plasma.fill({});
        state_.plasmaShock = 0;
        setObjective("DUSTOFF IS UP", "RETURNING TO BRIAR HOUSE");
        break;
    case OperationStage::Garden:
        for (const auto& p : gardenSquad) {
            const auto group = spawnAlien(p.role, -1, p.position, p.yaw, p.tier, difficulty_);
            configureAliens(group, [](Agent& a) {
                a.squad = 0;
                a.radar = 1.f;
            });
        }
        if (noraHere) noraA.goal = roadNora;
        setRadio(noraHere ? Line::NoraReturnToHouse : Line::DeltaReturnToBriar,
                 4.0f, host, !noraHere, RadioStory);
        if (legacyArray_) {
            setObjective("RETAKE BRIAR HOUSE", "FRONT GARDEN / THEY HAVEN'T SEEN US");
            setRadio(Line::DeltaGardenBreach, 3.4f, host, true, RadioStory);
        } else {
            const auto group = spawnAlien(MissionRole::AlienTrooper, -1, mapPoint(626, 374), pi, 0, difficulty_);
            configureAliens(group, [](Agent& a) { a.squad = 0; });
            setObjective("RETAKE BRIAR HOUSE", "FRONT PORTICO OR EAST GARAGE");
        }
        break;
    case OperationStage::Hall:
        discardQueuedRadio(Line::DeltaGardenBreach);
        operationGoal_ = frontDoor;
        operationHasGoal_ = true;
        setObjective("CLEAR THE GRAND HALL", legacyArray_ ? "THROUGH THE FRONT DOOR" : "SECURE THE HOUSE FOR THE DISH CREW");
        break;
    case OperationStage::Door:
        discardQueuedRadio(Line::DeltaGrandHall);
        if (noraHere) noraA.goal = consoleNora;
        op.kit = console;
        op.kitVisible = true;
        if (!legacyArray_) {
            commanderHolds_ = true;
            commanderPost_ = doorPost;
            state_.flagshipBeam = false;
            operationGoal_ = console;
            operationHasGoal_ = true;
            setObjective("CALIBRATE THE ROOF DISH", "EAST CONSOLE / FACE THE NORTH WINDOW");
            break;
        }
        operationGoal_ = rearDoor;
        operationHasGoal_ = true;
        setObjective("TAKE THE REAR DOOR", steerRoute);
        if (noraHere) setRadio(Line::NoraClearLanding, 3.6f, host, false, RadioStory);
        setRadio(Line::DeltaHoldDoor, 2.4f, host, true, RadioStory);
        break;
    case OperationStage::Steer:
        discardQueuedRadio(Line::NoraClearLanding);
        discardQueuedRadio(Line::DeltaHoldDoor);
        commanderHolds_ = true;
        commanderPost_ = doorPost;
        if (noraHere) noraA.goal = consoleNora;
        op.kit = console;
        op.kitVisible = true;
        if (!legacyArray_) {
            op.ship = testMark;
            op.targetSet = false;
            operation_.shipVelocity = {};
            state_.flagshipBeam = true;
        }
        operationGoal_ = legacyArray_ ? saddleCenter : northTurn;
        operationHasGoal_ = operationGoalDirect_ = true;
        op.dishDistance = planar(op.ship - ground(dishHub));
        operation_.nextDropAt = elapsed_ + 6.0;
        operation_.nextJammerAt = elapsed_ + 11.0;
        setObjective("MOVE THE BEAM TO THE WEST SADDLE", steerRoute);
        if (noraHere) {
            setRadio(Line::NoraDishLinked, 4.0f, host, false,
                     RadioStory);
        } else {
            setRadio(Line::DeltaDishNotes, 4.0f, host, true, RadioStory);
        }
        break;
    case OperationStage::Commit:
        commanderHolds_ = true;
        commanderPost_ = doorPost;
        recallAliens();
        state_.dishLink = false;
        op.painting = false;
        setObjective("THE FLEET IS SETTING DOWN", "BEYOND THE RIDGE");
        dismissRadio(host);
        setRadio(noraHere ? Line::NoraFleetLocked : Line::DeltaFleetCommitted,
                 3.6f, host, !noraHere, RadioUrgent);
        break;
    case OperationStage::Reunite:
        commanderHolds_ = true;
        commanderPost_ = doorPost;
        operation_.reuniteCalled = false;
        operationHasGoal_ = operationGoalDirect_ = true;
        operationGoal_ = agents_[noraHere ? noraSlot : 0].actor.position;
        setObjective("THE VALLEY IS CLEAR", noraHere ? "WAVE NORA OVER" : "SIGNAL DELTA-1 TO MOVE OUT");
        setSignalHint(noraHere ? "SIGNAL NORA" : "SIGNAL DELTA-1");
        break;
    default:
        break;
    }
}

// Where the pointer meets the ground. The Timberline emitter needs open ground in plain sight. The
// dish works on a bearing: look at the hills to send the beam out, lower to bring it closer.
bool MissionController::pointerGround(const Snapshot& snap, float range, bool generous, Vec3& out) const {
    const Vec3 eye = snap.camera.position;
    const Vec3 dir = forward(snap.camera.yaw, snap.camera.pitch);
    const float flat = planar(dir);
    if (generous) {
        float reach = range;
        if (dir.y < -.001f) reach = std::min(range, eye.y / -dir.y * flat);
        out = ground(eye) + Vec3{dir.x, 0, dir.z} * (std::max(reach, 3.f) / std::max(flat, .001f));
        return true;
    }
    if (dir.y > -.02f) return false;
    const float along = eye.y / -dir.y;
    if (along * flat > range) return false;
    if (world_.ray(eye, dir, along) < along - .35f) return false;
    out = ground(eye + dir * along);
    return true;
}

void MissionController::moveShip(Vec3 target, float speed, double dt) {
    auto& op = state_.operation;
    Vec3 to = ground(target) - op.ship;
    const float distance = planar(to);
    const Vec3 want = distance > .01f ? to * (std::min(speed, distance * 1.4f) / distance) : Vec3{};
    // A slight lag, so the ship reads as something heavy answering the call.
    operation_.shipVelocity += (want - operation_.shipVelocity) * float(1.0 - std::exp(-2.2 * dt));
    op.ship += operation_.shipVelocity * float(dt);
}

void MissionController::publishFleet() {
    const auto& op = state_.operation;
    // Outside the operation (a failure, or the results) the ships hold their last position.
    if (state_.phase != MissionPhase::Operation) return;
    state_.fleetShift = {};
    if (state_.chapter == MissionChapter::Timberline) {
        state_.fleetShift[2] = Vec3{op.ship.x, operation_.shipHeight, op.ship.z} - scoutShipPosition;
        state_.fleetShift[1] = cruiserPost - fleetHulls[1].center + operation_.cruiserShift;
        state_.fleetShift[0] = flagshipPost - flagshipPosition;
        if (op.stage == OperationStage::Retreat || op.stage == OperationStage::Pickup ||
            op.stage == OperationStage::Departure) {
            state_.fleetShift[0] += Vec3{33, 6, -47} * smooth(operation_.mothershipTime / 19.0);
        }
        return;
    }
    state_.fleetShift[0] = Vec3{op.ship.x - flagshipPosition.x, operation_.shipHeight - shipCruise,
                                op.ship.z - flagshipPosition.z};
}

void MissionController::failMission(const char* reason, MissionHost& host) {
    clearRadioQueue();
    state_.plasma.fill({});
    state_.phase = MissionPhase::Failed;
    state_.dishLink = false;
    std::snprintf(state_.failReason, sizeof(state_.failReason), "%s", reason);
    writeEpilogue();
    failDelay_ = radioLine_ == Line::DeltaNoraShot
        ? std::max(2.2, radioReadSeconds_ - (elapsed_ - radioShownAt_)) : 2.2;
    setSignalHint(nullptr);
    host.playSound(Sound::AlienHorn, .8f, false, {});
}

// The fleet calls its troops home once it commits to a landing.
void MissionController::recallAliens() {
    for (size_t i = 4; i < agents_.size(); ++i) {
        auto& a = agents_[i];
        if (!a.used || a.dead) continue;
        const Vec3 feet = ground(a.actor.position);
        addTracer(feet, feet + Vec3{0, 21.f, 0}, a.variant ? rgb565(255, 116, 163) : rgb565(64, 211, 255),
                  TracerStyle::DropBeam, 1.f);
        a.used = false;
    }
    state_.aliensRemaining = 0;
    state_.operation.jammer = -1;
}

// Find open ground in a ring around a point, in view of it and clear of the player, and drop there.
MissionController::AlienGroup MissionController::dropNear(Vec3 center, float nearRadius, float farRadius,
                                                           MissionRole role, uint8_t variant, float height, bool outdoors,
                                                           const Snapshot& snap, MissionHost& host) {
    const float start = float(rng_.draw("operation.drop.angle")) * 2.f * pi;
    Vec3 best{};
    float bestScore = -infinity;
    for (int i = 0; i < 12; ++i) {
        const float angle = start + float(i) * (pi / 6.f);
        const float radius = (nearRadius + farRadius) * .5f;
        const int node = nav_.nearest(center + Vec3{std::sin(angle) * radius, 0, std::cos(angle) * radius});
        if (!nav_.valid(node)) continue;
        const Vec3 at = nav_.node(node).position;
        const float fromCenter = planar(at - center), fromPlayer = planar(at - snap.player.position);
        if (fromCenter < nearRadius || fromCenter > farRadius || fromPlayer < 10.f) continue;
        // Behind Briar House the drops come over the rear lawn, not through the walls.
        if (outdoors && at.z < rearDoor.z + 1.2f) continue;
        const bool seen = !world_.blocked(atHeight(at, 1.4f), atHeight(center, 1.2f));
        const float score = (seen ? 10.f : 0.f) - std::abs(fromPlayer - 16.f) * .5f;
        if (score > bestScore) {
            bestScore = score;
            best = at;
        }
    }
    if (bestScore == -infinity) return {};
    if (role == MissionRole::AlienDrone) best.y += height;
    return dropAlien(role, best, variant, snap, host, state_.chapter == MissionChapter::Timberline ? 2 : -1);
}

// Who comes down on each call. Unaware drops patrol where they land, then go looking for the emitter.
// Lands the next members that fit and returns how many are still to come.
int MissionController::dropSquad(const Snapshot& snap, MissionHost& host) {
    const Vec3 center = operation_.dropPoint;
    const int call = operation_.squadCall;
    const bool aware = operation_.squadAware;
    struct Member { MissionRole role; uint8_t tier; };
    // The first call teaches; each later one brings more, and the last brings an Enforcer.
    constexpr Member none{MissionRole::Civilian, 0};
    constexpr Member squads[4][3]{
        {{MissionRole::AlienTrooper, 0}, {MissionRole::AlienTrooper, 0}, none},
        {{MissionRole::AlienTrooper, 0}, {MissionRole::AlienDrone, 0}, {MissionRole::AlienTrooper, 0}},
        {{MissionRole::AlienTrooper, 0}, {MissionRole::AlienTrooper, 0}, {MissionRole::AlienTrooper, 0}},
        {{MissionRole::AlienTrooper, 1}, {MissionRole::AlienTrooper, 0}, {MissionRole::AlienDrone, 0}},
    };
    const auto& squad = squads[std::clamp(call, 0, 3)];
    for (int& i = operation_.squadNext; i < 3; ++i) {
        const auto& member = squad[i];
        if (member.role == MissionRole::Civilian) continue;
        const float angle = float(i) * 2.1f + float(rng_.draw("operation.squad")) * .8f;
        Vec3 at = center;
        const int node = nav_.nearest(center + Vec3{std::sin(angle) * 2.4f, 0, std::cos(angle) * 2.4f});
        if (nav_.valid(node) && planar(nav_.node(node).position - center) < 4.f) at = nav_.node(node).position;
        if (member.role == MissionRole::AlienDrone) at.y += 2.6f;
        const auto group = dropAlien(member.role, at, member.tier, snap, host, 2);
        if (!group) break;
        configureAliens(group, [&](Agent& a) {
            a.squad = int8_t(call);
            a.alerted = aware;
            a.radar = aware ? 2.f : 1.f;
            if (!aware) {
                a.actor.yaw = float(rng_.draw("operation.facing")) * 2.f * pi;
                const double searchDelay = call == 0 ? huntDelay : call == 1 ? 3.4 : 5.5;
                a.huntAt = elapsed_ + searchDelay;
            } else {
                // Hijacked drops already know where the emitter is.
                a.huntAt = elapsed_;
                a.threatTimer = 8.0;
            }
        });
    }
    state_.aliensRemaining = livingAliens();
    int pending = 0;
    for (int i = operation_.squadNext; i < 3; ++i) pending += squad[i].role != MissionRole::Civilian;
    return pending;
}

// Jammer drones fly to the emitter or the dish and hold it. Returns the slot holding it, or -1.
int MissionController::updateJammers(Vec3 center, const Snapshot& snap, MissionHost& host) {
    const int last = state_.operation.jammer;
    int holding = -1;
    for (size_t i = 4; i < agents_.size(); ++i) {
        const auto& drone = agents_[i];
        if (!drone.used || drone.dead || !drone.operationJammer || drone.spawnFx > 0.f) continue;
        if (!drone.farJammer && planarSquared(drone.actor.position - center) > 6.8f * 6.8f) continue;
        if (world_.blocked(drone.actor.position, center)) continue;
        holding = int(i);
        if (snap.tick % 7 == 0) addTracer(drone.actor.position, center, jammerPink, TracerStyle::ShieldLink, .95f);
        break;
    }
    const bool noraHere = agents_[noraSlot].used && !agents_[noraSlot].isDead();
    if (last < 0 && holding >= 0) {
        host.playSound(Sound::RayCharge, .5f, true, center);
        setTacticalCallout("SIGNAL JAMMED", jammerPink, 2.1f);
        if (noraHere) {
            if (state_.chapter == MissionChapter::Timberline && !operation_.hijacked) {
                setRadio(Line::NoraEmitterJammed, 3.6f, host, false, RadioUrgent);
            } else if (state_.chapter == MissionChapter::Array) {
                setRadio(legacyArray_ && agents_[size_t(holding)].farJammer ? Line::NoraFarJammer
                                                                           : Line::NoraEastJammer,
                         3.2f, host, false, RadioUrgent);
            }
        }
    } else if (last >= 0 && holding < 0) {
        discardQueuedRadio(Line::NoraEmitterJammed);
        discardQueuedRadio(Line::NoraEastJammer);
        discardQueuedRadio(Line::NoraFarJammer);
        setTacticalCallout("SIGNAL RESTORED", green);
        host.playSound(Sound::RadioChirp, .46f, true, center);
    }
    state_.operation.jammer = int8_t(holding);
    return holding;
}

void MissionController::stepOperation(Snapshot& snap, double dt, MissionHost& host) {
    operation_.stageTime += dt;
    // Strays that wandered far from the fight and never found it are released for new drops.
    for (size_t i = 4; i < agents_.size(); ++i) {
        auto& a = agents_[i];
        if (a.used && !a.dead && !a.alerted && !a.farJammer && a.squad < 0 && a.huntAt <= 0 &&
            planarSquared(a.actor.position - snap.player.position) > 48.f * 48.f) a.used = false;
    }
    state_.aliensRemaining = livingAliens();
    state_.dishLink = false;
    if (state_.chapter == MissionChapter::Timberline) stepTimberline(snap, dt, host);
    else stepArray(snap, dt, host);
    if (scoutStage(state_.operation.stage)) publishScoutFleet();
    else publishFleet();
}

const char* MissionController::timberLandingIssue(Vec3 aim, Vec3 player, bool noraHere) const {
    const int node = nav_.nearest(aim);
    if (!nav_.valid(node) || planarSquared(nav_.node(node).position - aim) > 1.4f * 1.4f ||
        world_.walkBlocked(aim, aim) || world_.blocked(atHeight(aim, .08f), atHeight(aim, shipLanding))) {
        return "NO CLEAR GROUND";
    }
    if (planarSquared(aim - state_.operation.kit) < callClearOfKit * callClearOfKit) return noraHere ? "TOO CLOSE TO NORA" : "TOO CLOSE TO THE KIT";
    if (planarSquared(aim - player) < callClearOfPlayer * callClearOfPlayer) return "TOO CLOSE TO YOU";
    if (state_.operation.calls == 1 && planarSquared(aim - operation_.firstMark) < baselineDistance * baselineDistance)
        return "MARK AWAY FROM FIRST DROP";
    return nullptr;
}

void MissionController::updatePursuit(Snapshot& snap, MissionHost& host) {
    auto& op = state_.operation;
    const auto player = snap.player.position;
    const auto& spotter = agents_[0];
    const auto& nora = agents_[noraSlot];

    float best = infinity;
    float nearest = infinity;
    int contact = -1;
    for (int i = 4; i < int(agents_.size()); ++i) {
        auto& a = agents_[size_t(i)];
        if (!a.used || a.dead || !a.pursuer || a.spawnFx > 0) continue;
        const float range = planar(player - a.actor.position);
        if (range > 35.f && player.z - a.actor.position.z > 29.f && !a.seesPlayer &&
            dot(forward(snap.camera.yaw, snap.camera.pitch), a.actor.position - snap.camera.position) < 0.f) {
            // The player has left this squad in the basin and out of view.
            a.used = false;
            continue;
        }
        nearest = std::min(nearest, range);
        const bool drone = a.role == MissionRole::AlienDrone;
        const Vec3 aim = atHeight(a.actor.position, drone ? 0.f : 1.3f);
        if (world_.blocked(snap.camera.position, aim) || world_.blocked(atHeight(spotter.actor.position, 1.4f), aim)) continue;
        const float score = range - (drone ? 9.f : 0.f) - (i == op.spotted ? 4.f : 0.f);
        if (score < best) {
            best = score;
            contact = i;
        }
    }
    op.pursuitRange = nearest < infinity ? nearest : -1.f;
    if (contact >= 4 && contact != op.spotted && elapsed_ >= operation_.spotterReadyAt) {
        host.playSound(Sound::RadioChirp, .32f, false, {});
        operation_.spotterReadyAt = elapsed_ + 2.6;
    }
    op.spotted = int8_t(contact);
    if (contact >= 4) agents_[size_t(contact)].radar = 1.f;
    state_.aliensRemaining = livingAliens();

    const bool initialRush = operation_.pursuersSent < 3;
    const bool intercept = op.stage == OperationStage::Retreat && op.retreatLeg >= 2 &&
                           (operation_.pursuersSent % 4 == 3);
    if (elapsed_ < operation_.nextDropAt || state_.aliensRemaining >= (initialRush ? 4 : 3) * aliensPerSpawn()) return;

    // Reinforcements land along the road and ridge, occasionally cutting ahead of the squad.
    // Put them far enough out for the warning beam and the rifle's next shot to matter.
    int site = -1;
    Vec3 at{};
    best = infinity;
    for (int i = 0; i < int(std::size(pursuitLandings)); ++i) {
        auto p = pursuitLandings[i];
        const float range = planar(player - p);
        if (range < 9.f || range > 32.f || (nora.used && !nora.isDead() && planarSquared(nora.actor.position - p) < 6.f * 6.f)) continue;
        const bool ahead = p.z > player.z + 3.f;
        const float score = std::abs(range - (initialRush ? 13.f : 17.f)) +
            (ahead != intercept ? 12.f : 0.f) + (i == operation_.lastPursuitDrop ? 9.f : 0.f);
        if (score >= best) continue;
        const bool occupied = std::any_of(agents_.begin() + 4, agents_.end(), [&](const Agent& a) {
            return a.used && !a.dead && planarSquared(a.actor.position - p) < 2.5f * 2.5f;
        });
        if (occupied) continue;
        p.y = world_.groundHeight(p);
        if (world_.walkBlocked(p, p) || world_.blocked(atHeight(p, .08f), atHeight(p, 15.f))) continue;
        best = score;
        site = i;
        at = p;
    }
    // If there is nowhere to land, check again when the player has had time to move.
    operation_.nextDropAt = elapsed_ + .2;
    if (site < 0) return;
    const int roster = operation_.pursuersSent % 5;
    const MissionRole role = roster == 2 ? MissionRole::AlienDrone : MissionRole::AlienTrooper;
    if (role == MissionRole::AlienDrone) at.y += 2.8f;
    const auto group = dropAlien(role, at, 0, snap, host, 1);
    if (!group) return;
    configureAliens(group, [&](Agent& a) {
        a.pursuer = true;
        a.squad = 7;
        a.tacGoal = operation_.pursuitTrail;
    });
    if (!operation_.pursuersSent) {
        setTacticalCallout("PURSUERS ON THE ROAD", alertRed, 2.1f);
        host.playSound(Sound::AlienHorn, .68f, true, at);
    } else if (intercept && at.z > player.z + 3.f) {
        setTacticalCallout("DROP BEAM AHEAD", alertRed, 2.f);
    }
    ++operation_.pursuersSent;
    operation_.lastPursuitDrop = site;
    operation_.nextDropAt = elapsed_ + (initialRush ? .48 : 3.4 - .11 * difficulty_);
}

void MissionController::stepTimberlineRetreat(Snapshot& snap, MissionHost& host) {
    auto& op = state_.operation;
    const auto player = snap.player.position;
    while (op.retreatLeg < retreatLegCount && player.z >= fallbacks[op.retreatLeg].advanceNorthOf) {
        operation_.pursuitTrail = fallbacks[op.retreatLeg].cover;
        ++op.retreatLeg;
    }
    if (op.retreatLeg == retreatLegCount) {
        enterStage(OperationStage::Pickup, host);
        saveOperationCheckpoint(player);
        return;
    }
    updatePursuit(snap, host);
    operationGoal_ = fallbacks[op.retreatLeg].cover;
    operationGoal_.y = world_.groundHeight(operationGoal_);
    operationHasGoal_ = true;
    if (agents_[noraSlot].used && !agents_[noraSlot].isDead()) agents_[noraSlot].goal = operationGoal_;

    const bool close = op.pursuitRange >= 0.f && op.pursuitRange < 8.f;
    setObjective(op.retreatLeg == 0 ? "FALL BACK INTO THE HILLS" : "REACH THE NORTH SADDLE",
                 close ? "THEY'RE CLOSING / MOVE TO THE NEXT COVER"
                       : "TAKE A SHOT FROM COVER / KEEP MOVING NORTH");
    if (op.retreatLeg >= 3 && checkpoint_.retreatLeg < 3) {
        saveOperationCheckpoint(player);
        setTacticalCallout("RIDGE REACHED / CHECKPOINT", green, 2.f);
    }
}

void MissionController::stepTimberlinePickup(Snapshot& snap, MissionHost& host) {
    auto& nora = agents_[noraSlot];
    const bool noraHere = nora.used && !nora.isDead();
    const Vec3 player = snap.player.position;

    if (noraHere) nora.goal = boardingPoint;
    updatePursuit(snap, host);

    if (!state_.evacVisible) {
        state_.evacVisible = true;
        state_.evacPos = pickupStart;
        state_.evacYaw = pi;
        operation_.pickupTime = 0;
    }
    const bool landed = operation_.pickupTime >= pickupTime;
    operationGoal_ = landed ? boardingPoint : pickupCover;
    operationHasGoal_ = true;
    const float landing = smooth(operation_.pickupTime / pickupTime);
    state_.evacPos = pickupStart + (northPad - pickupStart) * landing;
    state_.evacPitch = -.12f * (1.f - landing);
    const bool nearDoor = planarSquared(player - boardingPoint) < 4.f * 4.f &&
                          std::abs(player.y - boardingPoint.y) < 1.1f;
    const bool noraReady = !noraHere || (planarSquared(nora.actor.position - boardingPoint) < 5.f * 5.f &&
                                        std::abs(nora.actor.position.y - boardingPoint.y) < 1.1f);
    setObjective("BOARD DUSTOFF", !landed ? "DUSTOFF ON FINAL / CROUCH AT THE WALL"
               : !nearDoor ? "MOVE TO THE CABIN DOOR" : !noraReady ? "WAIT FOR NORA AT THE DOOR" : "SIGNAL TO BOARD");
    if (!landed || !nearDoor || !noraReady) return;

    setSignalHint("BOARD DUSTOFF");
    if (!signalEdge_) return;
    enterStage(OperationStage::Departure, host);
    setSignalHint(nullptr);
}

void MissionController::updateTimberlineFlight(Snapshot& snap, const Input& input, double dt, MissionHost& host) {
    const bool arriving = state_.operation.stage == OperationStage::Arrival;
    const double duration = arriving ? arrivalTime : departureTime;
    operation_.stageTime += dt;
    if (!arriving) operation_.mothershipTime += dt;
    const double t = operation_.stageTime;
    const bool skip = t > 1.0 && (input.edge(Jump) || input.edge(Confirm));
    const bool talking = radioQueued_ > 0 || radioSpeaking();
    if (skip || (t >= duration && (arriving || !talking || t > duration + 4))) {
        state_.cinematic = false;
        state_.letterbox = 0;
        state_.fade = 0;
        if (arriving) {
            state_.evacPos = southPad;
            state_.evacYaw = 0;
            state_.evacPitch = 0;
            operation_.pickupTime = 0;
            const Vec3 toward = yardKit - dismount;
            snap.camera = {atHeight(dismount, snap.eyeHeight), std::atan2(toward.x, toward.z), 0, 72 * pi / 180};
            enterStage(OperationStage::Travel, host);
            updateWaypoint(snap);
        } else {
            dismissRadio(host);
            state_.evacVisible = false;
            state_.phase = MissionPhase::Victory;
            state_.fade = 1;
            writeOperationEpilogue();
            snap.match = Match::Results;
        }
        return;
    }
    const float motion = smooth(t / duration);
    const Vec3 start = arriving ? arrivalStart : northPad;
    const Vec3 finish = arriving ? southPad : pickupAway;
    state_.evacPos = start + (finish - start) * motion;
    state_.evacYaw = arriving ? 0.f : pi + std::remainder(.42f - pi, 2 * pi) * smooth(t / 1.5);
    state_.evacPitch = arriving ? .08f * (1.f - motion) : -.14f * std::sin(motion * pi);
    const Vec3 bird = state_.evacPos;
    // Follow the cabin from the port side, then come down to ground level with the player.
    const Vec3 chase = bird + (arriving ? Vec3{8.2f, 4.4f, -13.5f} : Vec3{-8, 3.7f, -7});
    const float exit = arriving ? smooth((t - duration + 1.2) / 1.2) : 0.f;
    snap.camera.position = chase + (atHeight(dismount, snap.eyeHeight) - chase) * exit;
    const Vec3 focus = (arriving ? atHeight(yardKit, 2.6f) : northPad + Vec3{15, 10, 16});
    const Vec3 look = focus - snap.camera.position;
    snap.camera.yaw = std::atan2(look.x, look.z);
    snap.camera.pitch = -std::atan2(look.y, planar(look));
    snap.camera.fov = 72 * pi / 180;
    state_.fade = arriving ? 1.f - smooth(t / .75) : smooth((t - duration + .9) / .9);
    publishFleet();
}

void MissionController::stepTimberline(Snapshot& snap, double dt, MissionHost& host) {
    auto& op = state_.operation;
    auto& noraA = agents_[noraSlot];
    const bool noraHere = noraA.used && !noraA.isDead();
    const Vec3 player = snap.player.position;
    if (noraA.used && noraA.isDead()) {
        failMission("NORA WAS KILLED", host);
        return;
    }
    // The cruiser moves between the quarry, haul road and pickup pad.
    if (op.calls >= 2) {
        const Vec3 station = timberlineRetreat(state_) ? pursuitCruiser : turnoutCruiser;
        const Vec3 to = station - (cruiserPost + operation_.cruiserShift);
        operation_.cruiserShift += to * std::min(float(dt) * .9f, float(dt) * 10.f / std::max(.01f, length(to)));
    }
    const auto climb = [&](float height) {
        operation_.shipHeight += (height - operation_.shipHeight) * float(1.0 - std::exp(-1.4 * dt));
    };
    switch (op.stage) {
    case OperationStage::Travel: {
        if (state_.evacVisible) {
            operation_.pickupTime += dt;
            const float departing = smooth((operation_.pickupTime - 1.0) / 3.6);
            state_.evacYaw = pi * smooth(operation_.pickupTime / 2.6);
            state_.evacPos = southPad + (arrivalStart - southPad) * departing;
            if (departing >= 1.f) state_.evacVisible = false;
        }
        operationGoal_ = yardKit;
        operationHasGoal_ = true;
        const bool clear = state_.aliensRemaining == 0;
        const bool arrived = planarSquared(player - yardKit) < kitArrival * kitArrival;
        if (noraHere) noraA.goal = clear ? yardNora : Vec3{-12.f, 0, -19.f};
        if (arrived && clear) {
            state_.evacVisible = false;
            enterStage(OperationStage::Call, host);
            host.refillPlayer(false);
            saveOperationCheckpoint(yardSpawn);
        } else if (arrived) {
            setObjective("CLEAR THE LOG YARD", "CLEAR THE PATROL TO SET UP THE EMITTER");
        }
        break;
    }
    case OperationStage::Relocate:
        if (operation_.drops < int(std::size(turnoutPatrol))) {
            // The last calibration squad may still occupy the slots. Introduce the turnout patrol
            // on a warning beam once the bodies clear.
            for (int& i = operation_.drops; i < int(std::size(turnoutPatrol)); ++i) {
                const auto& p = turnoutPatrol[i];
                const auto group = dropAlien(p.role, p.position, p.tier, snap, host, 1);
                if (!group) break;
                configureAliens(group, [](Agent& a) { a.squad = 5; });
            }
        }
        if (operation_.drops == int(std::size(turnoutPatrol)) && state_.aliensRemaining == 0 &&
            planarSquared(player - quarryKit) < kitArrival * kitArrival) {
            enterStage(OperationStage::Call, host);
            host.refillPlayer(false);
            saveOperationCheckpoint(quarrySpawn);
        }
        break;
    case OperationStage::Call: {
        jammerPost_ = op.kit;
        const int jammer = updateJammers(op.kit + Vec3{0, 1.2f, 0}, snap, host);
        const bool heavy = op.calls == 3;
        op.painting = signalHeld_ && jammer < 0;
        op.aimValid = false;
        const char* hint = op.targetSet ? (heavy ? "HOLD TO KEEP THE LOCK" : "SHIP INBOUND") : "HOLD TO MARK A LANDING";
        if (jammer >= 0) {
            hint = "SHOOT THE JAMMER";
        } else if (signalHeld_) {
            Vec3 aim{};
            if (!pointerGround(snap, callRange, false, aim)) {
                hint = "AIM AT OPEN GROUND";
            } else if (const char* issue = timberLandingIssue(aim, player, noraHere)) {
                hint = issue;
            } else {
                op.aim = aim;
                op.aimValid = true;
                hint = "MARKED / SHIP INBOUND";
                // A little tolerance keeps controller drift from resetting an otherwise stable lock.
                if (!op.targetSet || planarSquared(aim - op.target) > 1.25f * 1.25f) {
                    operation_.settled = 0;
                    op.target = aim;
                }
                op.targetSet = true;
            }
        }
        setSignalHint(hint);
        operationGoalDirect_ = true;
        operationHasGoal_ = jammer >= 0;
        if (jammer >= 0) {
            operationGoal_ = agents_[size_t(jammer)].actor.position - Vec3{0, 1.7f, 0};
        } else if (!op.targetSet) {
            for (size_t i = 0; i < std::size(landingFields); ++i) {
                const Vec3 spot = landingFields[(i + op.calls) % std::size(landingFields)];
                if (planarSquared(spot - player) > callRange * callRange ||
                    world_.blocked(snap.camera.position, spot) || timberLandingIssue(spot, player, noraHere))
                    continue;
                operationGoal_ = spot;
                operationHasGoal_ = true;
                break;
            }
        }

        // Where the saucer goes: the marked spot, unless something drags it to the emitter.
        Vec3 goal = op.targetSet ? op.target : ground(scoutPost);
        float speed = noraHere ? 5.f : 4.f;
        op.pulled = false;
        if (op.targetSet && jammer >= 0) {
            goal = op.kit;
            speed = 2.6f;
            op.pulled = true;
        } else if (op.targetSet && heavy && !signalHeld_) {
            // The heavy one fights the lock: let go and the spot creeps toward the emitter.
            const Vec3 toKit = op.kit - op.target;
            const float gap = planar(toKit);
            if (gap > .1f) op.target += toKit * (std::min(gap, 1.4f * float(dt)) / gap);
            goal = op.target;
            op.pulled = true;
        }
        moveShip(goal, speed, dt);
        climb(op.targetSet && planar(goal - op.ship) < 6.f ? shipLanding : shipCruise);

        if (op.targetSet && op.calls == 2 && !operation_.jammerSent) {
            const auto group = dropNear(op.kit, 10.f, 15.f, MissionRole::AlienDrone, 0, 3.0f, false, snap, host);
            if (group) agents_[group.lead].operationJammer = operation_.jammerSent = true;
        }
        if (op.targetSet && op.calls >= 2 && !operation_.harasserSent) {
            operation_.harasserSent = bool(dropNear(player, 11.f, 18.f, MissionRole::AlienDrone, 0, 2.6f, false, snap, host));
        }

        // While a jammer is up the signal is contested and nothing lands where it was called.
        const bool contested = std::any_of(agents_.begin() + 4, agents_.end(), [](const Agent& a) {
            return a.used && !a.dead && a.operationJammer;
        });
        // Dragged home: the jammer pulled the saucer onto the kit, or the heavy spot crept all the way to it.
        const bool onKit = op.targetSet && planar(op.ship - op.kit) < 2.5f &&
                           (jammer >= 0 || (heavy && !signalHeld_ && planar(op.target - op.kit) < 2.5f));
        if (op.targetSet && planar(goal - op.ship) < 1.2f && !op.pulled && !contested) operation_.settled += dt;
        else if (!onKit) operation_.settled = 0;
        const double holdTime = heavy ? heavySettle : settleTime;
        const float approach = 1.f - std::min(1.f, planar(goal - op.ship) / 24.f);
        op.lock = .65f * approach + .35f * float(std::clamp(operation_.settled / holdTime, 0.0, 1.0));
        if (op.targetSet && (onKit || operation_.settled >= holdTime)) {
            operation_.hijacked = onKit;
            Vec3 where = op.target;
            if (operation_.hijacked) {
                const int node = nav_.nearest(op.kit + normalized(player - op.kit) * 6.f);
                where = nav_.valid(node) ? nav_.node(node).position : op.kit + Vec3{6.f, 0, 0};
                setRadio(Line::DeltaLockLost, 2.8f, host, true, RadioUrgent);
            }
            operation_.dropPoint = where;
            operation_.squadCall = op.calls;
            operation_.squadNext = 0;
            operation_.squadAware = operation_.hijacked;
            if (op.calls == 0) operation_.firstMark = where;
            dropSquad(snap, host);
            ++op.calls;
            op.targetSet = false;
            op.painting = false;
            enterStage(OperationStage::Fight, host);
            if (op.calls == 1 && !operation_.hijacked) {
                setRadio(Line::DeltaUnawareLanding, 3.4f, host, true, RadioStory);
            }
        }
        break;
    }
    case OperationStage::Fight: {
        jammerPost_ = op.kit;
        updateJammers(op.kit + Vec3{0, 1.2f, 0}, snap, host);
        // The saucer climbs off the drop and drifts back over the lane.
        moveShip(operation_.stageTime > 2.5 ? ground(scoutPost) : op.ship, 3.f, dt);
        climb(shipCruise);
        operationGoal_ = operation_.dropPoint;
        operationGoalDirect_ = true;
        operationHasGoal_ = state_.aliensRemaining > 0 && planar(player - operation_.dropPoint) > 10.f;
        if (dropSquad(snap, host) > 0) break;
        if (op.calls >= OperationView::callsNeeded) {
            // The last landing rolls straight into the retreat. Survivors give chase.
            if (state_.aliensRemaining > 0 && operation_.stageTime < 1.8) break;
            op.jammer = -1;
            for (size_t i = 4; i < agents_.size(); ++i) {
                auto& a = agents_[i];
                if (!a.used || a.dead) continue;
                a.operationJammer = false;
                a.pursuer = a.alerted = true;
                a.squad = 7;
                a.tacGoal = player;
            }
            enterStage(OperationStage::Retreat, host);
            operation_.pursuitTrail = player;
            host.refillPlayer(false);
            saveOperationCheckpoint(quarrySpawn);
            break;
        }
        if (state_.aliensRemaining > 0) break;
        if (op.calls == 2) {
            enterStage(OperationStage::Relocate, host);
            saveOperationCheckpoint(yardSpawn);
        } else {
            enterStage(OperationStage::Call, host);
            saveOperationCheckpoint(player);
        }
        break;
    }
    case OperationStage::Retreat:
        operation_.mothershipTime += dt;
        moveShip(ground(scoutPost), 3.f, dt);
        climb(shipCruise);
        stepTimberlineRetreat(snap, host);
        break;
    case OperationStage::Pickup:
        operation_.mothershipTime += dt;
        if (state_.evacVisible) operation_.pickupTime += dt;
        moveShip(ground(scoutPost), 3.f, dt);
        climb(shipCruise);
        stepTimberlinePickup(snap, host);
        break;
    default:
        break;
    }
}

void MissionController::calibrateArray(Snapshot& snap, double dt, MissionHost& host, bool noraHere) {
    auto& op = state_.operation;
    const Vec3 player = snap.player.position;
    const bool nearConsole = planar(player - console) < 2.4f;
    switch (operation_.cue) {
    case TestDish:
        if (nearConsole) setSignalHint("HOLD SIGNAL TO TEST THE DISH");
        operation_.settled = nearConsole && signalHeld_ ? operation_.settled + dt : 0.;
        state_.dishLink = operation_.settled > 0.;
        if (operation_.settled < 1.4) break;
        op.target = testMark;
        op.targetSet = true;
        state_.flagshipBeam = true;
        state_.dishPulse = 1.f;
        operation_.cue = WatchResponse;
        operation_.lineAt = elapsed_;
        operationGoal_ = testMark;
        operationGoalDirect_ = true;
        setObjective("DISH TEST", "WATCH THE BEAM THROUGH THE NORTH WINDOW");
        host.playSound(Sound::RayCharge, .58f, true, dishFocus());
        break;
    case WatchResponse: {
        moveShip(testMark, 5.8f, dt);
        if (elapsed_ - operation_.lineAt < 2.3) break;
        const auto group = dropAlien(MissionRole::AlienDrone, roofJammer, 0, snap, host);
        if (!group) break;
        auto& drone = agents_[group.lead];
        drone.operationJammer = drone.farJammer = true;
        dropAlien(MissionRole::AlienTrooper, mapPoint(523, -112), 0, snap, host);
        dropAlien(MissionRole::AlienTrooper, mapPoint(596, -121), 0, snap, host);
        operation_.cue = FightJammer;
        operationGoal_ = eastYard;
        operationGoalDirect_ = false;
        setObjective("DESTROY THE ROOF JAMMER", "GET OUTSIDE FOR A SHOT AT THE EAST EAVES");
        if (!noraHere) setRadio(Line::ArrayDeltaJammer, 3.1f, host, false, RadioUrgent);
        break;
    }
    case FightJammer: {
        updateJammers(dishFocus(), snap, host);
        op.pulled = true;
        moveShip(testMark, 1.3f, dt);
        const bool jammerAlive = std::any_of(agents_.begin() + 4, agents_.end(), [](const Agent& a) {
            return a.used && !a.dead && a.operationJammer;
        });
        if (jammerAlive) break;
        operation_.cue = CopyCode;
        operation_.settled = 0.;
        operationGoal_ = op.kit = fleetRelay;
        op.pulled = false;
        setObjective("TAKE THE FLEET'S LANDING CODE", "NORTH VALLEY AVENUE / TAP THE ALIEN RELAY");
        setRadio(Line::ArrayFleetRelay, 4.3f, host, false, RadioStory);
        dropAlien(MissionRole::AlienTrooper, mapPoint(850, -192), 1, snap, host);
        dropAlien(MissionRole::AlienDrone, mapPoint(920, -96, 2.8f), 0, snap, host);
        break;
    }
    case CopyCode:
        if (planar(player - fleetRelay) > 2.65f) break;
        setSignalHint("COPY THE LANDING CODE");
        if (!signalEdge_) break;
        operation_.cue = SetRepeater;
        operationGoal_ = op.kit = westRepeater;
        setObjective("SET THE WEST REPEATER", "CROSS NORTH VALLEY AVENUE / KEEP OFF THE FLARES");
        setTacticalCallout("LANDING CODE CAPTURED", green, 2.3f);
        setRadio(Line::ArrayWestRepeater, 4.0f, host, false, RadioStory);
        host.playSound(Sound::RadioChirp, .75f, true, fleetRelay);
        break;
    case SetRepeater: {
        constexpr Patrol streetCrew[]{
            {MissionRole::AlienTrooper, mapPoint(-245, -127), pi},
            {MissionRole::AlienDrone, mapPoint(-165, -190, 3.1f), pi},
            {MissionRole::AlienTrooper, mapPoint(-217, -117), pi, 1},
        };
        const float distance = planar(player - westRepeater);
        if (distance < 34.f) {
            while (operation_.drops < int(std::size(streetCrew))) {
                const auto& p = streetCrew[operation_.drops];
                const auto group = dropAlien(p.role, p.position, p.tier, snap, host);
                if (!group) break;
                configureAliens(group, [](Agent& a) { a.squad = 3; });
                ++operation_.drops;
            }
        }
        if (distance > 2.65f) {
            operation_.settled = 0.;
            break;
        }
        setSignalHint("HOLD SIGNAL TO SET THE REPEATER");
        if (!signalHeld_) {
            operation_.settled = 0.;
            break;
        }
        operation_.settled += dt;
        if (operation_.settled < 1.7) break;
        setTacticalCallout("WEST REPEATER LIVE", green, 2.4f);
        host.refillPlayer(false);
        addTracer(atHeight(westRepeater, 1.2f), westRepeater + Vec3{0, 16.f, 0}, green, TracerStyle::ShieldLink, 1.f);
        host.playSound(Sound::RadioChirp, .8f, true, westRepeater);
        operation_.cue = DefendConsole;
        operation_.drops = 0;
        operation_.settled = 0.;
        operationGoal_ = op.kit = console;
        setObjective("BREAK THE COUNTERATTACK", "RETURN TO BRIAR HOUSE / PROTECT THE CONSOLE");
        setRadio(Line::ArrayConsoleBreach, 3.5f, host, false, RadioUrgent);
        saveOperationCheckpoint(westRepeater);
        break;
    }
    case DefendConsole: {
        constexpr Patrol breach[]{
            {MissionRole::AlienTrooper, mapPoint(358, 251), 0.f, 1},
            {MissionRole::AlienTrooper, mapPoint(543, 225), 0.f},
            {MissionRole::AlienDrone, mapPoint(494, 93, 2.5f), .7f},
        };
        if (planar(player - rearDoor) > 18.f && operation_.drops == 0) break;
        while (operation_.drops < int(std::size(breach))) {
            const auto& p = breach[operation_.drops];
            const auto group = spawnAlien(p.role, -1, p.position, p.yaw, p.tier, difficulty_);
            if (!group) break;
            configureAliens(group, [](Agent& a) {
                a.squad = arrayBreachSquad;
                a.alerted = true;
                a.radar = 2.f;
            });
            ++operation_.drops;
        }
        if (operation_.drops != int(std::size(breach)) || livingAliens() > 0) break;
        operation_.cue = noraHere ? ReachPatio : RelatchDish;
        operationGoal_ = noraHere ? rearDoor : console;
        setTacticalCallout("CONSOLE SECURE", green, 2.f);
        setObjective(noraHere ? "TAKE THE REAR PATIO" : "LATCH THE DISH LINK",
                     noraHere ? "REAR DOOR / MOVE THE BEAM WEST" : "RETURN TO THE CONSOLE AND HOLD SIGNAL");
        break;
    }
    case RelatchDish:
        if (nearConsole) setSignalHint("HOLD SIGNAL TO LATCH");
        operation_.settled = nearConsole && signalHeld_ ? operation_.settled + dt : 0.;
        if (operation_.settled < 1.2) break;
        operation_.cue = ReachPatio;
        operationGoal_ = rearDoor;
        setObjective("TAKE THE REAR PATIO", "REAR DOOR / MOVE THE BEAM WEST");
        break;
    case ReachPatio:
        if (planar(player - rearDoor) < doorZone) {
            enterStage(OperationStage::Steer, host);
            saveOperationCheckpoint(rearDoor);
        }
        break;
    }
}

bool MissionController::arrayCounterattack(Snapshot& snap, MissionHost& host, bool inSaddle) {
    // The first attack cuts across the patio. The second tries to take the doorway from the west.
    constexpr Patrol pushes[]{
        {MissionRole::AlienTrooper, mapPoint(503, -105), pi},
        {MissionRole::AlienTrooper, mapPoint(280, -104), pi},
        {MissionRole::AlienDrone, mapPoint(503, -155, 3.f), pi},
        {MissionRole::AlienTrooper, mapPoint(110, -108), pi, 1},
        {MissionRole::AlienTrooper, mapPoint(228, -119), pi},
        {MissionRole::AlienTrooper, mapPoint(340, -152), pi},
    };
    auto& count = operation_.drops;
    const bool lastPush = inSaddle || state_.operation.ship.x < mapPoint(87, 0).x;
    const int limit = lastPush || count > 3 ? 6 : operation_.stageTime > 2.0 ? 3 : 0;
    while (count < limit) {
        const auto& p = pushes[count];
        const auto group = dropAlien(p.role, p.position, p.tier, snap, host);
        if (!group) break;
        configureAliens(group, [](Agent& a) {
            a.squad = 2;
            a.alerted = a.pursuer = true;
            a.tacGoal = rearDoor;
        });
        operation_.pursuitTrail = rearDoor;
        if (count == 0 || count == 3)
            setTacticalCallout(count == 0 ? "PATIO CONTACT" : "WEST FLANK / ENFORCER", alertRed, 2.5f);
        ++count;
    }
    return count < 6 || std::any_of(agents_.begin() + 4, agents_.end(), [](const Agent& a) {
        return a.used && !a.dead && a.squad == 2;
    });
}

void MissionController::stepArray(Snapshot& snap, double dt, MissionHost& host) {
    auto& op = state_.operation;
    auto& noraA = agents_[noraSlot];
    const bool noraHere = noraA.used && !noraA.isDead();
    const Vec3 player = snap.player.position;
    if (noraA.used && noraA.isDead()) {
        failMission("NORA WAS KILLED", host);
        return;
    }
    switch (op.stage) {
    case OperationStage::Garden:
        operationGoal_ = legacyArray_ ? frontDoor :
            planarSquared(player - garageMouth) < planarSquared(player - frontDoor) ? garageEntry : frontEntry;
        operationHasGoal_ = true;
        operationGoalDirect_ = false;
        if (!legacyArray_) {
            const bool atEntry = player.z > garageMouth.z + .7f && player.z < frontEntry.z + 1.f &&
                (std::abs(player.x - garageMouth.x) < 2.0f || std::abs(player.x - frontDoor.x) < 2.8f);
            if (!atEntry) break;
            enterStage(OperationStage::Hall, host);
            for (size_t i = 4; i < agents_.size(); ++i) {
                auto& a = agents_[i];
                if (!a.used || a.dead) continue;
                a.alerted = a.pursuer = true;
                a.tacGoal = player;
            }
        } else if (state_.aliensRemaining == 0) {
            enterStage(OperationStage::Hall, host);
        } else if (operation_.cue == 0 && std::any_of(agents_.begin() + 4, agents_.end(), [](const Agent& a) {
                       return a.used && (a.alerted || a.dead);
                   })) {
            operation_.cue = 1;
            discardQueuedRadio(Line::DeltaGardenBreach);
            setObjective("RETAKE BRIAR HOUSE", "CLEAR THE FRONT GARDEN");
        }
        break;
    case OperationStage::Hall: {
        const bool inside = std::abs(player.x - mapPoint(368, 0).x) < 9.8f && player.z > mapPoint(0, 392).z &&
                            player.z < mapPoint(0, 28).z;
        if (!legacyArray_) operation_.pursuitTrail = player;
        // The hall squad comes in as the garden bodies clear, so all four always appear.
        const int squad = int(std::size(hallSquad));
        if (operation_.drops < squad && (!legacyArray_ || operation_.drops > 0 || inside || planar(player - frontDoor) < 9.f)) {
            if (operation_.drops == 0) setRadio(Line::DeltaGrandHall, 3.0f, host, true, RadioStory);
            while (operation_.drops < squad) {
                const auto& p = hallSquad[operation_.drops];
                const auto group = spawnAlien(p.role, -1, p.position, p.yaw, p.tier, difficulty_);
                if (!group) break;
                configureAliens(group, [](Agent& a) { a.squad = 1; });
                ++operation_.drops;
            }
            state_.aliensRemaining = livingAliens();
        }
        operationHasGoal_ = legacyArray_ && !inside;
        if (operation_.drops == squad && state_.aliensRemaining == 0) {
            setTacticalCallout("HALL CLEAR", green, 1.8f);
            enterStage(OperationStage::Door, host);
            if (!legacyArray_) saveOperationCheckpoint(mapPoint(560, 80));
        }
        break;
    }
    case OperationStage::Door: {
        if (!legacyArray_) {
            calibrateArray(snap, dt, host, noraHere);
            break;
        }
        const bool noraReady = !noraHere || planar(noraA.actor.position - consoleNora) < 2.f || operation_.stageTime > 20.0;
        if (planar(player - rearDoor) < doorZone && noraReady) {
            enterStage(OperationStage::Steer, host);
            saveOperationCheckpoint(rearDoor);
        }
        break;
    }
    case OperationStage::Steer: {
        jammerPost_ = dishJamPost;
        const Vec3 dish = ground(dishHub);
        const int jammer = legacyArray_ ? updateJammers(dishFocus(), snap, host) : -1;
        const bool atDoor = planar(player - rearDoor) < doorZone + (legacyArray_ ? 1.5f : 4.f);
        op.painting = signalHeld_ && jammer < 0 && atDoor;
        op.aimValid = op.painting;
        state_.dishLink = op.painting;
        if (op.painting) {
            pointerGround(snap, pointerRim, true, op.aim);
            op.target = op.aim;
            moveShip(op.target, legacyArray_ ? (noraHere ? 3.6f : 3.f) : (noraHere ? 4.f : 3.35f), dt);
        } else {
            // Left alone, the mothership homes on the strongest signal in the valley: the dish itself.
            op.target = op.ship + normalized(dish - op.ship) * 10.f;
            moveShip(op.target, legacyArray_ ? .75f : .57f, dt);
        }
        op.pulled = !op.painting;
        op.dishDistance = planar(op.ship - dish);
        if (op.dishDistance < dishReach) {
            failMission("THE FLEET FOUND THE DISH", host);
            return;
        }
        if (op.dishDistance < 14.f && !operation_.warnedDish) {
            operation_.warnedDish = true;
            setRadio(Line::DeltaBeamDrift, 2.8f, host, true, RadioUrgent);
        } else if (op.dishDistance > 19.f) {
            operation_.warnedDish = false;
        }
        if (!op.clearingBurned && planar(op.ship - clearingCenter) < clearingRadius) {
            op.clearingBurned = true;
            setObjective("MOVE THE BEAM TO THE WEST SADDLE", "HOLD SIGNAL / AIM AT THE WEST SADDLE");
            setTacticalCallout("CLEARING LOST", alertRed, 2.4f);
            setRadio(Line::DustoffClearingBurned, 3.8f, host, true, RadioUrgent);
        }
        const bool inSaddle = planar(op.ship - saddleCenter) < saddleRadius;
        if (!legacyArray_) {
            if (!op.targetSet && (op.clearingBurned || planar(op.ship - northTurn) < 2.8f)) {
                op.targetSet = true;
                operationGoal_ = saddleCenter;
                if (!op.clearingBurned)
                    setObjective("MOVE THE BEAM TO THE WEST SADDLE", "KEEP THE BEAM PAST THE FLARES / AIM WEST");
            }
            const bool underAttack = arrayCounterattack(snap, host, inSaddle);
            const bool contested = inSaddle && underAttack;
            setSignalHint(!atDoor ? "RETURN TO THE REAR PATIO" : contested ? "CLEAR THE PATIO TO LOCK" :
                          inSaddle ? "HOLD SIGNAL TO LOCK THE BEAM" :
                          op.targetSet ? "HOLD SIGNAL TO STEER" : "HOLD SIGNAL / AIM NORTH");
            if (contested) setObjective("REPEL THE LAST PUSH", "KEEP THE BEAM ABOVE THE CLEARING");
            if (inSaddle && op.painting && !underAttack) {
                const bool firstLock = operation_.cue == 0;
                operation_.cue = 1;
                if (firstLock) {
                    setObjective("LOCK THE WEST SADDLE", "HOLD SIGNAL / KEEP THE BEAM ON THE SADDLE");
                    setRadio(noraHere ? Line::NoraKeepInSaddle : Line::DeltaSaddleHold,
                             2.8f, host, !noraHere, RadioStory);
                }
                op.lock = std::min(1.f, op.lock + float(dt / 4.0));
            } else if (!inSaddle) {
                op.lock = std::max(0.f, op.lock - float(dt) * .15f);
            }
            if (op.lock >= 1.f) enterStage(OperationStage::Commit, host);
            break;
        }
        const char* jammerHint = jammer < 0 ? nullptr : agents_[size_t(jammer)].farJammer
            ? "SHOOT THE JAMMER PAST THE LAWN" : "SHOOT THE EAST ROOF JAMMER";
        setSignalHint(!atDoor ? "GET BACK TO THE REAR DOOR" : jammerHint ? jammerHint
                      : inSaddle ? "HOLD UNTIL THE BEAM LOCKS" : op.painting ? "STEERING THE BEAM" : "STEER THE BEAM");
        if (inSaddle && op.painting) op.lock = std::min(1.f, op.lock + float(dt / lockSeconds));
        else if (!inSaddle) op.lock = std::max(0.f, op.lock - float(dt) * .12f);
        // The lock does not complete until the last jammer has come for the dish.
        if (!operation_.finalJammer) op.lock = std::min(op.lock, .78f);
        if (inSaddle && operation_.cue == 0) {
            operation_.cue = 1;
            setRadio(noraHere ? Line::NoraKeepInSaddle : Line::DeltaSaddleHold,
                     2.8f, host, !noraHere, RadioStory);
        }
        // The last jammer holds the dish from across the lawn: one long shot through the doorway.
        if (op.lock >= .75f && !operation_.finalJammer) {
            const auto group = spawnAlien(MissionRole::AlienDrone, -1, lastJammerPost, pi, 0, difficulty_);
            if (group) {
                configureAliens(group, [](Agent& a) {
                    a.alerted = true;
                    a.spawnFx = arrivalDuration;
                    a.radar = 2.f;
                });
                auto& jammer = agents_[group.lead];
                jammer.operationJammer = jammer.farJammer = true;
                operation_.finalJammer = true;
            }
        }
        if (op.lock >= 1.f) {
            enterStage(OperationStage::Commit, host);
            break;
        }
        // The fleet pushes the house: riflemen and the odd Enforcer over the rear lawn, jammers for the dish.
        int riflemen = 0;
        bool jammerUp = false;
        for (size_t i = 4; i < agents_.size(); ++i) {
            const auto& a = agents_[i];
            if (!a.used || a.dead) continue;
            if (a.operationJammer) jammerUp = true;
            else if (a.role == MissionRole::AlienTrooper) ++riflemen;
        }
        // Keep space for the jammer group.
        const int limit = (2 + int(difficulty_ >= 4)) * aliensPerSpawn();
        if (elapsed_ >= operation_.nextDropAt && riflemen + aliensPerSpawn() <= limit) {
            const bool enforcer = operation_.drops % 3 == 2;
            const bool dropped =
                bool(dropNear(rearDoor, 11.f, 20.f, MissionRole::AlienTrooper, enforcer ? 1 : 0, 0.f, true, snap, host));
            if (dropped) ++operation_.drops;
            operation_.nextDropAt = elapsed_ + (dropped ? 9.0 - .3 * difficulty_ : 1.0);
        }
        if (!jammerUp && !operation_.finalJammer && elapsed_ >= operation_.nextJammerAt) {
            const auto group = dropNear(dishJamPost, 13.f, 22.f, MissionRole::AlienDrone, 0, dishJamPost.y, true, snap, host);
            if (group) agents_[group.lead].operationJammer = true;
            operation_.nextJammerAt = elapsed_ + (group ? 20.0 - .6 * difficulty_ : 2.0);
        }
        break;
    }
    case OperationStage::Commit: {
        // The beam lifts off the saddle and the mothership sinks behind the ridge.
        const float t = float(operation_.stageTime);
        moveShip(saddleCenter + (saddleCenter - rearDoor) * .25f, 1.6f, dt);
        operation_.shipHeight = shipCruise - 26.f * std::clamp((t - 1.f) / 6.f, 0.f, 1.f);
        state_.flagshipBeam = t < 3.2f;
        if (operation_.cue == 0 && t >= 7.f) {
            operation_.cue = 1;
            setRadio(op.clearingBurned ? Line::DustoffLandSouthRoad
                                       : Line::DustoffLandClearing,
                     3.6f, host, true, RadioStory);
        }
        if (t > 7.5f) {
            if (legacyArray_ || !assets_.scoutInterior) {
                enterStage(OperationStage::Reunite, host);
            } else {
                enterScoutStage(OperationStage::ScoutApproach, host);
                saveOperationCheckpoint(rearDoor);
            }
        }
        break;
    }
    case OperationStage::Reunite:
        updateReunite(snap, dt, host);
        break;
    default:
        break;
    }
}

// One last signal. This time Nora comes.
void MissionController::updateReunite(Snapshot& snap, double, MissionHost& host) {
    auto& noraA = agents_[noraSlot];
    const bool noraHere = noraA.used && !noraA.isDead();
    if (!operation_.reuniteCalled) {
        setSignalHint(noraHere ? "SIGNAL NORA" : "SIGNAL DELTA-1");
        if (!signalEdge_) return;
        operation_.reuniteCalled = true;
        operation_.lineAt = elapsed_;
        operationHasGoal_ = false;
        commanderHolds_ = false;
        return;
    }
    if (noraHere) noraA.goal = snap.player.position;
    if (operation_.cue == 0) {
        // Give Dustoff time to finish, and let Nora reach the player when she is here.
        const bool approaching = noraHere && planar(noraA.actor.position - snap.player.position) >= 2.6f;
        if ((approaching || radioQueued_ > 0 || radioSpeaking()) && elapsed_ - operation_.lineAt < 9.0) return;
        operation_.cue = 1;
        dismissRadio(host);
        const auto line = !noraHere ? Line::DeltaRememberNora
                        : state_.civiliansBoarded >= 2 ? Line::NoraReuniteBoth
                        : state_.civiliansBoarded == 1 ? Line::NoraReuniteOne
                                                     : Line::NoraReuniteAlone;
        setRadio(line, 3.2f, host, !noraHere, RadioUrgent);
        operation_.lineAt = radioShownAt_ + std::max(1.8, radioReadSeconds_);
    }
    const float fade = float(std::clamp((elapsed_ - operation_.lineAt) / 1.6, 0.0, 1.0));
    state_.fade = fade;
    state_.letterbox = .8f * fade;
    if (fade >= 1.f) {
        state_.phase = MissionPhase::Victory;
        writeEpilogue();
        snap.match = Match::Results;
    }
}

void MissionController::writeOperationEpilogue() {
    const char* line = state_.phase == MissionPhase::Failed ? "DELTA-1 CAN PULL YOU BACK TO THE LAST CHECKPOINT."
        : state_.chapter == MissionChapter::Timberline
            ? (noraAlive_ ? "NORA AND THE CALIBRATION ARE ABOARD DUSTOFF. NEXT STOP, BRIAR HOUSE."
                          : "DELTA HAS THE CALIBRATION. DUSTOFF IS HEADED BACK TO BRIAR HOUSE.")
        : state_.operation.clearingBurned ? "THE FLEET SET DOWN BEYOND THE RIDGE. DUSTOFF PULLED US OFF THE ROAD."
                                          : "THE FLEET SET DOWN BEYOND THE RIDGE. DUSTOFF BROUGHT US HOME.";
    std::snprintf(state_.epilogue, sizeof(state_.epilogue), "%s", line);
}

} // namespace fod::mission_detail

#include "mission_internal.hpp"

#include "briar_mission_data.hpp"

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

// Timberline. The room under the dish has a north window onto the avenue; the emitter goes at
// Walt's stone wall, with open lane and yards on every side for the drops.
constexpr Vec3 roomStart = mapPoint(556, 62), roomNora = mapPoint(611, 44), roomDelta = mapPoint(522, 70);
constexpr Vec3 timberKit = mapPoint(-245, 214), timberNora = mapPoint(-236, 226), timberDelta = mapPoint(-222, 206);
constexpr float kitArrival = 7.f;
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
constexpr float doorZone = 4.2f;
constexpr Vec3 console = mapPoint(612, 74), consoleNora = mapPoint(594, 80), doorPost = mapPoint(368, 78);
constexpr Vec3 dishJamPost = mapPoint(612, -6, 4.6f), lastJammerPost = mapPoint(150, -300, 7.f);
// The beam starts over the north-east avenue. The saddle is beyond the north-west lots, past the
// clearing, so the short way there runs straight across Dustoff's landing ground.
constexpr Vec3 beamStart = mapPoint(816, -307), clearingCenter = mapPoint(210, -220), saddleCenter = mapPoint(-364, -268);
constexpr float clearingRadius = 7.f, saddleRadius = 9.f, dishReach = 7.f, pointerRim = 42.f;
constexpr double lockSeconds = 5.5;

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
    op.ship = timber ? ground(scoutShipPosition) : beamStart;
    op.target = op.ship;
    operation_.shipHeight = shipCruise;
    OperationStage stage = timber ? OperationStage::Travel : OperationStage::Garden;
    if (retry) {
        restoreCheckpointStats();
        stage = checkpoint_.operationStage;
        op.calls = checkpoint_.calls;
        position = checkpoint_.operationSpawn;
    } else {
        state_.retries = retries_;
        position = timber ? roomStart : roadStart;
    }

    // Who is where when this stage begins.
    Vec3 noraAt = roomNora, deltaAt = roomDelta;
    switch (stage) {
    case OperationStage::Call: noraAt = timberNora; deltaAt = timberDelta; break;
    case OperationStage::Garden: noraAt = roadNora; deltaAt = roadDelta; break;
    case OperationStage::Steer: noraAt = consoleNora; deltaAt = doorPost; break;
    default: break;
    }
    const Vec3 ahead = stage == OperationStage::Travel ? mapPoint(611, 20)
                     : stage == OperationStage::Garden ? frontDoor
                     : stage == OperationStage::Steer ? mapPoint(368, -60) : timberKit + Vec3{-8.f, 0, 4.f};
    yaw = std::atan2(ahead.x - position.x, ahead.z - position.z);
    if (noraAlive_) {
        spawnCivilian(noraSlot, -1, noraAt, yaw, uint8_t(nora));
        agents_[noraSlot].civilian = CivilianOutcome::Sheltered;
        agents_[noraSlot].goal = noraAt;
    }
    spawnCommander(deltaAt, yaw);
    enterStage(stage, host);
    saveOperationCheckpoint(position);
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
    case OperationStage::Travel:
        setObjective(noraHere ? "TAKE NORA TO TIMBERLINE" : "GET TO TIMBERLINE", "WEST TIMBERLINE / WALT'S STONE WALL");
        if (noraHere) {
            setRadio("NORA MARSH", "LOOK AT THESE LOGS. EVERY DROP SHIP HOMED STRAIGHT ONTO A RADIO BEACON.", 4.0f, host, false, RadioStory);
            setRadio("NORA MARSH", "I RIGGED A PORTABLE EMITTER FROM SPARE RELAYS. IF IT WORKS, WE PICK WHERE THEY LAND.", 4.2f, host,
                     false, RadioStory);
            setRadio("DELTA-1", "WHERE DO WE TEST IT?", 1.8f, host, true, RadioStory);
            setRadio("NORA MARSH", "MY DAD'S PLACE IN TIMBERLINE. OPEN GROUND, AND NOBODY LEFT TO GET CAUGHT IN IT.", 4.0f, host, false, RadioStory);
        } else {
            setRadio("DELTA-1", "HER LOGS SHOW EVERY DROP SHIP HOMED ON A RADIO BEACON.", 3.6f, host, true, RadioStory);
            setRadio("DELTA-1", "SHE LEFT A RIGGED EMITTER ON THE BENCH. WE TEST IT UP AT HER DAD'S PLACE IN TIMBERLINE.", 4.2f, host,
                     true, RadioStory);
        }
        break;
    case OperationStage::Call:
        op.kit = timberKit;
        op.kitVisible = true;
        op.targetSet = false;
        operation_.jammerSent = operation_.harasserSent = operation_.hijacked = false;
        commanderHolds_ = true;
        commanderPost_ = timberDelta;
        if (noraHere) noraA.goal = timberNora;
        std::snprintf(title, sizeof(title), "CALL A SHIP DOWN %d/%d", op.calls + 1, OperationView::callsNeeded);
        setObjective(title, op.calls == 3 ? "KEEP HOLDING SIGNAL UNTIL IT LANDS" : "HOLD SIGNAL ON OPEN GROUND YOU CAN SEE");
        if (op.calls == 0) {
            setRadio(noraHere ? "NORA MARSH" : "DELTA-1",
                     noraHere ? "EMITTER'S HOT. HOLD SIGNAL ON OPEN GROUND - LET'S SEE IF THAT SCOUT BITES."
                              : "EMITTER'S LIVE. HOLD SIGNAL ON CLEAR GROUND AND SEE IF THAT SCOUT TAKES THE BAIT.",
                     4.0f, host, !noraHere, RadioStory);
        } else if (op.calls == 1) {
            setRadio(noraHere ? "NORA MARSH" : "DELTA-1",
                     noraHere ? "IT WORKED - DROPPED RIGHT ON YOUR MARK! GIVE ME ANOTHER PASS."
                              : "DROPPED RIGHT ON YOUR MARK. HER MATH HOLDS UP - DO IT AGAIN.",
                     3.4f, host, !noraHere, RadioStory);
        } else if (op.calls == 2) {
            setRadio(noraHere ? "NORA MARSH" : "DELTA-1",
                     noraHere ? "TWO DOWN, TWO TO GO. MARK ANOTHER SPOT SO I CAN LOCK THE DRIFT."
                              : "HALFWAY THERE. MARK ANOTHER ONE SO WE CAN DIAL THIS IN.",
                     3.2f, host, !noraHere, RadioStory);
        } else {
            setRadio(noraHere ? "NORA MARSH" : "DELTA-1",
                     noraHere ? "LAST PASS - IT'S FIGHTING THE SIGNAL. KEEP IT HELD OR IT'LL DRIFT ONTO US."
                              : "LAST PASS IS FIGHTING THE LOCK. KEEP SIGNAL HELD OR IT'LL PULL RIGHT TO US.",
                     4.0f, host, !noraHere, RadioStory);
        }
        break;
    case OperationStage::Fight:
        commanderHolds_ = true;
        commanderPost_ = timberDelta;
        std::snprintf(title, sizeof(title), "CLEAR THE LANDING %d/%d", op.calls, OperationView::callsNeeded);
        setObjective(title, operation_.hijacked ? "THEY CAME DOWN ON THE EMITTER" : "THEY HAVEN'T SEEN YOU YET");
        break;
    case OperationStage::Calibrated:
        commanderHolds_ = true;
        commanderPost_ = timberDelta;
        setObjective("CALIBRATION COMPLETE", "THE MOTHERSHIP IS TURNING");
        setRadio(noraHere ? "NORA MARSH" : "DELTA-1",
                 noraHere ? "GOT IT - FULL CALIBRATION. NOW WE CAN FEED THIS SPOOF TO THE ROOF DISH."
                          : "ALL FOUR PASSES LOGGED. WE'VE GOT ENOUGH TO RUN HER SPOOF ON THE MAIN DISH.",
                 4.0f, host, !noraHere, RadioStory);
        setRadio("DELTA-1", "LOOK UP - THE MOTHERSHIP JUST TURNED STRAIGHT FOR BRIAR HOUSE.", 3.4f, host, true, RadioStory);
        break;
    case OperationStage::Garden:
        for (const auto& p : gardenSquad) {
            const int slot = spawnAlien(p.role, -1, p.position, p.yaw, p.tier, difficulty_);
            if (slot < 0) continue;
            agents_[slot].squad = 0;
            agents_[slot].radar = 1.f;
        }
        if (noraHere) noraA.goal = roadNora;
        setObjective("RETAKE BRIAR HOUSE", "FRONT GARDEN / THEY HAVEN'T SEEN US");
        setRadio(noraHere ? "NORA MARSH" : "DELTA-1",
                 noraHere ? "THEY BEAT US BACK TO THE HOUSE. DISH IS STILL UP - GET ME TO THE CONSOLE."
                          : "THEY BEAT US BACK TO BRIAR HOUSE. DISH IS STILL INTACT - WE HAVE ONE SHOT.",
                 4.0f, host, !noraHere, RadioStory);
        setRadio("DELTA-1", "THEY HAVEN'T MADE US YET. CLEAR THE GARDEN AND BREACH THE FRONT DOOR.", 3.4f, host, true, RadioStory);
        break;
    case OperationStage::Hall:
        operationGoal_ = frontDoor;
        operationHasGoal_ = true;
        setObjective("CLEAR THE GRAND HALL", "THROUGH THE FRONT DOOR");
        break;
    case OperationStage::Door:
        if (noraHere) noraA.goal = consoleNora;
        op.kit = console;
        op.kitVisible = true;
        operationGoal_ = rearDoor;
        operationHasGoal_ = true;
        setObjective("TAKE THE REAR DOOR", noraHere ? "NORA IS GOING FOR THE CONSOLE" : "THE DISH NEEDS YOUR POINTER");
        if (noraHere) setRadio("NORA MARSH", "I'M HEADING TO THE CONSOLE. TAKE THE REAR DOOR SO YOU CAN SEE THE VALLEY.", 3.6f, host, false, RadioStory);
        break;
    case OperationStage::Steer:
        commanderHolds_ = true;
        commanderPost_ = doorPost;
        if (noraHere) noraA.goal = consoleNora;
        op.kit = console;
        op.kitVisible = true;
        operationGoal_ = saddleCenter;
        operationHasGoal_ = operationGoalDirect_ = true;
        op.dishDistance = planar(op.ship - ground(dishHub));
        operation_.nextDropAt = elapsed_ + 6.0;
        operation_.nextJammerAt = elapsed_ + 11.0;
        setObjective("MOVE THE BEAM TO THE WEST SADDLE", "HOLD SIGNAL / KEEP IT OFF THE CLEARING");
        if (noraHere) {
            setRadio("NORA MARSH", "DISH IS LINKED. HOLD SIGNAL TO WALK THAT BEAM OUT TO THE WEST SADDLE.", 4.0f, host, false,
                     RadioStory);
            setRadio("NORA MARSH", "KEEP IT OFF THE CLEARING - DUSTOFF NEEDS THAT GROUND TO PULL US OUT.", 3.6f, host, false, RadioStory);
        } else {
            setRadio("DELTA-1", "CONSOLE'S RUNNING OFF HER NOTES. HOLD SIGNAL TO WALK THE BEAM TO THE SADDLE.", 4.0f, host, true, RadioStory);
            setRadio("DELTA-1", "KEEP IT OFF THE CLEARING - IT'S THE ONLY LZ DUSTOFF HAS LEFT.", 3.6f, host, true, RadioStory);
        }
        setRadio("DELTA-1", "YOU STEER THE SHIP. I'LL HOLD THE DOOR.", 2.4f, host, true, RadioStory);
        break;
    case OperationStage::Commit:
        commanderHolds_ = true;
        commanderPost_ = doorPost;
        recallAliens();
        state_.dishLink = false;
        op.painting = false;
        setObjective("THE FLEET IS SETTING DOWN", "BEYOND THE RIDGE");
        clearRadioQueue();
        setRadio(noraHere ? "NORA MARSH" : "DELTA-1",
                 noraHere ? "DROP SIGNAL - IT'S LOCKED IN! THE WHOLE FLEET'S COMING DOWN PAST THE RIDGE."
                          : "DROP SIGNAL - IT'S COMMITTED! THE MOTHERSHIP'S GOING DOWN BEYOND THE RIDGE.",
                 3.6f, host, !noraHere, RadioUrgent);
        break;
    case OperationStage::Reunite:
        commanderHolds_ = true;
        commanderPost_ = doorPost;
        operation_.reuniteCalled = false;
        setObjective("THE VALLEY IS CLEAR", noraHere ? "SIGNAL NORA" : "SIGNAL DELTA-1");
        setSignalHint(noraHere ? "SIGNAL NORA" : "SIGNAL DELTA-1");
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
        if (op.stage == OperationStage::Calibrated) {
            // The mothership turns toward Briar House and edges in.
            const float t = std::clamp(float(operation_.stageTime) / 6.f, 0.f, 1.f);
            state_.fleetShift[0] = Vec3{2.f, -4.f, -9.f} * (t * t * (3.f - 2.f * t));
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
    failDelay_ = 2.2;
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
int MissionController::dropNear(Vec3 center, float nearRadius, float farRadius, MissionRole role, uint8_t variant,
                                float height, bool outdoors, const Snapshot& snap, MissionHost& host) {
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
    if (bestScore == -infinity) return -1;
    if (role == MissionRole::AlienDrone) best.y = height;
    return dropAlien(role, best, variant, snap, host);
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
        if (member.role == MissionRole::AlienDrone) at.y = 2.6f;
        const int slot = dropAlien(member.role, at, member.tier, snap, host);
        if (slot < 0) break;
        auto& a = agents_[size_t(slot)];
        a.squad = int8_t(call);
        a.alerted = aware;
        a.radar = aware ? 2.f : 1.f;
        if (!aware) {
            a.actor.yaw = float(rng_.draw("operation.facing")) * 2.f * pi;
            a.huntAt = elapsed_ + huntDelay;
        } else {
            // Hijacked drops already know where the emitter is.
            a.huntAt = elapsed_;
            a.threatTimer = 8.0;
        }
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
        // Without Nora there is nobody to read the jam off the gear; the tether is the only warning.
        if (noraHere) {
            setTacticalCallout("SIGNAL JAMMED", jammerPink, 2.1f);
            if (state_.chapter == MissionChapter::Timberline && !operation_.hijacked) {
                setRadio("NORA MARSH", "DRONE'S JAMMING THE EMITTER! SHOOT IT - IT'S PULLING THE DROP ONTO US!", 3.6f, host, false, RadioUrgent);
            } else if (state_.chapter == MissionChapter::Array) {
                setRadio("NORA MARSH", agents_[size_t(holding)].farJammer ? "ANOTHER JAMMER OUT PAST THE LAWN - SCOPE IT THROUGH THE DOOR!"
                                                                           : "JAMMER ON THE EAST ROOF - IT'S KILLING MY LINK!",
                         3.2f, host, false, RadioUrgent);
            }
        }
    } else if (last >= 0 && holding < 0) {
        if (noraHere) setTacticalCallout("SIGNAL RESTORED", green);
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
    const auto climb = [&](float height) {
        operation_.shipHeight += (height - operation_.shipHeight) * float(1.0 - std::exp(-1.4 * dt));
    };
    switch (op.stage) {
    case OperationStage::Travel: {
        operationGoal_ = timberKit;
        operationHasGoal_ = true;
        if (noraHere) noraA.goal = timberNora;
        const bool arrived = planar(player - timberKit) < kitArrival;
        if (!arrived) operation_.lineAt = 0;
        else if (operation_.lineAt <= 0) operation_.lineAt = elapsed_;
        if (noraHere && arrived && elapsed_ - operation_.lineAt > 12.0 &&
            planar(noraA.actor.position - timberNora) >= 2.5f) {
            // She took another way round; she is at the wall when the player turns back to it.
            noraA.actor.position = timberNora;
            noraA.path.clear();
        }
        const bool noraThere = !noraHere || planar(noraA.actor.position - timberNora) < 2.5f;
        if (arrived && noraThere) {
            enterStage(OperationStage::Call, host);
            saveOperationCheckpoint(player);
        }
        break;
    }
    case OperationStage::Call: {
        jammerPost_ = timberKit;
        const int jammer = updateJammers(timberKit + Vec3{0, 1.2f, 0}, snap, host);
        const bool heavy = op.calls == 3;
        op.painting = signalHeld_ && jammer < 0;
        op.aimValid = false;
        const char* hint = op.targetSet ? (heavy ? "HOLD IT ON THE SPOT" : "MOVE THE LANDING SPOT") : "MARK A LANDING SPOT";
        if (signalHeld_ && jammer >= 0) {
            hint = "THE JAMMER HAS THE EMITTER";
        } else if (signalHeld_) {
            Vec3 aim{};
            if (!pointerGround(snap, callRange, false, aim)) {
                hint = "NO CLEAR GROUND";
            } else {
                op.aim = aim;
                const int node = nav_.nearest(aim);
                if (!nav_.valid(node) || planar(nav_.node(node).position - aim) > 2.5f) hint = "NO CLEAR GROUND";
                else if (planar(aim - timberKit) < callClearOfKit) hint = noraHere ? "TOO CLOSE TO NORA" : "TOO CLOSE TO THE KIT";
                else if (planar(aim - player) < callClearOfPlayer) hint = "TOO CLOSE TO YOU";
                else {
                    op.aimValid = true;
                    hint = "MARKING";
                    if (!op.targetSet || planar(aim - op.target) > .8f) operation_.settled = 0;
                    op.target = aim;
                    op.targetSet = true;
                }
            }
        }
        setSignalHint(hint);

        // Where the saucer goes: the marked spot, unless something drags it to the emitter.
        Vec3 goal = op.targetSet ? op.target : ground(scoutShipPosition);
        float speed = noraHere ? 5.f : 4.f;
        op.pulled = false;
        if (op.targetSet && jammer >= 0) {
            goal = timberKit;
            speed = 2.6f;
            op.pulled = true;
        } else if (op.targetSet && heavy && !signalHeld_) {
            // The heavy one fights the lock: let go and the spot creeps toward the emitter.
            const Vec3 toKit = timberKit - op.target;
            const float gap = planar(toKit);
            if (gap > .1f) op.target += toKit * (std::min(gap, 1.4f * float(dt)) / gap);
            goal = op.target;
            op.pulled = true;
        }
        moveShip(goal, speed, dt);
        climb(op.targetSet && planar(goal - op.ship) < 6.f ? shipLanding : shipCruise);

        if (op.targetSet && op.calls == 2 && !operation_.jammerSent) {
            const int slot = dropNear(timberKit, 10.f, 15.f, MissionRole::AlienDrone, 0, 3.0f, false, snap, host);
            if (slot >= 0) agents_[size_t(slot)].operationJammer = operation_.jammerSent = true;
        }
        if (op.targetSet && op.calls >= 2 && !operation_.harasserSent) {
            operation_.harasserSent = dropNear(player, 11.f, 18.f, MissionRole::AlienDrone, 0, 2.6f, false, snap, host) >= 0;
        }

        // While a jammer is up the signal is contested and nothing lands where it was called.
        const bool contested = std::any_of(agents_.begin() + 4, agents_.end(), [](const Agent& a) {
            return a.used && !a.dead && a.operationJammer;
        });
        // Dragged home: the jammer pulled the saucer onto the kit, or the heavy spot crept all the way to it.
        const bool onKit = op.targetSet && planar(op.ship - timberKit) < 2.5f &&
                           (jammer >= 0 || (heavy && !signalHeld_ && planar(op.target - timberKit) < 2.5f));
        if (op.targetSet && planar(goal - op.ship) < 1.2f && !op.pulled && !contested) operation_.settled += dt;
        else if (!onKit) operation_.settled = 0;
        if (op.targetSet && (onKit || operation_.settled >= (heavy ? heavySettle : settleTime))) {
            operation_.hijacked = onKit;
            Vec3 where = op.target;
            if (operation_.hijacked) {
                const int node = nav_.nearest(timberKit + normalized(player - timberKit) * 6.f);
                where = nav_.valid(node) ? nav_.node(node).position : timberKit + Vec3{6.f, 0, 0};
                setRadio("DELTA-1", "LOST THE LOCK - THEY'RE DROPPING RIGHT ON THE KIT!", 2.8f, host, true, RadioUrgent);
            }
            operation_.dropPoint = where;
            operation_.squadCall = op.calls;
            operation_.squadNext = 0;
            operation_.squadAware = operation_.hijacked;
            dropSquad(snap, host);
            ++op.calls;
            op.targetSet = false;
            op.painting = false;
            enterStage(OperationStage::Fight, host);
            if (op.calls == 1 && !operation_.hijacked) {
                setRadio("DELTA-1", "THEY HAVE NO IDEA WE'RE HERE. HIT 'EM BEFORE THEY HUNT THE KIT.", 3.4f, host, true, RadioStory);
            }
        }
        break;
    }
    case OperationStage::Fight: {
        jammerPost_ = timberKit;
        updateJammers(timberKit + Vec3{0, 1.2f, 0}, snap, host);
        // The saucer climbs off the drop and drifts back over the lane.
        moveShip(operation_.stageTime > 2.5 ? ground(scoutShipPosition) : op.ship, 3.f, dt);
        climb(shipCruise);
        operationGoal_ = operation_.dropPoint;
        operationGoalDirect_ = true;
        operationHasGoal_ = state_.aliensRemaining > 0 && planar(player - operation_.dropPoint) > 10.f;
        if (dropSquad(snap, host) > 0 || state_.aliensRemaining > 0) break;
        if (op.calls >= OperationView::callsNeeded) {
            enterStage(OperationStage::Calibrated, host);
        } else {
            enterStage(OperationStage::Call, host);
            saveOperationCheckpoint(player);
        }
        break;
    }
    case OperationStage::Calibrated:
        moveShip(ground(scoutShipPosition), 3.f, dt);
        climb(shipCruise);
        if (operation_.stageTime >= 8.5) {
            state_.phase = MissionPhase::Victory;
            setSignalHint(nullptr);
            writeEpilogue();
            snap.match = Match::Results;
        }
        break;
    default:
        break;
    }
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
        operationGoal_ = frontDoor;
        operationHasGoal_ = true;
        operationGoalDirect_ = false;
        if (state_.aliensRemaining == 0) enterStage(OperationStage::Hall, host);
        break;
    case OperationStage::Hall: {
        const bool inside = std::abs(player.x - mapPoint(368, 0).x) < 9.8f && player.z > mapPoint(0, 392).z &&
                            player.z < mapPoint(0, 28).z;
        // The hall squad comes in as the garden bodies clear, so all four always appear.
        const int squad = int(std::size(hallSquad));
        if (operation_.drops < squad && (operation_.drops > 0 || inside || planar(player - frontDoor) < 9.f)) {
            if (operation_.drops == 0) setRadio("DELTA-1", "SAME HALL I PULLED YOU INTO AN HOUR AGO. CLEAR IT OUT.", 3.0f, host, true, RadioStory);
            while (operation_.drops < squad) {
                const auto& p = hallSquad[operation_.drops];
                const int slot = spawnAlien(p.role, -1, p.position, p.yaw, p.tier, difficulty_);
                if (slot < 0) break;
                agents_[size_t(slot)].squad = 1;
                ++operation_.drops;
            }
            state_.aliensRemaining = livingAliens();
        }
        operationHasGoal_ = !inside;
        if (operation_.drops == squad && state_.aliensRemaining == 0) {
            setTacticalCallout("HALL CLEAR", green, 1.8f);
            enterStage(OperationStage::Door, host);
        }
        break;
    }
    case OperationStage::Door: {
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
        const int jammer = updateJammers(dishFocus(), snap, host);
        const bool atDoor = planar(player - rearDoor) < doorZone + 1.5f;
        op.painting = signalHeld_ && jammer < 0 && atDoor;
        op.aimValid = op.painting;
        setSignalHint(!atDoor ? "GET BACK TO THE REAR DOOR" : jammer >= 0 ? "SOMETHING IS ON THE DISH"
                      : op.painting ? "STEERING THE BEAM" : "STEER THE BEAM");
        state_.dishLink = op.painting;
        if (op.painting) {
            pointerGround(snap, pointerRim, true, op.aim);
            op.target = op.aim;
            moveShip(op.target, noraHere ? 3.6f : 3.0f, dt);
        } else {
            // Left alone, the mothership homes on the strongest signal in the valley: the dish itself.
            op.target = op.ship + normalized(dish - op.ship) * 10.f;
            moveShip(op.target, .75f, dt);
        }
        op.pulled = !op.painting;
        op.dishDistance = planar(op.ship - dish);
        if (op.dishDistance < dishReach) {
            failMission("THE FLEET FOUND THE DISH", host);
            return;
        }
        if (op.dishDistance < 14.f && !operation_.warnedDish) {
            operation_.warnedDish = true;
            setRadio("DELTA-1", "THE BEAM'S DRIFTING BACK ONTO THE HOUSE - PULL IT WEST!", 2.8f, host, true, RadioUrgent);
        } else if (op.dishDistance > 19.f) {
            operation_.warnedDish = false;
        }
        if (!op.clearingBurned && planar(op.ship - clearingCenter) < clearingRadius) {
            op.clearingBurned = true;
            setTacticalCallout("CLEARING LOST", alertRed, 2.4f);
            setRadio("DUSTOFF", "THE CLEARING JUST WENT UP IN FLAMES, DELTA! HOLD TIGHT - WE'LL FIND ANOTHER PAD.", 3.8f, host, true, RadioUrgent);
        }
        const bool inSaddle = planar(op.ship - saddleCenter) < saddleRadius;
        if (inSaddle && op.painting) op.lock = std::min(1.f, op.lock + float(dt / lockSeconds));
        else if (!inSaddle) op.lock = std::max(0.f, op.lock - float(dt) * .12f);
        // The lock does not complete until the last jammer has come for the dish.
        if (!operation_.finalJammer) op.lock = std::min(op.lock, .78f);
        if (inSaddle && operation_.cue == 0) {
            operation_.cue = 1;
            setRadio(noraHere ? "NORA MARSH" : "DELTA-1",
                     noraHere ? "RIGHT IN THE SADDLE! HOLD IT THERE UNTIL IT LOCKS!"
                              : "YOU'VE GOT IT IN THE SADDLE - KEEP IT PINNED!",
                     2.8f, host, !noraHere, RadioStory);
        }
        // The last jammer holds the dish from across the lawn: one long shot through the doorway.
        if (op.lock >= .75f && !operation_.finalJammer) {
            const int slot = spawnAlien(MissionRole::AlienDrone, -1, lastJammerPost, pi, 0, difficulty_);
            if (slot >= 0) {
                auto& drone = agents_[size_t(slot)];
                drone.alerted = true;
                drone.operationJammer = drone.farJammer = true;
                drone.spawnFx = arrivalDuration;
                drone.radar = 2.f;
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
        // Three at most, so a slot is always left for the jammers.
        const int limit = 2 + int(difficulty_ >= 4);
        if (elapsed_ >= operation_.nextDropAt && riflemen < limit) {
            const bool enforcer = operation_.drops % 3 == 2;
            const bool dropped =
                dropNear(rearDoor, 11.f, 20.f, MissionRole::AlienTrooper, enforcer ? 1 : 0, 0.f, true, snap, host) >= 0;
            if (dropped) ++operation_.drops;
            operation_.nextDropAt = elapsed_ + (dropped ? 9.0 - .3 * difficulty_ : 1.0);
        }
        if (!jammerUp && !operation_.finalJammer && elapsed_ >= operation_.nextJammerAt) {
            const int slot = dropNear(dishJamPost, 13.f, 22.f, MissionRole::AlienDrone, 0, dishJamPost.y, true, snap, host);
            if (slot >= 0) agents_[size_t(slot)].operationJammer = true;
            operation_.nextJammerAt = elapsed_ + (slot >= 0 ? 20.0 - .6 * difficulty_ : 2.0);
        }
        break;
    }
    case OperationStage::Commit: {
        // The beam lifts off the saddle and the mothership sinks behind the ridge.
        const float t = float(operation_.stageTime);
        moveShip(saddleCenter + (saddleCenter - rearDoor) * .25f, 1.6f, dt);
        operation_.shipHeight = shipCruise - 26.f * std::clamp((t - 1.f) / 6.f, 0.f, 1.f);
        state_.flagshipBeam = t < 3.2f;
        if (operation_.cue == 0 && t > 3.6f) {
            operation_.cue = 1;
            setRadio("DUSTOFF",
                     op.clearingBurned ? "SKIES ARE CLEAR, DELTA. CLEARING'S TORCHED, SO WE'RE SETTING DOWN ON SOUTH ROAD."
                                       : "FLEET'S DOWN PAST THE RIDGE, DELTA. DUSTOFF-1 IS SWINGING BACK TO THE CLEARING.",
                     3.6f, host, true, RadioStory);
        }
        if (t > 7.5f) enterStage(OperationStage::Reunite, host);
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
    auto& cmd = agents_[0];
    if (!operation_.reuniteCalled) {
        setSignalHint(noraHere ? "SIGNAL NORA" : "SIGNAL DELTA-1");
        if (!signalEdge_) return;
        operation_.reuniteCalled = true;
        operation_.lineAt = elapsed_;
        commanderHolds_ = false;
        return;
    }
    if (noraHere) noraA.goal = snap.player.position;
    const float gap = noraHere ? planar(noraA.actor.position - snap.player.position)
                               : planar(cmd.actor.position - snap.player.position);
    if (operation_.cue == 0 && (gap < 2.6f || elapsed_ - operation_.lineAt > 9.0)) {
        operation_.cue = 1;
        operation_.lineAt = elapsed_;
        clearRadioQueue();
        if (noraHere) {
            const char* line = state_.civiliansBoarded >= 2 ? "IT WORKED. COME ON - DAD AND BEN ARE WAITING FOR US."
                             : state_.civiliansBoarded == 1 ? "IT WORKED. LET'S GO - WE STILL HAVE FAMILY WAITING."
                                                            : "IT WORKED. THEY'RE GONE... LET'S GET OUT OF HERE.";
            setRadio("NORA MARSH", line, 3.2f, host, false, RadioStory);
        } else {
            setRadio("DELTA-1", "SHE PULLED IT OFF... EVEN AFTER WE LOST HER. LET'S GO HOME.", 3.2f, host, true, RadioStory);
        }
    }
    if (operation_.cue == 1) {
        const float fade = float(std::clamp((elapsed_ - operation_.lineAt - 1.8) / 1.6, 0.0, 1.0));
        state_.fade = fade;
        state_.letterbox = .8f * fade;
        if (fade >= 1.f) {
            state_.phase = MissionPhase::Victory;
            writeEpilogue();
            snap.match = Match::Results;
        }
    }
}

void MissionController::writeOperationEpilogue() {
    const char* line = state_.phase == MissionPhase::Failed ? "DELTA-1 CAN PULL YOU BACK TO THE LAST CHECKPOINT."
        : state_.chapter == MissionChapter::Timberline
            ? (noraAlive_ ? "NORA HAS HER CALIBRATION. THE MOTHERSHIP IS TURNING ON BRIAR HOUSE."
                          : "HER CALIBRATION HELD. THE MOTHERSHIP IS TURNING ON BRIAR HOUSE.")
        : state_.operation.clearingBurned ? "THE FLEET SET DOWN BEYOND THE RIDGE. DUSTOFF PULLED US OFF THE ROAD."
                                          : "THE FLEET SET DOWN BEYOND THE RIDGE. DUSTOFF BROUGHT US HOME.";
    std::snprintf(state_.epilogue, sizeof(state_.epilogue), "%s", line);
}

} // namespace fod::mission_detail

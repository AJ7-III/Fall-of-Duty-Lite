#pragma once

#include "assets.hpp"
#include "game.hpp"

#include <algorithm>
#include <array>
#include <cstdint>

namespace fod {

enum class MissionRole : uint8_t { Commander, AlienTrooper, AlienDrone, Civilian };
enum class MissionPhase : uint8_t {
    Intro, Overwatch, Breach, Regroup, Rescue, Uplink, Incoming, Extraction, Victory, Failed, Operation,
    Boarding // Dustoff is down in the clearing and the family waits for the player's signal.
};
enum class MissionCheckpoint : uint8_t { None, Overwatch, Rescue, Uplink, Extraction, Operation };
enum class MissionChapter : uint8_t { BriarHouse, Timberline, Array };
// Stayed: Nora refused Dustoff to stay with her dish. She counts as saved and carries into the later missions.
enum class CivilianOutcome : uint8_t { Pinned, Escorting, Evacuating, Sheltered, Boarded, Lost, Stayed };
enum class TracerStyle : uint8_t { Ballistic, Scanner, DropBeam, ShieldLink, PlasmaImpact };

constexpr const char* missionName(MissionChapter chapter) {
    switch (chapter) {
    case MissionChapter::BriarHouse: return "BRIAR HOUSE";
    case MissionChapter::Timberline: return "TIMBERLINE";
    case MissionChapter::Array: return "THE ARRAY";
    }
    return "";
}

constexpr bool civilianRescued(CivilianOutcome outcome) {
    return outcome == CivilianOutcome::Evacuating || outcome == CivilianOutcome::Sheltered ||
           outcome == CivilianOutcome::Boarded || outcome == CivilianOutcome::Stayed;
}

// Timberline and The Array: the stage of the mission and the ship the player moves with Signal.
enum class OperationStage : uint8_t {
    Travel, Call, Fight, Calibrated,     // Timberline
    Garden, Hall, Door, Steer, Commit, Reunite // The Array
};

struct OperationView {
    OperationStage stage = OperationStage::Travel;
    Vec3 kit{};               // Nora's emitter kit at Timberline, or the console at Briar House
    bool kitVisible = false;
    Vec3 ship{};              // Ground point under the ship the player is calling
    Vec3 target{};            // Where that ship is heading
    Vec3 aim{};               // Ground under the pointer while Signal is held
    bool painting = false;    // Signal is held and the pointer is live
    bool aimValid = false;    // The pointer is on ground the ship can use
    bool targetSet = false;   // Timberline: a landing spot is marked
    bool pulled = false;      // The ship is being dragged toward Nora or the dish
    int8_t jammer = -1;       // Actor slot of the drone holding the emitter or dish
    uint8_t calls = 0;        // Timberline: drops landed where they were called
    static constexpr uint8_t callsNeeded = 4;
    Vec3 clearing{}, saddle{};
    float clearingRadius = 0, saddleRadius = 0;
    float lock = 0;           // The Array: 0..1 hold on the saddle
    float dishDistance = 0;   // The Array: metres from the beam to the dish
    bool clearingBurned = false;
};

constexpr float plasmaRadius = .12f, plasmaShockDuration = .7f;
constexpr uint16_t chargedPlasmaTint = 0xf9e3; // Hot orange in RGB565.
constexpr float arrivalDuration = .75f;
constexpr float rayBoreHeight = .085f, rayMuzzle = .94f;
constexpr float alienScale(uint8_t variant) { return variant ? 1.08f : 1.f; }
constexpr float rayLength(uint8_t variant) { return variant ? 1.18f : 1.f; }

inline Vec3 alienGunSocket(const Assets& assets, const Actor& actor) {
    const auto& sockets = assets.alienGunSockets;
    const auto a = sockets[std::min<size_t>(actor.frame, sockets.size() - 1)];
    const auto b = sockets[std::min<size_t>(actor.nextFrame, sockets.size() - 1)];
    return a + (b - a) * actor.frameBlend;
}

struct AlienGunFrame {
    Vec3 grip, right, up, axis;

    Vec3 muzzle(uint8_t variant) const { return grip + up * rayBoreHeight + axis * (rayMuzzle * rayLength(variant)); }
};

inline AlienGunFrame alienGunFrame(Vec3 grip, float yaw, float pitch, uint8_t variant) {
    const float s = alienScale(variant);
    const Vec3 axis = forward(yaw, pitch), right{std::cos(yaw), 0, -std::sin(yaw)};
    return {grip, right * s, cross(axis, right) * s, axis * s};
}

// The critical area sits in front of a ground alien's shoulders.
constexpr Vec3 alienWeakPoint{0, 1.58f, .37f};
// North Avenue uplink, shared by the renderer and projectile collision.
constexpr Vec3 uplinkPosition{(450.f - 316.f) * .05f, 0, (212.5f + 227.f) * .05f};
constexpr Vec3 uplinkEmitter{0, 1.62f, 0};
// The mothership, and the small saucer that hangs over West Timberline. The renderer builds both
// here and moves them by MissionSnapshot::fleetShift.
constexpr Vec3 flagshipPosition{(260.f - 316.f) * .05f, 36.f, (212.5f + 520.f) * .05f};
constexpr Vec3 scoutShipPosition{(-480.f - 316.f) * .05f, 30.f, (212.5f - 120.f) * .05f};
struct FleetHull {
    Vec3 center;
    float radius, height;

    constexpr Vec3 emitter() const { return {0, -.486f * height, 0}; }
    constexpr Vec3 boundsExtent() const { return {radius * 1.12f, height * .57f, radius * 1.12f}; }
};

constexpr std::array<FleetHull, 3> fleetHulls{{
    {flagshipPosition, 12.f, 7.6f},
    {{(430.f - 316.f) * .05f, 25.f, (212.5f - 890.f) * .05f}, 6.4f, 4.4f},
    {scoutShipPosition, 4.75f, 3.6f},
}};
// Nora's dish on the east wing roof, aimed at the mothership.
constexpr Vec3 dishHub{(612.f - 316.f) * .05f, 5.1f, (212.5f - 62.f) * .05f};
inline Vec3 dishFocus() { return dishHub + normalized(flagshipPosition - dishHub) * 1.55f; }

struct MissionActor {
    Actor actor{};
    MissionRole role = MissionRole::AlienTrooper;
    uint8_t variant = 0; // Civilian outfit (0-2) or Alien tier (0=standard, 1=enforcer)
    uint8_t tacRole = 0; // 0=patrol, 1=suppressor, 2=flanker, 3=vanguard, 4=support
    bool active = false;
    bool dead = false;
    CivilianOutcome civilian = CivilianOutcome::Pinned;
    bool alerted = false;
    float radar = 0;
    // Animated ray gun & tactical combat visuals
    float gunYaw = 0;
    float gunPitch = 0;        // Vertical aim pitch of held ray gun (radians, positive = down)
    float gunCharge = 0;       // 0..1 plasma charge level (spins coils & swells muzzle orb)
    float gunRecoil = 0;       // 0..1 firing kickback (snaps to 1 on shot, decays smoothly)
    float shield = 0;          // 0..1 remaining phase-shield ratio
    float stagger = 0;         // 0..1 stagger / charge-interrupt flinch
    bool overcharging = false; // True while charging an interruptible heavy shot
    bool linked = false;       // True while reinforced by a nearby Scout Drone's shield link
    float arrival = 0.f;       // Fraction of the reinforcement beam still visible
    bool chargedShot = false;  // A charged flash keeps its color through recoil.
};

struct MissionTracer {
    Vec3 start{}, end{};
    float time = 0;
    uint16_t color = 0xff35; // Default warm ballistic amber
    TracerStyle style = TracerStyle::Ballistic;
};

struct PlasmaCloud {
    Vec3 position{}, direction{};
    float radius = 0; // Zero marks an unused slot.
    bool charged = false;
    float traveled = 0; // The visible wake starts at the launch point.
};

struct MissionSnapshot {
    static constexpr size_t maxActors = 8; // 0: Commander, 1-3: Civilians, 4-7: Active Aliens
    static constexpr size_t maxTracers = 20;
    static constexpr size_t maxPlasma = 24;

    bool active = false;
    bool campaign = false;
    MissionChapter chapter = MissionChapter::BriarHouse;
    float earlierMissionSeconds = 0;
    MissionPhase phase = MissionPhase::Intro;
    float titleCard = 0;     // Seconds into the opening title card (0 once it has gone)
    float introProgress = 0; // 0..1 wake-up sequence
    float concussion = 0;    // 1..0 disorientation intensity
    float letterbox = 0;     // 0..1 cinematic top/bottom framing bars
    float heartbeatPulse = 0;// 0..1 subtle pulse during concussion
    float mistDensity = 0.65f; // 0..1 descending dusk fog intensity over Briar House
    uint8_t waveIntensity = 0; // 0..3 extraction escalation wave
    float radioTimer = 0;
    char radioSpeaker[20]{};
    char radioLine[96]{};
    char objective[64]{};
    char subObjective[64]{};
    char failReason[48]{};
    char epilogue[72]{};      // One line on the results screen about how it ended
    char tacticalCallout[32]{};
    float tacticalCalloutTimer = 0;
    uint16_t tacticalCalloutColor = 0;
    bool warningLock = false; // An Enforcer is charging a plasma shot.
    float plasmaShock = 0;
    bool shieldHit = false;   // Last player hit struck an alien energy shield
    Vec3 waypoint{};
    bool hasWaypoint = false;
    bool waypointIsRoute = false;
    Vec3 holdCenter{};
    float holdRadius = 0;
    int civiliansSaved = 0, civiliansTotal = 3, civiliansLost = 0, civiliansBoarded = 0;
    int aliensKilled = 0, criticalKills = 0, aliensRemaining = 0;
    int chargeInterrupts = 0, shieldsBroken = 0, ambushKills = 0;
    float missionSeconds = 0;
    float evacSeconds = 0;      // Extraction hold countdown; 0 once the birds are on station
    float evacTotal = 0;        // Length of the hold, for the HUD progress bar
    int retries = 0;            // Checkpoint reloads used on this run
    MissionCheckpoint checkpointStage = MissionCheckpoint::None;
    bool friendlyFireWarn = false;
    float friendlyFireTimer = 0;
    // Exfil cutscene: player input is locked and the camera follows the evac bird.
    bool cinematic = false;
    float fade = 0;             // 0..1 eyelid close at the very end
    bool evacVisible = false;
    Vec3 evacPos{};
    float evacYaw = 0, evacPitch = 0;
    bool uplinkVisible = false;
    float uplinkHealth = 0;
    OperationView operation{};
    // Offsets of the mothership, the cruiser and the Timberline saucer from where they were built.
    std::array<Vec3, 3> fleetShift{};
    bool flagshipBeam = true;
    float dishPulse = 0;        // 0..1 flash on Nora's dish
    bool dishLink = false;      // The Array: the dish is talking to the mothership
    bool signalReady = false;   // Signal does something right now
    char signalHint[32]{};      // What it does, shown beside the button
    std::array<MissionActor, maxActors> actors{};
    std::array<MissionTracer, maxTracers> tracers{};
    std::array<PlasmaCloud, maxPlasma> plasma{};

    bool nextMissionReady() const {
        return campaign && phase == MissionPhase::Victory && chapter != MissionChapter::Array;
    }
};

} // namespace fod

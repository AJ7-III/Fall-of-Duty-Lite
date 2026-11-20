#pragma once

#include "game.hpp"

#include <array>
#include <cstdint>

namespace fod {

enum class MissionRole : uint8_t { Commander, AlienTrooper, AlienDrone, Civilian };
enum class MissionPhase : uint8_t { Intro, Overwatch, Breach, Regroup, Rescue, Incoming, Extraction, Victory, Failed };
enum class TracerStyle : uint8_t { Ballistic, RayBolt, HeavyRay, Scanner, DropBeam, ShieldLink };

struct MissionActor {
    Actor actor{};
    MissionRole role = MissionRole::AlienTrooper;
    uint8_t variant = 0; // Civilian outfit (0-2) or Alien tier (0=standard, 1=enforcer)
    uint8_t tacRole = 0; // 0=patrol, 1=suppressor, 2=flanker, 3=vanguard, 4=support
    bool active = false;
    bool dead = false;
    bool rescued = false;
    bool alerted = false;
    float radar = 0;
    // Animated ray gun & tactical combat visuals
    float gunPitch = 0;        // Vertical aim pitch of held ray gun (radians, positive = down)
    float gunCharge = 0;       // 0..1 plasma charge level (spins coils & swells muzzle orb)
    float gunRecoil = 0;       // 0..1 firing kickback (snaps to 1 on shot, decays smoothly)
    float shield = 0;          // 0..1 remaining phase-shield ratio
    float stagger = 0;         // 0..1 stagger / charge-interrupt flinch
    bool overcharging = false; // True while charging an interruptible heavy beam
    bool linked = false;       // True while reinforced by a nearby Scout Drone's shield link
};

struct MissionTracer {
    Vec3 start{}, end{};
    float time = 0;
    uint16_t color = 0xff35; // Default warm ballistic amber
    TracerStyle style = TracerStyle::Ballistic;
};

struct MissionSnapshot {
    static constexpr size_t maxActors = 8; // 0: Commander, 1-3: Civilians, 4-7: Active Aliens
    static constexpr size_t maxTracers = 20;

    bool active = false;
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
    bool warningLock = false; // Enforcer actively charging a heavy beam at the player
    bool shieldHit = false;   // Last player hit struck an alien energy shield
    Vec3 waypoint{};
    bool hasWaypoint = false;
    int civiliansSaved = 0, civiliansTotal = 3, civiliansLost = 0;
    int aliensKilled = 0, headshots = 0, aliensRemaining = 0;
    int chargeInterrupts = 0, shieldsBroken = 0, ambushKills = 0;
    float missionSeconds = 0;
    float evacSeconds = 0;      // Extraction hold countdown; 0 once the birds are on station
    float evacTotal = 0;        // Length of the hold, for the HUD progress bar
    int retries = 0;            // Checkpoint reloads used on this run
    uint8_t checkpointStage = 0;// 0 none, 1 rescue, 2 extraction
    bool checkpointReached = false;
    bool friendlyFireWarn = false;
    float friendlyFireTimer = 0;
    // Exfil cutscene: player input is locked and the camera follows the evac bird.
    bool cinematic = false;
    float fade = 0;             // 0..1 eyelid close at the very end
    bool evacVisible = false;
    Vec3 evacPos{};
    float evacYaw = 0, evacPitch = 0;
    std::array<MissionActor, maxActors> actors{};
    std::array<MissionTracer, maxTracers> tracers{};
};

} // namespace fod

#pragma once

#include "game.hpp"

#include <array>
#include <cstdint>

namespace fod {

enum class MissionRole : uint8_t { Commander, AlienTrooper, AlienDrone, Civilian };
enum class MissionPhase : uint8_t { Intro, Overwatch, Breach, Regroup, Rescue, Incoming, Extraction, Victory, Failed };

struct MissionActor {
    Actor actor{};
    MissionRole role = MissionRole::AlienTrooper;
    uint8_t variant = 0; // Civilian outfit (0-2) or Alien tier (0=standard, 1=enforcer)
    bool active = false;
    bool dead = false;
    bool rescued = false;
    bool alerted = false;
    float radar = 0;
};

struct MissionTracer {
    Vec3 start{}, end{};
    float time = 0;
    uint16_t color = 0xff35; // Default warm ballistic amber
};

struct MissionSnapshot {
    static constexpr size_t maxActors = 8; // 0: Commander, 1-3: Civilians, 4-7: Active Aliens
    static constexpr size_t maxTracers = 12;

    bool active = false;
    MissionPhase phase = MissionPhase::Intro;
    float titleCard = 0;     // Seconds into the opening title card (0 once it has gone)
    float introProgress = 0; // 0..1 wake-up sequence
    float concussion = 0;    // 1..0 disorientation intensity
    float letterbox = 0;     // 0..1 cinematic top/bottom framing bars
    float heartbeatPulse = 0;// 0..1 subtle pulse during concussion
    float radioTimer = 0;
    char radioSpeaker[20]{};
    char radioLine[96]{};
    char objective[64]{};
    char subObjective[64]{};
    char failReason[48]{};
    char epilogue[72]{};      // One line on the results screen about how it ended
    Vec3 waypoint{};
    bool hasWaypoint = false;
    int civiliansSaved = 0, civiliansTotal = 3, civiliansLost = 0;
    int aliensKilled = 0, headshots = 0, aliensRemaining = 0;
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

#pragma once

#include "mission.hpp"

namespace fod::mission_detail {

constexpr Vec3 mapPoint(float x, float y, float height = 0.f) {
    return {(x - 316.f) * .05f, height, (212.5f - y) * .05f};
}

// Rescued survivors shelter behind the stone walls bordering the flare-lit North Town Clearing LZ.
constexpr float evacSpots[3][2]{{175, -180}, {210, -172}, {245, -180}};
constexpr float townLz[2]{210, -220};
constexpr float commanderLz[2]{152, -195};
constexpr float uplinkMaxHealth = 135.f;
constexpr float civilianHealth = 85.f;
constexpr float assemblyRadius = 8.f, holdRadius = 15.f, lzThreatRadius = 24.f;
// The Marsh family scattered into three town districts when the fleet struck Briar House.
// Their first words depend on what has happened to Nora by the time the player reaches them.
struct Survivor {
    float x, y, yaw;
    const char* objective;
    const char* name;       // Subtitle speaker
    const char* callout;    // Delta-1 as you reach the district
    const char* underFire;  // Delta-1 when the aliens turn on them
    const char* noraPinned; // Their words once safe, while Nora is still out there (Nora's own line for her)
    const char* noraSafe;   // ...once Nora is safe
    const char* noraLost;   // ...once Nora is gone
    const char* lost;       // Delta-1 when they die
};
constexpr Survivor family[3]{
    {920, 445, -pi / 2, "BEN / EAST RIDGE", "BEN MARSH",
     "HER SON'S PINNED BEHIND THAT PICKUP.",
     "THEY'RE OPENING UP ON BEN! PULL THEIR FIRE!",
     "THANK GOD - MY MOM RAN UP NORTH AVENUE TO HER DISH. IS SHE ALIVE?",
     "YOU ALREADY GOT MY MOM OUT? THANK GOD... OKAY, I'M RUNNING.",
     "WAIT - WHERE'S MY MOM? TELL ME YOU FOUND HER.",
     "DAMN IT - WE LOST BEN."},
    {535, -215, pi, "NORA / NORTH VALLEY", "NORA MARSH",
     "EYES ON DR. MARSH. A HEAVY HAS HER PINNED.",
     "THEY'RE CLOSING ON DR. MARSH! KEEP 'EM OFF HER!",
     "THEY DROPPED STRAIGHT ONTO MY ROOF ARRAY. THAT WASN'T RANDOM - THEY HOMED ON IT.",
     "THEY DROPPED STRAIGHT ONTO MY ROOF ARRAY. THAT WASN'T RANDOM - THEY HOMED ON IT.",
     "THEY DROPPED STRAIGHT ONTO MY ROOF ARRAY. THAT WASN'T RANDOM - THEY HOMED ON IT.",
     "DR. MARSH IS DOWN... WE LOST HER."},
    {-298, 252, pi / 2, "WALT / WEST TIMBERLINE", "WALT MARSH",
     "HER DAD'S BY THE CABIN. KEEP LOW OR CLEAR 'EM.",
     "THEY SPOTTED WALT BY THE CABIN! LIGHT 'EM UP!",
     "MUCH OBLIGED. YOU SEEN MY DAUGHTER UP BY BRIAR HOUSE?",
     "YOU ALREADY GOT NORA OUT? GOOD. I'LL MEET HER AT THE WALL.",
     "WHERE'S NORA? ...TELL ME SHE MADE IT.",
     "DAMN IT - WALT DIDN'T MAKE IT."},
};
constexpr int ben = 0;
constexpr int nora = 1;
constexpr int walt = 2;
constexpr Vec3 waltPickupApproach = mapPoint(-250, 225);
constexpr Vec3 waltCoverRoute[]{mapPoint(-298, 221), mapPoint(-245, 215), mapPoint(-240, 124)};
constexpr Vec3 districtApproach[]{mapPoint(908, 489), mapPoint(500, -225), mapPoint(-282, 280)};

struct Patrol {
    MissionRole role;
    Vec3 position;
    float yaw = 0.f;
    uint8_t tier = 0;
};
constexpr Patrol roadPatrol[]{
    {MissionRole::AlienTrooper, mapPoint(460, 668), -.2f},
    {MissionRole::AlienTrooper, mapPoint(272, 672), .35f},
    {MissionRole::AlienDrone, mapPoint(378, 638, 3.4f)},
};
constexpr Patrol districtPatrol[][2]{
    {{MissionRole::AlienTrooper, mapPoint(975, 305), pi},
     {MissionRole::AlienTrooper, mapPoint(845, 555), .4f}},
    {{MissionRole::AlienTrooper, mapPoint(655, -225), -pi / 2, 1},
     {MissionRole::AlienDrone, mapPoint(565, -248, 2.4f), -pi / 2}},
    {{MissionRole::AlienDrone, mapPoint(-338, 260, 3.4f), pi / 2},
     {MissionRole::AlienTrooper, mapPoint(-318, 395)}},
};
constexpr Vec3 breachFlank[]{mapPoint(625, 645), mapPoint(145, 645)};
constexpr Vec3 uplinkGuard[]{mapPoint(435, -246, 3.f), mapPoint(514, -248)};
constexpr Patrol lzVanguard[]{
    {MissionRole::AlienTrooper, mapPoint(40, -225), 0.f, 1},
    {MissionRole::AlienTrooper, mapPoint(395, -245)},
    {MissionRole::AlienDrone, mapPoint(210, -268, 2.5f)},
    {MissionRole::AlienTrooper, mapPoint(-135, -192)},
};
constexpr Vec3 lzGroundDrops[]{
    mapPoint(-150, -195), mapPoint(38, -242), mapPoint(388, -258),
    mapPoint(565, -240), mapPoint(215, -112), mapPoint(-255, -75),
};
constexpr Vec3 lzDroneDrops[]{
    mapPoint(-140, -180, 2.3f), mapPoint(420, -235, 2.3f),
    mapPoint(225, -265, 2.4f), mapPoint(560, -210, 2.3f),
};

// North Avenue: Nora's dish pulses on a timer and pulls the shield drone's attention for a moment.
constexpr double dishPulseInterval = 7.0, dishDistraction = 2.2;
// How far away Ben can see the player wave him over, and how far a watcher can see a survivor break cover.
constexpr float benSignalRange = 26.f, watcherRange = 30.f;
constexpr double signalCooldown = 2.5;

constexpr double exfilArrive = 4.4, exfilClimb = 3.2, liftDuration = 5.8;
constexpr Vec3 exfilHover = mapPoint(210, -235, 2.4f), exfilApproach = mapPoint(60, -560, 19.f);
constexpr Vec3 exfilAway = mapPoint(420, -680, 34.f), exfilEye = mapPoint(268, -162, 1.75f);

} // namespace fod::mission_detail

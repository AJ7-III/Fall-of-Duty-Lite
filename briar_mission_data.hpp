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
// The Marsh family scattered into three surrounding Colorado mountain town districts when the
// alien fleet struck Briar House. Delta Squad breaks out of Briar House to sweep the valley town.
struct Survivor {
    float x, y, yaw;
    const char* objective;
    const char* name;      // Subtitle speaker
    const char* callout;   // Delta-1 as you reach the district
    const char* underFire; // Delta-1 when the aliens turn on them
    const char* rescued;   // Their own words once the block is clear
    const char* worried;   // Said instead while Nora is still pinned
    const char* lost;
};
constexpr Survivor family[3]{
    {920, 445, -pi / 2, "BEN / EAST RIDGE", "BEN MARSH", "EAST RIDGE. TWO RIFLES COVER THE PICKUP. BEN IS BEHIND IT.",
     "THEY'RE FIRING ON BEN - PUSH UP EAST RIDGE!", "WE RAN WHEN THE SKY LIT UP... THANK YOU. THANK YOU.",
     "WE RAN WHEN THE SKY LIT UP... IS MY MOM OKAY? SHE HEADED UP NORTH AVENUE!", "WE LOST BEN! LOCK IT DOWN, DELTA!"},
    {535, -215, pi, "NORA / NORTH VALLEY", "DR. NORA MARSH", "NORTH VALLEY. DROP THE SHIELD DRONE, THEN HIT THE ENFORCER.",
     "THEY'RE FIRING ON DR. MARSH - MOVE!", "MY ARRAY LOGGED THEIR ROUTE. I HAVE THE DATA. GET US TO DUSTOFF!",
     "MY ARRAY LOGGED THEIR ROUTE. I HAVE THE DATA. GET US TO DUSTOFF!", "DR. MARSH IS DOWN! WE LOST THE PACKAGE!"},
    {-298, 252, pi / 2, "WALT / WEST TIMBERLINE", "WALT MARSH", "WEST TIMBERLINE. WATCH ABOVE THE TREES. WALT IS BY THE CABIN.",
     "THEY'RE HITTING WEST TIMBERLINE - GET EYES ON WALT!", "TOOK YOU LONG ENOUGH, SON. HELL OF A SUNSET FOR AN INVASION.",
     "TOOK YOU LONG ENOUGH, SON. WHERE'S MY DAUGHTER?", "WE LOST THE OLD MAN! LOCK IT DOWN!"},
};
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

constexpr double exfilArrive = 4.4, exfilClimb = 3.2;
constexpr Vec3 exfilHover = mapPoint(210, -235, 2.4f), exfilApproach = mapPoint(60, -560, 19.f);
constexpr Vec3 exfilAway = mapPoint(420, -680, 34.f), exfilEye = mapPoint(268, -162, 1.75f);

} // namespace fod::mission_detail

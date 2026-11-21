#pragma once

#include "assets.hpp"

namespace fod::mission_detail::timberline {

constexpr ArenaExtent extent{29, 102};
constexpr Vec3 southPad{-14, .12f, -36}, arrivalStart{-16, 25, -64};
constexpr Vec3 dismount{-9, 0, -35};
constexpr Vec3 northPad{20.5f, 4.32f, 94.5f}, pickupStart{28, 30, 146}, pickupAway{33, 29, 134};

constexpr Vec3 yardKit{-16, 0, -12}, yardNora{-17, 0, -14}, yardDelta{-13, 0, -11}, yardSpawn{-14, 0, -16};
constexpr Vec3 quarryKit{6, 0, 13}, quarryNora{5, 0, 11}, quarryDelta{8, 0, 13}, quarrySpawn{5, 0, 8};
constexpr Vec3 northRoad{11, 0, 24}, boardingPoint{16.9f, 4.2f, 93.5f};
constexpr Vec3 pickupCover{16.7f, 4.2f, 91.9f};

// Broad progress thresholds leave room to use either side of the boulders or cut across the valley.
struct Fallback {
    Vec3 cover;
    float advanceNorthOf;
};
constexpr Fallback fallbacks[]{
    {{18, 0, 15}, 13.5f},
    {{25.7f, 0, 25.5f}, 22.f},
    {{25.7f, 0, 39}, 36.f},
    {{23.5f, 0, 52}, 49.f},
    {{18.7f, 0, 67}, 63.f},
    {{20, 0, 81}, 77.f},
    {{20, 0, 90}, 85.f},
};

constexpr Vec3 scoutPost{-29, 30, 7}, flagshipPost{28, 43, -7}, cruiserPost{-28, 39, 11};

} // namespace fod::mission_detail::timberline

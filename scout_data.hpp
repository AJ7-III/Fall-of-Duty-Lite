#pragma once

#include "types.hpp"

namespace fod::mission_detail::scout {

inline constexpr Vec3 entry{0, 0, -25};
inline constexpr Vec3 cargoOrigin{0, 0, -21};
inline constexpr Vec3 cargoChute{-8.6f, 0, -19.9f};
inline constexpr Vec3 cargoExit{0, 0, -9};
inline constexpr Vec3 portChute{-12, 0, 1};
inline constexpr Vec3 starboardChute{12, 0, 1};
inline constexpr float chuteCanopy = 3.93f;
inline constexpr float chuteRadius = 1.48f;
#ifndef ESP_PLATFORM
inline constexpr Vec3 scoutDrive{1.52f, .38f, -4.68f};
#endif
inline constexpr Vec3 transponder{0, 0, 11.5f};
inline constexpr Vec3 bridgeEntry{0, 0, 16};
inline constexpr Vec3 helm{0, 0, 26};

// These points are outside the glass. They remain in the ship's coordinate space so the
// visible landing order and the lock use exactly the same positions.
inline constexpr Vec3 beamStart{25, -7.5f, 76};
inline constexpr Vec3 clearing{-5, -7.5f, 65};
inline constexpr Vec3 saddle{-30, -7.5f, 80};
inline constexpr Vec3 roadApproach{15, -7.5f, 56};
inline constexpr Vec3 releaseWinch(bool port) { return {port ? -10.2f : 10.2f, 0, 9.1f}; }
inline constexpr float clearingRadius = 5.7f;

} // namespace fod::mission_detail::scout

#pragma once

#include "types.hpp"

#include <algorithm>
#include <array>

namespace fod {

// Animation follows the distance traveled by the simulation. Drawing either co-op view only samples this state.
struct NpcMotion {
    float phase = 0, speed = 0, crouch = 0, flinch = 0;
    float aim = 0, recoil = 0, lean = 0, turn = 0, travelYaw = 0;
    float lookYaw = 0, lookPitch = 0;
    std::array<float, 2> footRise{};
    bool ready = false, grounded = true;
};

inline float motionEase(float x) {
    x = std::clamp(x, 0.f, 1.f);
    return x * x * (3.f - 2.f * x);
}

inline float gaitRun(float speed) { return motionEase((speed - 2.0f) / 2.3f); }
inline float gaitWeight(float speed) { return motionEase(speed / 1.24f); }
inline float gaitDistance(float run) { return 1.36f + 1.27f * run; }

struct FootCycle {
    float z, lift, roll;
};

inline FootCycle gaitFoot(float phase, float run) {
    phase -= std::floor(phase);
    const float support = .64f - .16f * run;
    const float distance = gaitDistance(run);
    const float reach = .5f * distance * support;
    if (phase < support) {
        const float contact = phase / support;
        const float heel = motionEase(1.f - contact * 5.3f);
        const float toe = motionEase((contact - .66f) * 2.95f);
        return {reach - distance * phase, 0.f, .22f * heel - (.34f + .15f * run) * toe};
    }
    const float swing = (phase - support) / (1.f - support);
    const float t = motionEase(swing);
    // Match the backward speed of the planted foot at each end of the swing.
    const float carry = distance * (1.f - support) * swing * (1.f - swing) * (1.f - 2.f * swing);
    const float arc = std::sin(pi * swing);
    return {-reach + 2.f * reach * t - carry, arc * arc * (.116f + .116f * run),
            (-.34f - .15f * run) * (1.f - t) + .22f * t};
}

} // namespace fod

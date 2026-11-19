#pragma once

#include "game.hpp"

namespace fod::game_detail {

class Random {
public:
    uint32_t state = 1;
    uint32_t tick = 0;
    Game::RandomHook hook = nullptr;
    void* data = nullptr;

    double draw(const char* site) {
        double value;
        if (hook && hook(data, tick, site, value) && value >= 0 && value < 1) return value;
        uint32_t n = state;
        n ^= n << 13;
        n ^= n >> 17;
        n ^= n << 5;
        state = n;
        return double(n) / 4294967296.0;
    }

    double between(const char* site, double min, double max) {
        return min + (max - min) * draw(site);
    }
};

inline double angleDelta(double from, double to) {
    constexpr double half = 3.14159265358979323846;
    double diff = std::fmod(to - from, 2 * half);
    if (diff > half) diff -= 2 * half;
    if (diff < -half) diff += 2 * half;
    return diff;
}

} // namespace fod::game_detail

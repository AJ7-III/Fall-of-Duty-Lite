#pragma once

#include <algorithm>
#include <array>
#include <cstdint>

namespace fod::atmosphere {

constexpr int maskSize = 32;
struct Mask {
    struct Span { uint8_t first = maskSize, end = 0; };
    std::array<uint8_t, maskSize * maskSize> pixels{};
    std::array<Span, maskSize> spans{};
};

constexpr float noise(int x, int y, unsigned seed) {
    const int u = x & 7, v = y & 7;
    const auto sample = [seed](int a, int b) {
        uint32_t h = uint32_t(a) * 0x8da6b343u + uint32_t(b) * 0xd8163841u + seed * 0x9e3779b9u;
        h = (h ^ (h >> 13)) * 0x85ebca6bu;
        return float((h ^ (h >> 16)) & 255u) / 255.f;
    };
    const auto lerp = [](float a, float b, int k) { return a + (b - a) * (float(k) / 8.f); };
    const int i = x >> 3, j = y >> 3;
    return lerp(lerp(sample(i, j), sample(i + 1, j), u),
                lerp(sample(i, j + 1), sample(i + 1, j + 1), u), v);
}

constexpr Mask makeMask(unsigned seed, bool flame) {
    Mask mask{};
    for (int y = 1; y < maskSize - 1; ++y) {
        for (int x = 1; x < maskSize - 1; ++x) {
            const float u = (2.f * x + 1.f) / maskSize - 1.f;
            const float v = (2.f * y + 1.f) / maskSize - 1.f;
            const float n = noise(x, y, seed), detail = noise(2 * x, 2 * y, seed + 19);
            float alpha = 0.f;
            if (flame) {
                const float rise = .5f - .5f * v;
                const float dx = (u + (n - .5f) * .9f * rise) / (.84f - .62f * rise);
                const float edge = std::clamp(1.f - dx * dx - v * v * .81f, 0.f, 1.f);
                alpha = edge * std::clamp((1.f - rise) * 3.f, 0.f, 1.f) * (.49f + .35f * n + .16f * detail);
            } else {
                const float edge = std::clamp((1.f - u * u - v * v) * (1.1f + .5f * n), 0.f, 1.f);
                alpha = edge * edge * (.2f + .56f * n + .24f * detail);
            }
            const uint8_t value = uint8_t(alpha * 255.f + .5f);
            mask.pixels[y * maskSize + x] = value;
            // Below five, even full sprite opacity cannot reach the rasterizer's cutoff.
            if (value >= 5) {
                auto& span = mask.spans[y];
                span.first = std::min(span.first, uint8_t(x));
                span.end = uint8_t(x + 1);
            }
        }
    }
    return mask;
}

inline constexpr auto clouds = std::array{makeMask(2, false), makeMask(5, false), makeMask(7, false)};
inline constexpr auto flames = std::array{makeMask(3, true), makeMask(6, true), makeMask(11, true)};

} // namespace fod::atmosphere

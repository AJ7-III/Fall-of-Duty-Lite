#pragma once

#include "assets.hpp"

#include <algorithm>
#include <limits>

namespace fod::game_detail {

constexpr float infinity = std::numeric_limits<float>::infinity();
constexpr float gravity = -15;

inline float planarSquared(Vec3 v) { return v.x * v.x + v.z * v.z; }
inline float planar(Vec3 v) { return std::sqrt(planarSquared(v)); }
inline Vec3 atHeight(Vec3 v, float height) { return {v.x, v.y + height, v.z}; }
inline float sign(float v) { return v < 0 ? -1.f : 1.f; }
inline float smooth(double t) {
    t = std::clamp(t, 0.0, 1.0);
    return float(t * t * (3 - 2 * t));
}

inline Vec3 local(const Obstacle& b, Vec3 p) {
    const float x = p.x - b.cx, z = p.z - b.cz;
    return {b.cos * x - b.sin * z, p.y, b.sin * x + b.cos * z};
}

inline Vec3 localDirection(const Obstacle& b, Vec3 d) {
    return {b.cos * d.x - b.sin * d.z, d.y, b.sin * d.x + b.cos * d.z};
}

inline bool slab(float origin, float direction, float min, float max, float& first, float& last) {
    if (std::abs(direction) < 1e-9f) return origin >= min && origin <= max;
    float a = (min - origin) / direction, b = (max - origin) / direction;
    if (a > b) std::swap(a, b);
    first = std::max(first, a);
    last = std::min(last, b);
    return first <= last;
}

inline bool intersects(Vec3 p, Vec3 d, Bounds box, float& first, float last) {
    return slab(p.x, d.x, box.min.x, box.max.x, first, last) &&
           slab(p.z, d.z, box.min.z, box.max.z, first, last) &&
           slab(p.y, d.y, box.min.y, box.max.y, first, last);
}

struct ShotHit {
    float distance = 1000;
    uint32_t surface = 0;
    int glass = -1;
    bool hit = false;
};

class World {
public:
    explicit World(const Assets& assets) : assets_(assets) {
        boxBounds_.reserve(assets.obstacles.size());
        for (const auto& b : assets.obstacles) {
            const float ex = std::abs(b.cos) * b.hw + std::abs(b.sin) * b.hd;
            const float ez = std::abs(b.sin) * b.hw + std::abs(b.cos) * b.hd;
            boxBounds_.push_back({{b.cx - ex, b.minY, b.cz - ez}, {b.cx + ex, b.maxY, b.cz + ez}});
        }
    }

    bool move(Vec3& pos, Vec3& velocity, float dt, float height, float radius) const {
        const int steps = std::max(1, int(std::ceil(planar(velocity) * dt / std::max(radius * .5f, .01f))));
        bool grounded = false;
        for (int i = 0; i < steps; ++i) grounded = moveStep(pos, velocity, dt / steps, height, radius);
        return grounded;
    }

    float clampX(float x) const { return std::clamp(x, -assets_.extent.x, assets_.extent.x); }
    float clampZ(float z) const { return std::clamp(z, -assets_.extent.z, assets_.extent.z); }

    bool canStand(Vec3 pos, float eye) const {
        for (const auto& b : assets_.obstacles) {
            if (b.maxY <= pos.y + .05f || b.minY >= pos.y + eye + .15f) continue;
            const Vec3 p = local(b, pos);
            if (std::abs(p.x) < b.hw + .38f && std::abs(p.z) < b.hd + .38f) return false;
        }
        return true;
    }

    bool walkBlocked(Vec3 from, Vec3 to) const {
        for (const auto& b : assets_.obstacles) {
            if (b.minY > 1.75f || b.maxY < .15f) continue;
            const Vec3 p = local(b, from), d = localDirection(b, to - from);
            const float w = b.hw + .42f, h = b.hd + .42f;
            if ((std::abs(d.x) < 1e-8f && std::abs(p.x) >= w) ||
                (std::abs(d.z) < 1e-8f && std::abs(p.z) >= h)) continue;
            float first = 0, last = 1;
            if (slab(p.x, d.x, -w, w, first, last) && slab(p.z, d.z, -h, h, first, last)) return true;
        }
        return false;
    }

    bool blocked(Vec3 from, Vec3 to) const {
        return boxRay(from, to - from, 1, true) != infinity;
    }

    float ray(Vec3 p, Vec3 direction, float maxDistance) const {
        float best = boxRay(p, direction, maxDistance, false);
        if (direction.y < -1e-6f) {
            const float floor = -p.y / direction.y;
            if (floor <= maxDistance) best = std::min(best, floor);
        }
        return best;
    }

    ShotHit shot(Vec3 p, Vec3 d, uint64_t broken) const {
        ShotHit result;
        const auto test = [&](uint32_t first, uint32_t count) {
            for (uint32_t i = first; i < first + count; ++i) {
                const auto& tri = assets_.shotSurfaces[i];
                if (tri.glass >= 0 && tri.glass < 64 && (broken & (uint64_t(1) << tri.glass))) continue;
                const float hit = triangle(p, d, assets_.shotCorners(i));
                if (hit < result.distance) result = {hit, tri.surface, tri.glass, true};
            }
        };
        // Shot chunks carry the browser mesh geometry independently of the
        // render pack, so render LOD never changes the surface that a ray hits.
        if (assets_.shotChunks.empty()) {
            test(0, uint32_t(assets_.shotSurfaces.size()));
        } else {
            for (const auto& chunk : assets_.shotChunks) {
                float near = 0;
                constexpr Vec3 margin{.00001f, .00001f, .00001f};
                const Bounds bounds{chunk.bounds.min - margin, chunk.bounds.max + margin};
                if (intersects(p, d, bounds, near, result.distance)) test(chunk.first, chunk.count);
            }
        }
        return result;
    }

private:
    const Assets& assets_;
    std::vector<Bounds> boxBounds_;

    static float triangle(Vec3 p, Vec3 d, const std::array<Vec3, 3>& corners) {
        const auto& [a, b, c] = corners;
        const Vec3 e = b - a, f = c - a;
        const Vec3 perpendicular = cross(d, f);
        const float determinant = dot(e, perpendicular);
        if (std::abs(determinant) < 1e-8f) return infinity;
        const float inverse = 1 / determinant;
        const Vec3 offset = p - a;
        const float u = dot(offset, perpendicular) * inverse;
        if (u < 0 || u > 1) return infinity;
        const Vec3 q = cross(offset, e);
        const float v = dot(d, q) * inverse;
        if (v < 0 || u + v > 1) return infinity;
        const float t = dot(f, q) * inverse;
        return t < 0 ? infinity : t;
    }

    float boxRay(Vec3 p, Vec3 d, float maxDistance, bool any) const {
        float best = maxDistance;
        bool hit = false;
        for (size_t i = 0; i < boxBounds_.size(); ++i) {
            const auto& b = assets_.obstacles[i];
            if (!b.blocksSight) continue;
            float first = 0;
            if (!intersects(p, d, boxBounds_[i], first, best)) continue;
            first = 0;
            if (intersects(local(b, p), localDirection(b, d),
                           {{-b.hw, b.minY, -b.hd}, {b.hw, b.maxY, b.hd}}, first, best)) {
                if (any) return first;
                best = first;
                hit = true;
            }
        }
        return hit ? best : infinity;
    }

    bool moveStep(Vec3& pos, Vec3& v, float dt, float height, float radius) const {
        Vec3 next{clampX(pos.x + v.x * dt), pos.y, clampZ(pos.z + v.z * dt)};
        for (int pass = 0; pass < 2; ++pass) {
            bool pushed = false;
            for (const auto& b : assets_.obstacles) {
                if (pos.y >= b.maxY || pos.y + height <= b.minY) continue;
                Vec3 p = local(b, next);
                const float x = b.hw + radius - std::abs(p.x), z = b.hd + radius - std::abs(p.z);
                if (x <= 0 || z <= 0) continue;
                Vec3 n;
                if (x < z) {
                    const float s = sign(p.x);
                    p.x = s * (b.hw + radius);
                    n = {b.cos * s, 0, -b.sin * s};
                } else {
                    const float s = sign(p.z);
                    p.z = s * (b.hd + radius);
                    n = {b.sin * s, 0, b.cos * s};
                }
                next.x = b.cx + b.cos * p.x + b.sin * p.z;
                next.z = b.cz - b.sin * p.x + b.cos * p.z;
                const float into = dot(v, n);
                if (into < 0) v -= n * into;
                pushed = true;
            }
            if (!pushed) break;
            next.x = clampX(next.x);
            next.z = clampZ(next.z);
        }
        const float vy = v.y, rise = std::clamp(-vy / gravity, 0.f, dt);
        const float apex = pos.y + std::max(0.f, (vy + .5f * gravity * rise) * rise);
        next.y += (vy + .5f * gravity * dt) * dt;
        v.y += gravity * dt;
        float floor = 0, roof = infinity;
        for (const auto& b : assets_.obstacles) {
            const bool landing = v.y <= 0 && apex >= b.maxY - .05f && next.y <= b.maxY;
            const bool bonk = vy > 0 && pos.y + height <= b.minY + .05f && apex + height > b.minY;
            if (!landing && !bonk) continue;
            const Vec3 p = local(b, next);
            if (std::abs(p.x) >= b.hw + radius || std::abs(p.z) >= b.hd + radius) continue;
            if (landing) floor = std::max(floor, b.maxY);
            if (bonk) roof = std::min(roof, b.minY - height);
        }
        if (roof < infinity) {
            next.y = std::max(0.f, roof);
            v.y = 0;
        } else if (next.y <= floor) {
            next.y = floor;
            v.y = 0;
            pos = next;
            return true;
        }
        pos = next;
        return next.y == 0;
    }
};

inline float cylinder(Vec3 p, Vec3 d, Vec3 base, float radius, float height) {
    const Vec3 offset = p - base;
    const float a = planarSquared(d);
    if (a < 1e-8f) return infinity;
    const float b = 2 * (offset.x * d.x + offset.z * d.z);
    const float c = planarSquared(offset) - radius * radius;
    const float disc = b * b - 4 * a * c;
    if (disc < 0) return infinity;
    const float root = std::sqrt(disc);
    float t = (-b - root) / (2 * a);
    if (t < 0) {
        t = (-b + root) / (2 * a);
        if (t < 0) return infinity;
    }
    const float y = offset.y + t * d.y;
    return y >= 0 && y <= height ? t : infinity;
}

} // namespace fod::game_detail
